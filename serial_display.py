import serial
import struct
import cv2
import numpy as np
import time

# ==============================================================================
# SERIAL FRAME RECEIVER - ESP32 → Python via UART
# ==============================================================================
# Nhận frame JPEG từ ESP32 qua cổng serial và hiển thị
# ==============================================================================

# Serial Configuration
SERIAL_PORT = "COM3"  # Thay đổi theo cổng serial của bạn (COM3, COM4, /dev/ttyUSB0, etc.)
BAUD_RATE = 115200

# Frame markers
FRAME_START_MARKER = 0xAA
FRAME_END_MARKER = 0x55

def read_frame_from_serial(ser):
    """Đọc một frame hoàn chỉnh từ serial"""
    try:
        # Đợi frame start marker, bỏ qua các dòng log text
        while True:
            byte = ser.read(1)
            if not byte:
                return None

            # Nếu là ký tự ASCII có thể in (log text), bỏ qua cho đến khi xuống dòng
            if 32 <= byte[0] <= 126:  # Printable ASCII
                # Đọc cho đến khi xuống dòng để bỏ qua dòng log
                while byte and byte[0] != ord('\n') and byte[0] != ord('\r'):
                    byte = ser.read(1)
                continue

            # Nếu là frame start marker
            if byte[0] == FRAME_START_MARKER:
                break

        # Đọc frame size (4 bytes, little endian)
        size_data = ser.read(4)
        if len(size_data) < 4:
            return None

        frame_size = struct.unpack('<I', size_data)[0]

        # Kiểm tra kích thước hợp lý
        if frame_size == 0 or frame_size > 1024 * 1024:  # Max 1MB
            print(f"[-] Invalid frame size: {frame_size}")
            return None

        # Đọc frame data
        frame_data = bytearray()
        bytes_read = 0

        while bytes_read < frame_size:
            chunk = ser.read(min(1024, frame_size - bytes_read))
            if not chunk:
                print(f"[-] Connection lost while reading frame data")
                return None
            frame_data.extend(chunk)
            bytes_read += chunk

        # Đợi frame end marker
        end_marker = ser.read(1)
        if not end_marker or end_marker[0] != FRAME_END_MARKER:
            print(f"[-] Missing frame end marker")
            return None

        return bytes(frame_data)

    except Exception as e:
        print(f"[-] Error reading frame: {e}")
        return None

def main():
    print("[*] Serial Frame Receiver")
    print(f"[*] Connecting to {SERIAL_PORT} at {BAUD_RATE} baud...")

    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
        print(f"[+] Connected to {SERIAL_PORT}")
        time.sleep(2)  # Đợi serial ổn định

        frame_count = 0
        start_time = time.time()

        while True:
            frame_data = read_frame_from_serial(ser)

            if frame_data:
                frame_count += 1

                # Decode JPEG frame
                np_arr = np.frombuffer(frame_data, dtype=np.uint8)
                frame = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)

                if frame is not None:
                    # Hiển thị frame
                    cv2.imshow("ESP32 Serial Stream", frame)

                    # Hiển thị thông tin frame
                    fps = frame_count / (time.time() - start_time)
                    print(f"[+] Frame #{frame_count} | Size: {len(frame_data)} bytes | FPS: {fps:.1f}")

                    # Hiển thị thông tin trên ảnh
                    cv2.putText(frame, f"Frame: {frame_count}", (10, 30),
                               cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
                    cv2.putText(frame, f"FPS: {fps:.1f}", (10, 70),
                               cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
                    cv2.imshow("ESP32 Serial Stream", frame)
                else:
                    print(f"[-] Failed to decode frame #{frame_count}")

            # Thoát khi nhấn 'q'
            if cv2.waitKey(1) & 0xFF == ord('q'):
                print("[*] 'q' pressed. Exiting...")
                break

    except serial.SerialException as e:
        print(f"[-] Serial error: {e}")
        print(f"[*] Make sure {SERIAL_PORT} is correct and not in use by another program")
    except KeyboardInterrupt:
        print("\n[*] Interrupted by user")
    finally:
        if 'ser' in locals():
            ser.close()
        cv2.destroyAllWindows()
        print("[*] Application closed")

if __name__ == "__main__":
    main()
