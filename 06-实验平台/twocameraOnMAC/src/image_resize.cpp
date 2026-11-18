#include "image_resize.h"

#include <algorithm>

void resize_bilinear_bgr(const BgrFrame& source,
                         int source_x, int source_y,
                         int source_width, int source_height,
                         BgrFrame& destination,
                         int destination_x, int destination_y,
                         int destination_width,
                         int destination_height)
{
    if (!source.valid() || !destination.valid()) {
        return;
    }
    if (source_width <= 0 || source_height <= 0 ||
        destination_width <= 0 || destination_height <= 0) {
        return;
    }

    /* 源 ROI 裁剪到源帧范围内 */
    source_x = std::max(0, std::min(source_x, source.width - 1));
    source_y = std::max(0, std::min(source_y, source.height - 1));
    source_width = std::min(source_width,
                            source.width - source_x);
    source_height = std::min(source_height,
                             source.height - source_y);
    if (source_width <= 0 || source_height <= 0) {
        return;
    }

    /* 目标 ROI 裁剪到目标帧范围内 */
    destination_x = std::max(0,
        std::min(destination_x, destination.width - 1));
    destination_y = std::max(0,
        std::min(destination_y, destination.height - 1));
    destination_width = std::min(destination_width,
        destination.width - destination_x);
    destination_height = std::min(destination_height,
        destination.height - destination_y);
    if (destination_width <= 0 || destination_height <= 0) {
        return;
    }

    /* 像素中心对齐的采样比例, 避免 1:1 时边缘错位 */
    const float x_ratio =
        static_cast<float>(source_width) / destination_width;
    const float y_ratio =
        static_cast<float>(source_height) / destination_height;

    for (int dy = 0; dy < destination_height; ++dy) {
        const float fy = (dy + 0.5f) * y_ratio - 0.5f;
        int y0 = static_cast<int>(fy);
        float wy = fy - y0;
        if (y0 < 0) {
            y0 = 0;
            wy = 0.0f;
        }
        if (y0 > source_height - 1) {
            y0 = source_height - 1;
            wy = 0.0f;
        }
        const int y1 = std::min(y0 + 1, source_height - 1);

        const uint8_t* row0 =
            source.data.data() +
            static_cast<size_t>(source_y + y0) * source.stride;
        const uint8_t* row1 =
            source.data.data() +
            static_cast<size_t>(source_y + y1) * source.stride;
        uint8_t* dst_row =
            destination.data.data() +
            static_cast<size_t>(destination_y + dy) *
                destination.stride;

        for (int dx = 0; dx < destination_width; ++dx) {
            const float fx = (dx + 0.5f) * x_ratio - 0.5f;
            int x0 = static_cast<int>(fx);
            float wx = fx - x0;
            if (x0 < 0) {
                x0 = 0;
                wx = 0.0f;
            }
            if (x0 > source_width - 1) {
                x0 = source_width - 1;
                wx = 0.0f;
            }
            const int x1 = std::min(x0 + 1, source_width - 1);

            const uint8_t* p00 =
                row0 + static_cast<size_t>(source_x + x0) * 3;
            const uint8_t* p01 =
                row0 + static_cast<size_t>(source_x + x1) * 3;
            const uint8_t* p10 =
                row1 + static_cast<size_t>(source_x + x0) * 3;
            const uint8_t* p11 =
                row1 + static_cast<size_t>(source_x + x1) * 3;
            uint8_t* dst =
                dst_row +
                static_cast<size_t>(destination_x + dx) * 3;

            const float w00 = (1.0f - wx) * (1.0f - wy);
            const float w01 = wx * (1.0f - wy);
            const float w10 = (1.0f - wx) * wy;
            const float w11 = wx * wy;

            for (int c = 0; c < 3; ++c) {
                const float value =
                    p00[c] * w00 + p01[c] * w01 +
                    p10[c] * w10 + p11[c] * w11;
                dst[c] = static_cast<uint8_t>(value + 0.5f);
            }
        }
    }
}
