# 互联异构计算 · 课程材料平台

面向《多核互联异构计算技术及应用》的实验教学工程。它把课堂上讲的**互联、异构、流水线、量化、Roofline、性能评估**做成一套可观察、可复现、可追问的实机装置：

- **Mac**：只做显示与控制（实时控制台 `127.0.0.1:8899`），不承担推理；
- **昇腾 QiZhi01-20T**：中心 LLM，llama.cpp OpenAI 兼容 API `:8080`；
- **RK3588**：G3 证据服务 `:9100` + 板端 dashboard `:8081`，做事件二次确认；
- **RV1126B**：双光采集 + 端侧 YOLOv8（RKNN）+ 热成像热斑/NPU 推理，视觉服务 `:8082`；
- 四节点通过 `192.168.77.0/24` 星型实验网互联（地址可改，见 `config/lab.env.example`）。

> 本工程是**课程材料与实验记录**，不是商业产品。所有 KPI 来自实机命令输出；未验证项标 `planned` / `N/A`，不填默认 0。归档关键帧与实时流是两个不同时间尺度的数据，页面明确区分。

---

## 部署与启动指南

按你手上有什么，选一档开始。三档对页面显示的影响不同，务必先分清。

| 档位 | 适用 | 需要的硬件 | 页面能看到什么 |
|---|---|---|---|
| **A · 归档快照** | 备课、预习、无设备 | 无 | 关键帧、节点角色、实测 KPI（构建期快照）；**无实时流** |
| **B · 单机控制台** | 检查平台本身、演示 UI | 仅 Mac/Linux | 页面 + SSE 曲线 + 事件流；远端节点会显示 offline |
| **C · 四节点全链路** | 完整实验 | 四节点 + 实验网 | 真实双光流、端侧检测、中心 LLM 问答 |

### 一键启动（最省事）

在**工程根目录**执行一条命令即可——自动清理旧进程、后台启动、健康检查、打开浏览器，并打印节点连通性诊断：

```bash
./start.sh                 # 重启控制台并打开浏览器（等价 make start）
./start.sh --no-open       # 启动但不打开浏览器
./start.sh --rebuild       # 先重建页面再启动
./start.sh status          # 查看状态（PID + /api/state + 日志尾部）
./start.sh stop            # 停止控制台
./start.sh 8900            # 指定端口
```

`start.sh` 会输出形如 `✓ 控制台就绪: http://127.0.0.1:8899/`，并列出 RV1126B / 昇腾的在线离线状态。**节点离线只说明实验网没接，服务本身是正常的**——排障见文末。服务日志在 `/tmp/g4-dashboard-<端口>.log`。

> 默认用 `nohup` 后台运行（关掉终端仍继续）。若在受限沙箱里服务随命令结束被回收，改用前台 `python3 eval_platform/dashboard/serve_g4.py --port 8899`。


### 环境要求

- Python **3.8+**（核心平台只用标准库，实现 HTTP/SSE/MJPEG/SSH 编排，**无需 pip 安装**；可选 ML 工具见 `requirements-ml.txt`）
- macOS 或 Linux；档位 B 只需本机，档位 C 需要四节点在同一实验网
- `make` 只是便捷封装，没有 `make` 时用括号里的等价命令

---

### 档位 A · 只看归档快照（无需硬件/网络）

```bash
# macOS
open eval_platform/dashboard/g4_live.html
# Linux
xdg-open eval_platform/dashboard/g4_live.html
```

改过 `g4_state.json` 或 `samples/` 后用 `make page` 重建页面（读归档 JSON，不需要网络）。此档 SSE/MJPEG 区域显示不可用属**正常**——见页面标注的 `archived` 口径。

---

### 档位 B · 单机跑控制台（推荐先验证）

在工程根目录执行（含 `Makefile` 与 `eval_platform/` 的那一层）：

