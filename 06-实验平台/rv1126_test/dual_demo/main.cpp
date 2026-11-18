// RV1126B 双模态差分显著 + YOLO 级联 Demo
// (基于第二期双相机拉流代码增量开发, 方案见
//  《RV1126B双光差分显著+YOLO级联Demo实现方案.md》)
//
// 架构: IR 采集线程 + VIS 采集线程(沿用) + 显著线程(新增, CPU)
//       + 主线程(NPU: 消费提名 → crop → YOLO 确认)
// 核心: 红外只做"看这里"(差分显著提名), 可见光只做"认"(crop YOLO)
//
// 配准: 参考 twocameraOnMAC 的 4 点单应(quad)方案,
//       calibration.txt(红外视野在可见光 640x480 中的四边形);
//       无标定文件时退化为 FOV 分轴缩放, margin 吸收系统偏差。
//
// 温度: 组合流下半段 Y16(K*64) → CV_32F ℃(情形 A, 辐射测温可用)
//
// 用法: rv1126_dual_demo <model.rknn> [选项]   (./rv1126_dual_demo --help 详见)

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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "eventlog.h"
#include "ir_camera.h"
#include "ir_temp_conf.h"
#include "nominator.h"
#include "perf_clock.h"
#include "registration.h"
#include "saliency.h"
#include "visible_camera.h"
#include "viz_grid.h"
#include "yolov8.h"

using namespace cv;

static const size_t kKeepPairs = 10;   // det_*_vis/_ir 成对保留
static const size_t kKeepViz = 10;     // viz_*.jpg 保留

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

// ---------------- 成对图片管理(沿用第二期) ----------------
static void prune_old_pairs(const std::string& dir, size_t keep)
{
    DIR* d = opendir(dir.c_str());
    if (d == NULL) {
        return;
    }
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
        groups[prefix].push_back(suffix);
    }
    closedir(d);

    std::vector<std::string> prefixes;
    for (std::map<std::string, std::vector<std::string> >::iterator it =
             groups.begin();
         it != groups.end(); ++it) {
        prefixes.push_back(it->first);
    }
    size_t complete_tail = 0;
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

// viz_*.jpg 滚动清理(单图, 按前缀时间序)
static void prune_old_viz(const std::string& dir, size_t keep)
{
    DIR* d = opendir(dir.c_str());
    if (d == NULL) {
        return;
    }
    std::vector<std::string> names;
    dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        std::string name = entry->d_name;
        if (name.compare(0, 4, "viz_") == 0 &&
            name.compare(name.size() - 4, 4, ".jpg") == 0) {
            names.push_back(name);
        }
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    while (names.size() > keep) {
        const std::string file = dir + "/" + names.front();
        if (unlink(file.c_str()) == 0) {
            printf("  [CLEAN] removed %s\n", names.front().c_str());
        }
        names.erase(names.begin());
    }
}

