#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <errno.h>
#include <cstdint>
#include <fstream>
#include <cstdio>
#include <cmath>
#include <algorithm>

#include <rga/RgaApi.h>
#include <rga/im2d.h>
#include <opencv2/opencv.hpp>
#include <rknn/rknn_api.h>

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>

const char* COCO_CLASSES[80] = {"person","bicycle","car","motorcycle","airplane","bus","train","truck","boat","traffic light","fire hydrant","stop sign","parking meter","bench","bird","cat","dog","horse","sheep","cow","elephant","bear","zebra","giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee","skis","snowboard","sports ball","kite","baseball bat","baseball glove","skateboard","surfboard","tennis racket","bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple","sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair","couch","potted plant","bed","dining table","toilet","tv","laptop","mouse","remote","keyboard","cell phone","microwave","oven","toaster","sink","refrigerator","book","clock","vase","scissors","teddy bear","hair drier","toothbrush"};

struct Object { float x, y, w, h, prob; int class_id; };
inline float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }
inline float calc_iou(const Object& a, const Object& b) {
    float x1 = std::max(a.x, b.x), y1 = std::max(a.y, b.y);
    float x2 = std::min(a.x + a.w, b.x + b.w), y2 = std::min(a.y + a.h, b.y + b.h);
    float w = std::max(0.0f, x2 - x1), h = std::max(0.0f, y2 - y1);
    float inter = w * h;
    return inter / (a.w * a.h + b.w * b.h - inter);
}

// --- V4L2 摄像头采集类 ---
struct BufferInfo { void* start; size_t length; int dma_fd; };
class V4L2Camera {
private:
    std::string dev_name; int fd; std::vector<BufferInfo> buffers;
    uint32_t buf_count; uint32_t buf_type; bool is_mplane;
public:
    V4L2Camera(const std::string& name) : dev_name(name), fd(-1), buf_count(4), buf_type(0), is_mplane(false) {}
    ~V4L2Camera() { cleanup(); }
    bool init(uint32_t pixel_format, int width, int height) {
        // 强行使用阻塞模式，强迫 CPU 等待摄像头画面，绝不丢帧！
        fd = open(dev_name.c_str(), O_RDWR, 0);
	if (fd < 0) return false;
        struct v4l2_capability cap; memset(&cap, 0, sizeof(cap));
        ioctl(fd, VIDIOC_QUERYCAP, &cap);
        uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) { is_mplane = true; buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; }
        else { is_mplane = false; buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE; }
        struct v4l2_format fmt; memset(&fmt, 0, sizeof(fmt)); fmt.type = buf_type;
        if (is_mplane) { fmt.fmt.pix_mp.width = width; fmt.fmt.pix_mp.height = height; fmt.fmt.pix_mp.pixelformat = pixel_format; } 
        else { fmt.fmt.pix.width = width; fmt.fmt.pix.height = height; fmt.fmt.pix.pixelformat = pixel_format; }
        if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) return false;
        struct v4l2_requestbuffers req; memset(&req, 0, sizeof(req)); req.count = buf_count; req.type = buf_type; req.memory = V4L2_MEMORY_MMAP; ioctl(fd, VIDIOC_REQBUFS, &req);
        buffers.resize(req.count);
        for (uint32_t i = 0; i < req.count; ++i) {
            struct v4l2_buffer buf; struct v4l2_plane planes[1]; memset(&buf, 0, sizeof(buf)); memset(planes, 0, sizeof(planes));
            buf.type = buf_type; buf.memory = V4L2_MEMORY_MMAP; buf.index = i; if (is_mplane) { buf.length = 1; buf.m.planes = planes; }
            if(ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) return false;
            size_t length = is_mplane ? buf.m.planes[0].length : buf.length;
            off_t offset = is_mplane ? buf.m.planes[0].m.mem_offset : buf.m.offset;
            buffers[i].length = length; buffers[i].start = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
            struct v4l2_exportbuffer expbuf; memset(&expbuf, 0, sizeof(expbuf)); expbuf.type = buf_type; expbuf.index = i;
            if (ioctl(fd, VIDIOC_EXPBUF, &expbuf) == 0) buffers[i].dma_fd = expbuf.fd;
            ioctl(fd, VIDIOC_QBUF, &buf);
        }
        return true;
    }
    bool start_stream() { return ioctl(fd, VIDIOC_STREAMON, &buf_type) == 0; }
    bool get_frame(int& index, int& dma_fd) {
        struct v4l2_buffer buf; struct v4l2_plane planes[1]; memset(&buf, 0, sizeof(buf)); memset(planes, 0, sizeof(planes));
        buf.type = buf_type; buf.memory = V4L2_MEMORY_MMAP; if (is_mplane) { buf.length = 1; buf.m.planes = planes; }
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) return false;
        index = buf.index; dma_fd = buffers[index].dma_fd; return true;
    }
    bool return_frame(int index) {
        struct v4l2_buffer buf; struct v4l2_plane planes[1]; memset(&buf, 0, sizeof(buf)); memset(planes, 0, sizeof(planes));
        buf.type = buf_type; buf.memory = V4L2_MEMORY_MMAP; buf.index = index; if (is_mplane) { buf.length = 1; buf.m.planes = planes; }
        return ioctl(fd, VIDIOC_QBUF, &buf) == 0;
    }
    void cleanup() { if (fd >= 0) { ioctl(fd, VIDIOC_STREAMOFF, &buf_type); close(fd); fd = -1; } }
};

