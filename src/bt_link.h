#pragma once

/*
 * ESP-NOW link between this ESP32-S3 (AI_BOT) and the ESP32-WROOM music
 * player (AI_BOT_speaker_transfer).
 *
 * Flow: UART console -> `music <command>` -> ESP-NOW -> WROOM -> Bluetooth A2DP.
 * The WROOM answers every command with one or more STATUS packets; they are
 * printed on the same UART console with a `[WROOM]` prefix.
 *
 * The packet layout below (magic, version, enum values and struct fields) is
 * shared by both projects.  Keep AI_BOT/src/bt_link.h and
 * AI_BOT_speaker_transfer/src/bt_link.h in sync: they may differ in the API
 * they declare (C++ class vs C functions) but never in the wire format.
 */

#include <stdbool.h>
#include <stdint.h>

#include <string>

#define BT_LINK_MAGIC       0xB7
#define BT_LINK_VERSION     1
#define BT_LINK_TEXT_LEN    192

enum class Cm02Event : uint8_t {
    DevicesReady = 1,
    FilesReady,
    ConnectFailed,
    FilesEmpty,
    DevicesEmpty,
};

/* Broadcast address: the S3 does not have to know the WROOM MAC in advance.
 * The WROOM answers unicast to the S3 MAC found in the received packet. */
#define BT_LINK_BROADCAST_ADDR { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }

typedef enum {
    BT_LINK_MSG_COMMAND = 1,    /* S3 -> WROOM: "play 1", "pause", ... */
    BT_LINK_MSG_STATUS  = 2,    /* WROOM -> S3: one line of output */
    BT_LINK_MSG_HELLO   = 3,    /* S3 -> WROOM: "which channel are you on?" */
} bt_link_msg_type_t;

typedef struct __attribute__((packed)) {
    uint8_t  magic;                 /* BT_LINK_MAGIC */
    uint8_t  version;               /* BT_LINK_VERSION */
    uint8_t  type;                  /* bt_link_msg_type_t */
    uint8_t  seq;                   /* command sequence, echoed in STATUS */
    uint16_t reserved;
    char     text[BT_LINK_TEXT_LEN];
} bt_link_packet_t;

class BtLink {
public:
    /* Create the ESP-NOW task.  Safe to call more than once.  The task waits
     * until WiFi is started by WifiStation. */
    static void Start();

    /* Queue an ASCII command for the WROOM.  Returns false if the queue is
     * full (e.g. the WROOM is not reachable yet). */
    static bool SendCommand(const std::string& command);

    /* Wait for private cm02 workflow events without displaying them as status. */
    static void ClearCm02Events();
    static bool WaitForCm02Event(Cm02Event* event, uint32_t timeout_ms);
    static void SetCm02WorkflowActive(bool active);

    /* True once esp_now_init() succeeded. */
    static bool IsReady();

    /* Last WiFi channel used for ESP-NOW (0 until ready). */
    static uint8_t Channel();

    /* Force the ESP-NOW channel while the S3 is not associated with an AP
     * (0 = back to the default fixed channel).  Returns false when the router
     * owns the channel or the radio refused the change. */
    static bool SetChannel(uint8_t channel);

    /* True once at least one STATUS packet came back from the WROOM. */
    static bool HeardWroom();

    /* Seconds since the last packet from the WROOM, -1 if never heard. */
    static int LastHeardAgeSec();
};
