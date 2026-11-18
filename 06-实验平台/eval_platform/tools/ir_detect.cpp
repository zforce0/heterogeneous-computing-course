// ir_detect —— 热成像（IR）通道的端侧检测器，跑在 RV1126B 上。
//
// 为什么单独一个进程：
//   热成像模组被厂商闭源 SDK（ir_snap，usbfs 独占）抓着，别的进程开不了设备；
//   但 ir_snap 会把最新一帧落到 /tmp/ir_sdk.raw（12 字节头 + BGR）。本程序只读这个
//   文件，既不解绑也不抢设备，因此可以和实时预览并行跑。
//
// 两级检测（都在端侧做，不占 Mac、不占昇腾）：
//   A. 神经网络：复用可见光的 YOLOv8 RKNN 模型，在 NPU 上对 IR 帧推理。
//      这是「跨模态复用」——模型本身是在可见光 COCO 上训练的，对热成像只是灰度
//      纹理，检出能力必须实测，不能假设。
//   B. 传统 CV 热斑：按帧内灰度中位数 + MAD 自适应阈值找亮区（相对辐照，不是温度），
//      补上 A 漏掉的无纹理热源（如刚放下的热水杯、被遮挡的人）。
//
// 输出 /tmp/ir_boxes.json：{ts_ms,seq,infer_ms,res,yolo[],blobs[],stats{}}
// 注意：全部数值都是 0..255 的相对灰度，不是摄氏度——本模组未做温度标定。
#include "yolov8.h"
#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>

static const char* RAW_PATH = "/tmp/ir_sdk.raw";
static const char* OUT_PATH = "/tmp/ir_boxes.json";
static const char* MODEL_DEFAULT = "/userdata/rv1126_test/yolov8m_rv1126b.rknn";

// 送进模型的尺寸（IR 原图 256x192，同为 4:3，等比放大不引入形变）
static int g_in_w = 640, g_in_h = 480;
static bool g_yolo = true;
static bool g_verbose = false;
// RKNN 模型固定 640x640 输入，单帧约 100ms 且要跟可见光 YOLO 抢 NPU；
// 因此神经网络每 N 帧跑一次（默认 2 ≈ 3 Hz），热斑检测仍然逐帧做（毫秒级）。
static int g_yolo_every = 2;

static long long now_ms(){ struct timeval t; gettimeofday(&t,0); return (long long)t.tv_sec*1000+t.tv_usec/1000; }

static void write_atomic(const char* tmp, const char* dst, const std::string& data){
  FILE* f = fopen(tmp, "wb"); if (!f) return;
  fwrite(data.data(), 1, data.size(), f); fclose(f);
  rename(tmp, dst);
}

struct Blob { int l,t,r,b,area; int peak; double mean; };

// 帧内自适应阈值：中位数 + k*MAD（MAD 用 1.4826 归一到 σ 量级），再设一个 8/255 的
// 最小对比度地板，避免场景很均匀时把噪声当成热源。
static void frame_stats(const cv::Mat& gray, int& med, int& p98, double& mad, int& thr){
  int hist[256] = {0};
  for (int y = 0; y < gray.rows; ++y) {
    const uchar* p = gray.ptr<uchar>(y);
    for (int x = 0; x < gray.cols; ++x) hist[p[x]]++;
  }
  long long total = (long long)gray.rows * gray.cols;
  long long acc = 0; med = 0; p98 = 255;
  bool got_med = false, got_p98 = false;
  for (int i = 0; i < 256; ++i) {
    acc += hist[i];
    if (!got_med && acc >= total / 2) { med = i; got_med = true; }
    if (!got_p98 && acc >= total * 98 / 100) { p98 = i; got_p98 = true; }
  }
  // 偏离中位数的绝对偏差再做一次中位数 = MAD
  int dev[256] = {0};
  long long dev_total = 0;
  for (int i = 0; i < 256; ++i) {
    if (!hist[i]) continue;
    int d = std::abs(i - med);
    dev[d] += hist[i]; dev_total += hist[i];
  }
  long long a2 = 0; int mad_i = 0;
  for (int d = 0; d < 256; ++d) {
    a2 += dev[d];
    if (a2 >= dev_total / 2) { mad_i = d; break; }
  }
  mad = mad_i * 1.4826;
  double t = med + 6.0 * mad;
  if (t < med + 8) t = med + 8;            // 对比度地板
  thr = (int)(t > 255 ? 255 : t);
}

