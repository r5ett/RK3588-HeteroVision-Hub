/* ==================== 1. 头文件引入区 ==================== */
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/kfifo_buf.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/sched.h>

/* ==================== 2. 宏定义区 ==================== */
#define SPI_DATA_LEN 32                   // 每次 SPI 传输数据长度
#define SHARED_MEM_SIZE PAGE_SIZE         // 共享内存大小 (4KB)
#define MAX_FRAMES 100                    // 环形缓冲区帧数

/* ==================== 3. 数据结构定义区 ==================== */
/* DMA 共享内存控制块 (暴露给用户态 mmap 零拷贝读取) */
struct h7_mmap_ctrl {
    volatile u32 head_idx;                 // 指向缓冲区下一个要写入的索引
    volatile u32 update_count;             // 全局统计接收帧数
    u8 tx_dummy[SPI_DATA_LEN];             // SPI 发送无意义填充区
    u8 buffer[MAX_FRAMES][SPI_DATA_LEN];   // 接收二维环形缓冲区
};

/* 驱动核心私有上下文 */
struct h7_gateway_state {
    struct spi_device *spi;
    struct iio_dev *indio_dev;
    struct mutex lock;
    
    struct h7_mmap_ctrl *mmap_vaddr;       // DMA 虚拟地址
    dma_addr_t mmap_paddr;                 // DMA 物理地址
    struct fasync_struct *async_queue;     // 异步通知队列
    struct miscdevice mmap_dev;            // 杂项设备
    
    s64 irq_ts;                            // 中断发生时间戳
    wait_queue_head_t wq;                  // 读等待队列
    bool data_ready;                       // 数据就绪标志

    struct {
        s16 channels[3];
        s64 ts __aligned(8);
    } scan;
};

/* ==================== 4. IIO 框架配置区 ==================== */
static const struct iio_chan_spec h7_channels[] = {
    {
        .type = IIO_ACCEL, .modified = 1, .channel2 = IIO_MOD_X,
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW), .scan_index = 0,
        .scan_type = { .sign = 's', .realbits = 16, .storagebits = 16, .endianness = IIO_BE }, 
    },
    {
        .type = IIO_ACCEL, .modified = 1, .channel2 = IIO_MOD_Y,
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW), .scan_index = 1,
        .scan_type = { .sign = 's', .realbits = 16, .storagebits = 16, .endianness = IIO_BE },
    },
    {
        .type = IIO_ACCEL, .modified = 1, .channel2 = IIO_MOD_Z,
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW), .scan_index = 2,
        .scan_type = { .sign = 's', .realbits = 16, .storagebits = 16, .endianness = IIO_BE },
    },
    IIO_CHAN_SOFT_TIMESTAMP(3),
};

static int h7_read_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
                       int *val, int *val2, long mask) {
    return -EINVAL; 
}

static const struct iio_info h7_iio_info = {
    .read_raw = h7_read_raw,
};

/* ==================== 5. 字符设备 (VFS) 接口区 ==================== */
static ssize_t h7_mmap_read(struct file *file, char __user *buf, size_t count, loff_t *f_pos) {
    struct h7_gateway_state *st = file->private_data;
    u32 idx;
    int ret;

    if (!st->data_ready && (file->f_flags & O_NONBLOCK)) return -EAGAIN;

    ret = wait_event_interruptible(st->wq, st->data_ready);
    if (ret) return ret;

    mutex_lock(&st->lock);
    idx = (st->mmap_vaddr->head_idx == 0) ? (MAX_FRAMES - 1) : (st->mmap_vaddr->head_idx - 1);
    if (count > SPI_DATA_LEN) count = SPI_DATA_LEN;

    if (copy_to_user(buf, st->mmap_vaddr->buffer[idx], count)) {
        mutex_unlock(&st->lock);
        return -EFAULT;
    }

    st->data_ready = false;
    mutex_unlock(&st->lock);
    return count;
}

static unsigned int h7_mmap_poll(struct file *file, poll_table *wait) {
    struct h7_gateway_state *st = file->private_data;
    unsigned int mask = 0;
    poll_wait(file, &st->wq, wait);
    if (st->data_ready) mask |= POLLIN | POLLRDNORM; 
    return mask;
}

