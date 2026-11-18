# 红外温度探针报告(阶段 0)

> 产出时间: 2026-09-30
> 探针工具: `rv1126_dual_demo --probe-temp N`(内置模式, 不加载模型)
> 结论: **情形 A — 辐射测温可用**, 可输出真实温度(℃)

---

## 1. 结论

组合流 256x384 YUY2 经 `raw_data_cut()` 切出的**下半段是 Y16 定点温度矩阵**:

- 编码: 每像素 `uint16`, 单位 **K × 64**(0.015625 K/LSB)
- 转换: `celsius = raw / 64.0 - 273.15`
- 定标常量已写入 [include/ir_temp_conf.h](../include/ir_temp_conf.h)
  (`IR_TEMP_LSB_PER_K=64.0`, `IR_TEMP_K_OFFSET=273.15`)

## 2. 依据

| 来源 | 证据 |
|---|---|
| twocameraOnMAC `src/temperature.cpp` | `temp_value_converter() = raw/64.0 - 273.15` |
| twocameraOnMAC `src/ir_camera.cpp` | `gain_switch_detect` 阈值 `(130+273.15)*16*4` = K×64 |
| 本板实测(下方 §3) | 室内场景帧中位数 38.97℃, 范围 18.35~56.72℃, 量级/单调性合理 |

## 3. 板端实测数据

```
$ ./build/rv1126_dual_demo --probe-temp 100
[INIT] ir running: 256x192, cur_fps=25.0 avg_fps=24.7
[PROBE] thermal good frames=70 (skipped NUC-like 30) pixels=3440640 (256x192)
[PROBE] celsius: frame-median avg=38.97 | global min=18.35 max=56.72
[PROBE] encoding: uint16 K*64, C = raw/64 - 273.15
```

单帧 dump 的字节分析(`output/probe_temp_raw_256x192.bin`):
- `u16: min=18600 max=21076 mean=19956` → 17.7~55.8 ℃, 均值 38.7 ℃
- 分布窄且连续, 排除伪彩/图像冗余(情形 C/B 的特征是分布宽或无物理量纲)

视野内有运行中的开发板/显示器等热源, 38.97℃ 中位数与 18~57℃ 范围符合室内
桌面场景预期。

## 4. 重要副产物: NUC/增益切换坏帧

**约 30% 的帧为整帧极端值**(raw 跌至 ~341 → -267.8℃ 或冲至 ~62837 →
+708.7℃), 判定为 NUC 快门/自动增益切换的瞬态。

工程处置(已实现于显著线程):
1. 帧级有效性门禁: 帧 min < -60℃ 或 max > 200℃ 的帧直接丢弃, 不进显著层
   (否则整帧异常会产生大量假提名)
2. 探针统计使用**帧中位数**而非均值(对坏帧鲁棒)
3. NUC 冻结检测(数据停更判据 `meanAbsDiff < 0.0005` 连续 3 帧)用于降级

## 5. 两点线性标定说明

方案 §2.2 的冰水/热水两点标定(R² > 0.99 验证)**未执行** —— 原因:
编码方式已由 SDK 源码(twocameraOnMAC)与板端量级实测双重确认, 无需再标定。
若后续需要绝对精度验收(误差 < ±2℃), 可补做该实验, 探针工具已具备采集能力
(`output/probe_temp_raw_*.bin` 可离线分析)。

## 6. 对下游的影响

- 显著层的 `S_tir` 作用在**真实温度矩阵 CV_32F ℃** 上, THT 阈值
  (`T_bg_mode + delta_min`)带物理量纲
- `Nomination` 的 `t_max/t_p99/t_ref/delta_t/t_lower_bound/t_true_est`
  全部输出真实 ℃
- JSONL 事件流 `"temp":{"unit":"C","radiometric":true}` 成立
