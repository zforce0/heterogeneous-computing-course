#include "frame_compositor.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

FrameCompositor::FrameCompositor(const PipConfig& config)
    : config_(config)
{
}

bool FrameCompositor::compose(const SyncedFrames& input,
                              int output_width, int output_height,
                              BgrFrame& output) const
{
    if (!input.visible.valid() || !input.infrared.valid() ||
        output_width <= 0 || output_height <= 0) {
        return false;
    }

    output.resize(output_width, output_height);
    std::fill(output.data.begin(), output.data.end(), 0);
    output.timestamp_us = input.visible.timestamp_us;
    output.sequence = input.visible.sequence;

    const double visible_scale = std::min(
        static_cast<double>(output_width) / input.visible.width,
        static_cast<double>(output_height) / input.visible.height);
    const int visible_width = std::max(
        1, static_cast<int>(input.visible.width * visible_scale));
    const int visible_height = std::max(
        1, static_cast<int>(input.visible.height * visible_scale));
    const int visible_x = (output_width - visible_width) / 2;
    const int visible_y = (output_height - visible_height) / 2;
    blit_scaled(input.visible, output, visible_x, visible_y,
                visible_width, visible_height);

    int pip_width = std::max(
        1, static_cast<int>(output_width * config_.width_ratio));
    int pip_height = std::max(
        1, pip_width * input.infrared.height / input.infrared.width);
    const int maximum_pip_height = std::max(
        1, static_cast<int>(
               output_height * config_.maximum_height_ratio));
    if (pip_height > maximum_pip_height) {
        pip_height = maximum_pip_height;
        pip_width = std::max(
            1, pip_height * input.infrared.width /
                   input.infrared.height);
    }

    const int pip_x = std::max(
        0, output_width - config_.margin - pip_width);
    const int pip_y = std::max(
        0, output_height - config_.margin - pip_height);
    blit_scaled(input.infrared, output, pip_x, pip_y,
                pip_width, pip_height);

    const int64_t absolute_delta =
        std::llabs(input.timestamp_delta_us);
    uint8_t blue = 0;
    uint8_t green = 255;
    uint8_t red = 0;
    if (absolute_delta > 100000) {
        green = 0;
        red = 255;
    } else if (absolute_delta > 50000) {
        green = 255;
        red = 255;
    }
    draw_border(output, pip_x, pip_y, pip_width, pip_height,
                config_.border, blue, green, red);

    /* 记录布局供 caller 叠加文字/FPS 等信息使用 */
    last_layout_.visible_x = visible_x;
    last_layout_.visible_y = visible_y;
    last_layout_.visible_width = visible_width;
    last_layout_.visible_height = visible_height;
    last_layout_.pip_x = pip_x;
    last_layout_.pip_y = pip_y;
    last_layout_.pip_width = pip_width;
    last_layout_.pip_height = pip_height;
    last_layout_.valid = true;
    return true;
}

void FrameCompositor::blit_scaled(const BgrFrame& source,
                                  BgrFrame& destination,
                                  int x, int y, int width, int height)
{
    if (!source.valid() || !destination.valid() ||
        width <= 0 || height <= 0) {
        return;
    }

    const int copy_width =
        std::min(width, destination.width - x);
    const int copy_height =
        std::min(height, destination.height - y);
    if (x < 0 || y < 0 || copy_width <= 0 || copy_height <= 0) {
        return;
    }

    if (source.width == width && source.height == height) {
        for (int row = 0; row < copy_height; ++row) {
            const uint8_t* source_row =
                source.data.data() +
                static_cast<size_t>(row) * source.stride;
            uint8_t* destination_row =
                destination.data.data() +
                static_cast<size_t>(y + row) *
                    destination.stride +
                x * 3;
            std::memcpy(destination_row, source_row,
                        static_cast<size_t>(copy_width) * 3);
        }
        return;
    }

    for (int destination_y = 0;
         destination_y < copy_height; ++destination_y) {
        const int source_y =
            destination_y * source.height / height;
        const uint8_t* source_row =
            source.data.data() +
            static_cast<size_t>(source_y) * source.stride;
        uint8_t* destination_row =
            destination.data.data() +
            static_cast<size_t>(y + destination_y) *
                destination.stride +
            x * 3;
        for (int destination_x = 0;
             destination_x < copy_width; ++destination_x) {
            const int source_x =
                destination_x * source.width / width;
            const uint8_t* source_pixel =
                source_row + source_x * 3;
            uint8_t* destination_pixel =
                destination_row + destination_x * 3;
            destination_pixel[0] = source_pixel[0];
            destination_pixel[1] = source_pixel[1];
            destination_pixel[2] = source_pixel[2];
        }
    }
}

void FrameCompositor::draw_border(BgrFrame& frame,
                                  int x, int y, int width, int height,
                                  int thickness, uint8_t blue,
                                  uint8_t green, uint8_t red)
{
    if (!frame.valid() || thickness <= 0) {
        return;
    }

    for (int layer = 0; layer < thickness; ++layer) {
        const int left = std::max(0, x - layer);
        const int right =
            std::min(frame.width - 1, x + width - 1 + layer);
        const int top = std::max(0, y - layer);
        const int bottom =
            std::min(frame.height - 1, y + height - 1 + layer);

        for (int px = left; px <= right; ++px) {
            uint8_t* top_pixel =
                frame.data.data() +
                static_cast<size_t>(top) * frame.stride +
                px * 3;
            uint8_t* bottom_pixel =
                frame.data.data() +
                static_cast<size_t>(bottom) * frame.stride +
                px * 3;
            top_pixel[0] = bottom_pixel[0] = blue;
            top_pixel[1] = bottom_pixel[1] = green;
            top_pixel[2] = bottom_pixel[2] = red;
        }
        for (int py = top; py <= bottom; ++py) {
            uint8_t* left_pixel =
                frame.data.data() +
                static_cast<size_t>(py) * frame.stride +
                left * 3;
            uint8_t* right_pixel =
                frame.data.data() +
                static_cast<size_t>(py) * frame.stride +
                right * 3;
            left_pixel[0] = right_pixel[0] = blue;
            left_pixel[1] = right_pixel[1] = green;
            left_pixel[2] = right_pixel[2] = red;
        }
    }
}
