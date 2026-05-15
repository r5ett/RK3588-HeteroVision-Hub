#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

/*
 * 首先要sudo insmod spi_drv.ko加载驱动，确保/dev/h7_mmap设备节点存在。
 * spi_drv.c测试程序
 */
/* --- 1. 结构体与宏定义：必须与驱动程序 spi_drv.c 保持一致 --- */
#define SPI_DATA_LEN 32                   // 每一帧的长度
#define MAX_FRAMES   100                  // 环形缓冲区大小
#define PAGE_SIZE    4096                 // 映射的总大小

// 驱动定义的共享内存控制块布局
struct h7_mmap_ctrl {
    volatile uint32_t head_idx;           // 下一个要写入的索引
    volatile uint32_t update_count;       // 总接收帧数统计
    uint8_t tx_dummy[SPI_DATA_LEN];       // 填充区
    uint8_t buffer[MAX_FRAMES][SPI_DATA_LEN]; // 100帧原始数据缓冲区
};

int main() {
    int fd;
    void *map_base;
    struct h7_mmap_ctrl *ctrl;

    // 1. 打开驱动生成的杂项设备节点
    fd = open("/dev/h7_mmap", O_RDONLY);
    if (fd < 0) {
        perror("❌ 无法打开 /dev/h7_mmap");
        return -1;
    }

    // 2. 建立零拷贝内存映射
    map_base = mmap(NULL, PAGE_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    if (map_base == MAP_FAILED) {
        perror("❌ mmap 映射失败");
        close(fd);
        return -1;
    }

    ctrl = (struct h7_mmap_ctrl *)map_base;
    uint32_t last_count = 0;

    printf("🚀 RK3588 零拷贝高速解析器已启动！\n");
    printf("📊 正在解析 MPU6050 原始加速度数据 (单位: g)...\n\n");

    while (1) {
        // 3. 检查是否有新数据更新
        if (ctrl->update_count != last_count) {
            // 计算最新一帧的索引 (处理回卷逻辑)
            uint32_t idx = (ctrl->head_idx == 0) ? (MAX_FRAMES - 1) : (ctrl->head_idx - 1);
            
            /* * 4. 核心解析逻辑：
             * H7 传输的是大端字节序 (Big-Endian) 的 int16_t。
             * 数据排列: [0]高位, [1]低位 -> ACC_X
             */
            int16_t raw_ax = (int16_t)((ctrl->buffer[idx][0] << 8) | ctrl->buffer[idx][1]);
            int16_t raw_ay = (int16_t)((ctrl->buffer[idx][2] << 8) | ctrl->buffer[idx][3]);
            int16_t raw_az = (int16_t)((ctrl->buffer[idx][4] << 8) | ctrl->buffer[idx][5]);

            // 5. 转换为物理 $g$ 值 (除以灵敏度 16384)
            float ax = raw_ax / 16384.0f;
            float ay = raw_ay / 16384.0f;
            float az = raw_az / 16384.0f;

            // 6. 打印结果
            printf("\r[帧:%u] | X: %6.2fg | Y: %6.2fg | Z: %6.2fg", 
                   ctrl->update_count, ax, ay, az);
            fflush(stdout);

            last_count = ctrl->update_count;
        }
        
        // 降低 CPU 占用，10ms 检查一次即可
        usleep(10000); 
    }

    munmap(map_base, PAGE_SIZE);
    close(fd);
    return 0;
}