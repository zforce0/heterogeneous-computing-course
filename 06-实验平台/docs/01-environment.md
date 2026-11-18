# 01 · 环境配置与实验网规划

> 本文描述**原实验机实测环境**（2026-10 ~ 2026-11）。换机部署时把差异写进 `config/lab.env`，不要直接改源码常量。

## 1.1 硬件与实测规格

| 节点 | 原实验机型号 | 实测规格 | 本课角色 |
|---|---|---|---|
| Mac | MacBook Air M1 (MacBookAir10,1) | 8 核 / 16 GB / 68.25 GB/s，千兆 USB 网卡 `AX88179B` (en5) | 显示/控制，运行控制台 :8899 |
| 昇腾盒子 | QiZhi01-20T / Atlas 200I A2 | 310B1，20 TOPS INT8 / 10 TFLOPS FP16，24 GB，CANN 8.0.RC3，Ubuntu 22.04 aarch64，无外网/无 pip/无 docker | 中心 LLM，llama.cpp b12375 |
| RK3588 | LubanCat-5IO | 6 TOPS NPU / 7.73 GiB / Debian 11 | 证据服务 :9100 + 板端 dashboard :8081 |
| RV1126B | EASY-EAI-NANO-TB | 4×aarch64 / 1.9 GiB / Ubuntu 22.04 / 2 TOPS NPU / kernel 6.1 | 双光采集 + YOLOv8m(RKNN) + 热斑 |

拓扑：四节点同挂 **非管理型交换机**，星型 `192.168.77.0/24`。交换机不可 SSH/SNMP/LLDP，因此页面对「设备→交换机端口」一律标 `inferred`，不伪造端口号。

## 1.2 地址与端口表

| 用途 | 变量（`config/lab.env`） | 原实验值 |
|---|---|---|
| Mac 实验口 | `MAC_IP` | `192.168.77.1` |
| 昇腾盒子 | `ASCEND_IP` / `LLM_PORT` | `192.168.77.10:8080` |
| RK3588 | `RK_IP` / `RK_DASH_PORT` / `RK_EVIDENCE_PORT` | `192.168.77.21:8081` / `:9100` |
| RV1126B | `RV_IP` / `RV_VISION_PORT` | `192.168.77.22:8082` |
| 控制台 | `DASH_HOST` / `DASH_PORT` | `127.0.0.1:8899`（只绑定本机） |
| SSH 别名 | `SSH_ASCEND` / `SSH_RK3588` / `SSH_RV1126B` | `ascend` / `rk3588` / `rv1126b` |

**配置优先级**：进程环境变量 > `config/lab.env` > `config/lab.env.example` 内置值。
`config/lab.env` 已被 `.gitignore` 排除，里面只放地址/别名，**不要写任何口令**。

```bash
cp config/lab.env.example config/lab.env
$EDITOR config/lab.env
make doctor          # 校验解析结果与三个远端服务是否真的在线
curl -s http://127.0.0.1:8899/api/config | python3 -m json.tool   # 服务启动后核对
```

SSH 免密放在本机 `~/.ssh/config`（别名与上表一致），公钥由各设备管理员安装；首次装的板端口令属于私有信息，不进入仓库。

## 1.3 软件依赖

**核心平台（必需）**：Python ≥ 3.8，纯标准库（`http.server`、`urllib`、`socketserver`、`subprocess`、`json`、`threading`）。系统命令：`ssh`（节点名单/拓扑采集）、`ping`（可达性）、`curl`（脚本自检）。

**可选 ML/模型转换**：

```bash
python3 -m pip install -r requirements-ml.txt
# numpy 1.26.4 / protobuf 4.25.3 / onnxruntime 1.17.3 / onnx / opencv-python
# torch 仅用于 twocameraOnMAC/models/yolov8n/export_onnx.py
```

**板端运行时（非 pip）**：RKNN Toolkit2 1.5.0 与板载 `librknnrt`（`rknn_convert.Dockerfile` 提供转换环境）、OpenCV 4.5 + g++（端侧 C++ 检测器在板上就地编译）、llama.cpp b12375 + CANN 8.0.RC3（昇腾盒子源码编译）。

## 1.4 模型与数据文件

| 文件 | 用途 | 是否随课程包分发 |
|---|---|---|
| `yolov8m_rv1126b.rknn` | RV1126B 可见光 + 热成像跨模态复用 | 否（>27 MB，从原板 `/userdata/rv1126_test/` 拷贝） |
| `resnet50.rknn` | RK3588 NPU 基准 | 否（50 MB，`convert_resnet50.py` 可重建） |
| `qwen3:0.6b Q4 GGUF` | 中心 LLM（499 MB，别名 `g4-edge-llm`） | 否（从昇腾 `/opt/g4-llm/models/` 或原 Ollama blob 搬运） |
| `keyframes_ascend.json` + `dashboard/samples/*` | 归档关键帧与小样本图 | 是（几百 KB ~ 1 MB） |
| `tau_*.bin` / 固件 `.bin` | 热成像标定/固件（厂商资料） | 否（二进制，原机保留） |

> 热成像**没有温度标定**：实时通道只有 0~255 相对灰度与帧内统计。V1.2/V1.3 文档里出现的 `ΔT` 是 2026-10-12 归档帧的历史字段，不代表当前系统能测温，页面实时卡片一律不显示摄氏度。

## 1.5 目录与职责

```
config/lab.env.example            地址/端口/SSH 别名模板（可公开）
eval_platform/config/lab_config.py   统一解析；被 dashboard 与页面生成器引用
eval_platform/dashboard/serve_g4.py  控制台服务：SSE、MJPEG、LLM 代理、事件与研判
eval_platform/dashboard/camera_stream.py  RV1126B 板端采集/取流服务（:8082）
eval_platform/tools/node_roster.py   SSH 下发执行的「在跑什么」采集器
eval_platform/tools/ir_detect.cpp   热成像端侧检测（RKNN + 热斑）
eval_platform/tools/yolo_live.cpp   可见光端侧 YOLO
eval_platform/tools/{evidence_server,evidence_sender}.py  G3 长度前缀证据协议
eval_platform/runs/<run_id>/        实测归档（report.md 是事实来源）
```

## 1.6 换机迁移清单

1. 新机器配同网段地址（或用 `lab.env` 换一整套地址），确认交换机与四节点上电。
2. 在 `~/.ssh/config` 配好三个别名并装公钥；`ssh <alias> hostname` 逐个验证。
3. 从原机拷贝模型二进制到对应目录（课程包不含），按 `docs/02-deployment.md` 重建端侧检测器。
4. `make doctor` 全绿后 `make page && make run`。
5. 若只有 Mac 一台：跳过所有远端检查，页面仍可看归档快照（档位 A/B）。
