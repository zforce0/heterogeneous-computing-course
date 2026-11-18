#include "tracking_overlay.h"
#include "pseudo_color.h"
#include "yolo_postprocess.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

/* ---------- 基础绘图原语 (纯像素, 后端无关) ---------- */

inline void plot(BgrFrame& frame, int x, int y,
                 const uint8_t color[3], int thickness)
{
    if (x < 0 || y < 0 || x >= frame.width || y >= frame.height) {
        return;
    }
    const int half = std::max(0, thickness - 1);
    for (int dy = 0; dy <= half; ++dy) {
        const int yy = y + dy;
        if (yy >= frame.height) {
            break;
        }
        for (int dx = 0; dx <= half; ++dx) {
            const int xx = x + dx;
            if (xx >= frame.width) {
                break;
            }
            uint8_t* px = frame.data.data() +
                static_cast<size_t>(yy) * frame.stride +
                static_cast<size_t>(xx) * 3;
            px[0] = color[0];
            px[1] = color[1];
            px[2] = color[2];
        }
    }
}

/* DDA 直线 */
void draw_line(BgrFrame& frame, float x0, float y0,
               float x1, float y1,
               const uint8_t color[3], int thickness)
{
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    int steps = static_cast<int>(
        std::max(std::fabs(dx), std::fabs(dy)));
    steps = std::max(1, std::min(steps, 512));
    for (int s = 0; s <= steps; ++s) {
        const float t =
            static_cast<float>(s) / static_cast<float>(steps);
        plot(frame,
             static_cast<int>(x0 + dx * t + 0.5f),
             static_cast<int>(y0 + dy * t + 0.5f),
             color, thickness);
    }
}

/* ---------- 5x7 点阵字库: '#' 与数字, 用于跟踪 ID 标签 ---------- */

/* 每字符 7 行, 每行低 5 位为像素 (MSB 在左) */
const uint8_t kFont5x7[11][7] = {
    /* '#' */
    {0x00, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x00},
    /* '0' */
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    /* '1' */
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    /* '2' */
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    /* '3' */
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    /* '4' */
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    /* '5' */
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    /* '6' */
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    /* '7' */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    /* '8' */
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    /* '9' */
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}
};

/* 字符索引: '#' → 0, '0'-'9' → 1-10 */
int glyph_index(char c)
{
    if (c == '#') {
        return 0;
    }
    if (c >= '0' && c <= '9') {
        return 1 + (c - '0');
    }
    return -1;
}

void draw_text(BgrFrame& frame, int x, int y, const char* text,
               const uint8_t color[3], int thickness)
{
    int cursor = x;
    for (const char* p = text; *p != '\0'; ++p) {
        const int glyph = glyph_index(*p);
        if (glyph < 0) {
            cursor += 6;
            continue;
        }
        for (int row = 0; row < 7; ++row) {
            const uint8_t bits = kFont5x7[glyph][row];
            for (int col = 0; col < 5; ++col) {
                if (bits & (0x10 >> col)) {
                    plot(frame, cursor + col, y + row, color,
                         std::max(1, thickness - 1));
                }
            }
        }
        cursor += 6; /* 5 像素字宽 + 1 像素间隔 */
    }
}

int text_width(const char* text)
{
    int n = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        ++n;
    }
    return n > 0 ? n * 6 - 1 : 0;
}

/* 运动箭头颜色分级 (camerayolo.md 5.11):
 * 静止/低速灰, 正常绿, 较快黄, 高速红 (BGR) */
void speed_color(float speed, uint8_t color[3])
{
    if (speed < 0.5f) {
        color[0] = 128; color[1] = 128; color[2] = 128; /* 灰 */
    } else if (speed < 2.0f) {
        color[0] = 0; color[1] = 255; color[2] = 0;     /* 绿 */
    } else if (speed < 5.0f) {
        color[0] = 0; color[1] = 255; color[2] = 255;   /* 黄 */
    } else {
        color[0] = 0; color[1] = 0; color[2] = 255;     /* 红 */
    }
}

} // namespace

TrackingOverlay::TrackingOverlay(
    const TrackingOverlayConfig& config)
    : config_(config)
{
}

