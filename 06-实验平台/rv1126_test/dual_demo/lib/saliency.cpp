// 差分显著层实现(实现方案 §4.2 ~ §4.4):
//   TIR: FT 算子(|T - GaussianBlur(T)|, 大核背景) + THT 物理阈值(默认)
//   RGB: VIS 灰度降采样到 IR 网格后同构 FT 算子
//   两路各自 P99 归一化 → 四象限判定 → 闭运算
//
// 算子纪律(踩坑教训, 见方案 §4.2):
//   - 禁用 LC(局部对比): 只点亮边缘, 大块均温区内部为零
//   - SR(频谱残差)在 256x192 上贵且收益小, 不用

#include "saliency.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

void Saliency::init(const SaliencyConfig& cfg, int ir_w, int ir_h)
{
    cfg_ = cfg;
    if ((cfg_.bg_kernel & 1) == 0) {
        cfg_.bg_kernel += 1;   // 高斯核必须奇数
    }
    ir_w_ = ir_w;
    ir_h_ = ir_h;
}

// 直方图法求分位数(O(n), 代替全帧 std::sort — sal_ms 预算的关键)
// q ∈ [0,1]; 值域 [lo, hi]
static float hist_percentile(const cv::Mat& S, double q, float lo,
                             float hi)
{
    if (S.empty() || hi <= lo) {
        return lo;
    }
    const int bins = 1024;
    static int hist[bins];
    memset(hist, 0, sizeof(hist));
    const double total = static_cast<double>(S.total());
    const float span = hi - lo;
    for (int r = 0; r < S.rows; ++r) {
        const float* row = S.ptr<float>(r);
        for (int c = 0; c < S.cols; ++c) {
            int b = static_cast<int>((row[c] - lo) / span * (bins - 1));
            if (b < 0) {
                b = 0;
            } else if (b >= bins) {
                b = bins - 1;
            }
            ++hist[b];
        }
    }
    const double target = q * total;
    double acc = 0.0;
    for (int b = 0; b < bins; ++b) {
        acc += hist[b];
        if (acc >= target && acc > 0) {
            return lo + static_cast<float>(static_cast<double>(b) + 1.0) /
                            bins * span;
        }
    }
    return hi;
}

cv::Mat Saliency::normalize_p99(const cv::Mat& S)
{
    // 稳健归一化: 用 P99 做分母, 避免单个极值点压扁整张图
    double dmin = 0.0, dmax = 0.0;
    cv::minMaxIdx(S, &dmin, &dmax);
    if (dmax <= 0.0) {
        return cv::Mat::zeros(S.size(), CV_32F);
    }
    const float p99 = hist_percentile(S, 0.99, 0.0f,
                                      static_cast<float>(dmax));
    const float denom = std::max(p99, 1.0e-6f);
    cv::Mat out(S.size(), CV_32F);
    const float scale = 1.0f / denom;
    for (int r = 0; r < S.rows; ++r) {
        const float* src = S.ptr<float>(r);
        float* dst = out.ptr<float>(r);
        for (int c = 0; c < S.cols; ++c) {
            dst[c] = std::min(src[c] * scale, 1.0f);
        }
    }
    return out;
}

