# 08 · FAQ：问题与解决方案清单

本清单汇总工程搭建与运行至今遇到的真实问题，按「现象 → 根因 → 解决 → 定位命令」整理，便于复现与排障。按类别分组，先找对应症状。

> 通用排障顺序：**物理层（线/电源/网口）→ 网络层（IP/路由/ARP）→ 服务层（进程/systemd）→ 数据口径（帧是否真在更新）→ 模型输出**。上一层没排除前不要改下一层。
>
> 一句话经验：**画面静止先看帧内容 MD5 是否变化，不要看帧号/fps** —— 帧号和 fps 在"读同一张旧图"时照样涨。

---

## A. 网络与节点发现

### A1. RV1126B 灯亮但 ping 不通、全网段扫描看不到它
- **根因**：静态 `.22` 配在 eth1（NET1，USB 口），网线却插在 eth0（NET0，DHCP 模式）；实验网没有 DHCP 服务器 → 板子拿不到地址、二层零流量。也可能网线插错口/松动。
- **解决**：网线插 **NET0/eth0**（板载 GMAC）。双口适配见 A2。
- **定位**：`arp -an | grep 77.22`（`incomplete`=二层不可见）、`ping -c2 192.168.77.22`、`lsusb`、`ssh rv1126b`。

### A2. 希望 NET0 和 NET1 插任一口都能用
- **根因**：两口 MAC 不同，不能都配静态同一 IP（冲突）。
- **解决**：eth0 静态 `.22`；eth1 走 DHCP，由 RK3588 的 `g4-dhcp` 按 MAC 固定分配 `.22`。一键适配（板端）：
  ```bash
  scp eval_platform/tools/setup_rv_dual_nic.sh rv1126b:/tmp/
  ssh rv1126b "sudo bash /tmp/setup_rv_dual_nic.sh"
  ```
  依赖 RK3588 `/etc/g4-dhcp.conf` 含（两 MAC 绑同一 IP，**必须合并成一条**）：
  ```
  dhcp-host=4e:0e:39:be:41:b3,a6:52:d5:68:39:f1,192.168.77.22,infinite
  ```
- **报错 `duplicate dhcp-host IP address`**：写成两条独立 `dhcp-host` 了，合并即可。

### A3. DHCP 兜底服务 `g4-dhcp` 起不来 / `unknown interface eth1`
- **根因**：RK3588 上没装 dnsmasq（服务早年被停用、disabled）。
- **解决**：装 dnsmasq（见 02-deployment），`systemctl enable --now g4-dhcp`。apt 若因 DNS 解析失败，在 Mac 侧下载 deb 再 `scp` 过去 `dpkg -i`。
- **定位**：`systemctl is-active g4-dhcp`、`journalctl -u g4-dhcp`、`pgrep -a dnsmasq`、`ss -ulnp | grep :67`。

### A4. 升腾盒子三层不可见
- **根因**：出厂地址是 `192.168.137.100`，不是规划的 `.10`；无 DHCP / mDNS。
- **解决**：经同网段跳板发现，`persist_ascend_lab_ip.sh` 固化双地址，装公钥后复检。

### A5. `.22` 全部丢包但 ARP 指向 RK3588 的 MAC
- **根因**：遗留 `rk-forward.service` 的 proxy ARP 劫持。
- **解决**：RK 上停 `rk-forward`、两口 `proxy_arp=0`、`ip_forward=0`、删 `.22` 主机路由；Mac 清 ARP 缓存。

### A6. RK3588 两口桥接失败（br0 起不来 / 时通时断）
- **根因**：这块板子的两块 GMAC **不能桥接**（内核拒绝），只能三层转发。
- **解决**：三层中继（eth0=`.23/32` + `/32` 主机路由 + proxy_arp + ip_forward）。**不要重试桥接**。
- **注意**：`ip route` 输出主机路由不打印 `/32`，解析要兼容 `X.X.X.X dev ethX` 与 `X.X.X.X/32 dev ethX` 两种写法。

### A7. 扫描把办公室打印机当 RK3588
- **根因**：`9100` 也是 RAW/JetDirect 打印端口。
- **解决**：用 ICMP + 8081 标题 + 9100 ping/pong 三重证据，不要只看单端口。

### A8. `ping` 判定误报 PASS
- **根因**：裸子串 `"0.0% packet loss"` 命中了 `"100.0% packet loss"`。
- **解决**：用退出码或 `\b0\.0% packet loss` 正则。

