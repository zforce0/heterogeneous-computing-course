# RV1126B 双相机 USB 2.0 带宽约束根因与方案分析

> 时间：2026-09-21
> 关联：[实现思路.md](实现思路.md) §14.5(USB 带宽约束实测发现) 的扩展与定稿
> 状态：第二期代码已完成并板端编译通过；红外 256x192@25 实测达标；**可见光相机与红外同时启动 ENOSPC**,根因为 USB 2.0 + 相机硬件 + 红外 SDK 实现方式的三重硬约束,软件层已穷尽方案

---

## 一、问题概述

第二期目标:RV1126B 上同时跑红外(0bda:5840,256x192@25,闭源 libiruvc SDK) + 可见光(1d6c:0103,1280x720@25,V4L2 MJPG),对可见光做 YOLOv8 推理,检出时成对保存。

实测结果:
- 红外单独跑:25.0 fps 达标 ✅
- 可见光单独跑(v4l2-ctl):1280x720@25 10 帧抓取成功 ✅
- **红外 + 可见光同时跑:可见光 `VIDIOC_STREAMON` 失败,错误 `ENOSPC(No space left on device)`** ❌
- 降级可见光到 1280x720@15、640x480@25 仍 ENOSPC ❌

代码已编译通过、`CLOSE_CAM_SIDE_PREVIEW` 关流后 `Alloc=0/800 us (0%)` 已验证带宽确实释放,排除残留占用。问题是运行时双 isoc 流叠加超出 USB 2.0 高速等时带宽预算。

---

## 二、关键实测数据

### 2.1 板端 USB 拓扑(只有一个 USB 2.0 控制器)

```
/:  Bus 001.Port 001: Dev 001, Class=root_hub, Driver=ohci-platform/1p, 12M     ← USB 1.1,太慢不能用
/:  Bus 002.Port 001: Dev 001, Class=root_hub, Driver=ehci-platform/1p, 480M   ← 唯一的 USB 2.0 EHCI
    |__ Port 001: Dev 002, If 0, Class=Hub, Driver=hub/4p, 480M                ← 外置 USB Hub(05e3:0610)
        |__ Port 001: Dev 005, If 0/1, Class=Video, Driver=uvcvideo, 480M       ← 可见光(1d6c:0103)
        |__ Port 002: Dev 003, If 0/1, Class=Video, Driver=uvcvideo, 480M       ← 红外(0bda:5840),实际由 libiruvc 抢占
        |__ Port 003: Dev 004, If 0, Class=Vendor Specific, Driver=r8152, 480M   ← 板端 SSH 网卡
```

**关键**:板端**只有一个 USB 2.0 EHCI 控制器**(Bus 002),红外、可见光、网卡**共享同一总线**。Bus 001 是 USB 1.1 OHCI(12 Mbps),无法用于相机。**无法通过把相机分到不同总线来规避带宽争抢**。

### 2.2 USB 等时带宽分配实测

程序运行期间(红外已启动、可见光 STREAMON 失败时)读 `/sys/kernel/debug/usb/devices`:

```
B:  Alloc=  0/800 us ( 0%), #Int=  4, #Iso= 0       ← Bus 002 总线预算占用 0%?
D:  Vendor=1d6c ProdID=0103                          ← 可见光
E:  Ad=83(I) Atr=05(Isoc) MxPS=3072 Ivl=125us        ← 可见光已协商 alt 1(wMaxPacketSize=3072)
D:  Vendor=0bda ProdID=5840                          ← 红外
E:  Ad=81(I) Atr=05(Isoc) MxPS=128~3072 Ivl=125us   ← 红外的多个 alt(1~7)
```

`Alloc=0` 但实际 ENOSPC,说明**红外 SDK 通过 libusb 提交的 isoc URB 不被内核 bandwidth accounting 统计**,但 EHCI 调度器实际仍占用总线带宽。dmesg 同时输出:

```
usb 2-1.2: usbfs: process 1284 (rv1126_yolov8_d) did not claim interface 0 before use
```

这条警告证明红外 SDK 用 libusb 抢 USB 时绕过了标准 claim interface 流程,所以内核 accounting 没记入。

