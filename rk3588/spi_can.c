#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>


/*
 * SPI+CAN测试程序
 * 首先要
 * sudo insmod led.ko
 * sudo insmod spi_drv.ko
 * sudo ip link set can0 type can bitrate 500000
 * sudo ip link set up can0
 * 然后sudo ./spi_can 来运行这个程序
 */

/* --- 1. SPI 共享内存结构体定义 --- */
#define SPI_DATA_LEN 32
#define MAX_FRAMES   100
#define PAGE_SIZE    4096

struct h7_mmap_ctrl {
    volatile uint32_t head_idx;
    volatile uint32_t update_count;
    uint8_t tx_dummy[SPI_DATA_LEN];
    uint8_t buffer[MAX_FRAMES][SPI_DATA_LEN];
};

int main() {
    int mmap_fd = -1, can_fd = -1, led_fd = -1;
    struct h7_mmap_ctrl *ctrl = NULL;
    struct sockaddr_can addr;
    struct ifreq ifr;
    
    // 全局状态变量
    float ax = 0, ay = 0, az = 0;
    int tpms = 0, door = 0;
    int alarm_flag = 0;
    uint32_t last_spi_count = 0;
    uint32_t loop_counter = 0;

    printf("\n==================================================\n");
    printf("🚀 RK3588 异构多核网关数据融合中心 (Sensor Fusion)\n");
    printf("==================================================\n");

    printf("[SYS] 正在检测并唤醒 CAN0 网络接口...\n");
    system("sudo ip link set can0 down 2>/dev/null");
    if (system("sudo ip link set can0 up type can bitrate 500000") == 0) {
        printf("✅ CAN0 总线初始化成功 (500kbps)。\n");
    } else {
        printf("❌ CAN0 初始化失败，请检查硬件连接或 sudo 权限。\n");
    }
    
    /* --- 初始化 LED --- */
    led_fd = open("/dev/led_drv", O_WRONLY);
    if (led_fd < 0) {
        printf("⚠️ 警告: 无法打开 /dev/led_drv，警报灯物理联动将失效。\n");
    } else {
        printf("✅ 警报 LED 驱动已就绪。\n");
    }

    /* --- 初始化 SPI --- */
    mmap_fd = open("/dev/h7_mmap", O_RDONLY);
    if (mmap_fd > 0) {
        ctrl = (struct h7_mmap_ctrl *)mmap(NULL, PAGE_SIZE, PROT_READ, MAP_SHARED, mmap_fd, 0);
        if (ctrl == MAP_FAILED) {
            perror("❌ mmap 失败");
            return -1;
        }
        printf("✅ STM32H7 (SPI4) 零拷贝高速通道已就绪。\n");
    } else {
        perror("❌ 无法打开 /dev/h7_mmap");
        return -1;
    }

    /* --- 初始化 CAN --- */
    can_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (can_fd < 0) {
        perror("❌ socket 创建失败");
        return -1;
    }
    
    // 🛡️ 核心防御 1：强制设置 CAN Socket 为非阻塞模式，防止死锁溢出
    int flags = fcntl(can_fd, F_GETFL, 0);
    fcntl(can_fd, F_SETFL, flags | O_NONBLOCK);

    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, "can0");
    ioctl(can_fd, SIOCGIFINDEX, &ifr);
    
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    
    if (bind(can_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("❌ CAN 绑定失败");
        return -1;
    }
    printf("✅ STM32F103 (CAN0) 底盘控制总线已就绪。\n\n");

    /* --- 主事件循环 --- */
    while (1) {
        // [A] 读取高频 SPI 数据
        if (ctrl->update_count != last_spi_count) {
            
            // 🛡️ 核心防御 2：多核脏读隔离装甲
            uint32_t head = ctrl->head_idx;
            if (head >= MAX_FRAMES) head = 0; // 钳制指针，绝对不许越界！
            
            uint32_t idx = (head == 0) ? (MAX_FRAMES - 1) : (head - 1);
            
            int16_t raw_ax = (ctrl->buffer[idx][0] << 8) | ctrl->buffer[idx][1];
            int16_t raw_ay = (ctrl->buffer[idx][2] << 8) | ctrl->buffer[idx][3];
            int16_t raw_az = (ctrl->buffer[idx][4] << 8) | ctrl->buffer[idx][5];

            ax = raw_ax / 16384.0f;
            ay = raw_ay / 16384.0f;
            az = raw_az / 16384.0f;

            last_spi_count = ctrl->update_count;
        }

        // [B] 非阻塞读取 CAN 总线数据
        fd_set r_fds;
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000; // 10ms timeout
        
        FD_ZERO(&r_fds);
        FD_SET(can_fd, &r_fds);

        // 监听 CAN 文件描述符
        if (select(can_fd + 1, &r_fds, NULL, NULL, &tv) > 0) {
            struct can_frame frame;
            
            // 🛡️ 核心防御 3：使用更加原生的 read() 配合非阻塞，替代 recv()
            while (read(can_fd, &frame, sizeof(frame)) == sizeof(frame)) {
                switch (frame.can_id) {
                    case 0x101: 
                        door = frame.data[0]; 
                        break;
                    case 0x201: 
                        tpms = (frame.data[0] << 8) | frame.data[1]; 
                        break;
                    case 0x050: 
                        alarm_flag = frame.data[0];
                        if (alarm_flag != 0) {
                            printf("\n💥💥💥 [紧急事件] 收到底盘危险警报！触发物理联动！\n");
                            if (led_fd > 0) {
                                int ret = write(led_fd, "1", 1);
                                (void)ret; // 消除编译警告
                            }
                        } else {
                            printf("\n🛡️🛡️🛡️ [事件解除] 底盘危险已排除。\n");
                            if (led_fd > 0) {
                                int ret = write(led_fd, "0", 1);
                                (void)ret;
                            }
                        }
                        break;
                }
            }
        }

        // [C] 限制打印频率 (保护终端性能)
        loop_counter++;
        if (loop_counter % 10 == 0) {
            printf("\r[实时融合] ACC(g): %5.2f, %5.2f, %5.2f | 胎压(kPa): %3d | 车门: %s | 状态: %s   ", 
                   ax, ay, az, 
                   tpms, 
                   door ? "开" : "关", 
                   alarm_flag ? "🚨危险" : "✅正常");
            fflush(stdout);
        }
    }

    return 0;
}