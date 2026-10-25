/*
 * ESP-NOW control link (sender side, ESP32-S3 / AI_BOT).
 *
 * The UART console forwards `music ...` commands to this module.  Packets are
 * broadcast so the WROOM MAC does not have to be configured; the WROOM replies
 * unicast to our STA MAC and the lines are printed on the same UART console.
 *
 * WiFi channel: ESP-NOW only works when both boards share a channel.  The S3 is
 * associated with the router, so its channel is whatever the AP chose.  While
 * the link is not confirmed we send HELLO every second; the WROOM hops over
 * channels 1..13 until it hears us and then locks to our channel.
 */
#include "bt_link.h"

#include <cstdio>
#include <cstring>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "uart_console.h"

#define TAG "BtLink"

namespace {

constexpr int kQueueLen = 8;
constexpr int kBurstCount = 3;
constexpr int kBurstGapMs = 120;
constexpr int kHelloBurst = 2;

/* If the WROOM answered recently, do not flood it with HELLO.  The confirmed
 * interval is shorter than the WROOM's 10 s lock window, so it never hops away
 * while we are alive. */
constexpr int64_t kHelloConfirmedUs = 5 * 1000 * 1000;
constexpr int64_t kHelloSearchUs = 1 * 1000 * 1000;
constexpr int64_t kRxFreshUs = 25 * 1000 * 1000;
constexpr int64_t kRemoteBusyTimeoutUs = 60 * 1000 * 1000;
constexpr int64_t kReplyTimeoutUs = 8 * 1000 * 1000;

/* Commands are repeated with the SAME seq until the WROOM ACKs them, so a
 * channel mismatch only delays a command instead of losing it.  The WROOM
 * dedups on (MAC, seq) for 8 s, so retries must stop before that. */
constexpr int kCommandRetryGapMs = 500;
constexpr int64_t kCommandRetryWindowUs = 6500 * 1000;
constexpr int64_t kPinRetryUs = 2 * 1000 * 1000;

/* Channel used when the S3 is not associated with an access point.  Without
 * this the WiFi stack keeps scanning (looking for an AP) and drags the radio
 * channel along, so ESP-NOW packets leave on a random channel. */
constexpr uint8_t kFixedChannel = 1;

const uint8_t kBroadcast[6] = BT_LINK_BROADCAST_ADDR;

QueueHandle_t g_tx_queue = nullptr;
QueueHandle_t g_rx_queue = nullptr;
QueueHandle_t g_cm02_event_queue = nullptr;
TaskHandle_t g_task = nullptr;

volatile bool g_ready = false;
volatile bool g_cm02_workflow_active = false;
volatile uint8_t g_channel = kFixedChannel;

/* WiFi state.  `esp_wifi_get_channel()` only reports a trustworthy value when
 * the station is associated; while a scan is running it returns the channel
 * being scanned (or 14).  That is why the console used to show the S3 hopping
 * 7 -> 11 -> 14 and the WROOM never stayed in sync. */
volatile bool g_wifi_started = false;
volatile bool g_sta_connected = false;
volatile bool g_ap_started = false;
uint8_t g_fixed_channel = kFixedChannel;

/* Last channel the WROOM reported in its "hello ch=N auto=A" heartbeat. */
volatile uint8_t g_wroom_channel = 0;
volatile bool g_wroom_auto_channel = true;
int64_t g_last_pin_us = 0;

/* ESP-NOW can only be initialised after the WiFi driver is started.  This
 * project starts WiFi much later than the console (board.StartNetwork()), and
 * calling esp_now_init() before esp_wifi_start() crashes inside the driver
 * (load from a null pointer).  Wait for WIFI_EVENT_STA_START/AP_START. */
esp_event_handler_instance_t g_wifi_event_instance = nullptr;

int64_t g_last_hello_us = 0;
int64_t g_last_rx_us = 0;
int64_t g_last_sweep_us = 0;
volatile int64_t g_remote_busy_since_us = 0;
volatile bool g_wroom_busy = false;
volatile uint8_t g_wroom_busy_seq = 0;
/* False until the WROOM has been heard at least once.  Comparing
 * `now - g_last_rx_us` alone is wrong right after boot: g_last_rx_us is 0, so
 * "no packet for 25 s" looks false and the channel-sweep burst is skipped. */
bool g_heard_wroom = false;

uint8_t g_seq = 0;
bool g_pending_reply = false;
int64_t g_pending_us = 0;
int64_t g_pending_timeout_us = kReplyTimeoutUs;
int64_t g_last_command_tx_us = 0;
char g_pending_command[BT_LINK_TEXT_LEN];
uint8_t g_pending_seq = 0;

/* Long-running commands need a longer warning timeout: `scan` waits up to
 * 40 s on the WROOM, `connect` retries 3 times.  The WROOM sends an ACK as
 * soon as it receives the command, so this is only a backstop. */
int64_t TimeoutForCommand(const char* command) {
    if (std::strcmp(command, "scan") == 0 || std::strncmp(command, "cm01", 4) == 0) {
        return 45 * 1000 * 1000;
    }
    if (std::strncmp(command, "connect ", 8) == 0 ||
        std::strncmp(command, "cm02", 4) == 0) {
        return 25 * 1000 * 1000;
    }
    if (std::strncmp(command, "play ", 5) == 0 ||
        std::strncmp(command, "cm07", 4) == 0) {
        return 15 * 1000 * 1000;
    }
    if (std::strcmp(command, "list") == 0 || std::strcmp(command, "devices") == 0 ||
        std::strncmp(command, "cm04", 4) == 0) {
        return 10 * 1000 * 1000;
    }
    return kReplyTimeoutUs;
}

void CopyText(char* dst, size_t len, const char* src) {
    std::snprintf(dst, len, "%s", src != nullptr ? src : "");
    size_t n = std::strlen(dst);
    while (n > 0 && (dst[n - 1] == '\r' || dst[n - 1] == '\n')) {
        dst[--n] = '\0';
    }
}

bool ApplyChannel();

bool SendPacket(uint8_t type, uint8_t seq, const char* text) {
    if (!g_ready) {
        return false;
    }
    if (!ApplyChannel()) {
        return false;       /* radio is scanning - try again on the next retry */
    }

    bt_link_packet_t pkt;
    std::memset(&pkt, 0, sizeof(pkt));
    pkt.magic = BT_LINK_MAGIC;
    pkt.version = BT_LINK_VERSION;
    pkt.type = type;
    pkt.seq = seq;
    CopyText(pkt.text, sizeof(pkt.text), text);

    esp_err_t err = esp_now_send(kBroadcast, reinterpret_cast<const uint8_t*>(&pkt),
                                 sizeof(pkt));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void SendBurst(uint8_t type, uint8_t seq, const char* text, int count) {
    for (int i = 0; i < count; i++) {
        SendPacket(type, seq, text);
        if (i + 1 < count) {
            vTaskDelay(pdMS_TO_TICKS(kBurstGapMs));
        }
    }
}

/* Read the channel from the driver, but only trust values in 1..13.
 * `esp_wifi_get_channel()` returns 14 (and the channel currently being
 * scanned) while a scan is in progress, which is what made the S3 channel
 * jump around. */
bool ReadDriverChannel(uint8_t* out) {
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &second) != ESP_OK) {
        return false;
    }
    if (primary < 1 || primary > 13) {
        return false;
    }
    if (out != nullptr) {
        *out = primary;
    }
    return true;
}

