#include "uart_console.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ssid_manager.h>
#include <wifi_station.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "application.h"
#include "board.h"
#include "bt_link.h"
#include "device_state.h"
#include "settings.h"
#include "system_info.h"

#define TAG "UartConsole"

#define CONSOLE_MAX_LINE 220
#define CONSOLE_RX_BUFFER 2048

namespace {

Application* g_app = nullptr;

const char* StateName(DeviceState state) {
    switch (state) {
        case kDeviceStateUnknown:        return "unknown";
        case kDeviceStateStarting:       return "starting";
        case kDeviceStateWifiConfiguring:return "wifi_configuring";
        case kDeviceStateIdle:           return "idle";
        case kDeviceStateConnecting:     return "connecting";
        case kDeviceStateListening:      return "listening";
        case kDeviceStateSpeaking:       return "speaking";
        case kDeviceStateUpgrading:      return "upgrading";
        case kDeviceStateActivating:     return "activating";
        case kDeviceStateAudioTesting:   return "audio_testing";
        case kDeviceStateFatalError:     return "fatal_error";
        default:                         return "?";
    }
}

/* Write raw bytes to the dedicated console UART. */
void ConsoleWrite(const char* data, size_t len) {
    if (data != nullptr && len > 0) {
        uart_write_bytes(CONSOLE_UART_NUM, data, len);
    }
}

/* Message for the user: shown on the console UART and in the ESP_LOG (UART0). */
void Print(const char* fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    ESP_LOGI(TAG, "%s", buffer);

    std::string line(buffer);
    line += "\r\n";
    ConsoleWrite(line.data(), line.size());
}

/* Guard against a noisy/floating RX line (or an accidental TX->RX loopback).
 *
 * Without it, every byte the console prints can come straight back in as
 * "input", get treated as a text query, produce another message, and so on:
 * the log ends up flooded with garbage lines within milliseconds.
 * Anything above MAX_LINES_PER_WINDOW inside WINDOW_US mutes the RX for
 * MUTE_US, and while muted bytes are dropped *without* being echoed, which
 * breaks the feedback loop instead of feeding it. */
constexpr int64_t RX_WINDOW_US = 2 * 1000 * 1000;
constexpr int64_t RX_MUTE_US = 5 * 1000 * 1000;
constexpr int RX_MAX_LINES_PER_WINDOW = 8;

int64_t g_rx_muted_until_us = 0;
int64_t g_rx_window_start_us = 0;
int g_rx_lines_in_window = 0;

bool RxIsMuted() {
    return esp_timer_get_time() < g_rx_muted_until_us;
}

/* Returns true when the line must be dropped. */
bool RxFloodDetected() {
    const int64_t now = esp_timer_get_time();
    if (now < g_rx_muted_until_us) {
        return true;
    }
    if (now - g_rx_window_start_us > RX_WINDOW_US) {
        g_rx_window_start_us = now;
        g_rx_lines_in_window = 0;
    }
    if (++g_rx_lines_in_window <= RX_MAX_LINES_PER_WINDOW) {
        return false;
    }

    g_rx_muted_until_us = now + RX_MUTE_US;
    Print("!! UART%d nhan qua nhieu du lieu - tam dung doc %d giay.",
          (int)CONSOLE_UART_NUM, (int)(RX_MUTE_US / 1000000));
    Print("   kiem tra day RX=GPIO%d: noi vao TX cua USB-TTL, khong de trong",
          (int)CONSOLE_UART_RX_PIN);
    Print("   va KHONG noi GPIO%d (TX board) chung voi GPIO%d (RX board).",
          (int)CONSOLE_UART_TX_PIN, (int)CONSOLE_UART_RX_PIN);
    return true;
}

void PrintHelp() {
    Print("---- Xiaozhi UART console (UART%d @ %d) ----", (int)CONSOLE_UART_NUM,
          (int)CONSOLE_UART_BAUD_RATE);
    Print("  media off        tat mic + loa, chat bang text qua UART");
    Print("  media on         bat lai mic + loa, chat bang giong noi");
    Print("  media            xem dang o che do nao");
    Print("  state            in trang thai thiet bi");
    Print("  device           ma thiet bi: MAC, UUID, ma kich hoat");
    Print("  wifi             xem trang thai + mang da luu");
    Print("  wifi <ssid> <pw> luu WiFi roi khoi dong lai");
    Print("  wifi scan        quet cac AP xung quanh (ten/kenh/RSSI)");
    Print("  wifi ap          vao che do AP cau hinh (TienHuyIoT-XXXX)");
    Print("  wifi clear       xoa het WiFi da luu");
    Print("  music <lenh>     dieu khien loa Bluetooth qua ESP32-WROOM");
    Print("                   go 'music' hoac 'menu' de mo menu dieu khien");
    Print("  help             danh sach lenh nay");
    Print("  AI: <cau hoi>    (chi khi media OFF) gui cau hoi toi AI");
    Print("                   cac lenh cm00..cm09 duoc danh rieng cho Bluetooth");
}

std::string Trim(const std::string& text);
std::string ToLowerCopy(const std::string& text);
bool StartsWith(const std::string& text, const char* prefix);
void SkipEscapeSequence();

class Cm02WorkflowGuard {
public:
    Cm02WorkflowGuard() {
        BtLink::SetCm02WorkflowActive(true);
    }

