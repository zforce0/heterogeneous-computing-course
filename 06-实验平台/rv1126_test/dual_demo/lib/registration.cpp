// 配准模块: 参考 twocameraOnMAC 的 4 点单应(quad)方案
//   - calibration.txt 的 quad 8 值 = 红外视野在可见光 640x480 中的四边形
//   - homography_4pt/solve_linear_8 搬自 twocameraOnMAC src/image_registrator.cpp
//   - 无标定文件时退化为 FOV 分轴缩放(实现方案 §3.2/§3.2.1)
//   - crop margin 换算在角度域恒定(§3.3): 不写死像素, 随 VIS 分辨率缩放

#include "registration.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

// ---- 以下两个函数源自 twocameraOnMAC src/image_registrator.cpp ----

// 8x8 线性方程组求解(高斯消元 + 部分主元, 带独立右端项数组)
static bool solve_linear_8(float a[8][8], const float b_in[8], float x[8])
{
    float rhs[8];
    for (int i = 0; i < 8; ++i) {
        rhs[i] = b_in[i];
    }
    for (int col = 0; col < 8; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 8; ++r) {
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) {
                pivot = r;
            }
        }
        if (std::fabs(a[pivot][col]) < 1.0e-12f) {
            return false;
        }
        if (pivot != col) {
            for (int c = 0; c < 8; ++c) {
                std::swap(a[col][c], a[pivot][c]);
            }
            std::swap(rhs[col], rhs[pivot]);
        }
        for (int r = col + 1; r < 8; ++r) {
            const float factor = a[r][col] / a[col][col];
            for (int c = col; c < 8; ++c) {
                a[r][c] -= factor * a[col][c];
            }
            rhs[r] -= factor * rhs[col];
        }
    }
    for (int row = 7; row >= 0; --row) {
        float s = rhs[row];
        for (int c = row + 1; c < 8; ++c) {
            s -= a[row][c] * x[c];
        }
        x[row] = s / a[row][row];
    }
    return true;
}

// 4 对点 DLT 单应(src → dst, h33=1)
static bool homography_4pt(const float src[8], const float dst[8],
                           float h[9])
{
    float ata[8][8];
    float atb[8];
    for (int i = 0; i < 8; ++i) {
        atb[i] = 0.0f;
        for (int j = 0; j < 8; ++j) {
            ata[i][j] = 0.0f;
        }
    }
    for (int p = 0; p < 4; ++p) {
        const float u = src[p * 2], v = src[p * 2 + 1];
        const float x = dst[p * 2], y = dst[p * 2 + 1];
        const float rows[2][8] = {
            { u, v, 1.0f, 0.0f, 0.0f, 0.0f, -x * u, -x * v },
            { 0.0f, 0.0f, 0.0f, u, v, 1.0f, -y * u, -y * v }
        };
        const float rhs[2] = { x, y };
        for (int r = 0; r < 2; ++r) {
            for (int i = 0; i < 8; ++i) {
                atb[i] += rows[r][i] * rhs[r];
                for (int j = 0; j < 8; ++j) {
                    ata[i][j] += rows[r][i] * rows[r][j];
                }
            }
        }
    }
    if (!solve_linear_8(ata, atb, h)) {
        return false;
    }
    h[8] = 1.0f;
    return true;
}

// ---- calibration.txt 加载(源自 twocameraOnMAC sample.cpp) ----
// 格式: "quad x0 y0 x1 y1 x2 y2 x3 y3"(顶点顺序 左上/右上/右下/左下,
// 坐标基于 640x480 可见光帧)
static bool load_calibration_quad(const char* path, float quad[8])
{
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    char line[512];
    bool ok = false;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        float v[8];
        if (std::sscanf(line,
                        " quad %f %f %f %f %f %f %f %f",
                        &v[0], &v[1], &v[2], &v[3],
                        &v[4], &v[5], &v[6], &v[7]) == 8) {
            for (int i = 0; i < 8; ++i) {
                quad[i] = v[i];
            }
            ok = true;
            break;
        }
    }
    std::fclose(file);
    return ok;
}

// ---- FOV 兜底: 对角 FOV 拆分轴(实现方案 §3.2.1) ----
static void diag_to_axis(double dfov, double aspect, double& fov_x,
                         double& fov_y)
{
    const double tan_d = std::tan(dfov * CV_PI / 360.0);
    const double tan_y = tan_d / std::sqrt(1.0 + aspect * aspect);
    const double tan_x = tan_y * aspect;
    fov_y = 2.0 * std::atan(tan_y) * 180.0 / CV_PI;
    fov_x = 2.0 * std::atan(tan_x) * 180.0 / CV_PI;
}

Registration::Registration()
    : use_quad_(false),
      scale_x_(1.0),
      scale_y_(1.0),
      ir_w_(0),
      ir_h_(0),
      vis_w_(0),
      vis_h_(0),
      kx_(1.0),
      ky_(1.0)
{
    for (int i = 0; i < 9; ++i) {
        h_[i] = 0.0f;
    }
    h_[0] = h_[4] = h_[8] = 1.0f;   // 默认恒等
}

