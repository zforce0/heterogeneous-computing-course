#include "tracker_bytetrack.h"
#include "hungarian.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

/* 第二轮匹配关联距离阈值 (1 - IoU, 即 IoU >= 0.5;
 * 原版 ByteTrack 第二轮 thresh=0.5, 低分框防误关联更严) */
const float kSecondMatchDistance = 0.5f;

/* xywh (左上角) IoU */
float iou_xywh(float ax, float ay, float aw, float ah,
               float bx, float by, float bw, float bh)
{
    const float x1 = std::max(ax, bx);
    const float y1 = std::max(ay, by);
    const float x2 = std::min(ax + aw, bx + bw);
    const float y2 = std::min(ay + ah, by + bh);
    const float inter_w = x2 - x1;
    const float inter_h = y2 - y1;
    if (inter_w <= 0.0f || inter_h <= 0.0f) {
        return 0.0f;
    }
    const float inter = inter_w * inter_h;
    const float area_a = aw * ah;
    const float area_b = bw * bh;
    return inter / (area_a + area_b - inter + 1.0e-6f);
}

struct BoxRef {
    float x, y, w, h;
};

struct MatchResult {
    std::vector<std::pair<int, int> > matches; /* (track_idx, det_idx) */
    std::vector<int> unmatched_tracks;
    std::vector<int> unmatched_dets;
};

/* 用 IoU 距离 (1 - IoU) 做 Hungarian 匹配。
 * distance_threshold 为关联距离阈值 (原版 ByteTrack 语义:
 * 第一轮 match_thresh=0.8 → IoU >= 0.2; 超过阈值的配对丢弃) */
MatchResult match_boxes(const std::vector<BoxRef>& track_boxes,
                        const std::vector<BoxRef>& det_boxes,
                        float distance_threshold)
{
    MatchResult result;
    const int num_tracks = static_cast<int>(track_boxes.size());
    const int num_dets = static_cast<int>(det_boxes.size());

    if (num_tracks == 0 || num_dets == 0) {
        for (int i = 0; i < num_tracks; ++i) {
            result.unmatched_tracks.push_back(i);
        }
        for (int j = 0; j < num_dets; ++j) {
            result.unmatched_dets.push_back(j);
        }
        return result;
    }

    /* cost = 1 - IoU */
    std::vector<std::vector<float> > cost(
        num_tracks, std::vector<float>(num_dets));
    for (int i = 0; i < num_tracks; ++i) {
        for (int j = 0; j < num_dets; ++j) {
            const float iou = iou_xywh(
                track_boxes[i].x, track_boxes[i].y,
                track_boxes[i].w, track_boxes[i].h,
                det_boxes[j].x, det_boxes[j].y,
                det_boxes[j].w, det_boxes[j].h);
            cost[i][j] = 1.0f - iou;
        }
    }

    std::vector<int> assignment;
    hungarian_solve(cost, assignment);

    std::vector<char> det_matched(num_dets, 0);
    for (int i = 0; i < num_tracks; ++i) {
        const int j = assignment[i];
        if (j >= 0 && j < num_dets &&
            cost[i][j] <= distance_threshold) {
            result.matches.push_back(std::make_pair(i, j));
            det_matched[j] = 1;
        } else {
            result.unmatched_tracks.push_back(i);
        }
    }
    for (int j = 0; j < num_dets; ++j) {
        if (!det_matched[j]) {
            result.unmatched_dets.push_back(j);
        }
    }
    return result;
}

} // namespace

ByteTrack::ByteTrack(const TrackerConfig& config)
    : config_(config),
      next_id_(1),
      new_target_callback_(NULL),
      new_target_user_data_(NULL)
{
}

void ByteTrack::set_new_target_callback(NewTargetCallback callback,
                                        void* user_data)
{
    new_target_callback_ = callback;
    new_target_user_data_ = user_data;
}

void ByteTrack::reset()
{
    tracklets_.clear();
    next_id_ = 1;
}

std::vector<TrackedObject> ByteTrack::init(
    const std::vector<Detection>& detections)
{
    /* 首帧无历史 track, 等价于一次 update (全部检测新建 track) */
    return update(detections);
}

