#pragma once

#include "detector.h"
#include "frame.h"

#include <deque>
#include <utility>
#include <vector>

/* 跟踪输出对象 (camerayolo.md 3.4.3):
 * 坐标/速度均在 model_size (640×640) 坐标系, x/y 为左上角,
 * vx/vy 为 Kalman 估计速度 (像素/帧)。 */
struct TrackedObject {
    int track_id = -1;         /* 全局唯一跟踪 ID, 从 1 递增 */
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    float confidence = 0.0f;
    int class_id = -1;
    float vx = 0.0f;
    float vy = 0.0f;
    int age = 0;               /* 连续命中帧数 */
    bool confirmed = false;    /* age >= track_thresh 后确认 */
    std::deque<std::pair<float, float> > trail; /* 质心历史 (cx, cy) */

    TrackedObject() : trail() {}
};

/* 新目标确认回调: track 首次达到 confirmed 时调用
 * (camerayolo.md 3.4.8 新目标提示音) */
typedef void (*NewTargetCallback)(int track_id, void* user_data);

struct TrackerConfig {
    int track_thresh = 5;             /* 确认跟踪所需连续帧数 */
    int track_buffer = 30;            /* 丢失后保留帧数 (超过删除) */
    float match_thresh = 0.8f;        /* 第一轮匹配 IoU 阈值 */
    float low_thresh = 0.1f;          /* 低置信度下限 (低于丢弃) */
    int max_age = 30;                 /* 同 track_buffer, 兼容文档字段 */
    int trail_length = 15;            /* 轨迹历史保留长度 */
    float iou_weight = 0.9f;          /* 预留: IoU/中心距离加权 */
    float high_thresh = 0.3f;         /* 第一轮匹配/新建 track 的分数门槛
                                          (需与模型置信度分布匹配:
                                          通道修复后 YOLOFuse 输出多在
                                          0.3~0.5, 取 0.3 避免漏跟) */
    bool alert_on_new_target = true;  /* 新目标确认时触发回调 */
    /* 新目标提示的稳定存在时长阈值 (秒): track 从创建起持续被
     * 观测超过该时长才触发 NewTargetCallback (3.4.8)。
     * 与 track_thresh (帧数, 用于显示确认) 相互独立 */
    float alert_stable_seconds = 1.0f;
};

class ITracker {
public:
    virtual ~ITracker() {}
    /* 初始化 (第一帧) */
    virtual std::vector<TrackedObject> init(
        const std::vector<Detection>& detections) = 0;
    /* 更新 (后续帧) */
    virtual std::vector<TrackedObject> update(
        const std::vector<Detection>& detections) = 0;
    virtual void reset() = 0;
};
