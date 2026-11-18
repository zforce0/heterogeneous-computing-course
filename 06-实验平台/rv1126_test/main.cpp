// RV1126B 双相机(可见光+红外)YOLOv8 实时推理 demo
//
// 用法: rv1126_yolov8_demo <model.rknn> [选项]
//   -d, --device /dev/videoN   指定可见光节点(默认按 VID/PID 1d6c:0103 自动探测)
//   -W, --width  N             可见光宽度   (默认 1280)
//   -H, --height N             可见光高度   (默认 720)
//   -f, --fps    N             可见光帧率   (默认 25)
//       --format mjpg|yuyv     像素格式     (默认 mjpg)
//       --save-dir DIR         结果图目录   (默认 ./output,只保留最近 10 对)
//   -h, --help                 帮助
//
// 红外相机(0bda:5840)经闭源 SDK 固定 256x192@25fps,优先启动;
// 仅对可见光做 YOLO 推理;检出时同时保存同一时刻的 _vis/_ir 两张图片。
// Ctrl+C 优雅退出,退出时打印两路实际帧率与推理/存图统计。

#include <opencv2/opencv.hpp>

#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <map>
#include <string>
#include <vector>

#include "ir_camera.h"
#include "perf_clock.h"
#include "visible_camera.h"
#include "yolov8.h"

using namespace cv;

static const size_t kKeepPairs = 10;

// ---------------- Ctrl+C 优雅退出 ----------------
static std::atomic<bool> g_running(true);

static void handle_signal(int sig)
{
    // 第一次触发:置停止标志,主循环自然退出
    // 第二次触发:恢复默认行为,允许强制终止
    if (!g_running.exchange(false)) {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

static void install_signal_handlers()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

// ---------------- 成对图片管理 ----------------

// 列出目录下全部 det_*.jpg,按前缀(去掉 _vis/_ir)分组
// 同时完成:删除无配对散文件 + 只保留最近 keep 对
static void prune_old_pairs(const std::string& dir, size_t keep)
{
    DIR* d = opendir(dir.c_str());
    if (d == NULL) {
        return;
    }
    // prefix -> 拥有的后缀("_vis"/"_ir"/其他)
    std::map<std::string, std::vector<std::string> > groups;
    dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        std::string name = entry->d_name;
        if (name.compare(0, 4, "det_") != 0 ||
            name.compare(name.size() - 4, 4, ".jpg") != 0) {
            continue;
        }
        std::string body = name.substr(0, name.size() - 4);
        std::string prefix = body;
        std::string suffix;
        if (body.size() > 4 && body.compare(body.size() - 4, 4, "_vis") == 0) {
            prefix = body.substr(0, body.size() - 4);
            suffix = "_vis";
        } else if (body.size() > 3 &&
                   body.compare(body.size() - 3, 3, "_ir") == 0) {
            prefix = body.substr(0, body.size() - 3);
            suffix = "_ir";
        }
        groups[prefix].push_back(suffix);  // 空 suffix = 第一期旧文件/散文件
    }
    closedir(d);

    // std::map 按 key 有序:前缀内嵌时间戳,字典序即时间序
    std::vector<std::string> prefixes;
    for (std::map<std::string, std::vector<std::string> >::iterator it =
             groups.begin();
         it != groups.end(); ++it) {
        prefixes.push_back(it->first);
    }

    // 1) 删除无配对的散文件(旧版单图、半对)
    //    完整对:同时含 "_vis" 与 "_ir"
    // 2) 超出 keep 的最旧对也删除
    size_t complete_tail = 0;  // 从后往前数完整对
    for (size_t i = prefixes.size(); i-- > 0; ) {
        const std::vector<std::string>& sfx = groups[prefixes[i]];
        const bool has_vis =
            std::find(sfx.begin(), sfx.end(), "_vis") != sfx.end();
        const bool has_ir =
            std::find(sfx.begin(), sfx.end(), "_ir") != sfx.end();
        if (has_vis && has_ir) {
            ++complete_tail;
        }
        bool keep_this = (has_vis && has_ir) && complete_tail <= keep;
        // 删除该组全部文件
        for (size_t k = 0; k < sfx.size(); ++k) {
            std::string file = dir + "/" + prefixes[i] + sfx[k] + ".jpg";
            if (!keep_this) {
                if (unlink(file.c_str()) == 0) {
                    printf("  [CLEAN] removed %s\n",
                           (prefixes[i] + sfx[k]).c_str());
                }
            }
        }
    }
}

// 生成成对文件名前缀:det_YYYYMMDD_HHMMSS_mmm
static std::string make_prefix()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tmv;
    localtime_r(&tv.tv_sec, &tmv);
    char name[64];
    snprintf(name, sizeof(name), "det_%04d%02d%02d_%02d%02d%02d_%03d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
             (int)(tv.tv_usec / 1000));
    return name;
}

