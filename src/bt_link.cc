#include "bt_link.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

#include "uart_console.h"
#include "s3_music_streamer.h"

#define TAG "BtLink"

namespace {

constexpr int kQueueLen = 8;
constexpr int kStatusQueueLen = 24;
constexpr int kCommandRetryGapMs = 500;
constexpr int64_t kCommandRetryWindowUs = 6500 * 1000;
constexpr int64_t kReplyTimeoutUs = 8 * 1000 * 1000;
constexpr int64_t kRemoteBusyTimeoutUs = 60 * 1000 * 1000;

QueueHandle_t g_tx_queue;
QueueHandle_t g_rx_queue;
QueueHandle_t g_cm02_event_queue;
TaskHandle_t g_task;
TaskHandle_t g_server_task;
SemaphoreHandle_t g_socket_mutex;

volatile bool g_wifi_started;
volatile bool g_ap_started;
volatile bool g_ready;
volatile bool g_heard_wroom;
volatile bool g_cm02_workflow_active;
volatile bool g_wroom_busy;
volatile bool g_pending_reply;
volatile int64_t g_last_rx_us;
volatile int64_t g_remote_busy_since_us;
volatile uint8_t g_wroom_busy_seq;
volatile uint8_t g_pending_seq;
uint8_t g_seq;
int g_client_socket = -1;
char g_pending_command[BT_LINK_TEXT_LEN];
int64_t g_pending_us;
int64_t g_pending_timeout_us;
int64_t g_last_command_tx_us;
esp_netif_t* g_ap_netif;
esp_event_handler_instance_t g_wifi_event_instance;

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

bool SendAll(int socket, const void* data, size_t len) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < len) {
        int ret = send(socket, bytes + sent, len - sent, 0);
        if (ret <= 0) {
            ESP_LOGW(TAG, "send() failed: errno=%d (%s)", errno,
                     ret < 0 ? strerror(errno) : "closed");
            return false;
        }
        sent += static_cast<size_t>(ret);
    }
    return true;
}

bool SendPacket(uint8_t type, uint8_t seq, const char* text) {
    if (!g_ready || g_socket_mutex == nullptr) {
        return false;
    }
    bt_link_packet_t pkt{};
    pkt.magic = BT_LINK_MAGIC;
    pkt.version = BT_LINK_VERSION;
    pkt.type = type;
    pkt.seq = seq;
    CopyText(pkt.data.text, sizeof(pkt.data.text), text);
    pkt.length = static_cast<uint16_t>(std::strlen(pkt.data.text) + 1);

    xSemaphoreTake(g_socket_mutex, portMAX_DELAY);
    const int socket = g_client_socket;
    const bool sent = socket >= 0 && SendAll(socket, &pkt, sizeof(pkt));
    xSemaphoreGive(g_socket_mutex);
    if (!sent) {
        ESP_LOGD(TAG, "TCP send failed or WROOM is not connected");
    }
    return sent;
}

void QueueStatusText(const char* text) {
    if (g_rx_queue == nullptr || text == nullptr || text[0] == '\0') {
        return;
    }
    char copy[BT_LINK_TEXT_LEN];
    CopyText(copy, sizeof(copy), text);
    xQueueSend(g_rx_queue, copy, 0);
}