static int h7_mmap_op(struct file *file, struct vm_area_struct *vma) {
    struct h7_gateway_state *st = file->private_data;
    unsigned long size = vma->vm_end - vma->vm_start;

    if (size > SHARED_MEM_SIZE) return -EINVAL;
    
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    if (remap_pfn_range(vma, vma->vm_start, st->mmap_paddr >> PAGE_SHIFT, size, vma->vm_page_prot)) {
        return -EAGAIN;
    }
    return 0;
}

static int h7_mmap_fasync(int fd, struct file *filp, int mode) {
    struct h7_gateway_state *st = filp->private_data;
    return fasync_helper(fd, filp, mode, &st->async_queue);
}

static int h7_mmap_release(struct inode *inode, struct file *file) {
    h7_mmap_fasync(-1, file, 0);
    return 0;
}

static int h7_mmap_open(struct inode *inode, struct file *file) {
    struct miscdevice *cdev = file->private_data;
    struct h7_gateway_state *st = container_of(cdev, struct h7_gateway_state, mmap_dev);
    file->private_data = st; 
    return 0;
}

static const struct file_operations h7_mmap_fops = {
    .owner   = THIS_MODULE,
    .open    = h7_mmap_open,
    .read    = h7_mmap_read,   
    .poll    = h7_mmap_poll,   
    .mmap    = h7_mmap_op, 
    .fasync  = h7_mmap_fasync,  
    .release = h7_mmap_release, 
};

/* ==================== 6. 核心业务逻辑 (Threaded IRQ) ==================== */
/* 顶半部：硬件中断瞬间执行，抓取时间戳并唤醒线程 */
static irqreturn_t h7_gateway_irq_handler(int irq, void *dev_id) {
    struct iio_dev *indio_dev = dev_id;
    struct h7_gateway_state *st = iio_priv(indio_dev);

    st->irq_ts = iio_get_time_ns(indio_dev);
    return IRQ_WAKE_THREAD; // 唤醒底半部内核线程
}

/* 底半部：独立内核线程，执行耗时的 SPI DMA 传输 */
static irqreturn_t h7_gateway_irq_thread(int irq, void *dev_id) {
    struct iio_dev *indio_dev = dev_id;
    struct h7_gateway_state *st = iio_priv(indio_dev);
    u32 idx = st->mmap_vaddr->head_idx;
    int ret;

    struct spi_transfer t = {0};
    struct spi_message m;

    t.tx_buf = st->mmap_vaddr->tx_dummy;
    t.tx_dma = st->mmap_paddr + offsetof(struct h7_mmap_ctrl, tx_dummy);
    
    t.rx_buf = st->mmap_vaddr->buffer[idx];
    t.rx_dma = st->mmap_paddr + offsetof(struct h7_mmap_ctrl, buffer) + (idx * SPI_DATA_LEN);
    
    t.len = SPI_DATA_LEN; 
    
    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    m.is_dma_mapped = 1; // 告知底层直接使用算好的物理地址

    ret = spi_sync(st->spi, &m);
    if (ret == 0) {
        // 数据推给 IIO
        memcpy(st->scan.channels, st->mmap_vaddr->buffer[idx], 6);
        iio_push_to_buffers_with_timestamp(indio_dev, &st->scan, st->irq_ts);

        // 更新无锁环形队列指针
        st->mmap_vaddr->head_idx = (idx + 1) % MAX_FRAMES;
        st->mmap_vaddr->update_count++;

        // 唤醒上层 VFS
        st->data_ready = true;
        wake_up_interruptible(&st->wq);

        if (st->async_queue && (st->mmap_vaddr->update_count % 100 == 0)) {
            kill_fasync(&st->async_queue, SIGIO, POLL_IN);
        }
    }

    return IRQ_HANDLED;
}

