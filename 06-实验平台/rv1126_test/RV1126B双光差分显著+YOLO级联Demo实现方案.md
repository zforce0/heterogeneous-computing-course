# RV1126B 双模态差分显著 + YOLO 级联 Demo — 实现方案

> 交付对象：Code Agent（可直接据此开工）
> 基线：本文档承接《实现思路.md》**第二期（双相机拉流 + YOLOv8）**的全部成果，在其代码库上增量开发。未提及的部分（V4L2 可见光采集、红外闭源 SDK 采集、`lib/` 下的 yolov8 推理链路、构建与部署、Ctrl+C 优雅退出、10 张滚动清理）**全部沿用，不做改动**。
> 目标一句话：**在现有双光拉流基础上，增加一条「差分显著提名 → crop → YOLO 确认」的级联通路，并用可量化指标证明它的价值。**

---

## 📌 修订说明 v2（2026-09-30）— 硬件条件变更

> **变更事实**：可见光走一路 USB，红外走另一路 USB，**两路独立，不再争抢带宽**。
> 因此 v1 里「可见光必须降到 640×480 以规避 ENOSPC」这条**强制约束作废**，连带若干由此推导出的设计也要改。

| # | v1 做法 | v2 做法 | 涉及章节 |
|---|---|---|---|
| 1 | VIS 强制 640×480@25（避开 USB 带宽） | **改为按用途选档**，默认目标 **1280×960@25**（首选 4:3）；自动降级链 | §1.2 |
| 2 | IR 必须先于 VIS 启动（总线抢占） | **不再是硬约束**，默认仍 IR 优先，但提供 `--start-order parallel` | §5.4 |
| 3 | VIS/IR 同为 4:3，映射可偷懒 | VIS 可能是 16:9，**FOV 必须显式按宽高比拆到分轴** | §3.2 |
| 4 | crop 的 `min_size=96` 写死像素 | **crop 尺寸必须随 VIS 分辨率换算**，改在角度域恒定 | §3.3 |
| 5 | 大比例降采样（2.5×）成本可忽略 | VIS 升到 5× 缩放，给出**两级 `INTER_AREA` 降级方案**与 `sal_ms` 超标处理 | §4.3 |
| 6 | — | 新增 **双路带宽稳定性验收**（P0.5）与 MJPG 解码 CPU 风险 | §1.2 / §12 / §14 |

> **除上述 6 处，其余章节（算法管线、四象限、指标定义、输出契约、验收判据）完全不变。**
> 特别注意：**判决指标 FAR ≥ 0.50 与 NPU 压缩比 ≤ 0.05 不受分辨率变更影响**——它们比较的是提名数量比，不是像素。

---

## 0. 三句话总览

1. **它不是第二个 YOLO demo。** 现有 demo 是「逐帧全图跑 YOLO」；本 demo 是「先用差分显著把 NPU 调用从 25 次/秒压到几次/分钟」，A/B 对比是核心交付物。
2. **第一目标不是识别准不准，是「虚警削减率」和「NPU 调用压缩比」**——这两个指标**不需要任何标注**，是本 demo 能否成立的唯一判决依据。
3. **在开始写代码之前，先解开一个阻塞项**：红外 SDK 到底给不给得出真实温度矩阵（见第 2 章）。给得出是「测温管线」，给不出只能退化为「灰度管线」，两者可输出的天花板差一个量级。

---

## 1. 现状盘点（基于你的第二期实测）

### 1.1 可直接复用的资产

| 资产 | 位置 | 本 demo 的用法 |
|---|---|---|
| 红外采集（闭源 SDK / libusb） | `ir_camera.{h,cpp}` + `libs/arm64/` | **不动**，新增帧环形缓冲（第二期已有） |
| 可见光采集（V4L2 + MJPG） | `visible_camera.{h,cpp}` + `mjpeg_decoder` | **不动**，仅改默认分辨率档位（见 §1.2）。**v2：两路 USB 独立，无需再为带宽降分辨率** |
| YOLOv8 RKNN 推理 + 后处理 | `lib/yolov8.cpp`、`yolov8_postprocess.cpp` | **不动**，新增「对 crop 推理」的调用入口（复用同一个 ctx） |
| 三线程架构 / 最近邻配对 / 成对存图 / FPS 统计 / Ctrl+C | `main.cpp` 等 | 保留，**在其上叠加新线程与新日志** |
| OpenCV 4.6（含 NEON） | 板端系统自带 | 差分显著**只依赖 OpenCV**，**不引入任何新第三方库** |

> ⚠️ 对 Code Agent 的重要约束：**本 demo 除 OpenCV 外不得新增任何依赖。** 显著性算子（GaussianBlur / absdiff / resize / connectedComponentsWithStats / threshold）OpenCV 全都有。不需要 libtorch、不需要 ONNX Runtime、不需要自己写 NEON intrinsic。

### 1.2 🆕 分辨率策略（v2 重写：带宽约束已解除）

**硬件现状**：VIS 一路 USB、IR 另一路 USB，互不抢带宽。所以 v1 的「必须降到 640×480」不再成立。
分辨率现在由**用途和 CPU 预算**决定，不再由总线决定。

#### 三条取舍原则

1. **优先 4:3**：IR 是 256×192 = 4:3。VIS 同为 4:3 时，映射退化为一个**各向同性缩放**（§3.2 里 `kx == ky`），少一个出错环节。**这是首选 1280×960 而不是 1280×720 的唯一理由。**
2. **够用就停**：差分层只用 VIS 降采样到 256×192 的版本；VIS 原生分辨率**只影响 crop 送进 YOLO 的细节**。而 YOLO 输入会被 letterbox 到 640 —— crop 边长超过 640 后，再提高 VIS 分辨率**几乎没有收益**。所以不是为了高而高。
3. **MJPG 解码成本要算**：分辨率上去后 CPU 侧解码/拷贝开销线性上升，会和显著线程抢 CPU。**建议上限 1280 宽。**

#### 分辨率档位表

| 档位 | 分辨率 | 宽高比 | 每 IR 像素对应 VIS 像素（宽/高） | 评价 |
|---|---|---|---|---|
| `960p` | **1280×960 @25** | **4:3** | 5.0× / 5.0× | **★ 首选**。与 IR 同宽高比 → **各向同性缩放**；crop 细节最充分 |
| `768p` | 1024×768 @25 | 4:3 | 4.0× / 4.0× | 次选，CPU 更省，同为各向同性 |
| `720p` | 1280×720 @25/30 | 16:9 | **5.0× / 3.75×（各向异性）** | 可用，但必须走 §3.2.1 的分轴 FOV 换算（`kx ≠ ky`）；且垂直有效视场小于 IR，上下边缘无热覆盖 |
| `600p` | 800×600 @25 | 4:3 | 3.125× / 3.125× | 保守档，稳定性最好 |
| `480p` | 640×480 @25 | 4:3 | 2.5× / 2.5× | v1 默认值，仍作为**兜底档**保留 |

**自动选择（`--res-preset auto`，默认）**：启动时用 V4L2 枚举该设备支持的 MJPG 档位，按上表顺序
`1280×960 → 1024×768 → 1280×720 → 800×600 → 640×480` 取**第一个协商成功**的档位，并把实际值打印到 `[INIT]`:

```
[INIT] vis requested 1280x960@25 MJPG -> negotiated 1280x960@25 OK (4:3)
[INIT] ir  256x384 YUY2 (-> image 256x192 + temp 256x192)
[INIT] vis_fov(diag)=80 -> fov_x=64.0 fov_y=49.0   (aspect 1.333)
```

枚举命令（给 Code Agent 自查用）：

```bash
v4l2-ctl -d /dev/videoX --list-formats-ext | grep -A2 MJPG
```

#### 双路稳定性验收（必做，0.25 天）

即使带宽不再是问题，**也必须先验证两路全速并发不抖动**——这是后面所有指标可信的前提：

```
连续跑 5 分钟，要求：
  IR  avg fps >= 24.5   且 单秒 cur fps 最低 >= 23
  VIS avg fps >= target × 0.98
  两路 skip_align 占比 < 1%
```

> **不达标就降一档**，不要硬扛。若 IR 在 VIS 高分辨率下抖动，说明内部仍共享某个资源（主机控制器/内存带宽），此时退回 `1024×768` 或恢复 `ir-first`。

