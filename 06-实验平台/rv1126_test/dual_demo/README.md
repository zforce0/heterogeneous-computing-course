# RV1126B 双模态差分显著 + YOLO 级联 Demo

> 基于 rv1126_test 第二期(双相机拉流+YOLOv8)增量开发, 原代码未改动。
> 方案: [RV1126B双光差分显著+YOLO级联Demo实现方案.md](../RV1126B双光差分显著+YOLO级联Demo实现方案.md)
> 阶段0温度探针报告: [docs/ir_temp_probe_report.md](docs/ir_temp_probe_report.md)

## 一句话

红外只做"看这里"(差分显著提名), 可见光只做"认"(crop → YOLO 确认),
把 NPU 调用从逐帧全图压缩到事件驱动。

## 构建与运行(板端)

```bash
./deploy.sh                 # 开发机同步源码+模型并板端编译
# 板端:
cd /userdata/dual_demo
./run.sh                                    # 默认: auto 模式 + inspect 卡
./build/rv1126_dual_demo ./yolov8m_rv1126b.rknn --help   # 全部参数
```

## 模式

| 命令 | 行为 |
|---|---|
| `--mode auto`(默认) | 级联 + 优雅降级(热交叉/NUC 冻结时自动切逐帧, 恢复后切回) |
| `--mode cascade` | 纯级联(不降级) |
| `--mode baseline` | 逐帧全图 YOLO(A/B 对照基准, 行为等同第二期) |
| `--card inspect`(默认) | Q1(热显著+视觉平淡)提名 — 巡检 |
| `--card yard` | Q2 + 活体打分提名 — 庭院 |
| `--probe-temp N` | 阶段0温度探针(采 N 帧统计后退出) |

## 输出物

| 路径 | 内容 |
|---|---|
| `events.jsonl` | 每个被 YOLO 确认的提名一行(温度/显著性/时序/健康/语义, append 不覆盖) |
| `output/viz_*.jpg` | 六宫格(留 10 张): VIS+提名框 / IR 温度 JET / S_rgb / S_tir / 差分图(红=热显著,蓝=视觉显著) / 四象限着色(红①蓝②黄③) |
| `output/det_*_vis/_ir.jpg` | 提名成对图(留 10 对, VIS 画 roi 框, IR 画 bbox) |

## 日志速读

```
[SAL] q1=.. q2=.. q3=.. tir_only=.. contrast=..K sal=..ms   ← 显著层(2s)
[EVT] id=.. quad=Q1 tmax=.. est=.. dT=+..K area=..px        ← 提名
[NPU] id=.. crop=WxH infer=..ms obj=.. label=..             ← YOLO 确认(label=none 是正常结果)
[DEGRADE]/[RECOVER]                                         ← 降级链
[STAT] >>> FAR / >>> NPU calls                              ← 判决指标(退出汇总)
```

## 配准标定(相机被动过之后需重做)

参考 twocameraOnMAC 流程:
1. 拍成对图(可见光 640x480 + 红外 256x192)
2. `test/camera_capture_test/calib_web.py` 网页选 ≥4 对同名点 → 拖点精调
3. 保存 `calibration.txt`(quad 8 值, 本项目直接读取)
无标定文件时自动退化为 FOV 缩放(3 倍 margin 可吸收偏差, 不影响 FAR)。

## 已知边界(实测)

- **FAR 强场景依赖**: 室内静态场景(热源视觉平淡)削减有限(~12%);
  FAR 的设计测法是方案 §9 的 S1-S3 受控场景(晒热路面/隐藏热源)
- `sal_ms` ≈ 13ms(目标 5ms): 显著线程独立于采集, 不影响双路 25fps
- `skip_align` 计数含 NUC 坏帧门禁丢弃(实测坏帧率 ~30% 属传感器特性)
- auto 模式降级期间提名排队, 恢复后集中消费, align 会短暂升高(预期)
