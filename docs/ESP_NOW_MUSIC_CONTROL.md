# Điều khiển loa Bluetooth qua ESP32-WROOM bằng ESP-NOW

Tài liệu này mô tả phần liên kết giữa firmware **AI_BOT (ESP32-S3)** và project
**AI_BOT_speaker_transfer (ESP32-WROOM)**.

## 1. Kiến trúc

```
PC / USB-TTL                ESP32-S3 (AI_BOT)              ESP32-WROOM                 Loa / tai nghe
------------                -----------------              ------------                ---------------
gõ "play 1" / menu  --UART1-->  UartConsole                  bt_link (ESP-NOW)          Bluetooth Classic
                                HandleMusicCommand  --ESP-NOW-->  execute_command  --A2DP-->  (SBC)
                                BtLink::SendCommand                bt_a2dp / mp3_player
                                <--ESP-NOW status---               <-- in từng dòng trạng thái
```

- **S3 nhận lệnh từ UART** (UART1: TX=GPIO21, RX=GPIO20, 115200 8N1), gồm cả
  mã Bluetooth `cm00`..`cm09`, menu số và lệnh chữ (`play 1`, `status`,
  `pause`, ...). Các lệnh này được xử lý riêng, không gửi tới AI.
- S3 gửi lệnh dạng text qua **ESP-NOW broadcast**.
- **WROOM nhận lệnh từ 2 nguồn**:
  - UART1 riêng của WROOM (TX=GPIO17, RX=GPIO16, 115200 8N1) — menu số cũ vẫn dùng được;
  - ESP-NOW từ S3.
- Mọi lệnh sau khi nhận đều đi vào **cùng một bộ xử lý** (`execute_command`) và
  được bảo vệ bằng mutex, nên UART và ESP-NOW không tranh nhau A2DP/bộ giải mã.
  Menu số UART `0..9` được chuẩn hóa thành `cm00..cm09` rồi cũng đi qua hàm
  này. Các mục cần chọn thêm (`cm03`, `cm07`, `cm08`) hỏi tiếp trên UART;
  khi gọi từ S3, tham số lựa chọn được gửi rõ trong lệnh ESP-NOW.
- WROOM điều khiển loa/tai nghe Bluetooth qua **A2DP Source** (giải mã MP3 trong
  SPIFFS rồi stream SBC).
- Mỗi dòng WROOM in ra được gửi ngược về S3 dưới dạng các gói `STATUS`; S3 in
  lên UART với tiền tố `[WROOM]`.

## 2. Giao thức ESP-NOW

Header: `src/bt_link.h` — phần định nghĩa gói (magic, version, enum, struct)
phải **giống hệt** ở `AI_BOT/src/bt_link.h` và
`AI_BOT_speaker_transfer/src/bt_link.h`. Hai file khác nhau ở phần API
(phía S3 bọc trong class C++, phía WROOM dùng hàm C).

```c
#define BT_LINK_MAGIC     0xB7
#define BT_LINK_VERSION   1
#define BT_LINK_TEXT_LEN  192

typedef struct __attribute__((packed)) {
    uint8_t  magic;                 // 0xB7
    uint8_t  version;               // 1
    uint8_t  type;                  // COMMAND / STATUS / HELLO
    uint8_t  seq;                   // tăng mỗi lệnh, WROOM echo lại
    uint16_t reserved;
    char     text[192];             // "play 1", "pause", ...
} bt_link_packet_t;
```

- S3 gửi **broadcast** `ff:ff:ff:ff:ff:ff` → không cần biết MAC của WROOM.
- WROOM trả lời **unicast** về MAC nguồn (lấy từ gói nhận được).
- Mỗi lệnh được S3 gửi 3 lần cùng `seq`; WROOM lọc trùng trong 4 giây nên lệnh
  chỉ chạy 1 lần.
- S3 gửi `HELLO` mỗi 1 giây khi chưa thấy WROOM, mỗi 5 giây khi đã thấy. WROOM
  trả lời `hello ch=<kenh> ...`.
- WROOM **ACK ngay** (`da nhan lenh: ...`) khi nhận lệnh, trước khi chạy. Nhờ vậy
  các lệnh dài như `scan` (~13–40 giây) hay `connect` (thử 3 lần) không bị S3
  báo nhầm "khong nhan duoc phan hoi".
- Riêng luồng chọn `cm02`, WROOM gửi các sự kiện nội bộ `devices-ready`,
  `files-ready`, `connect-failed`, `files-empty` hoặc `devices-empty`. S3 dùng
  chúng để chỉ hỏi bước tiếp theo sau khi danh sách/kết quả tương ứng đã sẵn
  sàng; các sự kiện này không hiện như dòng trạng thái cho người dùng.