#### 一个免费的副产品

带宽富余后，**可以同时把两路原始帧落盘做离线回放**（见 §8.3）。这对后续标注、离线标定 §3.4、以及难例回流都有用——越早开始积累越好。

### 1.3 模型：不用换

你们的 `yolov8m_rv1126b.rknn` 单帧 ~71 ms。逐帧跑嫌慢，**但级联模式下 NPU 一分钟只被调用几次，71 ms 完全不是瓶颈**。因此：

- **保持 yolov8m 不变**（甚至比 n 更准，对 crop 这种小目标友好）。
- 只有在 `--mode baseline`（A/B 对照的逐帧基准）里慢一点，而这恰恰是我们要展示的对比项。

> 附带好处：crop 被 letterbox 到 640 输入，等于**把小目标放大了再送进网络**，对提名区这种小目标反而是有利的。

> **v2 补充**：VIS 提到 1280×960 后要注意一件事——**同一个提名 crop 的像素数变大了**。若 IR 侧一个提名框放大 3 倍 margin 后，映射过去超过 `--max-crop`（默认 1024），必须截断，否则会出现超大 crop 拖慢推理（`infer_ms` 飙升）。§3.3 已给出换算规则。

---

## 2. 🔴 阶段 0（阻塞项）：先确认红外温度数据到底能不能拿到

**这是整个 demo 的地基。在它没确认之前，不要写任何算法代码。**

### 2.1 背景与重要性

你的第二期实测已经确认：

> SDK 必须协商 **256×384 YUY2**（image + temp 上下拼接），`raw_data_cut` 再切出上半段 256×192。

这意味着**下半段 256×192 是某种温度数据**。而 `twocameraOnMAC` 里存在 `libirtemp.so`（带 `tau_H.bin` / `tau_L.bin` 校正表）——强烈暗示 SDK 支持辐射测温输出。

**为什么这件事决定一切：**

| 情形 | 能做什么 | 不能做什么 |
|---|---|---|
| **A. 拿到真实温度矩阵**（float / 定标后的 16-bit，单位 °C 或已知 LSB→K 系数） | ΔT、t_max、热点判据、**dT/dt 稳态外推、分区热基线、趋势预测、国标 δ 映射** ← 完整方案 | — |
| **B. 只拿得到灰度 Y 通道** | 提名、差分、Q1/Q2/Q3 划分、cut UCSD crop | ❌ 任何绝对温度/ΔT 数值；❌ 宣称"测温" |
| **C. 只能拿伪彩 RGB** | 几乎什么都别做 | ❌ 灰度已被调色板扭曲，显著性失去物理意义 |

**C 是红线，必须避免。** B 是及格线，A 是目标线。

### 2.2 探针任务（预计 0.5–1 天，先做）

写一个一次性小程序 `ir_temp_probe`（或直接改 ir_temperature_test），做三件事：

1. **枚举 API**：读 `include/libirtemp.h` 与 `libirparse.h`，列出所有返回温度/浮点数组的函数签名，记录候选函数名。对照 `test/ir_temperature_test/` 的样例看它怎么调用。
2. **原始数据 dump**：把 256×384 的 YUYV 原始 buffer 落成文件（`frame_000.bin`），同时 dump 切分后的上半段（图像）与下半段（疑似温度）。检查下半段的位深与排布（是每像素一个 uint16？还是 YUYV 里的 Y 一个字节？有没有高低字节交错？）。
3. **可控温标定实验**（决定性）：
   - 准备三个已知温度的参考目标：**冰水混合物（0 °C）**、**室温物体**、**热水杯（50–70 °C，可用温度计确认）**。
   - 依次让它们**充满画面**，记录下半段的**整帧均值统计**（min / max / mean / std）。
   - 三组数据做**两点线性拟合**，得到 `LSB → °C` 的斜率与截距。

> **验收判据**：若三个温度点拟合出的**线性度 R² > 0.99**，说明下半段是线性编码的温度/辐射量 → **情形 A**，继续。
> 若下半段数据在不同目标下**几乎无变化**，或变化不符合 0<室温<热水 的单调顺序 → 那是伪彩或图像冗余 → **情形 B**，退化为灰度方案，且文档里必须明确标注"本 demo 不输出温度值，只输出提名"。

### 2.3 探针输出物

- `docs/ir_temp_probe_report.md`：API 清单、dump 说明、三点标定数据表、拟合结果、结论（A/B/C 三选一）。
- 若 A：额外产出标定常量 `#define IR_TEMP_SCALE ...` `IR_TEMP_OFFSET ...`，写进 `include/ir_temp_conf.h`。

> ⚠️ **若探针结论为 B 或 C，请回来找我**——此时第 4 章的属性包里所有温度字段需要改为可选，且 demo 的宣传口径要降级。不要在没确认的情况下先写死温度字段。

---

## 3. 配准（Modality Registration）

### 3.1 设计原则：先给"够用的近似"，把精确标定做成可选项

Demo 阶段不需要亚像素级精度。理由来自前期调研：

> **红外的低分辨率，已经把配准从"亚像素难题"降级为"整像素工程问题"** —— 红外侧 1–2 px 的误差，在**带 3 倍 margin 的 crop** 上完全可被吸收。

**v2 补充（重要，回答"提高 VIS 分辨率会不会让配准更难"）**：

> IR 仍是 256×192，但 VIS 从 640×480 提到 1280×960 后，**1 个红外像素对应的 VIS 像素块从 2.5×2.5 变成 5×5**。
> 表面上配准误差的绝对值放大了一倍 —— **但 crop 的物理尺寸也同步放大一倍，相对误差完全不变。**
>
> **结论：提高 VIS 分辨率不需要更精确的标定，也不会让配准变难。** 不要在这一步过度设计。

### 3.2 默认映射：以主点为中心的纯缩放（近似）

输入参数：`--ir-fov`（默认需从模组规格查，常见 56°，未知则先用 57），`--vis-fov`（默认 80，USB 相机典型值，可用命令行覆盖）。

小角近似下的映射（IR ↔ VIS，双向同构）：

```cpp
// IR 像素 (u_ir, v_ir) -> VIS 像素 (u_vis, v_vis)
// fov_x/fov_y 必须是【分轴】FOV、且已换算到各自实际宽高比（见 §3.2.1）
double kx = tan(vis_fov_x / 2.0) / tan(ir_fov_x / 2.0);   // 视场越大，同样角度落在更小的相对位移上
double ky = tan(vis_fov_y / 2.0) / tan(ir_fov_y / 2.0);
u_vis = (W_vis / 2.0) + (u_ir - W_ir / 2.0) * (W_vis / W_ir) / kx;
v_vis = (H_vis / 2.0) + (v_ir - H_ir / 2.0) * (H_vis / H_ir) / ky;

// 反向（VIS 框 -> IR 网格，给相邻整数化用）
u_ir = (W_ir / 2.0) + (u_vis - W_vis / 2.0) * (W_ir / W_vis) * kx;
```

> 当两边同为 4:3 且 FOV 都按对角给时，`kx == ky`，映射退化为一个**以主点为中心的各向同性缩放**。这就是 §1.2 建议优先 1280×960 的原因。

> FOV 需按各自宽高比换算，或直接让用户给定水平 FOV。**这两个参数不准，crop 位置就会偏，但不影响 demo 的核心指标——因为 crop 的 margin 会吸收系统性偏移。**

#### 3.2.1 🆕 FOV 分轴换算（v2：VIS 不再保证 4:3，这一步从"可选"变成"必做"）

模组手册通常给**对角 FOV**（少数给水平 FOV）。必须按各自实际宽高比拆到分轴，否则横向/纵向缩放比不一致：

```cpp
// dfov: 对角 FOV(度); W,H: 该相机实际像素; 返回 fov_x, fov_y (度)
static double TanFromDeg(double d) { return std::tan(d * CV_PI / 360.0); }  // tan(dfov/2)

void diag_to_axis(double dfov, int W, int H, double& fov_x, double& fov_y) {
    double aspect = (double)W / H;
    double tan_d  = TanFromDeg(dfov);
    double tan_y  = tan_d / std::sqrt(1.0 + aspect * aspect);
    double tan_x  = tan_y * aspect;
    fov_y = 2.0 * std::atan(tan_y) * 180.0 / CV_PI;
    fov_x = 2.0 * std::atan(tan_x) * 180.0 / CV_PI;
}
```

