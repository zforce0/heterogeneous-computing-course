#include "side_by_side_compositor.h"
#include "image_resize.h"

#include <algorithm>
#include <cstring>

SideBySideCompositor::SideBySideCompositor(
    const SideBySideConfig& config)
    : config_(config)
{
}

bool SideBySideCompositor::compose(const RegisteredFrames& input,
                                   int output_width, int output_height,
                                   BgrFrame& output) const
{
    if (!input.valid || !input.infrared.valid() ||
        !input.visible.valid() || output_width <= 0 ||
        output_height <= 0) {
        last_layout_.valid = false;
        return false;
    }

    /* 统一高度 = output_height, 各子画面按原始宽高比计算宽度
     * (双摄均为 4:3 → 两画面 1:1 等大, 无横向压缩) */
    const float ir_aspect =
        static_cast<float>(input.infrared.width) /
        input.infrared.height;
    const float vis_aspect =
        static_cast<float>(input.visible.width) /
        input.visible.height;

    int content_height = output_height;
    int ir_width = static_cast<int>(
        content_height * ir_aspect + 0.5f);
    int vis_width = static_cast<int>(
        content_height * vis_aspect + 0.5f);

    /* 内容总宽超过可用宽度时整体等比缩小 (保持各画面宽高比) */
    const int usable_width = std::max(1, output_width - config_.gap);
    if (ir_width + vis_width > usable_width) {
        const float scale =
            static_cast<float>(usable_width) /
            (ir_width + vis_width);
        content_height = std::max(
            1, static_cast<int>(content_height * scale));
        ir_width = std::max(
            1, static_cast<int>(content_height * ir_aspect + 0.5f));
        vis_width = std::max(
            1, static_cast<int>(content_height * vis_aspect + 0.5f));
    }

    const int ir_x = 0;
    const int vis_x = ir_width + config_.gap;
    if (vis_x + vis_width > output_width &&
        ir_width + vis_width > usable_width) {
        /* 数值取整后仍可能超出 1px, 收窄可见光画面兜底 */
        vis_width = std::max(1, output_width - vis_x);
    }

    output.resize(ir_width + config_.gap + vis_width,
                  content_height);

    /* 整帧填充分割线颜色 (gap 列之外的区域随后被 blit 覆盖,
     * 这样输出缓冲复用时不会残留上一帧内容) */
    std::memset(output.data.data(), config_.gap_color[0],
                output.data.size());

    /* 左侧: IR 主画面 */
    resize_bilinear_bgr(input.infrared,
                        0, 0, input.infrared.width,
                        input.infrared.height,
                        output, ir_x, 0, ir_width, content_height);

    /* 右侧: 可见光副画面 (已配准到 IR 视角) */
    resize_bilinear_bgr(input.visible,
                        0, 0, input.visible.width,
                        input.visible.height,
                        output, vis_x, 0, vis_width, content_height);

    last_layout_.ir_x = ir_x;
    last_layout_.ir_y = 0;
    last_layout_.ir_width = ir_width;
    last_layout_.ir_height = content_height;
    last_layout_.vis_x = vis_x;
    last_layout_.vis_y = 0;
    last_layout_.vis_width = vis_width;
    last_layout_.vis_height = content_height;
    last_layout_.valid = true;
    return true;
}
