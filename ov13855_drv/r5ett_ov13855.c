#include <linux/clk.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/compat.h>
#include <linux/rk-camera-module.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include <linux/pinctrl/consumer.h>
#include "otp_eeprom.h"

#define DRIVER_VERSION			KERNEL_VERSION(0, 0x01, 0x04)

#ifndef V4L2_CID_DIGITAL_GAIN
#define V4L2_CID_DIGITAL_GAIN		V4L2_CID_GAIN
#endif

#define OV13855_LINK_FREQ_540MHZ	540000000U
#define OV13855_LINK_FREQ_270MHZ	270000000U
/* pixel rate = link frequency * 2 * lanes / BITS_PER_SAMPLE */
#define OV13855_PIXEL_RATE		(OV13855_LINK_FREQ_540MHZ * 2LL * 4LL / 10LL)
#define OV13855_XVCLK_FREQ		24000000

#define CHIP_ID				0x00d855
#define OV13855_REG_CHIP_ID		0x300a

#define OV13855_REG_CTRL_MODE		0x0100
#define OV13855_MODE_SW_STANDBY		0x0
#define OV13855_MODE_STREAMING		BIT(0)

#define OV13855_REG_EXPOSURE		0x3500
#define	OV13855_EXPOSURE_MIN		4
#define	OV13855_EXPOSURE_STEP		1
#define OV13855_VTS_MAX			0x7fff

#define OV13855_REG_GAIN_H		0x3508
#define OV13855_REG_GAIN_L		0x3509
#define OV13855_GAIN_H_MASK		0x1f
#define OV13855_GAIN_H_SHIFT		8
#define OV13855_GAIN_L_MASK		0xff
#define OV13855_GAIN_MIN		0x80
#define OV13855_GAIN_MAX		0x7c0
#define OV13855_GAIN_STEP		1
#define OV13855_GAIN_DEFAULT		0x80

#define OV13855_REG_TEST_PATTERN	0x5e00
#define	OV13855_TEST_PATTERN_ENABLE	0x80
#define	OV13855_TEST_PATTERN_DISABLE	0x0

#define OV13855_REG_VTS			0x380e

#define REG_NULL			0xFFFF

#define OV13855_REG_VALUE_08BIT		1
#define OV13855_REG_VALUE_16BIT		2
#define OV13855_REG_VALUE_24BIT		3

#define OV13855_LANES			4
#define OV13855_BITS_PER_SAMPLE		10

#define OV13855_CHIP_REVISION_REG	0x302A

#define OF_CAMERA_PINCTRL_STATE_DEFAULT	"rockchip,camera_default"
#define OF_CAMERA_PINCTRL_STATE_SLEEP	"rockchip,camera_sleep"

#define OV13855_NAME			"ov13855"
#define OV13855_MEDIA_BUS_FMT		MEDIA_BUS_FMT_SBGGR10_1X10

static const char * const ov13855_supply_names[] = {
	"avdd",		/* 模拟电源 */
	"dovdd",	/* IO电源 */
	"dvdd",		/* 数字核心电源 */
};

#define OV13855_NUM_SUPPLIES ARRAY_SIZE(ov13855_supply_names) // = 3

/* 寄存器值结构体 */
struct regval {
	u16 addr;
	u8 val;
};

/* 模式定义结构体 */
struct ov13855_mode {
	u32 width;
	u32 height;
	struct v4l2_fract max_fps;
	u32 hts_def;
	u32 vts_def;
	u32 exp_def;
	u32 link_freq_idx;
	u32 bpp;
	const struct regval *reg_list;
};

/* 设备私有数据结构体 */
	struct ov13855 {
		// === I2C设备身份相关 ===
		struct i2c_client	*client;// I2C客户端对象，用于硬件通信

		// === 硬件资源管理 ===
		struct clk		*xvclk;// 外部时钟（24MHz）
		struct gpio_desc	*power_gpio;// 电源控制GPIO
		struct gpio_desc	*reset_gpio;// 复位控制GPIO
		struct gpio_desc	*pwdn_gpio; // 掉电控制GPIO
		struct regulator_bulk_data supplies[OV13855_NUM_SUPPLIES];// 电源调节器阵列

		// === 引脚控制 ===
		struct pinctrl		*pinctrl;// 引脚控制器
		struct pinctrl_state	*pins_default;// 默认引脚状态
		struct pinctrl_state	*pins_sleep;  // 休眠引脚状态

		// === V4L2子设备身份相关 ===
		struct v4l2_subdev	subdev;// V4L2子设备对象
		struct media_pad	pad;// 媒体实体的连接垫片
		struct v4l2_ctrl_handler ctrl_handler;// V4L2控制参数处理器
		struct v4l2_ctrl	*exposure;// 曝光控制
		struct v4l2_ctrl	*anal_gain;// 模拟增益控制
		struct v4l2_ctrl	*digi_gain;// 数字增益控制
		struct v4l2_ctrl	*hblank;// 水平消隐控制
		struct v4l2_ctrl	*vblank;// 垂直消隐控制
		struct v4l2_ctrl	*pixel_rate;// 像素速率控制
		struct v4l2_ctrl	*link_freq;// 链路频率控制
		struct v4l2_ctrl	*test_pattern;// 测试图案控制

		// === 运行状态管理 ===
		struct mutex		mutex;// 并发保护锁
		bool			streaming;// 数据流状态标志
		bool			power_on; // 电源状态标志

		// === 配置参数管理 ===
		const struct ov13855_mode *cur_mode;// 当前工作模式
		u32			module_index;// 模块索引号
		const char		*module_facing;// 模块朝向（前置/后置）
		const char		*module_name;// 模块名称
		const char		*len_name;// 镜头名称

		struct otp_info		*otp;
	};

/*
 * sd是指向v4l2_subdev成员的指针
 * struct ov13855是包含该成员的结构体类型
 * subdev是该成员在结构体中的名称
 * container_of宏会计算出包含这个成员的结构体实例的起始地址
 */
#define to_ov13855(sd) container_of(sd, struct ov13855, subdev)

/*
 * Xclk 24Mhz
 */
static const struct regval ov13855_global_regs[] = {
	{0x0103, 0x01},
	{0x0300, 0x02},
	{0x0301, 0x00},
	{0x0302, 0x5a},
	{0x0303, 0x00},
	{0x0304, 0x00},
	{0x0305, 0x01},
	{0x030b, 0x06},
	{0x030c, 0x02},
	{0x030d, 0x88},
	{0x0312, 0x11},
	{0x3022, 0x01},
	{0x3013, 0x32},
	{0x3016, 0x72},
	{0x301b, 0xF0},
	{0x301f, 0xd0},
	{0x3106, 0x15},
	{0x3107, 0x23},
	{0x3500, 0x00},
	{0x3501, 0x80},
	{0x3502, 0x00},
	{0x3508, 0x02},
	{0x3509, 0x00},
	{0x350a, 0x00},
	{0x350e, 0x00},
	{0x3510, 0x00},
	{0x3511, 0x02},
	{0x3512, 0x00},
	{0x3600, 0x2b},
	{0x3601, 0x52},
	{0x3602, 0x60},
	{0x3612, 0x05},
	{0x3613, 0xa4},
	{0x3620, 0x80},
	{0x3621, 0x10},
	{0x3622, 0x30},
	{0x3624, 0x1c},
	{0x3640, 0x10},
	{0x3641, 0x70},
	{0x3661, 0x80},
	{0x3662, 0x12},
	{0x3664, 0x73},
	{0x3665, 0xa7},
	{0x366e, 0xff},
	{0x366f, 0xf4},
	{0x3674, 0x00},
	{0x3679, 0x0c},
	{0x367f, 0x01},
	{0x3680, 0x0c},
	{0x3681, 0x50},
	{0x3682, 0x50},
	{0x3683, 0xa9},
	{0x3684, 0xa9},
	{0x3709, 0x5f},
	{0x3714, 0x24},
	{0x371a, 0x3e},
	{0x3737, 0x04},
	{0x3738, 0xcc},
	{0x3739, 0x12},
	{0x373d, 0x26},
	{0x3764, 0x20},
	{0x3765, 0x20},
	{0x37a1, 0x36},
	{0x37a8, 0x3b},
	{0x37ab, 0x31},
	{0x37c2, 0x04},
	{0x37c3, 0xf1},
	{0x37c5, 0x00},
	{0x37d8, 0x03},
	{0x37d9, 0x0c},
	{0x37da, 0xc2},
	{0x37dc, 0x02},
	{0x37e0, 0x00},
	{0x37e1, 0x0a},
	{0x37e2, 0x14},
	{0x37e3, 0x04},
	{0x37e4, 0x2a},
	{0x37e5, 0x03},
	{0x37e6, 0x04},
	{0x3800, 0x00},
	{0x3801, 0x00},
	{0x3802, 0x00},
	{0x3803, 0x08},
	{0x3804, 0x10},
	{0x3805, 0x9f},
	{0x3806, 0x0c},
	{0x3807, 0x57},
	{0x3808, 0x10},
	{0x3809, 0x80},
	{0x380a, 0x0c},
	{0x380b, 0x40},
	{0x380c, 0x04},
	{0x380d, 0x62},
	{0x380e, 0x0c},
	{0x380f, 0x8e},
	{0x3811, 0x10},
	{0x3813, 0x08},
	{0x3814, 0x01},
	{0x3815, 0x01},
	{0x3816, 0x01},
	{0x3817, 0x01},
	{0x3820, 0xa8},
	{0x3821, 0x00},
	{0x3822, 0xc2},
	{0x3823, 0x18},
	{0x3826, 0x11},
	{0x3827, 0x1c},
	{0x3829, 0x03},
	{0x3832, 0x00},
	{0x3c80, 0x00},
	{0x3c87, 0x01},
	{0x3c8c, 0x19},
	{0x3c8d, 0x1c},
	{0x3c90, 0x00},
	{0x3c91, 0x00},
	{0x3c92, 0x00},
	{0x3c93, 0x00},
	{0x3c94, 0x40},
	{0x3c95, 0x54},
	{0x3c96, 0x34},
	{0x3c97, 0x04},
	{0x3c98, 0x00},
	{0x3d8c, 0x73},
	{0x3d8d, 0xc0},
	{0x3f00, 0x0b},
	{0x3f03, 0x00},
	{0x4001, 0xe0},
	{0x4008, 0x00},
	{0x4009, 0x0f},
	{0x4011, 0xf0},
	{0x4050, 0x04},
	{0x4051, 0x0b},
	{0x4052, 0x00},
	{0x4053, 0x80},
	{0x4054, 0x00},
	{0x4055, 0x80},
	{0x4056, 0x00},
	{0x4057, 0x80},
	{0x4058, 0x00},
	{0x4059, 0x80},
	{0x405e, 0x00},
	{0x4500, 0x07},
	{0x4503, 0x00},
	{0x450a, 0x04},
	{0x4809, 0x04},
	{0x480c, 0x12},
	{0x481f, 0x30},
	{0x4833, 0x10},
	{0x4837, 0x0e},
	{0x4902, 0x01},
	{0x4d00, 0x03},
	{0x4d01, 0xc9},
	{0x4d02, 0xbc},
	{0x4d03, 0xd7},
	{0x4d04, 0xf0},
	{0x4d05, 0xa2},
	{0x5000, 0xff},
	{0x5001, 0x07},
	{0x5040, 0x39},
	{0x5041, 0x10},
	{0x5042, 0x10},
	{0x5043, 0x84},
	{0x5044, 0x62},
	{0x5180, 0x00},
	{0x5181, 0x10},
	{0x5182, 0x02},
	{0x5183, 0x0f},
	{0x5200, 0x1b},
	{0x520b, 0x07},
	{0x520c, 0x0f},
	{0x5300, 0x04},
	{0x5301, 0x0C},
	{0x5302, 0x0C},
	{0x5303, 0x0f},
	{0x5304, 0x00},
	{0x5305, 0x70},
	{0x5306, 0x00},
	{0x5307, 0x80},
	{0x5308, 0x00},
	{0x5309, 0xa5},
	{0x530a, 0x00},
	{0x530b, 0xd3},
	{0x530c, 0x00},
	{0x530d, 0xf0},
	{0x530e, 0x01},
	{0x530f, 0x10},
	{0x5310, 0x01},
	{0x5311, 0x20},
	{0x5312, 0x01},
	{0x5313, 0x20},
	{0x5314, 0x01},
	{0x5315, 0x20},
	{0x5316, 0x08},
	{0x5317, 0x08},
	{0x5318, 0x10},
	{0x5319, 0x88},
	{0x531a, 0x88},
	{0x531b, 0xa9},
	{0x531c, 0xaa},
	{0x531d, 0x0a},
	{0x5405, 0x02},
	{0x5406, 0x67},
	{0x5407, 0x01},
	{0x5408, 0x4a},
	{REG_NULL, 0x00},
};