/* Put the radio on the channel used by the ESP-NOW link.
 *
 * - Associated with an AP: the router owns the channel, just follow it.
 * - SoftAP running (WiFi config portal): APSTA, the AP channel is fixed.
 * - Otherwise (scanning for an AP): the driver keeps jumping, so force one
 *   channel ourselves.  If the radio is busy scanning, esp_wifi_set_channel()
 *   fails; the caller then skips the send instead of transmitting on the
 *   channel the scan happens to be on. */
bool ApplyChannel() {
    if (g_sta_connected || g_ap_started) {
        uint8_t ch = 0;
        if (ReadDriverChannel(&ch) && ch != g_channel) {
            ESP_LOGI(TAG, "kenh WiFi = %u", static_cast<unsigned>(ch));
            g_channel = ch;
        }
        return true;
    }

    uint8_t current = 0;
    if (!ReadDriverChannel(&current) || current != g_fixed_channel) {
        esp_err_t err = esp_wifi_set_channel(g_fixed_channel, WIFI_SECOND_CHAN_NONE);
        if (err != ESP_OK) {
            return false;       /* scanning: do not transmit on a random channel */
        }
    }
    if (g_channel != g_fixed_channel) {
        g_channel = g_fixed_channel;
        ESP_LOGI(TAG, "ESP-NOW dung kenh co dinh %u", static_cast<unsigned>(g_channel));
    }
    return true;
}

