# 02 · 部署方式

## 2.1 三档部署

### 档位 A · 只看归档快照（无需任何硬件/网络）

```bash
open eval_platform/dashboard/g4_live.html     # macOS
# 或 xdg-open（Linux）
```

页面里的关键帧、节点角色、实测 KPI 来自 `g4_state.json` 构建期快照，**不是实时值**；SSE/MJPEG 区域会显示不可用。适合备课与预习。

若修改过 `g4_state.json` 或 `samples/`，用 `make page` 重建页面（读归档 JSON，不需要网络）。

### 档位 B · 单机跑控制台（推荐先在 Mac/Linux 上验证）

```bash
python3 --version                              # >= 3.8
make doctor                                    # 只读自检
cp config/lab.env.example config/lab.env       # 可选：仅在改网段时编辑
make page                                      # 重建 g4_live.html
make run                                       # = bash eval_platform/tools/start_dashboard.sh
# 浏览器打开 http://127.0.0.1:8899/
```

服务只绑定 `127.0.0.1`，不会改本机网络配置。远端节点离线时页面会如实显示 `--` / 不可用，归档数据仍可查看。

直接启动亦可：`python3 eval_platform/dashboard/serve_g4.py --host 127.0.0.1 --port 8899`。

### 档位 C · 四节点全链路

按顺序做，前一步没过不要进入下一步。

**① 网络与免密**

```bash
# Mac 实验口示例（macOS；只在原实验网段需要）
networksetup -setmanual "AX88179B" 192.168.77.1 255.255.255.0
bash eval_platform/tools/recheck_lab_link.sh       # 只读复检
bash eval_platform/tools/finalize_lab_setup.sh     # 装公钥；不需要明文口令落盘
ssh ascend hostname && ssh rk3588 hostname && ssh rv1126b hostname
```

若 RV1126B 拿不到地址：先看它是否走 DHCP 而网段无服务器（原实验的静态地址曾误配在 `eth1`），不要直接判硬件故障。排障细节见 `docs/07-troubleshooting.md`。

**② 中心 LLM（昇腾盒子）**

```bash
ssh ascend 'bash -s' < eval_platform/tools/ascend_llm_service.sh
ssh ascend 'systemctl status g4-llm --no-pager'
curl -s http://192.168.77.10:8080/health     # 期望 200
python3 eval_platform/tools/bench_llm_ascend.py
```

要点：官方 ARM64 预编译包要求 GLIBC 2.38，盒子是 2.35，因此 llama.cpp 在盒上源码编译；模型 `qwen3:0.6b Q4 GGUF`（499 MB）离线搬运。生产后端是 **ARM CPU**，不是 NPU——CANN 8.0.RC3 / 310B1 缺 RmsNorm、RoPE 与 fp16 matmul NZ 内核，穷举验证记录见 `eval_platform/runs/.../npu_attempt.md`。

**③ 端侧视觉（RV1126B）**

```bash
bash eval_platform/tools/build_edge_detectors.sh rv1126b
ssh rv1126b 'systemctl is-active camera-preview yolo-live ir-detect'
```

该脚本把 `yolo_live.cpp` 与 `ir_detect.cpp` 推到板上用 g++ 就地编译（板载 OpenCV 4.5 / librknnrt 与运行库版本匹配），并安
装 `ir-detect.service`。两个检测器共用 2 TOPS NPU：热成像网络推理每 2 帧一次、热斑逐帧跑。换模型时改对应 systemd 单
元的 `ExecStart`（页面「控制清单」里有可直接复制的命令），页面顶部模型条会在 2 s 内跟着变。

**④ RK3588 证据与复核**

```bash
ssh rk3588 'python3 /home/cat/eval_dashboard/rk_llm_reconfirm.py'
# G3 证据服务：TCP :9100；板端 dashboard：:8081
bash eval_platform/tools/run_g4_pipeline.sh rk
```

**⑤ 启动控制台与全流程验收**

```bash
bash eval_platform/tools/run_g4_pipeline.sh check
bash eval_platform/tools/run_g4_pipeline.sh llm
bash eval_platform/tools/run_g4_pipeline.sh rk
bash eval_platform/tools/run_g4_pipeline.sh rv
make run
```

`run_g4_pipeline.sh` 的原则是**前置不通就明确拒绝**，绝不伪造 fresh 数据；`rv` 在板子离线时返回 `[WAIT]` 而不是假装采样成功。

## 2.2 验收清单

| 检查 | 命令 | 通过标准 |
|---|---|---|
| 本机自检 | `make doctor` | 关键文件齐全；8899 可绑定或被本平台占用 |
| 控制台 | `curl -s http://127.0.0.1:8899/api/stream --max-time 4` | 收到 `data:` SSE 帧 |
| 配置回显 | `curl -s http://127.0.0.1:8899/api/config` | 地址/SSH 别名与 `lab.env` 一致 |
| 节点名单 | `curl -s 'http://127.0.0.1:8899/api/roster?refresh=1'` | 四节点有采集时间；采不到的写 unavailable |
| 拓扑 | `curl -s http://127.0.0.1:8899/api/topology` | 有接口/介质证据；未测端口为 `inferred` |
| 事件事实 | `curl -s 'http://127.0.0.1:8899/api/events?seconds=60'` | 含 `counts` 与 `steady` 稳态行 |
| 中心 LLM | `POST /api/llm/stream` | 逐 token 回传，done 帧带 `wall_s` / `tok_s` |
| 双光研判 | `POST /api/analyze/stream` | 三行：判断 / 理由 / 建议，判断行含两路分数 |
| 课程包 | `make package && make verify` | 清单、SHA256、脱敏、语法、临时目录重建全过 |

## 2.3 换机/上传 GitHub

```bash
make package
make verify
# 把 dist/course-kit/ 作为新仓库根目录：
#   cd dist/course-kit && git init && git add . && git commit -m "course kit v1.0"
```

包内 `ARCHIVE_NOTICE.md` 列明有意排除的模型/固件/过程日志及其取回方式；`MANIFEST.json` 有每个文件的 SHA256。
**不要**把 `config/lab.env`、`~/.ssh` 私钥或板端口令提交进去。
