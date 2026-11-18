# 05 · 课程与工程对照表

课程共 **7 讲 lecture + 4 个 lab**。下表把每一讲/每个实验落到本工程的具体文件、演示入口和可观察量上，便于教师备课与同学复现。

> Lab 名称与课程规划 V6.4 的四教具包一致：M1 环境与链路核验 / M2 双光谱采集 / M3 异构推理 / M4 开放命题。

## 5.1 Lecture 对照

| # | Lecture | 核心概念 | 工程对应（文件/模块） | 课堂演示入口 |
|---|---|---|---|---|
| L1 | 异构计算导论 | CPU/NPU/加速器分工；为什么 TOPS 不是 LLM 指标 | `build_g4_dashboard.py` 的节点角色；G4 report §2/§15 | 控制台「节点分工」卡；`curl /api/roster?refresh=1` |
| L2 | 互联与通信基础 | 星型拓扑、链路介质、RTT、TCP 长度前缀协议 | `tools/discover_topology.py`、`evidence_{server,sender}.py`、`recheck_lab_link.sh` | 页面「拓扑」SVG + 接口证据表；G3 report §3–§5 |
| L3 | 体系结构与 Roofline | 算术强度、带宽屋顶、算子与搬运开销 | `tools/infer_bench.cpp`、`convert_resnet50.py`、`docs/04-principles.md` §4.3 | G3 report §10/§12（RK3588 NPU/CPU/GPU 与 ResNet-50 量化） |
| L4 | 并行编程与流水线调度 | 采集→推理→传输→复核→显示；共享 NPU 排队；帧新鲜度 | `tools/yolo_live.cpp`、`tools/ir_detect.cpp`、`dashboard/camera_stream.py` | 双光同屏模型条与帧率；G4 honest_note 中 NPU 排队口径 |
| L5 | 模型量化与短策推理 | Q4 GGUF / INT8 RKNN / FP16；短输出与提示词约束 | `eval_platform/llm/Modelfile`、`tools/ascend_llm_service.sh`、`serve_g4.py` 的提示词与重试 | 对话区问「双光研判」；`POST /api/analyze/stream` |
| L6 | 性能评估与试验方法论 | 实测口径、run_id 归档、A/B 消融、N/A 原则 | `eval_platform/runs/*/report.md`、`tools/bench_llm_ascend.py`、`manifest.json` | `bash run_g4_pipeline.sh llm`；G3 E10 策略消融表 |
| L7 | 课程复盘与开放命题工作坊 | 从现象到命题：跨模态漏检、事件定义、降级输出 | `monitor_events()`、`_grounded()` 意图分流、页面两类问答角标 | 问两个「近一分钟」问题，对比答案格式；`GET /api/events` |

### 每讲建议的课堂任务

- **L1**：在「节点分工」卡上指出每个节点的角色；解释为什么 Mac 退出 LLM 角色后吞吐反而不是瓶颈。
- **L2**：用 `/api/topology` 找出每个接口的介质证据（`r8152+usb` / `rk_gmac-dwmac+platform`），说明为何端口对端只能标 `inferred`。
- **L3**：给定 0.6B Q4 499 MB 与盒上 ARM CPU，判断 LLM decode 落在屋顶哪一侧；再用 G3 的 GPU GEMM 数据讨论峰值算力与搬运开销。
- **L4**：观察热成像 NPU 每 2 帧一次、热斑逐帧的调度，解释为什么可见光 fps 没有下降。
- **L5**：故意把研判提示词改成「从正常/关注/异常里选一个」，观察 0.6B 照抄整句，理解为什么最后用后端成品行。
- **L6**：对比 ping 判据 bug 修复前后的结论，讨论「测量方法本身也会产生假数据」。
- **L7**：让学生提出一个可测量命题（如「跨模态漏检与场景照度的关系」），指出需要新增哪些采样字段。

## 5.2 Lab 对照

### Lab 1 · 环境与链路核验（M1_lab_env）

**目标**：建立实验网、确认四节点可达、学会区分「配置错」与「硬件没上电」。

