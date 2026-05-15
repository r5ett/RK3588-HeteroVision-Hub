#include <linux/module.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/gpio/consumer.h> // 🔑 引入现代的 gpiod 子系统
#include <linux/miscdevice.h>    // 🔑 引入杂项设备，自动创建 /dev 节点
#include <linux/of.h>

// 保存 GPIO 描述符的指针
static struct gpio_desc *warning_led_gpio;

/* ================== 用户态控制接口 ================== */
static ssize_t alarm_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    char val;
    // 从用户空间 (C++ 引擎) 复制指令
    if (copy_from_user(&val, buf, 1)) {
        return -EFAULT;
    }

    // 根据指令控制 GPIO (彻底抛弃 ioremap，使用高层 API)
    if (val == '1') {
        gpiod_set_value(warning_led_gpio, 1); // 亮灯
    } else if (val == '0') {
        gpiod_set_value(warning_led_gpio, 0); // 灭灯
    }

    return count;
}

// 绑定文件操作
static const struct file_operations alarm_fops = {
    .owner = THIS_MODULE,
    .write = alarm_write,
};

// 定义杂项设备 (内核会自动帮我们在 /dev 下创建 led_drv 节点)
static struct miscdevice alarm_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "led_drv", // 暴露给 C++ 的节点名: /dev/led_drv
    .fops  = &alarm_fops,
};

/* ================== 平台驱动 Probe 与 Remove ================== */
static int alarm_probe(struct platform_device *pdev) {
    int ret;

    printk(KERN_INFO "LED Driver Probing...\n");

    // 🔑 核心升级：通过设备树自动获取 GPIO 控制权，告别硬件地址硬编码！
    // 这里的 "warning" 对应设备树里的 "warning-gpios"
    warning_led_gpio = devm_gpiod_get(&pdev->dev, "warning", GPIOD_OUT_LOW);
    if (IS_ERR(warning_led_gpio)) {
        printk(KERN_ERR "Failed to get warning GPIO from device tree\n");
        return PTR_ERR(warning_led_gpio);
    }

    // 注册杂项设备，生成 /dev/led_drv
    ret = misc_register(&alarm_misc);
    if (ret) {
        printk(KERN_ERR "Failed to register misc device\n");
        return ret;
    }

    printk(KERN_INFO "LED Driver Probe Success! /dev/led_drv created.\n");
    return 0;
}

static int alarm_remove(struct platform_device *pdev) {
    // 注销杂项设备 (gpiod_get 使用了 devm_ 前缀，内核会自动释放 GPIO，无需手动 free)
    misc_deregister(&alarm_misc);
    printk(KERN_INFO "LED Driver Removed.\n");
    return 0;
}

/* ================== 设备树匹配表 ================== */
static const struct of_device_id alarm_of_match[] = {
    { .compatible = "r5ett,led_drv" }, 
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, alarm_of_match);

static struct platform_driver alarm_platform_driver = {
    .probe  = alarm_probe,
    .remove = alarm_remove,
    .driver = {
        .name = "led_driver",
        .of_match_table = alarm_of_match,
    },
};

module_platform_driver(alarm_platform_driver); // 一键注册宏，替代 init 和 exit

MODULE_LICENSE("GPL");
MODULE_AUTHOR("R5ETT");
MODULE_DESCRIPTION("RK3588 LED Driver using modern GPIOD and Misc Device");