void ByteTrack::apply_detection(Tracklet& track,
                                const Detection& det)
{
    const float w = det.w > 0.0f ? det.w : 1.0f;
    const float h = det.h > 0.0f ? det.h : 1.0f;
    const float aspect = w / h;
    const float measurement[4] = {
        det.x + w * 0.5f,
        det.y + h * 0.5f,
        aspect,
        h
    };
    const float* s = track.kf.update(measurement);

    /* 滤波后状态 → xywh (左上角) */
    track.h = s[3];
    track.w = s[2] * track.h;
    track.x = s[0] - track.w * 0.5f;
    track.y = s[1] - track.h * 0.5f;
    track.vx = s[4];
    track.vy = s[5];
    track.confidence = det.confidence;
    track.class_id = det.class_id;
    ++track.age;
    track.time_since_update = 0;

    track.trail.push_back(std::make_pair(s[0], s[1]));
    while (static_cast<int>(track.trail.size()) >
           config_.trail_length) {
        track.trail.pop_front();
    }
}

void ByteTrack::create_track(const Detection& det)
{
    Tracklet track;
    track.track_id = next_id_++;
    const float w = det.w > 0.0f ? det.w : 1.0f;
    const float h = det.h > 0.0f ? det.h : 1.0f;
    const float measurement[4] = {
        det.x + w * 0.5f,
        det.y + h * 0.5f,
        w / h,
        h
    };
    track.kf.init(measurement);
    track.x = det.x;
    track.y = det.y;
    track.w = det.w;
    track.h = det.h;
    track.vx = 0.0f;
    track.vy = 0.0f;
    track.confidence = det.confidence;
    track.class_id = det.class_id;
    track.age = 1;
    track.time_since_update = 0;
    track.confirmed = false;
    track.first_seen = std::chrono::steady_clock::now();
    track.alerted = false;
    track.trail.push_back(std::make_pair(measurement[0],
                                         measurement[1]));
    tracklets_.push_back(track);
}