### A9. 改 RK3588/RV 网络后把自己锁在外面
- **预防**：改前挂回滚保险 `systemd-run --on-active=NN /usr/local/sbin/g4-*-rollback.sh`，实测多次靠它自动恢复管理通道。
- **注意**：Mac 侧 `sudo` 被策略禁用，任何需改 macOS 路由表的方案都不可行，必须让 Mac 无感。

---

## B. USB / 相机 / 热成像

### B1. 热成像"未枚举"，换板子也没用
- **根因**：TC1-C 模组**自身没有 USB 口**（40pin DVP/SPI 模组），必须经厂商 DVP→USB 转接板才成为 UVC 设备 `0bda:5840`。排查方向应是转接板 USB 头 → 排线 → 供电。
- **解决**：按物理连接逐段查；重插 USB 后 `0bda:5840` 立即枚举。
- **定位**：`lsusb | grep 0bda:5840`、`dmesg`。

### B2. 热成像"插着却看不到"，按 video4linux 找不到
- **根因**：厂商 `ir_snap` 以 **usbfs 独占**接口 1.0+1.1，uvcvideo 被 detach，`/dev/video*` 随之消失。此时按 video4linux 匹配必然落空，误报"未枚举"。
- **解决**：判断热成像插没插必须查 `/sys/bus/usb/devices`，不能用 `/dev/video`。
- **口径**：两态都正常——SDK 未起 `state=v4l2 / node=/dev/video53`；SDK 起后 `state=usbfs / node=None`，端口始终如 `4-1.2`。

### B3. 可见光/热成像画面静止，但帧号、fps 还在涨
- **根因**：**帧冻结**——程序循环在跑、mtime 在变，但读的是同一张图。
  - 可见光：UVC 相机固件冻结，持续输出同一帧（dmesg 无 USB 错误）。
  - 热成像：SDK 长时间运行后循环写同一帧（mtime 变、内容 MD5 不变）。
- **解决**：
  - 可见光固件冻结：软件复位（`authorized` 0→1、usbreset）无效，**整板 `systemctl reboot`** 才恢复。
  - 热成像 SDK 冻帧：新版 `ir_grabber` 检测连续 20 帧内容相同即自动重建 SDK；手动 `ssh rv1126b "sudo systemctl restart camera-preview"`。
- **定位**：`md5sum /tmp/live_frame.jpg`（可见光）、`md5sum /tmp/ir_sdk.raw`（热成像）连续几次，恒定即冻结。**这是最快的判据**。

### B4. 热成像黑屏但状态显示 streaming / 绿色
- **根因**：`ir_snap` 启动时设备若被 uvcvideo 抢绑，libusb 拿不到接口，循环写「帧头合法、内容全 0」的帧。旧代码只校验帧头，把死流判成正常。
- **解决**：`ensure_sdk()` 启动前先 `unbind uvcvideo`；`ir_grabber` 加全黑检测（`IR_BLACK_LIMIT=12` 帧≈1.5s），连续全黑即清空帧 + `ir_state=sdk_error` + 重建。
- **定位**：`stat` RAW 文件大小正常但 `min=max=0`。

### B5. 点「重连」后画面不恢复 / 覆盖层卡住
- **根因**：
  - 前端：两路流原先直接 `<img src="...mjpg">` 由浏览器托管长连接，服务端重启后 Chrome 停在最后一帧且不触发 `onerror`，看门狗不重挂。
  - 覆盖层只在 `img.onload` 隐藏，而 multipart 长连接基本不触发 load → 挂上就撤不掉。
- **解决**：前端改 `fetch` + 自解析 multipart，按**页面真实收帧时间**判活；新增 `syncOverlay` 每 2s 按端侧 ready 校正覆盖层。

### B6. 换 USB 口后可见光断流
- **根因**：`yolo_live` 编译好的 ELF 里 by-path 设备路径写死，换口后路径不存在；`camera_stream.py` 里的 `VIS` 常量是死代码。
- **解决**：重写 `yolo_live.cpp` 按 VID:PID（`1d6c:0103`）动态找设备，候选顺序 `G4_VIS_DEV` > `/dev/g4-vis` > VID:PID 匹配 > 其它；开机无设备时进程内每 2s 等待而非退出。
- **编译坑**：漏 `file_utils.cpp` 会 `undefined reference to read_data_from_file`；板端无 `opencv4.pc`，须手写 include/lib。