CLI：`--fov-unit diag|axis`（默认 `diag`）
- `diag`：用户给的是对角 FOV，走上面换算，**宽高比取实际协商出的 W/H**（16:9 与 4:3 结果差很多）
- `axis`：用户已分别给了水平/垂直 FOV，直接用（旧参数 `--ir-fov-x/-y` 保留兼容）

> ⚠️ **最容易犯的错**：手册写 "FOV 80°"（对角），却当成水平 FOV 用。结果是横向 crop 系统性外扩。
> **但它不影响 FAR 指标**——因为 §3.3 的 3 倍 margin 会吸收这个偏移。**它只影响 demo 图好不好看、以及能不能顺手截断超大 crop。**

#### 3.2.2 有效视场：只有 IR FOV ∩ VIS FOV 是可用区

两者 FOV 不一致时（典型：VIS 80°、IR 57°），**VIS 画面中超出 IR 视场的部分没有热覆盖**，那里永远不会有提名。

```cpp
// 提名只在 IR 网格内产生，再映射到 VIS —— 所以天然落在交集内，无需额外裁剪
// 但 viz 图上建议画一个可调的矩形框标出 IR 有效区，避免客户误以为边缘没检测到
```

> demo 图建议加这一框。**否则客户会问"为什么画面角落那个热水杯没标出来"——答案是那块压根不在红外视场里。**

### 3.3 crop 的 margin 策略（必须做）

不要把提名框直接当 crop。**必须用 context margin 扩展后再裁**，否则 YOLO 看到的是一个没有上下文的高倍放大块，检测效果会很差。

```cpp
// 以提名 bbox 中心扩展：长边 × margin_ratio，且不小于 min_size
int   side_ir = std::max(box_ir.width, box_ir.height);

// ★ v2：crop 尺寸必须随 VIS 分辨率换算，不能写死像素
double scale   = (double)W_vis / W_ir;                          // 1280/256 = 5.0
int    ctx_vis = (int)std::ceil(side_ir * cfg.margin_ratio * scale);
ctx_vis = std::max(ctx_vis, (int)(cfg.min_crop_ir * scale));    // min_crop_ir 默认 20 (IR px)
ctx_vis = std::max(ctx_vis, cfg.min_crop_px);                   // 绝对下限 96
ctx_vis = std::min(ctx_vis, cfg.max_crop_px);                   // 上限 1024 ← v2 新增，防超大 crop
cv::Rect roi = clamp_to_image(center_vis - cv::Point(ctx_vis/2, ctx_vis/2),
                              cv::Size(ctx_vis, ctx_vis));
```

> **为什么 v2 必须改这里**：同一个物理目标，在 640×480 下占 60 px，在 1280×960 下占 120 px。
> 若 `min_size` 仍写死 96，**高分辨率下 margin 的相对角度覆盖会缩水一半**，YOLO 看到的就是缺少上下文的放大块。
> **原则：crop 尺寸要在【角度域】恒定，不是在【像素域】恒定。**
>
> `max_crop` 是新增的安全阀：1280×960 下，一个 40 IR px 的大目标 ×3 margin ×5 缩放 = 600 px，已经够大；
> 更大的目标（如整个人）会到 1000+ px，而 YOLO 输入只吃 640 —— 超大 crop 纯属浪费 CPU 和推理时间。

再加上 **近距 margin 补偿**（视差项 `Δ ≈ b·f/d`，b 为双光基线，近距时最大）——demo 阶段用固定值即可，不必做真实深度。

### 3.4 可选精确标定（P3，非阻塞）

若后续要精度：红外用**带加热的专用红外标定板**（不要用普通棋盘格——常温下几分钟就失效，这是前期调研踩过的坑），可见光用普通棋盘格同时成像，取 4–8 组对应点，OpenCV `findHomography`，存 `homography.yaml`。接口预留 `--homography <file>`，没传就用 §3.2 近似。

---

## 4. 算法管线设计（核心章节）

### 4.0 一句话定位

> **红外不做"这是什么"，只做"看这里"；可见光不做"找"，只做"认"。**

这是整个 demo 的架构判据。任何"先把红外上采样再融合"的写法都是错的——在 1280×960 的 VIS 下会产生 **25 倍** 假像素（`(1280/256)²`；v1 的 640×480 时是 6.25 倍），而且**违反温度管线严禁超分的铁律**。

### 4.1 数据流总览

```
IR 线程 ──► IR ring (256×192 温度矩阵 T + 时间戳)
                            │
                            ├─► 最近邻配对(delta ≤ 20ms)
VIS 线程 ► VIS ring (640×480 BGR + 时间戳)
                            │
                            ▼
                  ┌──────────────────────┐
                  │  显著线程 (CPU/NEON)  │   ← 本 demo 新增核心
                  │  T → S_tir            │
                  │  G↓256×192 → S_rgb    │
                  │  归一化 → 四象限       │
                  │  连通域 → 属性包       │
                  │  时序维(P2)           │
                  └──────────┬───────────┘
                             │ Nomination 队列(有界, 丢弃最旧)
                             ▼
                  ┌──────────────────────┐
                  │  主线程 (NPU)         │
                  │  crop → letterbox     │
                  │  → yolov8 (现有 lib)  │
                  │  → 填 semantic_slot   │
                  │  → 事件JSONL + 存图   │
                  └──────────────────────┘
```

**关键点：显著线程跑在 CPU，主线程跑 NPU，两者天然并行，互不抢占。** 这就是为什么前面说「3 TOPS 有 95% 余量」。

### 4.2 Step 1：TIR 显著性（作用在温度矩阵上）

```cpp
// 输入：Mat T (CV_32F, IR_W×IR_H)，单位 °C（情形 A）
Mat bg  = cv::GaussianBlur(T, bg_size);   // 用 T 的背景估计
Mat S_tir = cv::abs(T - bg);              // FT: 偏离"周围应有的样子"
```

> 若情形 B（只能拿灰度），把 `T` 换成灰度矩阵的 float 版。**注意此时 `S_tir` 是灰度显著，不是温度显著，字段名要保持一致但在 JSON 里标 `radiometric=false`。**

**算子选择的教训（前期实拍踩过，务必遵守）：**
> ❌ 不要用 LC（局部对比，`|I - boxFilter(I)|` 小核）——它**只点亮边缘**，大块均温区域**内部为零**，导致"晒热的石头/路面"这类大面积虚警无法被空间差分剔除。
> ✅ 用 **FT**（偏离全图均值/大核背景，点亮整个区域）。
> SR（频谱残差）在 256×192 上偏贵且收益不明显，**不推荐**。

另外建议叠加 **THT（三角直方图阈值）作为物理阈值**：对温度直方图做三角法求出背景峰值温度 `T_bg_mode` 与阈值 `t_th`。它的好处是阈值**带物理量纲**（"高于背景峰值 X K"），可解释性强，且工业界实证它是最鲁棒的热异常分割法之一。

```cpp
// 提名掩膜两种取法，二选一（默认 tht，见 §4.2 说明）
// (a) 自适应： S_tir > mean + k*std，k 默认 1.0
// (b) THT  :   T     > T_bg_mode + delta_min，delta_min 默认 3.0 K   ← 推荐，物理可解释
```

### 4.3 Step 2：VIS 显著性（降采样到同分辨率）

```cpp
cv::Mat G_gray;                                     // VIS 灰度（原分辨率）
cv::cvtColor(vis_frame, G_gray, cv::COLOR_BGR2GRAY);
cv::resize(G_gray, G_small, cv::Size(IR_W, IR_H), 0, 0, cv::INTER_AREA);  // 例如 1280×960 → 256×192
cv::Mat bg_r  = cv::GaussianBlur(G_small, bg_size);
cv::Mat S_rgb = cv::abs(G_small - bg_r);
```

> **为什么必须降采样到 IR 分辨率？** 因为要在**同一个格点上做逐像素比较**。这不叫"上采样融合"，这是**在较低分辨率的公共网格上做差分**——高分辨率信息只在最后 crop 时才被取出来（§4.6）。这正是"红外做索引，可见光做查表"。

