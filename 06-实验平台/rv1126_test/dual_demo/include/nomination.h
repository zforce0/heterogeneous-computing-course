#pragma once

#include <cstdint>
#include <string>
#include <opencv2/core.hpp>

// ============================================================================
// 提名(Nomination)数据结构 — 差分显著层的输出 / YOLO 级联的输入
// 参见实现方案 §4.5
// ============================================================================

// YOLO 语义槽:只记录厂商模型的输出,不代表本系统的判定
struct SemanticSlot {
    std::string label;       // YOLO 类名;"none" = crop 内检不出(正常结果)
    float score = 0.0f;      // YOLO 置信度(仅是模型输出)
    std::string filled_by;   // "vendor_default" | "customer_model" | "none"
};

struct Nomination {
    uint64_t id = 0;             // 全局自增
    double t_emit_ms = 0.0;      // 事件产生时刻(CLOCK_MONOTONIC_RAW, ms)
    uint64_t ts_pair_us = 0;     // 配对 IR 帧时间戳(us)
    uint64_t ts_vis_us = 0;      // 配对 VIS 帧时间戳(us, 主线程取帧用)
    int quadrant = 0;            // 1 / 2

    // --- 几何(IR 网格坐标) ---
    cv::Rect box_ir;             // IR 上的 bbox
    cv::Point2f centroid_ir;     // IR 质心
    int area_px = 0;             // IR 像素数
    cv::Rect roi_vis;            // 映射到可见光并加 margin 后的 crop 区域

    // --- 温度属性(℃, 情形 A 辐射测温) ---
    float t_max = 0.0f;          // 区内最高温(观测值)
    float t_p99 = 0.0f;
    float t_ref = 0.0f;          // 区外环背景温度中位数
    float delta_t = 0.0f;        // t_max - t_ref, 最有价值的单个数字
    float thermal_gradient = 0.0f; // 区内最大梯度 K/px(近似)
    float t_lower_bound = 0.0f;  // 保守下界 = t_max(读数只会偏低不会偏高)
    float t_true_est = 0.0f;     // SSE 修正后的真值估计

    // --- 显著性(归一化 [0,1] 的均值) ---
    float sal_tir_mean = 0.0f;
    float sal_rgb_mean = 0.0f;
    float sal_diff = 0.0f;       // sal_tir_mean - sal_rgb_mean

    // --- 时序(P2) ---
    int track_id = -1;
    float persistence_s = 0.0f;  // 持续存在时长
    float dT_dt = 0.0f;          // K/min, 滑动线性拟合
    float t_std = 0.0f;          // t_max 时序标准差(活体判据之一)
    float micro_motion = 0.0f;   // VIS crop 内帧间差均值(活体判据之二)
    float score_living = 0.0f;   // 活体打分(yard 模式)

    // --- 健康 ---
    float scene_contrast_k = 0.0f; // 场景热对比度 = P99-P1 of T
    int crossover_risk = 0;       // 0/1/2 低/中/高
    int valid = 1;                // 1 ok / 0 degraded / -1 invalid
    std::string invalid_reason;   // "nuc_freeze"/"crossover"/"low_contrast"

    // --- 语义(主线程 YOLO 确认后填充) ---
    SemanticSlot semantic;
};
