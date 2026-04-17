# 基于 RK3588 的异构多摄像头零拷贝视频处理与AI推理系统

![Platform](https://img.shields.io/badge/Platform-RK3588%20(Orange%20Pi%205%20Max)-blue)
![Language](https://img.shields.io/badge/Language-C%2FCH%2B%2B-orange)
![Framework](https://img.shields.io/badge/Framework-V4L2%20%7C%20MPP%20%7C%20RKNN-green)

## 📖 项目简介
本项目基于香橙派 5 Max (Rockchip RK3588) 平台，构建了一套端到端的低延迟视频处理 Pipeline。系统支持 USB (UVC) 与 MIPI CSI (OV13855) 摄像头的异构并发接入。通过深度结合 Linux V4L2 框架与 `DMABUF` 机制，实现了视频帧在驱动层、硬件编码器 (VPU) 与神经网络加速器 (NPU) 之间的**零拷贝 (Zero-Copy)** 数据流转，大幅降低了 CPU 占用率与端到端延迟。

## 🏗️ 系统架构图

```text
                                +----------------------------------+
[MIPI Camera] --(V4L2/rkisp)--> |                                  | --(Zero-Copy)--> [ VPU (H.264/H.265 硬编) ] -> 本地存储/推流
                                |       DMABUF (物理内存共享)      |
[USB Camera ] ----(V4L2)------> |                                  | --(Zero-Copy)--> [ NPU (RKNN AI 推理) ]    -> 结果渲染/输出
                                +----------------------------------+
                                                 ^
                                                 | (控制与同步)
                                          [ 多线程调度模块 ]