    ~Cm02WorkflowGuard() {
        BtLink::SetCm02WorkflowActive(false);
    }
};

/* Prompt without a trailing newline (Print() always adds one). */
void PrintInline(const char* text) {
    if (text == nullptr) {
        return;
    }
    ConsoleWrite(text, std::strlen(text));
}

void PrintMusicMenu() {
    Print("===== AI_BOT -> Loa Bluetooth (ESP32-WROOM qua ESP-NOW) =====");
    if (BtLink::IsReady()) {
        Print("ESP-NOW: san sang, kenh S3 = %u",
              static_cast<unsigned>(BtLink::Channel()));
    } else {
        Print("ESP-NOW: dang khoi tao (doi 2-5 giay, WiFi phai start truoc)");
        Print("         dong [BT] ESP-NOW san sang se hien khi xong");
    }
    Print("cm01) Quet thiet bi Bluetooth");
    Print("cm02) Chon thiet bi + file MP3 de phat");
    Print("cm03) Dung phat / ngat ket noi");
    Print("cm04) Liet ke file MP3");
    Print("cm05) Xem trang thai (status)");
    Print("cm06) Lap lai bai vua phat");
    Print("cm07) Chon bai khac (giu ket noi)");
    Print("cm08) Tam dung / tiep tuc (pause/resume)");
    Print("cm09) Danh sach lenh dieu khien");
    Print("cm00) In lai menu");
    Print("Go cm01..cm09 (hoac 1..9), hoac lenh: play 1 / pause / resume / next / prev / stop / list / scan / status");
}

void PrintMusicHelp() {
    Print("---- Dieu khien loa Bluetooth qua ESP32-WROOM (ESP-NOW) ----");
    Print("  menu / music        in lai menu dieu khien");
    Print("  cm01..cm09          chon muc menu, gui ma cmNN sang WROOM");
    Print("                      vd: cm01 = scan, cm05 = status, cm08 = pause/resume");
    Print("  status              trang thai A2DP / bai hat / kenh ESP-NOW");
    Print("  list                liet ke file MP3 tren WROOM");
    Print("  devices             liet ke thiet bi Bluetooth da quet");
    Print("  scan                quet thiet bi Bluetooth (~13 giay)");
    Print("  connect <n>         ket noi thiet bi so n");
    Print("  play [n]            phat file so n (khong co n = phat lai)");
    Print("  pause               tam dung, giu ket noi + vi tri bai");
    Print("  resume              tiep tuc phat");
    Print("  next | prev         bai tiep theo / bai truoc");
    Print("  replay              phat lai bai vua phat");
    Print("  stop                dung phat, giu ket noi A2DP");
    Print("  disconnect          dung phat + ngat ket noi A2DP");
    Print("  channel             xem kenh ESP-NOW cua S3");
    Print("  channel <n>         dat kenh cho WROOM (1..13)");
    Print("  channel auto        de WROOM tu do kenh");
    Print("  ping                kiem tra duong ESP-NOW");
    Print("  (go duoc ca dang 'music <lenh>' / 'bt <lenh>')");
}

/* Doc 1 dong tu UART1, echo lai tung ky tu nhu console WROOM.
 * Tra ve false khi het thoi gian cho hoac gap loi UART. */
bool ReadConsoleLine(std::string* out, int timeout_ms) {
    if (out == nullptr) {
        return false;
    }
    out->clear();
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    uint8_t ch = 0;

    /* Enter usually arrives as CR+LF.  The LF is often still queued when the
     * prompt starts, which would cancel it immediately.  Drop leftover
     * terminators; a real character is kept and processed below. */
    int read = uart_read_bytes(CONSOLE_UART_NUM, &ch, 1, pdMS_TO_TICKS(20));
    while (read == 1 && (ch == '\r' || ch == '\n')) {
        read = uart_read_bytes(CONSOLE_UART_NUM, &ch, 1, pdMS_TO_TICKS(20));
    }

    while (esp_timer_get_time() < deadline) {
        if (read != 1) {
            read = uart_read_bytes(CONSOLE_UART_NUM, &ch, 1, pdMS_TO_TICKS(100));
            if (read != 1) {
                continue;
            }
        }

        if (ch == '\r' || ch == '\n') {
            ConsoleWrite("\r\n", 2);
            return true;
        }
        if (ch == 0x1B) {
            SkipEscapeSequence();
        } else if (ch == 0x08 || ch == 0x7F) {
            if (!out->empty()) {
                out->pop_back();
                ConsoleWrite("\b \b", 3);
            }
        } else if (ch >= 0x20) {
            if (out->size() < 32) {
                out->push_back(static_cast<char>(ch));
                ConsoleWrite(reinterpret_cast<const char*>(&ch), 1);
            }
        }
        read = 0;
    }

    Print("(het thoi gian cho, da huy)");
    return false;
}

/* Hoi 1 so nguyen trong khoang [min, max]. Enter = huy (tra ve false). */
bool PromptNumber(const char* prompt, int min, int max, int* out) {
    for (;;) {
        PrintInline(prompt);
        std::string line;
        if (!ReadConsoleLine(&line, 20000)) {
            return false;
        }
        line = Trim(line);
        if (line.empty()) {
            return false;
        }

        char* end = nullptr;
        const long value = std::strtol(line.c_str(), &end, 10);
        if (end == line.c_str() || *end != '\0') {
            Print("Chi nhap so.");
            continue;
        }
        if (value < min || value > max) {
            Print("Ngoai khoang %d..%d.", min, max);
            continue;
        }
        *out = static_cast<int>(value);
        return true;
    }
}

/* Gui 1 lenh cho WROOM va in lai cho nguoi dung thay. */
bool SendToWroom(const std::string& command) {
    if (command.empty()) {
        return false;
    }
    if (!BtLink::IsReady()) {
        /* ESP-NOW must not be initialised before WiFi starts, so a command
         * queued now would be dropped silently.  Tell the user instead of
         * pretending the command was sent. */
        Print("ESP-NOW chua san sang (WiFi chua khoi dong xong).");
        Print("Doi 2-3 giay roi go lai; kiem tra bang 'channel'.");
        return false;
    }
    if (!BtLink::SendCommand(command)) {
        Print("Khong gui duoc lenh (hang doi day). Thu lai sau 1 giay.");
        return false;
    }
    Print("-> WROOM: %s", command.c_str());
    return true;
}

/* "cm01".."cm09" (hoac "1".."9") -> so menu. */
bool ParseMenuCode(const std::string& text, int* out) {
    if (out == nullptr) {
        return false;
    }
    if (text.size() == 1 && std::isdigit(static_cast<unsigned char>(text[0]))) {
        *out = text[0] - '0';
        return true;
    }
    if (text.size() == 4 && text[0] == 'c' && text[1] == 'm' &&
        std::isdigit(static_cast<unsigned char>(text[2])) &&
        std::isdigit(static_cast<unsigned char>(text[3]))) {
        *out = (text[2] - '0') * 10 + (text[3] - '0');
        return true;
    }
    return false;
}

/* Ma gui sang WROOM cho muc menu: 1 -> "cm01". */
std::string MenuCode(int menu) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "cm%02d", menu);
    return std::string(buffer);
}