/*
 * Xclk 24Mhz
 * max_framerate 30fps
 * mipi_datarate per lane 1080Mbps
 */
static const struct regval ov13855_4224x3136_30fps_regs[] = {
	{0x0300, 0x02},
	{0x0301, 0x00},
	{0x0302, 0x5a},
	{0x0303, 0x00},
	{0x0304, 0x00},
	{0x0305, 0x01},
	{0x030b, 0x06},
	{0x030c, 0x02},
	{0x030d, 0x88},
	{0x0312, 0x11},
	{0x3022, 0x01},
	{0x3012, 0x40},
	{0x3013, 0x72},
	{0x3016, 0x72},
	{0x301b, 0xF0},
	{0x301f, 0xd0},
	{0x3106, 0x15},
	{0x3107, 0x23},
	{0x3500, 0x00},
	{0x3501, 0x80},
	{0x3502, 0x00},
	{0x3508, 0x02},
	{0x3509, 0x00},
	{0x350a, 0x00},
	{0x350e, 0x00},
	{0x3510, 0x00},
	{0x3511, 0x02},
	{0x3512, 0x00},
	{0x3600, 0x2b},
	{0x3601, 0x52},
	{0x3602, 0x60},
	{0x3612, 0x05},
	{0x3613, 0xa4},
	{0x3620, 0x80},
	{0x3621, 0x10},
	{0x3622, 0x30},
	{0x3624, 0x1c},
	{0x3640, 0x10},
	{0x3641, 0x70},
	{0x3660, 0x04},
	{0x3661, 0x80},
	{0x3662, 0x12},
	{0x3664, 0x73},
	{0x3665, 0xa7},
	{0x366e, 0xff},
	{0x366f, 0xf4},
	{0x3674, 0x00},
	{0x3679, 0x0c},
	{0x367f, 0x01},
	{0x3680, 0x0c},
	{0x3681, 0x50},
	{0x3682, 0x50},
	{0x3683, 0xa9},
	{0x3684, 0xa9},
	{0x3706, 0x40},
	{0x3709, 0x5f},
	{0x3714, 0x24},
	{0x371a, 0x3e},
	{0x3737, 0x04},
	{0x3738, 0xcc},
	{0x3739, 0x12},
	{0x373d, 0x26},
	{0x3764, 0x20},
	{0x3765, 0x20},
	{0x37a1, 0x36},
	{0x37a8, 0x3b},
	{0x37ab, 0x31},
	{0x37c2, 0x04},
	{0x37c3, 0xf1},
	{0x37c5, 0x00},
	{0x37d8, 0x03},
	{0x37d9, 0x0c},
	{0x37da, 0xc2},
	{0x37dc, 0x02},
	{0x37e0, 0x00},
	{0x37e1, 0x0a},
	{0x37e2, 0x14},
	{0x37e3, 0x04},
	{0x37e4, 0x2A},
	{0x37e5, 0x03},
	{0x37e6, 0x04},
	{0x3800, 0x00},
	{0x3801, 0x00},
	{0x3802, 0x00},
	{0x3803, 0x08},
	{0x3804, 0x10},
	{0x3805, 0x9f},
	{0x3806, 0x0c},
	{0x3807, 0x57},
	{0x3808, 0x10},
	{0x3809, 0x80},
	{0x380a, 0x0c},
	{0x380b, 0x40},
	{0x380c, 0x04},
	{0x380d, 0x62},
	{0x380e, 0x0c},
	{0x380f, 0x8e},
	{0x3811, 0x10},
	{0x3813, 0x08},
	{0x3814, 0x01},
	{0x3815, 0x01},
	{0x3816, 0x01},
	{0x3817, 0x01},
	{0x3820, 0xa8},
	{0x3821, 0x00},
	{0x3822, 0xd2},
	{0x3823, 0x18},
	{0x3826, 0x11},
	{0x3827, 0x1c},
	{0x3829, 0x03},
	{0x3832, 0x00},
	{0x3c80, 0x00},
	{0x3c87, 0x01},
	{0x3c8c, 0x19},
	{0x3c8d, 0x1c},
	{0x3c90, 0x00},
	{0x3c91, 0x00},
	{0x3c92, 0x00},
	{0x3c93, 0x00},
	{0x3c94, 0x40},
	{0x3c95, 0x54},
	{0x3c96, 0x34},
	{0x3c97, 0x04},
	{0x3c98, 0x00},
	{0x3d8c, 0x73},
	{0x3d8d, 0xc0},
	{0x3f00, 0x0b},
	{0x3f03, 0x00},
	{0x4001, 0xe0},
	{0x4008, 0x00},
	{0x4009, 0x0f},
	{0x4011, 0xf0},
	{0x4017, 0x08},
	{0x4050, 0x04},
	{0x4051, 0x0b},
	{0x4052, 0x00},
	{0x4053, 0x80},
	{0x4054, 0x00},
	{0x4055, 0x80},
	{0x4056, 0x00},
	{0x4057, 0x80},
	{0x4058, 0x00},
	{0x4059, 0x80},
	{0x405e, 0x00},
	{0x4500, 0x07},
	{0x4503, 0x00},
	{0x450a, 0x04},
	{0x4800, 0x60},
	{0x4809, 0x04},
	{0x480c, 0x12},
	{0x481f, 0x30},
	{0x4833, 0x10},
	{0x4837, 0x0e},
	{0x4902, 0x01},
	{0x4d00, 0x03},
	{0x4d01, 0xc9},
	{0x4d02, 0xbc},
	{0x4d03, 0xd7},
	{0x4d04, 0xf0},
	{0x4d05, 0xa2},
	{0x5000, 0xff},
	{0x5001, 0x07},
	{0x5040, 0x39},
	{0x5041, 0x10},
	{0x5042, 0x10},
	{0x5043, 0x84},
	{0x5044, 0x62},
	{0x5180, 0x00},
	{0x5181, 0x10},
	{0x5182, 0x02},
	{0x5183, 0x0f},
	{0x5200, 0x1b},
	{0x520b, 0x07},
	{0x520c, 0x0f},
	{0x5300, 0x04},
	{0x5301, 0x0C},
	{0x5302, 0x0C},
	{0x5303, 0x0f},
	{0x5304, 0x00},
	{0x5305, 0x70},
	{0x5306, 0x00},
	{0x5307, 0x80},
	{0x5308, 0x00},
	{0x5309, 0xa5},
	{0x530a, 0x00},
	{0x530b, 0xd3},
	{0x530c, 0x00},
	{0x530d, 0xf0},
	{0x530e, 0x01},
	{0x530f, 0x10},
	{0x5310, 0x01},
	{0x5311, 0x20},
	{0x5312, 0x01},
	{0x5313, 0x20},
	{0x5314, 0x01},
	{0x5315, 0x20},
	{0x5316, 0x08},
	{0x5317, 0x08},
	{0x5318, 0x10},
	{0x5319, 0x88},
	{0x531a, 0x88},
	{0x531b, 0xa9},
	{0x531c, 0xaa},
	{0x531d, 0x0a},
	{0x5405, 0x02},
	{0x5406, 0x67},
	{0x5407, 0x01},
	{0x5408, 0x4a},
	{0x0100, 0x01},
	{0x0100, 0x00},
	{0x380c, 0x04},
	{0x380d, 0x62},
	{0x0303, 0x00},
	{0x4837, 0x0e},
	//{0x0100, 0x01},
	{REG_NULL, 0x00},
};

/* 支持的模式 */
static const struct ov13855_mode supported_modes[] = {
	{
		.width = 4224,
		.height = 3136,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x0800,
		.hts_def = 0x0462,
		.vts_def = 0x0c8e,
		.bpp = 10,
		.reg_list = ov13855_4224x3136_30fps_regs,
		.link_freq_idx = 0,
	},
#ifdef DEBUG
	{
		.width = 2112,
		.height = 1568,
		.max_fps = {
			.numerator = 10000,
			.denominator = 600000,
		},
		.exp_def = 0x0400,
		.hts_def = 0x0462,
		.vts_def = 0x0c89,
		.bpp = 10,
		.reg_list = ov13855_2112x1568_60fps_regs,
		.link_freq_idx = 1,
	},
	{
		.width = 4224,
		.height = 3136,
		.max_fps = {
			.numerator = 10000,
			.denominator = 150000,
		},
		.exp_def = 0x0800,
		.hts_def = 0x08c4,
		.vts_def = 0x0c8e,
		.bpp = 10,
		.reg_list = ov13855_4224x3136_15fps_regs,
		.link_freq_idx = 0,
	},
#endif
};

/* 链路频率列表，单位为Hz */
static const s64 link_freq_items[] = {
	OV13855_LINK_FREQ_540MHZ,
	OV13855_LINK_FREQ_270MHZ,
};

/* 测试图案菜单选项 */
static const char * const ov13855_test_pattern_menu[] = {
	"Disabled",
	"Vertical Color Bar Type 1",
	"Vertical Color Bar Type 2",
	"Vertical Color Bar Type 3",
	"Vertical Color Bar Type 4"
};

/* 
 * 写入寄存器
 * client：I2C设备
 * reg：寄存器地址
 * len：寄存器值的字节长度（1-4）
 * val：要写入寄存器的值
 */
static int ov13855_write_reg(struct i2c_client *client, u16 reg,
			     u32 len, u32 val)
{
	u32 buf_i, val_i;
	u8 buf[6];
	u8 *val_p;
	__be32 val_be;

	dev_dbg(&client->dev, "write reg(0x%x val:0x%x)!\n", reg, val);

	if (len > 4)
		return -EINVAL;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	val_be = cpu_to_be32(val);
	val_p = (u8 *)&val_be;
	buf_i = 2;
	val_i = 4 - len;

	while (val_i < 4)
		buf[buf_i++] = val_p[val_i++];

	if (i2c_master_send(client, buf, len + 2) != len + 2)
		return -EIO;

	return 0;
}

/*
 * 批量写入寄存器
 * client：I2C设备
 * regs：寄存器地址和值的数组
 */
static int ov13855_write_array(struct i2c_client *client,
			       const struct regval *regs)
{
	u32 i;
	int ret = 0;

	for (i = 0; ret == 0 && regs[i].addr != REG_NULL; i++)
		ret = ov13855_write_reg(client, regs[i].addr,
					OV13855_REG_VALUE_08BIT,
					regs[i].val);

	return ret;
}

/*
 * 读取寄存器
 * client：I2C设备
 * reg：寄存器地址
 * len：要读取的字节长度（1-4）
 * val：指向存储读取值的变量的指针
 */
