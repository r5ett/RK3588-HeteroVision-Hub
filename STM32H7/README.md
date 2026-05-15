# STM32H7 高速感知节点

本节点负责高频采集 MPU6050 姿态传感器，并通过 SPI 总线高速透传至网关。

## 💎 硬核优化
* **SPI Slave DMA**：配置为 20MHz 从机模式，利用 DMA1 Stream 自动化收发。
* **D-Cache 刷新逻辑**：针对 H7 的 Cache Line 特性，实施了 `SCB_CleanDCache_by_Addr` 强制对齐刷新，防止 DMA 读取旧缓存。
* **IO 翻转提速**：SPI 相关 GPIO 引脚配置为 `VERY_HIGH` 速度等级，确保高频波形完整。
* **自愈状态机**：`while(1)` 中包含 100ms 超时心跳重发机制，解决开机启动时序死锁问题。

## 协议格式
| 偏移 | 长度 | 内容 |
| :--- | :--- | :--- |
| 0x00 | 6 字节 | MPU6050 原始加速度 (X, Y, Z) |
| 0x06 | 25 字节 | 填充位/保留位 |
| 0x1F | 1 字节 | Checksum (前 31 字节累加和) |

## 硬件连接
PF0<--->I2C2_SDA
PF1<--->I2C2_SCL
PI0<--->SPI2_NSS
PI1<--->SPI2_SCK
PI2<--->SPI2_MISO
PI3<--->SPI2_MOSI
PD6<--->SPI_INT