double Saliency::tht_temperature(const cv::Mat& T, double& bg_mode)
{
    // 三角直方图阈值(THT): 温度域直方图, 找背景峰值后做三角法分割
    // 阈值带物理量纲("高于背景峰值 X K"), 可解释性强
    const int bins = 256;
    const double t_lo = -20.0, t_hi = 160.0;   // 覆盖常见场景
    std::vector<int> hist(bins, 0);
    for (int r = 0; r < T.rows; ++r) {
        const float* row = T.ptr<float>(r);
        for (int c = 0; c < T.cols; ++c) {
            int b = static_cast<int>(
                (row[c] - t_lo) / (t_hi - t_lo) * (bins - 1));
            if (b < 0) {
                b = 0;
            } else if (b >= bins) {
                b = bins - 1;
            }
            ++hist[b];
        }
    }
    // 背景峰值 = 最大直方图 bin(常温背景占绝大多数像素)
    int peak = 0;
    for (int i = 1; i < bins; ++i) {
        if (hist[i] > hist[peak]) {
            peak = i;
        }
    }
    // 从峰值向高温侧找直方图谷/末端, 取三角法远端锚点
    int far_end = bins - 1;
    for (int i = peak + 1; i < bins; ++i) {
        if (hist[i] > hist[peak] / 20) {
            far_end = i;   // 最后一个仍有一定计数的 bin
        }
    }
    if (far_end <= peak) {
        far_end = std::min(peak + 8, bins - 1);
    }
    // 经典三角法: 峰值-远端连线, 找最大垂直距离的 bin
    const int x1 = peak, y1 = hist[peak];
    const int x2 = far_end, y2 = hist[far_end];
    int best = peak;
    double best_d = -1.0;
    for (int i = peak; i <= far_end; ++i) {
        const double d = std::fabs(
            (y2 - y1) * (i - x1) - (x2 - x1) * (hist[i] - y1));
        if (d > best_d) {
            best_d = d;
            best = i;
        }
    }
    const double bin_to_c = (t_hi - t_lo) / (bins - 1);
    bg_mode = t_lo + peak * bin_to_c;
    return t_lo + best * bin_to_c;
}