static int ov13855_read_reg(struct i2c_client *client, u16 reg,
			    unsigned int len, u32 *val)
{
	struct i2c_msg msgs[2];
	u8 *data_be_p;
	__be32 data_be = 0;
	__be16 reg_addr_be = cpu_to_be16(reg);
	int ret;

	if (len > 4 || !len)
		return -EINVAL;

	data_be_p = (u8 *)&data_be;
	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 2;
	msgs[0].buf = (u8 *)&reg_addr_be;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_be_p[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*val = be32_to_cpu(data_be);

	return 0;
}

/*
 * 计算模式与请求的帧格式之间的分辨率距离
 * mode：当前模式
 * framefmt：请求的帧格式
 * 返回值：分辨率距离，数值越小表示越接近
 */
static int ov13855_get_reso_dist(const struct ov13855_mode *mode,
				 struct v4l2_mbus_framefmt *framefmt)
{
	return abs(mode->width - framefmt->width) +
	       abs(mode->height - framefmt->height);
}

/*
 * 查找与请求的帧格式最匹配的模式
 * fmt：请求的帧格式
 * 返回值：指向最佳匹配模式的指针
 */
static const struct ov13855_mode *
ov13855_find_best_fit(struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *framefmt = &fmt->format;
	int dist;
	int cur_best_fit = 0;
	int cur_best_fit_dist = -1;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(supported_modes); i++) {
		dist = ov13855_get_reso_dist(&supported_modes[i], framefmt);
		if (cur_best_fit_dist == -1 || dist < cur_best_fit_dist) {
			cur_best_fit_dist = dist;
			cur_best_fit = i;
		}
	}

	return &supported_modes[cur_best_fit];
}

/*
 * 作用：设置（或试探设置）传感器的输出分辨率与图像格式。
 * 入参：
 * sd       - V4L2 子设备指针
 * sd_state - 子设备内部状态上下文（用于 TRY 模式保存数据）
 * fmt      - 包含了应用层【期望】设置的分辨率和格式，函数执行后会被【真实生效】的值覆盖并带回。
 */
static int ov13855_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	struct ov13855 *ov13855 = to_ov13855(sd);
	const struct ov13855_mode *mode;
	s64 h_blank, vblank_def;
	u64 pixel_rate = 0;
	u32 lane_num = OV13855_LANES;

	mutex_lock(&ov13855->mutex);

	/* 
     * 核心逻辑 1：硬件限制妥协（Best Fit 匹配）
     * 假设用户态传入了宽高为 1920x1080 的请求，但 Sensor 的 supported_modes 数组里
     * 并没有这个分辨率。ov13855_find_best_fit 函数会遍历数组，通过计算差值，
     * 找到一个与要求最接近的【硬件原生支持模式】（比如 2112x1568），并返回该模式的指针。
     */
	mode = ov13855_find_best_fit(fmt);
	/* 
     * 核心逻辑 2：强制覆写返回结构体
     * 无论应用层刚才要求的是什么乱七八糟的格式，现在全部强行覆写为硬件能支持的真实参数。
     * 这体现了 V4L2 的“协商机制”：应用层下发期望值，驱动将其修改为实际值并回传。
     */
	fmt->format.code = OV13855_MEDIA_BUS_FMT;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	// 核心逻辑 3：沙盒模式 (TRY) vs 真实生效模式 (ACTIVE)
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		/*
         * 【沙盒模式】：
         * 仅仅是将刚才协商好（纠正过）的 fmt->format 存入 sd_state 这个纯软件沙盒中。
         * 不更新底层的 cur_mode，也不修改底层的任何控制时序。
         */
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		*v4l2_subdev_get_try_format(sd, sd_state, fmt->pad) = fmt->format;
#else
		mutex_unlock(&ov13855->mutex);
		return -ENOTTY;
#endif
	} 
	else {
		/*
         * 【真实生效模式】：
         * 协商完成，真正要改变硬件物理运行状态了！
         */
        
        /* 1. 更新大管家中的全局状态指针，使其指向新的分辨率配置 */
		ov13855->cur_mode = mode;
		/* 2. 重新计算当前新分辨率下的水平消隐区 (HBLANK) */
		h_blank = mode->hts_def - mode->width;
		/*
         * 核心细节：__v4l2_ctrl_modify_range 
         * 因为分辨率变了（即 width 和 height 变了），图像的几何时序发生了根本性变化。
         * 必须通过这个函数，动态修改此前我们初始化的 HBLANK 滑动条的极值和默认值。
         * 注意这里的 `__` 双下划线前缀，表示这是一个“无锁调用”，因为外层已经加过 mutex_lock 了。
         */
		__v4l2_ctrl_modify_range(ov13855->hblank, h_blank,
					 h_blank, 1, h_blank);
		/* 3. 重新计算当前新分辨率下的垂直消隐区 (VBLANK) 并修改控制器范围 */
		vblank_def = mode->vts_def - mode->height;
		__v4l2_ctrl_modify_range(ov13855->vblank, vblank_def,
					 OV13855_VTS_MAX - mode->height,
					 1, vblank_def);
		/* 将 VBLANK 设为当前模式的默认值 */
		__v4l2_ctrl_s_ctrl(ov13855->vblank, vblank_def);
		/* 4. 重新计算并设置底层链路的像素传输率 (Pixel Rate) */
		pixel_rate = (u32)link_freq_items[mode->link_freq_idx] / mode->bpp * 2 * lane_num;

		__v4l2_ctrl_s_ctrl_int64(ov13855->pixel_rate,
					 pixel_rate);
		/* 5. 同步底层 MIPI 时钟频率索引 */
		__v4l2_ctrl_s_ctrl(ov13855->link_freq,
				   mode->link_freq_idx);
	}
	dev_info(&ov13855->client->dev, "%s: mode->link_freq_idx(%d)",
		 __func__, mode->link_freq_idx);

	mutex_unlock(&ov13855->mutex);

	return 0;
}

/*
 * 作用：获取传感器当前配置的图像格式（分辨率、像素格式、扫描场方式等）。
 * 入参：
 * sd       - V4L2 子设备指针
 * sd_state - 子设备内部状态上下文（专门用来存储 TRY 模式下的虚拟状态）
 * fmt      - 指向 v4l2_subdev_format 结构体的指针，包含了查询要求，并用于带回查询结果
 * 返回值：0 表示成功获取；-ENOTTY 表示不支持该操作。
 */
static int ov13855_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct ov13855 *ov13855 = to_ov13855(sd);
	const struct ov13855_mode *mode = ov13855->cur_mode;

	mutex_lock(&ov13855->mutex);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		/* 
		 * 从 sd_state 这个纯软件的“沙盒”上下文中，提取出之前用户试探设置的虚拟格式，
         * 并原封不动地返回给调用者。此时完全不看底层的 mode 变量。
         */
		fmt->format = *v4l2_subdev_get_try_format(sd, sd_state, fmt->pad);
#else
		// 如果没开启该宏，说明不支持 TRY 机制，直接解锁并返回错误码 -ENOTTY
		mutex_unlock(&ov13855->mutex);
		return -ENOTTY;
#endif
	} 
	else {
		/* 填入真实的宽度，取自大管家当前挂载的 mode */
        fmt->format.width = mode->width;
        /* 填入真实的高度 */
        fmt->format.height = mode->height;
        /* 填入写死的物理总线像素格式（10位 RAW） */
        fmt->format.code = OV13855_MEDIA_BUS_FMT;
        /* * 填入扫描场类型：V4L2_FIELD_NONE。
         * 这代表传感器使用的是“逐行扫描（Progressive）”，而不是老式电视的“隔行扫描（Interlaced）”。
         */
        fmt->format.field = V4L2_FIELD_NONE;
	}
	mutex_unlock(&ov13855->mutex);

	return 0;
}

/*
 * 作用：枚举（列出）传感器支持的媒体总线像素格式。
 * 入参：
 * sd       - V4L2 子设备指针
 * sd_state - 子设备的内部状态（通常用于保存试探性的格式，这里没用到）
 * code     - 指向 v4l2_subdev_mbus_code_enum 结构体的指针。
 * 调用者会传入一个 index（索引），要求驱动填入该索引对应的具体格式代码 (code)。
 * 返回值：0 表示成功填入；-EINVAL 表示索引越界（没有更多格式了）。
 */
static int ov13855_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index != 0)
		return -EINVAL;
	/* 
	 * 核心逻辑：填入格式数据
     * 将写死的唯一一种媒体总线格式，填入返回结构体中。
     * 如果你去看源码的最前面，OV13855_MEDIA_BUS_FMT 被宏定义为 MEDIA_BUS_FMT_SBGGR10_1X10。
     * 这代表：这是一张 10 位的 RAW Bayer (BGGR排列) 图像，每个像素占 10 bit。
     */
	code->code = OV13855_MEDIA_BUS_FMT;

	return 0;
}

/*
 * 作用：枚举当前 Sensor 支持的全部图像分辨率（帧大小）。
 * 入参：
 * sd       - V4L2 子设备指针
 * sd_state - 子设备内部状态
 * fse      - 指向 v4l2_subdev_frame_size_enum 结构体的指针。
 * 调用者会填入想要查询的 index（索引）和 code（媒体总线格式），
 * 驱动需要把对应的宽、高边界值填入该结构体并返回。
 * 返回值：0 表示成功；-EINVAL 表示索引越界或不支持的格式。
 */
static int ov13855_enum_frame_sizes(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != OV13855_MEDIA_BUS_FMT)
		return -EINVAL;

	/* 
	 * 核心逻辑：填充分辨率数据
     * 根据传入的 index，从 supported_modes 数组中提取宽和高，填充给调用者。
     */
	fse->min_width  = supported_modes[fse->index].width;
	fse->max_width  = supported_modes[fse->index].width;
	fse->max_height = supported_modes[fse->index].height;
	fse->min_height = supported_modes[fse->index].height;

	return 0;
}

static int ov13855_enable_test_pattern(struct ov13855 *ov13855, u32 pattern)
{
	u32 val;

	if (pattern)
		val = (pattern - 1) | OV13855_TEST_PATTERN_ENABLE;
	else
		val = OV13855_TEST_PATTERN_DISABLE;

	return ov13855_write_reg(ov13855->client,
				 OV13855_REG_TEST_PATTERN,
				 OV13855_REG_VALUE_08BIT,
				 val);
}

/*
 * 作用：获取当前传感器模式下的帧间隔（即帧率的倒数）。
 * 入参：
 * sd - V4L2 子设备指针
 * fi - 指向 v4l2_subdev_frame_interval 结构体的指针，用于将查询结果带回给用户空间
 * 返回值：0 表示查询成功。
 */
static int ov13855_g_frame_interval(struct v4l2_subdev *sd,
				    struct v4l2_subdev_frame_interval *fi)
{
	struct ov13855 *ov13855 = to_ov13855(sd);
	const struct ov13855_mode *mode = ov13855->cur_mode;

	fi->interval = mode->max_fps;

	return 0;
}

static void ov13855_get_otp(struct otp_info *otp,
			    struct rkmodule_inf *inf)
{
	u32 i, j;
	u32 w, h;

	/* awb */
	if (otp->awb_data.flag) {
		inf->awb.flag = 1;
		inf->awb.r_value = otp->awb_data.r_ratio;
		inf->awb.b_value = otp->awb_data.b_ratio;
		inf->awb.gr_value = otp->awb_data.g_ratio;
		inf->awb.gb_value = 0x0;

		inf->awb.golden_r_value = otp->awb_data.r_golden;
		inf->awb.golden_b_value = otp->awb_data.b_golden;
		inf->awb.golden_gr_value = otp->awb_data.g_golden;
		inf->awb.golden_gb_value = 0x0;
	}

	/* lsc */
	if (otp->lsc_data.flag) {
		inf->lsc.flag = 1;
		inf->lsc.width = otp->basic_data.size.width;
		inf->lsc.height = otp->basic_data.size.height;
		inf->lsc.table_size = otp->lsc_data.table_size;

		for (i = 0; i < 289; i++) {
			inf->lsc.lsc_r[i] = (otp->lsc_data.data[i * 2] << 8) |
					     otp->lsc_data.data[i * 2 + 1];
			inf->lsc.lsc_gr[i] = (otp->lsc_data.data[i * 2 + 578] << 8) |
					      otp->lsc_data.data[i * 2 + 579];
			inf->lsc.lsc_gb[i] = (otp->lsc_data.data[i * 2 + 1156] << 8) |
					      otp->lsc_data.data[i * 2 + 1157];
			inf->lsc.lsc_b[i] = (otp->lsc_data.data[i * 2 + 1734] << 8) |
					     otp->lsc_data.data[i * 2 + 1735];
		}
	}