/* Doc so nguyen o dau chuoi; tra ve phan con lai (da Trim) trong `rest`. */
bool TakeInt(const std::string& text, int* out, std::string* rest) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str()) {
        return false;
    }
    if (out != nullptr) {
        *out = static_cast<int>(value);
    }
    if (rest != nullptr) {
        *rest = Trim(std::string(end));
    }
    return true;
}

/* Thuc hien 1 muc menu va gui ma cmNN (kem tham so neu can) sang WROOM. */
void HandleMenuSelection(int menu, const std::string& code, const std::string& rest) {
    switch (menu) {
        case 0:
            PrintMusicMenu();
            return;

        case 1:     /* quet thiet bi */
        case 4:     /* liet ke file */
        case 5:     /* trang thai */
        case 6:     /* lap lai bai vua phat */
            SendToWroom(code);
            return;

        case 2: {   /* chon thiet bi, cho ket noi xong roi chon bai */
            if (rest.empty()) {
                Cm02WorkflowGuard workflow;
                BtLink::ClearCm02Events();
                if (!SendToWroom(code)) {
                    return;
                }

                Cm02Event event;
                if (!BtLink::WaitForCm02Event(&event, 30000)) {
                    Print("WROOM khong tra danh sach thiet bi trong thoi gian cho.");
                    return;
                }
                if (event != Cm02Event::DevicesReady) {
                    Print(event == Cm02Event::DevicesEmpty
                              ? "Chua co thiet bi; hay gui cm01 de quet Bluetooth."
                              : "Chua co danh sach thiet bi. Hay gui cm01 de quet lai.");
                    return;
                }

                int dev = 0;
                if (!PromptNumber("Chon so thiet bi (Enter = huy): ", 0, 99, &dev)) {
                    Print("Da huy.");
                    return;
                }

                BtLink::ClearCm02Events();
                if (!SendToWroom(code + " " + std::to_string(dev))) {
                    return;
                }
                if (!BtLink::WaitForCm02Event(&event, 60000)) {
                    Print("WROOM khong tra ket qua ket noi trong thoi gian cho.");
                    return;
                }
                if (event == Cm02Event::ConnectFailed) {
                    Print("Ket noi that bai; xem thong bao [WROOM] o tren.");
                    return;
                }
                if (event == Cm02Event::FilesEmpty) {
                    Print("Da ket noi nhung WROOM khong co bai MP3.");
                    return;
                }
                if (event != Cm02Event::FilesReady) {
                    Print("Phan hoi cm02 khong hop le.");
                    return;
                }

                int file = 0;
                if (!PromptNumber("Chon so bai MP3 (Enter = huy): ", 0, 999, &file)) {
                    Print("Da huy.");
                    return;
                }
                SendToWroom(code + " " + std::to_string(file));
                return;
            }

            int dev = 0;
            int file = -1;
            std::string left = rest;

            if (!TakeInt(left, &dev, &left)) {
                if (!PromptNumber("Thiet bi so (Enter = huy): ", 0, 99, &dev)) {
                    Print("Da huy.");
                    return;
                }
                left.clear();
            }
            if (!left.empty()) {
                TakeInt(left, &file, &left);
            }
            if (file < 0) {
                if (!PromptNumber("File MP3 so (Enter = huy): ", 0, 999, &file)) {
                    /* Chi ket noi, khong phat. */
                    SendToWroom(code + " " + std::to_string(dev));
                    return;
                }
            }
            SendToWroom(code + " " + std::to_string(dev) + " " + std::to_string(file));
            return;
        }

        case 3: {   /* 1 = dung phat, 2 = ngat ket noi */
            int choice = 1;
            if (!TakeInt(rest, &choice, nullptr)) {
                Print("1) Dung phat (giu ket noi A2DP)");
                Print("2) Ngat ket noi A2DP");
                if (!PromptNumber("Chon (Enter = huy): ", 1, 2, &choice)) {
                    Print("Da huy.");
                    return;
                }
            } else if (choice < 1 || choice > 2) {
                Print("Chon 1 (dung phat) hoac 2 (ngat ket noi).");
                return;
            }
            SendToWroom(code + " " + std::to_string(choice));
            return;
        }

        case 7: {   /* phat file khac, giu ket noi */
            int file = 0;
            if (!TakeInt(rest, &file, nullptr)) {
                if (!PromptNumber("File MP3 so (Enter = huy): ", 0, 999, &file)) {
                    Print("Da huy.");
                    return;
                }
            }
            SendToWroom(code + " " + std::to_string(file));
            return;
        }

        case 8: {   /* 1 = pause, 2 = resume */
            int choice = 1;
            if (!TakeInt(rest, &choice, nullptr)) {
                Print("1) Pause   2) Resume");
                if (!PromptNumber("Chon (Enter = huy): ", 1, 2, &choice)) {
                    Print("Da huy.");
                    return;
                }
            } else if (choice < 1 || choice > 2) {
                Print("Chon 1 (pause) hoac 2 (resume).");
                return;
            }
            SendToWroom(code + " " + std::to_string(choice));
            return;
        }

        case 9:
            PrintMusicHelp();
            return;

        default:
            Print("Ma menu '%s' khong hop le (cm00..cm09).", code.c_str());
            return;
    }
}

