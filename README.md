# RK3588-HeteroVision-Hub (车规级边缘多模态数据融合系统)

![Platform](https://img.shields.io/badge/Platform-RK3588%20(Orange%20Pi%205%20Max)-blue)
![Language](https://img.shields.io/badge/Language-C%2FCH%2B%2B-orange)
![Framework](https://img.shields.io/badge/Framework-V4L2%20%7C%20MPP%20%7C%20RKNN-green)

## 一、项目简介
本项目是一个运行在基于 RK3588 架构（Orange Pi 5 Max）上的车规级边缘计算与流媒体聚合网关。系统实现了异构双路摄像头（MIPI + USB）的底层视频流捕获、CAN 总线与 SPI 零拷贝内存映射的数据融合，以及具备高可用降级机制的紧急行车记录仪（DVR）功能。

## 二、快速开始（核心优先级最高）
### 2.1 环境准备
```
- 硬件平台与外设
核心主板: Orange Pi 5 Max (基于 Rockchip RK3588 SoC)
操作系统/固件: Ubuntu 22.04 LTS
视觉传感器:
MIPI CSI: OV13855
USB UVC: 标准 USB 2.0/3.0 免驱免感摄像头
通信与姿态传感器:
CAN 收发器: SN65HVD230(挂载于 can0 节点，支持 500kbps 及以上波特率)
SPI 姿态传感器: MPU6050，配合自定义 spi_drv.ko 内核模块驱动

- 系统版本约束
Linux Kernel: 6.1
Python: >= 3.8 (推荐 3.10)
GCC 交叉/本地编译工具链: >= 9.4.0 (用于编译自定义内核模块与 C 测试程序)
OpenCV: >= 4.5.x (必须携带 GStreamer 编译宏)
NumPy: < 2.0 (如 1.26.4，严禁使用 NumPy 2.x，避免 C-API 数组指针对接崩溃)
```
### 2.2 测试和验证
```
sudo insmod led_drv.ko
sudo ./led_test
sudo insmod spi_drv.ko
sudo ./spi_test
sudo ip link set can0 type can bitrate 500000
sudo ip link set up can0
sudo ./can_test
sudo ./spi_can
```
### 2.3 启动验证
```
sudo insmod led_drv.ko
sudo insmod spi_drv.ko
sudo ip link set can0 type can bitrate 500000
sudo ip link set up can0
python3 web_streamer.py
然后可在[开发板IP]:5000网站看到页面
```

## 三、 核心技术亮点 (Key Features)
### 3.1 异构多路视觉链路底层保障
打破普通应用层调用的性能瓶颈，针对 RK3588 的 RKISP 硬件单元，通过 GStreamer 管道强制指定 io-mode=4 开启 mmap 内存映射，实现了内核空间到用户空间的高效零拷贝图像传输，彻底解决了 MIPI 摄像头（OV13855）在多线程下的黑屏与锁死问题。

### 3.2 车规级端序对齐与总线解析
基于 SocketCAN 原生接口监听下位机（如 STM32F103）报文。针对不同架构的内存对齐规则，实现了 16位大端序（Big-Endian）实时车速（ID: 0x201）、刹车状态（ID: 0x101）及极端警报（ID: 0x050）的精准解析与状态机管理。

### 3.3 高可用 DVR 容灾降级机制
设计了抗毁伤的紧急录像逻辑。在接收到 0xFF 致命警报时触发 5 秒现场固化。针对 ARM 平台 V4L2 M2M 硬件 H.264 编码器易崩溃的痛点，创新性地引入了 Codec 降级机制：当硬件 MP4 编码器初始化失败时，系统自动平滑回退至高鲁棒性的 AVI/MJPG 软件编码，结合强制对齐（640x480），确保关键证据 100% 成功固化且主线程不崩溃。

### 3.4 SPI Mmap 姿态传感器零拷贝通道
通过 mmap 直接读取自研 SPI 驱动（/dev/h7_mmap）映射的内核级环形缓冲区，实现 IMU（六轴姿态）数据的微秒级无延迟获取。

## 四、 系统架构
```
[下位机节点]                   [RK3588 核心网关]                          [用户端]
STM32F103  ===(  CAN   )==>   SocketCAN 监听线程     \
STM32H743  ===(SPI Mmap)==>   SPI 零拷贝读取线程      |==> 数据聚合池 ==> Flask Web Dashboard
OV13855    ===(MIPI CSI)==>   V4L2 mmap 图像队列     |               ==> MJPEG 实时推流
USB Camera ===(  USB   )==>   V4L2 图像队列         /
                                   |
                                   v
                             [🚨 0xFF 警报触发] 
                                   |
                                   v
                       [高可用 DVR: MP4 -> AVI 容灾录制]
```

## 五、 引脚连接
```
Pin 3  (CAN0_RX_M0  ) -> CAN 收发器的 RXD
Pin 5  (CAN0_TX_M0  ) -> CAN 收发器的 TXD
Pin 11 (SPI4_MISO_M2) -> H7 的 SPI_MISO
Pin 12 (GPIO4_A6    ) -> H7 的 中断
Pin 13 (SPI4_MOSI_M2) -> H7 的 SPI_MOSI
Pin 15 (SPI4_CLK_M2 ) -> H7 的 SPI_SCK
Pin 16 (SPI4_CS0_M2 ) -> H7 的 SPI_NSS
Pin 18 (GPIO1_A4    ) -> LED
```

## 六、项目结构
```
RK3588-HeteroVision-Hub/
├── README.md  # 项目总体说明文档
├── code/
│   ├── can_test    # CAN 总线测试可执行文件
│   ├── can_test.c  # CAN 总线测试源代码
│   ├── led_drv.c   # LED 驱动源代码
│   ├── led_drv.ko  # LED 驱动内核模块
│   ├── led_test    # LED 测试可执行文件
│   ├── led_test.c  # LED 测试源代码
│   ├── Makefile    # 构建脚本
│   ├── ov13855.c   # OV13855 摄像头传感器驱动
│   ├── r5ett.dtsi  # RK3588 设备树配置文件
│   ├── rk3588-orangepi-5-max-camera1.dtsi  # Orange Pi 5 Max 摄像头设备树配置
│   ├── spi_can     # SPI CAN 可执行文件
│   ├── spi_can.c   # SPI CAN 源代码
│   ├── spi_drv.c   # SPI 驱动源代码
│   ├── spi_drv.ko  # SPI 驱动内核模块
│   ├── spi_test    # SPI 测试可执行文件
│   ├── spi_test.c  # SPI 测试源代码
│   ├── web_streamer.py  # 网络流媒体 Python 脚本
│   └── templates/
│       └── index.html   # Web 界面模板
├── doc/  # 文档目录
└── STM32F103C8T6/
    ├── README.md  # STM32F103 子项目说明文档
    └── F103_FreeRTOS_CAN_Node/
        ├── Core/
        │   ├── Inc/  # 头文件目录
        │   └── Src/  # 源文件目录
        ├── ...
└── STM32H7/
    ├── README.md  # STM32H7 子项目说明文档
    └── H7-HS-Bus/
        ├── Core/
        │   ├── Inc/  # 头文件目录
        │   └── Src/  # 源文件目录
        ├── ...
```

## 七、常见排错指南
| 症状 | 可能原因 | 解决方案 |
| ---- | ---- | ---- |
| 程序报错 `AttributeError: _ARRAY_API not found` | pip 错误升级了 NumPy 到 2.x | 运行 `sudo pip3 install "numpy<2"` 进行降级。 |
| MIPI OV13855 画面全黑 | `rkaiq_3A_server` 进程未启动或意外挂掉 | 执行 `sudo pkill -9 rkaiq_3A_server` 然后重新拉起。 |
| 触发警报后终端打印 `Aborted` 并闪退 | 硬件 `avc1` 编码器冲突，且被 OpenCV 致命捕获 | 检查代码中 DVR 降级逻辑是否开启（强制转为 MJPG 软编）。 |
| 生成的报警录像文件为 0 字节或无法播放 | V4L2 写入分辨率校验未通过 | 确保写入前调用了 `cv2.resize(frame, (640, 480))` 对齐缓冲区尺寸。 |

## 八、参考文献
## [ov13855阅读指南](https://share.note.youdao.com/s/21chGfSW)
## [参考博客](https://blog.csdn.net/weixin_49698162?type=blog)

## 九、维护与协作
### 9.1 更新日志
v1.0.0：首发版本，完成基础功能开发，项目正式上线。
### 9.2 许可证（License）
本项目基于 MIT License 开源，可自由使用、修改、分发，需保留原作者版权声明。
Copyright (c) 2026 RK3588-HeteroVision-Hub 开发者