### B7. 相机画面静止诊断口诀（可复用）
1. `md5sum` 帧文件连续几次 → 恒定=帧冻结
2. 区分"推理在跑" vs "输入帧冻结"：`live_boxes.json` 的 seq 涨但 detections 不变 → 读同一帧
3. UVC 固件冻结软件救不了 →  reboot 整板

### B8. `uvcvideo quirks` 不要照抄
- RK3588 原值 `4294967295`（全 quirk，已含 FIX_BANDWIDTH=0x80）。照抄 RV1126B 的 `quirks=128` 会关掉其他兼容补丁。已恢复原值。

---

## C. 服务与进程

### C1. "视频流不可用" —— 板子没坏，是 dashboard 没跑
- **根因**：`serve_g4.py` 进程没在跑（会话结束后台进程被回收）。板端 `/vis.jpg` 一直正常。
- **解决**：`./start.sh` 或 `make run`。页面提示"不可用"不代表板子故障。
- **注意**：`nohup ... &` 在某些受限沙箱里随命令结束被回收 → 改前台运行。

### C2. `camera_stream.py` 累计重启 6 万多次
- **根因**：`HOST` 写死 `192.168.77.22`，网口没地址时 bind 失败。
- **解决**：改 `G4_BIND` env，默认 `0.0.0.0`。

### C3. 板端服务自愈守护
- **方案**：`g4_guard.py`（`g4-guard.service`，Nice=5）：cam 回路 5s 扫 video4linux 按 VID:PID 维护 `/dev/g4-vis` 别名；net 回路 10s 补路由/proxy_arp。破坏后 14s 内恢复。
- **注意**：usbfs 独占态不建 `/dev/g4-ir`（避免指向不存在节点）。

### C4. SIGTERM 杀 SDK 后 USB 口挂死
- **现象**：`dmesg: usb4-port1 cannot reset (err=-32)`，设备从 `lsusb` 消失 5~15s 后自愈。
- **解决**：等 5~15s；长时间不回来需物理重插或重启板子。

### C5. launchd 常驻 dashboard 不可行
- **现象**：最小 ASCII plist 也 `bootstrap failed: 5: Input/output error`。
- **结论**：该环境禁用 launchctl bootstrap，已删除 plist。用 `start.sh` / 手动后台即可。

### C6. Mac 侧 `ps`/`sudo`/`ss -ltnp` 受限
- **现象**：`ps` 报 `operation not permitted`、`sudo` 禁用。
- **解决**：用 `lsof -nP -iTCP:8899 -sTCP:LISTEN`、`pgrep`；节点进程信息由板端脚本采集，Mac 上如实标注"其他进程不可枚举"。

---

## D. 数据口径与帧率统计

### D1. `ir_fps` 虚高 6.5
- **根因**：同一帧被重复推送（不看 mtime）。
- **解决**：按 `st_mtim.tv_nsec + size` 判新帧，实测 5.0。

### D2. 检测钉在 1 Hz
- **根因**：用了秒级 `st_mtime`。
- **解决**：改纳秒戳 `st_mtim.tv_nsec`，恢复 5 Hz。

### D3. `vis_fps` 偏低 5.0
- **根因**：轮询间隔 0.1s 漏帧。
- **解决**：间隔 0.02s，实测 7.5。

### D4. 热成像没有温度 / 别写摄氏度
- **口径**：TC1-C 只有 0~255 相对灰度与帧内统计，**无温度标定**。禁止写摄氏度/ΔT；厂商 `det_*_ir.jpg` 是把可见光框投影到热图，**不是热成像推理**。

### D5. 跨模态检测结果偏弱
- **口径**：热成像端侧复用可见光 YOLOv8m 权重，人物场景常整窗无同名类（真实漏检）。对比条写明"热成像未报出同类"，研判判"关注"。不要把可见光框投影冒充热成像检测。

### D6. 归档帧与实时流方向不同
- **口径**：实时流原始朝向（256×192，已取消旋转）；归档关键帧 `det_*_ir.jpg` 是早期物理旋转过的 192×256，不能直接叠加比对。

### D7. 别用 `/api/state`、`/api/status` 判断视频状态
- **原因**：前者是渲染期静态快照、后者只有节点探测，**都没有 vision 实时字段**。实时数据只在 SSE `/api/stream`。

### D8. 事件只存内存、窗口固定
- **口径**：重启即清零；LLM 事件问答固定 60s 窗口。0.6B 只能照抄条目+一句总结，不做归因。

