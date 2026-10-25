# Điều khiển loa Bluetooth qua Wi-Fi/TCP

ESP32-S3 (AI_BOT) vừa kết nối Wi-Fi router ở chế độ STA để dùng AI server, vừa
mở SoftAP `AI_BOT` để ESP32-WROOM kết nối. Lệnh điều khiển loa và trạng thái
được trao đổi qua TCP; firmware không dùng ESP-NOW.

## Mạng giữa hai board

| Thiết lập | Giá trị |
|---|---|
| SoftAP trên S3 | SSID `AI_BOT` |
| Mật khẩu | `123456789` |
| Địa chỉ S3 trên AP | `192.168.4.1` |
| TCP port | `3333` |
| WROOM | Tự kết nối AP S3 bằng DHCP và mở TCP tới `192.168.4.1:3333` |

Thông tin AP được khai báo trong `src/bt_link.h` của S3 và
`src/bt_link.h` của WROOM; nếu thay đổi SSID hoặc mật khẩu, cập nhật cả hai
firmware rồi nạp lại cả hai board. S3 dùng WPA2-PSK. AP chia sẻ radio với STA,
nên hoạt động cùng kênh Wi-Fi router; WROOM tự theo kênh AP, không cần cấu hình
kênh thủ công.

S3 khởi động AP khi driver Wi-Fi STA được bật. Việc mất kết nối router không
tắt AP nội bộ. Nếu router đổi kênh, WROOM sẽ tự nối lại TCP sau khi Wi-Fi AP
đổi kênh. Khi mất link, kiểm tra log Wi-Fi trên cả hai board và đợi reconnect.

## Luồng lệnh

```text
UART S3 -> BtLink -> TCP :3333 -> WROOM -> Bluetooth A2DP
UART WROOM ---------------------> WROOM -> Bluetooth A2DP
UART WROOM <- console_printf <- TCP status <- WROOM
```

- Chỉ `cm00..cm09` hoặc lệnh chữ được gửi tới WROOM; số menu đơn `0..9` không
  phải lệnh.
- Khi WROOM đang xử lý lệnh, các dòng trạng thái được gửi từng gói TCP riêng.
- TCP sử dụng bản ghi nhị phân cố định `bt_link_packet_t` để giữ nguyên ranh giới
  thông điệp (protocol v2, gồm cả gói PCM); cả hai firmware phải được cập nhật
  cùng nhau.
- S3 tự gửi `cm00` sau khi nhận trạng thái kết nối đầu tiên từ WROOM.
- Lệnh `AI: <câu hỏi>` vẫn chỉ đi vào AI; các lệnh loa Bluetooth không đi vào
  luồng AI.

## Lệnh thường dùng trên UART S3

| Lệnh | Tác dụng |
|---|---|
| `cm00` | In menu điều khiển từ WROOM |
| `cm01` | Quét thiết bị Bluetooth |
| `cm02` | Chọn loa, nguồn nhạc và bài hát |
| `cm03` | Dừng phát hoặc ngắt kết nối |
| `cm04` | Liệt kê file MP3 |
| `cm05` | In trạng thái |
| `cm06` | Phát lại bài vừa phát |
| `cm07` | Chọn file khác |
| `cm08` | Pause/resume |
| `cm09` | In hướng dẫn lệnh WROOM |
| `status`, `scan`, `devices`, `list`, `play 1`, `pause` | Lệnh chữ chuyển tới WROOM |
| `ping` | Kiểm tra TCP bằng lệnh `ping` trên WROOM |

Menu hiển thị lựa chọn dạng `0) [cm00]`, nhưng phải gửi mã `cmNN`, không gửi
số menu độc lập.

### Chọn nguồn phát với `cm02`

Sau khi chọn thiết bị Bluetooth, chọn một trong hai nguồn:

1. **WROOM SPIFFS**: S3 UART hiển thị danh sách file MP3 trên WROOM; WROOM giải mã
   và phát như trước.
2. **ESP32-S3 SPIFFS**: S3 UART hiển thị MP3 trong SPIFFS, giải mã MP3 trên
   S3, chuyển đổi sang PCM stereo 44.1 kHz và gửi PCM qua TCP tới WROOM để phát
   qua A2DP.

Tại bước chọn nguồn trên UART S3, nhập `1` cho WROOM SPIFFS hoặc `2` cho S3
SPIFFS. Lựa chọn số được gửi tương ứng qua TCP tới WROOM. Luồng UART WROOM
(`cm02`) chỉ hỗ trợ phát MP3 local trên WROOM; để phát stream từ S3, hãy bắt đầu
`cm02` trên UART S3.

Với lựa chọn stream, chọn nguồn trước; chỉ danh sách của nguồn đã chọn mới được
hiển thị. ESP32-S3 đọc MP3 có sẵn trong phân vùng SPIFFS `music` (1 MB), mount
tại `/music`; gói assets của giao diện/model vẫn nằm riêng trong phân vùng
`assets` (7 MB). Dùng tên file duy nhất, ASCII, tối đa 31 ký tự gồm cả phần mở
rộng. Build/upload firmware không tạo hoặc nạp ảnh filesystem; nội dung phân
vùng `/music` được giữ nguyên khi chỉ cập nhật firmware.

## Kiểm tra kết nối

1. Nạp firmware mới cho cả S3 và WROOM.
2. Khởi động S3; kiểm tra log có `SoftAP AI_BOT san sang` và `TCP server san
   sang tai 192.168.4.1:3333`.
3. Khởi động WROOM; UART WROOM cần báo đã kết nối Wi-Fi AP và TCP link tới S3.
4. UART S3 sẽ in trạng thái `[WROOM] ket noi TCP qua Wi-Fi AP AI_BOT`, sau đó
   tự gửi `cm00`.
5. Gửi `cm01`, sau đó `cm02`; chọn loa, nguồn phát và bài nhạc để kiểm tra.

Nếu không kết nối được:

- Đảm bảo hai header `bt_link.h` có cùng SSID/mật khẩu và đã nạp cả hai firmware.
- Kiểm tra WROOM nhận IP từ AP `AI_BOT`; TCP server của S3 dùng IP
  `192.168.4.1`, port `3333`.
- Kiểm tra log `WROOM TCP connected` trên UART0 S3 và `TCP link toi S3 da ket
  noi` trên UART WROOM.
- AP của S3 có thể dùng Wi-Fi router cùng lúc; không cấu hình WROOM kết nối
  router hoặc nhập lệnh `channel`.

## Build

```powershell
# S3
Set-Location D:\git\AI_BOT
pio run

# WROOM
Set-Location D:\git\AI_BOT_speaker_transfer
pio run
```