std::vector<TrackedObject> ByteTrack::update(
    const std::vector<Detection>& detections)
{
    /* Step 1: score 分组 (camerayolo.md 3.4.2)
     * high: score >= high_thresh 参与第一轮匹配并新建 track
     * low:  low_thresh <= score < high_thresh 仅参与第二轮匹配 */
    std::vector<int> high_idx;
    std::vector<int> low_idx;
    for (size_t i = 0; i < detections.size(); ++i) {
        const float score = detections[i].confidence;
        if (score >= config_.high_thresh) {
            high_idx.push_back(static_cast<int>(i));
        } else if (score >= config_.low_thresh) {
            low_idx.push_back(static_cast<int>(i));
        }
    }

    /* Step 2: 所有 track 的 Kalman 预测 */
    for (size_t i = 0; i < tracklets_.size(); ++i) {
        Tracklet& t = tracklets_[i];
        const float* s = t.kf.predict();
        t.pred_h = s[3];
        t.pred_w = s[2] * t.pred_h;
        t.pred_x = s[0] - t.pred_w * 0.5f;
        t.pred_y = s[1] - t.pred_h * 0.5f;
    }

    std::vector<BoxRef> track_boxes;
    track_boxes.reserve(tracklets_.size());
    for (size_t i = 0; i < tracklets_.size(); ++i) {
        BoxRef b;
        b.x = tracklets_[i].pred_x;
        b.y = tracklets_[i].pred_y;
        b.w = tracklets_[i].pred_w;
        b.h = tracklets_[i].pred_h;
        track_boxes.push_back(b);
    }

    /* Step 3: 第一轮匹配 (高分框 ↔ 全部 track) */
    std::vector<BoxRef> high_boxes;
    high_boxes.reserve(high_idx.size());
    for (size_t i = 0; i < high_idx.size(); ++i) {
        const Detection& d = detections[high_idx[i]];
        BoxRef b;
        b.x = d.x;
        b.y = d.y;
        b.w = d.w;
        b.h = d.h;
        high_boxes.push_back(b);
    }
    MatchResult first =
        match_boxes(track_boxes, high_boxes, config_.match_thresh);

    std::vector<char> track_matched(tracklets_.size(), 0);
    for (size_t i = 0; i < first.matches.size(); ++i) {
        const int t = first.matches[i].first;
        const int d = high_idx[first.matches[i].second];
        apply_detection(tracklets_[t], detections[d]);
        track_matched[t] = 1;
    }

    /* Step 4: 第二轮匹配 (低分框 ↔ 第一轮未匹配的 track) */
    std::vector<BoxRef> remaining_boxes;
    std::vector<int> remaining_tracks;
    for (size_t i = 0; i < first.unmatched_tracks.size(); ++i) {
        const int t = first.unmatched_tracks[i];
        if (track_matched[t]) {
            continue;
        }
        remaining_tracks.push_back(t);
        BoxRef b;
        b.x = tracklets_[t].pred_x;
        b.y = tracklets_[t].pred_y;
        b.w = tracklets_[t].pred_w;
        b.h = tracklets_[t].pred_h;
        remaining_boxes.push_back(b);
    }

    std::vector<BoxRef> low_boxes;
    low_boxes.reserve(low_idx.size());
    for (size_t i = 0; i < low_idx.size(); ++i) {
        const Detection& d = detections[low_idx[i]];
        BoxRef b;
        b.x = d.x;
        b.y = d.y;
        b.w = d.w;
        b.h = d.h;
        low_boxes.push_back(b);
    }
    MatchResult second = match_boxes(remaining_boxes, low_boxes,
                                     kSecondMatchDistance);
    for (size_t i = 0; i < second.matches.size(); ++i) {
        const int t = remaining_tracks[second.matches[i].first];
        const int d = low_idx[second.matches[i].second];
        apply_detection(tracklets_[t], detections[d]);
        track_matched[t] = 1;
    }

    /* 未匹配 track: 标记 lost */
    for (size_t i = 0; i < tracklets_.size(); ++i) {
        if (!track_matched[i]) {
            ++tracklets_[i].time_since_update;
        }
    }

    /* 未匹配的高分框: 新建 track */
    for (size_t i = 0; i < first.unmatched_dets.size(); ++i) {
        const int d = high_idx[first.unmatched_dets[i]];
        create_track(detections[d]);
    }

    /* 显示确认 (camerayolo.md 3.4): age 达到 track_thresh */
    for (size_t i = 0; i < tracklets_.size(); ++i) {
        Tracklet& t = tracklets_[i];
        if (!t.confirmed && t.age >= config_.track_thresh) {
            t.confirmed = true;
        }
    }

    /* 新目标提示 (camerayolo.md 3.4.8): track 从创建起稳定存在
     * (持续被观测) 超过 alert_stable_seconds 时, 每个 track_id
     * 仅触发一次回调; confirmed 状态不回退, 目标短暂丢失后找回
     * 不会重复提示 */
    if (config_.alert_on_new_target &&
        new_target_callback_ != NULL) {
        const std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now();
        for (size_t i = 0; i < tracklets_.size(); ++i) {
            Tracklet& t = tracklets_[i];
            if (t.alerted || t.time_since_update != 0) {
                continue; /* 已提示过 / 本帧无观测 */
            }
            const float alive =
                std::chrono::duration<float>(now - t.first_seen)
                    .count();
            if (alive >= config_.alert_stable_seconds) {
                t.alerted = true;
                new_target_callback_(t.track_id,
                                     new_target_user_data_);
            }
        }
    }

    /* 删除丢失超限的 track */
    std::vector<Tracklet> kept;
    kept.reserve(tracklets_.size());
    for (size_t i = 0; i < tracklets_.size(); ++i) {
        if (tracklets_[i].time_since_update <=
            config_.track_buffer) {
            kept.push_back(tracklets_[i]);
        }
    }
    tracklets_.swap(kept);

    /* 输出: 已确认且本帧有观测的 track (丢失期间不输出) */
    std::vector<TrackedObject> output;
    for (size_t i = 0; i < tracklets_.size(); ++i) {
        const Tracklet& t = tracklets_[i];
        if (!t.confirmed || t.time_since_update != 0) {
            continue;
        }
        TrackedObject o;
        o.track_id = t.track_id;
        o.x = t.x;
        o.y = t.y;
        o.w = t.w;
        o.h = t.h;
        o.confidence = t.confidence;
        o.class_id = t.class_id;
        o.vx = t.vx;
        o.vy = t.vy;
        o.age = t.age;
        o.confirmed = true;
        o.trail = t.trail;
        output.push_back(o);
    }
    return output;
}
