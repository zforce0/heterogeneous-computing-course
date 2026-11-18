#pragma once

#include "kalman_filter.h"
#include "tracker.h"

#include <chrono>
#include <vector>

/* ByteTrack C++ 原生实现 (camerayolo.md 3.4.2):
 * Step 1 score 分组 → Step 2 Kalman predict →
 * Step 3 高分框与活跃 track 的 Hungarian IoU 匹配 →
 * Step 4 低分框与剩余 track 的第二轮匹配 → 新目标确认回调 */
class ByteTrack : public ITracker {
public:
    explicit ByteTrack(
        const TrackerConfig& config = TrackerConfig());

    std::vector<TrackedObject> init(
        const std::vector<Detection>& detections) override;
    std::vector<TrackedObject> update(
        const std::vector<Detection>& detections) override;
    void reset() override;

    /* 新目标提示音回调注册 (camerayolo.md 3.4.8) */
    void set_new_target_callback(NewTargetCallback callback,
                                 void* user_data = NULL);

private:
    struct Tracklet {
        int track_id;
        KalmanFilter kf;
        /* 滤波后边界框 (model_size 坐标系, 左上角 xywh) */
        float x, y, w, h;
        /* Kalman 预测框 (匹配用) */
        float pred_x, pred_y, pred_w, pred_h;
        float vx, vy;
        float confidence;
        int class_id;
        int age;                /* 连续命中帧数 */
        int time_since_update;  /* 连续未匹配帧数 */
        bool confirmed;
        /* 新目标提示 (3.4.8): 创建时刻 + 是否已提示过 */
        std::chrono::steady_clock::time_point first_seen;
        bool alerted;
        std::deque<std::pair<float, float> > trail;

        Tracklet()
            : track_id(-1),
              x(0), y(0), w(0), h(0),
              pred_x(0), pred_y(0), pred_w(0), pred_h(0),
              vx(0), vy(0), confidence(0), class_id(-1),
              age(0), time_since_update(0), confirmed(false),
              first_seen(), alerted(false), trail() {}
    };

    void apply_detection(Tracklet& track,
                         const Detection& det);
    void create_track(const Detection& det);

    TrackerConfig config_;
    std::vector<Tracklet> tracklets_;
    int next_id_;
    NewTargetCallback new_target_callback_;
    void* new_target_user_data_;
};
