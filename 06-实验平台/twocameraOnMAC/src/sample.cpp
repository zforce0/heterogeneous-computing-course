#include "cmd.h"
#include "display.h"
#include "ir_camera.h"
#include "libirparse.h"
#include "libirprocess.h"
#include "libirtemp.h"
#include "libiruvc.h"
#include "sample_config.h"
#include "synchronizer.h"
#include "temperature.h"
#include "visible_camera.h"

#include "image_registrator.h"
#include "side_by_side_compositor.h"

#if defined(TWOCAMERA_HAS_YOLO)
#include "detector_onnx.h"
#include "detector_yolov8.h"
#include "yolo_postprocess.h"
#if defined(TWOCAMERA_HAS_TRACKER)
#include "tracker_bytetrack.h"
#include "tracking_overlay.h"
#if NEW_TARGET_ALERT_ENABLE
#include "alert_notifier.h"
#endif
#endif
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/resource.h>
#include <thread>

#if defined(TWOCAMERA_HAS_OPENCV) && FPS_OVERLAY_ENABLE
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#endif

namespace {

volatile sig_atomic_t stop_requested = 0;

#if defined(TWOCAMERA_HAS_OPENCV) && FPS_OVERLAY_ENABLE
/* FPS 滑动窗口计算器: 用 EMA 平滑瞬时帧间隔, 避免显示抖动
 * 算法: fps = ema_alpha * (1e6 / dt) + (1-ema_alpha) * prev_fps
 * dt 是本次帧相对上一帧的 timestamp 差(us)
 * 首帧只更新 prev_timestamp 不更新 fps, 等下一帧再算
 */
struct FpsTracker {
    uint64_t prev_timestamp_us = 0;
    float fps = 0.0f;
    bool has_prev = false;

    void reset()
    {
        prev_timestamp_us = 0;
        fps = 0.0f;
        has_prev = false;
    }

    void update(uint64_t timestamp_us)
    {
        if (timestamp_us == 0) {
            return;
        }
        if (!has_prev) {
            prev_timestamp_us = timestamp_us;
            has_prev = true;
            return;
        }
        const int64_t dt =
            static_cast<int64_t>(timestamp_us) -
            static_cast<int64_t>(prev_timestamp_us);
        if (dt <= 0) {
            /* 时间戳倒退(相机重置/重连), 仅更新锚点不更新 fps */
            prev_timestamp_us = timestamp_us;
            return;
        }
        const float instant_fps =
            1.0e6f / static_cast<float>(dt);
        if (fps <= 0.0f) {
            fps = instant_fps;
        } else {
            fps = FPS_OVERLAY_EMA_ALPHA * instant_fps +
                  (1.0f - FPS_OVERLAY_EMA_ALPHA) * fps;
        }
        prev_timestamp_us = timestamp_us;
    }
};

/* 在 BgrFrame 上绘制带黑色描边的 FPS 文字, 任意背景都能看清 */
static void draw_fps_text(
    BgrFrame& frame, int origin_x, int origin_y,
    const char* text, uint8_t blue, uint8_t green, uint8_t red)
{
    cv::Mat image(frame.height, frame.width, CV_8UC3,
                  frame.data.data(),
                  static_cast<size_t>(frame.stride));
    const int font_face = cv::FONT_HERSHEY_SIMPLEX;
    const double scale = FPS_OVERLAY_FONT_SCALE;
    const int thickness = FPS_OVERLAY_FONT_THICKNESS;
    int baseline = 0;
    const cv::Size text_size = cv::getTextSize(
        text, font_face, scale, thickness, &baseline);

    /* 黑色描边: 在文字四周画 8 个偏移副本, 形成黑边 */
    const cv::Scalar outline(0, 0, 0);
    const int outline_radius = 1;
    for (int dy = -outline_radius; dy <= outline_radius; ++dy) {
        for (int dx = -outline_radius; dx <= outline_radius; ++dx) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            cv::putText(image, text,
                        cv::Point(origin_x + dx, origin_y + dy),
                        font_face, scale, outline, thickness,
                        cv::LINE_AA, false);
        }
    }
    /* 黄色正文 */
    const cv::Scalar color(blue, green, red);
    cv::putText(image, text,
                cv::Point(origin_x, origin_y),
                font_face, scale, color, thickness,
                cv::LINE_AA, false);
    (void)text_size;
}

