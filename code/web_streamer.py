import os
import mmap
import socket
import struct
import threading
import math
import time
import cv2
import logging
from datetime import datetime
from flask import Flask, render_template, jsonify, Response, send_from_directory

/*
 * python3 web_streamer.py来运行
 * 然后去开发板IP地址的5000端口访问 http://<IP_ADDRESS>:5000 就能看到双路视频流和传感器数据了
 */

app = Flask(__name__)
log = logging.getLogger('werkzeug')
log.setLevel(logging.ERROR)

MIPI_DEV = "/dev/video11"
USB_DEV  = "/dev/video20"
STORAGE_PATH = "./records"        
RECORD_DURATION = 5               # 录制时长 5 秒

os.makedirs(STORAGE_PATH, exist_ok=True)

# 【核心修复1】严格对齐你 F103 的真实数据结构
latest_data = {
    "speed": 0.0, "brake": 0, "alarm": 0,
    "ax": 0.0, "ay": 0.0, "az": 0.0,
    "pitch": 0.0, "roll": 0.0,
    "ai_status": "正常"
}

global_frames = {"mipi": None, "usb": None}

def camera_worker(video_source, key):
    if "video11" in video_source:
        pipeline = (f"v4l2src device={video_source} ! "
                    f"video/x-raw, format=NV12, width=640, height=480 ! "
                    f"videoconvert ! appsink drop=true sync=false")
        cap = cv2.VideoCapture(pipeline, cv2.CAP_GSTREAMER)
    else:
        cap = cv2.VideoCapture(video_source, cv2.CAP_V4L2)
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
    
    print(f"🎥 {key.upper()} 摄像头数据总线已挂载！")
    while True:
        ret, frame = cap.read()
        if ret:
            global_frames[key] = frame
        else:
            time.sleep(0.1)

class VideoRecorder:
    def __init__(self, key):
        self.key = key
        self.is_recording = False

    def start(self):
        threading.Thread(target=self.monitor_and_record, daemon=True).start()

    def monitor_and_record(self):
        while True:
            # 【核心修复2】只有真正的 0x050 警报 (值为 0xFF=255) 才会触发录像！刹车绝对不触发！
            if latest_data["alarm"] == 255 and not self.is_recording:
                self.is_recording = True
                
                # 触发后立刻在软件层清零，防止死循环
                latest_data["alarm"] = 0
                
                timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
                filename = f"ALARM_{self.key}_{timestamp}.mp4"
                filepath = os.path.join(STORAGE_PATH, filename)
                
                print(f"🚨 极端危险警报！[{self.key.upper()}] 开始录制 5 秒现场...")
                
                fourcc = cv2.VideoWriter_fourcc(*'avc1')
                out = cv2.VideoWriter(filepath, fourcc, 20.0, (640, 480))
                
                start_time = time.time()
                while time.time() - start_time < RECORD_DURATION:
                    frame = global_frames.get(self.key)
                    if frame is not None:
                        out.write(frame)
                    time.sleep(0.05) 
                
                out.release()
                print(f"✅ 录制结束，已固化证据: {filename}")
                
                # 录完强制冷却 3 秒
                time.sleep(3)
                latest_data["alarm"] = 0
                self.is_recording = False
            else:
                time.sleep(0.1) 

def can_listener():
    try:
        can_sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
        can_sock.bind(("can0",))
        print("✅ Python CAN0 监听器已启动")
    except Exception as e:
        print(f"❌ CAN 初始化失败: {e}")
        return

    while True:
        try:
            cf, addr = can_sock.recvfrom(16)
            can_id, can_dlc, data = struct.unpack("<IB3x8s", cf)
            can_id &= 0x1FFFFFFF 
            
            # 【核心修复3】精准解析 F103 的真实 ID
            if can_id == 0x201:
                # 假设车速是 float (4字节)，如果不准的话后面可以调
                try:
                    speed = struct.unpack("<f", data[:4])[0]
                    latest_data["speed"] = round(speed, 1)
                except:
                    pass
            elif can_id == 0x101:
                # 常规刹车
                latest_data["brake"] = data[0]
            elif can_id == 0x050:
                # 极端危险警报 (FF就是255)
                latest_data["alarm"] = data[0]
        except Exception:
            pass