```bash
python3 --version                          # 需 >= 3.8
make doctor                                # ① 只读自检；等价 python3 scripts/doctor.py
cp config/lab.env.example config/lab.env   # ② 仅在换网段时编辑；不改=原实验网默认值
make page                                  # ③ 重建页面；等价 python3 .../build_g4_dashboard.py
make run                                   # ④ 前台启动，Ctrl-C 结束；等价 python3 eval_platform/dashboard/serve_g4.py --port 8899
```

启动后**确认就绪再打开**：

```bash
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8899/api/state   # 返回 200 = 就绪
```

浏览器打开 **http://127.0.0.1:8899/**（等价 `/g4_live.html`）。端口由 `DASH_PORT` 或 `--port` 决定，默认 `8899`。服务只绑定 `127.0.0.1`，不改本机网络配置。

> 双光画面、曲线、节点状态首次可能显示 `--` / 不可用：SSE 每 2 s 推一次、节点名单 25 s 刷新一次，等 10~30 s 才填满。远端设备未上电时离线属正常，归档数据仍可看——**这不代表平台故障**。

---

### 档位 C · 四节点全链路

在档位 B 基础上，先把硬件链路打通（**这一步是节点能否显示在线的关键**）：

```bash
# ① Mac 实验口配 IP（按实际网口名；macOS 示例）
networksetup -setmanual "AX88179B" 192.168.77.1 255.255.255.0
bash eval_platform/tools/recheck_lab_link.sh     # 只读复检
bash eval_platform/tools/finalize_lab_setup.sh   # 装公钥，不需要明文口令落盘
ssh ascend hostname && ssh rk3588 hostname && ssh rv1126b hostname

# ② 确认三块板在线（任一失败都说明链路没通）
ping -c 2 192.168.77.22
curl -s http://192.168.77.22:8082/status.json     # RV1126B 视觉服务
curl -s http://192.168.77.10:8080/health          # 昇腾 LLM

# ③ 再启动控制台（同档位 B 的 make run）
make run
```

四节点角色与端口：`昇腾 :8080`（中心 LLM）· `RK3588 :9100`（证据服务）· `RV1126B :8082`（双光+端侧检测）· `Mac :8899`（显示/控制）。换机迁移与板端 systemd 服务见 `docs/02-deployment.md`。

---

### 停止 / 重启 / 换端口

```bash
# 前台运行：Ctrl-C 结束
# 后台运行：按端口找 PID 再结束
lsof -nP -iTCP:8899 -sTCP:LISTEN -t | xargs kill
make run DASH_PORT=8900              # 或 python3 ... --port 8900
```

---

### 启动排障（先看这里）

| 现象 | 判断 | 处理 |
|---|---|---|
| `curl .../api/state` 不是 200 | 服务没起来或端口被占 | 看启动终端报错；`lsof -iTCP:8899` 查占用；换端口 |
| 页面白屏 | 服务挂了或 SSE 长连接卡住 | `curl` 确认 200 后刷新；否则重启服务 |
| **四节点全 offline、双光"流不可用"** | **多半是实验网没接，不是服务问题** | 见下方诊断 |
| 双光/曲线短暂 `--` | 采样未累积 | 等 10~30 s |

**"节点全 offline"快速诊断**（本工程最常见的"看起来不行"）：

```bash
# 1) 本机有没有实验网 IP？没有就说明 192.168.77.0/24 网口没插/没配
for i in $(ifconfig -l); do ip=$(ipconfig getifaddr "$i" 2>/dev/null); [ -n "$ip" ] && echo "$i: $ip"; done

# 2) 路由是否走实验网口（应看到 interface = AX88179B，而非 en0/WiFi）
route -n get 192.168.77.22 | grep -E 'interface|gateway'

# 3) 硬件是否通
ping -c 2 192.168.77.22
```

若第 1 步只有 `en0: 192.168.x.x`（WiFi）而没有 `192.168.77.x`，或第 2 步路由走 `en0` 网关——**就是实验网口没接入**。此时 dashboard 服务本身完全正常，页面只是如实显示离线；按档位 C 配网即可恢复实时数据。无硬件时可用档位 A 看归档快照。

更多故障见 `docs/07-troubleshooting.md`；使用细节见 `docs/03-user-guide.md`。

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
