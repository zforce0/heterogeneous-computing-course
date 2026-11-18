#pragma once

#include "frame.h"
#include "image_registrator.h"

/* 并排合成配置 (camerayolo.md 3.3):
 * IR 主画面与可见光副画面统一高度、1:1 等大,
 * 各自保持原始宽高比 (不横向压缩), 横向拼接 + 中间分割线。 */
struct SideBySideConfig {
    int gap = 4;             /* 中间分割线宽度 (像素) */
    uint8_t gap_color[3] = {128, 128, 128}; /* 分割线颜色 BGR */
};

/* 最近一次 compose() 计算出的画面布局, 供跟踪框/FPS 叠加定位
 * IR 主画面与可见光副画面区域。 */
struct SideBySideLayout {
    int ir_x = 0;
    int ir_y = 0;
    int ir_width = 0;
    int ir_height = 0;
    int vis_x = 0;
    int vis_y = 0;
    int vis_width = 0;
    int vis_height = 0;
    bool valid = false;
};

class SideBySideCompositor {
public:
    explicit SideBySideCompositor(
        const SideBySideConfig& config = SideBySideConfig());

    /* 并排合成: 以 output_height 为基准, 两个子画面统一高度、
     * 各自按原始宽高比计算宽度 (无纵横比失真), 横向拼接。
     * 输出宽度 = ir_w + gap + vis_w; 若超过 output_width 则
     * 整体等比缩小以适配 (FBDEV 固定屏幕场景)。 */
    bool compose(const RegisteredFrames& input, int output_width,
                 int output_height, BgrFrame& output) const;

    const SideBySideLayout& last_layout() const
    {
        return last_layout_;
    }

private:
    SideBySideConfig config_;
    mutable SideBySideLayout last_layout_;
};