- S3 chỉ cảnh báo khi **không nhận được bất kỳ gói nào**: 45 giây cho `scan`,
  25 giây cho `connect`, 15 giây cho `play`, 10 giây cho `list`/`devices`,
  8 giây cho lệnh khác. Đây chỉ là cảnh báo, không chặn lệnh tiếp theo.

> Khi sửa `bt_link.h`, phải sửa **cả hai** bản:
> `AI_BOT/src/bt_link.h` và `AI_BOT_speaker_transfer/src/bt_link.h`.

## 3. Kênh WiFi (quan trọng)

ESP-NOW chỉ hoạt động khi hai chip ở **cùng kênh WiFi**.

- S3 đang kết nối WiFi nhà → kênh do router quyết định (1..13).
- WROOM **không kết nối AP**, chỉ dùng WiFi cho ESP-NOW. Lúc rảnh (chưa kết nối
  A2DP) WROOM tự nhảy kênh 1..13 mỗi 350 ms cho đến khi nghe được `HELLO` của
  S3, sau đó **khóa cố định** vào kênh đó. Khoảng lặng không làm WROOM tự đổi
  kênh nên không ngắt luồng chọn thiết bị/bài hát.
- Khi rảnh, S3 gửi `HELLO` mỗi 5 giây và WROOM trả lời `hello ch=...`. Trong
  lúc WROOM đang xử lý lệnh, S3 tạm dừng ping và WROOM không gửi pong; các gói
  phản hồi dữ liệu được ưu tiên, tránh ping chen vào luồng kết quả.
- Vì vậy lần đầu bật WROOM chỉ cần đợi khoảng 5–10 giây là link tự đồng bộ.
  Nếu chưa từng nghe WROOM (hoặc quá 25 s không có gói nào), lệnh đầu tiên sẽ
  gửi một chuỗi `HELLO` quét đủ 13 kênh (tối đa ~5 s) rồi mới gửi lệnh; chuỗi
  này dừng ngay khi WROOM trả lời.
- Sau khi khóa, chỉ nhập `channel auto` trên UART WROOM để yêu cầu dò lại (ví
  dụ khi router đổi kênh mà ESP-NOW đã mất kết nối); WROOM sẽ khóa vào kênh S3
  khi tìm thấy. Lệnh đổi kênh gửi từ S3 chỉ dùng được khi link còn hoạt động.

Lệnh kênh (gửi từ S3 hoặc gõ trên UART WROOM):

| Lệnh | Tác dụng |
|---|---|
| `music channel` | Xem kênh ESP-NOW hiện tại của S3 (và gợi ý kiểm tra WROOM) |
| `music status` | Xem kênh WROOM đang dùng (dòng `ESP-NOW: ... kenh N`) |
| `music channel 6` | Khóa WROOM vào kênh 6 |
| `music channel auto` | Cho WROOM tự dò kênh lại |

## 4. Menu và lệnh điều khiển

### Menu trên UART1 của S3

Khi khởi động (và khi gõ `menu`, `music` hoặc `cm00`), S3 in menu giống console
của WROOM:

```
===== AI_BOT -> Loa Bluetooth (ESP32-WROOM qua ESP-NOW) =====
ESP-NOW: san sang, kenh S3 = 6
cm01) Quet thiet bi Bluetooth
cm02) Chon thiet bi + file MP3 de phat
cm03) Dung phat / ngat ket noi
cm04) Liet ke file MP3
cm05) Xem trang thai (status)
cm06) Lap lai bai vua phat
cm07) Chon bai khac (giu ket noi)
cm08) Tam dung / tiep tuc (pause/resume)
cm09) Danh sach lenh dieu khien
cm00) In lai menu
Go cm01..cm09 (hoac 1..9), hoac lenh: play 1 / pause / resume / next / prev / stop / list / scan / status
```

### Mã lệnh `cmNN` (dùng chung S3 ↔ WROOM)

Người dùng gõ **mã `cmNN`** rồi Enter. S3 gửi **nguyên mã đó** (kèm tham số nếu
có) sang WROOM qua ESP-NOW; WROOM đọc mã và điều khiển Bluetooth:

| Mã | S3 gửi sang WROOM | WROOM thực hiện |
|---|---|---|
| `cm01` | `cm01` | Quét thiết bị Bluetooth (~13 giây) |
| `cm02` | Tương tác | S3 gửi `cm02`, hỏi số thiết bị; gửi `cm02 <dev>`, chờ kết quả kết nối + danh sách bài; hỏi số bài rồi gửi `cm02 <file>` |
| `cm03` | `cm03 1` / `cm03 2` | `1` = dừng phát (giữ kết nối), `2` = ngắt kết nối A2DP |
| `cm04` | `cm04` | Liệt kê file MP3 |
| `cm05` | `cm05` | In trạng thái A2DP / bài / kênh ESP-NOW |
| `cm06` | `cm06` | Phát lại bài vừa phát |
| `cm07` | `cm07 <file>` | Phát file khác, giữ kết nối |
| `cm08` | `cm08 1` / `cm08 2` | `1` = pause, `2` = resume |
| `cm09` | — | In danh sách lệnh tại S3 |
| `cm00` | — | In lại menu tại S3 |

- Gõ số `1`..`9` tương đương `cm01`..`cm09`; S3 luôn gửi mã `cmNN` sang WROOM.
- `cm02` không kèm tham số sẽ gửi lệnh bắt đầu trước, chờ danh sách thiết bị,
  rồi hỏi số loa. WROOM chuyển số đã chọn thành `cm02 <tên thiết bị>` để kết
  nối. S3 chờ kết quả kết nối và danh sách bài sẵn sàng mới hỏi số bài; WROOM
  chuyển lựa chọn bài thành `cm02 <tên thiết bị> <tên file MP3>` để phát.
  Lỗi kết nối sẽ dừng luồng chọn.
- `cm03`, `cm07`, `cm08` nếu **không** kèm tham số thì S3 sẽ hỏi số
  (Enter suông = huỷ). Có thể gõ thẳng: `cm02 2 0`, `cm03 1`, `cm07 3`, `cm08 2`.
- `cm03` không kèm tham số trên console WROOM sẽ hỏi `1`/`2`; khi gửi qua
  ESP-NOW phải kèm tham số (`cm03 1` hoặc `cm03 2`).
- Menu UART của WROOM dùng **cùng bộ mã** này (`1) [cm01] ...`), nên gõ `cmNN`
  trên console WROOM cũng chạy đúng hành động.

### Lệnh chữ

Trên console UART1 của S3 có thể gõ **trực tiếp** như trên WROOM (không cần
tiền tố), hoặc dùng tiền tố `music`/`bt`/`speaker`:

| Lệnh | Tác dụng |
|---|---|
| `menu`, `music` | In lại menu điều khiển |
| `status` | Trạng thái A2DP, bài đang phát, pause, kênh ESP-NOW, heap |
| `list` | Liệt kê file `.mp3` trong SPIFFS của WROOM |
| `devices` | Liệt kê thiết bị Bluetooth đã quét |
| `scan` | Quét thiết bị Bluetooth (~13 giây) |
| `connect <n>` | Kết nối tới thiết bị số `n` |
| `play [n]` | Phát file số `n` (không có `n` = phát lại bài cũ). Tự kết nối A2DP nếu chưa kết nối |
| `pause` | Tạm dừng, **giữ kết nối và vị trí bài** |
| `resume` | Tiếp tục phát |
| `next` / `prev` | Bài tiếp theo / trước (giữ kết nối) |
| `replay` | Phát lại bài vừa phát |
| `stop` | Dừng phát, **giữ kết nối A2DP** |
| `disconnect` | Dừng phát + ngắt kết nối A2DP |
| `ping` | Kiểm tra đường ESP-NOW (WROOM trả `pong`) |
| `help` | In danh sách lệnh của S3 |

Lưu ý: `help`, `state`, `media`, `wifi`, `device` vẫn là lệnh của S3. Các lệnh
còn lại ở bảng trên (và mọi mã `cmNN`) được chuyển cho WROOM qua ESP-NOW.
Muốn gửi câu hỏi tới AI qua UART1, dùng `AI: <câu hỏi>` (ví dụ
`AI: hôm nay thời tiết thế nào`); dòng văn bản không có tiền tố `AI:` không
được gửi tới AI.

### Ví dụ

```
# cách 1: dùng mã menu (gõ rồi Enter)
cm01              # quet thiet bi Bluetooth
cm02              # hien DS loa -> chon 2 -> cho ket noi/DS bai -> chon 0
cm05              # xem trang thai
cm08              # 1 = pause, 2 = resume

# hoặc gõ kèm tham số, không bị hỏi
cm02 2 0          # ket noi thiet bi 2 + phat file 0
cm03 1            # dung phat, giu ket noi
cm07 3            # phat file 3
cm08 2            # resume

# cách 2: gõ lệnh trực tiếp (không cần 'music')
scan
devices
connect 2
play 0
pause
resume
next
stop

# cách 3: tiền tố music (vẫn dùng được)
music scan
music play 0
music stop
```

## 5. Cấu hình phần cứng

### S3 (AI_BOT)

