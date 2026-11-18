# 03 · 教具包

4 个教具化模块包，与学生 4 次大作业一一对应。每个包统一 6 目录结构：`README.md` + `config/` + `baseline/` + `student/` + `selftest/` + `runs/`。

| 教具包 | 对应实验 | 权重 | 主题 |
|---|---|---|---|
| `M1_lab_env/` | 实验 1 环境与链路 | 15% | 网络核验、TCP 探测、manifest |
| `M2_dual_spectrum/` | 实验 2 双光谱采集 | 20% | 双路并发、USB 带宽预算 |
| `M3_hetero_infer/` | 实验 3 异构推理 | 25% | 双 runtime、九段计时、分段时延 |
| `M4_co_eval/` | 实验 4 开放命题 | 40% | 证据协议、有界队列、结果包 |

## 通用目录结构

| 目录 | 状态 | 说明 |
|---|---|---|
| `README.md` | 给定 | 30 秒跑通、实机拓扑、依赖、安全约束 |
| `config/` | 给定 + 留白 | `lab_given.yaml`（拓扑+哈希，禁改）、`student.yaml`（填 run_id） |
| `baseline/` | 给定·完整 | 可运行最小基线，禁止修改 |
| `student/` | 留白·骨架 | `todo_required.py`（必做）/ `todo_optional.py`（选做） |
| `selftest/` | 给定 | `run_demo.py`（学生自测）/ `teacher_demo.py`（教师演示） |
| `runs/` | 输出 | 结果包（与测评包输入契约一致） |

## 30 秒跑通（sim 模式，无需硬件）

```bash
cd M1_lab_env && python3 selftest/run_demo.py     # 学生自测
cd M1_lab_env && python3 selftest/teacher_demo.py # 教师三场景演示
```

四个包默认 sim 模式覆盖 5 种状态（PASS/WARN/FAIL/SKIP/N/A）；`--mode live` 演示无板卡时的 N/A 口径；`--student-solved` 演示补齐后全 PASS。

## 状态字典

| 状态 | 含义 | 评分影响 |
|---|---|---|
| PASS | 基线/校验通过 | 基础分 70 自动核验 |
| WARN | 异常场景正确复现 | 报告需写处置与结论边界 |
| FAIL | 必做 TODO 未实现 | 补齐转 PASS，否则扣基础分 |
| SKIP | 选做 TODO 未启用 | 只影响排名分 |
| N/A | 无硬件或未埋点 | 必须 null/N/A，禁止填 0 |

> 口径红线：分段 P95 相加 ≠ 端到端 P95；跨板只报同钟 RTT；统一控制端 UTC。详见各包 README 与 `../04-实验手册/`。
