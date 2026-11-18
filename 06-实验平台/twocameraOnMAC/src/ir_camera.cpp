#include "ir_camera.h"

#include "libirparse.h"
#include "libirprocess.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

namespace {

uint64_t monotonic_time_us()
{
    const std::chrono::steady_clock::duration now =
        std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            now).count());
}

const char* stream_mode_name(IrStreamMode mode)
{
    switch (mode) {
    case IrStreamMode::ImageAndTemperature:
        return "image+temperature";
    case IrStreamMode::ImageOnly:
        return "image";
    case IrStreamMode::TemperatureOnly:
        return "temperature";
    }
    return "unknown";
}

} // namespace

IrCamera::IrCamera()
    : sdk_initialized_(false),
      camera_opened_(false),
      stream_started_(false),
      y16_preview_started_(false),
      running_(false),
      image_width_(0),
      image_height_(0),
      image_byte_size_(0),
      temp_byte_size_(0),
      raw_frame_(NULL),
      sequence_(0),
      temperature_sequence_(0),
      gain_low_detected_frames_(0),
      gain_high_detected_frames_(0),
      gain_waiting_frames_(0),
      gain_was_switched_(false),
      overexposure_frame_count_(0),
      shutter_was_closed_(false)
{
    std::memset(&camera_param_, 0, sizeof(camera_param_));
    std::memset(&sdk_callback_, 0, sizeof(sdk_callback_));
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
    irproc_log_register(IRPROC_LOG_ERROR);

    const int result = uvc_camera_init();
    if (result < 0) {
        std::fprintf(stderr, "uvc_camera_init failed: %d\n",
                     result);
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
        std::fprintf(stderr, "uvc_camera_list failed: %d\n",
                     result);
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
        std::fprintf(stderr,
                     "IR camera %04x:%04x was not found\n",
                     config_.vendor_id, config_.product_id);
        return false;
    }

    result = uvc_camera_info_get(devices[device_index],
                                 stream_info);
    if (result < 0) {
        std::fprintf(stderr,
                     "uvc_camera_info_get failed: %d\n",
                     result);
        return false;
    }

    result = uvc_camera_open(devices[device_index]);
    if (result < 0) {
        std::fprintf(stderr, "uvc_camera_open failed: %d\n",
                     result);
#if defined(__linux__)
        std::fprintf(
            stderr,
            "Check read/write access to the raw USB device "
            "%04x:%04x; install udev/70-twocamera.rules\n",
            config_.vendor_id, config_.product_id);
#endif
        return false;
    }
    camera_opened_ = true;
    camera_param_.dev_cfg = devices[device_index];

    if (!select_stream(stream_info)) {
        return false;
    }

    std::printf("IR camera: %s, %ux%u@%u, format=%s, mode=%s\n",
                devices[device_index].name != NULL
                    ? devices[device_index].name
                    : "unknown",
                camera_param_.width, camera_param_.height,
                camera_param_.fps,
                camera_param_.format != NULL
                    ? camera_param_.format
                    : "unknown",
                stream_mode_name(config_.stream_mode));
    return true;
}

