#pragma once

#include "nomination.h"

#include <cstdint>
#include <string>

// ============================================================================
// JSONL 事件流: 每个被 YOLO 确认后的提名一行, append 模式不覆盖
// 手写序列化, 无第三方依赖。字段契约见实现方案 §8.1
// ============================================================================

class EventLog {
public:
    bool open(const std::string& path);   // append 打开
    void close();

    // 场景上下文(q 计数与整帧温度中位数)由调用方提供
    void write(const Nomination& nom, float ir_temp_median, int q1_count,
               int q2_count, int q3_count, int vis_w, int vis_h);

    uint64_t count() const { return count_; }

private:
    FILE* file_ = nullptr;
    uint64_t count_ = 0;
};
