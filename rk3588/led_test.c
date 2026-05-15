#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

/*
 * led_drv.c测试程序
 * sudo ./test_led 1
 * sudo ./test_led 0
 */
int main(int argc, char *argv[]) {
    // 你的驱动在 /dev 下生成的节点名（如果不是 led_drv 请自行修改）
    const char *dev_node = "/dev/led_drv"; 
    int fd;
    char val;

    // 1. 检查命令行参数数量是否正确
    // ./test_led 1 算作 2 个参数 (argv[0]是程序名, argv[1]是"1")
    if (argc != 2) {
        printf("⚠️  用法错误！\n");
        printf("正确用法: %s <0|1>\n", argv[0]);
        printf("示例:\n");
        printf("  sudo %s 1  -> 点亮 LED\n", argv[0]);
        printf("  sudo %s 0  -> 熄灭 LED\n", argv[0]);
        return -1;
    }

    // 2. 解析参数是 '0' 还是 '1'
    if (strcmp(argv[1], "1") == 0) {
        val = '1';
    } else if (strcmp(argv[1], "0") == 0) {
        val = '0';
    } else {
        printf("❌ 错误: 参数只能输入 '0' 或 '1'！\n");
        return -1;
    }

    // 3. 打开设备节点
    fd = open(dev_node, O_WRONLY);
    if (fd < 0) {
        perror("❌ 无法打开设备节点");
        printf("请检查: 1. 驱动是否已加载 (insmod)? 2. 是否加了 sudo ?\n");
        return -1;
    }

    // 4. 将字符 '0' 或 '1' 写入内核驱动
    if (write(fd, &val, 1) < 0) {
        perror("❌ 写入驱动失败");
        close(fd);
        return -1;
    }

    // 5. 打印成功信息
    if (val == '1') {
        printf("💡 LED 已经点亮 (写入了 '1')\n");
    } else {
        printf("🌑 LED 已经熄灭 (写入了 '0')\n");
    }

    // 关闭设备
    close(fd);
    return 0;
}