> 🆕 **v2：降采样成本与降级手段**
> VIS 提到 1280×960 后是一次 **5× 的大比例降采样**。单次 `INTER_AREA` 质量最好，代价略高。
> 若 `sal_ms` 超标（`> 5 ms`），改成**两级降采样**：
> ```cpp
> cv::resize(G_gray, G_mid, cv::Size(512, 384), 0, 0, cv::INTER_AREA);
> cv::resize(G_mid,  G_small, cv::Size(IR_W, IR_H), 0, 0, cv::INTER_AREA);
> ```
> ⚠️ **永远不要用 `INTER_LINEAR` 做 ≥3× 的降采样**——会跳过像素导致混叠，显著性统计直接失真（大目标边缘会被抽稀成虚线）。
> 另外：**灰度转换可以放到采集线程做**，显著线程直接消费灰度图，省一份 cvtColor。

### 4.4 Step 3：归一化 + 四象限

**关键纪律：两个"显著"量纲不同，直接相减没有物理意义。** RGB 显著量的是纹理/颜色对比，TIR 显著量的是温度对比。正确做法是——**两路跑完全相同的算子，各自独立归一化到 [0,1]**：

```cpp
// 稳健归一化：用 P99 做分母，避免单个极值点压扁整张图
double p99_t = percentile(S_tir, 0.99);
double p99_r = percentile(S_rgb, 0.99);
Mat Sn_tir = S_tir / std::max(p99_t, 1e-6);   cv::min(Sn_tir, 1.0f, Sn_tir);
Mat Sn_rgb = S_rgb / std::max(p99_r, 1e-6);   cv::min(Sn_rgb, 1.0f, Sn_rgb);
```

四象限（**全分辨率 256×192 上逐像素判定**）：

| 象限 | 条件 | 物理含义 | 本 demo 的动作 |
|---|---|---|---|
| ① | Sn_tir > th_t **且** Sn_rgb ≤ th_r | **热显著，视觉平淡** = 热异常候选 | **提名**（巡检模式主角） |
| ② | 两者都 > | 一致性区 = 有形状有纹理的普通热目标 | **提名**（庭院/活体模式主角）；巡检模式仅计数 |
| ③ | Sn_tir ≤ th_t **且** Sn_rgb > th_r | **热不可知区** | **绝不判死**。只标 `degraded`，交给 RGB 全图 YOLO 兜底 |
| ④ | 两者都 ≤ | 背景 | **故意零输出**，只更新背景统计 |

**第③象限的重要性（不要偷懒跳过）：** 那里藏着 thermal crossover——LWIR 靠对比度工作，ΔT 过零时目标在热像中**直接消失**（每天两次，各 1–2 小时，实测探测距离下降 70%+）。**它是场景的物理属性，不是设备故障，NETD/NUC/增益调整都救不了。** 所以它必须是"降级"而不是"无目标"。

**阈值取法（`--thr-mode`，默认 `tht`）：**

| 模式 | TIR 侧 | RGB 侧 | 说明 |
|---|---|---|---|
| `tht`（**默认**） | `T ≥ T_bg_mode + delta_min`（默认 +3.0 K） | `Sn_rgb > mean + k·std`（k 默认 1.0） | TIR 阈值**带物理量纲**，demo 讲得清楚 |
| `adaptive` | `Sn_tir > mean + k·std` | 同上 | 两侧同构，便于调试 |
| `otsu` | `cv::threshold(..., THRESH_OTSU)` | 同上 | 极端场景下的兜底 |

> **为什么 TIR 侧默认不用自适应？** 因为 `mean + k·std` 的阈值没有物理量纲，客户问"你这个阈值代表什么"时答不上来；而 `T_bg_mode + 3.0 K` 可以直接翻译成「比周围背景高出 3 度以上的地方」。这是销售场景里差别很大的一句话。

**后处理顺序（顺序写反会出 bug，前期 python 验证踩过）：**
```
Q1 mask → morphologyEx(MORPH_CLOSE, 3×3) → connectedComponentsWithStats → 面积过滤
```
> ⚠️ 必须先闭运算再连通域。先加差分约束会让一个目标被碎片化成多个连通域，导致 Q1 计数**反而高于**单模态提名数，统计完全失真。

### 4.5 Step 4：属性包（每个提名必须带的字段）

```cpp
struct Nomination {
    uint64_t    id;                 // 全局自增
    double      t_emit_ms;          // 事件产生时刻 (CLOCK_MONOTONIC_RAW)
    int         quadrant;           // 1 / 2 / 3

    // --- 几何（IR 网格坐标）---
    cv::Rect    box_ir;             // IR 上的 bbox
    cv::Point2f centroid_ir;
    int         area_px;            // IR 像素数
    cv::Rect    roi_vis;            // 映射到可见光并加 margin 后的 crop 区域

    // --- 温度（情形 A 才有；情形 B 全部填 NaN，JSON 里省略）---
    float       t_max;              // 提名区内最高温 (°C) —— 观测值
    float       t_p99;
    float       t_ref;              // 提名区外环背景温度中位数
    float       delta_t;            // t_max - t_ref   ← 最有价值的单个数字
    float       thermal_gradient;   // 区内最大梯度 K/px
    float       t_lower_bound;      // 【必须】保守下界，= t_max（读数只会偏低不会偏高）
    float       t_true_est;         // 【必须】SSE 修正后的真值估计，见 §4.5.1

    // --- 显著性 ---
    float       sal_tir_mean;
    float       sal_rgb_mean;
    float       sal_diff;           // sal_tir_mean - sal_rgb_mean

    // --- 时序（P2）---
    float       persistence_s;      // 持续存在时长
    float       dT_dt;              // K/min，滑动线性拟合
    float       t_std;              // t_max 时序标准差（活体判据之一）
    float       micro_motion;       // VIS crop 内帧间差均值（活体判据之二）

    // --- 可信度/健康 ---
    float       scene_contrast_k;   // 场景实测热对比度 = P99 - P1 of T
    int         crossover_risk;     // 0/1/2  低/中/高
    int         valid;              // 1 ok / 0 degraded / -1 invalid
    std::string invalid_reason;     // "nuc_freeze" / "crossover" / "low_contrast" / "occlusion"
};
```

#### 4.5.1 `t_lower_bound` 为什么必须有（最容易被忽略、也最容易出事的一条）

前期实测的物理规律：**当目标占像素不足时，温度读数永远偏低，从不偏高。**

| 目标占像素 | 温度误差（真值 68 °C） |
|---|---|
| 7.4 px | +0.04 K ✅ |
| 3.7 px | −0.22 K |
| 1.5 px | **−7.35 K** ❌ |
| 0.7 px | **−22 K** |

> **最危险的组合是"检测还在、读数已死"**——68 °C 的实际故障被报成 32 °C，下游按阈值判断会直接漏检。

因此每个提名**必须**同时输出三个字段：

| 字段 | 含义 | 计算 |
|---|---|---|
| `t_max` | 观测读数 | 直接取区域内温度最大值 |
| `t_lower_bound` | **保守下界** | `= t_max`。因为已知读数只会偏低不会偏高，**观测值本身就是下界** |
| `t_true_est` | 真值估计 | `t_ref + (t_max - t_ref) / f(area_px)`，见下 |

修正因子用保守式，demo 阶段足够：

```cpp
// 目标占像素越少，读数的低估越严重；f 越小估计越高
float f = std::clamp(area_px / 25.0f, 0.5f, 1.0f);   // 25px 及以上认为读数可信
float t_true_est = t_ref + (t_max - t_ref) / f;
```

> 说明：实测 1.5 px 时读数低约 7 K，`f=0.5` 会把估计往上抬一倍残差，方向正确且偏保守。
> **注意 `t_true_est` 是估计值不是测量值。** 对外输出时必须保留三个字段，让下游自己选择用哪个——这是§4.6 里"我们只测量不判定"的直接体现。

### 4.6 Step 5：crop → YOLO（NPU）

```cpp
Mat crop = vis_frame(roi_vis).clone();       // 640×480 原图上裁（保留清晰度）
// ⚠️ 不要对 IR 做上采样送进网络 —— 那是"给 AI 喂假像素"
yolov8_detect_run(&ctx, crop, results);      // 复用现有接口，内部 letterbox→640
for (auto& r : results) {
    // 把 r.box 从 crop 坐标映射回 vis 全图坐标，再映射回 IR 坐标
    fill_semantic_slot(nom, r);
}
```

