#pragma once

#include "nomination.h"
#include "saliency.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

// ============================================================================
// 提名器: 对显著结果做连通域分析 → 属性包 → 时序链接 → 活体打分 → 有界队列
// 参见实现方案 §4.5 / §4.7 / §5.2
// ============================================================================

// 有界队列: 满时丢弃最旧(背压策略必须是丢弃, 不能阻塞 — 保护采集线程)
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity) : cap_(capacity) {}

    void push(const T& v)
    {
        std::lock_guard<std::mutex> lock(m_);
        if (q_.size() >= cap_) {
            q_.pop_front();
            ++dropped_;
        }
        q_.push_back(v);
        cv_.notify_one();
    }

    // 带超时的弹出;返回 false 表示超时(运行中无数据)
    bool pop(T& v, int timeout_ms, bool running)
    {
        std::unique_lock<std::mutex> lock(m_);
        while (q_.empty() && running) {
            if (cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms)) ==
                std::cv_status::timeout) {
                return false;
            }
        }
        if (q_.empty()) {
            return false;
        }
        v = q_.front();
        q_.pop_front();
        return true;
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(m_);
        return q_.size();
    }

    uint64_t dropped() const
    {
        std::lock_guard<std::mutex> lock(m_);
        return dropped_;
    }

private:
    size_t cap_;
    std::deque<T> q_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    uint64_t dropped_ = 0;
};

struct NominatorConfig {
    std::string card = "inspect";   // inspect(①为主) | yard(②+活体) | all
    int min_area = 8;               // 提名最小像素数(IR px)
    // 时序(P2)
    int link_radius = 8;            // 帧间关联半径(IR px)
    int history_len = 32;           // 轨迹历史帧数(≈1.3s @25fps)
    // 活体打分(yard)
    float body_lo = 26.0f;          // 表观体温带下限 ℃
    float body_hi = 40.0f;          // 上限 ℃
    float dt_thr = 0.5f;            // |dT/dt| 上限 K/min
    float motion_thr = 0.5f;        // micro_motion 阈值(灰度差)
    float tstd_thr = 0.05f;         // t_std 阈值 K
};

// 时序轨迹(质心最近邻链接)
struct Track {
    int id = -1;
    cv::Point2f centroid;
    uint64_t last_ts_us = 0;
    double first_ms = 0.0;          // 首次出现时刻(ms)
    std::deque<float> t_history;    // t_max 历史
    double last_update_ms = 0.0;
    double last_nominate_ms = -1.0e9;   // 上次被提名时刻(同轨迹节流)
};

class Nominator {
public:
    struct Stats {
        uint64_t tir_only = 0;   // 单模态热提名累计(连通域数, 组件口径)
        uint64_t q1 = 0;
        uint64_t q2 = 0;
        uint64_t q3 = 0;         // q3 只计数不提名
        // 像素口径(组件碎片化会使 q1>tir_only, 像素包含关系恒成立)
        uint64_t tir_px = 0;
        uint64_t q1_px = 0;
    };

    void init(const NominatorConfig& cfg);

    // 对一帧显著结果产出提名
    //   T_c        温度矩阵(℃) — 温度属性
    //   G_small    VIS 灰度(IR 网格) — micro_motion 当前帧
    //   G_prev     上一帧 G_small(可为空) — micro_motion 差分
    //   reg        配准(roi_vis 换算)
    //   ts_pair_us / ts_vis_us 配对时间戳
    //   t_now_ms   当前时刻(时序统计)
    //   out        本帧被接受的提名
    void process(const Saliency::Output& sal, const cv::Mat& T_c,
                 const cv::Mat& G_small, const cv::Mat& G_prev,
                 const class Registration& reg, uint64_t ts_pair_us,
                 uint64_t ts_vis_us, double t_now_ms,
                 std::vector<Nomination>& out);

    const Stats& stats() const { return stats_; }
    const NominatorConfig& config() const { return cfg_; }

private:
    // 从 mask 连通域产出组件(bbox 已按面积过滤), 提取温度/显著性属性
    void extract_components(const cv::Mat& mask, int quadrant,
                            const Saliency::Output& sal, const cv::Mat& T_c,
                            const cv::Mat& G_small, const cv::Mat& G_prev,
                            const Registration& reg, uint64_t ts_pair_us,
                            uint64_t ts_vis_us, double t_now_ms,
                            std::vector<Nomination>& out);

    int link_track(const cv::Point2f& centroid, double t_now_ms,
                   uint64_t ts_us);

    NominatorConfig cfg_;
    Stats stats_;
    std::vector<Track> tracks_;
    uint64_t next_id_ = 1;
    int next_track_id_ = 1;
};