bool IrCamera::select_stream(CameraStreamInfo_t stream_info[])
{
    if (stream_info[0].width == 0 ||
        stream_info[0].height == 0) {
        std::fprintf(stderr,
                     "IR camera returned no stream information\n");
        return false;
    }

    const unsigned int target_height =
        config_.stream_mode ==
                IrStreamMode::ImageAndTemperature
            ? stream_info[0].height * 2
            : stream_info[0].height;
    int stream_index = -1;
    int height_fallback = -1;
    for (int i = 0; i < 32; ++i) {
        if (stream_info[i].width == 0 ||
            stream_info[i].height == 0) {
            break;
        }
        std::printf("IR stream %d: %ux%u, format=%s\n", i,
                    stream_info[i].width,
                    stream_info[i].height,
                    stream_info[i].format != NULL
                        ? stream_info[i].format
                        : "unknown");
        if (stream_info[i].height == target_height &&
            height_fallback < 0) {
            height_fallback = i;
        }
        if (stream_info[i].height == target_height &&
            stream_info[i].format != NULL &&
            std::strcmp(stream_info[i].format,
                        FORMAT_YUY2) == 0) {
            stream_index = i;
            break;
        }
    }

    if (stream_index < 0) {
        stream_index = height_fallback;
    }
    if (stream_index < 0) {
        std::fprintf(
            stderr,
            "No stream matching infrared output mode %s\n",
            stream_mode_name(config_.stream_mode));
        return false;
    }

    camera_param_.format = stream_info[stream_index].format;
    camera_param_.width = stream_info[stream_index].width;
    camera_param_.height = stream_info[stream_index].height;
    camera_param_.frame_size =
        camera_param_.width * camera_param_.height * 2;
    camera_param_.fps = stream_info[stream_index].fps[0];
    camera_param_.timeout_ms_delay = 1000;

    image_width_ = static_cast<int>(camera_param_.width);
    image_height_ = static_cast<int>(
        config_.stream_mode ==
                IrStreamMode::ImageAndTemperature
            ? camera_param_.height / 2
            : camera_param_.height);
    image_byte_size_ = static_cast<uint32_t>(
        image_width_ * image_height_ * 2);
    temp_byte_size_ =
        config_.stream_mode ==
                IrStreamMode::ImageAndTemperature
            ? image_byte_size_
            : 0;
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
    if (config_.stream_mode ==
        IrStreamMode::ImageAndTemperature) {
        temp_frame_.resize(temp_byte_size_);
    } else if (config_.stream_mode ==
               IrStreamMode::TemperatureOnly) {
        temp_frame_.resize(image_byte_size_);
    }
    rgb_work_.resize(pixels * 3);
    bgr_work_.resize(pixels * 3);
    geometry_work_.resize(pixels * 3);
    pseudo_yuv_work_.resize(pixels * 2);
    y14_work_.resize(pixels);
    enhanced_y14_work_.resize(pixels);

    if (!config_.sdk_callback_mode) {
        std::fprintf(stderr, "[IR] uvc_frame_buf_create()...\n");
        raw_frame_ = uvc_frame_buf_create(camera_param_);
        if (raw_frame_ == NULL) {
            std::fprintf(stderr,
                         "uvc_frame_buf_create failed\n");
            return false;
        }
        std::fprintf(stderr, "[IR] uvc_frame_buf_create OK\n");
    }

    running_.store(true);
    UserCallback_t* callback = NULL;
    if (config_.sdk_callback_mode) {
        sdk_callback_.usr_func = reinterpret_cast<void*>(
            static_cast<void (*)(void*, void*)>(
                &IrCamera::sdk_frame_callback));
        sdk_callback_.usr_param = this;
        callback = &sdk_callback_;
    }

    std::fprintf(stderr, "[IR] uvc_camera_stream_start()...\n");
    const int result =
        uvc_camera_stream_start(camera_param_, callback);
    std::fprintf(stderr, "[IR] uvc_camera_stream_start returned %d\n",
                result);
    if (result < 0) {
        running_.store(false);
        std::fprintf(stderr,
                     "uvc_camera_stream_start failed: %d\n",
                     result);
        if (raw_frame_ != NULL) {
            uvc_frame_buf_release(raw_frame_);
            raw_frame_ = NULL;
        }
        return false;
    }
    stream_started_ = true;

    if (config_.stream_mode ==
        IrStreamMode::TemperatureOnly) {
        const int preview_result = y16_preview_start(
            PREVIEW_PATH0, Y16_MODE_TEMPERATURE);
        if (preview_result < 0) {
            std::fprintf(stderr,
                         "y16_preview_start failed: %d\n",
                         preview_result);
            close();
            return false;
        }
        y16_preview_started_ = true;
    }

    if (!config_.sdk_callback_mode) {
        capture_thread_ =
            std::thread(&IrCamera::capture_loop, this);
    }
    return true;
}

void IrCamera::sdk_frame_callback(void* frame, void* user_data)
{
    IrCamera* camera = static_cast<IrCamera*>(user_data);
    if (camera == NULL || frame == NULL ||
        !camera->running_.load()) {
        return;
    }
    camera->process_raw_frame(
        static_cast<const uint8_t*>(frame),
        monotonic_time_us());
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
                std::fprintf(
                    stderr,
                    "uvc_frame_get failed repeatedly: %d\n",
                    result);
            }
            continue;
        }
        consecutive_errors = 0;
        process_raw_frame(
            static_cast<const uint8_t*>(raw_frame_),
            monotonic_time_us());
    }
}

