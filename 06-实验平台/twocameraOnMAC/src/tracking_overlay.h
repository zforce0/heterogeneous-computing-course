#pragma once

#include "frame.h"
#include "side_by_side_compositor.h"
#include "tracker.h"

#include <vector>

/* 跟踪结果叠加配置 (camerayolo.md 4.2.2 TrackingOverlayConfig) */
struct TrackingOverlayConfig {
    /* 基础绘制 */
    int box_thickness = 2;
    bool draw_track_id = true;      /* 画跟踪 ID (#7) */
    /* 运动意图 (camerayolo.md 3.4.4 / 5.11) */
    bool draw_motion_arrow = true;  /* 画运动方向箭头 */
    bool draw_trail = true;         /* 画轨迹路径 */
    float arrow_scale = 2.0f;       /* 箭头长度系数 (像素/单位速度) */
    float max_arrow_length = 60.0f; /* 箭头最大长度 */
    float speed_threshold = 0.5f;   /* 低于此速度不画箭头 (静止判定) */
    /* 伪彩 */
    bool ir_pseudo_color = true;      /* IR 子画面: 框内红外自身伪彩 (完整) */
    bool visible_pseudo_color = true; /* 可见光子画面: 框内叠加红外伪彩 */
    float visible_pseudo_alpha = 0.5f; /* 红外伪彩在可见光上的不透明度 */
};

/* 在并排合成帧上绘制: 跟踪框 + 跟踪 ID 标签 + 运动方向箭头
 * + 轨迹路径 + 框内伪彩。
 * tracked_objects 的坐标/速度均在 model_size×model_size 坐标系,
 * 按 layout 两个子画面各自的缩放比例映射后绘制 (camerayolo.md 3.4.4)。
 * infrared: 配准后的红外帧 (与可见光子画面同视角), 其热力伪彩以
 * visible_pseudo_alpha 叠加到可见光子画面的检测框内。
 * 全部为纯像素绘制 (点阵字库 + DDA 直线), 不依赖 OpenCV。 */
class TrackingOverlay {
public:
    explicit TrackingOverlay(
        const TrackingOverlayConfig& config =
            TrackingOverlayConfig());

    void draw(BgrFrame& frame,
              const std::vector<TrackedObject>& tracked_objects,
              const SideBySideLayout& layout,
              int model_size,
              const BgrFrame& infrared) const;

private:
    void draw_on_region(BgrFrame& frame,
                        const SideBySideLayout& layout,
                        bool is_ir_region,
                        const std::vector<TrackedObject>& objects,
                        int model_size,
                        const BgrFrame& infrared) const;

    TrackingOverlayConfig config_;

    /* 伪彩多框重叠去重位图 (整帧尺寸, 按帧绝对坐标索引):
     * 同一像素只着色一次, 避免重叠框二次映射失真;
     * IR / 可见光子画面各自独立, 每个子画面绘制前清零 */
    mutable std::vector<uint8_t> pseudo_claim_;
};
