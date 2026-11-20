# 07 · 故障排查

按「物理层 → 网络层 → 服务层 → 数据口径 → 模型输出」顺序排查，不要在上一层没排除时就改下一层。

## 7.1 设备离线 / 拿不到地址

| 症状 | 常见原因 | 处理 |
|---|---|---|
| RV1126B 灯亮但 ping 不通 | `eth0` 走 DHCP，而实验网没有 DHCP 服务器；静态 `.22` 误配在 `eth1`；或网线插错口 | 确认网线插 **NET0/eth0**（板载 GMAC）；双口适配见下，或跑 `setup_rv_dual_nic.sh` |
| RV1126B 插 NET1 也要能用 | NET1 是 USB 网卡（eth1，MAC `a6:52:d5:68:39:f1`），与 eth0 MAC 不同 | 双口适配：eth0 静态 `.22`、eth1 走 DHCP 由 RK3588 按 MAC 固定分配 `.22`（见 7.2） |
| `.22` 全部丢包，ARP 指向 RK3588 的 MAC | 遗留 `rk-forward.service` 的 proxy ARP 劫持 | 在 RK 上停 `rk-forward.service`、两口 `proxy_arp=0`、`ip_forward=0`、删 `.22` 主机路由；Mac 清 ARP 缓存 |
| 升腾盒子三层不可见 | 出厂地址是 `192.168.137.100`，不是规划的 `.10`；无 DHCP/mDNS | 经同网段跳板发现，`persist_ascend_lab_ip.sh` 固化双地址，装公钥后复检 |
| 扫描把打印机 `192.168.1.52:9100` 当 RK3588 | 9100 也是 RAW/JetDirect 打印端口 | 用 ICMP + 8081 标题 + 9100 ping/pong 三重证据，不要只看单端口 |
| `ping` 判定误报 PASS | 裸子串 `"0.0% packet loss"` 命中 `"100.0% packet loss"` | 用退出码或 `\b0\.0% packet loss` 正则 |

只读复检顺序：

```bash
bash eval_platform/tools/recheck_lab_link.sh
bash eval_platform/tools/discover_lab_devices.py
python3 eval_platform/tools/discover_topology.py --json
```

### RV1126B 双网口适配（NET0 / NET1 都能用）

RV1126B（EASY-EAI-NANO-TB）有两个实验网口，MAC 不同，不能都配静态 `.22`（会冲突）。现行方案：

| 口 | 系统设备 | MAC | 配置 | 插它时 |
|---|---|---|---|---|
| **NET0**（板载 GMAC） | `eth0` | `4e:0e:39:be:41:b3` | 静态 `192.168.77.22/24` | 直接用静态地址 |
| **NET1**（USB 网卡） | `eth1` | `a6:52:d5:68:39:f1` | DHCP | 由 RK3588 按 MAC 固定分配 `.22` |

任一时刻只插一根网线，`.22` 不冲突；视觉服务 `camera_stream.py` 监听 `0.0.0.0:8082`，与具体网口无关。

一键适配（板端执行，幂等，自动备份 netplan）：

```bash
scp eval_platform/tools/setup_rv_dual_nic.sh rv1126b:/tmp/
ssh rv1126b "sudo bash /tmp/setup_rv_dual_nic.sh"
```

**依赖**：RK3588 上 `g4-dhcp.service` 必须为 active，且 `/etc/g4-dhcp.conf` 含两 MAC 绑定同一 IP：

```
dhcp-host=4e:0e:39:be:41:b3,a6:52:d5:68:39:f1,192.168.77.22,infinite
```

若 dnsmasq 报 `duplicate dhcp-host IP address`，就是把两个 MAC 写成了两条独立 `dhcp-host`——必须合并成上面一条逗号分隔的形式。验证：换线后 Mac 侧 `ping -c2 192.168.77.22` 与 `curl http://192.168.77.22:8082/status.json` 都应通过。

## 7.2 热成像 / 双光

- **热成像画面卡住但 `ir_fps` 正常、状态 streaming**：厂商 SDK 长时间运行后会「循环写同一帧」——`/tmp/ir_sdk.raw` 的 mtime 照常增长，内容 MD5 却不变。`ir_grabber` 已加重复帧检测（连续 `IR_DUP_LIMIT=20` 帧相同即重建 SDK）；也可手动 `ssh rv1126b "sudo systemctl restart camera-preview"` 立即恢复（板端需已同步新版 `camera_stream.py`）。注意旧版只检测全黑帧，识别不了冻结帧。