	/* pdaf */
	if (otp->pdaf_data.flag) {
		inf->pdaf.flag = 1;
		inf->pdaf.gainmap_width = otp->pdaf_data.gainmap_width;
		inf->pdaf.gainmap_height = otp->pdaf_data.gainmap_height;
		inf->pdaf.pd_offset = otp->pdaf_data.pd_offset;
		inf->pdaf.dcc_mode = otp->pdaf_data.dcc_mode;
		inf->pdaf.dcc_dir = otp->pdaf_data.dcc_dir;
		inf->pdaf.dccmap_width = otp->pdaf_data.dccmap_width;
		inf->pdaf.dccmap_height = otp->pdaf_data.dccmap_height;
		w = otp->pdaf_data.gainmap_width;
		h = otp->pdaf_data.gainmap_height;
		for (i = 0; i < h; i++) {
			for (j = 0; j < w; j++) {
				inf->pdaf.gainmap[i * w + j] =
					(otp->pdaf_data.gainmap[(i * w + j) * 2] << 8) |
					otp->pdaf_data.gainmap[(i * w + j) * 2 + 1];
			}
		}
		w = otp->pdaf_data.dccmap_width;
		h = otp->pdaf_data.dccmap_height;
		for (i = 0; i < h; i++) {
			for (j = 0; j < w; j++) {
				inf->pdaf.dccmap[i * w + j] =
					(otp->pdaf_data.dccmap[(i * w + j) * 2] << 8) |
					otp->pdaf_data.dccmap[(i * w + j) * 2 + 1];
			}
		}
	}

	/* af */
	if (otp->af_data.flag) {
		inf->af.flag = 1;
		inf->af.dir_cnt = 1;
		inf->af.af_otp[0].vcm_start = otp->af_data.af_inf;
		inf->af.af_otp[0].vcm_end = otp->af_data.af_macro;
		inf->af.af_otp[0].vcm_dir = 0;
	}
}

static void ov13855_get_module_inf(struct ov13855 *ov13855,
				   struct rkmodule_inf *inf)
{
	struct otp_info *otp = ov13855->otp;

	memset(inf, 0, sizeof(*inf));
	strscpy(inf->base.sensor, OV13855_NAME, sizeof(inf->base.sensor));
	strscpy(inf->base.module, ov13855->module_name,
		sizeof(inf->base.module));
	strscpy(inf->base.lens, ov13855->len_name, sizeof(inf->base.lens));
	if (otp)
		ov13855_get_otp(otp, inf);
}

/*
 * 作用：处理针对该 V4L2 子设备的私有 IOCTL 控制命令。
 * 入参：
 * sd  - V4L2 子设备指针
 * cmd - 用户空间下发的 IOCTL 命令字
 * arg - 伴随命令传递的参数指针（可能是传入的数据，也可能是用于带回数据的空 buffer）
 * 返回值：0 表示执行成功；-ENOIOCTLCMD 表示不支持该命令。
 */