/* `args` la phan sau 'music'/'bt'/'speaker' (hoac ca dong khi go truc tiep).
 * Nhan ca lenh chu, ma menu "cm01".."cm09" va so thu tu menu 0..9. */
void HandleMusicCommand(const std::string& args_in) {
    const std::string args = Trim(args_in);
    const std::string sub = ToLowerCopy(args);

    if (args.empty() || sub == "menu") {
        PrintMusicMenu();
        return;
    }
    if (sub == "help" || sub == "?") {
        PrintMusicHelp();
        return;
    }

    /* Tach token dau: "cm02 1 0" -> head "cm02", rest "1 0". */
    std::string head = sub;
    std::string rest;
    const auto space = sub.find(' ');
    if (space != std::string::npos) {
        head = sub.substr(0, space);
        rest = Trim(sub.substr(space + 1));
    }

    /* `channel` is a local S3 setting: ESP-NOW needs both boards on the same
     * channel, and the S3 decides which one (the WROOM is pinned to it). */
    if (head == "channel" || head == "ch" || head == "info") {
        Print("ESP-NOW: %s, kenh S3 = %u",
              BtLink::IsReady() ? "san sang" : "chua san sang",
              static_cast<unsigned>(BtLink::Channel()));
        const int age = BtLink::LastHeardAgeSec();
        if (age < 0) {
            Print("WROOM: chua bao gio nghe thay (kiem tra nguon/firmware/kenh)");
        } else {
            Print("WROOM: lan cuoi nghe thay %d giay truoc", age);
        }
        if (rest.empty() || rest == "auto") {
            if (rest == "auto" && BtLink::SetChannel(0)) {
                Print("Kenh: tu dong (mac dinh kenh 1 khi S3 chua vao WiFi)");
            }
            Print("WROOM se duoc ghim cung kenh tu dong.");
            return;
        }
        int channel = 0;
        if (!TakeInt(rest, &channel, nullptr) || channel < 1 || channel > 13) {
            Print("usage: channel | channel <1..13> | channel auto");
            return;
        }
        if (BtLink::SetChannel(static_cast<uint8_t>(channel))) {
            Print("Kenh ESP-NOW = %d (WROOM se duoc ghim theo)", channel);
        } else {
            Print("Khong doi duoc kenh (S3 dang vao WiFi - kenh do router quyet dinh).");
        }
        return;
    }

    int menu = -1;
    if (ParseMenuCode(head, &menu)) {
        HandleMenuSelection(menu, MenuCode(menu), rest);
        return;
    }

    /* ---- Lenh chu: gui nguyen van cho WROOM ---- */
    SendToWroom(args);
}

