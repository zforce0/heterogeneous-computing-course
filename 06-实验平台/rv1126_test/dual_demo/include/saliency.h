#pragma once

#include <opencv2/core.hpp>
#include <string>

// ============================================================================
// 差分显著层: TIR 显著性(温度域 FT+THT) + VIS 显著性(灰度 FT)
//             + P99 归一化 + 四象限判定 + 闭运算
// 参见实现方案 §4.2 ~ §4.4
//
// 算子纪律:
//   - FT(偏离大核背景)而非 LC(局部对比) — 大块均温区域内部也点亮
//   - 后处理顺序固定: mask → MORPH_CLOSE(3x3) 由调用方(nominator)执行
//   - 两路独立归一化(分母 P99), 量纲不混用
// ============================================================================

struct SaliencyConfig {
    std::string thr_mode = "tht";  // tht(默认,物理量纲) | adaptive | otsu
    double thr_k = 1.0;            // adaptive 模式 k
    double delta_min = 3.0;        // tht 模式最小温升 K
    int bg_kernel = 15;            // 背景高斯核(奇数)
};

class Saliency {
public:
    struct Output {
        cv::Mat sn_tir;            // 归一化 TIR 显著图 CV_32F [0,1] (IR 网格)
        cv::Mat sn_rgb;            // 归一化 RGB 显著图 CV_32F [0,1] (IR 网格)
        cv::Mat mask_tir;          // 热显著 mask CV_8U (255=显著), 已闭运算
        cv::Mat mask_rgb;          // 视觉显著 mask CV_8U, 已闭运算
        cv::Mat mask_q1;           // 热显著&视觉平淡
        cv::Mat mask_q2;           // 一致性区
        cv::Mat mask_q3;           // 热不可知区(视觉显著&热平淡)
        int q1_px = 0, q2_px = 0, q3_px = 0;   // 象限像素计数
        int tir_px = 0;                        // 热显著 mask 像素(FAR 像素口径分母)
        double scene_contrast_k = 0.0;         // 场景热对比度 P99-P1 (K)
        double t_bg_mode = 0.0;                // 背景峰值温度(℃, tht 模式)
        double thr_tir_used = 0.0;             // 实际使用的 TIR 阈值
        double frame_mean_c = 0.0;             // 整帧温度均值(NUC 冻结检测辅助)
        bool valid = false;
    };

    void init(const SaliencyConfig& cfg, int ir_w, int ir_h);

    // T_c: 温度矩阵 CV_32F (ir_w x ir_h, ℃)
    // G_small: VIS 灰度降采样 CV_8U (ir_w x ir_h)
    Output process(const cv::Mat& T_c, const cv::Mat& G_small);

    // 温度直方图三角法(THT): 求背景峰值温度与分割阈值
    // 返回阈值(℃), bg_mode 输出背景峰值温度(℃)
    static double tht_temperature(const cv::Mat& T_c, double& bg_mode);

    // P99 归一化: S / max(P99, eps), 再截断到 [0,1]
    static cv::Mat normalize_p99(const cv::Mat& S);

    const SaliencyConfig& config() const { return cfg_; }

private:
    SaliencyConfig cfg_;
    int ir_w_ = 0, ir_h_ = 0;
};
