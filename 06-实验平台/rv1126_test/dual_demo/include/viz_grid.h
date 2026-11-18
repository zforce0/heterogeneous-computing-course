#pragma once

#include "nomination.h"
#include "registration.h"

#include <opencv2/core.hpp>
#include <string>
#include <vector>

// ============================================================================
// 六宫格可视化: 2 行 x 3 列
//   1 VIS 原图+提名框+Q标签+ΔT   2 IR 温度 JET 伪彩+提名框
//   3 S_rgb 显著图(灰度)         4 S_tir 显著图(灰度)
//   5 差分图(红=热显著/蓝=视觉显著) 6 四象限着色(红①/蓝②/黄③/灰④)
// 参见实现方案 §8.2
// 注意: imwrite 为 BGR 顺序, 红=第2通道蓝=第0通道, 不要与 RGB 混用
// ============================================================================

class VizGrid {
public:
    struct Input {
        cv::Mat vis_bgr;    // VIS 原分辨率 BGR
        cv::Mat ir_bgr;     // IR 256x192 BGR(原始灰度成像)
        cv::Mat T_c;        // 温度矩阵 CV_32F ℃
        cv::Mat sn_tir;     // 归一化 TIR 显著 CV_32F
        cv::Mat sn_rgb;     // 归一化 RGB 显著 CV_32F
        cv::Mat mask_q1, mask_q2, mask_q3;   // 四象限 mask CV_8U
        std::vector<Nomination> noms;   // 本帧提名(semantic 已填则显示标签)
    };

    VizGrid();

    // 差分图(红=热更显著, 蓝=视觉更显著) BGR 输出
    static cv::Mat compose_diff_map(const cv::Mat& sn_tir,
                                    const cv::Mat& sn_rgb);
    // 四象限着色(红①/蓝②/黄③/灰④) BGR 输出
    static cv::Mat compose_quadrant_map(const cv::Mat& mask_q1,
                                        const cv::Mat& mask_q2,
                                        const cv::Mat& mask_q3);

    // 合成六宫格(约 1440x480); cell_w 默认 480
    cv::Mat compose(const Input& in, const Registration& reg,
                    int cell_w = 480) const;
};
