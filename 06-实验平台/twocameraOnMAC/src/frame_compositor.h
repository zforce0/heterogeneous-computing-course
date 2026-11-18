#pragma once

#include "frame.h"
#include "synchronizer.h"

struct PipConfig {
    float width_ratio = 0.32f;
    float maximum_height_ratio = 0.42f;
    int margin = 16;
    int border = 3;
};

/* 最近一次 compose() 计算出的画面布局, 用于在合成帧上叠加
 * 文字/FPS 等信息时定位 visible 主图和红外 pip 子图区域。 */
struct ComposeLayout {
    int visible_x = 0;
    int visible_y = 0;
    int visible_width = 0;
    int visible_height = 0;
    int pip_x = 0;
    int pip_y = 0;
    int pip_width = 0;
    int pip_height = 0;
    bool valid = false;
};

class FrameCompositor {
public:
    explicit FrameCompositor(const PipConfig& config = PipConfig());

    bool compose(const SyncedFrames& input, int output_width,
                 int output_height, BgrFrame& output) const;

    const ComposeLayout& last_layout() const { return last_layout_; }

private:
    static void blit_scaled(const BgrFrame& source, BgrFrame& destination,
                            int x, int y, int width, int height);
    static void draw_border(BgrFrame& frame, int x, int y,
                            int width, int height, int thickness,
                            uint8_t blue, uint8_t green, uint8_t red);

    PipConfig config_;
    mutable ComposeLayout last_layout_;
};