**`semantic_slot` 的字段纪律（法律责任相关，不要改）：**

```cpp
struct SemanticSlot {
    std::string label;        // YOLO 类名
    float       score;        // 注意：这是 YOLO 的输出，不是我们的判定
    std::string filled_by;    // "vendor_default" | "customer_model" | "none"
};
```

> **接口纪律**：整体 JSON 里用 `deviation` / `saliency` / `sal_diff` 这样的词，**不要用 `confidence` 描述我们自己的输出**（后者听起来像判定）。`score` 只在 `semantic_slot` 内部出现，且必须标明 `filled_by`。
> 当 YOLO 在 crop 里**什么都检不出时，`label="none"` 是正常的、甚至是好的结果**——那正是"热异常 + 视觉无物"的证据，应提升而不是降低该提名的可疑度。

### 4.7 Step 6：时序维（P2，可选但强烈建议做——它是庭院/活体方向的判决点）

空间差分**剔除不掉晒热的岩石**（它同样热、同样视觉显著，稳稳落在第②象限）。只有时间维能分开：

| 特征 | 活体（刺猬/猫） | 晒热岩石/路面 |
|---|---|---|
| 温度变化率 | **接近零**（恒温动物热平衡） | 显著单调升/降 |
| 微动 (`micro_motion`) | **非零**（呼吸/位移） | 零 |
| t_max 时序标准差 | 小但有呼吸级脉动 | 接近零 |

实现方式**全是滑动递推统计量，是确定性的、可审计的**（这一点很重要：它属于"参数层自适应"，不属于"权重层学习"，所以允许进 demo）。

**帧间关联**：用质心最近邻链接（IR 网格上距离 < `--link-radius`，默认 8 px），维护 `track_id` 与环形历史缓冲（默认 32 帧 ≈ 1.3 s @25fps）。

**活体打分（庭院模式使用）：**

```
score_living = 1.0
  × in_range(t_max, [26, 40])              // 表观体温带，CLI 可调（皮毛/衣物会使表观温度偏低）
  × (|dT/dt| < 0.5)                        // K/min
  × (micro_motion > motion_thr || t_std > std_thr)
```

---

## 5. 线程架构与改动点

### 5.1 线程清单（在原三线程上加一个）

| 线程 | 职责 | 是否新增 | CPU/NPU |
|---|---|---|---|
| IR 采集 | SDK 拉帧 → 解码/温度转换 → ring | 沿用 | CPU（低） |
| VIS 采集 | V4L2 MJPG → libjpeg → ring（**建议顺便产出灰度图**） | 沿用（改默认分辨率档位，见 §1.2） | CPU（**中**：MJPG 解码随分辨率上升，上限建议 1280 宽） |
| **显著（Saliency）** | **配对 → S_tir/S_rgb → 四象限 → 属性包 → 时序** | **新增** | **CPU/NEON**（目标 ≤ 5 ms/帧） |
| 主推理 | 消费提名队列 → crop → YOLO → 日志/存图 | 沿用（主循环改造） | **NPU** |

### 5.2 队列与背压（防止 demo 跑飞）

```cpp
// 有界队列，容量 32；满了丢弃最旧的提名（宁丢提名也不能让内存涨）
// 实现：std::deque<Nomination> + mutex + condvar，push 前若 size >= queue_cap 则 pop_front
```

> **背压策略必须是"丢弃"，不能是"阻塞"**——否则慢的 NPU 会反压显著线程，进而拖累 IR/VIS 采集。这与前期"下限不消费上限输出"的单向依赖铁律一致。

### 5.3 配对（沿用第二期机制）

IR ring 与 VIS ring 已存在（IR 8 帧 / VIS 4 帧）。显著线程每次取**各自 ring 中时间戳最近的一对**，要求 `|t_ir - t_vis| ≤ 40 ms`，超限则**跳过本轮**并计一次 `skip_align`（退出汇总要打印）。

> 注意：显著线程的处理速率（目标 < 5 ms/帧）高于采集帧率，所以理论上可以处理每一对；若处理不过来就自然降采样，不影响 IR/VIS 采集。

### 5.4 启动顺序（v2：不再是硬约束，但保留 IR 优先作为默认）

v1 之所以要求**必须 IR 先启动**，是因为两路共用同一根 USB 总线的等时带宽，VIS 先开会挤掉 IR 的 ISO 流。
**现在两路各占一路 USB，该抢占已消除**，所以这条从"硬性要求"降级为"保守默认值"。

新参数：`--start-order ir-first|parallel`（默认 `ir-first`）。

```
ir-first (默认，演示/CI 用)          parallel (可选，需先通过 §1.2 稳定性验收)
─────────────────────────────       ──────────────────────────────────────
1. open(IR)  → start(IR)             1. open(IR) 与 open(VIS) 并发
   等首窗 cur ≥ 24.5 fps                 → start(IR) / start(VIS)
   (3 s 超时则报错退出)              2. 各自等首窗达标，各自超时保护
2. open(VIS) → start(VIS)            3. 启动显著线程
   目标分辨率由 §1.2 档位表决定       4. 主循环开始消费提名
   (3 s 超时则降一档重试)
3. 启动显著线程
4. 主循环开始消费提名
```

> 切换判据：只有当 §1.2 的**双路稳定性验收**（连续 5 分钟 IR ≥24.5、VIS ≥ target×0.98、`skip_align < 1%`）在 `parallel` 下也通过，才切过去。
> **收益很小**（省几百毫秒启动时间），**风险不为零**（帧率抖动会污染所有指标），所以**默认别改**。

---

## 6. 运行模式与降级

### 6.1 三种模式（命令行 `--mode`）

| 模式 | 提名哪些象限 | 适用场景 | YOLO 调用 |
|---|---|---|---|
| `inspect`（默认） | ①为主，③进兜底 | 巡检/工业 | ①提名 crop；③触发 TOP-N 全图兜底 |
| `yard` | ②为主（需 `score_living`） | 庭院/活体 | ②提名 crop |
| `all` | ①②③都提名 | 调试/演示 | 全提名 crop |

### 6.2 逐帧基准模式（A/B 对照，必须有）

`--mode baseline`：**完全跳过显著层**，逐帧对 VIS 全图跑 YOLO，行为等同第二期 demo。

> **这一模式不是凑数的，它是整个 demo 的一半价值。** 没有它，"省了多少 NPU" 这句话无法被量化。

### 6.3 优雅降级（强烈建议做，它是 best demo 记忆点）

当以下任一条件成立时，主线程自动从级联切到 `-fallback per-frame`，并打印 `[DEGRADE]`：

| 触发条件 | 判据 | reason |
|---|---|---|
| 热交叉 | `scene_contrast_k < --c-min`（默认 1.5 K） | `crossover` |
| NUC 冻结 | 连续 ≥2 帧 `meanAbsDiff(T, T_prev) < 0.01` | `nuc_freeze` |
| 遮挡/失效 | 第②象限计数在窗口内从常态骤降到 0 | `occlusion` |
| 低温漂场景 | `t_ref` 长时间恒定且 `t_max` 波动 < NETD | `low_contrast` |

恢复后自动切回并打印 `[RECOVER]`。

> 这一条务必做。前期调研的核心商业论据就是：**"会在失效时明确说出来"比"默默输出错误数据"高一个维度**。这是唯一能在 demo 里让客户当场感受到的差异，而且代码量很小。

---

## 7. 日志与指标（demo 的价值全在这里）

### 7.1 逐帧/周期性日志

沿用第二期的简洁风格，**不刷屏**：

```
[FPS]  ir: cur=25.0 avg=24.9 (n=2150) | vis: cur=25.0 avg=25.0 (n=2148)
[SAL]  #1204 q1=1 q2=2 q3=0 tir_only=3 contrast=2.4K sal=1.9ms skip_align=1
[EVT]  id=17 quad=Q1 tmax=48.2 est=54.6 dT=+22.4K area=31px sal_diff=0.71 age=1.2s valid=1
[NPU]  id=17 crop=160x160 infer=68.4ms obj=0 label=none            <- 提名的！视觉确实没东西
[SAVE] pair_det_20260930_183000_123 save=5.1ms align=8ms
[DEGRADE] reason=crossover contrast=0.9K -> fallback per-frame
[RECOVER] contrast=3.2K -> back to cascade
```

