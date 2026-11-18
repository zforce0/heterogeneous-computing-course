#pragma once

#include "frame.h"
#include "ir_sdk/libiruvc.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// 红外相机配置:仅取原始图像,不开镜像/旋转/伪彩/增益切换
struct IrCameraConfig {
    unsigned int vendor_id = 0x0BDA;
    unsigned int product_id = 0x5840;
};

// 红外相机(闭源 SDK libiruvc + libusb):
//   open() 经 SDK 按 VID/PID 枚举并打开原始 USB 设备
//   start() 先启动 ISO 流,采集线程拉帧 raw_data_cut → YUY2→BGR
// 帧率计数在采集线程内完成;最近 8 帧环形缓冲供与可见光最近邻配对
class IrCamera {
public:
    IrCamera();
    ~IrCamera();

    bool open(const IrCameraConfig& config);
    bool start();
    void close();

    FrameRateInfo fps_info() const;
    // 在近期帧中取与 timestamp_us 最接近的帧
    bool nearest_frame(uint64_t timestamp_us, BgrFrame& out,
                       double& delta_ms) const;
    // 最新图像+温度帧(同一组合流产出, 时间戳/序号一致)
    bool copy_latest(BgrFrame& image, ThermalFrame& temp) const;
    // 在近期帧中取与 timestamp_us 最接近的 图像+温度 对
    bool nearest_pair(uint64_t timestamp_us, BgrFrame& image,
                      ThermalFrame& temp, double& delta_ms) const;

    int width() const { return image_width_; }
    int height() const { return image_height_; }
    int fps() const { return static_cast<int>(camera_param_.fps); }

private:
    bool select_and_open_device();
    bool select_stream(CameraStreamInfo_t stream_info[]);
    void capture_loop();

    IrCameraConfig config_;
    CameraParam_t camera_param_;
    bool sdk_initialized_;
    bool camera_opened_;
    bool stream_started_;
    std::atomic<bool> running_;
    std::thread capture_thread_;

    int image_width_;
    int image_height_;
    uint32_t image_byte_size_;
    void* raw_frame_;
    std::vector<uint8_t> image_frame_;
    std::vector<uint8_t> temp_frame_;   // 组合流的测温段(Y16, K*64)
    std::vector<uint8_t> rgb_work_;
    std::vector<uint8_t> bgr_work_;

    mutable std::mutex latest_mutex_;
    BgrFrame latest_frame_;
    ThermalFrame latest_temp_;          // 与 latest_frame_ 同步
    FrameRing ring_;
    std::deque<ThermalFrame> temp_ring_;   // 与 ring_ 同步 push, 容量 8
    FpsCounter fps_counter_;
    uint64_t sequence_;
};