static long ov13855_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct ov13855 *ov13855 = to_ov13855(sd);
	long ret = 0;
	u32 stream = 0;

	switch (cmd) {
	/* 
	 * 命令一：获取模组信息 (RKMODULE_GET_MODULE_INFO)
     * 核心意义：这就是我们之前讲 EEPROM 代码时的“最终去向”！
     */
	case RKMODULE_GET_MODULE_INFO:
		/* 
		 * 将泛型指针 arg 强转为 Rockchip 专用的结构体指针。
         * ov13855_get_module_inf 函数会把设备树里的朝向(facing)、模组名，
         * 以及我们之前辛辛苦苦从 EEPROM 里读出来的 OTP 校准数据，
         * 全部拷贝到这个 arg 指向的内存里，然后返回给用户空间的 ISP 算法库。
         */
		ov13855_get_module_inf(ov13855, (struct rkmodule_inf *)arg);
		break;
	/* 
	 * 	命令二：快速流控制 (RKMODULE_SET_QUICK_STREAM)
     * 核心意义：绕过 V4L2 标准的 s_stream 繁琐流程，直接在最底层暴力开关 Sensor 的图像输出。
     */
	case RKMODULE_SET_QUICK_STREAM:

		stream = *((u32 *)arg);

		if (stream)
			/* 
			 * 如果要求开流：
             * 直接通过 I2C 向寄存器 OV13855_REG_CTRL_MODE 写入 OV13855_MODE_STREAMING (通常是 0x01)。
             * 硬件收到此命令后，Sensor 的 MIPI 物理层立刻开始向外吐出图像数据。
             */
			ret = ov13855_write_reg(ov13855->client,
				 OV13855_REG_CTRL_MODE,
				 OV13855_REG_VALUE_08BIT,
				 OV13855_MODE_STREAMING);
		else
			/* 
			 * 如果要求关流：
             * 向同一个寄存器写入 OV13855_MODE_SW_STANDBY (Software Standby，软件待机模式，通常是 0x00)。
             * 硬件立刻停止吐图，并进入极低功耗状态，但保持 I2C 通信不断。
             */
			ret = ov13855_write_reg(ov13855->client,
				 OV13855_REG_CTRL_MODE,
				 OV13855_REG_VALUE_08BIT,
				 OV13855_MODE_SW_STANDBY);
		break;
	// 默认分支：未知的/不支持的命令
	default:
		/* 
		 * 返回内核标准的错误码 -ENOIOCTLCMD (Error No IOCTL Command)。
         * V4L2 核心层收到这个错误码后，会知道这个私有驱动不处理该命令，
         * 然后内核可能会尝试走其他标准的处理路径，或者直接把错误抛给用户态。
         */
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT // 只有在编译内核时开启了“32位程序兼容支持”才会编译这段代码
/*
 * 作用：处理 32 位用户态程序发给 64 位内核的 V4L2 子设备 IOCTL 命令。
 * 入参：
 * sd  - V4L2 子设备指针
 * cmd - IOCTL 命令字
 * arg - 32位程序传来的 32位长度的参数（通常是一个 32 位的内存地址）
 */
static long ov13855_compat_ioctl32(struct v4l2_subdev *sd,
				   unsigned int cmd, unsigned long arg)
{
	/* 
	 * 核心翻译动作 1：
     * compat_ptr() 是内核提供的神仙宏。它安全地将 32 位的用户空间指针（arg），
     * 扩展并转换为 64 位内核能够安全访问的 __user 指针（up）。
     */
	void __user *up = compat_ptr(arg);
	struct rkmodule_inf *inf;
	struct rkmodule_awb_cfg *cfg;
	long ret = 0;
	u32 stream = 0;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		/* 
		 * 核心翻译动作 2：不能直接把转换后的指针丢给底层！
         * 必须先在 64 位的内核空间里，老老实实按 64 位的大小分配一块干净的内存 (kzalloc)。
         */
		inf = kzalloc(sizeof(*inf), GFP_KERNEL);
		if (!inf) {
			ret = -ENOMEM;
			return ret;
		}

		/* 
		 * 调用真正干活的 64 位 ioctl 函数！
         * 注意：这里传进去的是内核刚才自己分配的 64 位结构体指针 (inf)。
         * 底层函数毫无察觉，开心地把 OTP 等数据填满了这个结构体。
         */
		ret = ov13855_ioctl(sd, cmd, inf);
		if (!ret) {
			/* 
			 * 核心翻译动作 3：安全拷贝过桥！
             * 数据填好后，使用 copy_to_user 将内核空间的数据，
             * 安全地拷贝回 32 位用户程序指定的地址 (up) 中去。
             */
			ret = copy_to_user(up, inf, sizeof(*inf));
			if (ret)
				ret = -EFAULT;
		}
		kfree(inf);
		break;
	case RKMODULE_AWB_CFG:
		/* 
		 * 处理方向相反的场景（用户把配置传给内核）：
         * 同样先在内核分配内存。
         */
		cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
		if (!cfg) {
			ret = -ENOMEM;
			return ret;
		}
		// 先用 copy_from_user 把 32 位程序的数据安全地拉到内核空间的 64 位结构体里
		ret = copy_from_user(cfg, up, sizeof(*cfg));
		if (!ret)
		  	// 转换完成后，再丢给真正的底层函数去处理
			ret = ov13855_ioctl(sd, cmd, cfg);
		else
			ret = -EFAULT;
		kfree(cfg);
		break;
	case RKMODULE_SET_QUICK_STREAM:
		// 处理基础类型：仅仅是一个 32 位的整数，直接拷过来就行，不需要 kzalloc 分配结构体
		ret = copy_from_user(&stream, up, sizeof(u32));
		if (!ret)
			ret = ov13855_ioctl(sd, cmd, &stream);
		else
			ret = -EFAULT;
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}
#endif

static int __ov13855_start_stream(struct ov13855 *ov13855)
{
	int ret;

	ret = ov13855_write_array(ov13855->client, ov13855->cur_mode->reg_list);
	if (ret)
		return ret;

	/* In case these controls are set before streaming */
	mutex_unlock(&ov13855->mutex);
	ret = v4l2_ctrl_handler_setup(&ov13855->ctrl_handler);
	mutex_lock(&ov13855->mutex);
	if (ret)
		return ret;

	return ov13855_write_reg(ov13855->client,
				 OV13855_REG_CTRL_MODE,
				 OV13855_REG_VALUE_08BIT,
				 OV13855_MODE_STREAMING);
}

static int __ov13855_stop_stream(struct ov13855 *ov13855)
{
	return ov13855_write_reg(ov13855->client,
				 OV13855_REG_CTRL_MODE,
				 OV13855_REG_VALUE_08BIT,
				 OV13855_MODE_SW_STANDBY);
}

/*
 * 启动数据流时的大量I2C写操作  
 * 开流流程（on=1）：
 * 1. 获取锁 (mutex_lock)
 * 2. 规范化参数 (on = !!on)
 * 3. 检查状态去重
 *    └─ 已经开流? → 直接返回
 * 4. 上电 (pm_runtime_get_sync)
 *    ├─ 调用 ov13855_runtime_resume
 *    └─ 执行 __ov13855_power_on
 *        ├─ 使能GPIO电源
 *        ├─ 启动24MHz时钟
 *        ├─ 使能regulator
 *        └─ 释放复位信号
 * 5. 启动流 (__ov13855_start_stream)
 *    ├─ 写入模式寄存器（分辨率、帧率）
 *    ├─ 应用控制器值（曝光、增益）
 *    └─ 写streaming寄存器0x01
 * 6. 更新状态 (streaming = 1)
 * 7. 释放锁返回
 *    ↓
 * [sensor开始输出MIPI数据]
 * 
 * 关流流程（on=0）：
 * 1. 获取锁
 * 2. 规范化参数
 * 3. 检查状态去重
 *    └─ 已经关流? → 直接返回
 * 4. 停止流 (__ov13855_stop_stream)
 *    └─ 写streaming寄存器0x00
 * 5. 下电 (pm_runtime_put)
 *    ├─ 降低使用计数
 *    └─ 计数=0时调用 ov13855_runtime_suspend
 *        └─ 执行 __ov13855_power_off
 *            ├─ 关闭regulator
 *            ├─ 停止时钟
 *            └─ 拉低电源GPIO
 * 6. 更新状态 (streaming = 0)
 * 7. 释放锁返回
 *    ↓
 * [sensor完全关闭，最低功耗]
 */
static int ov13855_s_stream(struct v4l2_subdev *sd, int on)
{
	// 已知条件：
	// 1. sd是V4L2框架传递的子设备指针
	// 2. on表示启动(1)或停止(0)数据流
	// 3. 我们需要通过I2C配置OV13855的流控制寄存器

	// 获取完整的驱动上下文
	struct ov13855 *ov13855 = to_ov13855(sd);
	// 获取I2C客户端指针，后续所有寄存器读写都通过它进行
	struct i2c_client *client = ov13855->client;
	int ret = 0;

	dev_info(&client->dev, "%s: on: %d, %dx%d@%d\n", __func__, on,
				ov13855->cur_mode->width,
				ov13855->cur_mode->height,
		DIV_ROUND_CLOSEST(ov13855->cur_mode->max_fps.denominator,
				  ov13855->cur_mode->max_fps.numerator));

	// 使用互斥锁保护并发访问
	mutex_lock(&ov13855->mutex);
	on = !!on;
	// 检查当前数据流状态，避免重复操作
	if (on == ov13855->streaming)
		goto unlock_and_return;

	if (on) {
		// 运行时电源管理：同步上电
		// 1. 增加设备使用计数
		// 2. 如果设备处于suspend状态，唤醒设备
		// 3. 调用ov13855_runtime_resume → __ov13855_power_on
		//    - 使能GPIO电源
		//    - 启动24MHz外部时钟
		//    - 按时序使能regulator电源
		//    - 释放复位信号
		// 4. 等待设备完全上电后返回

		// 启动数据流：向OV13855写入流控制寄存器
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			// 上电失败（可能是电源异常、时钟失败等）
			// 使用put_noidle：降低计数但不触发suspend
			// 原因：设备根本没成功上电，不能执行下电操作
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		// 启动数据流输出
		// 1. 写入当前模式的寄存器配置（分辨率、帧率、像素格式等）
		// 2. 应用控制器的值（曝光时间、增益等参数）
		// 3. 向control mode寄存器写0x01，sensor开始输出MIPI信号
		ret = __ov13855_start_stream(ov13855);
		if (ret) {
			// 启动流失败（I2C通信错误、寄存器配置错误等）
			v4l2_err(sd, "start stream failed while write regs\n");
			// 使用put：降低使用计数，允许PM框架正常关闭电源
			// 因为设备已经上电成功，需要执行完整的下电流程
			pm_runtime_put(&client->dev);
			goto unlock_and_return;
		}
	} 
	// ===== 关流分支 =====
	else {
		// 停止数据流输出
		// 向control mode寄存器写0x00，sensor进入standby模式
		// 硬件效果：停止输出MIPI信号，像素阵列停止工作，进入低功耗
		__ov13855_stop_stream(ov13855);
		// 运行时电源管理：允许下电
		// 1. 降低设备使用计数
		// 2. 如果计数降为0，PM框架调用ov13855_runtime_suspend
		// 3. 执行__ov13855_power_off：
		//    - 关闭regulator电源
		//    - 停止外部时钟
		//    - 拉低电源GPIO
		// 4. sensor进入最低功耗状态
		pm_runtime_put(&client->dev);
	}

	// 更新streaming状态标志
	// 用于：1) 下次调用时的状态去重判断
	//      2) 其他函数读取当前状态（如suspend流程）
	ov13855->streaming = on;// 更新内部状态

unlock_and_return:
	mutex_unlock(&ov13855->mutex);

	return ret;
}

/*
 * 作用：V4L2 子设备 core 操作集的回调函数，用于控制 Sensor 供电状态。
 * 入参：sd - 指向 V4L2 子设备的指针。
 * on - 非 0 表示请求上电，0 表示请求断电。
 * 返回值：0 表示成功；负数错误码表示失败。
 */
static int ov13855_s_power(struct v4l2_subdev *sd, int on)
{
	struct ov13855 *ov13855 = to_ov13855(sd);
	struct i2c_client *client = ov13855->client;
	int ret = 0;

	mutex_lock(&ov13855->mutex);

	/* If the power state is not modified - no work to do. */
	if (ov13855->power_on == !!on)
		goto unlock_and_return;

	// 如果请求上电
	if (on) {
		/* 
		 * 核心步骤 1：触发 Linux 运行时电源管理 (Runtime PM)。
         * 这句代码告诉 PM 核心：“我现在需要使用这个 I2C 设备，请把它唤醒”。
         * PM 核心会同步调用该驱动注册的 ov13855_runtime_resume 回调，
         * 那里才是真正操作 GPIO（给复位引脚通电）、使能 LDO 电源和开启 24MHz 时钟的地方。
         */
		ret = pm_runtime_get_sync(&client->dev);

		// 如果底层硬件通电失败（比如时钟没给上），进行错误处理
		if (ret < 0) {
			/* 
			 * 恢复引用计数，且不触发 idle 回调。
             * 因为 get_sync 失败了，我们需要把增加的引用计数减回去，保持引用平衡。
             */
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}
		
		/* 
		 * 核心步骤 2：硬件刚通完电，内部寄存器处于出厂混沌状态。
         * 此时必须通过 I2C 写入 ov13855_global_regs 数组（全局基础寄存器表）。
         * 这相当于给传感器刷入“底层固件”，让它的内部 PLL、ADC、数字核心初始化就绪。
         */
		ret = ov13855_write_array(ov13855->client, ov13855_global_regs);
		if (ret) {
			v4l2_err(sd, "could not set init registers\n");
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		// 软件状态同步：标记当前设备已完全就绪并通电
		ov13855->power_on = true;
	} 
	// 如果请求断电
	else {
		/* 
		 * 递减 Runtime PM 引用计数。
         * 当引用计数归零时，Linux 电源管理核心会自动调度调用 ov13855_runtime_suspend，
         * 从而真正地关闭时钟、切断电源、拉低休眠引脚，实现硬件层面的省电。
         */
		pm_runtime_put(&client->dev);
		// 软件状态同步：标记当前设备已断电
		ov13855->power_on = false;
	}

unlock_and_return:
	mutex_unlock(&ov13855->mutex);

	return ret;
}

/* Calculate the delay in us by clock rate and clock cycles */
static inline u32 ov13855_cal_delay(u32 cycles)
{
	return DIV_ROUND_UP(cycles, OV13855_XVCLK_FREQ / 1000 / 1000);
}

/*
 * 上电时序的物理过程：
 * 主电源使能 → LDO输出电压
 * 时钟启动 → 24MHz方波输出
 * 电源稳定 → AVDD/DOVDD/DVDD按序上电
 * 释放复位 → sensor内部开始初始化
 * 延时等待 → 内部电路稳定，I2C控制器就绪
 */
static int __ov13855_power_on(struct ov13855 *ov13855)
{
	int ret;
	u32 delay_us;
	struct device *dev = &ov13855->client->dev;

	// 1. 使能主电源GPIO
	if (!IS_ERR(ov13855->power_gpio))
		gpiod_set_value_cansleep(ov13855->power_gpio, 1);

	usleep_range(1000, 2000);

	// 2. 切换pinctrl到工作状态
	if (!IS_ERR_OR_NULL(ov13855->pins_default)) {
		ret = pinctrl_select_state(ov13855->pinctrl,
					   ov13855->pins_default);
		if (ret < 0)
			dev_err(dev, "could not set pins\n");
	}

	// 3. 设置并启动外部时钟
	ret = clk_set_rate(ov13855->xvclk, OV13855_XVCLK_FREQ);
	if (ret < 0)
		dev_warn(dev, "Failed to set xvclk rate (24MHz)\n");
	if (clk_get_rate(ov13855->xvclk) != OV13855_XVCLK_FREQ)
		dev_warn(dev, "xvclk mismatched, modes are based on 24MHz\n");
	ret = clk_prepare_enable(ov13855->xvclk);
	if (ret < 0) {
		dev_err(dev, "Failed to enable xvclk\n");
		return ret;
	}

	// 4. 释放复位信号(拉低)
	if (!IS_ERR(ov13855->reset_gpio))
		gpiod_set_value_cansleep(ov13855->reset_gpio, 0);

	// 5. 使能regulator电源
	ret = regulator_bulk_enable(OV13855_NUM_SUPPLIES, ov13855->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators\n");
		goto disable_clk;
	}

	// 6. 拉高复位信号，sensor开始工作
	if (!IS_ERR(ov13855->reset_gpio))
		gpiod_set_value_cansleep(ov13855->reset_gpio, 1);

	usleep_range(5000, 6000);

	// 7. 退出掉电模式
	if (!IS_ERR(ov13855->pwdn_gpio))
		gpiod_set_value_cansleep(ov13855->pwdn_gpio, 1);

	/* 8192 cycles prior to first SCCB transaction */
	// 8. 等待sensor内部稳定(8192时钟周期)
	delay_us = ov13855_cal_delay(8192);
	usleep_range(delay_us * 2, delay_us * 3);

	return 0;

disable_clk:
	clk_disable_unprepare(ov13855->xvclk);

	return ret;
}

/*
 * 下电
 */
static void __ov13855_power_off(struct ov13855 *ov13855)
{
	int ret;
	struct device *dev = &ov13855->client->dev;

	if (!IS_ERR(ov13855->pwdn_gpio))
		gpiod_set_value_cansleep(ov13855->pwdn_gpio, 0);
	clk_disable_unprepare(ov13855->xvclk);
	if (!IS_ERR(ov13855->reset_gpio))
		gpiod_set_value_cansleep(ov13855->reset_gpio, 0);

	if (!IS_ERR_OR_NULL(ov13855->pins_sleep)) {
		ret = pinctrl_select_state(ov13855->pinctrl,
					   ov13855->pins_sleep);
		if (ret < 0)
			dev_dbg(dev, "could not set pins\n");
	}
	if (!IS_ERR(ov13855->power_gpio))
		gpiod_set_value_cansleep(ov13855->power_gpio, 0);

	regulator_bulk_disable(OV13855_NUM_SUPPLIES, ov13855->supplies);
}

/*
 * 作用：执行设备从低功耗状态恢复到工作状态的具体硬件上电操作。
 * 宏 __maybe_unused：告诉编译器忽略“函数已定义但未使用”的警告（防止内核未开启 PM 选项时报错）。
 * 入参：dev - 指向内核中最底层的、通用的设备抽象层结构体。
 * 返回值：0 表示上电成功；负数表示上电失败。
 */
static int __maybe_unused ov13855_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov13855 *ov13855 = to_ov13855(sd);

	return __ov13855_power_on(ov13855);
}

/*
 * 作用：执行设备进入低功耗状态的具体硬件下电操作。
 * 宏 __maybe_unused：告诉编译器“如果内核编译时关掉了电源管理选项，这个函数就算没被调用，也别给我报警告”。
 * 入参：dev - 指向内核中最底层的、通用的设备抽象层结构体。
 */
static int __maybe_unused ov13855_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov13855 *ov13855 = to_ov13855(sd);

	__ov13855_power_off(ov13855);

	return 0;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API // 必须在开启 Subdev API（拥有独立设备节点）时才编译
/*
 * 作用：处理 /dev/v4l-subdevX 节点的打开事件，初始化文件句柄级别的 TRY 状态。
 * 入参：
 * sd - V4L2 子设备指针
 * fh - V4L2 子设备文件句柄 (File Handle)。每个 open 该设备节点的进程，
 * 内核都会为其分配一个独立的文件句柄，其中包含了该进程专属的沙盒状态 (fh->state)。
 * 返回值：0 表示打开并初始化成功。
 */
static int ov13855_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct ov13855 *ov13855 = to_ov13855(sd);
	/* 
	 * 核心获取：拿到当前进程专属的 TRY 格式存储区。
     * 从文件句柄的 state 中，获取第 0 号 pad (端口) 的 TRY 格式指针。
     */
	struct v4l2_mbus_framefmt *try_fmt =
				v4l2_subdev_get_try_format(sd, fh->state, 0);
	
	/* 获取驱动默认的初始模式（通常是 supported_modes 数组的第 0 项，即最大分辨率） */
	const struct ov13855_mode *def_mode = &supported_modes[0];

	mutex_lock(&ov13855->mutex);
	/* Initialize try_fmt */
	/* 
	 * 核心动作：初始化 try_fmt (布置沙盒)
     * 为什么要做这一步？
     * 因为如果用户打开设备节点后，什么都没设置，直接调 ioctl 去 get_fmt(TRY)，
     * 如果这里不赋初始值，用户拿到的就会是内存里的随机垃圾数据或全 0 数据。
     * 这里将沙盒里的数据初始化为传感器的“最高规格默认值”。
     */
	try_fmt->width = def_mode->width;
	try_fmt->height = def_mode->height;
	try_fmt->code = OV13855_MEDIA_BUS_FMT;
	try_fmt->field = V4L2_FIELD_NONE;

	mutex_unlock(&ov13855->mutex);
	/* No crop or compose */

	return 0;
}
#endif