**`[SAL]` 行字段说明：**
- `tir_only`：本帧**只用热模态**会提名几个（单模态基线）
- `q1`：**双模态差分后**还剩几个（我们的方法）
- **这两个数的比值就是 demo 的核心指标**，见 §7.2

### 7.2 核心指标定义（不需要任何标注，这是关键设计）

| 指标 | 定义 | 目标值 |
|---|---|---|
| **差分虚警削减率** `FAR` | `1 - Σq1 / Σtir_only` | **≥ 0.50**（低于 0.30 则该路线需重新评估） |
| **NPU 调用压缩比** | `npu_calls / sal_frames`，与 baseline 的 `1.0` 对比 | **≤ 0.05**（即 ≥20 倍） |
| **显著层耗时** `sal_ms` | S_tir + S_rgb + 四象限 + 连通域 + 属性 | **≤ 5 ms** @256×192 |
| **提名端到端延迟** | 帧时间戳 → 事件写入 JSONL | **≤ 100 ms** |
| **受控场景召回** | S3 隐藏热源是否全部进 Q1 | **3/3** |

> **注意 `FAR` 为什么不需要标注**：它比较的是"单模态热提名"与"双模态差分提名"的**数量比**，不是检测精度。前期 python 验证跑出 67%，行业侧（打猎相机 AI 化实测）是 86%——**两者同量级就说明方法成立**。

### 7.3 退出汇总 `[STAT]`

```
[STAT] ir frames=31250 avg_fps=25.0 | vis frames=31200 avg_fps=25.0
[STAT] sal frames=31100 sal_ms avg=1.9 max=6.2 | skip_align=12 (0.04%)
[STAT] nominate tir_only=8420 q1=2694 q2=5310 q3=416
[STAT] >>> FAR(diff false-alarm reduction) = 68.0%      <- 判决指标1
[STAT] >>> NPU calls: cascade 2694 / frames 31100 = 8.7% (baseline would be 100%)  <- 判决指标2
[STAT] mode=inspect  degrade_events=3 (crossover 2, nuc_freeze 1)
[STAT] infer call=2694 avg=68.9ms | saved_pairs=10
```

---

## 8. 输出物

### 8.1 JSONL 事件流（每个提名一行）

路径 `events.jsonl`，**append 模式，程序启动不覆盖**（便于多次实验对比）。字段：

```json
{"v":1,"ts_ms":1780000123456,"id":17,"quadrant":1,
 "ir_box":[110,84,18,15],"area_px":31,"centroid":[119.2,91.7],
 "temp":{"t_max":48.2,"t_p99":46.9,"t_ref":25.8,"delta_t":22.4,
         "t_lower_bound":48.2,"t_true_est":54.6,"gradient":3.1,
         "unit":"C","radiometric":true},
 "saliency":{"tir":0.83,"rgb":0.12,"diff":0.71},
 "temporal":{"persistence_s":1.2,"dT_dt":0.08,"t_std":0.11,"micro_motion":0.023},
 "health":{"scene_contrast_k":2.4,"crossover_risk":0,"valid":1,"reason":null},
 "scene":{"ir_temp_median":25.1,"q1_count":1,"q2_count":2,"q3_count":0},
 "semantic":{"label":"none","score":0.0,"filled_by":"vendor_default"},
 "roi_vis":[840,620,320,320],"vis_res":[1280,960]}
```

> **字段纪律**：温度为单位明确的量；没有任何 `confidence` 描述我们自己的输出；`valid` 三态强制在场（不允许省略）；情形 B（无真实温度）时整个 `temp` 对象省略，且 `"radiometric":false`。

### 8.2 六宫格可视化图

每 `--viz-interval`（默认 2 s）一张，以及**每次被接受的提名立即生成一张**。文件名 `viz_<ts>.jpg`，滚动保留最近 10 张（复用现有清理逻辑，从"对"改为"单图"）。

六宫格（建议 3 行 × 2 列或 2 行 × 3 列，总尺寸约 960×480）：

| 格 | 内容 |
|---|---|
| 1 | VIS 原图 + 提名框 + `Q1/Q2` 标签 + `ΔT` |
| 2 | IR 温度图（JET 伪彩）+ 提名框 |
| 3 | S_rgb 显著图（灰度） |
| 4 | S_tir 显著图（灰度） |
| 5 | **差分图**（红 = 热更显著；蓝 = 视觉更显著） |
| 6 | **四象限着色**（红=① 蓝=② 黄=③ 灰=④） |

> ⚠️ **OpenCV 坑（前期 python 版踩过）**：四象限着色图若用 matplotlib 显示必须按 RGB 顺序，本项目的 `imwrite` 则按 **BGR** 顺序。二者混用会导致红蓝完全反转——六宫格里第①象限（红）会显示成蓝色。

**演示要点（这段写进 README，是给客户看的）：**
> 找一个"看起来正常、摸起来烫"的目标（比如藏在杂物后面的暖宝宝/热水杯/运行中的充电器）。第 4 格里它和周围物体一样显著，**只有第 5、6 格把它单独标红**。这两张图单看都没问题，**它们的差异才是问题**——这是单模态产品物理上做不出来的。

### 8.3 🆕 可选：原始双路录制（带宽富余后的免费副产品）

既然 USB 带宽不再是瓶颈，可以**顺带把两路原始帧滚动落盘**，专门用于离线分析与后续数据集建设：

```
--record-raw                开启原始帧录制
--record-dir DIR            目录 (默认 ./raw)
--record-window SEC         滚动窗口秒数 (默认 120)
--record-trigger event|all  仅提名前后 N 秒 / 全程 (默认 event)
```

落地格式（**刻意用最简单的布局，方便 Python 直接读**）：

```
raw/<session>/
  ├── ir/<ts>_temp.bin       # 256×192 float32 温度矩阵（情形 A）
  ├── ir/<ts>_img.jpg        # 256×192 灰度图
  └── vis/<ts>.jpg           # 1280×960 可见光帧
  └── frames.csv             # ts, ir_path, vis_path, align_ms
```

> **为什么值得做**：`event` 模式下，每次提名自动保留前后 ±3 秒。跑一两周线上/实地，就会自然积累出第一批真实场景难例——这正好是 Phase 3 重训/换词表需要的种子数据。
> **成本几乎为零**：只在提名时触发磁盘写，平时只维护内存环形缓冲。写盘放在独立低优先级线程，**与主链路解耦**，不得反过来阻塞采集。
> **不是必须的**，但建议做——它是从 demo 走向数据飞轮的最小一步。

---

## 9. 受控验证场景（必须照做，用来验收）

不需要标注数据，只需要三个场景各录 60 秒：

| 场景 | 布置 | 期望结果 | 判据 |
|---|---|---|---|
| **S1 空背景** | 静态室内场景，无特殊热源 | Q1 ≈ 0 | `q1/frames < 0.02` |
| **S2 视觉明显热目标** | 人站在画面中 / 屏幕显示高对比图案 | 落在 **Q2** | Q2 计数明显 > Q1 |
| **S3 隐藏热源** | 暖宝宝/热水杯放在**视觉平淡处**或被部分遮挡 | 落在 **Q1** | **3/3 全部命中** |
| **S4 反射虚警（可选）** | 台灯照射金属/玻璃产生反射热点 | 观察是否被误判为 Q1 | 记录，不设期望（反射确实会造成虚警，属已知局限） |

> 三个场景跑完，`Σq1/Σtir_only` 即为 §7.2 的 FAR。
> **判据：FAR ≥ 0.50 且 S3 命中 3/3 → 方案成立。FAR < 0.30 → 停止后续开发，回到算法层重新审视。**

---

## 10. 文件与代码结构

在现有 `rv1126_test/` 上增量：