def spi_mmap_reader():
    try:
        fd = os.open("/dev/h7_mmap", os.O_RDONLY)
        mm = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ)
        print("✅ Python SPI 零拷贝通道已建立")
        last_count = 0
        while True:
            head_idx = mm[0] | (mm[1] << 8) | (mm[2] << 16) | (mm[3] << 24)
            current_count = mm[4] | (mm[5] << 8) | (mm[6] << 16) | (mm[7] << 24)
            if head_idx >= 100: head_idx = 0
            if current_count != last_count:
                latest_idx = 99 if head_idx == 0 else head_idx - 1
                frame_offset = 40 + (latest_idx * 32)
                raw_ax = (mm[frame_offset] << 8)   | mm[frame_offset+1]
                raw_ay = (mm[frame_offset+2] << 8) | mm[frame_offset+3]
                raw_az = (mm[frame_offset+4] << 8) | mm[frame_offset+5]
                if raw_ax >= 32768: raw_ax -= 65536
                if raw_ay >= 32768: raw_ay -= 65536
                if raw_az >= 32768: raw_az -= 65536
                ax = raw_ax / 16384.0
                ay = raw_ay / 16384.0
                az = raw_az / 16384.0
                pitch = math.atan2(-ax, math.sqrt(ay*ay + az*az)) * 180.0 / math.pi
                roll  = math.atan2(ay, az) * 180.0 / math.pi
                latest_data["ax"] = round(ax, 2)
                latest_data["ay"] = round(ay, 2)
                latest_data["az"] = round(az, 2)
                latest_data["pitch"] = round(pitch, 2)
                latest_data["roll"] = round(roll, 2)
                last_count = current_count
            time.sleep(0.01) 
    except Exception as e:
        print(f"❌ SPI Mmap 读取失败: {e}")

def generate_web_stream(key):
    while True:
        frame = global_frames.get(key)
        if frame is not None:
            ret, buffer = cv2.imencode('.jpg', frame, [int(cv2.IMWRITE_JPEG_QUALITY), 70])
            frame_bytes = buffer.tobytes()
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n\r\n' + frame_bytes + b'\r\n')
        else:
            time.sleep(0.1)
        time.sleep(0.04) 

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/mipi')
def mipi_stream():
    return Response(generate_web_stream("mipi"), mimetype="multipart/x-mixed-replace; boundary=frame")

@app.route('/usb')
def usb_stream():
    return Response(generate_web_stream("usb"), mimetype="multipart/x-mixed-replace; boundary=frame")

@app.route('/sensor_data')
def sensor_data():
    return jsonify(latest_data)

@app.route('/list_records')
def list_records():
    files = sorted([f for f in os.listdir(STORAGE_PATH) if f.endswith('.mp4')], reverse=True)
    return jsonify(files)

@app.route('/video/<filename>')
def get_video(filename):
    return send_from_directory(STORAGE_PATH, filename)

if __name__ == '__main__':
    threading.Thread(target=camera_worker, args=(MIPI_DEV, "mipi"), daemon=True).start()
    threading.Thread(target=camera_worker, args=(USB_DEV, "usb"), daemon=True).start()
    
    VideoRecorder("mipi").start()
    VideoRecorder("usb").start()
    
    threading.Thread(target=can_listener, daemon=True).start()
    threading.Thread(target=spi_mmap_reader, daemon=True).start()
    
    print("🚀 车规级系统全链路启动完毕: http://0.0.0.0:5000")
    app.run(host='0.0.0.0', port=5000, threaded=True)