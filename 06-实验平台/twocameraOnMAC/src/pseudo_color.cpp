#include "pseudo_color.h"

#include <algorithm>
#include <cmath>

namespace {

/* JET 伪彩查找表: 分段线性公式与 cv::COLORMAP_JET 一致
 *   r = clamp(1.5 - |4x - 3|), g = clamp(1.5 - |4x - 2|),
 *   b = clamp(1.5 - |4x - 1|) */
struct JetLut {
    uint8_t bgr[256][3];

    JetLut()
    {
        for (int i = 0; i < 256; ++i) {
            const float x = i / 255.0f;
            const float r = clamp01(1.5f - std::fabs(4.0f * x - 3.0f));
            const float g = clamp01(1.5f - std::fabs(4.0f * x - 2.0f));
            const float b = clamp01(1.5f - std::fabs(4.0f * x - 1.0f));
            bgr[i][0] = static_cast<uint8_t>(b * 255.0f + 0.5f);
            bgr[i][1] = static_cast<uint8_t>(g * 255.0f + 0.5f);
            bgr[i][2] = static_cast<uint8_t>(r * 255.0f + 0.5f);
        }
    }

    static float clamp01(float v)
    {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
};

const JetLut kJetLut;

} // namespace

void apply_pseudo_color_in_rect(BgrFrame& frame,
                                int x, int y,
                                int width, int height,
                                bool blend, float alpha,
                                uint8_t* claimed,
                                int claim_stride)
{
    if (!frame.valid() || width <= 0 || height <= 0) {
        return;
    }
    x = std::max(0, std::min(x, frame.width - 1));
    y = std::max(0, std::min(y, frame.height - 1));
    width = std::min(width, frame.width - x);
    height = std::min(height, frame.height - y);
    if (width <= 0 || height <= 0) {
        return;
    }

    const int stride = frame.stride;
    uint8_t* data = frame.data.data();
    const float keep = 1.0f - alpha;
    const bool use_claim =
        (claimed != nullptr && claim_stride > 0);

    for (int row = y; row < y + height; ++row) {
        uint8_t* line = data +
            static_cast<size_t>(row) * stride +
            static_cast<size_t>(x) * 3;
        uint8_t* claim_line =
            use_claim
                ? claimed +
                      static_cast<size_t>(row) * claim_stride + x
                : nullptr;
        for (int col = 0; col < width; ++col) {
            uint8_t* px = line + col * 3;
            if (use_claim) {
                if (claim_line[col]) {
                    continue; /* 已被其他框着色, 跳过 */
                }
                claim_line[col] = 1;
            }
            /* gray = 0.299R + 0.587G + 0.114B, 定点 (77+150+29)/256 */
            const int gray =
                (px[2] * 77 + px[1] * 150 + px[0] * 29) >> 8;
            const uint8_t* color = kJetLut.bgr[gray];
            if (!blend) {
                px[0] = color[0];
                px[1] = color[1];
                px[2] = color[2];
            } else {
                for (int c = 0; c < 3; ++c) {
                    px[c] = static_cast<uint8_t>(
                        alpha * color[c] + keep * px[c] + 0.5f);
                }
            }
        }
    }
}

void blend_pseudo_color_from(const BgrFrame& source,
                             int src_x, int src_y,
                             int src_w, int src_h,
                             BgrFrame& destination,
                             int dst_x, int dst_y,
                             int dst_w, int dst_h,
                             float alpha,
                             uint8_t* claimed,
                             int claim_stride)
{
    if (!source.valid() || !destination.valid()) {
        return;
    }
    /* ROI clamp */
    src_x = std::max(0, std::min(src_x, source.width - 1));
    src_y = std::max(0, std::min(src_y, source.height - 1));
    src_w = std::min(src_w, source.width - src_x);
    src_h = std::min(src_h, source.height - src_y);
    dst_x = std::max(0, std::min(dst_x, destination.width - 1));
    dst_y = std::max(0, std::min(dst_y, destination.height - 1));
    dst_w = std::min(dst_w, destination.width - dst_x);
    dst_h = std::min(dst_h, destination.height - dst_y);
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }

    const float keep = 1.0f - alpha;
    const uint8_t* src_data = source.data.data();
    const bool use_claim =
        (claimed != nullptr && claim_stride > 0);

    for (int row = 0; row < dst_h; ++row) {
        /* 最近邻采样源 ROI */
        const int sy = src_y + row * src_h / dst_h;
        uint8_t* line = destination.data.data() +
            static_cast<size_t>(dst_y + row) * destination.stride +
            static_cast<size_t>(dst_x) * 3;
        uint8_t* claim_line =
            use_claim
                ? claimed +
                      static_cast<size_t>(dst_y + row) *
                          claim_stride +
                      dst_x
                : nullptr;
        for (int col = 0; col < dst_w; ++col) {
            if (use_claim) {
                if (claim_line[col]) {
                    continue; /* 已被其他框着色, 跳过 */
                }
                claim_line[col] = 1;
            }
            const int sx = src_x + col * src_w / dst_w;
            const uint8_t* sp = src_data +
                static_cast<size_t>(sy) * source.stride +
                static_cast<size_t>(sx) * 3;
            /* 红外灰度 → JET 伪彩 */
            const int gray =
                (sp[2] * 77 + sp[1] * 150 + sp[0] * 29) >> 8;
            const uint8_t* color = kJetLut.bgr[gray];
            uint8_t* px = line + col * 3;
            for (int c = 0; c < 3; ++c) {
                px[c] = static_cast<uint8_t>(
                    alpha * color[c] + keep * px[c] + 0.5f);
            }
        }
    }
}