```
rv1126_test/
├── include/
│   ├── (现有: rknn_api.h, yolov8.h, ...)
│   ├── ir_temp_conf.h        # ★新增: IR_TEMP_SCALE/OFFSET 等标定常量(由阶段0产出)
│   ├── registration.h        # ★新增: IR↔VIS 映射
│   ├── saliency.h            # ★新增: 显著性算子与四象限
│   ├── nominator.h           # ★新增: 连通域/属性包/时序
│   ├── nomination.h          # ★新增: 数据结构定义(Nomination/SemanticSlot)
│   ├── viz_grid.h            # ★新增: 六宫格合成
│   └── eventlog.h            # ★新增: JSONL 写入(无第三方依赖,手写)
├── lib/
│   ├── (现有全部不动)
│   ├── registration.cpp      # ★新增
│   ├── saliency.cpp          # ★新增  ← 核心
│   ├── nominator.cpp         # ★新增  ← 核心
│   ├── viz_grid.cpp          # ★新增
│   └── eventlog.cpp          # ★新增
├── main.cpp                  # 改造: 加显著线程 + 队列 +新模式 +新日志 +降级
├── CMakeLists.txt            # 小改: 新增源文件; 依赖不变
├── models/                   # yolov8m_rv1126b.rknn (位置不变)
├── output/                   # viz_*.jpg (改名,仍留10张)
├── events.jsonl              # ★新增: 事件流
└── docs/
    └── ir_temp_probe_report.md   # ★阶段0产出
```

**明确禁止 Code Agent 做的事（防止过度设计）：**
- ❌ 不要引入任何新第三方库
- ❌ 不要把 IR 上采样后再送进神经网络
- ❌ 不要实现 8 点单应标定的完整 UI 流程（预留参数即可）
- ❌ 不要在板端做任何训练/微调/在线学习（这是架构红线：权重必须冻结可版本化）
- ❌ 不要引入 VLM / CLIP / YOLO-World（那是 Phase 3 的事，见第 13 章）
- ❌ 不要改现有的采集/推理/退出/清理逻辑

---

## 11. 命令行接口

```
用法: ./rv1126_dual_demo <model.rknn> [选项]

运行模式:
  --mode cascade|baseline|auto     级联 / 逐帧基准 / 自动(带降级,默认 auto)
  --card  inspect|yard|all         模式卡: 巡检(①为主) / 庭院(②+活体) / 全部(默认 inspect)

显著性参数:
  --thr-mode adaptive|otsu|tht     阈值方式 (默认 tht)
  --thr-k FLOAT                    adaptive 的 k (默认 1.0)
  --delta-min FLOAT                tht 的最小温升 K (默认 3.0)
  --bg-kernel INT                  背景模糊核 (默认 15, 需奇数)
  --min-area INT                   提名最小像素数 (默认 8)
  (crop 相关参数见下方 "crop 尺寸" 段)

几何/配准:
  --fov-unit diag|axis                对角 FOV / 分轴 FOV  (默认 diag)
  --ir-fov FLOAT                      红外 FOV (默认 57)
  --vis-fov FLOAT                     可见光 FOV (默认 80)
      # diag 模式下按各自实际 W/H 拆到 fov_x/fov_y (见 §3.2.1)
      # 旧参数 --ir-fov-x/-y --vis-fov-x/-y 仍兼容
  --ir-aspect FLOAT --vis-aspect FLOAT  覆盖宽高比 (默认取实际协商分辨率)
  --homography FILE                   可选,精确单应(cv::FileStorage)

crop 尺寸 (v2: 随分辨率换算):
  --margin-ratio FLOAT            上下文扩展倍数 (默认 3.0)
  --min-crop-ir INT               最小作物边长, IR 像素为单位 (默认 20)
  --min-crop-px INT               最小作物边长绝对下限, VIS 像素 (默认 96)
  --max-crop-px INT               最大作物边长上限, VIS 像素 (默认 1024)

启动与采集 (v2 修订):
  --res-preset auto|960p|768p|720p|600p|480p   可见光分辨率档位 (默认 auto)
      auto 依次尝试 1280x960 -> 1024x768 -> 1280x720 -> 800x600 -> 640x480
  --start-order ir-first|parallel              启动顺序 (默认 ir-first)
  --startup-timeout-ms INT                     首窗就绪超时 (默认 3000)

原始录制 (可选):
  --record-raw                    开启
  --record-dir DIR                (默认 ./raw)
  --record-trigger event|all      (默认 event)
  --record-window SEC             滚动窗口 (默认 120)

健康/降级:
  --c-min FLOAT                   最低场景热对比度 K,低于则宣告降级 (默认 1.5)
  --nuc-freeze-thr FLOAT          冻结判定帧差阈值 (默认 0.01)

时序(庭院模式):
  --body-lo FLOAT --body-hi FLOAT  表观体温带 °C (默认 26/40)
  --dt-thr FLOAT                   活体判据: |dT/dt| 上限 K/min (默认 0.5)
  --link-radius INT                帧间关联半径 px (默认 8)

输出:
  --save-dir DIR       (默认 ./output,仍留 10 张)
  --viz-interval SEC   六宫格周期间隔 (默认 2.0)
  --events FILE        JSONL 路径 (默认 ./events.jsonl)
  --no-viz             关闭可视化(纯指标跑)

(相机节点选型 -d/-f/--format 沿用第二期的 VID/PID 自动探测逻辑,不写死设备节点;
 显式传 -W/-H 时优先使用用户值,不再走 auto 档位链)
```

---

## 12. 实施步骤（每步都可独立验收）

| 步骤 | 内容 | 产出/验收 | 预计 |
|---|---|---|---|
| **P0** | **阶段 0 温度探针**（第 2 章） | `ir_temp_probe_report.md`，结论 A/B/C | 0.5–1 天 |
| **P0.5** | 🆕 **分辨率枚举 + 双路带宽稳定性验收**（§1.2）：`v4l2-ctl` 列举 MJPG 档位，选定后连续跑 5 分钟 | `[INIT]` 打印协商结果；IR ≥24.5 / VIS ≥ target×0.98 / `skip_align<1%`。**不达标则降一档** | 0.25 天 |
| **P1** | IR thread 输出温度矩阵 `Mat CV_32F` 到 ring；`registration.cpp` 实现双向映射 **+ FOV 分轴换算（§3.2.1）** | 用一处已知目标验证 IR→VIS 映射落点误差 < `max(10 px, 2% 画幅)`；非 4:3 档位额外验收横向不偏 | 0.5 天 |
| **P2** | `saliency.cpp`：S_tir / S_rgb / 归一化 / 四象限 / 闭运算 / 连通域 | 单帧显著图落地存盘，`sal_ms ≤ 5ms` | 1 天 |
| **P3** | `nominator.cpp`：属性包 + 队列 + 主线程消费 + crop YOLO + `[SAL]/[EVT]/[NPU]` 日志 | 提名→crop→YOLO 全链路跑通，`[EVT]/[NPU]` 日志正常 | 1.5 天 |
| **P4** | `viz_grid.cpp` 六宫格 + `eventlog.cpp` JSONL | 图上能讲清故事 | 0.5 天 |
| **P5** | `--mode baseline` + `[STAT]` 汇总 + FAR/NPU 压缩比统计 | **得到判决数字** | 0.5 天 |
| **P6** | 降级链路（`contrast_k` / `nuc_freeze` / q2 骤降）+ `[DEGRADE]/[RECOVER]` | 演示可用 | 0.5 天 |
| **P7** | `nominator` 时序维 + `--card yard` 活体打分 | 庭院方向判决点 | 1 天 |
| **P8** | 跑 S1–S4 受控场景，出实验报告 | **是否 Go/No-Go** | 0.5 天 |

> P7 是可选项，但**如果庭院/割草机是目标市场，它就是必做**——空间差分剔除不掉晒热岩石，只有时序能分开活体。

---

## 13. Phase 3 预告（本 demo 不做，但要知道它的位置）

本 demo 的 `semantic_slot` 用**封闭类别的 YOLOv8（COCO 80 类）**填充，这只是一个占位实现。真正的"上限"应该是：

- **开放词汇判别式**：`yolo_world`（瑞芯微 RKNN Model Zoo 官方支持 **RV1126B**，INT8 detector + FP16 CLIP text encoder 都跑 NPU，官方原话 *"allowing categories to be changed without rebuilding the detector"*）。改词表只需重跑一次 `clip_text`（~12 ms），**detector 不重训、不重新量化**。
- 这才是"上限可进化/可订阅"的技术底座：**卖的是还没造出来的新词表和新模型，不是已造好只锁住的功能**（后者会翻车，参考宝马 CarPlay 订阅 4 个月被迫放弃）。
- 迁移成本很低：本 demo 里 `semantic_slot` 的接口已经预留，替换是没有痛点的。

