import time
import os
from flask import Flask, Response

app = Flask(__name__)

def generate_frames(cam_name):
    shm_path = f"/dev/shm/{cam_name}_result.jpg"
    
    while True:
        try:
            # 尝试极速读取内存盘中的图片
            with open(shm_path, "rb") as f:
                image_bytes = f.read()
            
            # 🛡️ 核心防撕裂防护网：如果读到的图片太小（空文件或损坏），直接丢弃，继续读下一帧
            if not image_bytes or len(image_bytes) < 1000:
                time.sleep(0.01)
                continue
                
            yield (b'--frame\r\n'
                   b'Content-Type: image/jpeg\r\n\r\n' + image_bytes + b'\r\n')
                   
        except FileNotFoundError:
            # C++ 还没生成文件时，稍微等待
            time.sleep(0.01)
        except Exception as e:
            # 忽略文件占用等意外错误，保持流不断开
            pass
        
        # 控制帧率，防止 Python 跑得比 C++ 还快导致 CPU 占用过高
        time.sleep(0.015) 

@app.route('/mipi')
def mipi():
    return Response(generate_frames('MIPI'), mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route('/usb')
def usb():
    return Response(generate_frames('USB'), mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route('/')
def index():
    return """
    <html>
    <head>
        <title>RK3588 双路 NPU 监控台</title>
        <style>
            body { font-family: Arial; text-align: center; background-color: #f4f4f9; padding: 20px;}
            .cam-container { display: inline-block; margin: 20px; background: #fff; padding: 15px; border-radius: 10px; box-shadow: 0 4px 8px rgba(0,0,0,0.1);}
            img { width: 640px; height: 640px; background-color: #e0e0e0; border-radius: 5px;}
        </style>
    </head>
    <body>
        <h1>🚀 RK3588 双核 NPU 实时边缘计算监控台</h1>
        <div class="cam-container">
            <h3>MIPI 摄像头 (NPU Core 0)</h3>
            <img src="/mipi" alt="等待视频流...">
        </div>
        <div class="cam-container">
            <h3>USB 摄像头 (NPU Core 1)</h3>
            <img src="/usb" alt="等待视频流...">
        </div>
    </body>
    </html>
    """

if __name__ == '__main__':
    print("🚀 Web 视频服务器已启动！")
    print("👉 请在 Windows 浏览器输入: http://<你的板子IP地址>:5000")
    # 开启 threaded 确保能同时看两路视频流
    app.run(host='0.0.0.0', port=5000, threaded=True)
