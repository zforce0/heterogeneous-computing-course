#pragma once

#include <opencv2/core.hpp>
#include <string>

// ============================================================================
// 配准(IR ↔ VIS): 参考 twocameraOnMAC 的 4 点单应(quad)方案
//
// 两相机平行固定。标定产物为"红外视野在可见光 640x480 画面中的四边形"
// (calibration.txt 的 quad 8 值), 经 DLT 求得 IR 归一化坐标 → VIS 像素
// 坐标的单应;其他 VIS 分辨率按 640x480 基准等比换算。
//
// 无标定文件时退化为以主点为中心的 FOV 各向同性/分轴缩放(实现方案 §3.2),
// crop 的 3 倍 margin 可吸收系统性偏移,不影响 FAR 指标。
// ============================================================================

struct RegistrationConfig {
    std::string calib_path;      // calibration.txt 路径;空串 = 未标定(FOV 兜底)
    double ir_fov_diag = 57.0;   // 红外对角 FOV(度), 无标定时的兜底参数
    double vis_fov_diag = 80.0;  // 可见光对角 FOV(度)
    // crop margin 策略(实现方案 §3.3)
    double margin_ratio = 3.0;   // 上下文扩展倍数
    int min_crop_ir = 20;        // 最小 crop 边长(IR 像素)
    int min_crop_px = 96;        // 绝对下限(VIS 像素)
    int max_crop_px = 1024;      // 上限(VIS 像素), 防超大 crop 拖慢推理
};

class Registration {
public:
    Registration();

    // 加载标定(或初始化 FOV 兜底), 必须在相机启动后、处理帧前调用
    bool init(const RegistrationConfig& cfg, int ir_w, int ir_h,
              int vis_w, int vis_h);

    // IR 像素坐标 → VIS 像素坐标
    cv::Point2f ir_to_vis(float u_ir, float v_ir) const;

    // IR bbox → VIS crop 矩形(margin 扩展 + 角度域恒定换算 + 上下限截断)
    cv::Rect box_ir_to_roi_vis(const cv::Rect& box_ir) const;

    // IR bbox 中心在 VIS 上的投影(画框/关联用)
    cv::Point2f box_center_to_vis(const cv::Rect& box_ir) const;

    bool calibrated() const { return use_quad_; }
    const RegistrationConfig& config() const { return cfg_; }

    // IR 有效视场映射到 VIS 画面上的四边形(画提示框用, 防客户误解边缘无检测)
    // 返回 4 个 VIS 像素顶点: 左上/右上/右下/左下
    void ir_field_quad_vis(cv::Point2f quad[4]) const;

private:
    RegistrationConfig cfg_;
    bool use_quad_;              // true = quad 单应; false = FOV 缩放兜底
    float h_[9];                 // 单应(归一化 IR 坐标 → 640x480 基准 VIS)
    double scale_x_;             // 640x480 → 实际 VIS 宽换算
    double scale_y_;
    int ir_w_, ir_h_, vis_w_, vis_h_;
    double kx_, ky_;             // FOV 兜底分轴缩放系数
};