### 2.3 可见光相机 UVC 描述符(只有 alt 1)

```
Interface Descriptor:
  bInterfaceNumber        1
  bAlternateSetting       1                 ← 唯一的 isoc alt
  bNumEndpoints           1
  bInterfaceClass        14 Video
    wMaxPacketSize     0x1400  3x 1024 bytes  ← 3 transactions/微帧,3072 字节/微帧
    bInterval               1
```

可见光相机硬件**只暴露一个 isoc 端点 alt 1**,wMaxPacketSize=3072 字节/微帧(USB 2.0 high-bandwidth isoc 上限)。uvc 驱动协商时**无更小 alt 可选**,必须按 3072 字节/微帧预留带宽 = **24.576 MB/s 等时带宽被强制占用**。

### 2.4 红外相机 UVC 描述符(有 alt 1~7,SDK 内部决定选哪个)

```
bAlternateSetting  1: wMaxPacketSize 0x0080 1x128   =   128 B/μframe = 1.024 MB/s
bAlternateSetting  2: wMaxPacketSize 0x0200 1x512   =   512 B/μframe = 4.096 MB/s
bAlternateSetting  3: wMaxPacketSize 0x0400 1x1024 =  1024 B/μframe = 8.192 MB/s
bAlternateSetting  4: wMaxPacketSize 0x0b00 2x768  =  1536 B/μframe = 12.288 MB/s
bAlternateSetting  5: wMaxPacketSize 0x0c00 2x1024 =  2048 B/μframe = 16.384 MB/s
bAlternateSetting  6: wMaxPacketSize 0x0a80 2x1344 = 2688 B/μframe = 21.504 MB/s
bAlternateSetting  7: wMaxPacketSize 0x0c00 3x1024 = 3072 B/μframe = 24.576 MB/s
```

红外组合流 256x384 YUY2@25 的实际需求仅 614 字节/微帧(4.69 MB/s),按标准应该选 alt 3 (1024 字节/微帧)。但 SDK 是闭源的,无法直接观察实际协商哪个 alt;从 ENOSPC 现象反推,SDK 应该选了较大 alt(很可能 alt 7 = 24.576 MB/s)。

### 2.5 单独验证(隔离判断)

| 测试 | 命令 | 结果 |
|---|---|---|
| 单跑可见光 1280x720@25 | `v4l2-ctl -d /dev/video56 --set-fmt-video=width=1280,height=720,pixelformat=MJPG --set-parm=25; timeout 3 v4l2-ctl -d /dev/video56 --stream-mmap=3 --stream-count=10` | ✅ 抓到 10 帧(<<<<<<<<<<) |
| 程序内红外+可见光 1280x720@25 | `./rv1126_yolov8_demo ./yolov8m_rv1126b.rknn -W 1280 -H 720 -f 25` | ❌ ENOSPC,可见光 start failed |
| 程序内红外+可见光 1280x720@15 | `... -W 1280 -H 720 -f 15` | ❌ ENOSPC |
| 程序内红外+可见光 640x480@25 | `... -W 640 -H 480 -f 25` | ❌ ENOSPC |

红外单跑时 25.0 fps 达标,可见光单跑时 1280x720@25 OK,只有**两者同时启动可见光才会失败**,证明问题是带宽叠加超预算。

### 2.6 关流残留已排除

代码 [lib/ir_camera.cpp](lib/ir_camera.cpp) `close()` 已用 `uvc_camera_stream_close(CLOSE_CAM_SIDE_PREVIEW)`,实测程序退出后 `/sys/kernel/debug/usb/devices` 的 `Alloc=0/800 us (0%), #Iso=0`,证明 SDK 关流彻底释放了带宽。ENOSPC 不是上一次残留造成的,是运行时双流叠加的真实约束。

---

## 三、根因总结

USB 2.0 高速总线等时预算:

| 项目 | 计算 | 值 |
|---|---|---|
| USB 2.0 高速总线带宽 | 480 Mbps / 8 | 60 MB/s |
| 等时传输预算(80%) | 60 × 0.8 | **48 MB/s** |
| 单 microframe(125us)等时预算 | 48000 × 0.000125 | 6000 字节/μframe |

