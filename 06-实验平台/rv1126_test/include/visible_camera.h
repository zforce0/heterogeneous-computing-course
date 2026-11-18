#pragma once

#include "frame.h"
#include "mjpeg_decoder.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class V4l2CaptureFormat {
    Mjpg,
    Yuyv
};

struct VisibleCameraConfig {
    std::string device_path;              // 空 = 按 USB VID/PID 自动探测
    unsigned int vendor_id = 0x1D6C;      // 可见光摄像头 VID
    unsigned int product_id = 0x0103;     // 可见光摄像头 PID
    int width = 1280;
    int height = 720;
    int fps = 25;
    unsigned int buffer_count = 4;
    V4l2CaptureFormat format = V4l2CaptureFormat::Mjpg;
};

// V4L2 MMAP 流式采集:独立采集线程持续解码,
// copy_latest() 总是返回最新一帧;内置帧率计数与近期帧环形缓冲
class VisibleCamera {
public:
    VisibleCamera();
    ~VisibleCamera();

    bool open(const VisibleCameraConfig& config);
    bool start();
    void stop();
    bool copy_latest(BgrFrame& destination);

    FrameRateInfo fps_info() const;

    const std::string& device_path() const { return device_path_; }
    int width() const { return actual_width_; }
    int height() const { return actual_height_; }
    int fps() const { return actual_fps_; }

private:
    struct MappedBuffer {
        void* address = NULL;
        size_t length = 0;
    };

    bool find_usb_video_device(std::string& path) const;
    bool open_device(const std::string& path);
    bool configure_device();
    bool map_buffers();
    void unmap_buffers();
    void close();
    void capture_loop();

    VisibleCameraConfig config_;
    std::string device_path_;
    int fd_;
    int actual_width_;
    int actual_height_;
    int actual_fps_;
    bool stream_started_;
    std::vector<MappedBuffer> buffers_;

    std::atomic<bool> running_;
    std::thread capture_thread_;
    MjpegDecoder decoder_;
    BgrFrame work_frame_;

    mutable std::mutex latest_mutex_;
    BgrFrame latest_frame_;
    FrameRing ring_;
    FpsCounter fps_counter_;
    uint64_t sequence_;
};