void process_with_rga(int src_dma_fd, int src_w, int src_h, int src_format, int dst_format, cv::Mat& dst_mat) {
    rga_buffer_t src = wrapbuffer_fd(src_dma_fd, src_w, src_h, src_format);
    rga_buffer_t dst = wrapbuffer_virtualaddr(dst_mat.data, dst_mat.cols, dst_mat.rows, dst_format);
    im_rect src_rect = {0, 0, src_w, src_h}; im_rect dst_rect = {0, 0, dst_mat.cols, dst_mat.rows};
    improcess(src, dst, {}, src_rect, dst_rect, {}, IM_SYNC);
}

// --- 瑞芯微 VPU 硬件 JPEG 编码器 (零 CPU 消耗) ---
class VPUJpegEncoder {
private:
    MppCtx ctx;
    MppApi *mpi;
    int width, height;
    MppBufferGroup group;

public:
    VPUJpegEncoder() : ctx(NULL), mpi(NULL), group(NULL) {}
    ~VPUJpegEncoder() {
        if (ctx) { mpp_destroy(ctx); ctx = NULL; }
        if (group) { mpp_buffer_group_put(group); group = NULL; }
    }

    bool init(int w, int h) {
        width = w; height = h;
        if (mpp_create(&ctx, &mpi) != MPP_OK) return false;
        mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG);

        // 配置编码器硬件参数
        MppEncCfg cfg;
        mpp_enc_cfg_init(&cfg);
        mpp_enc_cfg_set_s32(cfg, "prep:width", width);
        mpp_enc_cfg_set_s32(cfg, "prep:height", height);
        mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", width);
        mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", height);
        mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_BGR888); // 直接吃 OpenCV 的 BGR 数据
        mpp_enc_cfg_set_s32(cfg, "rc:quality", 80);              // JPEG 压缩质量
        mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
        mpp_enc_cfg_deinit(cfg);

        // 初始化物理内存分配器 (ION/DRM)
        mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_ION);
        return true;
    }

    // 硬件压帧并原子写入
    bool encode_and_save(cv::Mat& img, const std::string& tmp_path, const std::string& real_path) {
        MppFrame frame = NULL;
        MppPacket packet = NULL;
        MppBuffer mpp_buf = NULL;

        mpp_frame_init(&frame);
        mpp_frame_set_width(frame, width);
        mpp_frame_set_height(frame, height);
        mpp_frame_set_hor_stride(frame, width);
        mpp_frame_set_ver_stride(frame, height);
        mpp_frame_set_fmt(frame, MPP_FMT_BGR888);

        // 内存挂载到 VPU 总线
        mpp_buffer_get(group, &mpp_buf, width * height * 3);
        memcpy(mpp_buffer_get_ptr(mpp_buf), img.data, width * height * 3);
        mpp_frame_set_buffer(frame, mpp_buf);
        mpp_frame_set_eos(frame, 1); // 一帧即结束 (JPEG)

        // 🚀 发送给 VPU 硬件压缩！
        mpi->encode_put_frame(ctx, frame);
        mpi->encode_get_packet(ctx, &packet);

        // 获取压缩后的极小体积 JPEG 数据并直接刷入磁盘
        if (packet) {
            FILE* f = fopen(tmp_path.c_str(), "wb");
            if (f) {
                fwrite(mpp_packet_get_pos(packet), 1, mpp_packet_get_length(packet), f);
                fclose(f);
                rename(tmp_path.c_str(), real_path.c_str());
            }
            mpp_packet_deinit(&packet);
        }

        mpp_frame_deinit(&frame);
        mpp_buffer_put(mpp_buf);
        return true;
    }
};