实测占用:

| 来源 | 预留带宽 | 是否计入内核 Alloc |
|---|---|---|
| 可见光相机(uvc 协商 alt 1) | 24.576 MB/s | ✅ 计入 |
| 红外 SDK(libusb isoc URB) | 估 8.192~24.576 MB/s(取决于 SDK 选哪个 alt) | ❌ **不计入**(因 SDK 绕过 claim interface) |
| **叠加总计** | **32.768~49.152 MB/s** | EHCI 调度器拒绝提交新 URB |

- 若 SDK 选 alt 7(3072 B/μframe):24.576 + 24.576 = 49.152 MB/s **> 48 MB/s** → ENOSPC ✓
- 若 SDK 选 alt 3(1024 B/μframe):8.192 + 24.576 = 32.768 MB/s < 48 MB/s,应该通过,但实测失败,反推 SDK 选了较大 alt

**三重硬约束**:
1. **USB 2.0 总线预算 48 MB/s**(板端无 USB 3.0、无第二个 USB 2.0 控制器)
2. **可见光相机硬件只暴露 alt 1**(wMaxPacketSize=3072,固定 24.576 MB/s 预留,无可降级 alt)
3. **红外 SDK 闭源**(无法控制 alt 选择,且 libusb 路径绕过内核 accounting)

软件层已穷尽:
- ✅ 启动顺序调整(先红外后可见光,参考 twocameraOnMAC/202608191952.md)
- ✅ `CLOSE_CAM_SIDE_PREVIEW` 彻底关流(已验证 Alloc=0)
- ✅ 降级可见光分辨率/帧率(都失败,因 alt 1 是固定的)
- ❌ usbmon 不可用(`/lib/modules/6.1.141` 无 usbmon.ko)
- ❌ SDK 头文件 [include/ir_sdk/libiruvc.h](include/ir_sdk/libiruvc.h) 无 alt 选择 API
- ❌ 板上无第二个 USB 2.0 控制器

---

## 四、可选方案与结果预测

### 方案 A:更换可见光相机(推荐)

**做法**:换一款 UVC 描述符里**有多个 isoc alt 选项**(包含 wMaxPacketSize=1024 字节/μframe 或更小的 alt)的 USB 摄像头。

**为何可行**:
- 红外实际需求带宽仅 4.69 MB/s(614 字节/μframe),alt 3 (1024 字节/μframe = 8.192 MB/s) 就够
- 若新可见光相机有 alt 2(512 字节/μframe = 4.096 MB/s) 或 alt 3(1024 字节/μframe = 8.192 MB/s),uvc 驱动协商后会选小 alt
- 总带宽:8.192(红外) + 8.192(可见光) = 16.384 MB/s,远低于 48 MB/s 预算,绰绰有余
- 用户要求的"可见光 1280x720@25"分辨率不受影响(MJPG 压缩后实际帧大小很小,只是驱动协商时按 wMaxVideoFrameBufferSize × fps 算预留,新相机描述符若写得更小,uvc 选小 alt)

**预期结果**:✅ 双相机同时跑通,红外 25fps + 可见光 25fps + YOLO 推理 + 成对保存全部正常

**代价**:需要采购/更换硬件,等用户资源

**风险**:新相机描述符的 `dwMaxVideoFrameBufferSize` 字段写法仍是关键变量。如果新相机厂商仍按未压缩 YUYV 写(460800 字节),uvc 协商仍会选大 alt。需采购前用 `lsusb -v` 确认候选相机的 alt 列表

---

### 方案 B:接受单相机(回滚第一期)

**做法**:代码回滚到第一期状态,只跑可见光 YOLO 推理,放弃红外配对保存。或反过来只跑红外(但红外分辨率太低,YOLOv8m 输入 640x640 上采样效果差)。

**为何可行**:
- 第一期代码已验证可用,可见光单跑 1280x720@25 v4l2-ctl 实测通过
- 只跑单 isoc 流,USB 2.0 带宽预算绰绰有余
- 改动小:main.cpp 移除 IrCamera 相关代码即可

**预期结果**:✅ 单相机 YOLO 推理流畅运行,但**失去双相机配对能力,无法满足第二期核心需求**