// ---------------- 画框(来自 yolov8_detect_demo) ----------------
static Scalar colorArray[10] = {
    Scalar(255, 0, 0, 255),   Scalar(0, 255, 0, 255),  Scalar(0, 0, 139, 255),
    Scalar(0, 100, 0, 255),   Scalar(139, 139, 0, 255), Scalar(209, 206, 0, 255),
    Scalar(0, 127, 255, 255), Scalar(139, 61, 72, 255), Scalar(0, 255, 0, 255),
    Scalar(255, 0, 0, 255),
};

static int plot_one_box(Mat src, int x1, int x2, int y1, int y2,
                        const char* label, int colour)
{
    int tl = round(0.002 * (src.rows + src.cols) / 2) + 1;
    rectangle(src, cv::Point(x1, y1), cv::Point(x2, y2),
              colorArray[colour % 10], 3);

    int tf = std::max(tl - 1, 1);
    int base_line = 0;
    cv::Size t_size = getTextSize(label, FONT_HERSHEY_SIMPLEX,
                                  (float)tl / 3, tf, &base_line);
    int x3 = x1 + t_size.width;
    int y3 = y1 - t_size.height - 3;

    rectangle(src, cv::Point(x1, y1), cv::Point(x3, y3),
              colorArray[colour % 10], -1);
    putText(src, label, cv::Point(x1, y1 - 2), FONT_HERSHEY_SIMPLEX,
            (float)tl / 3, cv::Scalar(255, 255, 255, 255), tf, 8);
    return 0;
}

// ---------------- 参数解析 ----------------
static void print_usage(const char* prog)
{
    printf("Usage: %s <model.rknn> [options]\n"
           "  -d, --device /dev/videoN  visible camera (default: auto by 1d6c:0103)\n"
           "  -W, --width  N            visible width  (default 1280)\n"
           "  -H, --height N            visible height (default 720)\n"
           "  -f, --fps    N            visible fps    (default 25)\n"
           "      --format mjpg|yuyv    pixel format   (default mjpg)\n"
           "      --save-dir DIR        output dir     (default ./output, keep last %zu pairs)\n"
           "  IR camera 0bda:5840 fixed 256x192@25 via closed SDK\n"
           "  -h, --help                show this help\n",
           prog, kKeepPairs);
}