class YoloNPU {
private:
    rknn_context ctx;
    VPUJpegEncoder vpu_encoder;
    const float CONF_THRESH = 0.5f; // 调高门限，进一步防止绿屏
    const float NMS_THRESH = 0.45f;
    const int STRIDES[3] = {8, 16, 32};
    const int GRIDS[3] = {80, 40, 20};
    const float ANCHORS[3][3][2] = {{{10,13}, {16,30}, {33,23}}, {{30,61}, {62,45}, {59,119}}, {{116,90}, {156,198}, {373,326}}};

public:
    YoloNPU() : ctx(0) {}
    ~YoloNPU() { if (ctx > 0) rknn_destroy(ctx); }

    bool init(const std::string& model_path, rknn_core_mask core, int w, int h) {
        std::ifstream file(model_path, std::ios::binary | std::ios::ate);
        if(!file.is_open()) return false;
        std::streamsize size = file.tellg(); file.seekg(0, std::ios::beg);
        std::vector<char> buffer(size); file.read(buffer.data(), size);
        if (rknn_init(&ctx, buffer.data(), size, 0, NULL) != RKNN_SUCC) return false;
        rknn_set_core_mask(ctx, core);
        
        // 传递给底层的 MPP 硬件编码器
        vpu_encoder.init(w, h);
        return true;
    }