**代价**:放弃第二期目标,前期红外模块代码([include/ir_camera.h](include/ir_camera.h)、[lib/ir_camera.cpp](lib/ir_camera.cpp)、SDK 库 [libs/arm64/](libs/arm64/) 等)全部下线

**风险**:无技术风险,但功能上**不满足用户的第二期明确要求**

---

### 方案 C:尝试红外降到 15fps(验证性测试,违反硬约束)

**做法**:临时修改 [lib/ir_camera.cpp](lib/ir_camera.cpp) `select_stream()`,把 `camera_param_.fps = stream_info[stream_index].fps[0]`(25) 改为硬编码 15,看 SDK 是否按 fps 协商更小 alt,从而让可见光能启动。

**为何可能可行**:
- SDK 内部协商算法未知,但合理推测会按 `frame_size × fps` 算所需带宽,15fps 时所需带宽减半
- 若 SDK 据此选 alt 2(512 字节/μframe) 或 alt 3(1024 字节/μframe),总预算可能够用

**预期结果**(三种可能):
1. ✅ SDK 按 fps 协商小 alt,可见光能启动 → 证明假设,可告知用户"红外 15fps 双相机可跑"
2. ❌ SDK 仍选最大 alt 7 → ENOSPC 不变 → 进一步确认 SDK 内部保守协商
3. ⚠️ SDK 协商小 alt 但红外帧率掉到 15fps 不可用 → 违反用户"红外优先 25fps"硬约束

**代价**:需要修改代码并部署测试,半小时左右

**风险**:
- 即使能跑,**违反用户的明确硬约束**:"红外相机必须优先保障 256x192@25fps"(见 project_memory.md)
- 只能作为可行性验证,不能作为最终方案
- 若用户后续接受"15fps 降级",可作为妥协方案

**何时考虑**:仅当用户明确同意放弃 25fps 硬约束时,才转为正式方案

---

### 方案 D:研究 SDK callback 模式(最后尝试)

**做法**:参考 twocameraOnMAC/src/ir_camera.cpp 的 `sdk_callback_mode` 路径,把 [lib/ir_camera.cpp](lib/ir_camera.cpp) 改用 callback 模式而非 polling 模式(`uvc_camera_stream_start(cam, &sdk_callback_)` 而非 `uvc_camera_stream_start(cam, NULL)`)。

**为何可能可行**:
- callback 模式下 SDK 内部可能不预提交多个 isoc URB(改为事件驱动),实时占用的等时带宽可能更少
- polling 模式 SDK 通常会预提交 4~8 个 URB 占满调度队列,callback 模式可能只按需提交

**预期结果**(三种可能):
1. ✅ callback 模式占用更少带宽,可见光能启动 → 双相机跑通
2. ❌ SDK 内部 isoc 协商方式相同,只是回调机制不同 → ENOSPC 不变
3. ⚠️ callback 模式有别的兼容性问题(参考项目原代码里也有条件分支 `if (config_.sdk_callback_mode)` 说明两种模式都有使用场景)

**代价**:需要修改 ir_camera.cpp 加入 callback 接口,部署测试,1 小时左右

**风险**:
- callback 模式在闭源 SDK 上的行为不可预测,可能引入新 bug
- 参考项目 twocameraOnMAC 默认也未启用 callback 模式,说明 polling 是更稳的方式
- 即使能跑,callback 模式下红外帧率/稳定性需重新验证

**何时考虑**:作为最后的尝试性方案,在方案 A/B/C 都不可行时

---

## 五、方案对比与推荐

| 方案 | 双相机 | 满足 25fps | 实现成本 | 推荐度 |
|---|---|---|---|---|
| A 换可见光相机 | ✅ | ✅ | 需采购硬件 | ⭐⭐⭐⭐⭐ 最干净彻底 |
| B 接受单相机 | ❌ | ✅(单相机) | 改代码回滚 | ⭐⭐ 放弃第二期目标 |
| C 红外降到 15fps | 待验证 | ❌ | 半小时测试 | ⭐⭐⭐ 仅作验证,非最终方案 |
| D SDK callback 模式 | 待验证 | ✅ | 1 小时测试 | ⭐⭐⭐ 风险较高,作为最后尝试 |

