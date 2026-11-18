#include "image_registrator.h"
#include "image_resize.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

/* 8x8 线性方程组求解 (高斯消元 + 部分主元) */
bool solve_linear_8(float a[8][8], const float b[8], float x[8])
{
    float m[8][9];
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            m[i][j] = a[i][j];
        }
        m[i][8] = b[i];
    }
    for (int col = 0; col < 8; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 8; ++r) {
            if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) {
                pivot = r;
            }
        }
        if (std::fabs(m[pivot][col]) < 1.0e-12f) {
            return false;
        }
        if (pivot != col) {
            for (int j = 0; j < 9; ++j) {
                float t = m[col][j];
                m[col][j] = m[pivot][j];
                m[pivot][j] = t;
            }
        }
        const float d = m[col][col];
        for (int j = col; j < 9; ++j) {
            m[col][j] /= d;
        }
        for (int r = 0; r < 8; ++r) {
            if (r != col && m[r][col] != 0.0f) {
                const float f = m[r][col];
                for (int j = col; j < 9; ++j) {
                    m[r][j] -= f * m[col][j];
                }
            }
        }
    }
    for (int i = 0; i < 8; ++i) {
        x[i] = m[i][8];
    }
    return true;
}

/* 4 对点 DLT 单应 (与 calib_web.py 同算法):
 * src/dst 各 8 值 (4 个 (x,y)), 求解 src→dst 的 3x3 单应 (h33=1) */
bool homography_4pt(const float src[8], const float dst[8], float h[9])
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
        const float u = src[2 * p];
        const float v = src[2 * p + 1];
        const float x = dst[2 * p];
        const float y = dst[2 * p + 1];
        const float rows[2][9] = {
            {u, v, 1.0f, 0.0f, 0.0f, 0.0f, -x * u, -x * v, x},
            {0.0f, 0.0f, 0.0f, u, v, 1.0f, -y * u, -y * v, y}
        };
        for (int r = 0; r < 2; ++r) {
            for (int i = 0; i < 8; ++i) {
                for (int j = 0; j < 8; ++j) {
                    ata[i][j] += rows[r][i] * rows[r][j];
                }
                atb[i] += rows[r][i] * rows[r][8];
            }
        }
    }
    float hh[8];
    if (!solve_linear_8(ata, atb, hh)) {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        h[i] = hh[i];
    }
    h[8] = 1.0f;
    return true;
}

} // namespace

ImageRegistrator::ImageRegistrator(const RegistrationConfig& config)
    : config_(config), quad_h_valid_(false)
{
    if (config_.use_quad) {
        /* 归一化输出四角 (0,0)(1,0)(1,1)(0,1) → quad 顶点 */
        const float src[8] = {0.0f, 0.0f, 1.0f, 0.0f,
                              1.0f, 1.0f, 0.0f, 1.0f};
        quad_h_valid_ = homography_4pt(src, config_.quad, quad_h_);
    }
}

bool ImageRegistrator::register_frames(
    const SyncedFrames& input, RegisteredFrames& output)
{
    if (!input.infrared.valid() || !input.visible.valid()) {
        output.valid = false;
        return false;
    }

    /* IR 帧无需空间变换, 直接传递 */
    output.infrared = input.infrared;

    const int ir_w = input.infrared.width;
    const int ir_h = input.infrared.height;
    const BgrFrame& visible = input.visible;

    if (config_.use_quad && quad_h_valid_) {
        /* 四边形透视截取: 输出每像素经单应映射到可见光采样,
         * 越界区域保持黑色 (不做边缘延伸) */
        registered_visible_.resize(ir_w, ir_h);
        std::memset(registered_visible_.data.data(), 0,
                    registered_visible_.data.size());

        /* quad 基于 640x480, 按实际采集分辨率等比换算 */
        const float scale_x =
            static_cast<float>(visible.width) / 640.0f;
        const float scale_y =
            static_cast<float>(visible.height) / 480.0f;
        const float h00 = quad_h_[0], h01 = quad_h_[1];
        const float h02 = quad_h_[2], h10 = quad_h_[3];
        const float h11 = quad_h_[4], h12 = quad_h_[5];
        const float h20 = quad_h_[6], h21 = quad_h_[7];
        const float h22 = quad_h_[8];

        for (int v = 0; v < ir_h; ++v) {
            const float vn = static_cast<float>(v) / ir_h;
            uint8_t* dst_row = registered_visible_.data.data() +
                static_cast<size_t>(v) * registered_visible_.stride;
            for (int u = 0; u < ir_w; ++u) {
                const float un = static_cast<float>(u) / ir_w;
                const float d = h20 * un + h21 * vn + h22;
                if (std::fabs(d) < 1.0e-12f) {
                    continue;
                }
                /* 角点约定 (与 calib_web.py / cv2.getPerspectiveTransform
                 * 一致): 输出 (0,0) ↔ q0, (ir_w,ir_h) ↔ q2 */
                const float x =
                    (h00 * un + h01 * vn + h02) / d * scale_x;
                const float y =
                    (h10 * un + h11 * vn + h12) / d * scale_y;

                /* 双线性采样 (越界跳过, 保持黑色) */
                const int x0 = static_cast<int>(std::floor(x));
                const int y0 = static_cast<int>(std::floor(y));
                if (x0 < 0 || y0 < 0 || x0 >= visible.width - 1 ||
                    y0 >= visible.height - 1) {
                    continue;
                }
                const float fx = x - x0;
                const float fy = y - y0;
                const uint8_t* p00 =
                    visible.data.data() +
                    static_cast<size_t>(y0) * visible.stride +
                    static_cast<size_t>(x0) * 3;
                const uint8_t* p10 = p00 + 3;
                const uint8_t* p01 = p00 + visible.stride;
                const uint8_t* p11 = p01 + 3;
                uint8_t* dst = dst_row +
                    static_cast<size_t>(u) * 3;
                for (int c = 0; c < 3; ++c) {
                    const float value =
                        p00[c] * (1.0f - fx) * (1.0f - fy) +
                        p10[c] * fx * (1.0f - fy) +
                        p01[c] * (1.0f - fx) * fy +
                        p11[c] * fx * fy;
                    dst[c] = static_cast<uint8_t>(value + 0.5f);
                }
            }
        }
        output.visible = registered_visible_;
        output.timestamp_delta_us = input.timestamp_delta_us;
        output.valid = true;
        return true;
    }

    /* 轴对齐 crop+resize (兼容模式) */
    int crop_x = config_.crop_x;
    int crop_y = config_.crop_y;
    int crop_w = config_.crop_width > 0
                     ? config_.crop_width
                     : visible.width;
    int crop_h = config_.crop_height > 0
                     ? config_.crop_height
                     : visible.height;

    crop_w = std::min(crop_w, visible.width);
    crop_h = std::min(crop_h, visible.height);
    if (crop_w <= 0 || crop_h <= 0) {
        output.valid = false;
        return false;
    }
    crop_x = std::max(0,
        std::min(crop_x, visible.width - crop_w));
    crop_y = std::max(0,
        std::min(crop_y, visible.height - crop_h));

    registered_visible_.resize(ir_w, ir_h);
    resize_bilinear_bgr(visible, crop_x, crop_y, crop_w, crop_h,
                        registered_visible_, 0, 0, ir_w, ir_h);
    output.visible = registered_visible_;

    output.timestamp_delta_us = input.timestamp_delta_us;
    output.valid = true;
    return true;
}