/* 根据并排布局, 在可见光副画面左上角 + IR 主画面左上角绘制 FPS
 * 文字坐标采用 cv::putText 的 "左下角为锚点" 约定:
 *   锚点 = (region_x + margin, region_y + margin + text_height)
 */
static void draw_fps_overlay(
    BgrFrame& frame, const SideBySideLayout& layout,
    float visible_fps, float ir_fps)
{
    if (!layout.valid || !frame.valid()) {
        return;
    }
    const int font_face = cv::FONT_HERSHEY_SIMPLEX;
    const double scale = FPS_OVERLAY_FONT_SCALE;
    const int thickness = FPS_OVERLAY_FONT_THICKNESS;
    const int margin = FPS_OVERLAY_MARGIN;

    char visible_text[32];
    char ir_text[32];
    std::snprintf(visible_text, sizeof(visible_text),
                  "FPS: %.1f", static_cast<double>(visible_fps));
    std::snprintf(ir_text, sizeof(ir_text),
                  "FPS: %.1f", static_cast<double>(ir_fps));

    int baseline = 0;
    const cv::Size visible_size = cv::getTextSize(
        visible_text, font_face, scale, thickness, &baseline);
    const cv::Size ir_size = cv::getTextSize(
        ir_text, font_face, scale, thickness, &baseline);
    const int text_height = std::max(visible_size.height,
                                     ir_size.height);

    /* 可见光副画面左上角 */
    const int vis_x = layout.vis_x + margin;
    const int vis_y = layout.vis_y + margin + text_height;
    draw_fps_text(frame, vis_x, vis_y, visible_text,
                  FPS_OVERLAY_COLOR_VISIBLE_B,
                  FPS_OVERLAY_COLOR_VISIBLE_G,
                  FPS_OVERLAY_COLOR_VISIBLE_R);

    /* IR 主画面左上角 */
    const int ir_x = layout.ir_x + margin;
    const int ir_y = layout.ir_y + margin + text_height;
    draw_fps_text(frame, ir_x, ir_y, ir_text,
                  FPS_OVERLAY_COLOR_IR_B,
                  FPS_OVERLAY_COLOR_IR_G,
                  FPS_OVERLAY_COLOR_IR_R);
}
#endif /* TWOCAMERA_HAS_OPENCV && FPS_OVERLAY_ENABLE */

enum class LogLevel {
    Debug,
    Error,
    None
};

void handle_signal(int)
{
    stop_requested = 1;
}

void register_log_level(LogLevel level)
{
    iruvc_log_level_t uvc_level = IRUVC_LOG_NO_PRINT;
    irtemp_log_level_t temperature_level = IRTEMP_LOG_NO_PRINT;
    irproc_log_level_t process_level = IRPROC_LOG_NO_PRINT;
    irparse_log_level_t parse_level = IRPARSE_LOG_NO_PRINT;

    if (level == LogLevel::Debug) {
        uvc_level = IRUVC_LOG_DEBUG;
        temperature_level = IRTEMP_LOG_DEBUG;
        process_level = IRPROC_LOG_DEBUG;
        parse_level = IRPARSE_LOG_DEBUG;
    } else if (level == LogLevel::Error) {
        uvc_level = IRUVC_LOG_ERROR;
        temperature_level = IRTEMP_LOG_ERROR;
        process_level = IRPROC_LOG_ERROR;
        parse_level = IRPARSE_LOG_ERROR;
    }

    iruvc_log_register(uvc_level);
    irtemp_log_register(temperature_level);
    irproc_log_register(process_level);
    irparse_log_register(parse_level);
}

