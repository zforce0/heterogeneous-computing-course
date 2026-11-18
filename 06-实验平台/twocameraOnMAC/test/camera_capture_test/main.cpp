/*
 * camera_capture_test — 双摄标定辅助拍照工具
 *
 * 采集链路与主程序 sample.cpp 完全一致:
 *   IrCamera (USB SDK, 256×192) ─┐
 *                                ├→ LatestFrameSynchronizer → 同步帧对
 *   VisibleCamera (V4L2 MJPG 640×480) ─┘
 * 启动顺序也与主程序一致: 先启动红外 ISO 流, 再开可见光 V4L2,
 * 避免可见光抢占 USB 带宽导致红外 uvc_camera_stream_start 阻塞。
 *
 * 使用:
 *   ./build/camera_capture_test
 *   预览窗口: 左=可见光(原始), 右=红外(放大预览, 保存原始尺寸)
 *   Enter / 空格  : 拍照, 保存同一时刻的可见光+红外成对图像
 *   q / Esc / 关闭窗口 / Ctrl+C : 退出
 *
 * 保存位置 (与 YOLOFuse 数据集结构一致, 可直接用于标定/训练):
 *   captures/images/NNN.jpg    可见光原始帧 (640×480)
 *   captures/imagesIR/NNN.jpg  红外原始帧 (256×192)
 */
#include "ir_camera.h"
#include "synchronizer.h"
#include "visible_camera.h"

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

namespace {

volatile sig_atomic_t stop_requested = 0;

void handle_signal(int)
{
    stop_requested = 1;
}

bool ensure_directory(const std::string& path)
{
    if (mkdir(path.c_str(), 0755) == 0) {
        return true;
    }
    return errno == EEXIST;
}

/* 从 000 开始找到第一个未占用的编号 */
int next_capture_index(const std::string& dir)
{
    int index = 0;
    char path[256];
    while (index < 1000) {
        std::snprintf(path, sizeof(path), "%s/%03d.jpg",
                      dir.c_str(), index);
        FILE* file = std::fopen(path, "rb");
        if (file == NULL) {
            return index;
        }
        std::fclose(file);
        ++index;
    }
    return index;
}

cv::Mat wrap_bgr(BgrFrame& frame)
{
    if (!frame.valid()) {
        return cv::Mat();
    }
    return cv::Mat(frame.height, frame.width, CV_8UC3,
                   frame.data.data(),
                   static_cast<size_t>(frame.stride));
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    /* ---- 采集初始化 (与 sample.cpp 一致) ---- */
    IrCameraConfig infrared_config;
    /* 标定需要原始图像: 不开镜像/旋转/伪彩 (默认即如此) */

    IrCamera infrared;
    std::fprintf(stderr, "[STEP] opening IR camera...\n");
    if (!infrared.open(infrared_config)) {
        std::fprintf(stderr, "Failed to open infrared camera\n");
        return 1;
    }
    /* 先启动红外 ISO 流 (见 sample.cpp 方案 1 的 USB 带宽分析) */
    if (!infrared.start()) {
        std::fprintf(stderr, "Failed to start infrared camera\n");
        infrared.close();
        return 1;
    }
    std::fprintf(stderr, "[STEP] IR camera started\n");

    /* MJPG 640×480@30, 与主程序一致 */
    VisibleCameraConfig visible_config;
    visible_config.format = V4l2CaptureFormat::Mjpeg;
    visible_config.width = 640;
    visible_config.height = 480;
    visible_config.fps = 30;

    VisibleCamera visible;
    std::fprintf(stderr, "[STEP] opening visible camera...\n");
    if (!visible.open(visible_config) || !visible.start()) {
        std::fprintf(stderr, "Failed to start visible camera\n");
        visible.close();
        infrared.close();
        return 1;
    }
    std::fprintf(stderr, "[STEP] visible camera started\n");

    if (!ensure_directory("captures") ||
        !ensure_directory("captures/images") ||
        !ensure_directory("captures/imagesIR")) {
        std::fprintf(stderr, "Cannot create captures directory\n");
        visible.close();
        infrared.close();
        return 1;
    }

    LatestFrameSynchronizer synchronizer;
    synchronizer.set_sources(&infrared, &visible);
    SyncedFrames synced_frames;

    const std::string window = "calib-capture";
    cv::namedWindow(window, cv::WINDOW_NORMAL);

    int capture_index =
        next_capture_index("captures/images");
    int saved_count = 0;

    std::printf("[CAPTURE] ready. Focus the preview window, then:\n"
                "  Enter / Space : save synced RGB+IR pair\n"
                "  q / Esc / Ctrl+C : quit\n");

    while (!stop_requested) {
        if (!synchronizer.acquire(synced_frames)) {
            continue;
        }

        cv::Mat vis = wrap_bgr(synced_frames.visible);
        cv::Mat ir = wrap_bgr(synced_frames.infrared);
        if (vis.empty() || ir.empty()) {
            continue;
        }

        /* 预览: IR 放大到与可见光同高后横向拼接 (仅显示用,
         * 保存的是原始分辨率) */
        cv::Mat ir_up;
        cv::resize(ir, ir_up, cv::Size(vis.cols * vis.rows / ir.rows,
                                       vis.rows));
        cv::Mat preview;
        cv::hconcat(vis, ir_up, preview);

        char hint[128];
        std::snprintf(hint, sizeof(hint),
                      "saved:%d  [Enter]=capture  [q]=quit",
                      saved_count);
        cv::putText(preview, hint, cv::Point(10, 24),
                    cv::FONT_HERSHEY_SIMPLEX, 0.7,
                    cv::Scalar(0, 255, 255), 2, cv::LINE_AA);

        cv::imshow(window, preview);
        const int key = cv::waitKey(1) & 0xFF;

        if (cv::getWindowProperty(window,
                                  cv::WND_PROP_VISIBLE) < 1) {
            break;
        }
        if (key == 'q' || key == 'Q' || key == 27) {
            break;
        }

        /* Enter (\r=13 / \n=10) 或空格: 保存同步帧对 */
        if (key == 13 || key == 10 || key == ' ') {
            char rgb_path[256];
            char ir_path[256];
            std::snprintf(rgb_path, sizeof(rgb_path),
                          "captures/images/%03d.jpg",
                          capture_index);
            std::snprintf(ir_path, sizeof(ir_path),
                          "captures/imagesIR/%03d.jpg",
                          capture_index);
            const bool ok_rgb = cv::imwrite(rgb_path, vis);
            const bool ok_ir = cv::imwrite(ir_path, ir);
            if (ok_rgb && ok_ir) {
                ++saved_count;
                std::printf("[CAPTURE] #%03d saved: %s (%dx%d), "
                            "%s (%dx%d), time_delta=%.1fms\n",
                            capture_index, rgb_path,
                            synced_frames.visible.width,
                            synced_frames.visible.height,
                            ir_path,
                            synced_frames.infrared.width,
                            synced_frames.infrared.height,
                            synced_frames.timestamp_delta_us /
                                1000.0);
                ++capture_index;
            } else {
                std::fprintf(stderr,
                             "[CAPTURE] save failed (rgb=%d ir=%d)\n",
                             ok_rgb ? 1 : 0, ok_ir ? 1 : 0);
            }
        }
    }

    std::printf("[CAPTURE] done. %d pair(s) saved under "
                "captures/images and captures/imagesIR\n",
                saved_count);
    cv::destroyWindow(window);
    visible.close();
    infrared.close();
    return 0;
}