---

## E. 中心 LLM（0.6B）问答

### E1. 给示例数字/长判据，模型整段照抄
- **现象**：提示词里写"从正常/关注/异常里选…"或具体数字示例，输出原样抄这些文字。
- **解决**：判断值由后端算好给"成品"让它抄；不给可执行长句、不给数字示例。

### E2. 占位符 `<类别>` 被原样吐出
- **解决**：改成纯文字枚举/成品句，不用尖括号占位。

### E3. "理由：一句话" 约 1/3 概率照抄不替换
- **解决**：研判改非流式取完整结果，检测占位最多重试 3 次（第 2 次起 temperature 0.45）；三连败删占位行并标注"本次模型未给出理由与建议"，绝不把占位当结论。

### E4. 空窗口把稳态说成事件
- **现象**：让模型自由总结时说出"监测到 2.4 个热斑"（把稳态当事件），或把用户问题原样吐回。
- **解决**：单条 user，只给成品句 + "把这句话原样说一遍"，实测一字不差 24 tok。

### E5. 两个"问事件"问题串味
- **现象**："近一分钟有哪些事件"和"双光事件分析"都走事件复述，后者答不出结论。
- **解决**：意图三分流——`ANALYSIS_Q` 优先于 `EVENT_Q`（否则"双光**事件**分析"被"事件"抢去）；事件=只列条目，`analysis`=只给判断/理由/建议。
- **附带**：带 `confirm/reject` 的复核请求直接透传（intent=review），不接地。

### E6. 判断行说"关注"、理由说"一致"（自相矛盾）
- **解决**：判断由后端机械判定并按就绪率/异常数/跨模态漏检算，返回前**无条件替换**模型写的判断行（模型会省括号分数、会把"关注"改成"异常"）。

---

## F. 前端显示 / 页面

### F1. 可见光/热成像左右布局仍上下堆叠
- **根因**：窄 viewport 触发媒体查询。
- **解决**：断点设 560px 才堆叠；`.duo{grid-template-columns:1fr 1fr}`。

### F2. 页面完全空白
- **根因**：自己引入的 JS 语法错误。
- **解决**：`node --check` 抽取脚本检查。

### F3. 改完页面刷新没变化
- **根因**：静态页由 `SimpleHTTPRequestHandler` 只发 Last-Modified，浏览器启发式缓存。
- **解决**：手动发 `Cache-Control: no-store, must-revalidate`，并补 `do_HEAD`（原先 HEAD 命中旧 `index.html`）。

### F4. 相机状态 offline / 图片不加载
- **根因**：Chrome PNA 预检、host CSP 拦截。
- **解决**：检查 CSP 与 PNA；静态预览的 host CSP 是真因之一。

### F5. 遮罩层多行文字间距散开
- **根因**：`display:grid` 把多行拆成多个 grid item。
- **解决**：内容统一包一个 `<div class="ovin">`。

### F6. 画面右缘检测标签被裁
- **解决**：标签 x 按 `min(bx, CW-tw)` 夹住。

### F7. 光条 URL 泄漏致跑久画面不动
- **根因**：blob URL 挂在 `onload` revoke，下一帧覆盖 onload → 永久泄漏 → 解码失败。
- **解决**：用队列延迟 2s revoke。

### F8. 窄卡片文件名被 flex 压空
- **解决**：`.mtext` 单截断点 + `min-width:0`。

---

## G. 打包 / 发布 / 解锁页

### G1. GitHub 上点 `unlock.html` 只显示源码
- **根因**：GitHub 不渲染仓库内 HTML；免费版私有仓库不支持 Pages（API 422）。
- **解决**：仓库转 public + 启用 Pages → `https://zforce0.github.io/heterogeneous-computing-course/unlock.html`。

### G2. 读取加密包后长时间无反应
- **根因**：1.8MB 加密包被 Pages 限流（实测 30s 仅 724KB），且无进度反馈。
- **解决**：先 gzip 压缩再加密（1.8MB→580KB），拆 3 片并行拉取 + 逐片进度提示。

### G3. 解锁后文件名乱码
- **根因**：tar 用 PAX 扩展头，真实文件名在 `LEN path=...` 记录里，不在标准 name 字段。
- **解决**：解析器加 PAX path 提取；显式 UTF-8 TextDecoder。

