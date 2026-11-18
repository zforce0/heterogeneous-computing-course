// 红外相机采集模块(基于 twocameraOnMAC ir_camera.cpp 精简):
//   - 仅保留 ImageOnly 链路:uvc_frame_get → raw_data_cut → YUY2→RGB→BGR
//   - 去掉测温/伪彩/增强/增益切换/几何变换等全部无关逻辑
//   - 加入帧率计数(FpsCounter)与最近 8 帧环形缓冲(配对用)

#include "ir_camera.h"

#include "ir_sdk/libirparse.h"
#include "perf_clock.h"

#include <cstdio>
#include <cstring>

IrCamera::IrCamera()
    : sdk_initialized_(false),
      camera_opened_(false),
      stream_started_(false),
      running_(false),
      image_width_(0),
      image_height_(0),
      image_byte_size_(0),
      raw_frame_(NULL),
      ring_(8),
      sequence_(0)
{
    std::memset(&camera_param_, 0, sizeof(camera_param_));
}

IrCamera::~IrCamera()
{
    close();
}

bool IrCamera::open(const IrCameraConfig& config)
{
    close();
    config_ = config;

    iruvc_log_register(IRUVC_LOG_ERROR);
    irparse_log_register(IRPARSE_LOG_ERROR);

    const int result = uvc_camera_init();
    if (result < 0) {
        std::fprintf(stderr, "uvc_camera_init failed: %d\n", result);
        return false;
    }
    sdk_initialized_ = true;

    if (!select_and_open_device()) {
        close();
        return false;
    }
    return true;
}

bool IrCamera::select_and_open_device()
{
    DevCfg_t devices[64] = {};
    CameraStreamInfo_t stream_info[32] = {};

    int result = uvc_camera_list(devices);
    if (result < 0) {
        std::fprintf(stderr, "uvc_camera_list failed: %d\n", result);
        return false;
    }

    int device_index = -1;
    for (int i = 0; i < 64; ++i) {
        if (devices[i].vid == config_.vendor_id &&
            devices[i].pid == config_.product_id) {
            device_index = i;
            break;
        }
    }
    if (device_index < 0) {
        std::fprintf(stderr, "IR camera %04x:%04x was not found\n",
                     config_.vendor_id, config_.product_id);
        return false;
    }

    result = uvc_camera_info_get(devices[device_index], stream_info);
    if (result < 0) {
        std::fprintf(stderr, "uvc_camera_info_get failed: %d\n", result);
        return false;
    }

    result = uvc_camera_open(devices[device_index]);
    if (result < 0) {
        std::fprintf(stderr, "uvc_camera_open failed: %d\n", result);
        std::fprintf(stderr,
                     "check access to raw USB %04x:%04x (run as root)\n",
                     config_.vendor_id, config_.product_id);
        return false;
    }
    camera_opened_ = true;
    camera_param_.dev_cfg = devices[device_index];

    if (!select_stream(stream_info)) {
        return false;
    }

    std::printf("IR camera: %s, %ux%u@%u, format=%s (combined stream %ux%u)\n",
                devices[device_index].name != NULL
                    ? devices[device_index].name
                    : "unknown",
                image_width_, image_height_,
                camera_param_.fps,
                camera_param_.format != NULL ? camera_param_.format
                                            : "unknown",
                camera_param_.width, camera_param_.height);
    return true;
}

bool IrCamera::select_stream(CameraStreamInfo_t stream_info[])
{
    if (stream_info[0].width == 0 || stream_info[0].height == 0) {
        std::fprintf(stderr,
                     "IR camera returned no stream information\n");
        return false;
    }

    // 设备实际只输出 image+temp 上下拼接的组合流(高=单帧高*2),
    // raw_data_cut 再从中切出图像段;单高流描述符虽存在但取帧为 NULL
    const unsigned int target_height = stream_info[0].height * 2;
    int stream_index = -1;
    int height_fallback = -1;
    for (int i = 0; i < 32; ++i) {
        if (stream_info[i].width == 0 || stream_info[i].height == 0) {
            break;
        }
        if (stream_info[i].height == target_height && height_fallback < 0) {
            height_fallback = i;
        }
        if (stream_info[i].height == target_height &&
            stream_info[i].format != NULL &&
            std::strcmp(stream_info[i].format, FORMAT_YUY2) == 0) {
            stream_index = i;
            break;
        }
    }

    if (stream_index < 0) {
        stream_index = height_fallback;
    }
    if (stream_index < 0) {
        std::fprintf(stderr, "no matching IR combined stream\n");
        return false;
    }

    camera_param_.format = stream_info[stream_index].format;
    camera_param_.width = stream_info[stream_index].width;
    camera_param_.height = stream_info[stream_index].height;
    camera_param_.frame_size = camera_param_.width * camera_param_.height * 2;
    camera_param_.fps = stream_info[stream_index].fps[0];
    camera_param_.timeout_ms_delay = 1000;

    // 输出图像为组合流的上半段
    image_width_ = static_cast<int>(camera_param_.width);
    image_height_ = static_cast<int>(camera_param_.height / 2);
    image_byte_size_ =
        static_cast<uint32_t>(image_width_ * image_height_ * 2);
    return true;
}