/* ==================== 7. 设备驱动初始化/卸载区 ==================== */
static int h7_gateway_probe(struct spi_device *spi) {
    struct iio_dev *indio_dev;
    struct h7_gateway_state *st;
    struct iio_buffer *buffer;
    u32 custom_speed = 20000000; // 默认拉到 20MHz
    int ret;

    printk(KERN_INFO "RK3588 H7 Gateway SPI Driver Probing...\n");

    // 1. 绕过内核 API 的严格空指针检查，手动强制配置 DMA 掩码（修复 Error -5 关键）
    if (!spi->dev.dma_mask) {
        spi->dev.dma_mask = &spi->dev.coherent_dma_mask;
    }
    spi->dev.coherent_dma_mask = DMA_BIT_MASK(32);

    indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
    if (!indio_dev) return -ENOMEM;

    st = iio_priv(indio_dev);
    st->spi = spi;
    st->indio_dev = indio_dev;
    mutex_init(&st->lock);
    init_waitqueue_head(&st->wq);
    st->data_ready = false;
    spi_set_drvdata(spi, indio_dev);

    indio_dev->name = "h7_gateway";
    indio_dev->info = &h7_iio_info;
    indio_dev->channels = h7_channels;
    indio_dev->num_channels = ARRAY_SIZE(h7_channels);
    indio_dev->modes = INDIO_DIRECT_MODE | INDIO_BUFFER_SOFTWARE;

    buffer = iio_kfifo_allocate();
    if (!buffer) return -ENOMEM;
    iio_device_attach_buffer(indio_dev, buffer);

    // 分配 DMA 一致性内存
    st->mmap_vaddr = dma_alloc_coherent(&spi->dev, SHARED_MEM_SIZE, &st->mmap_paddr, GFP_KERNEL);
    if (!st->mmap_vaddr) {
        ret = -ENOMEM;
        goto err_free_iio;
    }
    memset(st->mmap_vaddr, 0, SHARED_MEM_SIZE);

    st->mmap_dev.minor = MISC_DYNAMIC_MINOR;
    st->mmap_dev.name  = "h7_mmap";
    st->mmap_dev.fops  = &h7_mmap_fops;
    ret = misc_register(&st->mmap_dev);
    if (ret) goto err_free_dma;

    spi->mode = SPI_MODE_0;
    spi->bits_per_word = 8;
    spi->max_speed_hz = custom_speed;
    ret = spi_setup(spi);
    if (ret) goto err_deregister_misc;

    // 核心重构：使用线程化中断代替工作队列
    ret = devm_request_threaded_irq(&spi->dev, spi->irq, 
                                    h7_gateway_irq_handler, // 顶半部
                                    h7_gateway_irq_thread,  // 底半部
                                    IRQF_TRIGGER_FALLING | IRQF_ONESHOT, 
                                    "h7_ready", indio_dev);
    if (ret < 0) goto err_deregister_misc;

    ret = devm_iio_device_register(&spi->dev, indio_dev);
    if (ret < 0) goto err_deregister_misc;

    dev_info(&spi->dev, "HeteroCore Gateway Probed Successfully with Threaded IRQ!\n");
    return 0;

err_deregister_misc:
    misc_deregister(&st->mmap_dev);
err_free_dma:
    dma_free_coherent(&spi->dev, SHARED_MEM_SIZE, st->mmap_vaddr, st->mmap_paddr);
err_free_iio:
    iio_kfifo_free(buffer);
    return ret;
}

// ⚠️ 修改了这里：由 int 变更为 void，适配 Linux 6.1 内核标准
static void h7_gateway_remove(struct spi_device *spi) {
    struct iio_dev *indio_dev = spi_get_drvdata(spi);
    struct h7_gateway_state *st = iio_priv(indio_dev);

    misc_deregister(&st->mmap_dev);
    dma_free_coherent(&spi->dev, SHARED_MEM_SIZE, st->mmap_vaddr, st->mmap_paddr);
    if (indio_dev->buffer) iio_kfifo_free(indio_dev->buffer);
    
    // 删除了原本的 return 0;
}

/* ==================== 8. 模块注册表区 ==================== */
static const struct of_device_id h7_gateway_of_match[] = {
    {.compatible = "r5ett,h7-gateway-spi"}, // 必须与 r5ett.dtsi 保持绝对一致
    {}, 
};
MODULE_DEVICE_TABLE(of, h7_gateway_of_match);

// ⚠️ 新增：传统设备 ID 匹配表，彻底消除 "has no spi_device_id" 警告
static const struct spi_device_id h7_gateway_id[] = {
    { "h7-gateway-spi", 0 },
    { } 
};
MODULE_DEVICE_TABLE(spi, h7_gateway_id);

static struct spi_driver h7_gateway_driver = {
    .probe = h7_gateway_probe,             
    .remove = h7_gateway_remove,           
    .id_table = h7_gateway_id,             // 绑定传统的 ID 表
    .driver = {
        .name = "h7_gateway",              
        .of_match_table = h7_gateway_of_match, 
    },
};

module_spi_driver(h7_gateway_driver);
MODULE_LICENSE("GPL");