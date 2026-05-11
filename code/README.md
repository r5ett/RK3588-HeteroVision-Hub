OV13855 probe函数详解：
基础准备阶段：分配驱动私有数据结构，这个结构体将贯穿整个probe流程，存储所有硬件资源句柄和状态信息。probe函数执行后，内存中的数据结构
ov13855 (私有数据)
├── client (I2C客户端)
├── subdev (V4L2子设备)
│   ├── ops → ov13855_subdev_ops
│   ├── ctrl_handler → 控制器集合
│   └── entity (media实体)
│       └── pad[0] (输出pad)
├── xvclk (时钟句柄)
├── power_gpio (电源GPIO)
├── reset_gpio (复位GPIO)
└── cur_mode → supported_modes[0]

任务一：硬件资源准备
1. 硬件上电时序背景
2. 读取DTS中的模块配置
3. 获取时钟资源
4. 获取GPIO控制引脚
5. 配置电源调节器
6. 获取引脚复用配置
7. 初始化同步机制
8. 硬件上电
9. 验证硬件ID

任务二：V4L2框架集成
1. V4L2子设备的作用
V4L2子设备概念：把硬件模块抽象成一个具有标准化操作接口的软件对象。每个子设备通过v4l2_subdev_ops结构体向框架提供功能函数。
OV13855作为子设备：对外提供开流、关流、设置分辨率、读取帧率等标准操作，隐藏底层寄存器细节。
2. 初始化V4L2子设备
3. 创建V4L2控制器
4. 设置子设备ops函数集
5. 配置子设备标志位
6. 初始化media entity
7. 构造子设备名称

任务三：异步框架注册
1. 异步注册的必要性
2. 注册到异步框架
3. ISP驱动的notifier建立
文件: drivers/staging/media/rkisp1/rkisp1-dev.c中static int rkisp1_subdev_notifier(struct rkisp1_device *rkisp1)
4. 异步匹配与回调
static int rkisp1_subdev_notifier_bound(struct v4l2_async_notifier *notifier,
					struct v4l2_subdev *sd,
					struct v4l2_async_subdev *asd)
5. complete回调：创建设备节点
static int rkisp1_subdev_notifier_complete(struct v4l2_async_notifier *notifier)
6. 设备节点创建的实际执行
int v4l2_device_register_subdev_nodes(struct v4l2_device *v4l2_dev)
7. 使能运行时电源管理

时间线：
    ↓
[OV13855 probe开始]
    ├─ 任务一：硬件资源准备
    │   ├─ 分配私有数据结构
    │   ├─ 读取DTS模块配置
    │   ├─ 获取时钟、GPIO、regulator、pinctrl
    │   ├─ 初始化mutex
    │   ├─ 上电（按时序控制GPIO和regulator）
    │   └─ 验证芯片ID ✓
    │
    ├─ 任务二：V4L2框架集成
    │   ├─ 初始化v4l2_subdev
    │   ├─ 创建控制器（曝光、增益等）
    │   ├─ 设置ops函数集
    │   ├─ 设置V4L2_SUBDEV_FL_HAS_DEVNODE标志 ← 预约设备节点
    │   └─ 初始化media entity和pad
    │
    └─ 任务三：异步框架注册
        ├─ 调用v4l2_async_register_subdev_sensor_common
        ├─ 注册到全局async子设备列表
        └─ 使能runtime PM
[OV13855 probe结束]
    ↓
[等待ISP驱动probe...]
    ↓
[ISP probe]
    ├─ 建立async notifier
    ├─ 注册notifier
    └─ 框架开始匹配
        ↓
[框架匹配到OV13855]
    ├─ 调用bound回调
    │   └─ 保存sensor指针，初始化D-PHY
    ↓
[所有子设备匹配完成]
    ├─ 调用complete回调
    │   ├─ rkisp1_create_links() - 建立media连接
    │   └─ v4l2_device_register_subdev_nodes() ← 实际创建设备节点
    │       └─ 遍历子设备，检查V4L2_SUBDEV_FL_HAS_DEVNODE标志
    │           └─ 调用video_register_device()
    │               └─ 创建/dev/v4l-subdev2 ✓
    └─ 系统就绪

/dev/v4l-subdev2 → 字符设备 → 主设备号81，次设备号由内核分配

总结
probe函数按三大任务清晰组织：

任务一：硬件资源准备

获取资源句柄 → 按上电时序控制硬件 → 验证ID
任务二：V4L2框架集成

初始化subdev → 创建控制器 → 设置ops → 配置标志位和media entity
任务三：异步框架注册

注册到async框架 → 等待ISP匹配 → complete回调创建节点
设备节点创建的关键：
probe中设置V4L2_SUBDEV_FL_HAS_DEVNODE标志，ISP complete回调中调用v4l2_device_register_subdev_nodes()实际创建。这是一个"预约-兑现"的两阶段机制。



