bool IrCamera::process_raw_frame(
    const uint8_t* frame, uint64_t timestamp_us)
{
    if (frame == NULL) {
        return false;
    }

    if (raw_data_cut(
            const_cast<uint8_t*>(frame),
            static_cast<int>(image_byte_size_),
            static_cast<int>(temp_byte_size_),
            image_frame_.data(),
            temp_frame_.empty()
                ? NULL
                : temp_frame_.data()) != IRPARSE_SUCCESS) {
        std::fprintf(stderr, "raw_data_cut failed\n");
        return false;
    }

    if (config_.stream_mode ==
        IrStreamMode::TemperatureOnly) {
        std::memcpy(temp_frame_.data(),
                    image_frame_.data(), image_byte_size_);
    }
    if (config_.stream_mode != IrStreamMode::ImageOnly) {
        publish_temperature_frame(timestamp_us);
        if (config_.auto_gain_switch) {
            run_auto_gain_switch();
        }
        if (config_.avoid_overexposure) {
            run_overexposure_protection();
        }
    }

    const bool processed =
        process_image_frame(timestamp_us);
    if (config_.raw_frame_callback != NULL) {
        config_.raw_frame_callback(
            frame, camera_param_.frame_size,
            config_.raw_frame_user_data);
    }
    return processed;
}

void IrCamera::publish_temperature_frame(uint64_t timestamp_us)
{
    if (temp_frame_.size() <
        static_cast<size_t>(image_width_) *
            image_height_ * sizeof(uint16_t)) {
        return;
    }

    std::lock_guard<std::mutex> lock(temperature_mutex_);
    latest_temperature_.resize(image_width_, image_height_);
    std::memcpy(
        latest_temperature_.data.data(),
        temp_frame_.data(),
        latest_temperature_.data.size() * sizeof(uint16_t));
    latest_temperature_.timestamp_us = timestamp_us;
    latest_temperature_.sequence = ++temperature_sequence_;
}

