#include "kalman_filter.h"

#include <cmath>

namespace {

/* 噪声权重 (与 SORT/boxmot 一致) */
const float kStdWeightPosition = 1.0e-2f;
const float kStdWeightVelocity = 1.0e-3f;

/* 4x4 矩阵求逆 (高斯-约当消元), 不可逆时返回 false */
bool invert4(const float m[4][4], float inv[4][4])
{
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            inv[i][j] = (i == j) ? 1.0f : 0.0f;
        }
    }
    float a[4][4];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            a[i][j] = m[i][j];
        }
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        float best = std::fabs(a[col][col]);
        for (int r = col + 1; r < 4; ++r) {
            if (std::fabs(a[r][col]) > best) {
                best = std::fabs(a[r][col]);
                pivot = r;
            }
        }
        if (best < 1.0e-9f) {
            return false;
        }
        if (pivot != col) {
            for (int j = 0; j < 4; ++j) {
                float t = a[col][j];
                a[col][j] = a[pivot][j];
                a[pivot][j] = t;
                t = inv[col][j];
                inv[col][j] = inv[pivot][j];
                inv[pivot][j] = t;
            }
        }
        const float diag = a[col][col];
        for (int j = 0; j < 4; ++j) {
            a[col][j] /= diag;
            inv[col][j] /= diag;
        }
        for (int r = 0; r < 4; ++r) {
            if (r == col) {
                continue;
            }
            const float factor = a[r][col];
            if (factor == 0.0f) {
                continue;
            }
            for (int j = 0; j < 4; ++j) {
                a[r][j] -= factor * a[col][j];
                inv[r][j] -= factor * inv[col][j];
            }
        }
    }
    return true;
}

} // namespace

KalmanFilter::KalmanFilter()
{
    for (int i = 0; i < 8; ++i) {
        x_[i] = 0.0f;
    }
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            p_[i][j] = 0.0f;
        }
    }
}

void KalmanFilter::init(const float measurement[4])
{
    for (int i = 0; i < 4; ++i) {
        x_[i] = measurement[i];
        x_[i + 4] = 0.0f; /* 初始速度未知, 置 0 */
    }

    const float h = measurement[3] > 0.0f ? measurement[3] : 1.0f;
    const float diag[8] = {
        2.0f * kStdWeightPosition * h,   /* cx */
        2.0f * kStdWeightPosition * h,   /* cy */
        1.0e-2f,                         /* aspect */
        2.0f * kStdWeightPosition * h,   /* height */
        10.0f * kStdWeightVelocity * h,  /* vcx */
        10.0f * kStdWeightVelocity * h,  /* vcy */
        1.0e-5f,                         /* vaspect */
        10.0f * kStdWeightVelocity * h   /* vheight */
    };
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            p_[i][j] = 0.0f;
        }
        p_[i][i] = diag[i] * diag[i];
    }
}

const float* KalmanFilter::predict()
{
    /* x = F x, F = I + 速度耦合: 位置 += 速度 */
    for (int i = 0; i < 4; ++i) {
        x_[i] += x_[i + 4];
    }

    /* P = F P F^T: 先行加 (F P), 再列加 (* F^T) */
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 8; ++j) {
            p_[i][j] += p_[i + 4][j];
        }
    }
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 8; ++i) {
            p_[i][j] += p_[i][j + 4];
        }
    }

    /* P += Q, 过程噪声按当前 h 加权 */
    const float h = x_[3] > 0.0f ? x_[3] : 1.0f;
    const float pos_var =
        kStdWeightPosition * h * kStdWeightPosition * h;
    const float vel_var =
        kStdWeightVelocity * h * kStdWeightVelocity * h;
    const float q[8] = {
        pos_var, pos_var, 1.0e-2f, pos_var,
        vel_var, vel_var, 1.0e-5f, vel_var
    };
    for (int i = 0; i < 8; ++i) {
        p_[i][i] += q[i];
    }

    return x_;
}

const float* KalmanFilter::update(const float measurement[4])
{
    /* 残差 y = z - H x (H 取状态前 4 维) */
    float y[4];
    for (int i = 0; i < 4; ++i) {
        y[i] = measurement[i] - x_[i];
    }

    /* S = H P H^T + R */
    const float h = x_[3] > 0.0f ? x_[3] : 1.0f;
    const float r_var =
        kStdWeightPosition * h * kStdWeightPosition * h;
    float s[4][4];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            s[i][j] = p_[i][j] + ((i == j) ? r_var : 0.0f);
        }
    }

    /* K = P H^T S^-1 = P[:, :4] * S^-1 */
    float s_inv[4][4];
    if (!invert4(s, s_inv)) {
        return x_; /* S 奇异, 放弃本次修正 */
    }
    float k[8][4];
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 4; ++j) {
            float sum = 0.0f;
            for (int m = 0; m < 4; ++m) {
                sum += p_[i][m] * s_inv[m][j];
            }
            k[i][j] = sum;
        }
    }

    /* x += K y; P = (I - K H) P = P - K * P[:4, :] */
    for (int i = 0; i < 8; ++i) {
        float delta = 0.0f;
        for (int j = 0; j < 4; ++j) {
            delta += k[i][j] * y[j];
        }
        x_[i] += delta;
    }
    float p_new[8][8];
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            float correction = 0.0f;
            for (int m = 0; m < 4; ++m) {
                correction += k[i][m] * p_[m][j];
            }
            p_new[i][j] = p_[i][j] - correction;
        }
    }
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            p_[i][j] = p_new[i][j];
        }
    }

    return x_;
}
