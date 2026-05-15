# 指定要编译的模块名（生成 led_drv.ko）
obj-m += led_drv.o
obj-m += spi_drv.o

# 你的内核源码路径
KERNELDIR := /home/pi/orangepi-build/kernel/orange-pi-6.1-rk35xx

# 当前目录
PWD := $(shell pwd)

# 交叉编译器路径
CROSS_COMPILE := /home/pi/orangepi-build/toolchains/gcc-arm-11.2-2022.02-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-
# 架构
ARCH := arm64

# 定义应用层程序的交叉编译器 (在 CROSS_COMPILE 后面加上 gcc)
CC := $(CROSS_COMPILE)gcc

# 默认目标：同时编译 modules (内核驱动) 和 app (测试程序)
all: modules app

# 编译内核模块
modules:
	$(MAKE) -C $(KERNELDIR) \
	ARCH=$(ARCH) \
	CROSS_COMPILE=$(CROSS_COMPILE) \
	M=$(PWD) modules

# 编译测试程序
app:
	$(CC) led_test.c -o led_test
	$(CC) can_test.c -o can_test
	$(CC) spi_test.c -o spi_test
	$(CC) spi_can.c -o spi_can

# 清理生成的文件
clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f led_test can_test spi_test spi_can