static std::string make_prefix(const char* head)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tmv;
    localtime_r(&tv.tv_sec, &tmv);
    char name[64];
    snprintf(name, sizeof(name), "%s_%04d%02d%02d_%02d%02d%02d_%03d", head,
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

// ---------------- 温度矩阵转换 ----------------
static cv::Mat thermal_to_celsius(const ThermalFrame& t)
{
    cv::Mat m(t.height, t.width, CV_32F);
    const size_t n = static_cast<size_t>(t.width) * t.height;
    const uint16_t* src = t.data.data();
    float* dst = m.ptr<float>(0);
    for (size_t i = 0; i < n; ++i) {
        dst[i] = ir_raw_to_celsius(src[i]);
    }
    return m;
}

// ---------------- 显著线程与主线程的共享状态 ----------------

// 健康状态(显著线程写, 主线程读; 降级判据)
struct HealthShared {
    mutable std::mutex m;
    // 初始给健康值, 避免显著层首帧产出前被误判为热交叉(假 DEGRADE)
    double scene_contrast_k = 999.0;
    bool nuc_frozen = false;
    bool valid = false;
};

// viz 素材槽(显著线程每帧更新小图, 主线程按需取用合成六宫格)
struct VizSlot {
    mutable std::mutex m;
    cv::Mat sn_tir, sn_rgb, mask_q1, mask_q2, mask_q3, T_c;
    std::vector<Nomination> last_noms;
    bool valid = false;
};

// 显著层统计(显著线程写, 退出时主线程读)
struct SalStats {
    mutable std::mutex m;
    uint64_t frames = 0;
    uint64_t skip_align = 0;
    double sum_ms = 0.0;
    double max_ms = 0.0;
};

// ---------------- 参数 ----------------
struct AppConfig {
    // 模式
    std::string mode = "auto";        // cascade | baseline | auto
    std::string card = "inspect";     // inspect | yard | all
    // 显著性
    std::string thr_mode = "tht";
    double thr_k = 1.0;
    double delta_min = 3.0;
    int bg_kernel = 15;
    int min_area = 8;
    // 配准
    std::string calib_path = "calibration.txt";
    double ir_fov = 57.0;
    double vis_fov = 80.0;
    double margin_ratio = 3.0;
    int min_crop_ir = 20;
    int min_crop_px = 96;
    int max_crop_px = 1024;
    // 采集
    std::string res_preset = "auto";  // auto|960p|768p|720p|600p|480p
    bool user_res = false;            // 用户显式给 -W/-H
    int width = 1280;
    int height = 960;
    int fps = 25;
    std::string device;
    V4l2CaptureFormat format = V4l2CaptureFormat::Mjpg;
    int startup_timeout_ms = 3000;
    // 健康/降级
    double c_min = 1.5;
    double nuc_freeze_thr = 0.0005;  // 真冻结(数据停更)帧差≈0;
                                     // 静态场景 NETD 级噪声也 <0.01K,
                                     // 阈值必须远小于 NETD 才不误报
    // 时序(yard)
    float body_lo = 26.0f, body_hi = 40.0f, dt_thr = 0.5f;
    int link_radius = 8;
    // 输出
    std::string save_dir = "output";
    double viz_interval = 2.0;
    std::string events_path = "events.jsonl";
    bool no_viz = false;
    // 探针
    int probe_temp_frames = 0;        // >0 = 阶段0温度探针模式
};

static void print_usage(const char* prog)
{
    printf(
        "Usage: %s <model.rknn> [options]\n"
        "\n"
        "运行模式:\n"
        "  --mode cascade|baseline|auto   级联/逐帧基准/自动降级 (默认 auto)\n"
        "  --card  inspect|yard|all       巡检(Q1)/庭院(Q2+活体)/全部 (默认 inspect)\n"
        "\n"
        "显著性参数:\n"
        "  --thr-mode tht|adaptive|otsu   阈值方式 (默认 tht)\n"
        "  --thr-k FLOAT                  adaptive 的 k (默认 1.0)\n"
        "  --delta-min FLOAT              tht 最小温升 K (默认 3.0)\n"
        "  --bg-kernel INT                背景高斯核 (默认 15, 奇数)\n"
        "  --min-area INT                 提名最小像素数 (默认 8)\n"
        "\n"
        "几何/配准(参考 twocameraOnMAC quad 单应):\n"
        "  --calib FILE                   calibration.txt (默认 ./calibration.txt,\n"
        "                                 缺失时退化为 FOV 缩放)\n"
        "  --ir-fov FLOAT                 红外对角 FOV 度 (默认 57)\n"
        "  --vis-fov FLOAT                可见光对角 FOV 度 (默认 80)\n"
        "  --margin-ratio FLOAT           crop 上下文扩展倍数 (默认 3.0)\n"
        "  --min-crop-ir INT              最小 crop 边长 IR px (默认 20)\n"
        "  --min-crop-px INT              crop 边长绝对下限 VIS px (默认 96)\n"
        "  --max-crop-px INT              crop 边长上限 VIS px (默认 1024)\n"
        "\n"
        "启动与采集:\n"
        "  --res-preset auto|960p|768p|720p|600p|480p  VIS 档位 (默认 auto:\n"
        "        依次尝试 1280x960 -> 1024x768 -> 1280x720 -> 800x600 -> 640x480)\n"
        "  -W N -H N -f N                 显式分辨率/帧率(优先于 auto 档位链)\n"
        "  -d, --device /dev/videoN       指定可见光节点\n"
        "      --format mjpg|yuyv         像素格式 (默认 mjpg)\n"
        "  --startup-timeout-ms INT       首窗就绪超时 (默认 3000)\n"
        "\n"
        "健康/降级(auto 模式):\n"
        "  --c-min FLOAT                  最低场景热对比度 K (默认 1.5)\n"
        "  --nuc-freeze-thr FLOAT         冻结判定帧差阈值 (默认 0.002)\n"
        "\n"
        "时序(庭院模式):\n"
        "  --body-lo/--body-hi FLOAT      表观体温带 C (默认 26/40)\n"
        "  --dt-thr FLOAT                 |dT/dt| 上限 K/min (默认 0.5)\n"
        "  --link-radius INT              帧间关联半径 px (默认 8)\n"
        "\n"
        "输出:\n"
        "  --save-dir DIR                 结果目录 (默认 ./output)\n"
        "  --viz-interval SEC             六宫格周期 (默认 2.0)\n"
        "  --events FILE                  JSONL 事件流 (默认 ./events.jsonl)\n"
        "  --no-viz                       关闭六宫格可视化\n"
        "  --probe-temp N                 阶段0温度探针: 采 N 帧打印统计后退出\n"
        "\n"
        "IR 0bda:5840 256x192@25(闭源SDK, Y16 温度 K*64); Ctrl+C 退出打印统计\n",
        prog);
}

int main(int argc, char** argv)
{
    install_signal_handlers();

    AppConfig cfg;
    std::string model_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        // C++11 兼容的取值 lambda
        struct NextVal {
            static std::string get(char** argv, int argc, int& i,
                                   const char* opt)
            {
                if (i + 1 >= argc) {
                    fprintf(stderr, "missing value for %s\n", opt);
                    exit(1);
                }
                return argv[++i];
            }
        };
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--mode") {
            cfg.mode = NextVal::get(argv, argc, i, "--mode");
        } else if (arg == "--card") {
            cfg.card = NextVal::get(argv, argc, i, "--card");
        } else if (arg == "--thr-mode") {
            cfg.thr_mode = NextVal::get(argv, argc, i, "--thr-mode");
        } else if (arg == "--thr-k") {
            cfg.thr_k = atof(NextVal::get(argv, argc, i, "--thr-k").c_str());
        } else if (arg == "--delta-min") {
            cfg.delta_min =
                atof(NextVal::get(argv, argc, i, "--delta-min").c_str());
        } else if (arg == "--bg-kernel") {
            cfg.bg_kernel =
                atoi(NextVal::get(argv, argc, i, "--bg-kernel").c_str());
        } else if (arg == "--min-area") {
            cfg.min_area =
                atoi(NextVal::get(argv, argc, i, "--min-area").c_str());
        } else if (arg == "--calib" || arg == "--homography") {
            cfg.calib_path =
                NextVal::get(argv, argc, i, "--calib");
        } else if (arg == "--ir-fov") {
            cfg.ir_fov = atof(NextVal::get(argv, argc, i, "--ir-fov").c_str());
        } else if (arg == "--vis-fov") {
            cfg.vis_fov =
                atof(NextVal::get(argv, argc, i, "--vis-fov").c_str());
        } else if (arg == "--margin-ratio") {
            cfg.margin_ratio =
                atof(NextVal::get(argv, argc, i, "--margin-ratio").c_str());
        } else if (arg == "--min-crop-ir") {
            cfg.min_crop_ir =
                atoi(NextVal::get(argv, argc, i, "--min-crop-ir").c_str());
        } else if (arg == "--min-crop-px") {
            cfg.min_crop_px =
                atoi(NextVal::get(argv, argc, i, "--min-crop-px").c_str());
        } else if (arg == "--max-crop-px") {
            cfg.max_crop_px =
                atoi(NextVal::get(argv, argc, i, "--max-crop-px").c_str());
        } else if (arg == "--res-preset") {
            cfg.res_preset =
                NextVal::get(argv, argc, i, "--res-preset");
        } else if (arg == "-W" || arg == "--width") {
            cfg.width = atoi(NextVal::get(argv, argc, i, "--width").c_str());
            cfg.user_res = true;
        } else if (arg == "-H" || arg == "--height") {
            cfg.height = atoi(NextVal::get(argv, argc, i, "--height").c_str());
            cfg.user_res = true;
        } else if (arg == "-f" || arg == "--fps") {
            cfg.fps = atoi(NextVal::get(argv, argc, i, "--fps").c_str());
        } else if (arg == "-d" || arg == "--device") {
            cfg.device = NextVal::get(argv, argc, i, "--device");
        } else if (arg == "--format") {
            std::string fmt = NextVal::get(argv, argc, i, "--format");
            if (fmt == "mjpg" || fmt == "mjpeg") {
                cfg.format = V4l2CaptureFormat::Mjpg;
            } else if (fmt == "yuyv") {
                cfg.format = V4l2CaptureFormat::Yuyv;
            } else {
                fprintf(stderr, "unknown format: %s\n", fmt.c_str());
                return 1;
            }
        } else if (arg == "--startup-timeout-ms") {
            cfg.startup_timeout_ms = atoi(
                NextVal::get(argv, argc, i, "--startup-timeout-ms").c_str());
        } else if (arg == "--c-min") {
            cfg.c_min = atof(NextVal::get(argv, argc, i, "--c-min").c_str());
        } else if (arg == "--nuc-freeze-thr") {
            cfg.nuc_freeze_thr = atof(
                NextVal::get(argv, argc, i, "--nuc-freeze-thr").c_str());
        } else if (arg == "--body-lo") {
            cfg.body_lo =
                (float)atof(NextVal::get(argv, argc, i, "--body-lo").c_str());
        } else if (arg == "--body-hi") {
            cfg.body_hi =
                (float)atof(NextVal::get(argv, argc, i, "--body-hi").c_str());
        } else if (arg == "--dt-thr") {
            cfg.dt_thr =
                (float)atof(NextVal::get(argv, argc, i, "--dt-thr").c_str());
        } else if (arg == "--link-radius") {
            cfg.link_radius =
                atoi(NextVal::get(argv, argc, i, "--link-radius").c_str());
        } else if (arg == "--save-dir") {
            cfg.save_dir = NextVal::get(argv, argc, i, "--save-dir");
        } else if (arg == "--viz-interval") {
            cfg.viz_interval = atof(
                NextVal::get(argv, argc, i, "--viz-interval").c_str());
        } else if (arg == "--events") {
            cfg.events_path = NextVal::get(argv, argc, i, "--events");
        } else if (arg == "--no-viz") {
            cfg.no_viz = true;
        } else if (arg == "--probe-temp") {
            cfg.probe_temp_frames =
                atoi(NextVal::get(argv, argc, i, "--probe-temp").c_str());
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

    // ---- 阶段0: 温度探针模式(不加载模型, 相机启动后打印统计退出) ----
    const bool probe_mode = cfg.probe_temp_frames > 0;
    if (!probe_mode && model_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    mkdir(cfg.save_dir.c_str(), 0755);
    prune_old_pairs(cfg.save_dir, kKeepPairs);
    prune_old_viz(cfg.save_dir, kKeepViz);

    // 1. 先启动红外(优先保障 256x192@25; 两路 USB 独立,
    //    但 IR 优先仍是保守默认, 方案 §5.4)
    IrCamera ir_camera;
    IrCameraConfig ir_cfg;
    if (!ir_camera.open(ir_cfg) || !ir_camera.start()) {
        fprintf(stderr, "IR camera open/start failed\n");
        return 3;
    }
    printf("waiting IR stream to settle...\n");
    for (int w = 0;
         w < cfg.startup_timeout_ms / 100 && g_running.load(); ++w) {
        usleep(100000);
    }
    FrameRateInfo ir_check = ir_camera.fps_info();
    printf("[INIT] ir running: %dx%d, cur_fps=%.1f avg_fps=%.1f\n",
           ir_camera.width(), ir_camera.height(),
           ir_check.cur_fps, ir_check.avg_fps);
    if (ir_check.cur_fps < 20.0) {
        fprintf(stderr,
                "[INIT] warning: IR fps %.1f below target 25, continue\n",
                ir_check.cur_fps);
    }

    // ---- 阶段0 探针: 采 N 帧温度统计 ----
    // 注: NUC 快门事件会产生整帧极端值(raw 341/62837 量级),
    //     统计必须用帧中位数并跳过坏帧, 否则均值被污染
    if (probe_mode) {
        BgrFrame img;
        ThermalFrame temp;
        double median_sum = 0.0;
        float gmin = 1e9f, gmax = -1e9f;
        uint64_t n_frames = 0, n_bad = 0, n_px = 0;
        uint64_t last_seq = 0;
        for (int f = 0; f < cfg.probe_temp_frames && g_running.load(); ) {
            if (ir_camera.copy_latest(img, temp) &&
                temp.sequence != last_seq) {
                last_seq = temp.sequence;
                ++f;
                cv::Mat T = thermal_to_celsius(temp);
                // 帧级有效性: NUC 坏帧会跌到 -250℃ 以下或冲到 300℃ 以上
                double tmin = 1e9, tmax = -1e9;
                for (int r = 0; r < T.rows; ++r) {
                    const float* row = T.ptr<float>(r);
                    for (int c = 0; c < T.cols; ++c) {
                        if (row[c] < tmin) tmin = row[c];
                        if (row[c] > tmax) tmax = row[c];
                    }
                }
                if (tmin < -60.0 || tmax > 200.0) {
                    ++n_bad;
                    continue;
                }
                // 帧中位数(对残余噪声鲁棒)
                std::vector<float> v;
                v.reserve(T.total());
                for (int r = 0; r < T.rows; ++r) {
                    const float* row = T.ptr<float>(r);
                    for (int c = 0; c < T.cols; ++c) {
                        v.push_back(row[c]);
                    }
                }
                std::sort(v.begin(), v.end());
                median_sum += v[v.size() / 2];
                if (gmin > v.front()) gmin = v.front();
                if (gmax < v.back()) gmax = v.back();
                n_px += v.size();
                ++n_frames;
                usleep(40000);   // ~25fps
            } else {
                usleep(5000);
            }
        }
        printf("\n[PROBE] thermal good frames=%llu (skipped NUC-like %llu) "
               "pixels=%llu (%dx%d)\n",
               (unsigned long long)n_frames, (unsigned long long)n_bad,
               (unsigned long long)n_px, temp.width, temp.height);
        printf("[PROBE] celsius: frame-median avg=%.2f | global min=%.2f "
               "max=%.2f\n",
               n_frames ? median_sum / n_frames : 0.0, gmin, gmax);
        printf("[PROBE] encoding: uint16 K*64, C = raw/64 - 273.15\n");
        printf("[PROBE] verdict: 室内场景 median 应在 15~40 C 量级即"
               "情形 A(辐射测温)成立\n");
        // dump 一帧原始温度便于离线分析
        if (temp.valid()) {
            char path[256];
            snprintf(path, sizeof(path), "%s/probe_temp_raw_%dx%d.bin",
                     cfg.save_dir.c_str(), temp.width, temp.height);
            FILE* fp = fopen(path, "wb");
            if (fp != NULL) {
                fwrite(temp.data.data(), sizeof(uint16_t),
                       temp.data.size(), fp);
                fclose(fp);
                printf("[PROBE] dumped one raw frame -> %s\n", path);
            }
        }
        ir_camera.close();
        printf("[PROBE] done\n");
        return 0;
    }

    // 2. 加载模型
    rknn_app_context_t rknn_app_ctx;
    memset(&rknn_app_ctx, 0, sizeof(rknn_app_ctx));
    if (yolov8_detect_init(model_path.c_str(), &rknn_app_ctx) != 0) {
        fprintf(stderr, "yolov8_detect_init failed (model: %s)\n",
                model_path.c_str());
        ir_camera.close();
        return 2;
    }
    printf("[INIT] model loaded: %s\n", model_path.c_str());

    // 3. 可见光: 档位链自动协商(方案 §1.2; 用户显式 -W/-H 优先)
    VisibleCamera vis_camera;
    bool vis_ok = false;
    struct Preset { int w, h; const char* name; };
    const Preset presets[] = {
        { 1280, 960, "960p" }, { 1024, 768, "768p" },
        { 1280, 720, "720p" }, { 800, 600, "600p" },
        { 640, 480, "480p" },
    };
    if (cfg.user_res) {
        // 用户显式指定, 直接使用
        VisibleCameraConfig vis_cfg;
        vis_cfg.device_path = cfg.device;
        vis_cfg.width = cfg.width;
        vis_cfg.height = cfg.height;
        vis_cfg.fps = cfg.fps;
        vis_cfg.format = cfg.format;
        vis_ok = vis_camera.open(vis_cfg) && vis_camera.start();
        if (vis_ok) {
            printf("[INIT] vis requested %dx%d@%d -> negotiated %dx%d@%d\n",
                   cfg.width, cfg.height, cfg.fps, vis_camera.width(),
                   vis_camera.height(), vis_camera.fps());
        }
    } else {
        int start = 0;
        if (cfg.res_preset != "auto") {
            for (int k = 0; k < 5; ++k) {
                if (cfg.res_preset == presets[k].name) {
                    start = k;
                    break;
                }
            }
        }
        for (int k = start; k < 5 && g_running.load(); ++k) {
            VisibleCameraConfig vis_cfg;
            vis_cfg.device_path = cfg.device;
            vis_cfg.width = presets[k].w;
            vis_cfg.height = presets[k].h;
            vis_cfg.fps = cfg.fps;
            vis_cfg.format = cfg.format;
            if (vis_camera.open(vis_cfg) && vis_camera.start() &&
                vis_camera.width() == presets[k].w &&
                vis_camera.height() == presets[k].h) {
                printf("[INIT] vis requested %dx%d@%d -> negotiated "
                       "%dx%d@%d OK (%s, %.3f)\n",
                       presets[k].w, presets[k].h, cfg.fps,
                       vis_camera.width(), vis_camera.height(),
                       vis_camera.fps(), presets[k].name,
                       (double)vis_camera.width() / vis_camera.height());
                vis_ok = true;
                break;
            }
            vis_camera.stop();
            printf("[INIT] vis preset %s failed, trying next...\n",
                   presets[k].name);
            usleep(200000);
        }
    }
    if (!vis_ok) {
        fprintf(stderr, "visible camera open/start failed\n");
        ir_camera.close();
        yolov8_detect_release(&rknn_app_ctx);
        return 3;
    }
    printf("[INIT] vis running: %s %dx%d@%d\n",
           vis_camera.device_path().c_str(), vis_camera.width(),
           vis_camera.height(), vis_camera.fps());

    // 4. 配准/显著/提名/日志 初始化
    RegistrationConfig reg_cfg;
    reg_cfg.calib_path = cfg.calib_path;
    reg_cfg.ir_fov_diag = cfg.ir_fov;
    reg_cfg.vis_fov_diag = cfg.vis_fov;
    reg_cfg.margin_ratio = cfg.margin_ratio;
    reg_cfg.min_crop_ir = cfg.min_crop_ir;
    reg_cfg.min_crop_px = cfg.min_crop_px;
    reg_cfg.max_crop_px = cfg.max_crop_px;
    Registration registration;
    registration.init(reg_cfg, ir_camera.width(), ir_camera.height(),
                      vis_camera.width(), vis_camera.height());

    SaliencyConfig sal_cfg;
    sal_cfg.thr_mode = cfg.thr_mode;
    sal_cfg.thr_k = cfg.thr_k;
    sal_cfg.delta_min = cfg.delta_min;
    sal_cfg.bg_kernel = cfg.bg_kernel;
    Saliency saliency;
    saliency.init(sal_cfg, ir_camera.width(), ir_camera.height());

    NominatorConfig nom_cfg;
    nom_cfg.card = cfg.card;
    nom_cfg.min_area = cfg.min_area;
    nom_cfg.link_radius = cfg.link_radius;
    nom_cfg.body_lo = cfg.body_lo;
    nom_cfg.body_hi = cfg.body_hi;
    nom_cfg.dt_thr = cfg.dt_thr;
    Nominator nominator;
    nominator.init(nom_cfg);

    EventLog event_log;
    event_log.open(cfg.events_path);

    HealthShared health;
    VizSlot viz_slot;
    SalStats sal_stats;
    BoundedQueue<Nomination> nom_queue(32);   // 有界, 满丢最旧(§5.2)

    // 5. 显著线程(CPU/NEON; baseline 模式不启动)
    std::thread saliency_thread;
    const bool run_saliency = (cfg.mode != "baseline");
    if (run_saliency) {
        saliency_thread = std::thread([&]() {
            const int ir_w = ir_camera.width();
            const int ir_h = ir_camera.height();
            uint64_t last_seq = 0;
            cv::Mat T_prev, G_prev;
            int nuc_streak = 0;
            double last_sal_print = 0.0;
            std::vector<Nomination> noms;
            BgrFrame ir_img;
            ThermalFrame temp;
            BgrFrame vis_frame;

            while (g_running.load()) {
                BgrFrame img;
                ThermalFrame tp;
                if (!ir_camera.copy_latest(img, tp) ||
                    tp.sequence == last_seq) {
                    usleep(2000);
                    continue;
                }
                last_seq = tp.sequence;

                // 最近邻配对 VIS(|dt| <= 40ms, §5.3)
                double delta_ms = 0.0;
                if (!vis_camera.nearest_frame(img.timestamp_us, vis_frame,
                                               delta_ms) ||
                    delta_ms > 40.0) {
                    std::lock_guard<std::mutex> lk(sal_stats.m);
                    ++sal_stats.skip_align;
                    continue;
                }

                // 温度矩阵(CV_32F ℃) + VIS 灰度降采样到 IR 网格
                cv::Mat T_c = thermal_to_celsius(tp);
                // 帧级有效性门禁(阶段0实测: ~30% 帧为 NUC/增益切换坏帧,
                // 整帧极端值, 不进显著层否则产生假提名)
                double fmin = 0.0, fmax = 0.0;
                cv::minMaxIdx(T_c, &fmin, &fmax);
                if (fmin < -60.0 || fmax > 200.0) {
                    std::lock_guard<std::mutex> lk(sal_stats.m);
                    ++sal_stats.skip_align;   // 复用 skip 计数(对齐外丢弃)
                    continue;
                }
                cv::Mat vis_mat(vis_frame.height, vis_frame.width, CV_8UC3,
                                vis_frame.data.data(), vis_frame.stride);
                cv::Mat g_gray, g_small;
                cv::cvtColor(vis_mat, g_gray, cv::COLOR_BGR2GRAY);
                // ≥3x 降采样必须 INTER_AREA(方案 §4.3), 禁 INTER_LINEAR
                cv::resize(g_gray, g_small, cv::Size(ir_w, ir_h), 0, 0,
                           cv::INTER_AREA);

                // NUC 冻结检测: 连续帧温度几乎不变(§6.3)
                if (!T_prev.empty()) {
                    cv::Mat diff;
                    cv::absdiff(T_c, T_prev, diff);
                    cv::Scalar m = cv::mean(diff);
                    if (m[0] < cfg.nuc_freeze_thr) {
                        ++nuc_streak;
                    } else {
                        nuc_streak = 0;
                    }
                }
                const bool nuc_frozen = nuc_streak >= 3;
                {
                    std::lock_guard<std::mutex> lk(health.m);
                    // 注意: 此处不得清零 contrast — 主线程 500ms 轮询,
                    // 清零与 sal 回填之间的间隙会被误读为热交叉(假 DEGRADE)
                    health.nuc_frozen = nuc_frozen;
                }

                const double t0 = now_ms();
                Saliency::Output sal = saliency.process(T_c, g_small);
                const double sal_ms = now_ms() - t0;

                {
                    std::lock_guard<std::mutex> lk(health.m);
                    health.scene_contrast_k = sal.scene_contrast_k;
                    health.valid = sal.valid;
                }

                noms.clear();
                if (!nuc_frozen && sal.valid) {
                    nominator.process(sal, T_c, g_small, G_prev,
                                      registration, img.timestamp_us,
                                      vis_frame.timestamp_us, now_ms(),
                                      noms);
                    for (size_t i = 0; i < noms.size(); ++i) {
                        nom_queue.push(noms[i]);
                    }
                }

                // T_c 槽供 eventlog 的场景 median 取用(不受 no_viz 控制);
                // 其余 viz 素材仅可视化开启时更新
                {
                    std::lock_guard<std::mutex> lk(viz_slot.m);
                    viz_slot.T_c = T_c.clone();
                    viz_slot.valid = true;
                    if (!cfg.no_viz) {
                        viz_slot.sn_tir = sal.sn_tir.clone();
                        viz_slot.sn_rgb = sal.sn_rgb.clone();
                        viz_slot.mask_q1 = sal.mask_q1.clone();
                        viz_slot.mask_q2 = sal.mask_q2.clone();
                        viz_slot.mask_q3 = sal.mask_q3.clone();
                        viz_slot.last_noms = noms;
                    }
                }

                {
                    std::lock_guard<std::mutex> lk(sal_stats.m);
                    ++sal_stats.frames;
                    sal_stats.sum_ms += sal_ms;
                    if (sal_ms > sal_stats.max_ms) {
                        sal_stats.max_ms = sal_ms;
                    }
                }

                // [SAL] 周期日志(§7.1)
                const double t_now = now_ms();
                if (last_sal_print == 0.0 ||
                    t_now - last_sal_print >= 2000.0) {
                    const Nominator::Stats& st = nominator.stats();
                    uint64_t skip = 0;
                    {
                        std::lock_guard<std::mutex> lk(sal_stats.m);
                        skip = sal_stats.skip_align;
                    }
                    printf("[SAL] #%llu q1=%lld q2=%lld q3=%lld "
                           "tir_only=%lld contrast=%.1fK sal=%.1fms "
                           "skip_align=%llu%s\n",
                           (unsigned long long)last_seq,
                           (long long)st.q1, (long long)st.q2,
                           (long long)st.q3, (long long)st.tir_only,
                           sal.scene_contrast_k, sal_ms,
                           (unsigned long long)skip,
                           nuc_frozen ? " [NUC-FROZEN]" : "");
                    last_sal_print = t_now;
                }

                T_prev = T_c.clone();
                G_prev = g_small.clone();
            }
        });
    }

    // 6. 主循环(NPU)
    detect_result_group_t results;
    uint64_t npu_calls = 0;
    double sum_infer = 0.0;
    uint64_t obj_frames = 0;
    uint64_t saved_pairs = 0;
    uint64_t viz_written = 0;
    double sum_save = 0.0, max_save = 0.0, sum_align = 0.0, max_align = 0.0;
    double last_fps_print = 0.0;
    double last_viz_write = 0.0;
    double last_health_check = 0.0;
    uint64_t degrade_events = 0;
    uint64_t degrade_crossover = 0, degrade_nuc = 0;
    bool degraded = false;            // auto 模式的降级状态
    uint64_t last_seq_baseline = 0;
    const bool auto_mode = (cfg.mode == "auto");

    VizGrid viz_grid;
    printf("press Ctrl+C to exit\n");

    while (g_running.load()) {
        // ---- auto 模式健康检查(每 500ms; §6.3 优雅降级) ----
        if (auto_mode && run_saliency) {
            const double t_now = now_ms();
            if (t_now - last_health_check >= 500.0) {
                last_health_check = t_now;
                double ck = 999.0;
                bool frozen = false;
                {
                    std::lock_guard<std::mutex> lk(health.m);
                    ck = health.scene_contrast_k;
                    frozen = health.nuc_frozen;
                }
                if (!degraded) {
                    const char* reason = NULL;
                    if (ck < cfg.c_min) {
                        reason = "crossover";
                    } else if (frozen) {
                        reason = "nuc_freeze";
                    }
                    if (reason != NULL) {
                        degraded = true;
                        ++degrade_events;
                        if (std::string(reason) == "crossover") {
                            ++degrade_crossover;
                        } else {
                            ++degrade_nuc;
                        }
                        printf("[DEGRADE] reason=%s contrast=%.1fK -> "
                               "fallback per-frame\n",
                               reason, ck);
                    }
                } else if (ck >= cfg.c_min * 1.2 && !frozen) {
                    degraded = false;
                    printf("[RECOVER] contrast=%.1fK -> back to cascade\n",
                           ck);
                }
            }
        }

        // ---- 降级态/auto 降级: 逐帧全图兜底 ----
        if ((cfg.mode == "baseline") ||
            (auto_mode && degraded && run_saliency)) {
            BgrFrame frame;
            bool got = false;
            while (g_running.load()) {
                if (vis_camera.copy_latest(frame) &&
                    frame.sequence != last_seq_baseline) {
                    got = true;
                    break;
                }
                usleep(2000);
            }
            if (!got) {
                break;
            }
            last_seq_baseline = frame.sequence;
            cv::Mat image(frame.height, frame.width, CV_8UC3,
                          frame.data.data(), frame.stride);
            double infer_ms = 0.0;
            yolov8_detect_run(&rknn_app_ctx, image, &results, &infer_ms);
            ++npu_calls;
            sum_infer += infer_ms;
            if (results.count > 0) {
                ++obj_frames;
            }
            printf("#%llu infer=%.1fms obj=%d%s\n",
                   (unsigned long long)frame.sequence, infer_ms,
                   results.count,
                   (auto_mode && degraded) ? " [FALLBACK]" : "");
            for (int i = 0; i < results.count; i++) {
                detect_result_t* det = &(results.results[i]);
                printf("  %s %.1f%% @ (%d %d %d %d)\n", det->name,
                       det->prop * 100, det->box.left, det->box.top,
                       det->box.right, det->box.bottom);
            }
        } else if (run_saliency) {
            // ---- 级联: 消费提名 → crop → YOLO 确认 ----
            Nomination nom;
            if (nom_queue.pop(nom, 100, g_running.load())) {
                // 取提名时刻的 VIS 帧(最近邻)
                BgrFrame vis_frame;
                double vis_delta = 0.0;
                if (!vis_camera.nearest_frame(nom.ts_vis_us, vis_frame,
                                               vis_delta)) {
                    continue;
                }
                cv::Mat image(vis_frame.height, vis_frame.width, CV_8UC3,
                              vis_frame.data.data(), vis_frame.stride);
                cv::Rect roi = nom.roi_vis &
                               cv::Rect(0, 0, image.cols, image.rows);
                if (roi.area() <= 0) {
                    continue;
                }
                cv::Mat crop = image(roi).clone();   // 保留原分辨率细节

                double infer_ms = 0.0;
                yolov8_detect_run(&rknn_app_ctx, crop, &results,
                                  &infer_ms);
                ++npu_calls;
                sum_infer += infer_ms;

                // semantic 填充(检不出是正常结果: "热异常+视觉无物")
                if (results.count > 0) {
                    ++obj_frames;
                    nom.semantic.label = results.results[0].name;
                    nom.semantic.score = results.results[0].prop;
                    nom.semantic.filled_by = "vendor_default";
                } else {
                    nom.semantic.label = "none";
                    nom.semantic.score = 0.0f;
                    nom.semantic.filled_by = "none";
                }

                // [EVT](§7.1)
                printf("[EVT] id=%llu quad=Q%d tmax=%.1f est=%.1f "
                       "dT=%+.1fK area=%dpx sal_diff=%.2f age=%.2fs "
                       "valid=%d%s\n",
                       (unsigned long long)nom.id, nom.quadrant,
                       nom.t_max, nom.t_true_est, nom.delta_t,
                       nom.area_px, nom.sal_diff, nom.persistence_s,
                       nom.valid,
                       nom.quadrant == 2
                           ? (std::string(" living=") +
                              (nom.score_living > 0.0f ? "1" : "0"))
                                 .c_str()
                           : "");
                // [NPU]
                printf("[NPU] id=%llu crop=%dx%d infer=%.1fms obj=%d "
                       "label=%s\n",
                       (unsigned long long)nom.id, roi.width, roi.height,
                       infer_ms, results.count,
                       nom.semantic.label.c_str());

                // 事件流
                {
                    float ir_median = 0.0f;
                    int q1c = 0, q2c = 0, q3c = 0;
                    {
                        std::lock_guard<std::mutex> lk(viz_slot.m);
                        if (!viz_slot.T_c.empty()) {
                            cv::Scalar m = cv::mean(viz_slot.T_c);
                            ir_median = (float)m[0];
                        }
                        q1c = (int)viz_slot.last_noms.size();
                    }
                    event_log.write(nom, ir_median, q1c, q2c, q3c,
                                    vis_frame.width, vis_frame.height);
                }

                // 成对保存(被接受的提名: vis 画 roi 框 + ir 原始图)
                {
                    const double t_save0 = now_ms();
                    BgrFrame ir_pair;
                    ThermalFrame tp_pair;
                    double align_ms = 0.0;
                    const bool ir_ok = ir_camera.nearest_pair(
                        nom.ts_pair_us, ir_pair, tp_pair, align_ms);

                    cv::Mat annotated = image.clone();
                    cv::rectangle(annotated, roi,
                                  nom.quadrant == 1
                                      ? cv::Scalar(0, 0, 255)
                                      : cv::Scalar(255, 0, 0),
                                  2);
                    char tag[96];
                    snprintf(tag, sizeof(tag), "Q%d dT=%.1fK %s",
                             nom.quadrant, nom.delta_t,
                             nom.semantic.label.c_str());
                    cv::putText(annotated, tag,
                                cv::Point(roi.x, std::max(16, roi.y - 6)),
                                cv::FONT_HERSHEY_SIMPLEX, 0.5,
                                cv::Scalar(0, 255, 255), 1, cv::LINE_AA);

                    bool saved = false;
                    if (ir_ok) {
                        const std::string prefix =
                            make_prefix("det");
                        const std::string vis_path =
                            cfg.save_dir + "/" + prefix + "_vis.jpg";
                        const std::string ir_path =
                            cfg.save_dir + "/" + prefix + "_ir.jpg";
                        cv::Mat ir_mat(ir_pair.height, ir_pair.width,
                                       CV_8UC3, ir_pair.data.data(),
                                       ir_pair.stride);
                        cv::rectangle(ir_mat, nom.box_ir,
                                      cv::Scalar(0, 255, 255), 1);
                        const bool ok_vis = cv::imwrite(vis_path, annotated);
                        const bool ok_ir = cv::imwrite(ir_path, ir_mat);
                        saved = ok_vis && ok_ir;
                        if (saved) {
                            ++saved_pairs;
                            prune_old_pairs(cfg.save_dir, kKeepPairs);
                        }
                        sum_align += align_ms;
                        if (align_ms > max_align) {
                            max_align = align_ms;
                        }
                    }
                    const double save_ms = now_ms() - t_save0;
                    sum_save += save_ms;
                    if (save_ms > max_save) {
                        max_save = save_ms;
                    }
                    if (saved) {
                        printf("[SAVE] pair save=%.1fms align=%.1fms\n",
                               save_ms, align_ms);
                    }
                }

                // 六宫格(提名触发, 限频 500ms — compose+imwrite ~100ms,
                // 不限频会拖慢提名消费导致配对 align 恶化)
                if (!cfg.no_viz &&
                    now_ms() - last_viz_write >= 500.0) {
                    VizGrid::Input vi;
                    vi.vis_bgr = image;
                    {
                        std::lock_guard<std::mutex> lk(viz_slot.m);
                        if (viz_slot.valid) {
                            vi.T_c = viz_slot.T_c.clone();
                            vi.sn_tir = viz_slot.sn_tir.clone();
                            vi.sn_rgb = viz_slot.sn_rgb.clone();
                            vi.mask_q1 = viz_slot.mask_q1.clone();
                            vi.mask_q2 = viz_slot.mask_q2.clone();
                            vi.mask_q3 = viz_slot.mask_q3.clone();
                        }
                    }
                    if (!vi.T_c.empty()) {
                        vi.noms.push_back(nom);
                        cv::Mat grid =
                            viz_grid.compose(vi, registration, 480);
                        const std::string p =
                            cfg.save_dir + "/" + make_prefix("viz") + ".jpg";
                        if (cv::imwrite(p, grid)) {
                            ++viz_written;
                            last_viz_write = now_ms();
                            prune_old_viz(cfg.save_dir, kKeepViz);
                        }
                    }
                }
            }
        } else {
            usleep(10000);
        }

        // ---- 周期六宫格 ----
        if (!cfg.no_viz && run_saliency) {
            const double t_now = now_ms();
            if (last_viz_write == 0.0 ||
                t_now - last_viz_write >= cfg.viz_interval * 1000.0) {
                BgrFrame frame;
                if (vis_camera.copy_latest(frame)) {
                    VizGrid::Input vi;
                    vi.vis_bgr = cv::Mat(frame.height, frame.width, CV_8UC3,
                                         frame.data.data(), frame.stride)
                                     .clone();
                    {
                        std::lock_guard<std::mutex> lk(viz_slot.m);
                        if (viz_slot.valid) {
                            vi.T_c = viz_slot.T_c.clone();
                            vi.sn_tir = viz_slot.sn_tir.clone();
                            vi.sn_rgb = viz_slot.sn_rgb.clone();
                            vi.mask_q1 = viz_slot.mask_q1.clone();
                            vi.mask_q2 = viz_slot.mask_q2.clone();
                            vi.mask_q3 = viz_slot.mask_q3.clone();
                            vi.noms = viz_slot.last_noms;
                        }
                    }
                    if (!vi.T_c.empty()) {
                        cv::Mat grid =
                            viz_grid.compose(vi, registration, 480);
                        const std::string p =
                            cfg.save_dir + "/" + make_prefix("viz") + ".jpg";
                        if (cv::imwrite(p, grid)) {
                            ++viz_written;
                            last_viz_write = now_ms();
                            prune_old_viz(cfg.save_dir, kKeepViz);
                        }
                    }
                }
            }
        }

        // ---- [FPS] 周期 ----
        const double t_now2 = now_ms();
        if (last_fps_print == 0.0 || t_now2 - last_fps_print >= 2000.0) {
            FrameRateInfo ir_fps = ir_camera.fps_info();
            FrameRateInfo vis_fps = vis_camera.fps_info();
            printf("[FPS] ir : cur=%.1f avg=%.1f (n=%llu) | vis: "
                   "cur=%.1f avg=%.1f (n=%llu)\n",
                   ir_fps.cur_fps, ir_fps.avg_fps,
                   (unsigned long long)ir_fps.total, vis_fps.cur_fps,
                   vis_fps.avg_fps, (unsigned long long)vis_fps.total);
            last_fps_print = t_now2;
        }
    }

    // 7. 清理: 先取统计 → join 显著线程 → 停相机 → 释放模型
    FrameRateInfo ir_final_fps = ir_camera.fps_info();
    FrameRateInfo vis_final_fps = vis_camera.fps_info();

    uint64_t sal_frames = 0, skip_align = 0;
    double sal_sum = 0.0, sal_max = 0.0;
    {
        std::lock_guard<std::mutex> lk(sal_stats.m);
        sal_frames = sal_stats.frames;
        skip_align = sal_stats.skip_align;
        sal_sum = sal_stats.sum_ms;
        sal_max = sal_stats.max_ms;
    }

    if (saliency_thread.joinable()) {
        saliency_thread.join();
    }
    vis_camera.stop();
    ir_camera.close();
    yolov8_detect_release(&rknn_app_ctx);
    event_log.close();

    // ---- [STAT] 退出汇总(§7.3, 判决指标带 >>>) ----
    const Nominator::Stats& nst = nominator.stats();
    printf("\nexit by Ctrl+C\n");
    printf("[STAT] ir frames=%llu avg_fps=%.1f | vis frames=%llu "
           "avg_fps=%.1f\n",
           (unsigned long long)ir_final_fps.total, ir_final_fps.avg_fps,
           (unsigned long long)vis_final_fps.total,
           vis_final_fps.avg_fps);
    if (run_saliency) {
        printf("[STAT] sal frames=%llu sal_ms avg=%.1f max=%.1f | "
               "skip_align=%llu (%.2f%%)\n",
               (unsigned long long)sal_frames,
               sal_frames ? sal_sum / sal_frames : 0.0, sal_max,
               (unsigned long long)skip_align,
               sal_frames ? 100.0 * skip_align / sal_frames : 0.0);
        printf("[STAT] nominate tir_only=%llu q1=%llu q2=%llu q3=%llu "
               "| px: tir=%llu q1=%llu\n",
               (unsigned long long)nst.tir_only,
               (unsigned long long)nst.q1, (unsigned long long)nst.q2,
               (unsigned long long)nst.q3,
               (unsigned long long)nst.tir_px,
               (unsigned long long)nst.q1_px);
        // FAR 判决用像素口径: 组件口径在 rgb mask 咬洞碎片化时
        // q1 可能 > tir_only(FAR 为负, 方案 §14 风险#4);
        // 像素包含关系 q1_px ⊆ tir_px 恒成立, 口径稳健
        if (nst.tir_px > 0) {
            printf("[STAT] >>> FAR(diff false-alarm reduction, px) = "
                   "%.1f%%\n",
                   100.0 * (1.0 -
                            static_cast<double>(nst.q1_px) /
                                static_cast<double>(nst.tir_px)));
        }
        if (sal_frames > 0) {
            printf("[STAT] >>> NPU calls: cascade %llu / sal frames %llu "
                   "= %.1f%% (baseline would be 100%%)\n",
                   (unsigned long long)npu_calls,
                   (unsigned long long)sal_frames,
                   100.0 * npu_calls / sal_frames);
        }
    } else {
        printf("[STAT] baseline mode: NPU calls = vis frames handled\n");
    }
    printf("[STAT] mode=%s card=%s%s degrade_events=%llu "
           "(crossover %llu, nuc_freeze %llu)\n",
           cfg.mode.c_str(), cfg.card.c_str(),
           registration.calibrated() ? " calib=quad" : " calib=fov",
           (unsigned long long)degrade_events,
           (unsigned long long)degrade_crossover,
           (unsigned long long)degrade_nuc);
    printf("[STAT] infer call=%llu avg=%.1fms obj_frames=%llu | "
           "saved_pairs=%llu viz=%llu events=%llu queue_dropped=%llu\n",
           (unsigned long long)npu_calls,
           npu_calls ? sum_infer / npu_calls : 0.0,
           (unsigned long long)obj_frames,
           (unsigned long long)saved_pairs,
           (unsigned long long)viz_written,
           (unsigned long long)event_log.count(),
           (unsigned long long)nom_queue.dropped());
    if (saved_pairs) {
        printf("[STAT] save avg=%.1fms max=%.1fms align avg=%.1fms "
               "max=%.1fms\n",
               sum_save / saved_pairs, max_save,
               sum_align / saved_pairs, max_align);
    }
    return 0;
}
