# 互联异构计算 · 课程材料平台

面向《多核互联异构计算技术及应用》的实验教学工程。它把课堂上讲的**互联、异构、流水线、量化、Roofline、性能评估**做成一套可观察、可复现、可追问的实机装置：

- **Mac**：只做显示与控制（实时控制台 `127.0.0.1:8899`），不承担推理；
- **昇腾 QiZhi01-20T**：中心 LLM，llama.cpp OpenAI 兼容 API `:8080`；
- **RK3588**：G3 证据服务 `:9100` + 板端 dashboard `:8081`，做事件二次确认；
- **RV1126B**：双光采集 + 端侧 YOLOv8（RKNN）+ 热成像热斑/NPU 推理，视觉服务 `:8082`；
- 四节点通过 `192.168.77.0/24` 星型实验网互联（地址可改，见 `config/lab.env.example`）。

> 本工程是**课程材料与实验记录**，不是商业产品。所有 KPI 来自实机命令输出；未验证项标 `planned` / `N/A`，不填默认 0。归档关键帧与实时流是两个不同时间尺度的数据，页面明确区分。

---

## 快速开始（三档复现）

| 档位 | 适用 | 需要硬件 | 做法 |
|---|---|---|---|
| A · 看快照 | 备课/预习/无设备 | 无 | 直接打开 `eval_platform/dashboard/g4_live.html`，看构建期实测快照 |
| B · 跑服务 | 检查平台本身 | 仅 Mac/Linux | `make doctor` → `make page` → `make run` → 打开 `http://127.0.0.1:8899/` |
| C · 全链路 | 完整实验 | 四节点 + 交换机 | 先按 `docs/01-environment.md` 配网与模型，再按 `docs/02-deployment.md` 启动 |

```bash
python3 --version                       # 3.8+
make doctor                             # 只读自检：源码/端口/远端服务
cp config/lab.env.example config/lab.env  # 换网段时再改；不改也与原实验一致
make page                               # 用归档实测 JSON 重建页面
make run                                # 前台运行，Ctrl-C 结束
```

核心平台**不依赖 pip 第三方包**（Python 标准库实现 HTTP/SSE/MJPEG/SSH 编排）。可选 ML 工具另见 `requirements-ml.txt`。

---

## 文档导航

| 文档 | 内容 |
|---|---|
| `docs/01-environment.md` | 硬件清单、网络规划、端口表、软件/模型版本、环境变量、目录结构 |
| `docs/02-deployment.md` | 三档部署流程、板端 systemd 服务、换机迁移步骤、验收清单 |
| `docs/03-user-guide.md` | 控制台每个区域怎么用、API 一览、两类问答口径、抓拍与控制 |
| `docs/04-principles.md` | Roofline 与 LLM 解码、量化、流水线调度、证据协议、事件抽取、接地问答 |
| `docs/05-course-map.md` | **7 讲 lecture + 4 个 lab** 与工程文件/演示命令逐项对照 |
| `docs/06-reproduce-and-verify.md` | 数据归档、复现实验、校验方法、时间/时钟口径 |
| `docs/07-troubleshooting.md` | 常见故障：设备离线、USB 热成像、NPU 后端、0.6B 输出、端口冲突 |
| `docs/08-界面效果说明.md` | 6 张截图逐项对照「每个部分跑对了应长什么样」及跑错迹象 |
| `ARCHIVE_NOTICE.md`（打包后生成） | 包内有意排除哪些文件、如何取回模型 |

---

## 课程结构速览

**Lecture（7 讲）**：异构计算导论 · 互联与通信基础 · 体系结构与 Roofline · 并行编程与流水线调度 · 模型量化与短策推理 · 性能评估与试验方法论 · 课程复盘与开放命题工作坊

**Lab（4 个）**：环境与链路核验（M1）· 双光谱采集（M2）· 异构推理（M3）· 开放命题（M4）

> Lab 名称与课程规划 V6.4 的四教具包一致。逐讲/逐实验的对应关系见 `docs/05-course-map.md`、`docs/08-界面效果说明.md`（截图对照）。

---

## 打包与上传 GitHub

```bash
make package          # 生成 dist/course-kit/ 与 tar.gz / zip（脱敏、删大二进制）
make verify           # 校验 SHA256、语法、脱敏，并在临时目录模拟解包重建
```

把 `dist/course-kit/` **整个目录**作为新仓库根目录上传即可；包内已有 `.gitignore`、`README.md`、`MANIFEST.json`。原始工程文件不会被打包脚本修改。

---

## 目录结构（原工程）

```
.
├── config/                      # lab.env.example：可移植地址/端口/SSH 别名模板
├── docs/                        # 本套课程文档
├── scripts/                     # 自检、打包、验收、缓存清理
├── eval_platform/
│   ├── config/                  # lab_config.py + 异构设置 YAML
│   ├── dashboard/               # 实时控制台、MJPEG/SSE 服务、页面生成器
│   ├── llm/                     # LLM 基准、关键帧构建、LAN 服务（历史基线）
│   ├── tools/                   # 证据协议、拓扑/节点探测、端侧检测器源码、编排
│   └── runs/                    # 带 run_id 的实测归档（report.md 等）
├── rv1126_test/                 # RV1126B 双光 Demo、USB 带宽分析、SSH 初始化
├── twocameraOnMAC/              # 早期双摄采集/配准工程（源码与过程记录）
├── yolov8_detect_C_demo/        # RKNN C API 检测示例
└── 互联异构计算实验设置_V1.{2,3}.md
```