Saliency::Output Saliency::process(const cv::Mat& T_c, const cv::Mat& G_small)
{
    Output out;
    if (T_c.empty() || G_small.empty() ||
        T_c.size() != G_small.size()) {
        return out;
    }

    const cv::Size ksize(cfg_.bg_kernel, cfg_.bg_kernel);

    // ---- Step 1: TIR 显著性(温度域 FT 算子) ----
    cv::Mat bg_t;
    cv::GaussianBlur(T_c, bg_t, ksize, 0);
    cv::Mat s_tir;
    cv::absdiff(T_c, bg_t, s_tir);            // CV_32F

    // ---- Step 2: RGB 显著性(灰度 FT 算子, IR 网格) ----
    cv::Mat g_f;
    G_small.convertTo(g_f, CV_32F);
    cv::Mat bg_r;
    cv::GaussianBlur(g_f, bg_r, ksize, 0);
    cv::Mat s_rgb;
    cv::absdiff(g_f, bg_r, s_rgb);

    // ---- Step 3: P99 归一化(两路独立, 量纲不混用) ----
    out.sn_tir = normalize_p99(s_tir);
    out.sn_rgb = normalize_p99(s_rgb);

    // ---- 场景统计(直方图分位数, 免全帧排序) ----
    {
        double dmin = 0.0, dmax = 0.0, sum = 0.0;
        for (int r = 0; r < T_c.rows; ++r) {
            const float* row = T_c.ptr<float>(r);
            for (int c = 0; c < T_c.cols; ++c) {
                sum += row[c];
                if (row[c] < dmin) dmin = row[c];
                if (row[c] > dmax) dmax = row[c];
            }
        }
        const float p1 = hist_percentile(T_c, 0.01,
                                         static_cast<float>(dmin),
                                         static_cast<float>(dmax));
        const float p99 = hist_percentile(T_c, 0.99,
                                          static_cast<float>(dmin),
                                          static_cast<float>(dmax));
        out.scene_contrast_k = p99 - p1;
        out.frame_mean_c = sum / T_c.total();
    }

    // ---- Step 4: TIR 阈值(三种模式) ----
    cv::Mat mask_tir(T_c.size(), CV_8U, cv::Scalar(0));
    if (cfg_.thr_mode == "tht") {
        // (b) THT: T > T_bg_mode + delta_min, 物理量纲
        double bg_mode = 0.0;
        const double th = tht_temperature(T_c, bg_mode);
        out.t_bg_mode = bg_mode;
        out.thr_tir_used = th;
        const float cut = static_cast<float>(
            std::max(th, bg_mode + cfg_.delta_min));
        for (int r = 0; r < T_c.rows; ++r) {
            const float* row = T_c.ptr<float>(r);
            uint8_t* m = mask_tir.ptr<uint8_t>(r);
            for (int c = 0; c < T_c.cols; ++c) {
                m[c] = row[c] > cut ? 255 : 0;
            }
        }
    } else if (cfg_.thr_mode == "otsu") {
        cv::Mat u8;
        out.sn_tir.convertTo(u8, CV_8U, 255.0);
        cv::threshold(u8, mask_tir, 0, 255, cv::THRESH_OTSU);
        out.thr_tir_used = -1.0;
    } else {
        // (a) adaptive: sn_tir > mean + k*std
        cv::Scalar mean, dev;
        cv::meanStdDev(out.sn_tir, mean, dev);
        const float cut =
            static_cast<float>(mean[0] + cfg_.thr_k * dev[0]);
        out.thr_tir_used = cut;
        for (int r = 0; r < out.sn_tir.rows; ++r) {
            const float* row = out.sn_tir.ptr<float>(r);
            uint8_t* m = mask_tir.ptr<uint8_t>(r);
            for (int c = 0; c < out.sn_tir.cols; ++c) {
                m[c] = row[c] > cut ? 255 : 0;
            }
        }
    }

    // ---- RGB 阈值: 统一 mean + k*std ----
    cv::Mat mask_rgb(T_c.size(), CV_8U, cv::Scalar(0));
    {
        cv::Scalar mean, dev;
        cv::meanStdDev(out.sn_rgb, mean, dev);
        const float cut =
            static_cast<float>(mean[0] + cfg_.thr_k * dev[0]);
        for (int r = 0; r < out.sn_rgb.rows; ++r) {
            const float* row = out.sn_rgb.ptr<float>(r);
            uint8_t* m = mask_rgb.ptr<uint8_t>(r);
            for (int c = 0; c < out.sn_rgb.cols; ++c) {
                m[c] = row[c] > cut ? 255 : 0;
            }
        }
    }

    // ---- 闭运算(顺序固定: mask → MORPH_CLOSE → 连通域, 方案 §4.4) ----
    cv::Mat kernel3 = cv::getStructuringElement(
        cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(mask_tir, mask_tir, cv::MORPH_CLOSE, kernel3);
    cv::morphologyEx(mask_rgb, mask_rgb, cv::MORPH_CLOSE, kernel3);
    out.mask_tir = mask_tir;
    out.mask_rgb = mask_rgb;

    // ---- 四象限 ----
    cv::Mat mask_q1(T_c.size(), CV_8U, cv::Scalar(0));
    cv::Mat mask_q2(T_c.size(), CV_8U, cv::Scalar(0));
    cv::Mat mask_q3(T_c.size(), CV_8U, cv::Scalar(0));
    int q1 = 0, q2 = 0, q3 = 0;
    for (int r = 0; r < mask_tir.rows; ++r) {
        const uint8_t* mt = mask_tir.ptr<uint8_t>(r);
        const uint8_t* mr = mask_rgb.ptr<uint8_t>(r);
        uint8_t* m1 = mask_q1.ptr<uint8_t>(r);
        uint8_t* m2 = mask_q2.ptr<uint8_t>(r);
        uint8_t* m3 = mask_q3.ptr<uint8_t>(r);
        for (int c = 0; c < mask_tir.cols; ++c) {
            const bool t = mt[c] != 0;
            const bool v = mr[c] != 0;
            if (t && !v) {
                m1[c] = 255;
                ++q1;
            } else if (t && v) {
                m2[c] = 255;
                ++q2;
            } else if (!t && v) {
                m3[c] = 255;
                ++q3;
            }
        }
    }
    // 交集会产生锯齿碎片(q1 > tir_only → FAR 为负, 方案 §14 风险#4),
    // 象限 mask 必须再闭运算一次(5x5: 补 rgb mask 在目标内部咬出的洞)
    cv::Mat kernel5 = cv::getStructuringElement(
        cv::MORPH_RECT, cv::Size(5, 5));
    cv::morphologyEx(mask_q1, mask_q1, cv::MORPH_CLOSE, kernel5);
    out.mask_q1 = mask_q1;
    out.mask_q2 = mask_q2;
    out.mask_q3 = mask_q3;
    out.q1_px = q1;
    out.q2_px = q2;
    out.q3_px = q3;
    out.tir_px = cv::countNonZero(mask_tir);
    out.valid = true;
    return out;
}
