#pragma once

#include "frame.h"
#if !defined(TWOCAMERA_NO_JPEG)
#include "mjpeg_decoder.h"
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class V4l2CaptureFormat {
    Mjpeg,
    Yuyv
};

struct VisibleCameraConfig {
    std::string device_path;
    unsigned int vendor_id = 0x1D6C;
    unsigned int product_id = 0x0103;
    int width = 1920;
    int height = 1080;
    int fps = 30;
    unsigned int buffer_count = 4;
    V4l2CaptureFormat format = V4l2CaptureFormat::Mjpeg;
};

struct VisibleCaptureStats {
    uint64_t timestamp_us = 0;
    uint64_t sequence = 0;
    size_t bytes_used = 0;
    uint32_t sample_checksum = 0;
    bool payload_valid = false;
    bool jpeg_markers_valid = false;
};

class VisibleCamera : public IFrameSource {
public:
    VisibleCamera();
    ~VisibleCamera() override;

    bool open(const VisibleCameraConfig& config);
    bool start();
    void close();
    bool copy_latest(BgrFrame& destination) override;
    bool copy_latest_stats(VisibleCaptureStats& destination);

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
#if !defined(TWOCAMERA_NO_JPEG)
    MjpegDecoder decoder_;
    BgrFrame decode_frame_;
#else
    BgrFrame convert_frame_;
#endif

    std::mutex latest_mutex_;
    BgrFrame latest_frame_;
    uint64_t sequence_;
    std::mutex stats_mutex_;
    VisibleCaptureStats latest_stats_;
    uint64_t capture_sequence_;
};
