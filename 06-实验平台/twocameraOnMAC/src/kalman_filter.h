#pragma once

/* 8 维状态 Kalman 滤波器 (SORT/ByteTrack 边界框运动模型,
 * camerayolo.md 3.4.2 Step 2 / kalman_filter.h)
 *
 * 状态 x = [cx, cy, aspect, height, vcx, vcy, vaspect, vheight]
 * 观测 z = [cx, cy, aspect, height]
 * 匀速运动模型 (dt=1), 过程/观测噪声按 box 高度 h 加权
 * (与 boxmot/sort 的 KalmanBoxMotion 参数一致)。 */
class KalmanFilter {
public:
    KalmanFilter();

    /* 用首次观测初始化状态: 位置取 z, 速度置 0 */
    void init(const float measurement[4]);

    /* 预测一步, 返回 8 维预测状态 */
    const float* predict();

    /* 用观测 [cx, cy, aspect, height] 修正, 返回滤波后状态 */
    const float* update(const float measurement[4]);

    const float* state() const { return x_; }

private:
    float x_[8];      /* 状态向量 */
    float p_[8][8];   /* 估计协方差 */
};