void HandleStatus(const bt_link_packet_t& pkt) {
    if (pkt.type != BT_LINK_MSG_STATUS || pkt.length == 0 ||
        pkt.length > sizeof(pkt.data.text) ||
        std::memchr(pkt.data.text, '\0', pkt.length) == nullptr) {
        return;
    }
    const char* text = pkt.data.text;
    const bool first_contact = !g_heard_wroom;
    g_last_rx_us = esp_timer_get_time();
    g_heard_wroom = true;

    if (std::strcmp(text, "link:busy") == 0) {
        g_wroom_busy = true;
        g_wroom_busy_seq = pkt.seq;
        g_remote_busy_since_us = g_last_rx_us;
        return;
    }
    if (std::strcmp(text, "link:idle") == 0) {
        if (pkt.seq == g_wroom_busy_seq) {
            g_wroom_busy = false;
            g_remote_busy_since_us = 0;
        }
        return;
    }
    if (std::strcmp(text, "link:connected") == 0) {
        if (first_contact) {
            QueueStatusText("ket noi TCP qua Wi-Fi AP AI_BOT");
        }
        return;
    }
    if (std::strcmp(text, "stream:pause") == 0) {
        S3MusicStreamer::SetPaused(true);
        return;
    }
    if (std::strcmp(text, "stream:resume") == 0) {
        S3MusicStreamer::SetPaused(false);
        return;
    }

    static const char* kCm02Events[] = {
        "cm02-event:devices-ready",
        "cm02-event:files-ready",
        "cm02-event:connect-failed",
        "cm02-event:files-empty",
        "cm02-event:devices-empty",
        "cm02-event:playback-ready",
        "cm02-event:stream-ready",
    };
    for (size_t i = 0; i < sizeof(kCm02Events) / sizeof(kCm02Events[0]); i++) {
        if (std::strcmp(text, kCm02Events[i]) == 0) {
            if (pkt.seq != g_pending_seq) {
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

    if (std::strncmp(text, "da nhan lenh: channel ", 22) == 0) {
        return;
    }

    g_pending_reply = false;
    QueueStatusText(text);
}

void OnWifiEvent(void*, esp_event_base_t base, int32_t id, void*) {
    if (base != WIFI_EVENT) {
        return;
    }
    if (id == WIFI_EVENT_STA_START) {
        g_wifi_started = true;
        ESP_LOGI(TAG, "Wi-Fi STA da khoi dong; bat AP AI_BOT");
    } else if (id == WIFI_EVENT_STA_STOP) {
        g_wifi_started = false;
    } else if (id == WIFI_EVENT_AP_START) {
        g_ap_started = true;
        ESP_LOGI(TAG, "SoftAP AI_BOT san sang tai 192.168.4.1");
    } else if (id == WIFI_EVENT_AP_STOP) {
        g_ap_started = false;
        ESP_LOGW(TAG, "SoftAP AI_BOT da dung");
    }
}

bool StartAccessPoint() {
    if (g_ap_netif == nullptr) {
        g_ap_netif = esp_netif_create_default_wifi_ap();
        if (g_ap_netif == nullptr) {
            ESP_LOGE(TAG, "khong tao duoc AP netif");
            return false;
        }
    }

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA): %s", esp_err_to_name(err));
        return false;
    }

    wifi_config_t config{};
    std::snprintf(reinterpret_cast<char*>(config.ap.ssid),
                  sizeof(config.ap.ssid), "%s", BT_LINK_AP_SSID);
    std::snprintf(reinterpret_cast<char*>(config.ap.password),
                  sizeof(config.ap.password), "%s", BT_LINK_AP_PASSWORD);
    config.ap.ssid_len = std::strlen(BT_LINK_AP_SSID);
    config.ap.channel = 1;
    config.ap.max_connection = 1;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.pmf_cfg.capable = true;
    config.ap.pmf_cfg.required = false;
    err = esp_wifi_set_config(WIFI_IF_AP, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP): %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "SoftAP SSID=%s; TCP port=%u", BT_LINK_AP_SSID,
             static_cast<unsigned>(BT_LINK_TCP_PORT));
    return true;
}

void CloseClientSocket() {
    xSemaphoreTake(g_socket_mutex, portMAX_DELAY);
    if (g_client_socket >= 0) {
        shutdown(g_client_socket, SHUT_RDWR);
        close(g_client_socket);
        g_client_socket = -1;
    }
    xSemaphoreGive(g_socket_mutex);
}