/* Lenh go truc tiep (khong co tien to 'music'), giong console cua WROOM.
 * Tra ve true neu da xu ly. */
bool HandleBareMusicCommand(const std::string& cmd) {
    if (cmd.size() == 1 && cmd[0] >= '0' && cmd[0] <= '9') {
        HandleMusicCommand(cmd);
        return true;
    }
    /* Ma menu dung chung voi WROOM: cm01..cm09 (co the kem tham so). */
    if (StartsWith(cmd, "cm") && cmd.size() >= 4 &&
        std::isdigit(static_cast<unsigned char>(cmd[2])) &&
        std::isdigit(static_cast<unsigned char>(cmd[3])) &&
        (cmd.size() == 4 || cmd[4] == ' ')) {
        HandleMusicCommand(cmd);
        return true;
    }

    static const char* kExact[] = {
        "status", "list", "devices", "scan", "play", "pause", "resume",
        "continue", "next", "prev", "previous", "replay", "stop",
        "disconnect", "channel", "ch", "ping", "menu",
    };
    for (const char* command : kExact) {
        if (cmd == command) {
            HandleMusicCommand(cmd);
            return true;
        }
    }

    if (StartsWith(cmd, "connect ") || StartsWith(cmd, "play ") ||
        StartsWith(cmd, "channel ") || StartsWith(cmd, "ch ")) {
        HandleMusicCommand(cmd);
        return true;
    }
    return false;
}

void PrintMedia() {
    if (g_app == nullptr) {
        Print("application not ready");
        return;
    }
    Print("media: %s   (%s)", g_app->IsMediaEnabled() ? "ON" : "OFF",
          g_app->IsMediaEnabled() ? "voice: mic + speaker"
                                  : "text: UART only, mic + speaker off");
}

void SetMedia(bool enable) {
    if (g_app == nullptr) {
        Print("application not ready");
        return;
    }
    g_app->SetMediaEnabled(enable);
    if (enable) {
        Print("media ON  -> noi 'hi ESP' hoac nhan nut de noi chuyen");
    } else {
        Print("media OFF -> go 'AI: <cau hoi>' roi Enter de chat bang text");
    }
}

std::string Trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

