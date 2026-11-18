#pragma once

#include "frame.h"

/* 红外灰度区域 → JET 伪彩 (camerayolo.md 4.2.1 / 4.2.5.1)
 * 256 级预计算 LUT, 纯 C++ 实现, 不依赖 OpenCV。
 *
 * blend = false: 完整覆盖 (IR 主画面, 热力图效果)
 * blend = true : 半透明叠加 (可见光副画面, alpha blending)
 *
 * 多框重叠去重: claimed 为"整帧尺寸"的已着色标记位图
 * (claimed[y*claim_stride + x], 按帧绝对坐标索引):
 *   - 非零像素表示已着色过, 跳过 (避免重叠框二次映射失真)
 *   - 本像素完成着色后置 1
 *   - nullptr (默认) = 不去重, 保持旧行为
 * IR / 可见光两个子画面各自独立去重, 每个子画面开始前
 * 由调用方清零位图。 */
void apply_pseudo_color_in_rect(BgrFrame& frame,
                                int x, int y,
                                int width, int height,
                                bool blend = false,
                                float alpha = 0.5f,
                                uint8_t* claimed = nullptr,
                                int claim_stride = 0);

/* 跨帧伪彩叠加: 取 source 帧指定 ROI 的伪彩色 (灰度→JET),
 * 以 alpha 混合到 destination 帧的指定 ROI (两 ROI 可不同尺寸,
 * 内部最近邻采样)。用于把红外帧的热力伪彩叠加到可见光画面。
 * claimed 含义同上 (针对 destination 帧, 按帧绝对坐标索引)。 */
void blend_pseudo_color_from(const BgrFrame& source,
                             int src_x, int src_y,
                             int src_w, int src_h,
                             BgrFrame& destination,
                             int dst_x, int dst_y,
                             int dst_w, int dst_h,
                             float alpha,
                             uint8_t* claimed = nullptr,
                             int claim_stride = 0);
