# 基于 RK3588 的异构多摄像头零拷贝视频处理与AI推理系统

![Platform](https://img.shields.io/badge/Platform-RK3588%20(Orange%20Pi%205%20Max)-blue)
![Language](https://img.shields.io/badge/Language-C%2FCH%2B%2B-orange)
![Framework](https://img.shields.io/badge/Framework-V4L2%20%7C%20MPP%20%7C%20RKNN-green)

## 📖 项目简介
本项目基于香橙派 5 Max (Rockchip RK3588) 平台，构建了一套端到端的低延迟视频处理 Pipeline。系统支持 USB (UVC) 与 MIPI CSI (OV13855) 摄像头的异构并发接入。通过深度结合 Linux V4L2 框架与 `DMABUF` 机制，实现了视频帧在驱动层、硬件编码器 (VPU) 与神经网络加速器 (NPU) 之间的**零拷贝 (Zero-Copy)** 数据流转，大幅降低了 CPU 占用率与端到端延迟。

## 🏗️ 引脚连接

Pin 3 (CAN0_RX_M0) -> 连接 CAN 收发器的 RXD
Pin 5 (CAN0_TX_M0) -> 连接 CAN 收发器的 TXD
Pin 11 (SPI4_MISO_M2) -> H7 的 SPI_MISO
Pin 12 (GPIO4_A6 ) -> H7 的 中断
Pin 13 (SPI4_MOSI_M2) -> H7 的 SPI_MOSI
Pin 15 (SPI4_CLK_M2 ) -> H7 的 SPI_SCK
Pin 16 (SPI4_CS0_M2 ) -> H7 的 SPI_NSS
Pin 18 (GPIO1_A4 ) -> LED
