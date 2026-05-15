#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>     // 用于打开 LED 节点
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>

/*
 * 使用前先
 * sudo ip link set can0 type can bitrate 500000
 * sudo ip link set up can0
 * 然后sudo ./can_test 来运行这个程序
 */
int main() {
    int s, led_fd;
    struct sockaddr_can addr;
    struct ifreq ifr;
    struct can_frame frame;

    // 1. 尝试打开 LED 驱动节点
    led_fd = open("/dev/led_drv", O_WRONLY);
    if (led_fd < 0) {
        printf("⚠️ 警告: 无法打开 /dev/led_drv，警报灯联动功能将失效。\n");
        printf("   (请确保已经执行 sudo insmod led_drv.ko)\n\n");
    } else {
        printf("✅ LED 驱动已就绪，等待总线警报...\n\n");
    }

    // 2. 初始化 SocketCAN
    s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    strcpy(ifr.ifr_name, "can0");
    ioctl(s, SIOCGIFINDEX, &ifr);
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    bind(s, (struct sockaddr *)&addr, sizeof(addr));

    printf("📡 开始监听 CAN0 总线数据...\n");

    while (1) {
        int nbytes = read(s, &frame, sizeof(struct can_frame));
        if (nbytes < 0) break;

        switch (frame.can_id) {
            case 0x101: {
                if (frame.data[0] == 1) printf("🚨 [ID:101] 收到常规刹车信号\n");
                break;
            }
            case 0x201: {
                int speed_raw = (frame.data[0] << 8) | frame.data[1];
                printf("🏎️ [ID:201] 实时车速: %.1f km/h\n", speed_raw / 10.0);
                break;
            }
            // 👇 这里是我们新加的按键紧急警报解析
            case 0x050: {
                // 如果第一个字节是 0xFF，说明警报触发
                if (frame.data[0] == 0xFF) {
                    printf("\n💥💥💥 [ID:050] 接收到极端危险紧急警报！(Data: FF FF...) \n");
                    if (led_fd > 0) write(led_fd, "1", 1); // 瞬间点亮物理 LED 灯
                } 
                // 如果第一个字节是 0x00，说明警报解除
                else if (frame.data[0] == 0x00) {
                    printf("\n🛡️🛡️🛡️ [ID:050] 危险已解除。(Data: 00 00...) \n\n");
                    if (led_fd > 0) write(led_fd, "0", 1); // 熄灭物理 LED 灯
                }
                break;
            }
            default:
                break;
        }
    }

    close(s);
    if (led_fd > 0) close(led_fd);
    return 0;
}