bool IrCamera::start()
{
    if (!camera_opened_ || stream_started_) {
        return false;
    }

    const size_t pixels =
        static_cast<size_t>(image_width_) * image_height_;
    image_frame_.resize(image_byte_size_);
    temp_frame_.resize(image_byte_size_);
    rgb_work_.resize(pixels * 3);
    bgr_work_.resize(pixels * 3);

    raw_frame_ = uvc_frame_buf_create(camera_param_);
    if (raw_frame_ == NULL) {
        std::fprintf(stderr, "uvc_frame_buf_create failed\n");
        return false;
    }

    running_.store(true);
    const int result = uvc_camera_stream_start(camera_param_, NULL);
    if (result < 0) {
        running_.store(false);
        std::fprintf(stderr, "uvc_camera_stream_start failed: %d\n",
                     result);
        uvc_frame_buf_release(raw_frame_);
        raw_frame_ = NULL;
        return false;
    }
    stream_started_ = true;

    capture_thread_ = std::thread(&IrCamera::capture_loop, this);
    return true;
}

void IrCamera::capture_loop()
{
    int consecutive_errors = 0;
    while (running_.load()) {
        const int result = uvc_frame_get(raw_frame_);
        if (!running_.load()) {
            break;
        }
        if (result < 0) {
            ++consecutive_errors;
            if (consecutive_errors == 3) {
                std::fprintf(stderr,
                             "uvc_frame_get failed repeatedly: %d\n",
                             result);
            }
            continue;
        }
        consecutive_errors = 0;

        const double t_now = now_ms();

        // 从组合帧中切出图像段与测温段(测温段不再使用)
        if (raw_data_cut(static_cast<uint8_t*>(raw_frame_),
                         static_cast<int>(image_byte_size_),
                         static_cast<int>(image_byte_size_),
                         image_frame_.data(),
                         temp_frame_.data()) != IRPARSE_SUCCESS) {
            std::fprintf(stderr, "raw_data_cut failed\n");
            continue;
        }

        const int pixel_count = image_width_ * image_height_;
        if (yuv422_to_rgb(image_frame_.data(), pixel_count,
                          rgb_work_.data()) != IRPARSE_SUCCESS) {
            std::fprintf(stderr, "yuv422_to_rgb failed\n");
            continue;
        }
        if (rgb_to_bgr(rgb_work_.data(), pixel_count,
                       bgr_work_.data()) != IRPARSE_SUCCESS) {
            std::fprintf(stderr, "rgb_to_bgr failed\n");
            continue;
        }

        BgrFrame frame;
        frame.resize(image_width_, image_height_);
        std::memcpy(frame.data.data(), bgr_work_.data(),
                    frame.data.size());
        frame.timestamp_us = static_cast<uint64_t>(t_now * 1000.0);
        frame.sequence = ++sequence_;

        {
            std::lock_guard<std::mutex> lock(latest_mutex_);
            latest_frame_ = frame;
            ring_.push(frame);
            fps_counter_.tick(t_now);
        }
    }
}

FrameRateInfo IrCamera::fps_info() const
{
    std::lock_guard<std::mutex> lock(latest_mutex_);
    return fps_counter_.info(now_ms());
}

bool IrCamera::nearest_frame(uint64_t timestamp_us, BgrFrame& out,
                             double& delta_ms) const
{
    std::lock_guard<std::mutex> lock(latest_mutex_);
    return ring_.nearest(timestamp_us, out, delta_ms);
}

void IrCamera::close()
{
    running_.store(false);

    if (stream_started_) {
        // 彻底关闭相机侧预览并停止 ISO 流,释放 USB 带宽
        uvc_camera_stream_close(CLOSE_CAM_SIDE_PREVIEW);
        stream_started_ = false;
    }
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    if (raw_frame_ != NULL) {
        uvc_frame_buf_release(raw_frame_);
        raw_frame_ = NULL;
    }
    if (camera_opened_) {
        uvc_camera_close();
        camera_opened_ = false;
    }
    if (sdk_initialized_) {
        uvc_camera_release();
        sdk_initialized_ = false;
    }

    image_frame_.clear();
    temp_frame_.clear();
    rgb_work_.clear();
    bgr_work_.clear();
    ring_ = FrameRing(8);
    fps_counter_ = FpsCounter();
    {
        std::lock_guard<std::mutex> lock(latest_mutex_);
        latest_frame_ = BgrFrame();
    }
    std::memset(&camera_param_, 0, sizeof(camera_param_));
    image_width_ = 0;
    image_height_ = 0;
    image_byte_size_ = 0;
    sequence_ = 0;
}