- **SDK 报 `IR camera 0bda:5840 was not found`**：先查物理连接。TC1-C 模组自身无 USB 口，须经 DVP→USB 转接板；厂商 SDK 以 usbfs 独占接口，因此 `uvcvideo` 不会创建 `/dev/video` 节点——按 video4linux 判断会误判为未插入。以 `lsusb` / USB 拓扑为准。
- **`ir_fps` 虚高 6.5**：同帧被重复推送。现按 `st_mtim.tv_nsec + size` 判新帧，实测 5.0。
- **检测钉在 1 Hz**：用了秒级 `st_mtime`。改纳秒戳后恢复 5 Hz。
- **热成像人检整窗为空**：跨模态复用可见光权重的真实漏检；对比条会写「热成像未报出同类」，研判判「关注」。不要用可见光框投影冒充热成像检测结果。
- **SIGTERM 杀 SDK 后 USB 端口挂死（err=-32）**：5–15 s 会自愈；重启 `camera-preview.service` 前留出间隔。
- **热成像没有温度**：只有 0~255 相对灰度与帧内统计；不要写摄氏度/ΔT。

## 7.3 Web/控制台

| 症状 | 处理 |
|---|---|
| 视频区「可见光流不可用」 | 本服务没在跑或 RV:8082 不可达；页面提示不代表板子坏了。先 `make run`，再查远端 |
| 改完页面刷新没变化 | 服务对 `/` 禁缓存；若直接双击 HTML，请重新执行 `make page` 后刷新 |
| 端口 8899 冲突 | `lsof -nP -iTCP:8899 -sTCP:LISTEN` 找 PID；或用 `DASH_PORT` / `--port` 换端口 |
| 节点名单全 unavailable | `~/.ssh/config` 别名不对、没装公钥、或远端无 `python3`；`/api/roster?refresh=1` 看错误摘要 |
| Mac 上进程列表为空 | macOS 沙箱/权限限制，页面如实标「其他进程不可枚举」，不是服务故障 |
| 页面事实条数字与 LLM 回答不一致 | 以事实条为准；LLM 只组织语言，数字由后端算。若出现编造数字请提 issue 并附 done 帧 |

## 7.4 LLM 与研判输出

- **0.6B 照抄整句模板**：带可执行文字的模板行（「从正常/关注/异常里选…」「，并用对比里的数字说明理由」）会被原样吐出。现行做法：判断行给后端成品，理由/建议只留「一句话」三字占位。
- **仍吐「理由：一句话」**：研判走非流式，检测占位后最多重试 3 次（第 2 次起 temperature 0.45）；三连败删掉占位行并标注「本次模型未给出理由与建议」。
- **判断词被改成「异常」**：`enforce_verdict()` 在返回前无条件替换整行成后端成品，模型无权改写。
- **把 steady 当事件**：空窗口给成品句 +「把这句话原样说一遍」（实测一字不差）；自由总结会说出「监测到 2.4 个热斑」这种误导句。
- **「双光事件分析」被当成事件列举**：研判关键词优先级高于事件关键词；`confirm/reject` 复核请求直接透传、不注入事实。

## 7.5 NPU / 编译

- **官方 ARM64 预编译 llama.cpp 起不来**：要求 GLIBC 2.38，盒子是 2.35；在盒上源码编译。
- **CANN NPU 后端**：`GGML_CANN_NO_PINNED=1`、`GGML_CANN_MEM_POOL=leg`、`GGML_CANN_WEIGHT_NZ=off` 能绕过前三个限制；第四个 `ascend310b` 缺 RmsNorm 算子无解，生产固定 CPU 后端。详见 `npu_attempt.md`。
- **端侧 C++ 编译失败**：在 RV1126B 上就地编译（板载 OpenCV 4.5 + librknnrt 版本匹配），不要直接拿交叉编译产物；依赖 `/userdata/rv1126_test/{include,lib}`。
- **RKNN 模型加载失败**：`rknn-toolkit2` 转换版本要与板端 `librknnrt` 一致（原机是 1.5.0）；格式更新会导致加载失败。

## 7.6 提交 GitHub 前

```bash
make package && make verify
python3 scripts/sanitize.py --check dist/course-kit
```

若仍有命中，说明历史文档被新增进了打包清单，请从 `scripts/package_course_kit.py` 的清单中移除或补充脱敏规则。SSH 私钥、`config/lab.env`、板端口令**永不入库**。