bool Registration::init(const RegistrationConfig& cfg, int ir_w, int ir_h,
                        int vis_w, int vis_h)
{
    cfg_ = cfg;
    ir_w_ = ir_w;
    ir_h_ = ir_h;
    vis_w_ = vis_w;
    vis_h_ = vis_h;
    use_quad_ = false;

    // quad 基于 640x480 基准, 按实际 VIS 分辨率等比换算
    scale_x_ = vis_w / 640.0;
    scale_y_ = vis_h / 480.0;

    if (!cfg_.calib_path.empty()) {
        float quad[8];
        if (load_calibration_quad(cfg_.calib_path.c_str(), quad)) {
            // 归一化输出四角(0,0)(1,0)(1,1)(0,1) → quad 顶点
            const float src[8] = { 0.0f, 0.0f, 1.0f, 0.0f,
                                   1.0f, 1.0f, 0.0f, 1.0f };
            if (homography_4pt(src, quad, h_)) {
                use_quad_ = true;
                std::printf(
                    "[INIT] registration: quad calibration loaded from %s\n"
                    "[INIT]   quad=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)"
                    " @640x480 -> scale %.2fx%.2f\n",
                    cfg_.calib_path.c_str(),
                    quad[0], quad[1], quad[2], quad[3],
                    quad[4], quad[5], quad[6], quad[7],
                    scale_x_, scale_y_);
            }
        }
    }

    if (!use_quad_) {
        // FOV 兜底(§3.2): 分轴换算后取各向异性缩放
        double ir_fx, ir_fy, vis_fx, vis_fy;
        diag_to_axis(cfg_.ir_fov_diag,
                     static_cast<double>(ir_w) / ir_h, ir_fx, ir_fy);
        diag_to_axis(cfg_.vis_fov_diag,
                     static_cast<double>(vis_w) / vis_h, vis_fx, vis_fy);
        kx_ = std::tan(vis_fx / 2.0 * CV_PI / 180.0) /
              std::tan(ir_fx / 2.0 * CV_PI / 180.0);
        ky_ = std::tan(vis_fy / 2.0 * CV_PI / 180.0) /
              std::tan(ir_fy / 2.0 * CV_PI / 180.0);
        std::printf(
            "[INIT] registration: FOV fallback (no calibration), "
            "ir_fov=%.1f vis_fov=%.1f -> fov_x=%.1f/%.1f fov_y=%.1f/%.1f "
            "kx=%.3f ky=%.3f\n",
            cfg_.ir_fov_diag, cfg_.vis_fov_diag,
            ir_fx, vis_fx, ir_fy, vis_fy, kx_, ky_);
    }
    return true;
}

cv::Point2f Registration::ir_to_vis(float u_ir, float v_ir) const
{
    if (use_quad_) {
        const float un = u_ir / ir_w_;
        const float vn = v_ir / ir_h_;
        const float d = h_[6] * un + h_[7] * vn + h_[8];
        if (std::fabs(d) < 1.0e-12f) {
            return cv::Point2f(-1.0f, -1.0f);
        }
        return cv::Point2f(
            (h_[0] * un + h_[1] * vn + h_[2]) / d * scale_x_,
            (h_[3] * un + h_[4] * vn + h_[5]) / d * scale_y_);
    }
    // FOV 兜底(§3.2)
    return cv::Point2f(
        static_cast<float>(
            vis_w_ / 2.0 +
            (u_ir - ir_w_ / 2.0) * (vis_w_ / ir_w_) / kx_),
        static_cast<float>(
            vis_h_ / 2.0 +
            (v_ir - ir_h_ / 2.0) * (vis_h_ / ir_h_) / ky_));
}

cv::Point2f Registration::box_center_to_vis(const cv::Rect& box_ir) const
{
    return ir_to_vis(box_ir.x + box_ir.width * 0.5f,
                     box_ir.y + box_ir.height * 0.5f);
}

cv::Rect Registration::box_ir_to_roi_vis(const cv::Rect& box_ir) const
{
    // §3.3: crop 尺寸在角度域恒定 — 随 VIS 分辨率换算, 不写死像素
    const int side_ir = std::max(box_ir.width, box_ir.height);
    const double scale = static_cast<double>(vis_w_) / ir_w_;
    int ctx = static_cast<int>(
        std::ceil(side_ir * cfg_.margin_ratio * scale));
    ctx = std::max(ctx, static_cast<int>(cfg_.min_crop_ir * scale));
    ctx = std::max(ctx, cfg_.min_crop_px);
    ctx = std::min(ctx, cfg_.max_crop_px);

    const cv::Point2f c = box_center_to_vis(box_ir);
    cv::Point tl(static_cast<int>(c.x - ctx / 2.0),
                 static_cast<int>(c.y - ctx / 2.0));
    // 夹紧到图像内
    tl.x = std::max(0, std::min(tl.x, vis_w_ - ctx));
    tl.y = std::max(0, std::min(tl.y, vis_h_ - ctx));
    if (ctx > vis_w_) {
        ctx = vis_w_;
        tl.x = 0;
    }
    if (ctx > vis_h_) {
        ctx = vis_h_;
        tl.y = 0;
    }
    return cv::Rect(tl.x, tl.y, ctx, ctx);
}

void Registration::ir_field_quad_vis(cv::Point2f quad[4]) const
{
    quad[0] = ir_to_vis(0.0f, 0.0f);
    quad[1] = ir_to_vis(static_cast<float>(ir_w_), 0.0f);
    quad[2] = ir_to_vis(static_cast<float>(ir_w_),
                        static_cast<float>(ir_h_));
    quad[3] = ir_to_vis(0.0f, static_cast<float>(ir_h_));
}
