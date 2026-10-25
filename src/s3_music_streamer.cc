#include "s3_music_streamer.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <dirent.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_spiffs.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "bt_link.h"
#include "esp_audio_dec.h"
#include "esp_mp3_dec.h"
#include "uart_console.h"

namespace {

constexpr char TAG[] = "S3MusicStream";
constexpr uint32_t kOutputSampleRate = 44100;
constexpr size_t kDecoderOutputBytes = 8192;
constexpr size_t kMaxDecoderOutputBytes = 64 * 1024;
/* esp_mp3_dec_decode khong duoc thiet ke cho input lon: test app cua Espressif
 * feed theo chunk 10 KB. Feed ca file (hang tram KB) khien decoder ghi tran
 * buffer noi bo -> CORRUPT HEAP. */
constexpr uint32_t kDecoderInputChunk = 10 * 1024;
constexpr size_t kOutputSamples = 8192;
constexpr uint32_t kTaskStackBytes = 16 * 1024;
constexpr char kMusicMountPath[] = "/music";
constexpr char kMusicPartitionLabel[] = "music";
/* PCM stereo 44.1 kHz / 16-bit: 176400 byte/giay. Lead 500 ms: S3 chay truoc
 * du de lap day ring buffer 8 KB phia WROOM; phan duoc TCP backpressure giu
 * lai, send() block cho den khi WROOM tieu thu (SO_SNDTIMEO=10s). */
constexpr double kPcmBytesPerSecond = 44100.0 * 2 * 2;
constexpr int64_t kPcmLeadUs = 500 * 1000;

char s_track_name[32];
TaskHandle_t s_task;
volatile bool s_stop_requested;
volatile bool s_active;
volatile bool s_paused;
bool s_filesystem_mounted;
int64_t s_stream_start_us;
uint64_t s_stream_bytes_sent;

bool EnsureFilesystemMounted() {
    if (s_filesystem_mounted) {
        return true;
    }

    esp_vfs_spiffs_conf_t config = {
        .base_path = kMusicMountPath,
        .partition_label = kMusicPartitionLabel,
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    const esp_err_t err = esp_vfs_spiffs_register(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot mount SPIFFS partition '%s': %s",
                 kMusicPartitionLabel, esp_err_to_name(err));
        return false;
    }
    s_filesystem_mounted = true;
    ESP_LOGI(TAG, "Mounted music SPIFFS partition at %s", kMusicMountPath);
    return true;
}

bool HasMp3Extension(const char* name) {
    const size_t length = std::strlen(name);
    if (length < 4) {
        return false;
    }
    return name[length - 4] == '.' &&
           std::tolower(static_cast<unsigned char>(name[length - 3])) == 'm' &&
           std::tolower(static_cast<unsigned char>(name[length - 2])) == 'p' &&
           std::tolower(static_cast<unsigned char>(name[length - 1])) == '3';
}

void WaitWhilePaused() {
    while (s_paused && !s_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

struct ResamplerState {
    double position = 0;
    int16_t previous[2] = {0, 0};
    uint32_t sample_rate = 0;
};

int16_t SourceSample(const int16_t* samples, int index, int channel, int channels) {
    const int source_channel = channels == 1 ? 0 : channel;
    return samples[index * channels + source_channel];
}

size_t ResampleToStereo(const int16_t* input, size_t input_samples, int channels,
                        uint32_t sample_rate, int16_t* output,
                        size_t output_capacity, ResamplerState* state) {
    if (input_samples == 0 || (channels != 1 && channels != 2) ||
        sample_rate == 0 || state == nullptr) {
        return 0;
    }
    if (state->sample_rate != sample_rate) {
        state->position = 0;
        state->previous[0] = 0;
        state->previous[1] = 0;
        state->sample_rate = sample_rate;
    }

    const double step = static_cast<double>(sample_rate) / kOutputSampleRate;
    size_t produced = 0;
    while (state->position < static_cast<double>(input_samples) &&
           produced < output_capacity) {
        const int index = static_cast<int>(state->position);
        const double fraction = state->position - index;
        for (int channel = 0; channel < 2; ++channel) {
            const int16_t a = index == 0
                                  ? state->previous[channel]
                                  : SourceSample(input, index - 1, channel, channels);
            const int16_t b = SourceSample(input, index, channel, channels);
            output[produced * 2 + channel] =
                static_cast<int16_t>(a + (b - a) * fraction);
        }
        ++produced;
        state->position += step;
    }

    state->position -= static_cast<double>(input_samples);
    if (state->position < 0) {
        state->position = 0;
    }
    state->previous[0] = SourceSample(input, static_cast<int>(input_samples) - 1, 0, channels);
    state->previous[1] = SourceSample(input, static_cast<int>(input_samples) - 1, 1, channels);
    return produced;
}

bool SendPcm(const int16_t* pcm, size_t length) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(pcm);
    size_t sent = 0;
    while (sent < length && !s_stop_requested) {
        WaitWhilePaused();
        if (s_stop_requested) {
            break;
        }
        const size_t chunk = std::min(length - sent, static_cast<size_t>(BT_LINK_PCM_CHUNK_LEN));
        if (!BtLink::SendPcmPacket(BT_LINK_MSG_PCM_DATA, bytes + sent, chunk)) {
            return false;
        }
        sent += chunk;
        s_stream_bytes_sent += chunk;

        /* Pace theo toc do phat that: WROOM chi tieu thu PCM o toc do A2DP,
         * gui nhanh hon se lam day ring buffer va nghet ca duong TCP/heartbeat.
         * Cho phep di truoc realtime toi da kPcmLeadUs (burst ban dau). */
        const int64_t ideal_us = static_cast<int64_t>(
            s_stream_bytes_sent * 1000000.0 / kPcmBytesPerSecond);
        const int64_t elapsed_us = esp_timer_get_time() - s_stream_start_us;
        const int64_t delay_us = ideal_us - elapsed_us - kPcmLeadUs;
        if (delay_us > 0) {
            vTaskDelay(pdMS_TO_TICKS(static_cast<TickType_t>((delay_us + 999) / 1000)));
        }
    }
    return sent == length;
}

size_t SkipId3v2(const uint8_t* data, size_t size) {
    if (size < 10 || std::memcmp(data, "ID3", 3) != 0) {
        return 0;
    }
    const size_t tag_size = (static_cast<size_t>(data[6] & 0x7f) << 21) |
                            (static_cast<size_t>(data[7] & 0x7f) << 14) |
                            (static_cast<size_t>(data[8] & 0x7f) << 7) |
                            static_cast<size_t>(data[9] & 0x7f);
    const size_t total = 10 + tag_size + ((data[5] & 0x10) ? 10 : 0);
    return total <= size ? total : size;
}

void RunMusicStream() {
    if (!EnsureFilesystemMounted()) {
        UartConsole::Notify("[MP3] Khong mount duoc SPIFFS nhac tren S3.");
        return;
    }

    char track_path[64];
    const int path_length = std::snprintf(track_path, sizeof(track_path), "%s/%s",
                                          kMusicMountPath, s_track_name);
    if (path_length < 0 || static_cast<size_t>(path_length) >= sizeof(track_path)) {
        ESP_LOGE(TAG, "Invalid track path for '%s'", s_track_name);
        UartConsole::Notify("[MP3] Duong dan file SPIFFS khong hop le.");
        return;
    }
    FILE* file = std::fopen(track_path, "rb");
    if (file == nullptr || std::fseek(file, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "Cannot open S3 SPIFFS track '%s'", track_path);
        UartConsole::Notify("[MP3] Khong mo duoc file trong SPIFFS S3.");
        if (file != nullptr) {
            std::fclose(file);
        }
        return;
    }
    const long file_size = std::ftell(file);
    if (file_size <= 0 || static_cast<unsigned long>(file_size) > UINT32_MAX ||
        std::fseek(file, 0, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Invalid S3 SPIFFS file size for '%s'", track_path);
        UartConsole::Notify("[MP3] Kich thuoc file SPIFFS khong hop le.");
        std::fclose(file);
        return;
    }
    auto* input_data = static_cast<uint8_t*>(
        heap_caps_malloc(static_cast<size_t>(file_size),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (input_data == nullptr) {
        ESP_LOGE(TAG, "Not enough PSRAM for %ld-byte MP3 file", file_size);
        UartConsole::Notify("[MP3] Khong du PSRAM de doc file MP3.");
        std::fclose(file);
        return;
    }
    const size_t bytes_read = std::fread(input_data, 1, static_cast<size_t>(file_size), file);
    std::fclose(file);
    if (bytes_read != static_cast<size_t>(file_size)) {
        ESP_LOGE(TAG, "Short read on '%s': %u/%ld bytes", track_path,
                 static_cast<unsigned>(bytes_read), file_size);
        UartConsole::Notify("[MP3] Doc file SPIFFS khong day du.");
        heap_caps_free(input_data);
        return;
    }

    size_t asset_size = bytes_read;
    const size_t id3_bytes = SkipId3v2(input_data, asset_size);
    input_data += id3_bytes;
    asset_size -= id3_bytes;

    {
        void* decoder = nullptr;
        esp_audio_err_t ret = esp_mp3_dec_open(nullptr, 0, &decoder);
        if (ret != ESP_AUDIO_ERR_OK || decoder == nullptr) {
            ESP_LOGE(TAG, "MP3 decoder open failed: %d", static_cast<int>(ret));
            UartConsole::Notify("[MP3] Khong mo duoc bo giai ma MP3 tren S3.");
            heap_caps_free(input_data - id3_bytes);
            return;
        }

        auto* decoded = static_cast<uint8_t*>(
            heap_caps_malloc(kDecoderOutputBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        auto* output = static_cast<int16_t*>(
            heap_caps_malloc(kOutputSamples * 2 * sizeof(int16_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        size_t decoded_capacity = kDecoderOutputBytes;
        if (decoded == nullptr || output == nullptr) {
            ESP_LOGE(TAG, "Not enough PSRAM for MP3 stream buffers");
            UartConsole::Notify("[MP3] Khong du PSRAM de phat file.");
            heap_caps_free(decoded);
            heap_caps_free(output);
            esp_mp3_dec_close(decoder);
            heap_caps_free(input_data - id3_bytes);
            return;
        }

        if (!BtLink::SendPcmPacket(BT_LINK_MSG_PCM_START, nullptr, 0)) {
            ESP_LOGE(TAG, "Cannot start PCM stream: WROOM TCP link unavailable");
            UartConsole::Notify("[MP3] Khong gui duoc lenh bat dau stream toi WROOM.");
            heap_caps_free(decoded);
            heap_caps_free(output);
            esp_mp3_dec_close(decoder);
            heap_caps_free(input_data - id3_bytes);
            return;
        }
        s_stream_start_us = esp_timer_get_time();
        s_stream_bytes_sent = 0;

        UartConsole::Notify(std::string("[MP3] Dang giai ma va stream tu SPIFFS: ") +
                            s_track_name);
        esp_audio_dec_in_raw_t raw = {
            .buffer = input_data,
            .len = 0,
        };
        ResamplerState resampler;
        uint32_t decoded_frames = 0;
        bool failed = false;

        for (;;) {
            const size_t pos = static_cast<size_t>(raw.buffer - input_data);
            const size_t left = asset_size - pos;
            if (left == 0 || s_stop_requested) {
                break;
            }
            WaitWhilePaused();
            if (s_stop_requested) {
                break;
            }
            /* Feed decoder theo chunk nho: raw.buffer tro vao file lien tuc,
             * decoder chi nhin thay toi da kDecoderInputChunk byte ke tiep. */
            raw.len = static_cast<uint32_t>(
                std::min(left, static_cast<size_t>(kDecoderInputChunk)));
            raw.consumed = 0;
            esp_audio_dec_out_frame_t frame = {
                .buffer = decoded,
                .len = static_cast<uint32_t>(decoded_capacity),
            };
            esp_audio_dec_info_t info = {};
            const uint32_t previous_len = raw.len;
            ret = esp_mp3_dec_decode(decoder, &raw, &frame, &info);

            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH &&
                frame.needed_size > frame.len) {
                if (frame.needed_size > kMaxDecoderOutputBytes) {
                    ESP_LOGE(TAG, "MP3 decoder requested an unsupported output size: %lu",
                             static_cast<unsigned long>(frame.needed_size));
                    failed = true;
                    break;
                }
                uint8_t* larger = static_cast<uint8_t*>(
                    heap_caps_realloc(decoded, frame.needed_size,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (larger == nullptr) {
                    ESP_LOGE(TAG, "Cannot grow MP3 decoder output buffer to %lu bytes",
                             static_cast<unsigned long>(frame.needed_size));
                    failed = true;
                    break;
                }
                decoded = larger;
                decoded_capacity = frame.needed_size;
                continue;
            }

            if (raw.consumed > raw.len) {
                ESP_LOGE(TAG, "MP3 decoder reported invalid input consumption");
                failed = true;
                break;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                if (ret == ESP_AUDIO_ERR_DATA_LACK && raw.consumed == 0) {
                    break;
                }
                if (raw.consumed > 0) {
                    raw.buffer += raw.consumed;
                    raw.len -= raw.consumed;
                } else if (raw.len > 1) {
                    ++raw.buffer;
                    --raw.len;
                } else {
                    break;
                }
                if (raw.len >= previous_len) {
                    failed = true;
                    break;
                }
                continue;
            }

            if (raw.consumed > 0) {
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;
            }
            if (frame.decoded_size == 0) {
                if (raw.len >= previous_len) {
                    ESP_LOGE(TAG, "MP3 decoder made no progress");
                    failed = true;
                    break;
                }
                continue;
            }
            if (raw.len >= previous_len && raw.consumed == 0) {
                ESP_LOGE(TAG, "MP3 decoder returned data without consuming input");
                failed = true;
                break;
            }
            if (info.bits_per_sample != 16 ||
                (info.channel != ESP_AUDIO_MONO && info.channel != ESP_AUDIO_DUAL) ||
                frame.decoded_size % (info.channel * sizeof(int16_t)) != 0) {
                ESP_LOGE(TAG, "Unsupported MP3 PCM format: %u bits, %u channels",
                         static_cast<unsigned>(info.bits_per_sample),
                         static_cast<unsigned>(info.channel));
                failed = true;
                break;
            }

            const size_t input_samples =
                frame.decoded_size / (info.channel * sizeof(int16_t));
            const size_t output_samples = ResampleToStereo(
                reinterpret_cast<const int16_t*>(decoded), input_samples,
                info.channel, info.sample_rate, output, kOutputSamples, &resampler);
            if (output_samples == 0 ||
                !SendPcm(output, output_samples * 2 * sizeof(int16_t))) {
                if (!s_stop_requested) {
                    ESP_LOGE(TAG, "Failed sending decoded PCM to WROOM");
                    failed = true;
                }
                break;
            }
            ++decoded_frames;
        }

        if (s_stop_requested || failed || decoded_frames == 0) {
            BtLink::SendPcmPacket(BT_LINK_MSG_PCM_STOP, nullptr, 0);
            if (failed || decoded_frames == 0) {
                ESP_LOGE(TAG, "MP3 stream failed after %lu decoded frames",
                         static_cast<unsigned long>(decoded_frames));
                UartConsole::Notify("[MP3] Giai ma/stream that bai; xem log S3.");
            }
        } else if (!BtLink::SendPcmPacket(BT_LINK_MSG_PCM_END, nullptr, 0)) {
            ESP_LOGE(TAG, "Could not signal end of PCM stream");
            UartConsole::Notify("[MP3] Khong gui duoc ket thuc stream toi WROOM.");
        } else {
            UartConsole::Notify(std::string("[MP3] Phat xong: ") + s_track_name);
        }

        heap_caps_free(decoded);
        heap_caps_free(output);
        esp_mp3_dec_close(decoder);
    }
    heap_caps_free(input_data - id3_bytes);
}

void MusicStreamTask(void*) {
    RunMusicStream();
    ESP_LOGI(TAG, "stream task exit, stack HWM=%u bytes",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    s_active = false;
    s_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

bool S3MusicStreamer::ListTracks(std::vector<std::string>* tracks) {
    if (tracks == nullptr || !EnsureFilesystemMounted()) {
        return false;
    }
    tracks->clear();

    DIR* directory = opendir(kMusicMountPath);
    if (directory == nullptr) {
        ESP_LOGE(TAG, "Cannot open music SPIFFS directory");
        return false;
    }
    while (dirent* entry = readdir(directory)) {
        if (entry->d_type == DT_DIR || !HasMp3Extension(entry->d_name) ||
            std::strlen(entry->d_name) >= sizeof(s_track_name)) {
            continue;
        }
        tracks->emplace_back(entry->d_name);
    }
    closedir(directory);
    std::sort(tracks->begin(), tracks->end());
    return true;
}

bool S3MusicStreamer::Start(const std::string& track_name) {
    if (s_active || track_name.empty() || track_name.size() >= sizeof(s_track_name) ||
        track_name.find('/') != std::string::npos ||
        track_name.find('\\') != std::string::npos ||
        !HasMp3Extension(track_name.c_str())) {
        return false;
    }
    if (!EnsureFilesystemMounted()) {
        return false;
    }
    std::memcpy(s_track_name, track_name.c_str(), track_name.size() + 1);
    s_stop_requested = false;
    s_paused = false;
    s_active = true;
    if (xTaskCreate(MusicStreamTask, "s3_mp3_stream", kTaskStackBytes, nullptr, 4,
                    &s_task) != pdPASS) {
        s_active = false;
        s_task = nullptr;
        ESP_LOGE(TAG, "Could not create MP3 stream task");
        return false;
    }
    return true;
}

void S3MusicStreamer::Stop() {
    if (!s_active) {
        return;
    }
    s_stop_requested = true;
    BtLink::SendPcmPacket(BT_LINK_MSG_PCM_STOP, nullptr, 0);
}

void S3MusicStreamer::SetPaused(bool paused) {
    s_paused = paused;
}

bool S3MusicStreamer::IsActive() {
    return s_active;
}
