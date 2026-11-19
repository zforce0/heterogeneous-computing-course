# 03 · 使用文档（实时控制台）

页面只负责显示与转发，**不做任何推理**。首次使用按 `3.0` 的四步启动后再看分区说明。

## 3.0 快速开始：启动系统与打开控制台

在**工程根目录**（含 `Makefile` 与 `eval_platform/` 的那一层）执行。核心服务只用 Python 标准库，无需安装第三方包；`make` 只是便捷封装，没有 `make` 时用括号里的等价命令。

**① 前置检查（一次性）**

```bash
python3 --version                          # 需 >= 3.8
make doctor                                # 只读自检：Python/文件/端口/远端可达性
                                           # 等价：python3 scripts/doctor.py
```

`doctor` 在远端设备未上电时也会给出提示，不算失败。若换过网段，先 `cp config/lab.env.example config/lab.env` 再改里面的 IP/端口；不改则沿用原实验网默认值。

**② 重建页面（可选，归档快照已随包提供）**

```bash
make page                                  # 从 g4_state.json 重建 g4_live.html，无需网络
                                           # 等价：python3 eval_platform/dashboard/build_g4_dashboard.py
```

**③ 启动服务**

```bash
make run                                   # 前台运行，Ctrl-C 结束
                                           # 等价：python3 eval_platform/dashboard/serve_g4.py --port 8899
```

脚本会先探测 RV1126B 视频服务；若板子未上电只会打印提示，不阻断启动。服务只绑定 `127.0.0.1`，不改本机网络配置。

**④ 确认就绪并打开**

```bash
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8899/api/state   # 返回 200 即就绪
```

浏览器打开：**http://127.0.0.1:8899/**（等价 `/g4_live.html`）。端口由 `DASH_PORT` 或启动参数决定，默认 `8899`。

> 打开后若双光画面、曲线、节点状态短暂显示 `--` / 不可用：SSE 采样每 2 s 推一次、节点名单 25 s 刷新一次，等 10~30 s 即会填满。远端节点离线时如实显示不可用，归档数据仍可查看——这不代表板子故障。

**停服务 / 换端口**

```bash
# 前台运行时 Ctrl-C 即可；后台运行时按端口找 PID 再结束
lsof -nP -iTCP:8899 -sTCP:LISTEN -t | xargs kill
make run DASH_PORT=8900                    # 或 python3 ... --port 8900
```

**端口被占用**：先确认是不是上一次的服务还在（`curl .../api/state` 是否 200）；是则直接复用，否则换端口或结束占用进程。**页面白屏**：多半是服务没在跑或 SSE 长连接卡住，重启服务后刷新即可（`curl` 确认 200 再刷新）。

## 3.1 页面分区

| 区域 | 看到什么 | 数据来源 |
|---|---|---|
| 双光同屏 | 左可见光 / 右热成像 MJPEG，约 5~8 fps；框叠加（实线=NPU、虚线=热斑）；各自模型条 | RV1126B:8082 → 本服务中继；模型名来自两个检测器上报 |
| 节点拓扑 | 四节点、链路介质/速率、逐网口证据表；离线画虚线 | `/api/topology`（SSH 实测，60 s 缓存） |
| 节点分工 | 每节点角色、关键进程、systemd 服务、正在跑的模型 | `/api/roster`（每 25 s SSH 采一次；右上角可「立即刷新」） |
| 实时曲线 | RTT、帧率、LLM tok/s | `/api/stream` SSE 推 2 s 采样 |
| 关键帧 | 4 组归档配对帧（KF-01~04）与 LLM verdict | `keyframes_ascend.json`（**archived**，不是当前帧） |
| 事件流 | 近 60 s 状态切换与「当前稳态」 | `/api/events`，后端从 2 s 采样提取 |
| 对话区 | 流式问答，气泡下方事实/证据条 | `POST /api/llm/stream`，LLM 在昇腾盒子 |

**模型条怎么读**：显示 `文件名 · 引擎 · 单帧耗时`。切换模型只需改板端 systemd 单元的 `ExecStart` 并重启服务，2 s 内页面自动改名——名字取自运行时上报，不是构建期写死。

## 3.2 常用操作

```bash
# 抓拍当前双光帧到 dashboard/samples/
curl -s 'http://127.0.0.1:8899/api/snapshot?kind=vis'
curl -s 'http://127.0.0.1:8899/api/snapshot?kind=ir'

# 暂停/恢复 RV1126B 视觉服务（经控制台代理）
curl -s 'http://127.0.0.1:8899/api/control?action=pause'
curl -s 'http://127.0.0.1:8899/api/control?action=start'

# 现场跑 3 条 LLM 基准
curl -sN -X POST http://127.0.0.1:8899/api/bench -H 'Content-Type: application/json' -d '{}'
```