/*
 * 作用：枚举当前 Sensor 支持的帧间隔（即帧率）。
 * 入参：
 * sd       - V4L2 子设备指针
 * sd_state - 子设备内部状态
 * fie      - 指向 v4l2_subdev_frame_interval_enum 结构体的指针。
 * 调用者通常会传入指定的 width、height 和 code，想要查询在这个分辨率下支持的帧率。
 * 返回值：0 表示成功填入数据；-EINVAL 表示索引越界。
 */
static int ov13855_enum_frame_interval(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *sd_state,
				       struct v4l2_subdev_frame_interval_enum *fie)
{
	if (fie->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	/* 
	 * 核心逻辑：强行覆盖与填入数据
     * 注意：V4L2 官方的规范其实是希望调用者提供宽、高和格式，然后驱动去匹配。
     * 但这里的写法非常“粗暴且直接”，它不管你传进来想问什么尺寸，
     * 它直接把你传进来的结构体里的数据，用 supported_modes 数组里的配置给“强行覆盖”掉。
     */
	fie->code = OV13855_MEDIA_BUS_FMT;
	fie->width = supported_modes[fie->index].width;
	fie->height = supported_modes[fie->index].height;
	/* 
	 * 核心逻辑：上报最大帧率（帧间隔）
     * 把静态数组里写死的 max_fps（比如 4224x3136 对应的 30fps，即 1/30）交差。
     */
	fie->interval = supported_modes[fie->index].max_fps;

	return 0;
}

/*
 * 作用：获取传感器硬件层面的媒体总线（物理链路）配置。
 * 入参：
 * sd     - V4L2 子设备指针
 * pad    - 端口号（通常是 0，即 Sensor 的输出端口）
 * config - 指向 v4l2_mbus_config 结构体的指针，用于带回底层的物理总线配置信息
 * 返回值：0 表示成功。
 */
static int ov13855_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
				struct v4l2_mbus_config *config)
{
	/* * 核心配置 1：指定总线类型与物理层协议
     * V4L2_MBUS_CSI2_DPHY 代表这是 MIPI CSI-2 协议，且底层物理层是 D-PHY。
     * 摄像头总线通常有老式的并行口 (DVP/BT656) 和高速的串行口 (MIPI)。
     * 这里明确告诉主控：“我是高速 MIPI 接口”。
     */
	config->type = V4L2_MBUS_CSI2_DPHY;
	/* * 核心配置 2：指定 MIPI 数据通道（Lane）数量
     * MIPI 接口通过增加物理线（Lane）的数量来成倍提升数据传输带宽。
     * OV13855_LANES 通常被宏定义为 4。
     * 这里告诉主控：“我接下来会火力全开，同时用 4 对差分数据线（Data Lanes）给你发数据”。
     */
	config->bus.mipi_csi2.num_data_lanes = OV13855_LANES;

	return 0;
}

/*
 * 作用：获取 V4L2 子设备的选区信息（通常用于查询支持的裁剪边界）。
 * 入参：
 * sd       - V4L2 子设备指针
 * sd_state - 子设备内部状态上下文
 * sel      - 指向 v4l2_subdev_selection 结构体的指针。
 * 调用者会在 sel->target 中指定想要查询的“目标类型”，
 * 驱动需要将对应的矩形区域（x, y, width, height）填入 sel->r 中返回。
 * 返回值：0 表示成功；-EINVAL 表示不支持该类型的查询。
 */
static int ov13855_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	struct ov13855 *ov13855 = to_ov13855(sd);

	/* 
	 *  核心逻辑 1：判断查询目标 (Target)
     * V4L2_SEL_TGT_CROP_BOUNDS 代表“裁剪的绝对物理边界”。
     * 应用程序通过这个 target 来询问：“我最多能在多大的一块画布上进行裁剪？”
     */
	if (sel->target == V4L2_SEL_TGT_CROP_BOUNDS) {
		/* 
		 * 核心逻辑 2：填充矩形数据 (Rectangle)
         * 因为 Sensor 只是纯粹地输出当前模式配置的画面，
         * 所以它的最大裁剪边界，就是它当前正在运行的分辨率模式（cur_mode）的宽高。
         */
		sel->r.left = 0;                              // 矩形左上角 X 坐标为 0
        sel->r.width = ov13855->cur_mode->width;      // 矩形宽度为当前模式的有效像素宽度
        sel->r.top = 0;                               // 矩形左上角 Y 坐标为 0
        sel->r.height = ov13855->cur_mode->height;    // 矩形高度为当前模式的有效像素高度
		return 0;
	}

	return -EINVAL;
}

/*
 * 电源管理
 */
static const struct dev_pm_ops ov13855_pm_ops = {
	SET_RUNTIME_PM_OPS(ov13855_runtime_suspend,/* 当设备进入空闲状态时调用，用于降低功耗 */
			   ov13855_runtime_resume, NULL)/* 当设备重新被访问时调用，用于恢复摄像头的电源和时钟，重新使其工作 */
};

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
/*
 * 定义一个类型为 v4l2_subdev_internal_ops 的静态常量结构体。
 * 为什么叫 "internal_ops" (内部操作集)？
 * 因为 V4L2 还有 core_ops, video_ops, pad_ops 等，那些是用来控制纯硬件行为的。
 * 而 internal_ops 是专门用来处理“设备节点本身的文件操作”（如 open, close）的。
 */
static const struct v4l2_subdev_internal_ops ov13855_internal_ops = {
	/* 
	 * 核心绑定：
     * 将框架标准的 .open 回调函数指针，指向我们刚才精读过的那个 
     * 会分配“私人沙盒 (TRY state)”的 ov13855_open 函数。
     */
	.open = ov13855_open,
};
#endif

/*
 * 定义核心操作，涵盖子设备的一些通用功能
 */
static const struct v4l2_subdev_core_ops ov13855_core_ops = {
	/* 用于控制摄像头传感器的电源。根据参数 on，可以开启或关闭摄像头电源（或复位） */
	.s_power = ov13855_s_power,
	/* 用于处理应用程序通过 ioctl 系统调用发送的自定义命令。它可以扩展标准的 V4L2 控制功能，处理额外的摄像头配置操作。 */
	.ioctl = ov13855_ioctl,
#ifdef CONFIG_COMPAT
	/* 用于 32-bit 用户空间应用和 64-bit 内核之间的兼容处理，确保在 64-bit 系统上可以正常处理 32-bit 应用的 ioctl 请求。 */
	.compat_ioctl32 = ov13855_compat_ioctl32,
#endif
};

/*
 * 定义视频相关的操作，主要用于处理摄像头传感器的视频流和帧控制
 */
static const struct v4l2_subdev_video_ops ov13855_video_ops = {
	/* 用于开启或关闭视频流。当这个函数被调用时，摄像头开始传输图像数据或者停止传输 */
	.s_stream = ov13855_s_stream,
	/* 用于获取视频流的帧间隔（帧率）。这可以帮助上层应用了解摄像头的输出帧率。 */
	.g_frame_interval = ov13855_g_frame_interval,
};

/*
 * 用于配置视频数据的传输格式、分辨率和相关参数的操作集。此类操作允许配置摄像头传感器的输出格式和尺寸
 */
static const struct v4l2_subdev_pad_ops ov13855_pad_ops = {
	/* 枚举支持的媒体总线格式（比如 Bayer 格式的 RAW 数据类型）。这有助于应用程序知道摄像头支持哪些数据格式 */
	.enum_mbus_code = ov13855_enum_mbus_code,
	/* 枚举摄像头支持的帧尺寸（分辨率），比如 4208x3120 */
	.enum_frame_size = ov13855_enum_frame_sizes,
	/* 枚举支持的帧间隔（帧率） */
	.enum_frame_interval = ov13855_enum_frame_interval,
	/* 获取当前的帧格式（如分辨率和数据类型） */
	.get_fmt = ov13855_get_fmt,
	/* 设置帧格式，通常包括分辨率、数据类型等信息 */
	.set_fmt = ov13855_set_fmt,
	/* 获取当前的图像裁剪窗口或其他选择区域的参数 */
	.get_selection = ov13855_get_selection,
	/* 获取媒体总线配置，比如 MIPI CSI-2 的通道配置、数据速率等 */
	.get_mbus_config = ov13855_g_mbus_config,
};

/*
 * 2.3 设置子设备ops函数集
 * 整合所有不同操作集的接口定义
 * V4L2 靠这个结构体转发应用的命令到对应的操作函数
 */
static const struct v4l2_subdev_ops ov13855_subdev_ops = {
	.core	= &ov13855_core_ops,// 核心操作（上电、复位等），电源管理(s_power)、ioctl处理
	.video	= &ov13855_video_ops,// 视频操作（数据流控制），流控制(s_stream)、帧率操作(g_frame_interval)
	.pad	= &ov13855_pad_ops,// 媒体垫片操作（格式协商），格式协商(set_fmt)、枚举支持的格式
};

// 参数控制时的I2C写操作
static int ov13855_set_ctrl(struct v4l2_ctrl *ctrl)
{
	// 已知条件：
	// 1. ctrl是V4L2框架传递的控制对象指针
	// 2. ctrl->handler指向我们在ov13855结构体中的ctrl_handler成员
	// 3. 我们需要获取完整的ov13855结构体来进行硬件操作

	// 第一步：从ctrl->handler获取到包含它的ov13855结构体
	struct ov13855 *ov13855 = container_of(ctrl->handler,
					     struct ov13855, ctrl_handler);
	// 第二步：获取I2C客户端对象用于硬件通信
	struct i2c_client *client = ov13855->client;
	s64 max;
	int ret = 0;

	// 第三步：检查设备状态，确保可以进行硬件操作

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/* Update max exposure while meeting expected vblanking */
		max = ov13855->cur_mode->height + ctrl->val - 4;
		__v4l2_ctrl_modify_range(ov13855->exposure,
					 ov13855->exposure->minimum, max,
					 ov13855->exposure->step,
					 ov13855->exposure->default_value);
		break;
	}

	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	// 第四步：根据不同的控制参数类型，执行相应的硬件操作
	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		/* 4 least significant bits of expsoure are fractional part */
		// 设置曝光参数需要向特定寄存器写入数值
		ret = ov13855_write_reg(ov13855->client,
					OV13855_REG_EXPOSURE,
					OV13855_REG_VALUE_24BIT,
					ctrl->val << 4);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		// 设置模拟增益参数
		ret = ov13855_write_reg(ov13855->client,
					OV13855_REG_GAIN_H,
					OV13855_REG_VALUE_08BIT,
					(ctrl->val >> OV13855_GAIN_H_SHIFT) &
					OV13855_GAIN_H_MASK);
		ret |= ov13855_write_reg(ov13855->client,
					 OV13855_REG_GAIN_L,
					 OV13855_REG_VALUE_08BIT,
					 ctrl->val & OV13855_GAIN_L_MASK);
		break;
	case V4L2_CID_VBLANK:
		ret = ov13855_write_reg(ov13855->client,
					OV13855_REG_VTS,
					OV13855_REG_VALUE_16BIT,
					ctrl->val + ov13855->cur_mode->height);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = ov13855_enable_test_pattern(ov13855, ctrl->val);
		break;
	default:
		dev_warn(&client->dev, "%s Unhandled id:0x%x, val:0x%x\n",
			 __func__, ctrl->id, ctrl->val);
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops ov13855_ctrl_ops = {
	.s_ctrl = ov13855_set_ctrl,
};

/*
 * 初始化 V4L2 控制器处理器
 * 控制器的分类：
 * 1. 只读控制器：link_freq、pixel_rate、hblank，用于查询sensor当前参数
 * 2. 可写控制器：vblank、exposure、anal_gain、test_pattern，用户可以调整
 * 将传感器支持的各类调节参数（如曝光、增益、空白区）注册到 V4L2 子设备中
 */