bool IrCamera::process_image_frame(uint64_t timestamp_us)
{
    const int pixel_count = image_width_ * image_height_;
    if (config_.stream_mode !=
        IrStreamMode::TemperatureOnly) {
        if (yuv422_to_rgb(
                image_frame_.data(), pixel_count,
                rgb_work_.data()) != IRPARSE_SUCCESS) {
            return false;
        }
    } else {
        std::memcpy(
            y14_work_.data(), image_frame_.data(),
            static_cast<size_t>(pixel_count) *
                sizeof(uint16_t));
        if (y16_to_y14(y14_work_.data(), pixel_count,
                       y14_work_.data()) != IRPARSE_SUCCESS) {
            return false;
        }

        uint16_t* y14_image = y14_work_.data();
        if (config_.image_enhance) {
            const int lower_limit =
                static_cast<int>((10.0 + 273.15) * 16);
            const int upper_limit =
                static_cast<int>((35.5 + 273.15) * 16);
            ImgEnhanceParam_t parameters = {
                {0.01f, 0.01f, 0.001f, 0.0001f, 150, 0,
                 0.25f,
                 {1,
                  static_cast<uint16_t>(lower_limit),
                  static_cast<uint16_t>(upper_limit)}},
                {0, 6, 2, 0, 10, 60, 120}
            };
            ImageRes_t resolution = {
                static_cast<uint16_t>(image_width_),
                static_cast<uint16_t>(image_height_)
            };
            if (y14_image_enhance(
                    y14_work_.data(), resolution, parameters,
                    enhanced_y14_work_.data()) !=
                IRPROC_SUCCESS) {
                return false;
            }
            y14_image = enhanced_y14_work_.data();
        }

        if (config_.pseudo_color) {
            if (y14_map_to_yuyv_pseudocolor(
                    y14_image, pixel_count,
                    IRPROC_COLOR_MODE_6,
                    pseudo_yuv_work_.data()) !=
                IRPROC_SUCCESS) {
                return false;
            }
            if (yuv422_to_rgb(
                    pseudo_yuv_work_.data(), pixel_count,
                    rgb_work_.data()) != IRPARSE_SUCCESS) {
                return false;
            }
        } else if (y14_to_rgb(
                       y14_image, pixel_count,
                       rgb_work_.data()) != IRPARSE_SUCCESS) {
            return false;
        }
    }

    if (rgb_to_bgr(rgb_work_.data(), pixel_count,
                   bgr_work_.data()) != IRPARSE_SUCCESS) {
        return false;
    }

    uint8_t* output = bgr_work_.data();
    int output_width = image_width_;
    int output_height = image_height_;
    if (!apply_geometry(output, output_width,
                        output_height)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(latest_mutex_);
    latest_frame_.resize(output_width, output_height);
    std::memcpy(latest_frame_.data.data(), output,
                latest_frame_.data.size());
    latest_frame_.timestamp_us = timestamp_us;
    latest_frame_.sequence = ++sequence_;
    return true;
}

void IrCamera::run_auto_gain_switch()
{
    if (temp_frame_.empty()) {
        return;
    }

    const int camera_fps =
        camera_param_.fps > 0
            ? static_cast<int>(camera_param_.fps)
            : 25;
    if (gain_was_switched_) {
        ++gain_waiting_frames_;
        if (gain_waiting_frames_ > 7 * camera_fps) {
            gain_was_switched_ = false;
            gain_waiting_frames_ = 0;
        }
        return;
    }

    ImageRes_t resolution = {
        static_cast<uint16_t>(image_width_),
        static_cast<uint16_t>(image_height_)
    };
    GainSwitchParam_t parameters = {
        0.1f,
        static_cast<int>((130 + 273.15) * 16 * 4),
        0.95f,
        static_cast<int>((110 + 273.15) * 16 * 4)
    };
    uint8_t detected = 0;
    if (gain_switch_detect(
            reinterpret_cast<uint16_t*>(temp_frame_.data()),
            resolution, parameters,
            &detected) != IRPROC_SUCCESS) {
        std::fprintf(stderr, "gain_switch_detect failed\n");
        return;
    }

    if (detected == 1) {
        ++gain_low_detected_frames_;
        gain_high_detected_frames_ = 0;
    } else if (detected == 2) {
        ++gain_high_detected_frames_;
        gain_low_detected_frames_ = 0;
    } else {
        gain_low_detected_frames_ = 0;
        gain_high_detected_frames_ = 0;
    }

    if (gain_low_detected_frames_ <= 5 * camera_fps &&
        gain_high_detected_frames_ <= 5 * camera_fps) {
        return;
    }

    gain_low_detected_frames_ = 0;
    gain_high_detected_frames_ = 0;
    gain_was_switched_ = true;
    gain_waiting_frames_ = 0;

    uint16_t current_gain = 0;
    if (get_prop_tpd_params(TPD_PROP_GAIN_SEL,
                            &current_gain) != IRUVC_SUCCESS) {
        std::fprintf(stderr,
                     "get gain property failed\n");
        return;
    }
    if (detected == 1 && current_gain != 0) {
        std::printf("Switch IR camera to low gain\n");
        set_prop_tpd_params(TPD_PROP_GAIN_SEL, 0);
    } else if (detected == 2 && current_gain != 1) {
        std::printf("Switch IR camera to high gain\n");
        set_prop_tpd_params(TPD_PROP_GAIN_SEL, 1);
    }
}

void IrCamera::run_overexposure_protection()
{
    if (temp_frame_.empty()) {
        return;
    }

    ++overexposure_frame_count_;
    if (overexposure_frame_count_ % 10 != 0) {
        return;
    }

    uint16_t current_gain = 0;
    if (get_prop_tpd_params(TPD_PROP_GAIN_SEL,
                            &current_gain) != IRUVC_SUCCESS) {
        std::fprintf(stderr,
                     "get gain property failed\n");
        return;
    }

    const int over_temperature =
        current_gain == 1
            ? (105 + 273) * 16 * 4
            : (550 + 273) * 16 * 4;
    ImageRes_t resolution = {
        static_cast<uint16_t>(image_width_),
        static_cast<uint16_t>(image_height_)
    };
    uint8_t detected = 0;
    if (overexposure_detect(
            reinterpret_cast<uint16_t*>(temp_frame_.data()),
            resolution, over_temperature, 0.02f,
            &detected) != IRPROC_SUCCESS) {
        std::fprintf(stderr,
                     "overexposure_detect failed\n");
        return;
    }

    if (detected == 1) {
        std::printf("IR overexposure: close shutter\n");
        shutter_manual_switch(SHUTTER_CLOSE);
        shutter_sta_set(SHUTTER_CTL_DIS);
        shutter_was_closed_ = true;
        return;
    }

    const int camera_fps =
        camera_param_.fps > 0
            ? static_cast<int>(camera_param_.fps)
            : 25;
    if (overexposure_frame_count_ > 10 * camera_fps) {
        if (shutter_was_closed_) {
            std::printf("IR exposure recovered: open shutter\n");
            shutter_sta_set(SHUTTER_CTL_EN);
            shutter_manual_switch(SHUTTER_OPEN);
            shutter_was_closed_ = false;
        }
        overexposure_frame_count_ = 0;
    }
}

bool IrCamera::apply_geometry(
    uint8_t*& frame_data, int& output_width,
    int& output_height)
{
    const ImageRes_t resolution = {
        static_cast<uint16_t>(image_width_),
        static_cast<uint16_t>(image_height_)
    };
    uint8_t* scratch = geometry_work_.data();

    if (config_.mirror) {
        if (mirror(frame_data, resolution,
                   IRPROC_SRC_FMT_BGR888,
                   scratch) != IRPROC_SUCCESS) {
            return false;
        }
        std::swap(frame_data, scratch);
    }
    if (config_.flip) {
        if (flip(frame_data, resolution,
                 IRPROC_SRC_FMT_BGR888,
                 scratch) != IRPROC_SUCCESS) {
            return false;
        }
        std::swap(frame_data, scratch);
    }

    irproc_error_t result = IRPROC_SUCCESS;
    switch (config_.rotation) {
    case IrRotation::Left90:
        result = rotate_left_90(
            frame_data, resolution,
            IRPROC_SRC_FMT_BGR888, scratch);
        output_width = image_height_;
        output_height = image_width_;
        break;
    case IrRotation::Right90:
        result = rotate_right_90(
            frame_data, resolution,
            IRPROC_SRC_FMT_BGR888, scratch);
        output_width = image_height_;
        output_height = image_width_;
        break;
    case IrRotation::Rotate180:
        result = rotate_180(
            frame_data, resolution,
            IRPROC_SRC_FMT_BGR888, scratch);
        break;
    case IrRotation::None:
        return true;
    }

    if (result != IRPROC_SUCCESS) {
        return false;
    }
    std::swap(frame_data, scratch);
    return true;
}

bool IrCamera::copy_latest(BgrFrame& destination)
{
    std::lock_guard<std::mutex> lock(latest_mutex_);
    if (!latest_frame_.valid()) {
        return false;
    }
    destination = latest_frame_;
    return true;
}

bool IrCamera::copy_latest_temperature(
    TemperatureFrame& destination)
{
    std::lock_guard<std::mutex> lock(temperature_mutex_);
    if (!latest_temperature_.valid()) {
        return false;
    }
    destination = latest_temperature_;
    return true;
}

void IrCamera::close()
{
    running_.store(false);

    if (stream_started_) {
        uvc_camera_stream_close(KEEP_CAM_SIDE_PREVIEW);
        stream_started_ = false;
    }
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    if (y16_preview_started_) {
        y16_preview_stop(PREVIEW_PATH0);
        y16_preview_started_ = false;
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
    geometry_work_.clear();
    pseudo_yuv_work_.clear();
    y14_work_.clear();
    enhanced_y14_work_.clear();
    {
        std::lock_guard<std::mutex> lock(latest_mutex_);
        latest_frame_ = BgrFrame();
    }
    {
        std::lock_guard<std::mutex> lock(temperature_mutex_);
        latest_temperature_ = TemperatureFrame();
    }
    std::memset(&camera_param_, 0, sizeof(camera_param_));
    std::memset(&sdk_callback_, 0, sizeof(sdk_callback_));
    image_width_ = 0;
    image_height_ = 0;
    image_byte_size_ = 0;
    temp_byte_size_ = 0;
    sequence_ = 0;
    temperature_sequence_ = 0;
    gain_low_detected_frames_ = 0;
    gain_high_detected_frames_ = 0;
    gain_waiting_frames_ = 0;
    gain_was_switched_ = false;
    overexposure_frame_count_ = 0;
    shutter_was_closed_ = false;
}
