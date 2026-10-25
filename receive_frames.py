import socket
import struct
import cv2
import numpy as np
import threading
import queue
import time

# ==============================================================================
# SYSTEM ARCHITECTURE: ESP32-CAM → AI_BOT → Python
# ==============================================================================
# 1. ESP32-CAM captures frames and sends to AI_BOT (WiFi AP at 192.168.137.1:8888)
# 2. AI_BOT receives frames and forwards to this Python program
# 3. Python program displays frames with optional filters
# ==============================================================================

# Configuration
AI_BOT_IP = "192.168.137.1"      # IP của AI_BOT (WiFi AP)
AI_BOT_PORT = 8888               # Cổng AI_BOT lắng nghe
AUTH_PASSWORD = "CAM_ACCESS_PASS_992"  # Mật khẩu xác thực trùng với AI_BOT

# CHỌN BỘ LỌC TẠI ĐÂY:
# "none"      -> Ảnh gốc
# "gray"      -> Ảnh trắng đen
# "blur"      -> Làm mịn / Khử nhiễu hạt
# "sharp"     -> Làm nét chi tiết
# "vintage"   -> Màu hoài cổ (ấm)
FILTER_MODE = "sharp"

# Hàng đợi chứa các frame ảnh sẵn sàng hiển thị (Giới hạn tối đa 5 frame)
frame_queue = queue.Queue(maxsize=5)

def apply_filter(frame, mode):
    """Hàm xử lý và áp dụng bộ lọc lên frame ảnh OpenCV"""
    if mode == "none":
        return frame

    elif mode == "gray":
        # Chuyển sang ảnh xám (Trắng đen)
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        # Chuyển ngược lại 3 kênh màu để đồng bộ hiển thị
        return cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)

    elif mode == "blur":
        # Bộ lọc Gaussian Blur giúp làm mịn ảnh, giảm nhiễu hạt (noise) từ ESP32-CAM
        return cv2.GaussianBlur(frame, (5, 5), 0)

    elif mode == "sharp":
        # Ma trận Kernel giúp tăng cường độ sắc nét cho các đường viền
        kernel = np.array([[0, -1, 0],
                           [-1, 5, -1],
                           [0, -1, 0]])
        return cv2.filter2D(frame, -1, kernel)

    elif mode == "vintage":
        # Tạo hiệu ứng màu phim cũ bằng cách thay đổi ma trận màu sắc
        kernel = np.array([[0.272, 0.534, 0.131],
                           [0.349, 0.686, 0.168],
                           [0.393, 0.769, 0.189]])
        vintage_frame = cv2.transform(frame, kernel)
        # Giới hạn giá trị pixel trong khoảng 0-255 để tránh lỗi hiển thị
        return np.clip(vintage_frame, 0, 255).astype(np.uint8)

    return frame

def tcp_receiver_thread():
    """Luồng phụ: Chuyên nhận dữ liệu từ AI_BOT và đẩy vào hàng đợi"""
    print(f"[*] Connecting to AI_BOT at {AI_BOT_IP}:{AI_BOT_PORT}...")

    while True:
        try:
            client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            client_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

            # Set timeout to avoid hanging
            client_socket.settimeout(5.0)

            try:
                client_socket.connect((AI_BOT_IP, AI_BOT_PORT))
                print(f"[+] Connected to AI_BOT at {AI_BOT_IP}:{AI_BOT_PORT}")

                # --- STEP 1: AUTHENTICATION ---
                client_socket.sendall(AUTH_PASSWORD.encode('utf-8'))

                # Receive auth response
                auth_response = client_socket.recv(32).decode('utf-8', errors='ignore').strip()

                if "AUTH_OK" in auth_response:
                    print(f"[+] Authentication successful!")
                else:
                    print(f"[-] Authentication failed. Response: {auth_response}")
                    client_socket.close()
                    time.sleep(3)
                    continue

                # --- STEP 2: VIDEO RECEPTION ---
                data = b""
                payload_size = struct.calcsize("<I")

                while True:
                    # Receive frame size header (4 bytes)
                    while len(data) < payload_size:
                        packet = client_socket.recv(4096)
                        if not packet:
                            raise ConnectionError("Connection lost")
                        data += packet

                    packed_msg_size = data[:payload_size]
                    data = data[payload_size:]
                    msg_size = struct.unpack("<I", packed_msg_size)[0]

                    if msg_size == 0 or msg_size > 1024 * 1024:
                        print(f"[-] Invalid size {msg_size}. Resetting...")
                        data = b""
                        continue

                    # Receive frame data
                    while len(data) < msg_size:
                        packet = client_socket.recv(4096)
                        if not packet:
                            raise ConnectionError("Connection lost")
                        data += packet

                    frame_data = data[:msg_size]
                    data = data[msg_size:]

                    # Put frame in queue
                    if not frame_queue.full():
                        frame_queue.put(frame_data)
                    else:
                        try:
                            frame_queue.get_nowait()
                            frame_queue.put(frame_data)
                        except queue.Empty:
                            pass

            except (socket.timeout, ConnectionError, OSError) as e:
                print(f"[-] Connection error: {e}. Reconnecting in 3 seconds...")
                client_socket.close()
                time.sleep(3)
                continue

        except Exception as e:
            print(f"[-] Unexpected error: {e}. Reconnecting in 3 seconds...")
            time.sleep(3)
            continue

# ---------------------------------------------------------
# LUỒNG CHÍNH (MAIN THREAD): Xử lý bộ lọc và hiển thị UI
# ---------------------------------------------------------
if __name__ == "__main__":
    import time

    receiver = threading.Thread(target=tcp_receiver_thread, daemon=True)
    receiver.start()

    print(f"[*] Main UI Thread running. Active filter: {FILTER_MODE.upper()}")

    while True:
        try:
            frame_data = frame_queue.get(timeout=0.1)

            np_arr = np.frombuffer(frame_data, dtype=np.uint8)
            frame = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)

            if frame is not None:
                # 👉 ÁP DỤNG BỘ LỌC TẠI ĐÂY TRƯỚC KHI HIỂN THỊ
                processed_frame = apply_filter(frame, FILTER_MODE)

                # Hiển thị frame đã qua xử lý
                cv2.imshow("AI_BOT Stream (Filtered)", processed_frame)
            else:
                print("[-] Warning: Failed to decode JPEG frame.")

        except queue.Empty:
            pass

        if cv2.waitKey(1) & 0xFF == ord('q'):
            print("[*] 'q' pressed. Exiting...")
            break

    cv2.destroyAllWindows()
    print("[*] Application closed.")