static void hot_blobs(const cv::Mat& gray, int thr, std::vector<Blob>& out){
  cv::Mat mask;
  cv::threshold(gray, mask, thr, 255, cv::THRESH_BINARY);
  cv::Mat k = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3,3));
  cv::morphologyEx(mask, mask, cv::MORPH_OPEN, k);   // 去单像素噪声
  cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, k);  // 补小块空洞
  cv::Mat lbl, stats, cent;
  int n = cv::connectedComponentsWithStats(mask, lbl, stats, cent, 8);
  const int min_area = (int)(gray.total() * 0.0015);   // ≈74 px @256x192
  const int max_area = (int)(gray.total() * 0.45);
  for (int i = 1; i < n; ++i) {
    int area = stats.at<int>(i, cv::CC_STAT_AREA);
    if (area < min_area || area > max_area) continue;
    Blob b;
    b.l = stats.at<int>(i, cv::CC_STAT_LEFT);
    b.t = stats.at<int>(i, cv::CC_STAT_TOP);
    b.r = b.l + stats.at<int>(i, cv::CC_STAT_WIDTH) - 1;
    b.b = b.t + stats.at<int>(i, cv::CC_STAT_HEIGHT) - 1;
    b.area = area;
    // 峰值 / 均值：在该连通域内取，用于给「有多亮」一个相对量（非温度）
    cv::Mat submask = (lbl(cv::Rect(b.l, b.t, b.r - b.l + 1, b.b - b.t + 1)) == i);
    double mean = cv::mean(gray(cv::Rect(b.l, b.t, b.r - b.l + 1, b.b - b.t + 1)), submask)[0];
    double mx = 0; cv::minMaxLoc(gray(cv::Rect(b.l, b.t, b.r - b.l + 1, b.b - b.t + 1)), NULL, &mx, NULL, NULL, submask);
    b.peak = (int)mx; b.mean = mean;
    out.push_back(b);
  }
  std::sort(out.begin(), out.end(), [](const Blob& a, const Blob& b){ return a.area > b.area; });
  if (out.size() > 5) out.resize(5);
}

