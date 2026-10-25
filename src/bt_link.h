#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#define BT_LINK_AP_SSID       "AI_BOT"
#define BT_LINK_AP_PASSWORD   "123456789"
#define BT_LINK_TCP_PORT      3333
#define BT_LINK_MAGIC         0xB7
#define BT_LINK_VERSION       2
#define BT_LINK_TEXT_LEN      192
#define BT_LINK_PCM_CHUNK_LEN 512
#define BT_LINK_AP_IP         "192.168.4.1"

typedef enum {
    BT_LINK_MSG_COMMAND = 1,
    BT_LINK_MSG_STATUS = 2,
    BT_LINK_MSG_PCM_START = 3,
    BT_LINK_MSG_PCM_DATA = 4,
    BT_LINK_MSG_PCM_END = 5,
    BT_LINK_MSG_PCM_STOP = 6,
} bt_link_msg_type_t;

typedef struct __attribute__((packed)) {
    uint8_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t seq;
    uint16_t length;
    union {
        char text[BT_LINK_TEXT_LEN];
        uint8_t pcm[BT_LINK_PCM_CHUNK_LEN];
    } data;
} bt_link_packet_t;

static_assert(sizeof(bt_link_packet_t) == 6 + BT_LINK_PCM_CHUNK_LEN,
              "S3/WROOM TCP packet layout must remain synchronized");

enum class Cm02Event : uint8_t {
    DevicesReady = 1,
    FilesReady,
    ConnectFailed,
    FilesEmpty,
    DevicesEmpty,
    PlaybackReady,
    StreamReady,
};

class BtLink {
public:
    static void Start();
    static bool SendCommand(const std::string& command);
    static bool SendPcmPacket(uint8_t type, const void* data, size_t length);
    static void ClearCm02Events();
    static bool WaitForCm02Event(Cm02Event* event, uint32_t timeout_ms);
    static void SetCm02WorkflowActive(bool active);
    static bool IsReady();
    static bool HeardWroom();
};