    void detect(cv::Mat& img, const std::string& label, bool swap) {
        rknn_input inputs[1]; memset(inputs, 0, sizeof(inputs));
        inputs[0].index = 0; inputs[0].type = RKNN_TENSOR_UINT8; inputs[0].fmt = RKNN_TENSOR_NHWC;
        inputs[0].size = img.cols * img.rows * 3; inputs[0].buf = img.data;
        rknn_inputs_set(ctx, 1, inputs);
        if(rknn_run(ctx, NULL) != RKNN_SUCC) return;

        rknn_output outputs[3]; memset(outputs, 0, sizeof(outputs));
        for(int i=0; i<3; i++) { outputs[i].want_float = 1; }
        rknn_outputs_get(ctx, 3, outputs, NULL);

        std::vector<Object> proposals;
        for (int i = 0; i < 3; i++) {
            float* data = (float*)outputs[i].buf;
            int grid = GRIDS[i], area = grid * grid;
            for (int a = 0; a < 3; a++) {
                for (int h = 0; h < grid; h++) {
                    for (int w = 0; w < grid; w++) {
                        float box_conf = sigmoid(data[(a * 85 + 4) * area + h * grid + w]);
                        if (box_conf < CONF_THRESH) continue;
                        
                        float max_conf = 0; int cls = -1;
                        for (int c = 0; c < 80; c++) {
                            float cc = sigmoid(data[(a * 85 + 5 + c) * area + h * grid + w]);
                            if (cc > max_conf) { max_conf = cc; cls = c; }
                        }
                        if (box_conf * max_conf < CONF_THRESH) continue;

                        Object obj;
                        float bx = sigmoid(data[(a * 85 + 0) * area + h * grid + w]);
                        float by = sigmoid(data[(a * 85 + 1) * area + h * grid + w]);
                        float bw = sigmoid(data[(a * 85 + 2) * area + h * grid + w]);
                        float bh = sigmoid(data[(a * 85 + 3) * area + h * grid + w]);
                        obj.x = (bx * 2.f - 0.5f + w) * STRIDES[i] - (std::pow(bw * 2.f, 2) * ANCHORS[i][a][0]) / 2.f;
                        obj.y = (by * 2.f - 0.5f + h) * STRIDES[i] - (std::pow(bh * 2.f, 2) * ANCHORS[i][a][1]) / 2.f;
                        obj.w = std::pow(bw * 2.f, 2) * ANCHORS[i][a][0];
                        obj.h = std::pow(bh * 2.f, 2) * ANCHORS[i][a][1];
                        obj.prob = box_conf * max_conf; obj.class_id = cls;
                        proposals.push_back(obj);
                    }
                }
            }
        }
        rknn_outputs_release(ctx, 3, outputs);

        std::sort(proposals.begin(), proposals.end(), [](const Object& a, const Object& b){return a.prob > b.prob;});
        std::vector<Object> res;
        for (auto& p : proposals) {
            bool keep = true;
            for (auto& r : res) { if (p.class_id == r.class_id && calc_iou(p, r) > NMS_THRESH) { keep = false; break; } }
            if (keep && res.size() < 20) res.push_back(p); // 🛡️ 强制熔断：每帧最多只画 20 个框！
        }

        if (swap) cv::cvtColor(img, img, cv::COLOR_BGR2RGB);
        for (auto& o : res) {
            cv::rectangle(img, cv::Rect(o.x, o.y, o.w, o.h), cv::Scalar(0, 255, 0), 2);
            cv::putText(img, COCO_CLASSES[o.class_id], cv::Point(o.x, o.y-5), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);
        }
        std::string tmp = "/dev/shm/"+label+"_tmp.jpg", real = "/dev/shm/"+label+"_result.jpg";
        cv::imwrite(tmp, img); 
        rename(tmp.c_str(), real.c_str());
    }
};

int main() {
    V4L2Camera mipi("/dev/video11"), usb("/dev/video20");
    std::cout << ">> 启动 MIPI..." << (mipi.init(V4L2_PIX_FMT_NV12, 1280, 720) ? "✅" : "❌") << std::endl;
    std::cout << ">> 启动 USB..." << (usb.init(V4L2_PIX_FMT_YUYV, 640, 480) ? "✅" : "❌") << std::endl;

    YoloNPU n0, n1;
    if(!n0.init("./yolov5s.rknn", RKNN_NPU_CORE_0, 640, 640)) { std::cerr << "MIPI NPU Init Failed\n"; return -1; };
    if(!n1.init("./yolov5s.rknn", RKNN_NPU_CORE_1, 640, 640)) { std::cerr << "USB NPU Init Failed\n"; return -1; };
    
    mipi.start_stream(); usb.start_stream();
    cv::Mat m_img(640, 640, CV_8UC3), u_img(640, 640, CV_8UC3);

    std::cout << "\n🚀 终极诊断版启动！(限流防绿屏)\n";
    while(true) {
        int idx, fd;
        if (mipi.get_frame(idx, fd)) {
            // 1. 🔑 恢复为正确的 YCbCr 宏！让 U 和 V 归位！
            process_with_rga(fd, 1280, 720, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888, m_img);
            
            // 2. 🔑 传入 false！坚决不让 OpenCV 在软件层面去瞎翻转通道！
            n0.detect(m_img, "MIPI", false); 
            
            mipi.return_frame(idx);
	} else {
            std::cout << "⚠️ MIPI 丢帧" << std::endl;
        }
        if (usb.get_frame(idx, fd)) {
            process_with_rga(fd, 640, 480, RK_FORMAT_YUYV_422, RK_FORMAT_BGR_888, u_img);
            n1.detect(u_img, "USB", false); usb.return_frame(idx);
        } else {
            static int err_cnt = 0;
            if (err_cnt++ % 100 == 0) std::cout << "⚠️ USB 丢帧，检查连接或权限" << std::endl;
        }
        usleep(10000);
    }
    return 0;
}