int main(int argc, char** argv){
  std::string model = MODEL_DEFAULT;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--no-yolo") g_yolo = false;
    else if (a == "-v") g_verbose = true;
    else if (a == "--size" && i + 1 < argc) {
      g_in_w = atoi(argv[++i]); g_in_h = g_in_w * 3 / 4;
    } else if (a == "--yolo-every" && i + 1 < argc) {
      g_yolo_every = atoi(argv[++i]); if (g_yolo_every < 1) g_yolo_every = 1;
    } else if (a[0] != '-') model = a;
  }

  rknn_app_context_t ctx; memset(&ctx, 0, sizeof(ctx));
  if (g_yolo) {
    if (yolov8_detect_init(model.c_str(), &ctx) < 0) {
      fprintf(stderr, "[ir_detect] rknn init failed: %s (fallback to blob-only)\n", model.c_str());
      g_yolo = false;
    }
  }
  fprintf(stderr, "[ir_detect] start model=%s yolo=%d in=%dx%d every=%d\n",
          model.c_str(), (int)g_yolo, g_in_w, g_in_h, g_yolo_every);

  // 模型身份随结果一起上报：换模型只需改启动参数（或 YOLO_MODEL 环境变量），
  // 页面从 status.json 读，不写死任何文件名。
  long long model_bytes = 0, model_mtime = 0;
  {
    struct stat mst;
    if (stat(model.c_str(), &mst) == 0) {
      model_bytes = (long long)mst.st_size;
      model_mtime = (long long)mst.st_mtim.tv_sec;
    }
  }
  std::string model_name = model.substr(model.rfind('/') == std::string::npos ? 0 : model.rfind('/') + 1);

  long long seq = 0;
  struct stat st; long long last_mtime = -1, last_size = -1; int idle = 0;
  for (;;) {
    if (stat(RAW_PATH, &st) != 0) { usleep(100000); continue; }
    // 必须用纳秒级 mtime：st_mtime 只到秒，会把 5 fps 的新帧误判成「同一帧」，
    // 结果检测频率被钉死在 1 Hz，看起来"在跑"但一整秒才看一次。
    long long mtime = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
    long long size = (long long)st.st_size;
    if (mtime == last_mtime && size == last_size) {      // 没新帧就别重复算同一张
      if (++idle % 50 == 0 && g_verbose) fprintf(stderr, "[ir_detect] waiting for new IR frame\n");
      usleep(40000); continue;
    }
    last_mtime = mtime;
    last_size = size;
    ++seq;

    FILE* f = fopen(RAW_PATH, "rb");
    if (!f) { usleep(100000); continue; }
    unsigned hdr[3] = {0,0,0};
    if (fread(hdr, 4, 3, f) != 3) { fclose(f); usleep(100000); continue; }
    int w = (int)hdr[0], h = (int)hdr[1], stride = (int)hdr[2];
    if (w <= 0 || h <= 0 || stride < w * 3 || w > 2048 || h > 2048) { fclose(f); usleep(100000); continue; }
    std::vector<unsigned char> buf((size_t)stride * h);
    size_t got = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (got < buf.size()) { usleep(100000); continue; }

    cv::Mat bgr(h, w, CV_8UC3, buf.data(), stride);
    cv::Mat gray; cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);

    // ---- A. NPU 推理（跨模态复用可见光模型；按 --yolo-every 降频，其余帧复用上次结果）----
    static std::string yolo_js;            // 上次推理的框（已映射回 IR 原图坐标）
    static int ycount = 0;
    static double infer_ms = -1;
    static long long yolo_ts = 0;
    if (g_yolo && (seq % g_yolo_every) == 0) {
      cv::Mat big; cv::resize(bgr, big, cv::Size(g_in_w, g_in_h), 0, 0, cv::INTER_LINEAR);
      detect_result_group_t od; memset(&od, 0, sizeof(od));
      double ms = 0;
      yolo_js.clear(); ycount = 0;
      if (yolov8_detect_run(&ctx, big, &od, &ms) == 0 && od.count > 0) {
        infer_ms = ms;
        double sx = (double)w / g_in_w, sy = (double)h / g_in_h;   // 框从输入尺寸映射回 IR 原图
        for (int i = 0; i < od.count && i < 12; ++i) {
          const detect_result_t& d = od.results[i];
          int l = (int)(d.box.left * sx), t = (int)(d.box.top * sy);
          int r = (int)(d.box.right * sx), b2 = (int)(d.box.bottom * sy);
          char one[192];
          snprintf(one, sizeof(one), "%s{\"name\":\"%s\",\"score\":%.3f,\"box\":[%d,%d,%d,%d]}",
                   ycount ? "," : "", d.name, d.prop, l, t, r, b2);
          yolo_js += one; ycount++;
        }
      } else if (infer_ms < 0) {
        infer_ms = ms;
      }
      yolo_ts = now_ms();
    }

    // ---- B. 传统 CV 热斑 ----
    int med, p98, thr; double mad;
    frame_stats(gray, med, p98, mad, thr);
    std::vector<Blob> blobs; hot_blobs(gray, thr, blobs);
    std::string blob_js;
    for (size_t i = 0; i < blobs.size(); ++i) {
      char one[192];
      snprintf(one, sizeof(one), "%s{\"box\":[%d,%d,%d,%d],\"area\":%d,\"peak\":%d,\"mean\":%.1f}",
               i ? "," : "", blobs[i].l, blobs[i].t, blobs[i].r, blobs[i].b,
               blobs[i].area, blobs[i].peak, blobs[i].mean);
      blob_js += one;
    }

    char head[768];
    snprintf(head, sizeof(head),
      "{\"ts_ms\":%lld,\"seq\":%lld,\"raw\":\"%s\",\"res\":[%d,%d],\"infer_ms\":%.1f,"
      "\"engine\":\"%s\",\"model\":\"%s\",\"model_path\":\"%s\",\"model_bytes\":%lld,"
      "\"model_mtime\":%lld,\"stats\":{\"median\":%d,\"p98\":%d,\"mad\":%.1f,\"thr\":%d},"
      "\"yolo_age_ms\":%lld,\"yolo_count\":%d,\"yolo\":[%s],\"blobs\":[%s]}",
      now_ms(), seq, RAW_PATH, w, h, infer_ms,
      g_yolo ? "YOLOv8-RKNN+CV热斑" : "CV热斑",
      model_name.c_str(), model.c_str(), model_bytes, model_mtime,
      med, p98, mad, thr,
      yolo_ts ? now_ms() - yolo_ts : -1, ycount, yolo_js.c_str(), blob_js.c_str());
    write_atomic("/tmp/ir_boxes.tmp", OUT_PATH, std::string(head));
    if (g_verbose)
      fprintf(stderr, "[ir_detect] seq=%lld yolo=%d blob=%zu thr=%d med=%d p98=%d %.1fms\n",
              seq, ycount, blobs.size(), thr, med, p98, infer_ms);
    usleep(20000);
  }
  if (g_yolo) yolov8_detect_release(&ctx);
  return 0;
}