void print_and_record_versions()
{
    const char* versions[] = {
        IR_SAMPLE_VERSION,
        irproc_version(),
        irparse_version(),
        irtemp_version(),
        libiruvc_version()
    };
    for (size_t i = 0;
         i < sizeof(versions) / sizeof(versions[0]); ++i) {
        std::puts(versions[i] != NULL ? versions[i] : "unknown");
    }

    FILE* file = std::fopen("libs_version.txt", "wb");
    if (file == NULL) {
        std::fprintf(stderr,
                     "Cannot write libs_version.txt: %s\n",
                     std::strerror(errno));
        return;
    }
    for (size_t i = 0;
         i < sizeof(versions) / sizeof(versions[0]); ++i) {
        std::fprintf(file, "%s\n",
                     versions[i] != NULL
                         ? versions[i]
                         : "unknown");
    }
    std::fclose(file);
}

void print_usage(const char* program)
{
    std::printf(
        "Usage: %s [options]\n"
        "  --visible PATH   visible camera node; default: "
        "auto-detect 1d6c:0103\n"
        "  --fb PATH        framebuffer node; default: /dev/fb0\n"
        "  --width N        visible-camera width; default: 1920\n"
        "  --height N       visible-camera height; default: 1080\n"
        "  --fps N          capture and display rate; default: 30\n"
        "  --calibration PATH  registration quad file; default: "
        "calibration.txt\n"
#if defined(TWOCAMERA_HAS_YOLO)
        "  --model PATH     YOLO ONNX model path (default: "
        YOLO_MODEL_PATH ")\n"
#endif
        "  --help           show this help\n",
        program);
}

bool parse_positive_int(const char* value, int& destination)
{
    char* end = NULL;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed <= 0 ||
        parsed > 10000) {
        return false;
    }
    destination = static_cast<int>(parsed);
    return true;
}

/* 加载标定文件 (calib_web.py 生成):
 * 格式: 每行一条, '#' 开头为注释,
 *   quad x0 y0 x1 y1 x2 y2 x3 y3   (红外视野四边形, RGB 640x480 坐标,
 *                                   顺序: 左上/右上/右下/左下) */
bool load_calibration_quad(const char* path, float quad[8])
{
    FILE* file = std::fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    char line[512];
    bool found = false;
    while (std::fgets(line, sizeof(line), file) != NULL) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        float v[8];
        if (std::sscanf(line,
                        " quad %f %f %f %f %f %f %f %f",
                        &v[0], &v[1], &v[2], &v[3],
                        &v[4], &v[5], &v[6], &v[7]) == 8) {
            for (int i = 0; i < 8; ++i) {
                quad[i] = v[i];
            }
            found = true;
            break;
        }
    }
    std::fclose(file);
    return found;
}

void run_temperature_demos(
    IrCamera& infrared, uint64_t& last_sequence)
{
#if defined(POINT_TEMP_DEMO) || defined(LINE_TEMP_DEMO) || \
    defined(RECT_TEMP_DEMO)
    TemperatureFrame frame;
    if (!infrared.copy_latest_temperature(frame) ||
        frame.sequence == last_sequence ||
        frame.sequence % TEMPERATURE_PRINT_INTERVAL != 0) {
        return;
    }
    last_sequence = frame.sequence;
    TempDataRes_t resolution = {
        static_cast<uint16_t>(frame.width),
        static_cast<uint16_t>(frame.height)
    };
#if defined(POINT_TEMP_DEMO)
    point_temp_demo(frame.data.data(), resolution);
#endif
#if defined(LINE_TEMP_DEMO)
    line_temp_demo(frame.data.data(), resolution);
#endif
#if defined(RECT_TEMP_DEMO)
    rect_temp_demo(frame.data.data(), resolution);
#endif
#else
    (void)infrared;
    (void)last_sequence;
#endif
}

/* 主管线 (camerayolo.md 3.1.1 同步模式, 本机开发用):
 * Synchronizer → ImageRegistrator → [YOLO IDetector]
 * → SideBySideCompositor → [FPS/检测框+伪彩 Overlay] → IDisplay
 * ByteTrack 跟踪与运动意图为后续阶段, 不在本版实现。 */