static int ov13855_initialize_controls(struct ov13855 *ov13855)
{
	const struct ov13855_mode *mode;
	struct v4l2_ctrl_handler *handler;
	s64 exposure_max, vblank_def;
	u32 h_blank;
	int ret;
	u64 dst_pixel_rate = 0;
	u32 lane_num = OV13855_LANES;

	handler = &ov13855->ctrl_handler;
	mode = ov13855->cur_mode;// 获取当前分辨率模式

	// 初始化控制器处理器，预分配 8 个控制实体的内存空间
	ret = v4l2_ctrl_handler_init(handler, 8);
	if (ret)
		return ret;
	// 绑定设备私有互斥锁，确保多线程下（如多个应用并发配置）的并发安全
	handler->lock = &ov13855->mutex;

	/* 
     * 1. 链路频率控制器 (LINK_FREQ)
     * 类型：整数菜单 (Integer Menu)
     * 权限：默认只读 (由硬件物理布线和 MIPI 协议协商决定，用户态不可随意更改)
     */
	ov13855->link_freq = v4l2_ctrl_new_int_menu(handler, NULL,
			V4L2_CID_LINK_FREQ,
			1, 0, link_freq_items);
	// 根据 MIPI 协议公式计算像素吞吐率：(链路频率 / 像素位宽) * 2(DDR双边沿) * MIPI通道数
	dst_pixel_rate = (u32)link_freq_items[mode->link_freq_idx] / mode->bpp * 2 * lane_num;

	/* 
     * 2. 像素速率控制器 (PIXEL_RATE)
     * 权限：只读 (系统/ISP 需要根据此值计算处理带宽和时间戳，不可更改)
     */
	ov13855->pixel_rate = v4l2_ctrl_new_std(handler, NULL,
			V4L2_CID_PIXEL_RATE,
			0, OV13855_PIXEL_RATE,
			1, dst_pixel_rate);
	// 同步底层实际选用的链路频率索引
	__v4l2_ctrl_s_ctrl(ov13855->link_freq,
			   mode->link_freq_idx);
	// 计算水平消隐区大小 (HTS - 图像有效宽度)
	h_blank = mode->hts_def - mode->width;

	/* 
     * 3. 水平消隐控制器 (HBLANK)
     * 权限：显式设置为只读 (READ_ONLY)。
     * 原因：修改 HBLANK 通常会破坏传感器内部的锁相环 (PLL) 或行同步时序，严禁用户态干预。
     */
	ov13855->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
				h_blank, h_blank, 1, h_blank);
	if (ov13855->hblank)
		ov13855->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	// 计算垂直消隐区默认值 (VTS - 图像有效高度)
	vblank_def = mode->vts_def - mode->height;

	/* 
     * 4. 垂直消隐控制器 (VBLANK)
     * 权限：可写。关联操作集：ov13855_ctrl_ops
     * 核心意义：用户态通过增加 VBLANK 可以延长每一帧的输出周期，从而实现“降低帧率”的功能（不改变时钟频率）。
     */
	ov13855->vblank = v4l2_ctrl_new_std(handler, &ov13855_ctrl_ops,
				V4L2_CID_VBLANK, vblank_def,
				OV13855_VTS_MAX - mode->height,
				1, vblank_def);
	
	/* 
     * 计算当前模式下的最大曝光时间。
     * 物理约束：曝光时间本质上是传感器光电二极管在读取前积累电荷的时间。
     * 它不能超过整个画面的垂直总扫描时间 (VTS) 减去必要的内部处理开销 (这里是 4 行)。
     */
	exposure_max = mode->vts_def - 4;

	/* 
     * 5. 曝光时间控制器 (EXPOSURE)
     * 权限：可写。关联操作集：ov13855_ctrl_ops
     */
	ov13855->exposure = v4l2_ctrl_new_std(handler, &ov13855_ctrl_ops,
				V4L2_CID_EXPOSURE, OV13855_EXPOSURE_MIN,
				exposure_max, OV13855_EXPOSURE_STEP,
				mode->exp_def);

	/* 
     * 6. 模拟增益控制器 (ANALOGUE_GAIN)
     * 权限：可写。关联操作集：ov13855_ctrl_ops
     */
	ov13855->anal_gain = v4l2_ctrl_new_std(handler, &ov13855_ctrl_ops,
				V4L2_CID_ANALOGUE_GAIN, OV13855_GAIN_MIN,
				OV13855_GAIN_MAX, OV13855_GAIN_STEP,
				OV13855_GAIN_DEFAULT);

	/* 
     * 7. 测试图案控制器 (TEST_PATTERN)
     * 类型：标准菜单 (Menu Items)
     * 作用：用于排查 MIPI 硬件连线问题。开启后，Sensor 直接输出纯色条纹（如彩条），绕过光电转换环节。
     */
	ov13855->test_pattern = v4l2_ctrl_new_std_menu_items(handler,
				&ov13855_ctrl_ops, V4L2_CID_TEST_PATTERN,
				ARRAY_SIZE(ov13855_test_pattern_menu) - 1,
				0, 0, ov13855_test_pattern_menu);
	
	// 校验所有控制器实体是否创建成功
	if (handler->error) {
		ret = handler->error;
		dev_err(&ov13855->client->dev,
			"Failed to init controls(%d)\n", ret);
		goto err_free_handler;
	}

	/* 
     * 将初始化完成并填满 Controls 的 Handler，
     * 正式挂载到当前设备的 v4l2_subdev 对象上。
     * 至此，用户态程序就可以看到并使用这些滑动条了。
     */
	ov13855->subdev.ctrl_handler = handler;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);// 异常处理路径：释放 Handler 内部动态分配的内存

	return ret;
}

/*
 * 硬件验证逻辑：
 * 通过I2C从寄存器OV13855_REG_CHIP_ID读取3字节
 * 期望值是CHIP_ID = 0x00d855
 * 如果匹配，说明硬件连接正确、上电时序正确、I2C通信正常
 * 否则返回-ENODEV错误，probe失败
 */
static int ov13855_check_sensor_id(struct ov13855 *ov13855,
				   struct i2c_client *client)
{
	struct device *dev = &ov13855->client->dev;
	u32 id = 0;
	int ret;

	// 读取3字节芯片ID寄存器
	ret = ov13855_read_reg(client, OV13855_REG_CHIP_ID,
			       OV13855_REG_VALUE_24BIT, &id);
	if (id != CHIP_ID) {
		dev_err(dev, "Unexpected sensor id(%06x), ret(%d)\n", id, ret);
		return -ENODEV;
	}

	// 读取芯片版本号
	ret = ov13855_read_reg(client, OV13855_CHIP_REVISION_REG,
			       OV13855_REG_VALUE_08BIT, &id);
	if (ret) {
		dev_err(dev, "Read chip revision register error\n");
		return ret;
	}

	dev_info(dev, "Detected OV%06x sensor, REVISION 0x%x\n", CHIP_ID, id);

	return 0;
}

/*
 * 作用：初始化并批量获取传感器所需的多个电源调节器（Regulators）句柄。
 * 入参：ov13855 - 指向当前设备私有数据结构的指针。
 * 返回值：0 表示成功获取所有电源；负数错误码（如 -ENODEV）表示获取失败。
 */
static int ov13855_configure_regulators(struct ov13855 *ov13855)
{
	unsigned int i;
	/* 
     * 步骤一：填充“采购清单”
     * OV13855_NUM_SUPPLIES 通常是一个宏，代表需要的电源总数（这里是 3）。
     * 这个 for 循环在给 regulator_bulk_data 结构体数组的每一个元素命名。
     */
	for (i = 0; i < OV13855_NUM_SUPPLIES; i++)
		ov13855->supplies[i].supply = ov13855_supply_names[i];
	/* 
     * 步骤二：向内核提交清单，批量索要电源控制权
     * devm_regulator_bulk_get 是内核提供的高效 API。
     */
	return devm_regulator_bulk_get(&ov13855->client->dev,// 绑定的硬件设备
				       OV13855_NUM_SUPPLIES,// 要申请的电源数量（3路）
				       ov13855->supplies);// 刚才填好的采购清单数组
}

/*
 * struct i2c_client *client 里自带了属于自己的 DTS 节点
 * 1. 分配私有数据结构：为ov13855结构体分配内存空间
 * 2. 解析设备树配置：获取GPIO、时钟、电源等硬件资源信息
 * 3. 初始化硬件资源：配置时钟、电源、GPIO等
 * 4. 创建V4L2子设备：将摄像头注册到V4L2框架中
 * 5. 建立关联关系：让I2C客户端和V4L2子设备能够相互引用
 */