> **但前提是先把 P5 的判决数字跑出来。** 在 FAR ≥ 0.5 之前，不要投入任何开放词汇的工作。

---

## 14. 风险与坑（按可能性排序）

| # | 风险 | 对策 |
|---|---|---|
| 1 | **红外下半段不是温度**（情形 B/C） | P0 探针先确认；B 则降级为灰度方案并如实标注；C 则停止，改用 SDK 的温度输出模式重新协商 |
| 2 | ~~**USB 等时带宽 ghost reservation**~~ **→ v2 已基本解除**（v1 时实测：多次启停后连 640×480 也 ENOSPC） | 现状是两路各占一路 USB，**不再是降分辨率的理由**。但保留防御性写法：关流仍须 `uvc_camera_stream_close(CLOSE_CAM_SIDE_PREVIEW)`、关闭顺序不变、长时间验证前冷启动一次。**若 §1.2 的稳定性验收不通过，按档位表降一档，而不是继续在原地排查** |
| 3 | **误用自适应阈值而不是 THT** → 阈值失去物理量纲，讲不清楚 | **默认 `--thr-mode tht`**，它的阈值是"高于背景峰值 X K"，demo 讲得清楚 |
| 4 | **忘记闭运算直接连通域** → 碎片化导致 `q1 > tir_only`，FAR 变成负数 | 顺序固定：mask → `MORPH_CLOSE 3×3` → connectedComponents |
| 5 | **crop 太紧** → YOLO 在没有上下文的高倍放大块上失效 | `margin_ratio=3.0` + `min_crop_ir=20`（换算后约 100–200 VIS px），不得省。**v2 后不要写死 96 px**——那在高分辨率下会相对变小 |
| 6 | **NUC 冻结期产生假提名**（整帧温度突变） | 冻结检测 + `valid=0` + 该帧不提名；这也是 `[DEGRADE]` 的演示素材 |
| 7 | **反射造成第①象限虚警**（金属/玻璃台灯反射） | 已知局限，不消除。日志记录即可（S4 场景）。可在文档里说明这是后续用 RGB 材质判读的方向 |
| 8 | **显著线程吃满 CPU 拖累采集** | 目标 `sal_ms ≤ 5ms`；若超标，先做三件事再谈降采样：① `cvtColor` 挪到采集线程；② 降采样改两级 `INTER_AREA`（§4.3）；③ 显著线程每隔 1 帧处理。**IR/VIS 采集帧率永远第一优先** |
| 9 | **模型 fixed 在 COCO 80 类，crop 里常见工业目标检不出** | 这是 Phase 3 的问题。**但要注意：检不出不是坏事**——`label=none` 恰恰是"这里有个视觉认不出的热异常"，应作为提名的加分项而非失败项 |
| 10 | 🆕 **VIS 提到 1280×960 后 MJPG 解码吃 CPU，挤压 IR 采集与显著线程** | 先跑 §1.2 双路稳定性验收。**1280 宽是建议上限**（若板端无硬件 JPEG 解码则更低）。不达标就依次退到 1024×768 / 800×600 —— 记住 §1.2 的原则二：**crop 超过 640 px 后高分辨率几乎无收益**，不要为数字好看牺牲帧率稳定性 |
| 11 | 🆕 **对角 FOV 当成水平 FOV 用** | 必跑 §3.2.1 换算。校验方法：画面中心放一个热源，检查 crop 是否居中；再放到画面横向 70% 位置，检查横向偏移。**注意这不影响 FAR，只影响图和 max_crop 触发频率** |

> **一句话总结 v2 的风险结构变化**：v1 最大的风险是"带宽不够导致跑不起来"，**v2 这个风险消失了**；取而代之的新风险是"分辨率上去了但 CPU 扛不住"。**验收方式也因此改变：从"能不能协商成功"变成"能不能 5 分钟不抖动"。**

---

## 15. 验收标准清单

**阶段 0（阻塞）**
- [ ] `ir_temp_probe_report.md` 给出 A/B/C 明确结论
- [ ] 若为 A：三点标定 R² > 0.99，`IR_TEMP_SCALE/OFFSET` 写入 `ir_temp_conf.h`

**功能**
- [ ] IR 输出 `Mat CV_32F` 温度矩阵进入 ring（情形 A）
- [ ] 🆕 VIS 按 §1.2 档位表自动选取最高可用 MJPG **4:3** 档（默认目标 **1280×960@25**）；沿用第二期的 VID/PID 自动探测，不写死节点
- [ ] 🆕 双路同时跑到目标帧率、连续 5 分钟无抖动（IR ≥24.5 / VIS ≥ target×0.98 / `skip_align < 1%`）——**这是所有指标可信的前提**
- [ ] 🆕 FOV 按 `--fov-unit` 正确换算到分轴；1280×720 等非 4:3 档位额外验收横向 crop 不偏
- [ ] 🆕 crop 尺寸随 VIS 分辨率换算（`min_crop_ir × scale`），**不得写死 96 px 下限**；`max_crop` 生效
- [ ] IR 优先启动（`--start-order ir-first`），稳定达 25 fps 后才启动 VIS；推理期间 IR 不掉帧
- [ ] 显著线程输出四象限计数与提名属性包，`sal_ms avg ≤ 5 ms`
- [ ] 提名→crop→YOLO 全链路跑通，`[EVT]/[NPU]` 日志正常
- [ ] `events.jsonl` 每提名一行，字段符合第 8.1 节
- [ ] 六宫格图：提名触发 + 每 2 s 周期，滚动保留 10 张
- [ ] `--mode baseline` 可跑，行为等同第二期
- [ ] `auto` 模式在 `contrast_k < c_min` / NUC 冻结时打印 `[DEGRADE]` 并切逐帧，恢复后 `[RECOVER]`
- [ ] 除 OpenCV 外无新增依赖；CMakeLists 增量改动；构建部署沿用现有脚本
- [ ] Ctrl+C 退出：显著线程 join、队列释放、SDK 关流、打印 `[STAT]`

**指标（判决）**
- [ ] `[STAT]` 输出 **FAR ≥ 0.50**
- [ ] `[STAT]` 输出 **NPU 调用压缩比 ≤ 0.05**
- [ ] S1–S4 受控场景跑完，其中 **S3 隐藏热源 3/3 命中 Q1**
- [ ] 提名端到端延迟 ≤ 100 ms

> **只要 FAR < 0.30，无论其它做得多好，都不要继续投 Phase 3。** 那时应该回来重新审视算法假设，而不是投入更多工程。

---

## 附：给 Code Agent 的最短开工路径

```
1. 读 docs/ir_temp_probe_report.md  → 确认情形 A/B/C          【半天，先做】
1.5 v4l2-ctl 枚举 MJPG 档位 → 选最高 4:3 档 → 跑 5 分钟双路稳定性  【1~2 小时】
2. 改 ir_camera.cpp: 在 process_image_frame 里产出 CV_32F 温度矩阵进 ring
3. 新建 lib/registration.cpp: diag FOV 拆轴 → IR↔VIS 双向映射 → crop 尺寸换算
4. 新建 lib/saliency.cpp: GaussianBlur → absdiff → resize(INTER_AREA) → normalize(P99)
5. 新建 lib/nominator.cpp: 四象限 → MORPH_CLOSE → connectedComponentsWithStats → Nomination
6. 改 main.cpp: 起显著线程 → 有界队列 → 主线程 crop+YOLO → 日志
7. 跑 --mode baseline 与 --mode cascade 各 60 秒,对比 [STAT] 两组数字
```

**一句话提醒：** 这个 demo 的成功标准不是"检测框画得多好多准"，而是 **`[STAT]` 里那两行带 `>>>` 的数字**。盯住它们。

> 🆕 v2 补一句：**分辨率不是越大越好。** 1280×960 的价值只在 crop 质量，代价是 CPU。
> **一旦它让 IR 帧率抖动或 `sal_ms` 超标，立刻退回 1024×768** —— 这个 demo 的全部说服力建立在稳定帧率之上，不要为了参数好看牺牲它。