页面底部「控制清单」卡片里每条命令都有「复制」按钮，可直接照抄。

## 3.3 两类「问事件」的口径（重要）

这两个问题看起来像，但**答案格式完全不同**，后端按关键词分流（研判优先于列举）：

### ① 事件台账 —— 只列条目，不下结论

问：`近一分钟在本地系统中监测到哪些事件？`

```
近一分钟在本地系统中监测到以下事件:
1) 19:15:20 [变化] pottedplant 离开可见光视野（8 秒未检出）
```

- 气泡角标：**事件台账 · 只列条目，不下结论**
- 下方事实条：`台账 · 事件 N 条 · <首条> ｜ 持续 …`（悬停看完整事件块）
- 事件为 0 条时：`近一分钟无事件。当前持续监测 …，热斑 … 个/帧。`
- 模型只复述后端给出的条目；判断/建议被明令禁止，防止把稳态说成事件。

### ② 双光研判 —— 只给三行结论

问：`近 1 分钟双光事件分析`（或点 chip「双光事件分析」）

```
判断：关注（可见光 max0.94 / 热成像未报出同类）
理由：可见光数据达标，热成像未报出同类，存在跨模态漏检。
建议：优化数据采集策略，提升热成像的检测精度。
```

- 气泡角标：**双光研判 · 判断 / 理由 / 建议**
- 下方证据条：`对比 · 30 次采样 / 58s · 可见光 person 26/30 max0.91 ↔ 热成像未报出同类 · 热斑 1.8/帧 · 异常 0 条`
- **判断行由后端机械判定并强制替换**：异常事件>0→异常；任一路就绪<90%→关注；可见光命中≥80% 而热成像整窗无同名类→关注（跨模态漏检）；其余正常。
- 所有数字由后端算（分差、命中率、热斑、异常数），0.6B 不参与计算。

### ③ 事件复核（自包含，不接地）

问：`事件：可见光检测到 person，分数 0.91 … 请判断是否值得复核，只回答 confirm 或 reject，并给一行理由。`

带 `confirm/reject` 的请求会**跳过实时事实注入**，只回答给定事件本身，避免被事件台账带跑。

### ④ 节点问答

问：`现在四个节点分别负责什么、各自在跑哪个模型或关键进程？`

后端把 `/api/roster` 的实时名单注入 system 上下文；答案随实际状态变，不是构建期快照。请求里加 `"ground": false` 可关闭注入。

## 3.4 HTTP API 一览

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/`、`/g4_live.html` | 页面（禁用缓存，改完刷新即生效） |
| GET | `/api/state` | 构建期状态快照 |
| GET | `/api/config` | 当前地址/端口/SSH 别名（无凭据） |
| GET | `/api/status` | 四节点一次性探测（旧版兼容） |
| GET | `/api/stream` | SSE：2 s 推送节点/视觉/LLM/事件 |
| GET | `/api/series` | 近 6 分钟时序 |
| GET | `/api/metrics` | llama-server Prometheus 指标解析结果 |
| GET | `/api/topology` | 逐网口实测拓扑；`?refresh=1` 强制重采 |
| GET | `/api/roster` | 节点分工；`?refresh=1` 强制 SSH 重采 |
| GET | `/api/events?seconds=60` | 事件事实块（counts + changes + steady） |
| GET | `/stream/vis.mjpg`、`/stream/ir.mjpg` | 双光 MJPEG 中继 |
| GET | `/frame/vis.jpg`、`/frame/ir.png` | 单帧 |
| GET | `/api/snapshot?kind=vis\|ir` | 存证到 `samples/` |
| GET | `/api/control?action=start\|pause\|stop` | 代理视觉服务控制 |
| POST | `/api/llm`、`/api/llm/stream` | 一次性 / 流式对话 |
| POST | `/api/analyze/stream` | 双光研判（同一 chip 路径） |
| POST | `/api/bench` | 3 条标准用例实测吞吐 |

## 3.5 口径与限制

- 视频是 RV1126B 的 MJPEG 中继，服务没启动时页面提示「可见光流不可用」，**不代表板子坏了**。
- 热成像是 0~255 相对灰度 + 热斑；复用可见光 YOLO 权重属跨模态，检出的 person 只能当「疑似」。
- `ir_fps` 只统计真正更新过的帧（原 6.5 是同帧重复推送的虚高值）；可见光轮询间隔 20 ms，避免漏帧。
- 事件只记状态切换；「chair 一直在场」这类稳态放在「当前稳态」一行，不算事件。分数 <0.40 的弱检测不触发事件。
- 事件为**内存态**，服务重启后清零；需要留存请定期抓 `/api/events` 输出。
- 0.6B 模型语义有限：研判只做「判断/理由/建议」三行，不归因、不推理复杂因果；三次重试仍吐模板占位时会删掉该行并如实标注降级。