### G4. 正确密码被判"密码错误"
- **根因**：校验用 tar 原始头字节匹配 `^(02-|...)`，但 gzip 包里 tar 开头是 PAX 头 `././@PaxHeader`。
- **解决**：改用解析后的第一个文件名 `files[0].name` 校验。

### G5. 密码错误没有明确提示
- **原因**：AES-CBC 错密码偶尔填充合法不抛异常，解出乱码。
- **解决**：tar 头特征二次校验，明确显示红色"密码错误"并清空输入框。

### G6. 核心材料明文进了 Git 历史
- **解决**：`filter-branch --index-filter` 清除 4 个核心目录 → 删 `refs/original` → `reflog expire` → `gc --prune=now --aggressive` → 强推。清单桩从加密包重新生成。
- **注意**：`git ls-files` 对中文路径做八进制转义，grep 中文会**假阴性**；用 `git show --name-status` 或 Python 核对。

### G7. 加密包脱敏 / 密码管理
- **口径**：密码只从 `KIT_PASSWORD` 环境变量读，不写仓库；核心先 tar+gzip+AES-256-CBC/PBKDF2 200k+随机 salt；明文只留目录清单（文件+大小，无内容）。

### G8. `openssl enc -aes-256-gcm` 报 bad decrypt
- **原因**：LibreSSL 3.3.6 下 GCM 标签处理有问题。
- **解决**：改用 AES-256-CBC + PBKDF2。

### G9. 录屏输出重复帧 / 画面冻结
- **根因**：`ffmpeg avfoundation` 输出重复帧；Chrome 失焦时 MJPEG/SSE 渲染被节流。
- **解决**：改 `screencapture` 连续截图 + ffmpeg 合成；录制前 `open -a "Google Chrome"` 激活且不再开新 URL。

### G10. Chrome 自动化授权无效（osascript -10004）
- **根因**：系统设置里授权对象是 `.WorkBuddy.app.installing...`（安装临时进程），与实际执行进程不匹配。
- **解决**：改用内置浏览器 + URL 锚点（`#rosterGrid`/`#topoSvg`/`#msgs`）定位截图，稳定且精确到卡片。

### G11. 一键启动脚本报 unbound variable / stop 被当端口
- **原因**：① 中文全角标点紧跟 `$PORT`（如 `$PORT，`）被当变量名；② 参数解析把 `stop/status` 当端口校验。
- **解决**：所有变量统一 `${VAR}` 包裹；用 `ACTION` 变量区分动作与端口。

---

## H. 其它环境/工具坑

### H1. 远端 heredoc 定界符用 `SC` 被拦截
- **原因**：安全策略误判成 Windows `sc` 命令，整条命令被拦。
- **解决**：换其它定界符。

### H2. apt 装包 DNS 解析失败
- **原因**：实验网段无 DNS。
- **解决**：Mac 侧下载 deb（官方源，注意 USTC 的 `+deb11u2` 路径可能 404）→ `scp` → `dpkg -i`。

### H3. 进入被隔离设备的过渡通道
- **做法**：Mac 的 key `scp` 到 RK3588 `/tmp/rvkey`，再从 RK3588 ssh 过去（源地址走 `.23`）。

### H4. RKNN 转换版本要求 / Apple Silicon 转换路径
- 详见 `docs/04-principles.md` 与 `eval_platform/llm/` 记录；经验：用 x86_64 Docker 模拟，rknn-toolkit2 版本须与板端 runtime 匹配。

---

## 快速索引（按关键词）

| 关键词 | 看哪条 |
|---|---|
| ping 不通 / ARP incomplete / 扫描不到 RV | A1、A2 |
| DHCP / dnsmasq / g4-dhcp | A3、G2 关联 |
| 桥接失败 / 三层中继 | A6 |
| 画面静止 / 帧冻结 / MD5 | B3、B7、D1~D3 |
| 黑屏 streaming / 全 0 帧 | B4 |
| 重连无效 / 覆盖层卡住 | B5 |
| 设备未枚举 / usbfs / uvcvideo | B1、B2、B8 |
| 视频不可用 / 服务没跑 | C1 |
| 重启过多 / bind 失败 | C2 |
| 0.6B 照抄 / 占位符 / 串味 | E1~E6 |
| 左右布局 / 空白 / 缓存 | F1~F3 |
| GitHub Pages / 解锁 / 乱码 / 密码 | G1~G7 |
| 中文路径 git / 历史明文 | G6 |
| 录屏 / 授权 | G9、G10 |