static int ov13855_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	/* ===================== 函数入口与基础准备 ===================== */
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct i2c_client *eeprom_ctrl_client;
	struct device_node *eeprom_ctrl_node;
	struct v4l2_subdev *eeprom_ctrl;
	struct otp_info *otp_ptr;
	struct ov13855 *ov13855;
	struct v4l2_subdev *sd;
	char facing[2];
	int ret;

	// 打印驱动版本信息
	dev_info(dev, "driver version: %02x.%02x.%02x",
		DRIVER_VERSION >> 16,
		(DRIVER_VERSION & 0xff00) >> 8,
		DRIVER_VERSION & 0x00ff);

	// 主要工作：建立驱动框架
	/*
	 * devm_ (Device Managed)： 表示“设备资源管理”
	 * k (Kernel)： 表示这块内存是在 Linux 的内核空间（Kernel Space）分配的
	 * z (Zeroed)： 表示内存分配成功后，内核会自动把这块内存里的所有字节全部清零
	 * alloc (Allocate)： 申请内存
	 * 
	 * dev：挂载点，就是把这块新申请的内存登记在这个硬件设备的名下
	 * sizeof(*ov13855)：申请大小
	 * GFP_KERNEL：分配标志/行为准则
	 * 它是在告诉内核内存管理系统：“我这次要内存并不着急，如果现在物理内存不够了，
	 * 你可以把我的进程休眠（阻塞），去慢慢整理或者交换出一些内存来，等凑够了再把我唤醒给我。”
	 */
	ov13855 = devm_kzalloc(dev, sizeof(*ov13855), GFP_KERNEL);
	if (!ov13855)
		return -ENOMEM;

	/* ===================== 任务一：硬件资源准备 ===================== */
	/* 1.1 从设备树读取硬件模块的身份信息，保存I2C客户端指针用于后续寄存器访问，设置默认输出模式。 */
	/*
	 * node = 当前设备自己的设备树节点
	 * 在 ov13855 驱动里，node 就指向 ov13855_1: ov13855-1@36 { ... }
	 * 这几个宏是在include/uapi/linux/rk-camera-module.h里面定义的
	 * 这几句的作用大概是从设备树节点读取相应的数据然后存到ov13855->...中
	 */
	ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
				   &ov13855->module_index);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
				       &ov13855->module_facing);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
				       &ov13855->module_name);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
				       &ov13855->len_name);
	if (ret) {
		dev_err(dev, "could not get module information!\n");
		return -EINVAL;
	}

	ov13855->client = client;
	ov13855->cur_mode = &supported_modes[0];

	/* 1.2 获取时钟资源 */
	ov13855->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(ov13855->xvclk)) {
		dev_err(dev, "Failed to get xvclk\n");
		return -EINVAL;
	}

	/* 1.3 获取GPIO控制引脚 */
	/* 1.3.1 控制LDO使能，决定是否给sensor供电 */
	/*
	 * 1. 从设备树获取名叫 power 的 GPIO 引脚
	 * 2. 申请占用这个 GPIO
	 * 3. 直接设置为输出模式 + 初始低电平（LOW）
	 */
	ov13855->power_gpio = devm_gpiod_get(dev, "power", GPIOD_OUT_LOW);
	if (IS_ERR(ov13855->power_gpio))
		dev_warn(dev, "Failed to get power-gpios, maybe no use\n");

	/* 1.3.2 硬件复位线，低电平时sensor内部寄存器复位 */
	ov13855->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ov13855->reset_gpio))
		dev_warn(dev, "Failed to get reset-gpios\n");

	/* 1.3.3 掉电模式控制，低电平进入超低功耗模式 */
	ov13855->pwdn_gpio = devm_gpiod_get(dev, "pwdn", GPIOD_OUT_LOW);
	if (IS_ERR(ov13855->pwdn_gpio))
		dev_warn(dev, "Failed to get pwdn-gpios\n");

	/* 1.4 配置电源调节器 */
	ret = ov13855_configure_regulators(ov13855);// 给传感器打开供电电源（DVDD、AVDD、IOVDD 等），让 sensor 通电工作
	if (ret) {
		dev_err(dev, "Failed to get power regulators\n");
		return ret;
	}

	/* 1.5 获取引脚复用配置 */
	ov13855->pinctrl = devm_pinctrl_get(dev);
	if (!IS_ERR(ov13855->pinctrl)) {
		// 查找【默认工作状态】的引脚配置
		ov13855->pins_default =
			pinctrl_lookup_state(ov13855->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_DEFAULT);
		if (IS_ERR(ov13855->pins_default))
			dev_err(dev, "could not get default pinstate\n");
		
		// 查找【休眠状态】的引脚配置
		ov13855->pins_sleep =
			pinctrl_lookup_state(ov13855->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_SLEEP);
		if (IS_ERR(ov13855->pins_sleep))
			dev_err(dev, "could not get sleep pinstate\n");
	}

	/* 1.6. 初始化同步机制 */
	mutex_init(&ov13855->mutex);

	/* ===================== 任务二：V4L2框架集成 ===================== */
	/*  2.1 初始化V4L2子设备 */
	sd = &ov13855->subdev;
	/*
	 * v4l2_i2c_subdev_init做的事情：
	 * 1. 设置sd->owner = THIS_MODULE
	 * 2. 设置sd->dev = &client->dev（指向I2C设备）
	 * 3. 设置sd->ops = &ov13855_subdev_ops（操作函数集）
	 * 4. 用I2C设备名初始化sd->name
	 * 5. 调用v4l2_set_subdevdata(sd, client)（保存私有数据）
	 * 
	 * 建立的关系：
	 * v4l2_subdev (sd)
	 * ├── ops → ov13855_subdev_ops（提供标准操作）
	 * ├── dev → i2c_client->dev（关联硬件设备）
	 * └── i2c_client（通过subdevdata获取）
	 */
	v4l2_i2c_subdev_init(sd, client, &ov13855_subdev_ops);

	/* 
	 * 2.2 创建V4L2控制器 
	 * 控制器的作用：让用户空间能够调整sensor参数（曝光、增益、测试图案等）
	 */
	ret = ov13855_initialize_controls(ov13855);
	if (ret)
		goto err_destroy_mutex;

	/* 1.7. 硬件上电 */
	ret = __ov13855_power_on(ov13855);
	if (ret)
		goto err_free_handler;

	/* 1.8. 验证硬件ID */
	ret = ov13855_check_sensor_id(ov13855, client);
	if (ret)
		goto err_power_off;

	// 解析设备树（Device Tree），查找当前节点中名为 "eeprom-ctrl" 的属性，不过设备树里没有，下面不会成立
	eeprom_ctrl_node = of_parse_phandle(node, "eeprom-ctrl", 0);
	if (eeprom_ctrl_node) {
		// 根据获取到的设备树节点，在 I2C 总线上查找并获取对应的 I2C 客户端设备实例（i2c_client）
		eeprom_ctrl_client =
			of_find_i2c_device_by_node(eeprom_ctrl_node);
		of_node_put(eeprom_ctrl_node);// 释放 EEPROM 设备树节点的引用计数
		if (IS_ERR_OR_NULL(eeprom_ctrl_client)) {
			dev_err(dev, "can not get node\n");
			goto continue_probe;
		}
		eeprom_ctrl = i2c_get_clientdata(eeprom_ctrl_client);// 提取 I2C 客户端的私有数据
		if (IS_ERR_OR_NULL(eeprom_ctrl)) {
			dev_err(dev, "can not get eeprom i2c client\n");
		} 
		else {
			// 使用设备资源管理宏（devres）在内核空间分配一块清零的内存
			otp_ptr = devm_kzalloc(dev, sizeof(*otp_ptr), GFP_KERNEL);
			if (!otp_ptr) {
				put_device(&eeprom_ctrl_client->dev);
				goto continue_probe;
			}
			// 发起 V4L2 跨子设备调用
			ret = v4l2_subdev_call(eeprom_ctrl,
				core, ioctl, 0, otp_ptr);
			if (!ret) {
				// 将填充好 OTP 数据的结构体指针绑定到当前 OV13855 设备的私有数据上下文中
				ov13855->otp = otp_ptr;
			} 
			else {
				ov13855->otp = NULL;
				// 释放为 OTP 数据分配的内存，避免无用内存持续占用
				devm_kfree(dev, otp_ptr);
			}
		}
		/* 
		 * 无论 OTP 数据读取成功与否，与 EEPROM 的交互均已结束。
		 * 必须递减 i2c_client 的设备引用计数，以维持内核对象生命周期管理的平衡。
		 */
		put_device(&eeprom_ctrl_client->dev);
	}

continue_probe:
/* 
 * 2.4 配置子设备标志位 
 * V4L2_SUBDEV_FL_HAS_DEVNODE标志的含义：
 * 告诉V4L2框架"这个子设备需要创建独立的字符设备节点"。
 * 
 * 为什么需要子设备节点：
 * 子设备节点 = sensor的直通接口
 * 没有子设备节点：应用 → /dev/video0 → ISP驱动 → sensor（间接控制）
 * 有子设备节点：应用 → /dev/v4l-subdev2 → sensor（直接控制）
 * 应用程序可以直接打开/dev/v4l-subdev2，独立控制sensor而不经过ISP。这对调试和高级应用很有用。
 */
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API // Kconfig 配置选项，有了它，底层的 Sensor 驱动才配拥有 /dev/v4l-subdevX 节点
    /* 1. 设置内部操作集（当设备节点被打开时该干嘛） */
	sd->internal_ops = &ov13855_internal_ops;
	/* 2. 贴上一个极其关键的标签：告诉内核“我要拥有自己的设备节点！” */
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
#endif
/* 
 * 2.5 初始化media entity 
 * media框架的作用：描述硬件模块间的连接拓扑。
 * pad的概念：连接点，类似硬件接口。sensor有一个输出pad，连接到CSI2的输入pad。
 * entity：代表一个具有独立功能的硬件逻辑块
 * 
 * media_entity_pads_init做的事：
 * 1. 分配pad数组内存
 * 2. 设置pad属性（方向、标志）
 * 3. 关联pad到entity
 * 拓扑关系：
 * [OV13855 entity]
 *     └─ pad[0] (SOURCE) --连接线--> [CSI2 entity] pad[0] (SINK)
 * 正因为驱动中写了这段代码，可以使用 media-ctl 工具来动态查看和修改底层的硬件连线。
 */
#if defined(CONFIG_MEDIA_CONTROLLER)// 对于 RK3588 的相机驱动，这个宏通常是强制开启的
	ov13855->pad.flags = MEDIA_PAD_FL_SOURCE;// MEDIA_PAD_FL_SOURCE代表数据输出端
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;// MEDIA_ENT_F_CAM_SENSOR 代表 “这是一个 Camera Sensor 类型的实体”
	/*
	 * &sd->entity：当前子设备的实体对象。
	 * 1：表示这个实体一共有 1 个端口（Pad）。
	 * &ov13855->pad：具体的端口对象指针。
	 */
	ret = media_entity_pads_init(&sd->entity, 1, &ov13855->pad);
	if (ret < 0)
		goto err_power_off;
#endif

	/*
	 * 2.6 构造子设备名称
	 * 为当前这个摄像头子设备动态生成一个全局唯一、且符合严格命名规范的“实体名称”
	 * 命名规则：m<模块号>_<朝向>_<sensor型号> <I2C设备名>
	 * 示例：m00_b_ov13855 4-0010
	 * m00：模块0
	 * b：后置摄像头
	 * ov13855：sensor型号
	 * 4-0010：I2C总线4，设备地址0x10
	 * 用途：用户空间通过这个名字识别具体的摄像头。
	 */
	// 确定摄像头朝向
	memset(facing, 0, sizeof(facing));
	if (strcmp(ov13855->module_facing, "back") == 0)
		facing[0] = 'b';
	else
		facing[0] = 'f';
	// 拼接全局唯一名称
	snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
		 ov13855->module_index, facing,
		 OV13855_NAME, dev_name(sd->dev));
	
	/* ===================== 任务三：异步框架注册 ===================== */
	/* 
	 * 3.1 注册到异步框架
	 * v4l2_async_register_subdev_sensor做的事：
	 * 3.1.1 设置子设备的fwnode（从设备树获取）
	 * 3.1.2 调用v4l2_async_register_subdev(sd)注册到全局异步子设备列表
	 * 3.1.3 框架开始尝试匹配：查找是否有notifier在等待这个fwnode的子设备
	 * 
	 * 注册后的状态：
	 * 全局async子设备列表
	 * ├── ov13855_subdev (fwnode = /i2c@.../ov13855@10)
	 * └── ... 其他子设备
	 *
	 */
	ret = v4l2_async_register_subdev_sensor(sd);
	if (ret) {
		dev_err(dev, "v4l2 async register subdev failed\n");
		goto err_clean_entity;
	}

	/* 3.6 使能运行时电源管理 */
	pm_runtime_set_active(dev);	/* 标记设备当前为活动状态 */
	pm_runtime_enable(dev);		/* 使能运行时电源管理 */
	pm_runtime_idle(dev);		/* 设为空闲态，PM框架可以决定是否关闭电源 */

	return 0;

err_clean_entity:
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
err_power_off:
	__ov13855_power_off(ov13855);
err_free_handler:
	v4l2_ctrl_handler_free(&ov13855->ctrl_handler);
err_destroy_mutex:
	mutex_destroy(&ov13855->mutex);

	return ret;
}

static void ov13855_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov13855 *ov13855 = to_ov13855(sd);

	v4l2_async_unregister_subdev(sd);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
	v4l2_ctrl_handler_free(&ov13855->ctrl_handler);
	mutex_destroy(&ov13855->mutex);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		__ov13855_power_off(ov13855);
	pm_runtime_set_suspended(&client->dev);
}

/*
 * 设备树匹配表
 * 只有内核开启了设备树功能，才编译这段代码
 */
#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id ov13855_of_match[] = {
	{ .compatible = "ovti,ov13855" },
	{},
};
MODULE_DEVICE_TABLE(of, ov13855_of_match);// 把设备树匹配表导出，让内核能找到、读取、用来做匹配
#endif

static const struct i2c_device_id ov13855_match_id[] = {
	{ "ovti,ov13855", 0 },
	{},
};

/*
 * I2C 驱动结构体
 * of_match_table → 设备树匹配（DTS）
 * id_table → 传统 I2C/SPI 设备名匹配
 */
static struct i2c_driver ov13855_i2c_driver = {
	.driver = {
		.name = OV13855_NAME, // 驱动名称："ov13855"
		.pm = &ov13855_pm_ops,// 电源管理操作
		.of_match_table = of_match_ptr(ov13855_of_match),// 设备树匹配表
	},
	.probe		= &ov13855_probe,  // 设备发现时的初始化函数
	.remove		= &ov13855_remove, // 设备移除时的清理函数
	.id_table	= ov13855_match_id,// I2C设备ID匹配表
};

/*
 * 内核模块入口函数（加载时执行）
 */
static int __init sensor_mod_init(void)
{
	return i2c_add_driver(&ov13855_i2c_driver);// 注册 I2C 驱动程序
}

/*
 * 内核模块出口函数（卸载时执行）
 */
static void __exit sensor_mod_exit(void)
{
	i2c_del_driver(&ov13855_i2c_driver);// 注销 I2C 驱动程序
}

/*
 * 把入口函数注册给内核
 * 问题：为什么不用module_init
 * 因为这是设备驱动（摄像头 sensor），必须在内核启动更早、和其他设备同步的时机初始化，不能用普通的 module_init（太晚了）
 */
device_initcall_sync(sensor_mod_init);

/*
 * 把出口函数注册给内核
 */
module_exit(sensor_mod_exit);

MODULE_DESCRIPTION("OmniVision ov13855 sensor driver");
MODULE_LICENSE("GPL v2");
