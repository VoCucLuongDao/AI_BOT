#ifndef UART_CONSOLE_H
#define UART_CONSOLE_H

#include <driver/gpio.h>
#include <driver/uart.h>

#include <string>

/*
 * Dedicated text-chat console on its own UART, wired out to free GPIO pins.
 *
 * It does NOT use UART0 (the ESP-IDF log console), so `pio device monitor`
 * keeps showing logs while a USB-TTL adapter on the pins below carries the
 * typed conversation.
 *
 * Default wiring for xiaozhi-ai-iot-vietnam-es3n28p-lcd-2.8 (ESP32-S3 N16R8):
 *   UART1 TX = GPIO21  -> USB-TTL RX
 *   UART1 RX = GPIO14  <- USB-TTL TX
 *   GND      = GND     --  USB-TTL GND   (3.3 V level, do NOT use 5 V)
 *
 * GPIO21 and GPIO14 are used here for UART1 (I2S 4-8, LCD 10-13/45/46, I2C 15/16,
 * touch 17/18, SD 38-41/47/48, LED 42, boot 0, PA 1, flash/PSRAM 26-37).
 * Override with -DCONSOLE_UART_* if another board needs different pins.
 */

#ifndef CONSOLE_UART_NUM
#define CONSOLE_UART_NUM        UART_NUM_1
#endif
#ifndef CONSOLE_UART_TX_PIN
#define CONSOLE_UART_TX_PIN     GPIO_NUM_21
#endif
#ifndef CONSOLE_UART_RX_PIN
#define CONSOLE_UART_RX_PIN     GPIO_NUM_14
#endif
#ifndef CONSOLE_UART_BAUD_RATE
#define CONSOLE_UART_BAUD_RATE  115200
#endif

class Application;

/*
 * Commands
 *   media             show the media mode (voice or UART text)
 *   media on|off      enable / disable the microphone + speaker
 *   media toggle      flip the mode
 *   state             print the current device state
 *   wifi              show WiFi state and the saved SSID list
 *   wifi <ssid> <pw>  save credentials to NVS and reboot to connect
 *   wifi scan         list nearby APs (SSID / channel / RSSI)
 *   wifi ap           reboot into the config AP (TienHuyIoT-XXXX)
 *   wifi clear        erase every saved SSID and reboot
 *   help              list commands
 *   AI: <question>    sent to the AI as the user's utterance (only when
 *                     media is OFF; when media is ON you just talk)
 *   cm00..cm09        Bluetooth speaker test/control commands
 */
class UartConsole {
public:
    static void Start(Application* app);

    /* Print an AI answer on the console UART (used when media is disabled so
     * the user can still read the reply even though the speaker is off). */
    static void PrintAiReply(const std::string& text);

    /* Short status/notice shown on the console UART (and in the ESP_LOG). */
    static void Notify(const std::string& text);
};

#endif // UART_CONSOLE_H