void AddBroadcastPeer() {
    esp_now_peer_info_t peer;
    std::memset(&peer, 0, sizeof(peer));
    std::memcpy(peer.peer_addr, kBroadcast, 6);
    peer.channel = 0;               /* current channel */
    /* WiFi config mode runs APSTA, a normal boot runs STA; pick the interface
     * that actually exists so esp_now_send() does not fail with ESP_ERR_ESPNOW_IF. */
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    peer.ifidx = (mode == WIFI_MODE_AP) ? WIFI_IF_AP : WIFI_IF_STA;
    peer.encrypt = false;
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGW(TAG, "esp_now_add_peer: %s", esp_err_to_name(err));
    }
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void OnRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    const uint8_t* src = info != nullptr ? info->src_addr : nullptr;
#else
void OnRecv(const uint8_t* src, const uint8_t* data, int len) {
#endif
    if (src == nullptr || data == nullptr || len < static_cast<int>(sizeof(bt_link_packet_t))) {
        return;
    }

    const bt_link_packet_t* pkt = reinterpret_cast<const bt_link_packet_t*>(data);
    if (pkt->magic != BT_LINK_MAGIC || pkt->version != BT_LINK_VERSION) {
        return;
    }
    if (pkt->type != BT_LINK_MSG_STATUS) {
        return;
    }

    const bool first_contact = !g_heard_wroom;
    g_last_rx_us = esp_timer_get_time();
    g_heard_wroom = true;

    if (std::strcmp(pkt->text, "link:busy") == 0) {
        g_wroom_busy = true;
        g_wroom_busy_seq = pkt->seq;
        g_remote_busy_since_us = g_last_rx_us;
        return;
    }
    if (std::strcmp(pkt->text, "link:idle") == 0) {
        if (pkt->seq == g_wroom_busy_seq) {
            g_wroom_busy = false;
            g_remote_busy_since_us = 0;
        }
        return;
    }

    static const char* kCm02Events[] = {
        "cm02-event:devices-ready",
        "cm02-event:files-ready",
        "cm02-event:connect-failed",
        "cm02-event:files-empty",
        "cm02-event:devices-empty",
    };
    for (size_t i = 0; i < sizeof(kCm02Events) / sizeof(kCm02Events[0]); i++) {
        if (std::strcmp(pkt->text, kCm02Events[i]) == 0) {
            if (pkt->seq != g_pending_seq) {
                return;
            }
            g_pending_reply = false;
            g_pending_command[0] = '\0';
            const uint8_t event = static_cast<uint8_t>(i + 1);
            if (g_cm02_event_queue != nullptr) {
                xQueueSend(g_cm02_event_queue, &event, 0);
            }
            return;
        }
    }

    /* "hello ch=..." is the channel heartbeat: acknowledge it internally,
     * do not print it every 5 seconds.  It must not count as the reply to a
     * pending command either, otherwise the timeout warning is suppressed.
     * The very first one is printed: it proves the link works. */
    if (std::strncmp(pkt->text, "hello ", 6) == 0) {
        int ch = 0;
        int auto_channel = -1;
        if (std::sscanf(pkt->text, "hello ch=%d auto=%d", &ch, &auto_channel) >= 2) {
            if (ch >= 1 && ch <= 13) {
                if (g_wroom_channel != 0 && g_wroom_channel != ch) {
                    ESP_LOGI(TAG, "WROOM doi kenh %u -> %d",
                             static_cast<unsigned>(g_wroom_channel), ch);
                }
                g_wroom_channel = static_cast<uint8_t>(ch);
            }
            g_wroom_auto_channel = (auto_channel != 0);
        }
        if (first_contact && g_rx_queue != nullptr) {
            char text[BT_LINK_TEXT_LEN];
            CopyText(text, sizeof(text), pkt->text);
            xQueueSend(g_rx_queue, text, 0);
        }
        return;
    }

    /* ACK of the internal "channel N" command that pins the WROOM to our
     * channel.  Do not print it (it is not a user command) and do not clear a
     * pending user command's reply state. */
    if (std::strncmp(pkt->text, "da nhan lenh: channel ", 22) == 0) {
        g_wroom_auto_channel = false;
        return;
    }

    g_pending_reply = false;

    if (g_rx_queue != nullptr) {
        char text[BT_LINK_TEXT_LEN];
        CopyText(text, sizeof(text), pkt->text);
        if (text[0] != '\0') {
            xQueueSend(g_rx_queue, text, 0);    /* drop if the console is busy */
        }
    }
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void OnSent(const wifi_tx_info_t* tx_info, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS || tx_info == nullptr || tx_info->des_addr == nullptr) {
        return;
    }
    const uint8_t* mac = tx_info->des_addr;
#else
void OnSent(const uint8_t* mac, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS || mac == nullptr) {
        return;
    }
#endif
    ESP_LOGD(TAG, "send failed (%02x:%02x:%02x:%02x:%02x:%02x)",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* WiFi is started by WifiStation long after this task is created.  Remember
 * when that happened; only then is esp_now_init() safe to call. */
void OnWifiEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    (void)data;
    if (base != WIFI_EVENT) {
        return;
    }

    switch (id) {
        case WIFI_EVENT_STA_START:
            if (!g_wifi_started) {
                ESP_LOGI(TAG, "WiFi started - initialising ESP-NOW");
            }
            g_wifi_started = true;
            g_sta_connected = false;
            break;
        case WIFI_EVENT_STA_CONNECTED:
            g_sta_connected = true;
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            g_sta_connected = false;
            break;
        case WIFI_EVENT_AP_START:
            if (!g_wifi_started) {
                ESP_LOGI(TAG, "WiFi AP started - initialising ESP-NOW");
            }
            g_wifi_started = true;
            g_ap_started = true;
            break;
        case WIFI_EVENT_AP_STOP:
            g_ap_started = false;
            break;
        default:
            break;
    }
}

bool TryInit() {
    if (!g_wifi_started) {
        /* Waiting for WIFI_EVENT_STA_START; do not touch esp_now_init yet. */
        return false;
    }

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "esp_now_init: %s", esp_err_to_name(err));
        return false;
    }

    esp_now_register_recv_cb(OnRecv);
    esp_now_register_send_cb(OnSent);
    AddBroadcastPeer();
    ApplyChannel();

    g_ready = true;
    ESP_LOGI(TAG, "ESP-NOW ready on channel %u", static_cast<unsigned>(g_channel));
    /* Also show it on the UART1 console: the user usually only watches that
     * port, and "-> WROOM" without this line is not proof of a live link. */
    UartConsole::Notify("[BT] ESP-NOW san sang, kenh " + std::to_string(g_channel));
    return true;
}