int main(int argc, char** argv)
{
    install_signal_handlers();

    std::string model_path;
    VisibleCameraConfig vis_cfg;
    std::string save_dir = "output";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next_value = [&](const char* opt) -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for %s\n", opt);
                exit(1);
            }
            return argv[++i];
        };
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-d" || arg == "--device") {
            vis_cfg.device_path = next_value("--device");
        } else if (arg == "-W" || arg == "--width") {
            vis_cfg.width = atoi(next_value("--width").c_str());
        } else if (arg == "-H" || arg == "--height") {
            vis_cfg.height = atoi(next_value("--height").c_str());
        } else if (arg == "-f" || arg == "--fps") {
            vis_cfg.fps = atoi(next_value("--fps").c_str());
        } else if (arg == "--format") {
            std::string fmt = next_value("--format");
            if (fmt == "mjpg" || fmt == "mjpeg") {
                vis_cfg.format = V4l2CaptureFormat::Mjpg;
            } else if (fmt == "yuyv") {
                vis_cfg.format = V4l2CaptureFormat::Yuyv;
            } else {
                fprintf(stderr, "unknown format: %s (mjpg|yuyv)\n",
                        fmt.c_str());
                return 1;
            }
        } else if (arg == "--save-dir") {
            save_dir = next_value("--save-dir");
        } else if (!arg.empty() && arg[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", arg.c_str());
            print_usage(argv[0]);
            return 1;
        } else if (model_path.empty()) {
            model_path = arg;
        } else {
            fprintf(stderr, "unexpected argument: %s\n", arg.c_str());
            print_usage(argv[0]);
            return 1;
        }
    }

    if (model_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    mkdir(save_dir.c_str(), 0755);
    prune_old_pairs(save_dir, kKeepPairs);

    // 1. 加载模型
    rknn_app_context_t rknn_app_ctx;
    memset(&rknn_app_ctx, 0, sizeof(rknn_app_ctx));
    if (yolov8_detect_init(model_path.c_str(), &rknn_app_ctx) != 0) {
        fprintf(stderr, "yolov8_detect_init failed (model: %s)\n",
                model_path.c_str());
        return 2;
    }
    printf("model loaded: %s\n", model_path.c_str());

    // 2. 先启动红外(优先保障 256x192@25)
    IrCamera ir_camera;
    IrCameraConfig ir_cfg;
    if (!ir_camera.open(ir_cfg) || !ir_camera.start()) {
        fprintf(stderr, "IR camera open/start failed\n");
        yolov8_detect_release(&rknn_app_ctx);
        return 3;
    }
    printf("waiting IR stream to settle...\n");
    for (int w = 0; w < 30 && g_running.load(); ++w) {
        usleep(100000);  // 共等待最多 3s
    }
    FrameRateInfo ir_check = ir_camera.fps_info();
    printf("IR running: %dx%d, cur_fps=%.1f avg_fps=%.1f\n",
           ir_camera.width(), ir_camera.height(),
           ir_check.cur_fps, ir_check.avg_fps);
    if (ir_check.cur_fps < 20.0) {
        fprintf(stderr,
                "warning: IR fps %.1f below target 25, continue anyway\n",
                ir_check.cur_fps);
    }

    // 3. 再启动可见光
    VisibleCamera vis_camera;
    if (!vis_camera.open(vis_cfg) || !vis_camera.start()) {
        fprintf(stderr, "visible camera open/start failed\n");
        ir_camera.close();
        yolov8_detect_release(&rknn_app_ctx);
        return 3;
    }
    printf("visible running: %s %dx%d\n", vis_camera.device_path().c_str(),
           vis_camera.width(), vis_camera.height());
    printf("press Ctrl+C to exit\n");

    // 4. 主循环:只取可见光最新帧推理
    BgrFrame frame;
    uint64_t last_seq = 0;
    detect_result_group_t results;

    uint64_t infer_count = 0;
    uint64_t obj_frames = 0;
    double sum_infer = 0.0;
    uint64_t saved_pairs = 0;
    double sum_save = 0.0, max_save = 0.0;
    double sum_align = 0.0, max_align = 0.0;

    double last_fps_print = 0.0;

    while (g_running.load()) {
        // 等待新的可见光帧
        bool got_frame = false;
        while (g_running.load()) {
            if (vis_camera.copy_latest(frame) &&
                frame.sequence != last_seq) {
                got_frame = true;
                break;
            }
            usleep(2000);
        }
        if (!got_frame) {
            break;
        }
        last_seq = frame.sequence;

        cv::Mat image(frame.height, frame.width, CV_8UC3,
                      frame.data.data(), frame.stride);

        // YOLO 推理(仅计时整体推理时长)
        double infer_ms = 0.0;
        yolov8_detect_run(&rknn_app_ctx, image, &results, &infer_ms);
        ++infer_count;
        sum_infer += infer_ms;
        if (results.count > 0) {
            ++obj_frames;
        }

        printf("#%llu infer=%.1fms obj=%d\n",
               (unsigned long long)frame.sequence, infer_ms,
               results.count);

        // 检出明细
        for (int i = 0; i < results.count; i++) {
            detect_result_t* det = &(results.results[i]);
            printf("  %s %.1f%% @ (%d %d %d %d)\n", det->name,
                   det->prop * 100, det->box.left, det->box.top,
                   det->box.right, det->box.bottom);
        }

        // 检出目标:配对红外 → 成对保存 → 按对清理
        if (results.count > 0) {
            const double t_save0 = now_ms();

            // 在红外近期帧中取与当前可见光帧时刻最接近的一帧
            BgrFrame ir_frame;
            double align_ms = 0.0;
            const bool ir_ok = ir_camera.nearest_frame(
                frame.timestamp_us, ir_frame, align_ms);

            // 可见光画框
            char text[256];
            for (int i = 0; i < results.count; i++) {
                detect_result_t* det = &(results.results[i]);
                snprintf(text, sizeof(text), "%s %.1f%%", det->name,
                         det->prop * 100);
                plot_one_box(image, det->box.left, det->box.right,
                             det->box.top, det->box.bottom, text, i);
            }

            bool saved = false;
            if (ir_ok) {
                std::string prefix = make_prefix();
                std::string vis_path =
                    save_dir + "/" + prefix + "_vis.jpg";
                std::string ir_path =
                    save_dir + "/" + prefix + "_ir.jpg";
                const bool ok_vis = cv::imwrite(vis_path, image);
                cv::Mat ir_mat(ir_frame.height, ir_frame.width, CV_8UC3,
                               ir_frame.data.data(), ir_frame.stride);
                const bool ok_ir = cv::imwrite(ir_path, ir_mat);
                saved = ok_vis && ok_ir;
                if (saved) {
                    ++saved_pairs;
                    prune_old_pairs(save_dir, kKeepPairs);
                }
            } else {
                fprintf(stderr, "no matching IR frame for this detection\n");
            }

            const double save_ms = now_ms() - t_save0;
            sum_save += save_ms;
            if (save_ms > max_save) {
                max_save = save_ms;
            }
            if (ir_ok) {
                sum_align += align_ms;
                if (align_ms > max_align) {
                    max_align = align_ms;
                }
                if (align_ms > 40.0) {
                    printf("  [WARN] large align delta %.1fms\n",
                           align_ms);
                }
            }
            if (saved) {
                printf("[SAVE] pair save=%.1fms align=%.1fms\n",
                       save_ms, align_ms);
            }
        }

        // 每 2s 打印两路实际采样帧率
        const double t_now = now_ms();
        if (last_fps_print == 0.0 || t_now - last_fps_print >= 2000.0) {
            FrameRateInfo ir_fps = ir_camera.fps_info();
            FrameRateInfo vis_fps = vis_camera.fps_info();
            printf("[FPS] ir : cur=%.1f avg=%.1f (frames=%llu) | "
                   "vis: cur=%.1f avg=%.1f (frames=%llu)\n",
                   ir_fps.cur_fps, ir_fps.avg_fps,
                   (unsigned long long)ir_fps.total, vis_fps.cur_fps,
                   vis_fps.avg_fps, (unsigned long long)vis_fps.total);
            last_fps_print = t_now;
        }
    }

    // 5. 清理:关闭前先取最终统计 → 停相机 → 释放模型
    FrameRateInfo ir_final_fps = ir_camera.fps_info();
    FrameRateInfo vis_final_fps = vis_camera.fps_info();
    vis_camera.stop();
    ir_camera.close();
    yolov8_detect_release(&rknn_app_ctx);

    printf("\nexit by Ctrl+C\n");
    printf("[STAT] ir  frames=%llu avg_fps=%.1f\n",
           (unsigned long long)ir_final_fps.total, ir_final_fps.avg_fps);
    printf("[STAT] vis frames=%llu avg_fps=%.1f\n",
           (unsigned long long)vis_final_fps.total,
           vis_final_fps.avg_fps);
    printf("[STAT] infer count=%llu avg=%.1fms obj_frames=%llu "
           "saved_pairs=%llu\n",
           (unsigned long long)infer_count,
           infer_count ? sum_infer / infer_count : 0.0,
           (unsigned long long)obj_frames,
           (unsigned long long)saved_pairs);
    if (saved_pairs) {
        printf("[STAT] save avg=%.1fms max=%.1fms align avg=%.1fms "
               "max=%.1fms\n",
               sum_save / saved_pairs, max_save,
               sum_align / saved_pairs, max_align);
    }
    return 0;
}