void BtLinkServerTask(void*) {
    while (!g_ap_started) {
        if (g_wifi_started && !StartAccessPoint()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listener < 0) {
        ESP_LOGE(TAG, "TCP socket create failed: errno=%d", errno);
        vTaskDelete(nullptr);
        return;
    }
    int reuse = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        ESP_LOGW(TAG, "TCP SO_REUSEADDR setup failed: errno=%d", errno);
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(BT_LINK_TCP_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        ESP_LOGE(TAG, "TCP listen failed: errno=%d", errno);
        close(listener);
        vTaskDelete(nullptr);
        return;
    }

    g_ready = true;
    ESP_LOGI(TAG, "TCP server san sang tai %s:%u", BT_LINK_AP_IP,
             static_cast<unsigned>(BT_LINK_TCP_PORT));
    UartConsole::Notify("[BT] AP S3 AI_BOT san sang; TCP port 3333");

    for (;;) {
        if (!g_ap_started) {
            if (g_wifi_started) {
                StartAccessPoint();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        fd_set listen_set;
        FD_ZERO(&listen_set);
        FD_SET(listener, &listen_set);
        timeval accept_timeout{1, 0};
        const int ready = select(listener + 1, &listen_set, nullptr, nullptr, &accept_timeout);
        if (ready == 0) {
            continue;
        }
        if (ready < 0) {
            if (errno != EINTR) {
                ESP_LOGW(TAG, "TCP listener select failed: errno=%d", errno);
            }
            continue;
        }
        int client = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (client < 0) {
            ESP_LOGW(TAG, "TCP accept failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        timeval send_timeout{10, 0};
        if (setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                       &send_timeout, sizeof(send_timeout)) != 0) {
            ESP_LOGW(TAG, "TCP send timeout setup failed: errno=%d", errno);
        }
        CloseClientSocket();
        xSemaphoreTake(g_socket_mutex, portMAX_DELAY);
        g_client_socket = client;
        xSemaphoreGive(g_socket_mutex);
        g_heard_wroom = false;
        int64_t last_packet_us = esp_timer_get_time();
        ESP_LOGI(TAG, "WROOM TCP connected");

        bt_link_packet_t pkt{};
        size_t received = 0;
        while (true) {
            if (esp_timer_get_time() - last_packet_us > 15 * 1000 * 1000) {
                ESP_LOGW(TAG, "WROOM heartbeat timeout; closing stale TCP session");
                break;
            }
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(client, &read_set);
            timeval timeout{0, 200000};
            int ready = select(client + 1, &read_set, nullptr, nullptr, &timeout);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (ready == 0) {
                continue;
            }
            int ret = recv(client, reinterpret_cast<uint8_t*>(&pkt) + received,
                           sizeof(pkt) - received, 0);
            if (ret <= 0) {
                break;
            }
            received += static_cast<size_t>(ret);
            if (received == sizeof(pkt)) {
                if (pkt.magic == BT_LINK_MAGIC && pkt.version == BT_LINK_VERSION) {
                    HandleStatus(pkt);
                    last_packet_us = esp_timer_get_time();
                }
                received = 0;
            }
        }
        CloseClientSocket();
        g_heard_wroom = false;
        g_wroom_busy = false;
        ESP_LOGW(TAG, "WROOM TCP disconnected");
    }
}

void BtLinkTask(void*) {
    char command[BT_LINK_TEXT_LEN];
    char status[BT_LINK_TEXT_LEN];

    for (;;) {
        if (!g_ready) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        while (xQueueReceive(g_rx_queue, status, 0) == pdTRUE) {
            UartConsole::Notify(std::string("[WROOM] ") + status);
        }

        if (xQueueReceive(g_tx_queue, command, 0) == pdTRUE) {
            ++g_seq;
            g_pending_seq = g_seq;
            CopyText(g_pending_command, sizeof(g_pending_command), command);
            g_pending_us = esp_timer_get_time();
            g_pending_timeout_us = TimeoutForCommand(command);
            g_pending_reply = true;
            ESP_LOGI(TAG, "TCP -> WROOM: %s (seq %u)", command,
                     static_cast<unsigned>(g_seq));
            SendPacket(BT_LINK_MSG_COMMAND, g_seq, command);
            g_last_command_tx_us = esp_timer_get_time();
            continue;
        }

        const int64_t now = esp_timer_get_time();
        if (g_wroom_busy && now - g_remote_busy_since_us > kRemoteBusyTimeoutUs) {
            g_wroom_busy = false;
            g_remote_busy_since_us = 0;
        }
        if (g_pending_reply && g_pending_command[0] != '\0' &&
            now - g_last_command_tx_us >= kCommandRetryGapMs * 1000 &&
            now - g_pending_us < kCommandRetryWindowUs) {
            g_last_command_tx_us = now;
            SendPacket(BT_LINK_MSG_COMMAND, g_pending_seq, g_pending_command);
        }
        if (g_pending_reply && now - g_pending_us > g_pending_timeout_us) {
            g_pending_reply = false;
            g_pending_command[0] = '\0';
            UartConsole::Notify("[WROOM] khong nhan duoc phan hoi TCP - kiem tra WROOM/AP AI_BOT");
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

}  // namespace

void BtLink::Start() {
    if (g_task != nullptr) {
        return;
    }
    g_tx_queue = xQueueCreate(kQueueLen, BT_LINK_TEXT_LEN);
    g_rx_queue = xQueueCreate(kStatusQueueLen, BT_LINK_TEXT_LEN);
    g_cm02_event_queue = xQueueCreate(4, sizeof(uint8_t));
    g_socket_mutex = xSemaphoreCreateMutex();
    if (g_tx_queue == nullptr || g_rx_queue == nullptr ||
        g_cm02_event_queue == nullptr || g_socket_mutex == nullptr) {
        ESP_LOGE(TAG, "failed to create link queues/mutex");
        return;
    }

    esp_err_t err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, OnWifiEvent, nullptr, &g_wifi_event_instance);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register Wi-Fi events: %s", esp_err_to_name(err));
        return;
    }
    if (xTaskCreate(BtLinkTask, "bt_link", 4096, nullptr, 2, &g_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create TCP link tasks");
        return;
    }
    if (xTaskCreate(BtLinkServerTask, "bt_tcp_server", 4096, nullptr, 3,
                    &g_server_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create TCP server task");
        vTaskDelete(g_task);
        g_task = nullptr;
    }
}

bool BtLink::SendPcmPacket(uint8_t type, const void* data, size_t length) {
    if ((type != BT_LINK_MSG_PCM_START && type != BT_LINK_MSG_PCM_DATA &&
         type != BT_LINK_MSG_PCM_END && type != BT_LINK_MSG_PCM_STOP) ||
        length > BT_LINK_PCM_CHUNK_LEN ||
        (length > 0 && data == nullptr) || !g_ready || g_socket_mutex == nullptr) {
        return false;
    }

    bt_link_packet_t pkt{};
    pkt.magic = BT_LINK_MAGIC;
    pkt.version = BT_LINK_VERSION;
    pkt.type = type;
    pkt.length = static_cast<uint16_t>(length);
    if (length > 0) {
        std::memcpy(pkt.data.pcm, data, length);
    }

    xSemaphoreTake(g_socket_mutex, portMAX_DELAY);
    const int socket = g_client_socket;
    const bool sent = socket >= 0 && SendAll(socket, &pkt, sizeof(pkt));
    xSemaphoreGive(g_socket_mutex);
    if (!sent) {
        ESP_LOGW(TAG, "TCP PCM send failed (type=%u, length=%u)",
                 static_cast<unsigned>(type), static_cast<unsigned>(length));
    }
    return sent;
}

bool BtLink::SendCommand(const std::string& command) {
    Start();
    if (g_tx_queue == nullptr || command.empty() || command.size() >= BT_LINK_TEXT_LEN) {
        return false;
    }
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
        value > static_cast<uint8_t>(Cm02Event::StreamReady)) {
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

bool BtLink::HeardWroom() {
    return g_heard_wroom;
}
