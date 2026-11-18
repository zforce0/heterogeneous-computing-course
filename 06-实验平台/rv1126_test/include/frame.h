#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

// BGR24 连续帧,带全链路计时信息
struct BgrFrame {
    int width = 0;
    int height = 0;
    int stride = 0;
    uint64_t timestamp_us = 0;   // 采集/解码完成时刻(单调时钟,微秒)
    uint64_t sequence = 0;
    std::vector<uint8_t> data;

    bool valid() const
    {
        return width > 0 && height > 0 && stride >= width * 3 &&
               data.size() >= static_cast<size_t>(stride) * height;
    }

    void resize(int new_width, int new_height)
    {
        width = new_width;
        height = new_height;
        stride = new_width * 3;
        data.resize(static_cast<size_t>(stride) * height);
    }
};

// ---- 帧率信息(主线程每 2s 打印/退出汇总)----
struct FrameRateInfo {
    double cur_fps = 0.0;   // 最近 2s 窗口帧率
    double avg_fps = 0.0;   // 启动以来平均帧率
    uint64_t total = 0;     // 累计帧数
};

// 帧率计数器:2s 滑动窗口 + 启动以来平均
// tick 的时间参数统一来自 perf_clock.h 的 now_ms()
struct FpsCounter {
    void tick(double now_ms_)
    {
        if (total == 0) {
            start_ms = now_ms_;
        }
        ++total;
        window.push_back(now_ms_);
        prune(now_ms_);
    }

    FrameRateInfo info(double now_ms_) const
    {
        FrameRateInfo out;
        out.total = total;
        // 窗口内保留近 2s
        double newest = window.empty() ? now_ms_ : window.back();
        size_t cnt = 0;
        for (size_t i = window.size(); i-- > 0; ) {
            if (newest - window[i] <= 2000.0) {
                ++cnt;
            } else {
                break;
            }
        }
        double span = newest - (window.empty() ? start_ms : window[0]);
        out.cur_fps = cnt > 1 && span > 0 ? (cnt - 1) * 1000.0 / span : 0.0;
        double since_start = now_ms_ - start_ms;
        out.avg_fps = total > 1 && since_start > 0
                          ? (total - 1) * 1000.0 / since_start
                          : 0.0;
        return out;
    }

    uint64_t total = 0;
    double start_ms = 0.0;
    std::deque<double> window;

private:
    void prune(double now_ms_)
    {
        while (window.size() > 1 && now_ms_ - window.front() > 2000.0) {
            window.pop_front();
        }
    }
};

// 近期帧环形缓冲:用于跨相机最近邻配对
struct FrameRing {
    explicit FrameRing(size_t capacity_ = 8) : capacity(capacity_) {}

    void push(const BgrFrame& frame)
    {
        frames.push_back(frame);
        while (frames.size() > capacity) {
            frames.pop_front();
        }
    }

    // 在环形缓冲中选取与 timestamp_us 时间差最小的帧
    bool nearest(uint64_t timestamp_us, BgrFrame& out,
                 double& delta_ms) const
    {
        if (frames.empty()) {
            return false;
        }
        size_t best = 0;
        int64_t best_delta = INT64_MAX;
        for (size_t i = 0; i < frames.size(); ++i) {
            int64_t d = static_cast<int64_t>(frames[i].timestamp_us) -
                        static_cast<int64_t>(timestamp_us);
            if (d < 0) {
                d = -d;
            }
            if (d < best_delta) {
                best_delta = d;
                best = i;
            }
        }
        out = frames[best];
        delta_ms = best_delta / 1000.0;
        return true;
    }

    size_t capacity;
    std::deque<BgrFrame> frames;
};