/* Reboot after a short delay so the console line is flushed out first. */
void RebootSoon() {
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

/* Some serial tools are configured with "no line ending", so the Enter key
 * sends nothing and the line sits in the buffer forever - commands look like
 * they were "not received". After a pause we print a hint, and for a complete
 * argument-less command we submit it anyway so the console stays usable. */
constexpr int64_t LINE_HINT_AFTER_US = 2500 * 1000;
constexpr int64_t LINE_AUTO_SUBMIT_US = 3000 * 1000;

std::string ToLowerCopy(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool IsCompleteCommand(const std::string& line) {
    const std::string cmd = ToLowerCopy(Trim(line));
    static const char* kCommands[] = {
        "help", "h", "?",       "state",     "s",           "media",       "m",
        "device", "info", "id", "mac",
        "wifi", "w", "wifi status", "wifi scan", "wifi ap", "wifi clear", "wifi reset",
        "music", "music menu", "music help", "music status", "music list",
        "music devices", "music scan", "music play", "music pause",
        "music resume", "music next", "music prev", "music replay",
        "music stop", "music disconnect", "music channel",
        "music channel auto", "music ping",
        "music 0", "music 1", "music 2", "music 3", "music 4",
        "music 5", "music 6", "music 7", "music 8", "music 9",
        "bt", "speaker",
        "menu", "status", "list", "devices", "scan", "play", "pause",
        "resume", "continue", "next", "prev", "previous", "replay",
        "stop", "disconnect", "channel", "ch", "ping",
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
        "cm00", "cm01", "cm02", "cm03", "cm04",
        "cm05", "cm06", "cm07", "cm08", "cm09",
    };
    for (const char* command : kCommands) {
        if (cmd == command) {
            return true;
        }
    }

    /* `cm02 1 0`, `cm03 1`, `cm08 2`, `channel 6` ... have arguments, but they
     * are still complete commands: submit them without waiting for Enter. */
    if (cmd.size() > 4 && cmd[4] == ' ' && StartsWith(cmd, "cm") &&
        std::isdigit(static_cast<unsigned char>(cmd[2])) &&
        std::isdigit(static_cast<unsigned char>(cmd[3]))) {
        return true;
    }
    if (StartsWith(cmd, "channel ") || StartsWith(cmd, "ch ")) {
        return true;
    }
    return false;
}

void PrintWifiStatus() {
    auto& station = WifiStation::GetInstance();
    auto& manager = SsidManager::GetInstance();
    const auto& list = manager.GetSsidList();

    if (station.IsConnected()) {
        Print("wifi: CONNECTED");
        Print("  ssid: %s", station.GetSsid().c_str());
        Print("  ip  : %s", station.GetIpAddress().c_str());
        Print("  rssi: %d dBm", static_cast<int>(station.GetRssi()));
    } else {
        Print("wifi: NOT connected");
    }

    Print("  da luu %d mang:", static_cast<int>(list.size()));
    for (size_t i = 0; i < list.size(); i++) {
        Print("   [%d] %s", static_cast<int>(i), list[i].ssid.c_str());
    }
    if (list.empty()) {
        Print("   (chua co) -> go: wifi <ssid> <password>");
    }
}

void PrintDeviceInfo() {
    auto& board = Board::GetInstance();
    auto& station = WifiStation::GetInstance();

    Print("---- thong tin thiet bi ----");
    Print("  board      : %s", board.GetBoardType().c_str());
    Print("  firmware   : %s", SystemInfo::GetUserAgent().c_str());
    Print("  chip       : %s", SystemInfo::GetChipModelName().c_str());
    Print("  Device-Id  : %s", SystemInfo::GetMacAddress().c_str());
    Print("  Client-Id  : %s", board.GetUuid().c_str());
    if (station.IsConnected()) {
        Print("  wifi       : %s  ip %s", station.GetSsid().c_str(),
              station.GetIpAddress().c_str());
    } else {
        Print("  wifi       : not connected");
    }
    if (g_app != nullptr) {
        const std::string code = g_app->GetActivationCode();
        if (!code.empty()) {
            Print("  ma kich hoat: %s  (nhap tai xiaozhi.me)", code.c_str());
        }
    }
    Print("  (Device-Id = dia chi MAC WiFi, Client-Id = UUID luu trong NVS)");
}

void PrintWifiScan() {    wifi_scan_config_t config = {};
    config.show_hidden = true;  // SSID an (hidden) cung phai thay

    if (WifiStation::GetInstance().IsConnected()) {
        Print("dang ket noi - scan se lam rot ket noi tam thoi...");
    }

    esp_err_t err = esp_wifi_scan_start(&config, true);
    if (err != ESP_OK) {
        Print("scan loi: %s (wifi chua san sang, thu lai sau vai giay)", esp_err_to_name(err));
        return;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) {
        Print("khong thay AP nao");
        return;
    }

    auto* records = static_cast<wifi_ap_record_t*>(malloc(sizeof(wifi_ap_record_t) * count));
    if (records == nullptr) {
        Print("het bo nho de luu ket qua scan");
        return;
    }
    esp_wifi_scan_get_ap_records(&count, records);

    Print("tim thay %d AP:", static_cast<int>(count));
    for (uint16_t i = 0; i < count; i++) {
        Print("  %-32s ch=%2d rssi=%4d auth=%d", reinterpret_cast<const char*>(records[i].ssid),
              records[i].primary, records[i].rssi, static_cast<int>(records[i].authmode));
    }
    free(records);
}

void SaveWifiAndReboot(const std::string& ssid, const std::string& password) {
    if (ssid.empty() || ssid.size() > 32) {
        Print("ssid khong hop le (1..32 ky tu)");
        return;
    }
    if (password.size() > 64) {
        Print("password qua dai (toi da 64 ky tu)");
        return;
    }
    if (password.empty()) {
        Print("canh bao: password rong -> se thu ket noi mang mo (open)");
    }

    SsidManager::GetInstance().AddSsid(ssid, password);
    Print("da luu WiFi '%s' vao NVS", ssid.c_str());
    Print("khoi dong lai de ket noi...");
    RebootSoon();
}

void EnterWifiConfigModeFromConsole() {
    Settings settings("wifi", true);
    settings.SetInt("force_ap", 1);
    Print("khoi dong lai vao che do AP cau hinh (TienHuyIoT-XXXX)");
    RebootSoon();
}

void ClearSavedWifi() {
    SsidManager::GetInstance().Clear();
    Print("da xoa danh sach WiFi da luu, khoi dong lai...");
    RebootSoon();
}

/* wifi | wifi status | wifi scan | wifi <ssid> <password> | wifi ap | wifi clear */
void HandleWifiCommand(const std::string& command, const std::string& raw_line) {
    if (command == "wifi" || command == "w" || command == "wifi status") {
        PrintWifiStatus();
        return;
    }
    if (command == "wifi scan" || command == "wifi s") {
        PrintWifiScan();
        return;
    }
    if (command == "wifi ap") {
        EnterWifiConfigModeFromConsole();
        return;
    }
    if (command == "wifi clear" || command == "wifi reset") {
        ClearSavedWifi();
        return;
    }

    // wifi <ssid> <password>  (lay tu dong goc de giu nguyen chu hoa/thuong)
    const auto space = raw_line.find(' ');
    if (space == std::string::npos) {
        Print("usage: wifi <ssid> <password>   |   wifi   |   wifi scan   |   wifi ap");
        return;
    }
    const std::string args = Trim(raw_line.substr(space + 1));
    const auto split = args.find(' ');
    const std::string ssid = Trim(split == std::string::npos ? args : args.substr(0, split));
    const std::string password = split == std::string::npos ? "" : Trim(args.substr(split + 1));
    SaveWifiAndReboot(ssid, password);
}

bool StartsWith(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

void HandleLine(const std::string& raw_line) {
    const std::string line = Trim(raw_line);
    std::string cmd;
    cmd.reserve(line.size());
    for (char c : line) {
        cmd.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (cmd == "help" || cmd == "h" || cmd == "?") {
        PrintHelp();
        return;
    }

    if (cmd == "state" || cmd == "s") {
        if (g_app != nullptr) {
            Print("state: %s", StateName(g_app->GetDeviceState()));
        }
        return;
    }

    if (cmd == "device" || cmd == "info" || cmd == "id" || cmd == "mac") {
        PrintDeviceInfo();
        return;
    }

    if (cmd == "media" || cmd == "m") {
        PrintMedia();
        return;
    }

    if (StartsWith(cmd, "media ") || StartsWith(cmd, "m ")) {
        std::string arg = cmd.substr(cmd.find(' ') + 1);
        // Trim surrounding spaces
        const auto first = arg.find_first_not_of(" \t");
        const auto last = arg.find_last_not_of(" \t");
        arg = (first == std::string::npos) ? "" : arg.substr(first, last - first + 1);

        if (arg == "on" || arg == "1" || arg == "enable" || arg == "voice") {
            SetMedia(true);
        } else if (arg == "off" || arg == "0" || arg == "disable" || arg == "text") {
            SetMedia(false);
        } else if (arg == "toggle") {
            if (g_app != nullptr) {
                SetMedia(!g_app->IsMediaEnabled());
            }
        } else {
            Print("usage: media on | media off | media toggle | media");
        }
        return;
    }

    // WiFi commands must work in both media modes (they are not AI questions).
    if (cmd == "w" || cmd == "wifi" || StartsWith(cmd, "wifi ")) {
        HandleWifiCommand(cmd, line);
        return;
    }

    // Music / Bluetooth speaker control: forwarded to the ESP32-WROOM over
    // ESP-NOW.  Same command names and numeric menu as the WROOM console.
    // Must be checked before the AI-text fallback.
    if (cmd == "music" || cmd == "bt" || cmd == "speaker") {
        PrintMusicMenu();
        return;
    }
    if (StartsWith(cmd, "music ") || StartsWith(cmd, "bt ") ||
        StartsWith(cmd, "speaker ")) {
        HandleMusicCommand(line.substr(line.find(' ') + 1));
        return;
    }
    if (HandleBareMusicCommand(cmd)) {
        return;
    }

    if (StartsWith(cmd, "ai:")) {
        const std::string question = Trim(line.substr(3));
        if (question.empty()) {
            Print("usage: AI: <cau hoi>");
            return;
        }
        if (g_app == nullptr) {
            Print("application not ready");
            return;
        }
        if (g_app->IsMediaEnabled()) {
            Print("media dang ON - hay noi voi thiet bi, hoac go 'media off' de chat bang text");
            return;
        }

        Print("> %s", question.c_str());
        g_app->SendTextQuery(question);
        return;
    }

    Print("Khong nhan dang lenh. Cau hoi AI phai bat dau bang 'AI:'; go 'help' de xem lenh.");
}

/* Skip an ANSI escape sequence (arrow keys, function keys, ...). */
void SkipEscapeSequence() {
    uint8_t ch = 0;
    if (uart_read_bytes(CONSOLE_UART_NUM, &ch, 1, pdMS_TO_TICKS(20)) <= 0) {
        return;
    }
    if (ch != '[' && ch != 'O') {
        return;
    }
    while (uart_read_bytes(CONSOLE_UART_NUM, &ch, 1, pdMS_TO_TICKS(20)) > 0) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '~') {
            return;
        }
    }
}

void ConsoleTask(void* /*arg*/) {
    std::string line;
    uint8_t ch = 0;
    int64_t last_rx_us = 0;
    bool enter_hint_shown = false;

    PrintMusicMenu();

    while (true) {
        int read = uart_read_bytes(CONSOLE_UART_NUM, &ch, 1, pdMS_TO_TICKS(100));
        if (read <= 0) {
            // Nothing arrived: the terminal may never send CR/LF on Enter.
            if (!line.empty()) {
                const int64_t idle_us = esp_timer_get_time() - last_rx_us;
                if (idle_us > LINE_AUTO_SUBMIT_US && IsCompleteCommand(line)) {
                    ConsoleWrite("\r\n", 2);
                    Print("(khong nhan duoc Enter -> tu gui lenh sau %d giay)",
                          (int)(LINE_AUTO_SUBMIT_US / 1000000));
                    if (!RxFloodDetected()) {
                        HandleLine(line);
                    }
                    line.clear();
                    enter_hint_shown = false;
                } else if (idle_us > LINE_HINT_AFTER_US && !enter_hint_shown) {
                    Print("(chua nhan duoc Enter: dat terminal gui CR+LF, hoac bam Ctrl+J)");
                    enter_hint_shown = true;
                }
            }
            continue;
        }
        last_rx_us = esp_timer_get_time();

        if (RxIsMuted()) {
            // Drop the byte *and* do not echo it: echoing would keep feeding
            // a TX->RX loopback (the classic "console repeats itself" flood).
            line.clear();
            continue;
        }

        if (ch == 0x1B) {
            SkipEscapeSequence();
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            ConsoleWrite("\r\n", 2);
            if (!line.empty()) {
                if (!RxFloodDetected()) {
                    HandleLine(line);
                }
                line.clear();
            }
            continue;
        }

        if (ch == 0x08 || ch == 0x7F) {  // backspace / delete
            if (!line.empty()) {
                line.pop_back();
                ConsoleWrite("\b \b", 3);
            }
            continue;
        }

        // Many people type the two characters "\n" / "\r" instead of pressing
        // Enter (some terminals send nothing at all for Enter). Accept that as
        // a line terminator so commands still work.
        if (ch == '\\') {
            uint8_t next = 0;
            if (uart_read_bytes(CONSOLE_UART_NUM, &next, 1, pdMS_TO_TICKS(60)) > 0) {
                last_rx_us = esp_timer_get_time();
                if (next == 'n' || next == 'r') {
                    ConsoleWrite("\r\n", 2);
                    if (!line.empty()) {
                        if (!RxFloodDetected()) {
                            HandleLine(line);
                        }
                        line.clear();
                    }
                    enter_hint_shown = false;
                    continue;
                }
                // Not a terminator: keep both characters as typed.
                if (line.size() + 2 <= CONSOLE_MAX_LINE) {
                    line.push_back('\\');
                    line.push_back(static_cast<char>(next));
                    ConsoleWrite("\\", 1);
                    ConsoleWrite(reinterpret_cast<const char*>(&next), 1);
                    enter_hint_shown = false;
                }
                continue;
            }
        }

        // Control bytes are noise: never echo them (echoing noise is what
        // turns a dirty RX line into an endless loop).
        if (ch < 0x20) {
            continue;
        }

        if (line.size() < CONSOLE_MAX_LINE) {
            line.push_back(static_cast<char>(ch));
            ConsoleWrite(reinterpret_cast<const char*>(&ch), 1);
            enter_hint_shown = false;
        }
    }
}

}  // namespace

void UartConsole::PrintAiReply(const std::string& text) {
    Print("AI: %s", text.c_str());
}

void UartConsole::Notify(const std::string& text) {
    Print("%s", text.c_str());
}

void UartConsole::Start(Application* app) {
    g_app = app;

    uart_config_t config = {};
    config.baud_rate = CONSOLE_UART_BAUD_RATE;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(CONSOLE_UART_NUM, CONSOLE_RX_BUFFER, 0, 0, nullptr, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install(UART%d) failed: %s", (int)CONSOLE_UART_NUM,
                 esp_err_to_name(err));
        g_app = nullptr;
        return;
    }
    ESP_ERROR_CHECK(uart_param_config(CONSOLE_UART_NUM, &config));
    ESP_ERROR_CHECK(uart_set_pin(CONSOLE_UART_NUM, CONSOLE_UART_TX_PIN, CONSOLE_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // A floating RX pin picks up noise (and even crosstalk from our own TX
    // wire), which the console would read as endless garbage typing.
    // The USB-TTL adapter drives the line when connected, so the pull-up is
    // harmless there but keeps the idle state clean when nothing is wired.
    if (CONSOLE_UART_RX_PIN != GPIO_NUM_NC) {
        gpio_set_pull_mode(CONSOLE_UART_RX_PIN, GPIO_PULLUP_ONLY);
    }

    uart_flush_input(CONSOLE_UART_NUM);

    /* ESP-NOW link to the WROOM music player.  It waits for WiFi to start, so
     * calling it here (before board.StartNetwork()) is safe and lets the
     * channel handshake finish while the device is still booting. */
    BtLink::Start();

    if (xTaskCreate(ConsoleTask, "uart_console", 4096, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "failed to create console task");
        g_app = nullptr;
        return;
    }

    Print("");
    Print("================================");
    Print(" Xiaozhi text console ready");
    Print(" UART%d @ %d 8N1, TX=GPIO%d, RX=GPIO%d", (int)CONSOLE_UART_NUM,
          (int)CONSOLE_UART_BAUD_RATE, (int)CONSOLE_UART_TX_PIN, (int)CONSOLE_UART_RX_PIN);
    Print(" media: %s", app->IsMediaEnabled() ? "ON (voice)" : "OFF (UART text)");
    Print(" wifi:  go 'wifi <ssid> <password>' de luu WiFi, 'wifi' de xem trang thai");
    Print(" go 'help' de xem lenh, 'media off' de chat bang text");
    Print("================================");

    if (!app->IsMediaEnabled()) {
        Print("media OFF -> go 'AI: <cau hoi>' roi Enter de chat bang text");
    }
}