- 文件：`config/lab.env.example`、`scripts/doctor.py`、`eval_platform/tools/{recheck_lab_link,finalize_lab_setup,check_lab_network}.py`
- 步骤：`make doctor` → 配 `lab.env` → `recheck_lab_link.sh` → `ssh <alias> hostname` → 看 `/api/topology`
- 可观察量：IP/MAC、RTT、接口速率与载波、SSH 别名是否可用
- 历史案例（课堂讨论）：RV1126B 静态地址误配在 `eth1`；RK 遗留 proxy ARP 劫持 `.22`；`0.0% packet loss` 子串误报

### Lab 2 · 双光谱采集（M2_dual_spectrum）

**目标**：理解可见光与热成像在接口、帧率、标定上的差异。

- 文件：`eval_platform/dashboard/camera_stream.py`、`tools/{yolo_live,ir_detect}.cpp`、`tools/build_edge_detectors.sh`、`rv1126_test/USB带宽约束分析.md`
- 步骤：板上编译两个检测器 → `systemctl is-active camera-preview yolo-live ir-detect` → 页面看双光流与模型条 → 抓拍 `/api/snapshot`
- 可观察量：VIS ~7.5 fps、IR ~5.0 fps、NPU 单帧 77 / 110–130 ms、热斑数/帧、模型名切换
- 口径：热成像是 0~255 相对灰度，无温度；厂商 SDK 独占 usbfs，不创建 `/dev/video` 节点；归档关键帧与实时流方向不同，不可直接叠加

### Lab 3 · 异构推理（M3_hetero_infer）

**目标**：跑通端侧推理 + 跨板证据协议 + 中心 LLM 语义确认的完整链路。

- 文件：`tools/{evidence_sender,evidence_server,rk_llm_reconfirm,bench_llm_ascend}.py`、`tools/ascend_llm_service.sh`、`run_g4_pipeline.sh`
- 步骤：`run_g4_pipeline.sh check` → `llm` → `rk` → 看 G3 report 的 sent/ACK/duplicate 对账
- 可观察量：LLM decode 中位 4.04 tok/s（昇腾 ARM）、RTT 0.82 ms、策略消融带宽 -24.1%、二次确认率 99.2%
- 讨论：为什么极限场景要用单调钟 + RTT 而不是墙钟；为什么 RK 侧先做规则版复核

### Lab 4 · 开放命题（M4_co_eval）

**目标**：在已有可观测面上提出并验证自己的命题。

- 文件：`serve_g4.py` 的 `monitor_events()` / `_grounded()` / `compare_line()` / `verdict()`、页面两类问答角标
- 起点命题示例：
  1. 热成像跨模态漏检率是否随可见光置信度上升而下降？
  2. 事件去抖参数（连续 2 次进入 / 8 s 离开 / 0.40 门限）如何改变事件数与抖动？
  3. 把研判判据从「就绪率 + 漏检」换成学生自定义规则，如何保证结论可核对？
- 交付要求：新增字段要有来源；未测写 `N/A`；结论必须能被 `/api/events` 或 SSE 采样复核；不能让 LLM 自己算数

## 5.3 一页式对照（可投影）

| 课程概念 | 页面上看哪里 | 后端文件 |
|---|---|---|
| 异构分工 | 节点分工卡 | `tools/node_roster.py` |
| 互联介质 | 拓扑卡接口表 | `tools/discover_topology.py` |
| Roofline | LLM tok/s 与模型/后端说明 | `ascend_llm_service.sh`、`npu_attempt.md` |
| 流水线调度 | 双光 fps 与 NPU 推理间隔 | `yolo_live.cpp`、`ir_detect.cpp` |
| 量化 | 模型条文件名（`.rknn` / GGUF Q4） | `convert_resnet50.py`、`Modelfile` |
| 性能评估 | 事件/曲线/G3 归档表 | `bench_llm_ascend.py`、`runs/*/report.md` |
| 开放命题 | 两类问答的事实条与角标 | `monitor_events()`、`analysis_prompt()` |
