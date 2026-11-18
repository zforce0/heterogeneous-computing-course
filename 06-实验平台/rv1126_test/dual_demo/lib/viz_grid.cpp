// 六宫格可视化(实现方案 §8.2):
//   1 VIS+提名框  2 IR 温度 JET  3 S_rgb  4 S_tir  5 差分图  6 四象限着色
// 注意 OpenCV imwrite 为 BGR: 红=Vec3b(0,0,255), 蓝=Vec3b(255,0,0)

#include "viz_grid.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>

VizGrid::VizGrid()
{
}

cv::Mat VizGrid::compose_diff_map(const cv::Mat& sn_tir,
                                  const cv::Mat& sn_rgb)
{
    // 红 = 热更显著(sn_tir>sn_rgb), 蓝 = 视觉更显著
    cv::Mat out(sn_tir.size(), CV_8UC3, cv::Scalar(0, 0, 0));
    for (int r = 0; r < sn_tir.rows; ++r) {
        const float* pt = sn_tir.ptr<float>(r);
        const float* pr = sn_rgb.ptr<float>(r);
        cv::Vec3b* d = out.ptr<cv::Vec3b>(r);
        for (int c = 0; c < sn_tir.cols; ++c) {
            const float diff = pt[c] - pr[c];
            if (diff > 0.0f) {
                d[c][2] = static_cast<uint8_t>(
                    std::min(255.0f, diff * 255.0f));   // 红
            } else {
                d[c][0] = static_cast<uint8_t>(
                    std::min(255.0f, -diff * 255.0f));  // 蓝
            }
        }
    }
    return out;
}

cv::Mat VizGrid::compose_quadrant_map(const cv::Mat& mask_q1,
                                      const cv::Mat& mask_q2,
                                      const cv::Mat& mask_q3)
{
    // 红=① 蓝=② 黄=③ 灰=④(BGR: 黄=BGR(0,255,255))
    cv::Mat out(mask_q1.size(), CV_8UC3,
                cv::Scalar(60, 60, 60));   // ④ 背景(暗灰)
    for (int r = 0; r < mask_q1.rows; ++r) {
        const uint8_t* m1 = mask_q1.ptr<uint8_t>(r);
        const uint8_t* m2 = mask_q2.ptr<uint8_t>(r);
        const uint8_t* m3 = mask_q3.ptr<uint8_t>(r);
        cv::Vec3b* d = out.ptr<cv::Vec3b>(r);
        for (int c = 0; c < mask_q1.cols; ++c) {
            if (m1[c]) {
                d[c] = cv::Vec3b(0, 0, 255);       // ① 红
            } else if (m2[c]) {
                d[c] = cv::Vec3b(255, 0, 0);       // ② 蓝
            } else if (m3[c]) {
                d[c] = cv::Vec3b(0, 255, 255);     // ③ 黄
            }
        }
    }
    return out;
}

// 把 src 缩放到 cell 尺寸(保持填满, 不保比例 — 各格来源宽高比一致或可接受)
static cv::Mat fit_cell(const cv::Mat& src, int cell_w, int cell_h)
{
    cv::Mat out;
    if (src.empty()) {
        out = cv::Mat(cell_h, cell_w, CV_8UC3, cv::Scalar(0, 0, 0));
        return out;
    }
    cv::resize(src, out, cv::Size(cell_w, cell_h), 0, 0, cv::INTER_AREA);
    return out;
}

