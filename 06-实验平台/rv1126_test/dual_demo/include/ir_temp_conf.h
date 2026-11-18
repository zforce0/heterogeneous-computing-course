#pragma once

// ============================================================================
// 红外温度定标常量(阶段0探针产出)
//
// 编码结论(情形 A — 辐射测温可用):
//   组合流 256x384 YUY2 经 raw_data_cut 切出的下半段为 Y16 定点温度,
//   每像素 uint16, 单位 K*64 (0.015625 K/LSB)。
//
// 依据(twocameraOnMAC 项目两处互相印证):
//   1) src/temperature.cpp: temp_value_converter() = raw/64.0 - 273.15
//   2) src/ir_camera.cpp : gain_switch_detect 阈值 (130+273.15)*16*4 = K*64
//
// 板端快速验证(见 docs/ir_temp_probe_report.md): 室温场景整帧均值应在
// 15~35 ℃ 量级, 方向单调性符合预期。
// ============================================================================

#define IR_TEMP_LSB_PER_K 64.0    // 每 K 的 LSB 数
#define IR_TEMP_K_OFFSET  273.15  // 开尔文零点偏移

// Y16 原始值 → 摄氏度
inline float ir_raw_to_celsius(uint16_t raw)
{
    return static_cast<float>(
        static_cast<double>(raw) / IR_TEMP_LSB_PER_K - IR_TEMP_K_OFFSET);
}

// 摄氏度 → Y16 原始值
inline uint16_t ir_celsius_to_raw(float celsius)
{
    double k = static_cast<double>(celsius) + IR_TEMP_K_OFFSET;
    if (k < 0.0) {
        k = 0.0;
    }
    return static_cast<uint16_t>(k * IR_TEMP_LSB_PER_K + 0.5);
}