bool run_once(const VisibleCameraConfig& visible_config,
              const DisplayConfig& display_config,
              const SideBySideConfig& side_by_side_config,
              const std::string& calibration_path,
              const std::string& yolo_model_path)
{
    IrCameraConfig infrared_config;
#if !defined(TWOCAMERA_HAS_YOLO)
    (void)yolo_model_path;
#endif

    IrCamera infrared;
    std::fprintf(stderr, "[STEP] opening IR camera...\n");
    if (!infrared.open(infrared_config)) {
        std::fprintf(stderr,
                     "Failed to open infrared camera\n");
        return false;
    }
    std::fprintf(stderr, "[STEP] IR camera opened\n");

    if (command_init() != IRUVC_SUCCESS) {
        std::fprintf(stderr,
                     "Infrared command channel is unavailable\n");
    }
    std::fprintf(stderr, "[STEP] command_init done\n");

#if defined(TWOCAMERA_HAS_YOLO)
#if DETECTOR_BACKEND_YOLOV8N
    /* ★ 2. YOLO 推理后端 (暂时方案): 可见光 YOLOv8n 人像检测
     * (CrowdHuman 训练, 半身~全身; 自带 LetterBox 预处理 + YOLOv8
     * 后处理, 只用 visible 帧; YOLOFuse 双模态代码保留可随时切回) */
    Yolov8OnnxDetector detector;
#else
    /* YOLOFuse 双模态融合 (IR 前 3 通道 + RGB 后 3 通道) */
    OnnxDetector detector;
#endif
    detector.set_conf_threshold(YOLO_CONF_THRESHOLD);
    detector.set_iou_threshold(YOLO_IOU_THRESHOLD);
    if (!detector.load(yolo_model_path)) {
        std::fprintf(stderr,
                     "Failed to load YOLO model: %s\n",
                     yolo_model_path.c_str());
        infrared.close();
        return false;
    }
    std::fprintf(stderr, "[STEP] YOLO model loaded: %s (input %d)\n",
                 yolo_model_path.c_str(), detector.model_size());

#if defined(TWOCAMERA_HAS_TRACKER)
    /* ★ 3. ByteTrack 多目标跟踪 (camerayolo.md 3.4):
     * 检测 → 跟踪, 输出含 track_id/速度/轨迹的 TrackedObject */
    TrackerConfig tracker_config;
    tracker_config.track_thresh = TRACKER_THRESH;
    tracker_config.track_buffer = TRACKER_BUFFER;
    tracker_config.match_thresh = TRACKER_MATCH_THRESH;
    tracker_config.low_thresh = TRACKER_LOW_THRESH;
    tracker_config.trail_length = TRACKER_TRAIL_LENGTH;
#if NEW_TARGET_ALERT_ENABLE
    /* 新目标提示音 (3.4.8): 稳定存在 NEW_TARGET_STABLE_MS 触发 */
    tracker_config.alert_stable_seconds =
        NEW_TARGET_STABLE_MS / 1000.0f;
#endif
    ByteTrack tracker(tracker_config);

#if NEW_TARGET_ALERT_ENABLE
    /* 提示音组件: 探测 ALSA/蜂鸣器后端, 非阻塞播放 + 限流 */
    AlertNotifier alert_notifier;
    alert_notifier.set_tone(NEW_TARGET_ALERT_FREQ_HZ,
                            NEW_TARGET_ALERT_DURATION_MS);
    alert_notifier.set_min_interval_ms(
        NEW_TARGET_ALERT_MIN_INTERVAL_MS);
    alert_notifier.start();
    tracker.set_new_target_callback(
        [](int track_id, void* user) {
            static_cast<AlertNotifier*>(user)
                ->play_new_target_tone();
        },
        &alert_notifier);
#endif

    TrackingOverlayConfig overlay_config;
    overlay_config.box_thickness = DETECTION_BOX_THICKNESS;
    overlay_config.draw_track_id = DETECTION_DRAW_TRACK_ID != 0;
    overlay_config.draw_motion_arrow = MOTION_ARROW_ENABLE != 0;
    overlay_config.draw_trail = MOTION_TRAIL_ENABLE != 0;
    overlay_config.arrow_scale = MOTION_ARROW_SCALE;
    overlay_config.max_arrow_length = MOTION_ARROW_MAX_LEN;
    overlay_config.speed_threshold = MOTION_SPEED_THRESH;
    overlay_config.ir_pseudo_color = IR_PSEUDO_COLOR_ENABLE != 0;
    overlay_config.visible_pseudo_color =
        VISIBLE_PSEUDO_COLOR_ENABLE != 0;
    overlay_config.visible_pseudo_alpha =
        VISIBLE_PSEUDO_COLOR_ALPHA;
    TrackingOverlay overlay(overlay_config);

    std::vector<TrackedObject> tracked_objects;
#else
    /* 无跟踪编译时: 检测框直接映射绘制 */
    const int overlay_box_thickness = DETECTION_BOX_THICKNESS;
#endif

    /* 检测间隔策略 (camerayolo.md 3.4.7): 每 N 帧推理一次,
     * 中间帧复用缓存检测框 */
    std::vector<Detection> cached_detections;
    uint64_t detection_frame_index = 0;
    int detect_fail_count = 0;
#endif

    /* 方案 1：先启动红外 ISO 流（避免被可见光 V4L2 ISO 抢占 USB 带宽）
     * 见 202608191952.md 的根因分析：USB_sample 不开可见光不阻塞，
     * twocamera 先开 visible 再开 infrared 会导致 infrared 的
     * uvc_camera_stream_start 永久阻塞。改成先启红外，让 V4L2 内核
     * 协商剩余带宽（V4L2 协商比 libusb 直接 transfer 更健壮）。 */
    if (!infrared.start()) {
        std::fprintf(stderr,
                     "Failed to start infrared camera\n");
        infrared.close();
        return false;
    }
    std::fprintf(stderr, "[STEP] IR camera started\n");

    VisibleCamera visible;
    std::fprintf(stderr, "[STEP] opening visible camera...\n");
    if (!visible.open(visible_config) || !visible.start()) {
        std::fprintf(stderr,
                     "Failed to start visible camera\n");
        visible.close();
        infrared.close();
        return false;
    }
    std::fprintf(stderr, "[STEP] visible camera started\n");

    std::unique_ptr<IDisplay> display = create_display();
    std::fprintf(stderr, "[STEP] display create_display() returned %p\n",
                (void*)display.get());
    if (!display || !display->open(display_config)) {
        std::fprintf(stderr, "Failed to open display\n");
        visible.close();
        infrared.close();
        return false;
    }
    std::fprintf(stderr, "[STEP] display opened, entering capture loop\n");

    CommandConsole command_console;
#if defined(COMMAND_ENABLE)
    command_console.start();
#endif

    LatestFrameSynchronizer synchronizer;
    synchronizer.set_sources(&infrared, &visible);

    /* ★ 1. 图像配准 (camerayolo.md 3.2):
     * 优先加载 calibration.txt 的四边形透视截取参数
     * (calib_web.py 选点/拖点标定后一键写入, 摄像头被碰后
     * 重新标定只需保存新文件, 无需改代码);
     * 文件缺失时回退到 sample_config.h 的 REGISTRATION_CROP_* 兜底窗口 */
    RegistrationConfig registration_config;
    float calib_quad[8];
    if (load_calibration_quad(calibration_path.c_str(),
                              calib_quad)) {
        for (int i = 0; i < 8; ++i) {
            registration_config.quad[i] = calib_quad[i];
        }
        registration_config.use_quad = true;
        std::fprintf(stderr,
                     "[STEP] registration: quad from %s "
                     "(q0=%.0f,%.0f q1=%.0f,%.0f q2=%.0f,%.0f "
                     "q3=%.0f,%.0f)\n",
                     calibration_path.c_str(),
                     calib_quad[0], calib_quad[1], calib_quad[2],
                     calib_quad[3], calib_quad[4], calib_quad[5],
                     calib_quad[6], calib_quad[7]);
    } else {
        const float scale_x =
            static_cast<float>(visible_config.width) / 640.0f;
        const float scale_y =
            static_cast<float>(visible_config.height) / 480.0f;
        registration_config.crop_width = std::max(32, static_cast<int>(
            REGISTRATION_CROP_W * scale_x + 0.5f));
        registration_config.crop_height = std::max(24, static_cast<int>(
            REGISTRATION_CROP_H * scale_y + 0.5f));
        registration_config.crop_width =
            std::min(registration_config.crop_width,
                     visible_config.width);
        registration_config.crop_height =
            std::min(registration_config.crop_height,
                     visible_config.height);
        registration_config.crop_x = std::max(0, std::min(
            static_cast<int>(REGISTRATION_CROP_X * scale_x + 0.5f),
            visible_config.width -
                registration_config.crop_width));
        registration_config.crop_y = std::max(0, std::min(
            static_cast<int>(REGISTRATION_CROP_Y * scale_y + 0.5f),
            visible_config.height -
                registration_config.crop_height));
        std::fprintf(stderr,
                     "[STEP] registration: fallback crop "
                     "(%d,%d %dx%d), no %s\n",
                     registration_config.crop_x,
                     registration_config.crop_y,
                     registration_config.crop_width,
                     registration_config.crop_height,
                     calibration_path.c_str());
    }
    ImageRegistrator registrator(registration_config);

    /* ★ 3. 并排合成 (camerayolo.md 3.3): IR 主画面左侧 + 可见光副画面右侧 */
    SideBySideCompositor compositor(side_by_side_config);

    RegisteredFrames registered_frames;
    SyncedFrames synced_frames;
    BgrFrame composed_frame;
    uint64_t last_temperature_sequence = 0;

#if defined(TWOCAMERA_HAS_OPENCV) && FPS_OVERLAY_ENABLE
    FpsTracker visible_fps_tracker;
    FpsTracker ir_fps_tracker;
    const bool fps_overlay_enabled = true;
#else
    const bool fps_overlay_enabled = false;
#endif

    const int display_fps =
        visible_config.fps > 0 ? visible_config.fps : 30;
    const std::chrono::microseconds interval(
        1000000 / display_fps);
    std::chrono::steady_clock::time_point next_frame =
        std::chrono::steady_clock::now();
    const std::chrono::steady_clock::time_point start_time =
        next_frame;
    uint64_t composed_count = 0;
    uint64_t display_frames = 0;

    std::printf("[MAIN] Starting capture loop for %d seconds, "
                "display: %dx%d @ %dfps\n",
                STREAM_TIME_SECONDS,
                display->width(), display->height(), display_fps);

    while (!stop_requested) {
        next_frame += interval;
        if (synchronizer.acquire(synced_frames)) {
            if (!synced_frames.visible.valid()) {
                std::fprintf(stderr,
                             "[MAIN] Visible frame invalid: "
                             "width=%d height=%d stride=%d\n",
                             synced_frames.visible.width,
                             synced_frames.visible.height,
                             synced_frames.visible.stride);
            }
            if (!synced_frames.infrared.valid()) {
                std::fprintf(stderr,
                             "[MAIN] IR frame invalid: "
                             "width=%d height=%d stride=%d\n",
                             synced_frames.infrared.width,
                             synced_frames.infrared.height,
                             synced_frames.infrared.stride);
            }

            /* ★ 1. 图像配准: visible 帧 warp/crop 到 IR 视角
             * 失败时 output.valid 置 false, 由 compose 走失败分支 */
            if (!registrator.register_frames(synced_frames,
                                              registered_frames)) {
                static int register_fail_count = 0;
                if (++register_fail_count % 30 == 0) {
                    std::fprintf(stderr,
                                 "[MAIN] Frame registration failed "
                                 "(fail count: %d)\n",
                                 register_fail_count);
                }
            }

#if defined(TWOCAMERA_HAS_YOLO)
            /* ★ 2. YOLO 推理: 配准后的 visible + infrared
             *     → 640×640 坐标系检测框 */
            if (detection_frame_index % DETECTION_INTERVAL == 0) {
                if (!detector.detect(registered_frames.visible,
                                     registered_frames.infrared,
                                     cached_detections)) {
                    if (++detect_fail_count % 30 == 1) {
                        std::fprintf(stderr,
                                     "[MAIN] YOLO detect failed "
                                     "(fail count: %d)\n",
                                     detect_fail_count);
                    }
                }
            }
            ++detection_frame_index;
#endif

#if defined(TWOCAMERA_HAS_TRACKER)
            /* ★ 2.5 ByteTrack 跟踪: 检测框 → 含 ID/速度/轨迹的
             *     跟踪对象 (Kalman predict + 两轮 Hungarian 匹配) */
            tracked_objects = tracker.update(cached_detections);
#endif

            /* ★ 3. 并排合成: IR 主画面(左) + 可见光副画面(右) */
            if (compositor.compose(
                registered_frames, display->width(),
                display->height(), composed_frame)) {
                composed_count++;
                if (composed_count % display_fps == 0) {
                    std::printf("[MAIN] Composed frame #%llu: "
                                "registered=%dx%d seq=%llu, "
                                "ir=%dx%d seq=%llu, "
                                "delta=%.1fms"
#if defined(TWOCAMERA_HAS_YOLO)
                                ", dets=%zu"
#endif
#if defined(TWOCAMERA_HAS_TRACKER)
                                ", tracked=%zu"
#endif
                                "\n",
                                static_cast<unsigned long long>(composed_count),
                                registered_frames.visible.width,
                                registered_frames.visible.height,
                                static_cast<unsigned long long>(synced_frames.visible.sequence),
                                registered_frames.infrared.width,
                                registered_frames.infrared.height,
                                static_cast<unsigned long long>(synced_frames.infrared.sequence),
                                synced_frames.timestamp_delta_us / 1000.0
#if defined(TWOCAMERA_HAS_YOLO)
                                ,
                                cached_detections.size()
#endif
#if defined(TWOCAMERA_HAS_TRACKER)
                                ,
                                tracked_objects.size()
#endif
                                );
                }

#if defined(TWOCAMERA_HAS_OPENCV) && FPS_OVERLAY_ENABLE
                if (fps_overlay_enabled) {
                    visible_fps_tracker.update(
                        synced_frames.visible.timestamp_us);
                    ir_fps_tracker.update(
                        synced_frames.infrared.timestamp_us);
                    draw_fps_overlay(composed_frame,
                                     compositor.last_layout(),
                                     visible_fps_tracker.fps,
                                     ir_fps_tracker.fps);
                }
#endif

#if defined(TWOCAMERA_HAS_TRACKER)
                /* ★ 4. 跟踪结果叠加: 跟踪框 + ID 标签 + 运动方向
                 *     箭头 + 轨迹路径 + IR 框内红外伪彩 +
                 *     可见光框内以 50% 不透明度叠加红外热力伪彩 */
                overlay.draw(composed_frame, tracked_objects,
                             compositor.last_layout(),
                             detector.model_size(),
                             registered_frames.infrared);
#elif defined(TWOCAMERA_HAS_YOLO)
                /* 无跟踪编译时: 仅画裸检测框 */
                draw_detections_in_region(
                    composed_frame, cached_detections,
                    detector.model_size(),
                    compositor.last_layout().ir_x,
                    compositor.last_layout().ir_y,
                    compositor.last_layout().ir_width,
                    compositor.last_layout().ir_height,
                    overlay_box_thickness);
                draw_detections_in_region(
                    composed_frame, cached_detections,
                    detector.model_size(),
                    compositor.last_layout().vis_x,
                    compositor.last_layout().vis_y,
                    compositor.last_layout().vis_width,
                    compositor.last_layout().vis_height,
                    overlay_box_thickness);
#endif

                if (!display->present(composed_frame)) {
                    std::fprintf(stderr,
                                 "[MAIN] Failed to present frame #%llu\n",
                                 static_cast<unsigned long long>(composed_count));
                    break;
                }
                ++display_frames;
            } else {
                std::fprintf(stderr,
                             "[MAIN] Frame composition failed: "
                             "visible_valid=%d ir_valid=%d\n",
                             registered_frames.visible.valid(),
                             registered_frames.infrared.valid());
            }
        } else {
            /* 无法获取同步帧 */
            static int sync_fail_count = 0;
            if (++sync_fail_count % 30 == 0) {
                std::fprintf(stderr,
                             "[MAIN] Failed to acquire synced frames "
                             "(fail count: %d)\n", sync_fail_count);
            }
        }

        run_temperature_demos(
            infrared, last_temperature_sequence);

        const std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now();
        if (STREAM_TIME_SECONDS > 0 &&
            now - start_time >=
                std::chrono::seconds(
                    STREAM_TIME_SECONDS)) {
            break;
        }
        if (next_frame > now) {
            std::this_thread::sleep_until(next_frame);
        } else {
            next_frame = now;
        }
    }

    std::printf("[MAIN] Capture loop ended. "
                "Composed: %llu, Displayed: %llu frames\n",
                static_cast<unsigned long long>(composed_count),
                static_cast<unsigned long long>(display_frames));

#if defined(COMMAND_ENABLE)
    command_console.stop();
#endif
    display->close();
    visible.close();
    infrared.close();
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    /* 强制 stdout/stderr 无缓冲，避免子shell启动时日志丢失 */
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    VisibleCameraConfig visible_config;
    DisplayConfig display_config;
    SideBySideConfig side_by_side_config;
    side_by_side_config.gap = LAYOUT_GAP_WIDTH;
    std::string calibration_path = "calibration.txt";  // calib_web.py 写入的四边形标定文件
    std::string yolo_model_path = YOLO_MODEL_PATH;

    /* 方案 A：使用 MJPG 压缩格式，避免 USB 2.0 ISO microframe 调度冲突。
     * MJPG 压缩后每 packet 小（典型 640×480@30 MJPG ≈ 1-2 Mbps，而
     * YUYV 同规格 = 18 Mbps），与红外 ISO 流共存时 V4L2 STREAMON 协商
     * 成功率高。可见光摄像头硬件支持的 MJPG 分辨率：1280×720、
     * 1024×576、640×480、640×360。选 640×480@30 兼顾清晰度和带宽。 */
    visible_config.format = V4l2CaptureFormat::Mjpeg;
    visible_config.width = 640;
    visible_config.height = 480;
    visible_config.fps = 30;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr,
                         "Missing value for %s\n", argv[i]);
            return 2;
        }

        const char* option = argv[i];
        const char* value = argv[++i];
        if (std::strcmp(option, "--visible") == 0) {
            visible_config.device_path =
                std::strcmp(value, "auto") == 0
                    ? ""
                    : value;
        } else if (std::strcmp(option, "--fb") == 0) {
            display_config.device = value;
        } else if (std::strcmp(option, "--width") == 0) {
            if (!parse_positive_int(
                    value, visible_config.width)) {
                std::fprintf(stderr,
                             "Invalid width: %s\n", value);
                return 2;
            }
        } else if (std::strcmp(option, "--height") == 0) {
            if (!parse_positive_int(
                    value, visible_config.height)) {
                std::fprintf(stderr,
                             "Invalid height: %s\n", value);
                return 2;
            }
        } else if (std::strcmp(option, "--fps") == 0) {
            if (!parse_positive_int(
                    value, visible_config.fps)) {
                std::fprintf(stderr,
                             "Invalid fps: %s\n", value);
                return 2;
            }
        } else if (std::strcmp(option, "--calibration") == 0) {
            calibration_path = value;
        } else if (std::strcmp(option, "--model") == 0) {
            yolo_model_path = value;
        } else {
            std::fprintf(stderr,
                         "Unknown option: %s\n", option);
            return 2;
        }
    }

    /* 并排显示窗口尺寸: 双画面统一高度 (=可见光高度)、各保持
     * 4:3 宽高比 1:1 等大, 总宽 = 2 × 高×4/3 + 分割线。
     * 640×480 采集 → 1284×480 显示 (无横向压缩) */
    display_config.preferred_height = visible_config.height;
    display_config.preferred_width =
        visible_config.height * 4 / 3 * 2 + LAYOUT_GAP_WIDTH;

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    if (setpriority(PRIO_PROCESS, 0, -20) != 0) {
        std::fprintf(stderr,
                     "Cannot raise process priority: %s\n",
                     std::strerror(errno));
    }

    print_and_record_versions();
    register_log_level(LogLevel::Error);

    return run_once(visible_config, display_config,
                    side_by_side_config, calibration_path,
                    yolo_model_path)
               ? 0
               : 1;
}