Đã có sẵn trong `src/uart_console.h`:

```
USB-TTL TX  -> GPIO20 (UART1 RX)
USB-TTL RX  -> GPIO21 (UART1 TX)
GND         -> GND
115200 8N1
```

### WROOM (AI_BOT_speaker_transfer)

```
USB-TTL TX  -> GPIO16 (UART1 RX)
USB-TTL RX  -> GPIO17 (UART1 TX)
GND         -> GND
115200 8N1
```

- Mức 3.3 V, không dùng adapter 5 V.
- Không nối TX–TX, không để RX thả nổi.
- Hai board chỉ cần chung GND nếu muốn; ESP-NOW là không dây.

## 6. Build / nạp

```powershell
# ESP32-S3
cd D:\git\AI_BOT
pio run -t upload

# ESP32-WROOM
cd D:\git\AI_BOT_speaker_transfer
pio run -t upload
tools\upload_fs.ps1     # nạp file .mp3 vào SPIFFS
```

> Project WROOM đặt `CONFIG_ESP_WIFI_IRAM_OPT=n` và
> `CONFIG_ESP_WIFI_RX_IRAM_OPT=n` để đủ IRAM khi chạy đồng thời WiFi/ESP-NOW và
> Bluetooth Classic.

## 7. Xử lý sự cố

| Hiện tượng | Kiểm tra |
|---|---|
| `music ping` báo "khong nhan duoc phan hoi" | Trước hết xem console UART1 có dòng `[BT] ESP-NOW san sang, kenh N` chưa. Nếu chưa: log UART0 115200 của S3 phải có `BtLink: WiFi started - initialising ESP-NOW` và `BtLink: ESP-NOW ready on channel N`. Nếu có dòng `[WROOM] hello ch=...` nghĩa là link đã thông. Sau đó kiểm tra WROOM đã nạp firmware có `bt_link.c` chưa (`ESP-NOW san sang (kenh N)` trên UART WROOM). Cuối cùng kiểm tra hai board cùng kênh: `music status` / `music channel`. |
| `[BT] dang dong bo kenh voi WROOM...` rồi `[BT] chua thay WROOM sau khi quet kenh` | S3 đã phát HELLO đủ 13 kênh nhưng WROOM không trả lời. Kiểm tra theo thứ tự: (1) WROOM đã nạp firmware mới có `bt_link.c` chưa — log boot phải có `Khoi tao ESP-NOW...` và `ESP-NOW san sang (kenh N)`; (2) WROOM có nguồn và không báo `ESP-NOW that bai`; (3) WROOM đang phát nhạc (streaming) thì không dò kênh — dừng phát, hoặc đặt `channel N` trên WROOM bằng đúng kênh S3; (4) gõ `channel` trên S3: nếu hiện `WROOM: chua bao gio nghe thay` thì link chưa từng thông. |
| Báo `ESP-NOW chua san sang (WiFi chua khoi dong xong)` | WiFi chưa start nên ESP-NOW chưa được init (cố tình: init sớm sẽ panic). Đợi 2–3 s rồi gõ lại; nếu quá 15 s vẫn vậy, xem log UART0. |
| Lệnh chạy 2–3 lần | Sai `BT_LINK_VERSION` / khác header giữa hai project (mất lọc trùng `seq`). |
| Kết nối A2DP thất bại | `music scan` lại; loa phải đang ở chế độ Bluetooth và chưa nối điện thoại. |
| Pause nhưng loa vẫn phát tiếp | Loa không hỗ trợ AVRCP/AVDTP suspend; dùng `music stop` (ngắt stream) thay thế. |
| WROOM hết RAM / link không lên | Xem log UART WROOM lúc boot; `bt_link_init` in heap và lỗi `esp_now_init`. |
| S3 boot khi chưa có WiFi | Link chờ WiFi start rồi mới `esp_now_init`; vào AP cấu hình vẫn dùng được ESP-NOW trên kênh AP. |

## 8. Code liên quan

| Project | File | Nội dung |
|---|---|---|
| AI_BOT | `src/bt_link.h/.cc` | Gửi lệnh, nhận status, HELLO/đồng bộ kênh |
| AI_BOT | `src/uart_console.cc` | Lệnh `music ...`, in `[WROOM] ...` |
| WROOM | `src/bt_link.h/.c` | Nhận lệnh, trả status, tự dò kênh |
| WROOM | `src/main.c` | `execute_command()` dùng chung cho UART + ESP-NOW |
| WROOM | `src/bt_a2dp.c` | A2DP source, suspend/resume |
| WROOM | `src/mp3_player.c` | Giải mã MP3, pause/resume giữ vị trí |