void TrackingOverlay::draw_on_region(
    BgrFrame& frame, const SideBySideLayout& layout,
    bool is_ir_region,
    const std::vector<TrackedObject>& objects,
    int model_size,
    const BgrFrame& infrared) const
{
    const int region_x = is_ir_region ? layout.ir_x : layout.vis_x;
    const int region_y = is_ir_region ? layout.ir_y : layout.vis_y;
    const int region_w =
        is_ir_region ? layout.ir_width : layout.vis_width;
    const int region_h =
        is_ir_region ? layout.ir_height : layout.vis_height;
    if (region_w <= 0 || region_h <= 0) {
        return;
    }

    const float scale_x =
        static_cast<float>(region_w) / model_size;
    const float scale_y =
        static_cast<float>(region_h) / model_size;

    /* 1. 框内伪彩 (camerayolo.md 4.2.5):
     * - IR 子画面: 红外自身伪彩完整覆盖 (热力图)
     * - 可见光子画面: 取红外帧对应区域的伪彩, 以 50% 不透明度
     *   叠加到可见光画面上 (显示的是红外热力, 而非可见光伪彩)
     * 多框重叠去重: 本子画面开始前清零标记位图, 同一像素
     * 只着色一次 (重叠区域不被两个框叠加重复映射) */
    const bool any_pseudo =
        (is_ir_region ? config_.ir_pseudo_color
                      : config_.visible_pseudo_color) &&
        !objects.empty();
    if (any_pseudo) {
        pseudo_claim_.assign(
            static_cast<size_t>(frame.width) * frame.height, 0);
    }
    for (size_t i = 0; i < objects.size(); ++i) {
        const TrackedObject& obj = objects[i];
        const bool enable =
            is_ir_region ? config_.ir_pseudo_color
                         : config_.visible_pseudo_color;
        if (!enable) {
            continue;
        }
        const int px =
            region_x + static_cast<int>(obj.x * scale_x);
        const int py =
            region_y + static_cast<int>(obj.y * scale_y);
        const int pw =
            static_cast<int>(obj.w * scale_x) + 1;
        const int ph =
            static_cast<int>(obj.h * scale_y) + 1;
        if (is_ir_region) {
            apply_pseudo_color_in_rect(
                frame, px, py, pw, ph, false, 1.0f,
                any_pseudo ? pseudo_claim_.data() : nullptr,
                frame.width);
        } else {
            /* 检测框 → 红外帧 ROI (同视角, 按红外帧分辨率换算) */
            const float ir_sx = static_cast<float>(
                infrared.width) / model_size;
            const float ir_sy = static_cast<float>(
                infrared.height) / model_size;
            blend_pseudo_color_from(
                infrared,
                static_cast<int>(obj.x * ir_sx),
                static_cast<int>(obj.y * ir_sy),
                static_cast<int>(obj.w * ir_sx) + 1,
                static_cast<int>(obj.h * ir_sy) + 1,
                frame, px, py, pw, ph,
                config_.visible_pseudo_alpha,
                any_pseudo ? pseudo_claim_.data() : nullptr,
                frame.width);
        }
    }

    /* 2. 轨迹路径 (折线渐变, 最近点最亮) */
    if (config_.draw_trail) {
        for (size_t i = 0; i < objects.size(); ++i) {
            const std::deque<std::pair<float, float> >& trail =
                objects[i].trail;
            if (trail.size() < 2) {
                continue;
            }
            const size_t n = trail.size();
            for (size_t k = 1; k < n; ++k) {
                /* 越新的点亮度越高: 0.35 ~ 1.0 */
                const float f =
                    static_cast<float>(k) / (n - 1);
                const uint8_t level =
                    static_cast<uint8_t>(90 + 165 * f);
                const uint8_t color[3] = {0, level, level}; /* 青 */
                draw_line(
                    frame,
                    region_x + trail[k - 1].first * scale_x,
                    region_y + trail[k - 1].second * scale_y,
                    region_x + trail[k].first * scale_x,
                    region_y + trail[k].second * scale_y,
                    color, 1);
            }
        }
    }

    /* 3. 跟踪框 (画在伪彩之上, 复用 yolo_postprocess 的区域映射绘制) */
    {
        std::vector<Detection> boxes;
        boxes.reserve(objects.size());
        for (size_t i = 0; i < objects.size(); ++i) {
            Detection d;
            d.x = objects[i].x;
            d.y = objects[i].y;
            d.w = objects[i].w;
            d.h = objects[i].h;
            d.confidence = objects[i].confidence;
            d.class_id = objects[i].class_id;
            boxes.push_back(d);
        }
        draw_detections_in_region(frame, boxes, model_size,
                                  region_x, region_y, region_w,
                                  region_h, config_.box_thickness);
    }

    /* 4. 跟踪 ID 标签 + 运动方向箭头 */
    for (size_t i = 0; i < objects.size(); ++i) {
        const TrackedObject& obj = objects[i];

        /* 框映射到子画面坐标 */
        const float box_x = region_x + obj.x * scale_x;
        const float box_y = region_y + obj.y * scale_y;
        const float box_w = obj.w * scale_x;
        const float box_h = obj.h * scale_y;

        /* ID 标签: 框上方 "#<id>", 黑底白字 */
        if (config_.draw_track_id) {
            char label[16];
            std::snprintf(label, sizeof(label), "#%d",
                          obj.track_id);
            const int tw = text_width(label);
            const int lx =
                std::max(region_x, static_cast<int>(box_x));
            const int ly =
                std::max(region_y,
                         static_cast<int>(box_y) - 9);
            /* 黑底 */
            for (int r = ly - 1; r < ly + 8; ++r) {
                for (int c = lx - 1; c < lx + tw + 1; ++c) {
                    if (r >= 0 && c >= 0 && r < frame.height &&
                        c < frame.width) {
                        uint8_t* px =
                            frame.data.data() +
                            static_cast<size_t>(r) *
                                frame.stride +
                            static_cast<size_t>(c) * 3;
                        px[0] = 0;
                        px[1] = 0;
                        px[2] = 0;
                    }
                }
            }
            const uint8_t white[3] = {255, 255, 255};
            draw_text(frame, lx, ly, label, white, 2);
        }

        /* 运动方向箭头 (camerayolo.md 3.4.4):
         * 起点 = 框底部中心, 方向 = 速度向量 (区域坐标缩放),
         * 长度 = min(|v| * arrow_scale, max_arrow_length),
         * 颜色按速度分级; 低于 speed_threshold 视为静止不画 */
        if (config_.draw_motion_arrow) {
            const float speed_640 =
                std::sqrt(obj.vx * obj.vx + obj.vy * obj.vy);
            if (speed_640 < config_.speed_threshold) {
                continue;
            }
            float dir_x = obj.vx * scale_x;
            float dir_y = obj.vy * scale_y;
            const float region_speed =
                std::sqrt(dir_x * dir_x + dir_y * dir_y);
            if (region_speed < 1.0e-3f) {
                continue;
            }
            const float length =
                std::min(region_speed * config_.arrow_scale,
                         config_.max_arrow_length);
            dir_x /= region_speed;
            dir_y /= region_speed;

            const float start_x = box_x + box_w * 0.5f;
            const float start_y = box_y + box_h;
            const float tip_x = start_x + dir_x * length;
            const float tip_y = start_y + dir_y * length;

            uint8_t color[3];
            speed_color(speed_640, color);
            draw_line(frame, start_x, start_y, tip_x, tip_y,
                      color, config_.box_thickness);

            /* 箭头头部: 沿反方向 ±30° 两条短线 */
            const float head = std::min(length * 0.35f, 10.0f);
            const float cos30 = 0.866f;
            const float sin30 = 0.5f;
            const float back_x = -dir_x;
            const float back_y = -dir_y;
            const float wing1_x =
                back_x * cos30 - back_y * sin30;
            const float wing1_y =
                back_x * sin30 + back_y * cos30;
            const float wing2_x =
                back_x * cos30 + back_y * sin30;
            const float wing2_y =
                -back_x * sin30 + back_y * cos30;
            draw_line(frame, tip_x, tip_y,
                      tip_x + wing1_x * head,
                      tip_y + wing1_y * head, color, 1);
            draw_line(frame, tip_x, tip_y,
                      tip_x + wing2_x * head,
                      tip_y + wing2_y * head, color, 1);
        }
    }
}

void TrackingOverlay::draw(
    BgrFrame& frame,
    const std::vector<TrackedObject>& tracked_objects,
    const SideBySideLayout& layout, int model_size,
    const BgrFrame& infrared) const
{
    if (!layout.valid || tracked_objects.empty() ||
        model_size <= 0 || !frame.valid()) {
        return;
    }
    /* IR 主画面 (左侧) */
    draw_on_region(frame, layout, true, tracked_objects,
                   model_size, infrared);
    /* 可见光副画面 (右侧) */
    draw_on_region(frame, layout, false, tracked_objects,
                   model_size, infrared);
}
