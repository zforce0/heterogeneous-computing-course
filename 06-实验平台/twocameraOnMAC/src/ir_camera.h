#pragma once

#include "frame.h"
#include "libiruvc.h"
#include "sample_config.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

enum class IrRotation {
    None,
    Left90,
    Right90,
    Rotate180
};

enum class IrStreamMode {
    ImageAndTemperature,
    ImageOnly,
    TemperatureOnly
};

typedef void (*IrRawFrameCallback)(
    const uint8_t* frame, size_t size, void* user_data);

struct IrCameraConfig {
    unsigned int vendor_id = 0x0BDA;
    unsigned int product_id = 0x5840;
    bool mirror = false;
    bool flip = false;
    IrRotation rotation = IrRotation::None;
#if defined(IMAGE_AND_TEMP_OUTPUT)
    IrStreamMode stream_mode = IrStreamMode::ImageAndTemperature;
#elif defined(IMAGE_OUTPUT)
    IrStreamMode stream_mode = IrStreamMode::ImageOnly;
#else
    IrStreamMode stream_mode = IrStreamMode::TemperatureOnly;
#endif
#if defined(IR_IMAGE_ENHANCE)
    bool image_enhance = true;
#else
    bool image_enhance = false;
#endif
#if defined(IR_PSEUDO_COLOR)
    bool pseudo_color = true;
#else
    bool pseudo_color = false;
#endif
#if defined(AUTO_GAIN_SWITCH)
    bool auto_gain_switch = true;
#else
    bool auto_gain_switch = false;
#endif
#if defined(AVOID_OVEREXPOSURE)
    bool avoid_overexposure = true;
#else
    bool avoid_overexposure = false;
#endif
    bool sdk_callback_mode = false;
    IrRawFrameCallback raw_frame_callback = NULL;
    void* raw_frame_user_data = NULL;
};

class IrCamera : public IFrameSource,
                 public ITemperatureSource {
public:
    IrCamera();
    ~IrCamera() override;

    bool open(const IrCameraConfig& config);
    bool start();
    void close();
    bool copy_latest(BgrFrame& destination) override;
    bool copy_latest_temperature(
        TemperatureFrame& destination) override;

    int width() const { return image_width_; }
    int height() const { return image_height_; }
    int fps() const { return static_cast<int>(camera_param_.fps); }

private:
    bool select_and_open_device();
    bool select_stream(CameraStreamInfo_t stream_info[]);
    bool process_raw_frame(const uint8_t* frame,
                           uint64_t timestamp_us);
    bool process_image_frame(uint64_t timestamp_us);
    void publish_temperature_frame(uint64_t timestamp_us);
    void run_auto_gain_switch();
    void run_overexposure_protection();
    bool apply_geometry(uint8_t*& frame_data, int& output_width,
                        int& output_height);
    void capture_loop();
    static void sdk_frame_callback(void* frame, void* user_data);

    IrCameraConfig config_;
    CameraParam_t camera_param_;
    UserCallback_t sdk_callback_;
    bool sdk_initialized_;
    bool camera_opened_;
    bool stream_started_;
    bool y16_preview_started_;
    std::atomic<bool> running_;
    std::thread capture_thread_;

    int image_width_;
    int image_height_;
    uint32_t image_byte_size_;
    uint32_t temp_byte_size_;
    void* raw_frame_;
    std::vector<uint8_t> image_frame_;
    std::vector<uint8_t> temp_frame_;
    std::vector<uint8_t> rgb_work_;
    std::vector<uint8_t> bgr_work_;
    std::vector<uint8_t> geometry_work_;
    std::vector<uint8_t> pseudo_yuv_work_;
    std::vector<uint16_t> y14_work_;
    std::vector<uint16_t> enhanced_y14_work_;

    std::mutex latest_mutex_;
    BgrFrame latest_frame_;
    std::mutex temperature_mutex_;
    TemperatureFrame latest_temperature_;
    uint64_t sequence_;
    uint64_t temperature_sequence_;
    int gain_low_detected_frames_;
    int gain_high_detected_frames_;
    int gain_waiting_frames_;
    bool gain_was_switched_;
    int overexposure_frame_count_;
    bool shutter_was_closed_;
};