cv::Mat VizGrid::compose(const Input& in, const Registration& reg,
                         int cell_w) const
{
    // IR 是 4:3(256x192), cell 也按 4:3
    const int cell_h = cell_w * 3 / 4;
    const int gap = 4;

    // 格 1: VIS 原图 + 提名框 + Q 标签 + ΔT
    cv::Mat vis = in.vis_bgr.clone();
    const double sx = static_cast<double>(cell_w) / vis.cols;
    const double sy = static_cast<double>(cell_h) / vis.rows;
    for (size_t i = 0; i < in.noms.size(); ++i) {
        const Nomination& n = in.noms[i];
        cv::Rect rv = n.roi_vis &
                      cv::Rect(0, 0, vis.cols, vis.rows);
        cv::rectangle(vis, rv,
                      n.quadrant == 1 ? cv::Scalar(0, 0, 255)
                                      : cv::Scalar(255, 0, 0),
                      2);
        char tag[96];
        std::snprintf(tag, sizeof(tag), "Q%d dT=%.1fK %s", n.quadrant,
                      n.delta_t, n.semantic.label.c_str());
        cv::putText(vis, tag, cv::Point(rv.x, std::max(14, rv.y - 4)),
                    cv::FONT_HERSHEY_SIMPLEX, 0.45,
                    cv::Scalar(0, 255, 255), 1, cv::LINE_AA);
    }
    // IR 有效视场提示框(§3.2.2: 交集外永远不会有提名, 防客户误解)
    {
        cv::Point2f quad[4];
        reg.ir_field_quad_vis(quad);
        for (int i = 0; i < 4; ++i) {
            const cv::Point p0(static_cast<int>(quad[i].x),
                               static_cast<int>(quad[i].y));
            const cv::Point p1(
                static_cast<int>(quad[(i + 1) % 4].x),
                static_cast<int>(quad[(i + 1) % 4].y));
            cv::line(vis, p0, p1, cv::Scalar(0, 200, 0), 1, cv::LINE_AA);
        }
    }

    // 格 2: IR 温度 JET 伪彩 + 提名框(把 VIS roi 映射回 IR 就是 box_ir)
    cv::Mat ir_jet;
    {
        cv::Mat u8;
        in.T_c.convertTo(u8, CV_8U, 255.0 / 40.0, -255.0 / 40.0 * 10.0);
        cv::applyColorMap(u8, ir_jet, cv::COLORMAP_JET);
        for (size_t i = 0; i < in.noms.size(); ++i) {
            cv::rectangle(ir_jet, in.noms[i].box_ir,
                          cv::Scalar(0, 255, 255), 1);
        }
    }

    // 格 3/4: 显著图(灰度)
    cv::Mat s_rgb_u8, s_tir_u8;
    in.sn_rgb.convertTo(s_rgb_u8, CV_8U, 255.0);
    in.sn_tir.convertTo(s_tir_u8, CV_8U, 255.0);
    cv::cvtColor(s_rgb_u8, s_rgb_u8, cv::COLOR_GRAY2BGR);
    cv::cvtColor(s_tir_u8, s_tir_u8, cv::COLOR_GRAY2BGR);

    // 格 5/6
    cv::Mat diff_map = compose_diff_map(in.sn_tir, in.sn_rgb);
    cv::Mat quad_map =
        (in.mask_q1.empty() || in.mask_q2.empty() || in.mask_q3.empty())
            ? cv::Mat()
            : compose_quadrant_map(in.mask_q1, in.mask_q2, in.mask_q3);

    // 拼接 2 行 x 3 列
    cv::Mat canvas(cell_h * 2 + gap, cell_w * 3 + gap * 2, CV_8UC3,
                   cv::Scalar(30, 30, 30));
    cv::Mat c1 = fit_cell(vis, cell_w, cell_h);
    cv::Mat c2 = fit_cell(ir_jet, cell_w, cell_h);
    cv::Mat c3 = fit_cell(s_rgb_u8, cell_w, cell_h);
    cv::Mat c4 = fit_cell(s_tir_u8, cell_w, cell_h);
    cv::Mat c5 = fit_cell(diff_map, cell_w, cell_h);
    cv::Mat c6 = quad_map.empty()
                     ? cv::Mat(cell_h, cell_w, CV_8UC3, cv::Scalar(0, 0, 0))
                     : fit_cell(quad_map, cell_w, cell_h);
    c1.copyTo(canvas(cv::Rect(0, 0, cell_w, cell_h)));
    c2.copyTo(canvas(
        cv::Rect(cell_w + gap, 0, cell_w, cell_h)));
    c3.copyTo(canvas(
        cv::Rect((cell_w + gap) * 2, 0, cell_w, cell_h)));
    c4.copyTo(canvas(cv::Rect(0, cell_h + gap, cell_w, cell_h)));
    c5.copyTo(canvas(
        cv::Rect(cell_w + gap, cell_h + gap, cell_w, cell_h)));
    c6.copyTo(canvas(
        cv::Rect((cell_w + gap) * 2, cell_h + gap, cell_w, cell_h)));
    return canvas;
}