void BtLinkTask(void* arg) {
    (void)arg;
    char command[BT_LINK_TEXT_LEN];
    char status[BT_LINK_TEXT_LEN];
    const int64_t wait_start_us = esp_timer_get_time();
    bool wait_warned = false;

    for (;;) {
        if (!g_ready) {
            if (!TryInit()) {
                /* One-shot hint on UART1: without it the console just says
                 * "ESP-NOW chua san sang" with no clue why. */
                if (!wait_warned && (esp_timer_get_time() - wait_start_us) > 15 * 1000 * 1000) {
                    wait_warned = true;
                    UartConsole::Notify(g_wifi_started
                                            ? "[BT] esp_now_init loi - xem log UART0"
                                            : "[BT] WiFi chua start sau 15s - ESP-NOW chua the khoi tao");
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        ApplyChannel();

        /* Print every status line from the WROOM on the UART console. */
        while (xQueueReceive(g_rx_queue, status, 0) == pdTRUE) {
            UartConsole::Notify(std::string("[WROOM] ") + status);
        }

        /* Forward one queued command. */
        if (xQueueReceive(g_tx_queue, command, 0) == pdTRUE) {
            const int64_t now = esp_timer_get_time();

            /* If the WROOM has not been heard for a long time it may be
             * hunting channels: send a HELLO burst long enough to cover one
             * full hop cycle (13 channels x 350 ms = 4.55 s) before the actual
             * command, and stop as soon as the WROOM answers. */
            if (!g_heard_wroom || (now - g_last_rx_us) > kRxFreshUs) {
                /* Do not repeat the 5 s burst for every queued command: if we
                 * did it within the last 20 s, just send the command and let
                 * the 1 s HELLO heartbeat keep searching. */
                if (g_last_sweep_us == 0 ||
                    (now - g_last_sweep_us) > 20 * 1000 * 1000) {
                    g_last_sweep_us = now;
                    ApplyChannel();
                    UartConsole::Notify(
                        "[BT] dang dong bo kenh voi WROOM (kenh S3 = " +
                        std::to_string(g_channel) + ", toi da 5 giay)...");

                    for (int i = 0; i < 34; i++) {
                        SendPacket(BT_LINK_MSG_HELLO, 0, "hello");
                        vTaskDelay(pdMS_TO_TICKS(150));
                        if (g_heard_wroom &&
                            (esp_timer_get_time() - g_last_rx_us) < 1000 * 1000) {
                            break;      /* WROOM found us - no need to search more */
                        }
                    }

                    if (!g_heard_wroom ||
                        (esp_timer_get_time() - g_last_rx_us) > 2 * 1000 * 1000) {
                        UartConsole::Notify(
                            "[BT] chua thay WROOM sau khi quet kenh - kiem tra: "
                            "da nap firmware moi, da bat nguon, khong dang phat nhac");
                        UartConsole::Notify(
                            "[BT] hoac dat cung kenh: tren WROOM go 'channel N' "
                            "(N = kenh S3 o tren), roi 'status' de kiem tra");
                    }
                }
            }

            g_seq++;
            g_pending_seq = g_seq;
            CopyText(g_pending_command, sizeof(g_pending_command), command);
            g_pending_us = esp_timer_get_time();
            g_pending_timeout_us = TimeoutForCommand(command);
            g_pending_reply = true;
            ESP_LOGI(TAG, "gui lenh: %s (seq %u)", command, static_cast<unsigned>(g_seq));
            SendBurst(BT_LINK_MSG_COMMAND, g_seq, command, kBurstCount);
            g_last_command_tx_us = esp_timer_get_time();
            continue;
        }

        const int64_t now = esp_timer_get_time();

        /* Keep the WROOM on our channel.  While it is still hunting it
         * reports "hello ch=N auto=1"; send it "channel N" so it locks there
         * (and stops hopping, which is required while it streams audio).
         * Only when no user command is pending, so the two seq numbers cannot
         * confuse the ACK tracking. */
        if (g_ready && g_heard_wroom && !g_wroom_busy &&
            !g_pending_reply && !g_cm02_workflow_active &&
            (g_wroom_auto_channel || g_wroom_channel != g_channel) &&
            (now - g_last_pin_us) > kPinRetryUs) {
            g_last_pin_us = now;
            char pin[16];
            std::snprintf(pin, sizeof(pin), "channel %u",
                          static_cast<unsigned>(g_channel));
            g_seq++;
            ESP_LOGI(TAG, "ghim WROOM vao kenh %u", static_cast<unsigned>(g_channel));
            SendBurst(BT_LINK_MSG_COMMAND, g_seq, pin, 2);
        }

        /* Retransmit the pending command with the same seq until the WROOM
         * ACKs.  The WROOM dedups for 8 s, so stop before that. */
        if (g_pending_reply && g_pending_command[0] != '\0' &&
            (now - g_last_command_tx_us) >= kCommandRetryGapMs * 1000 &&
            (now - g_pending_us) < kCommandRetryWindowUs) {
            g_last_command_tx_us = now;
            SendPacket(BT_LINK_MSG_COMMAND, g_pending_seq, g_pending_command);
        }

        /* Channel heartbeat so the WROOM stays locked to our WiFi channel. */
        const int64_t hello_gap = (g_heard_wroom && (now - g_last_rx_us) < kRxFreshUs)
                                      ? kHelloConfirmedUs
                                      : kHelloSearchUs;
        if (g_wroom_busy &&
            (now - g_remote_busy_since_us) > kRemoteBusyTimeoutUs) {
            ESP_LOGW(TAG, "WROOM busy status timeout; resume ESP-NOW heartbeat");
            g_wroom_busy = false;
            g_remote_busy_since_us = 0;
        }
        if (!g_wroom_busy && !g_pending_reply &&
            (now - g_last_hello_us) >= hello_gap) {
            SendBurst(BT_LINK_MSG_HELLO, 0, "hello", kHelloBurst);
            g_last_hello_us = esp_timer_get_time();
        }

        if (g_pending_reply &&
            (now - g_pending_us) > g_pending_timeout_us) {
            g_pending_reply = false;
            g_pending_command[0] = '\0';
            UartConsole::Notify(
                "[WROOM] khong nhan duoc phan hoi - kiem tra ESP-NOW/kenh WiFi "
                "(go 'music ping' de thu lai)");
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

}  // namespace

void BtLink::Start() {
    if (g_task != nullptr) {
        return;
    }

    if (g_tx_queue == nullptr) {
        g_tx_queue = xQueueCreate(kQueueLen, BT_LINK_TEXT_LEN);
    }
    if (g_rx_queue == nullptr) {
        g_rx_queue = xQueueCreate(kQueueLen, BT_LINK_TEXT_LEN);
    }
    if (g_cm02_event_queue == nullptr) {
        g_cm02_event_queue = xQueueCreate(4, sizeof(uint8_t));
    }
    if (g_tx_queue == nullptr || g_rx_queue == nullptr || g_cm02_event_queue == nullptr) {
        ESP_LOGE(TAG, "failed to create queues");
        return;
    }

    /* The default event loop already exists (main.cc creates it before the
     * application starts), so this cannot miss WIFI_EVENT_STA_START. */
    if (g_wifi_event_instance == nullptr) {
        esp_err_t err = esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, OnWifiEvent, nullptr, &g_wifi_event_instance);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "register WIFI_EVENT: %s", esp_err_to_name(err));
        }
    }

    if (xTaskCreate(BtLinkTask, "bt_link", 4096, nullptr, 2, &g_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create task");
        g_task = nullptr;
    }
}

bool BtLink::SendCommand(const std::string& command) {
    Start();
    if (g_tx_queue == nullptr || command.empty()) {
        return false;
    }
    if (command.size() >= BT_LINK_TEXT_LEN) {
        return false;
    }

    /* The queue stores fixed-size items, so copy the string into a full-size
     * buffer (sending c_str() would make FreeRTOS read past the string). */
    char item[BT_LINK_TEXT_LEN];
    CopyText(item, sizeof(item), command.c_str());
    return xQueueSend(g_tx_queue, item, 0) == pdTRUE;
}

void BtLink::ClearCm02Events() {
    if (g_cm02_event_queue == nullptr) {
        return;
    }
    uint8_t event;
    while (xQueueReceive(g_cm02_event_queue, &event, 0) == pdTRUE) {
    }
}

bool BtLink::WaitForCm02Event(Cm02Event* event, uint32_t timeout_ms) {
    if (event == nullptr || g_cm02_event_queue == nullptr) {
        return false;
    }
    uint8_t value = 0;
    if (xQueueReceive(g_cm02_event_queue, &value, pdMS_TO_TICKS(timeout_ms)) != pdTRUE ||
        value < static_cast<uint8_t>(Cm02Event::DevicesReady) ||
        value > static_cast<uint8_t>(Cm02Event::DevicesEmpty)) {
        return false;
    }
    *event = static_cast<Cm02Event>(value);
    return true;
}

void BtLink::SetCm02WorkflowActive(bool active) {
    g_cm02_workflow_active = active;
}

bool BtLink::IsReady() {
    return g_ready;
}

uint8_t BtLink::Channel() {
    return g_channel;
}

bool BtLink::SetChannel(uint8_t channel) {
    if (!g_ready) {
        return false;
    }
    if (channel == 0) {
        g_fixed_channel = kFixedChannel;
        g_last_pin_us = 0;
        g_wroom_auto_channel = true;
        return true;
    }
    if (channel < 1 || channel > 13) {
        return false;
    }
    if (g_sta_connected) {
        return false;       /* the router owns the channel */
    }
    if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        return false;
    }
    g_fixed_channel = channel;
    g_channel = channel;
    /* Ask the task to send the new channel to the WROOM right away. */
    g_last_pin_us = 0;
    g_wroom_auto_channel = true;
    ESP_LOGI(TAG, "kenh ESP-NOW doi thanh %u", static_cast<unsigned>(channel));
    return true;
}

bool BtLink::HeardWroom() {
    return g_heard_wroom;
}

int BtLink::LastHeardAgeSec() {
    if (!g_heard_wroom) {
        return -1;
    }
    return static_cast<int>((esp_timer_get_time() - g_last_rx_us) / 1000000);
}