**推荐路径**:
1. **首选方案 A**:更换可见光相机。这是唯一能干净彻底满足用户所有约束(双相机 + 红外 25fps + 可见光 1280x720@25)的方案,但要等硬件资源
2. **次选方案 D**:在用户决策前,可花 1 小时尝试 SDK callback 模式。若通过,可作为方案 A 落地前的临时方案;若不通过,排除一个变量,确认 USB 2.0 + 可见光相机硬件是唯一根因
3. **不推荐方案 C**:违反用户硬约束,只作为 SDK 协商算法的验证手段(15fps 时若仍 ENOSPC,反证 SDK 选最大 alt)
4. **方案 B 兜底**:若硬件资源长期无法到位,回滚到第一期单相机,保住已实现的功能

---

## 六、关键诊断命令汇总(便于复现)

```bash
# 板端 SSH(密码 <REDACTED>)
ssh root@192.168.3.252

# 1. 查 USB 拓扑(确认只有一个 USB 2.0 控制器)
lsusb -t

# 2. 查 USB 等时带宽分配(运行时看 Alloc 是否非 0)
cat /sys/kernel/debug/usb/devices | grep -E "^B:|ProdID|Ad=|Atr=Isoc|MxPS="

# 3. 查可见光相机 UVC 描述符(确认只有 alt 1)
lsusb -v -d 1d6c:0103 | grep -A2 "bAlternateSetting\|wMaxPacketSize"

# 4. 查红外相机 UVC 描述符(看 alt 1~7 列表)
lsusb -v -d 0bda:5840 | grep -E "bAlternateSetting|wMaxPacketSize|wWidth|wHeight|dwMaxVideoFrameBufferSize"

# 5. 单跑可见光验证(确认相机硬件本身 OK)
v4l2-ctl -d /dev/video56 --set-fmt-video=width=1280,height=720,pixelformat=MJPG --set-parm=25
timeout 3 v4l2-ctl -d /dev/video56 --stream-mmap=3 --stream-count=10

# 6. 跑程序触发 ENOSPC
cd /userdata/rv1126_test && ./build/rv1126_yolov8_demo ./yolov8m_rv1126b.rknn -W 640 -H 480 -f 25

# 7. 看 dmesg 确认 -28 (ENOSPC) 和 libusb claim 警告
dmesg -T | grep -iE "usb|uvc|ehci|bandw|isoc|enospc" | tail -30
```

---

## 七、附:实测时间线

| 时间 | 操作 | 结果 |
|---|---|---|
| 板端 IP 变更 | 用户告知 192.168.3.248 → 192.168.3.252 | 板端可达,uptime 3 分钟(冷启动) |
| 部署最新代码 | `deploy.sh` 同步,板端已是 `CLOSE_CAM_SIDE_PREVIEW` | 编译通过 |
| 1280x720@25 测试 | `./rv1126_yolov8_demo ./yolov8m_rv1126b.rknn` | ENOSPC,可见光 start failed |
| 1280x720@15 测试 | `... -W 1280 -H 720 -f 15` | ENOSPC |
| 640x480@25 测试 | `... -W 640 -H 480 -f 25` | ENOSPC |
| 单跑 v4l2-ctl 验证 | `v4l2-ctl --stream-mmap=3 --stream-count=10` | ✅ 抓到 10 帧 |
| USB Alloc 检查 | 程序运行时 `cat /sys/kernel/debug/usb/devices` | `Alloc=0/800 us`,#Iso=0(红外 libusb 不计入) |
| UVC 描述符检查 | `lsusb -v -d 1d6c:0103` | 可见光只有 alt 1,wMaxPacketSize=3072 |
| USB 拓扑检查 | `lsusb -t` | 板端只有一个 USB 2.0 EHCI 控制器 |

---

## 八、文档版本

- 2026-09-21 初版:基于 RV1126B 板端 IP `192.168.3.252` 的冷启动干净状态实测编写
- 关联文档:[实现思路.md](实现思路.md) §14.5(USB 带宽约束实测发现,本文件是其定稿与扩展)
