# 双摄像头 + YOLOFuse 融合检测集成方案

## 1. 项目概述

将 `twocameraOnMAC`（双摄像头采集）与 `YOLOFuse`（双模态目标检测）集成，实现：
- **本机运行**：C++ 程序内嵌 ONNX 推理，实时处理可见光 + 红外双路视频流
- **RV1126B 部署**：ONNX → RKNN 转换，利用 NPU 加速推理

> T113-S3 相关部署不在本方案范围内。

## 2. 现有架构分析

### 2.1 twocameraOnMAC（C++ 双摄像头采集）

```
IrCamera (USB SDK, 256×192, BGR)  ─┐
                                    ├─→ LatestFrameSynchronizer ─→ SyncedFrames
VisibleCamera (V4L2, 640×480, BGR) ─┘         │
                                               ↓
                                    FrameCompositor (PiP 合成)
                                               │
                                               ↓
                                          IDisplay (OpenCV/FBDev)
```

关键数据结构：
- `BgrFrame`：BGR 3通道，含 width/height/stride/timestamp/sequence/data
- `SyncedFrames`：{ infrared: BgrFrame, visible: BgrFrame, timestamp_delta_us }
- `LatestFrameSynchronizer::acquire(SyncedFrames&)`：取双路最新帧
- 主循环在 `sample.cpp` 的 `run_once()` 中，每帧调用 synchronizer → compositor → display

### 2.2 YOLOFuse（Python 双模态检测）

- **模型**：`best.pt`，Mid-level Feature Fusion 架构
- **输入**：`[1, 6, 640, 640]`（RGB 3ch + IR 3ch 拼接）
- **结构**：IdentityInput → ModalitySelector 切分为 RGB(3ch) 和 IR(3ch) → 双流 backbone → 中期特征融合 → Detect head
- **输出**：YOLO 标准检测头输出 `[1, nc+4, num_anchors]`，经 NMS 后得到检测框
- **导出**：支持 `model.export(format='onnx')` 导出为 ONNX

### 2.3 数据格式对应关系

| YOLOFuse 训练时 | twocameraOnMAC 实时采集 |
|---|---|
| `images/xxx.jpg`（RGB 可见光，3ch BGR） | `VisibleCamera` 输出的 `BgrFrame` |
| `imagesIR/xxx.jpg`（红外伪彩/灰度，3ch BGR） | `IrCamera` 输出的 `BgrFrame` |
| `cv2.merge((ir, rgb))` → 6ch | C++ 中 memcpy 拼接两个 3ch 帧 |

### 2.4 BgrFrame 内存策略

#### 内存分配

`BgrFrame` 采用**预分配 + 复用**策略，避免每帧 malloc/free：

```cpp
// src/frame.h (修订)
struct BgrFrame {
    int width;
    int height;
    int stride;           // 行对齐字节数
    int64_t timestamp_us;
    uint32_t sequence;
    uint8_t* data;        // 预分配缓冲区 (外部管理)
    bool owns_data;       // data 是否由本对象拥有

    // 禁止自动分配, 必须外部提供缓冲区
    BgrFrame(int w, int h, uint8_t* external_buf);
    ~BgrFrame(); // 不释放 data (外部负责)
};
```

#### 缓冲区池化

```cpp
// src/frame_pool.h
template <int ALIGN = 16>
class FramePool {
public:
    // 创建 N 个预分配帧, 每个尺寸 w×h×3
    void init(int w, int h, int count);
    // 获取一个空闲帧 (无锁, 用于单生产者)
    BgrFrame& acquire();
    // 归还帧
    void release(BgrFrame& frame);
private:
    struct Impl;  // 隐藏实现
};
```

#### 零拷贝传递

管线中帧的传递全部通过指针/引用，不拷贝像素数据：

```
IrCamera 采集线程 → FramePool::acquire() → 填充数据 → push 到队列
                                                              ↓
主线程: Synchronizer 从队列取 BgrFrame& → 传递给 ImageRegistrator
         → 传递给 IDetector::detect() → 传递给 SideBySideCompositor
         → 传递给 TrackingOverlay → 传递给 IDisplay::present()
                                                              ↓
                    显示完成 → FramePool::release() → 回到池中
```

**全流程零拷贝，只有以下操作涉及数据复制**：
1. YOLO 预处理：BGR→RGB、HWC→CHW、通道拼接（必要的格式转换）
2. 伪彩叠加：ROI 区域内 applyColorMap/addWeighted（必要的像素变换）
3. 合成帧：IR + Visible 拼接为 SideBySide 帧（必要的布局变换）

#### RV1126B 特殊考虑

- 使用 DMA 可分配内存（`dma_alloc_coherent` 或 RGA buffer）作为帧缓冲区
- RGA 操作直接读写 DMA buffer，无需 CPU 搬运
- NPU 推理输入直接使用预处理后的 DMA buffer，零拷贝送入 NPU
- 避免在 RV1126B 上频繁 malloc/free，所有内存启动时一次性分配

### 2.5 IR 分辨率与伪影说明

IR 摄像头原生分辨率为 **256×192**，YOLOFuse 模型输入为 **640×640**，存在约 8 倍像素放大。

**影响**：
- IR 帧从 256×192 放大到 640×640 会引入块状伪影（bilinear/bicubic 插值无法恢复细节）
- 在 640×640 坐标系下的检测框精度受限于原始 256×192 分辨率
- YOLOFuse 训练时 LLVIP 数据集的 IR 图是原始分辨率，模型学到的特征可能与放大后的帧存在 domain gap

**缓解措施**：
- 使用 `cv::INTER_CUBIC` 替代 `bilinear` 做插值，减少块状伪影
- 考虑在 256×192 分辨率下做检测，然后将检测框按比例映射到 640×640（但这需要修改模型输入尺寸）
- 最佳方案：重新训练/导出模型时使用 256×192 输入分辨率，避免放大引入的 domain gap
- 在 RV1126B 上用 RGA 硬件缩放比 OpenCV 软件缩放更平滑（RGA 有专用缩放滤波器）

**建议**：当前阶段先用 640×640 方案验证管线，后续根据检测效果决定是否调整模型输入分辨率。

## 3. 整体架构设计

### 3.1 管线总览

#### 3.1.1 主管线流程（同步模式，本机开发用）

```
┌──────────────────────────────────────────────────────────────────────┐
│                        twocameraOnMAC 主程序                          │
│                                                                      │
│  IrCamera 采集线程 ──┐                                                │
│                      ├→ Synchronizer → SyncedFrames                   │
│  VisCamera 采集线程 ──┘                 │                             │
│                                        ↓                              │
│                              ┌─ ImageRegistrator ──────────┐          │
│                              │  (图像配准, 独立步骤)          │          │
│                              │  可见光帧 → 对齐到 IR 视角      │          │
│                              │  本机: OpenCV warp/crop       │          │
│                              │  RV1126B: RGA 硬件加速         │          │
│                              └─────────────────────────────┘          │
│                                        │                             │
│                                        ↓ RegisteredFrames            │
│                                        │                             │
│                          ┌──────────────┼───────────────┐             │
│                          ↓                              ↓             │
│                ┌─ IDetector ───────────┐   ┌─ SideBySideCompositor ─┐│
│                │  (ONNX/RKNN 推理)       │   │  (并排合成)              ││
│                │                       │   │  左半: IR 主画面         ││
│                │  1. 预处理: resize +   │   │  右半: 可见光副画面      ││
│                │     merge → [1,6,640,640]│  │  中间: 分割线          ││
│                │  2. 推理: forward     │   └──────────────────────┘│
│                │  3. 后处理: NMS + 解码  │            │              │
│                └───────────┬───────────┘            ↓              │
│                            ↓ detections              │              │
│                            │                         │              │
│                ┌───────────┴───────────┐            │              │
│                ↓  ITracker (ByteTrack)  ↓            │              │
│                │  多目标跟踪            │            │              │
│                │  Kalman 预测 +         │            │              │
│                │  Hungarian 匹配       │            │              │
│                └───────────┬───────────┘            │              │
│                            ↓ tracked_objects         │              │
│                            │                         │              │
│                ┌───────────┴───────────┐            │              │
│                ↓  TrackingOverlay      ↓            │              │
│                │  检测框 + 跟踪 ID 标签   │            │              │
│                │  运动箭头 + 轨迹路径     │─────────────┘              │
│                │  IR: 框内完整伪彩       │                           │
│                │  可见光: 半透明伪彩     │                           │
│                └───────────┬───────────┘                           │
│                            ↓ annotated_frame (~1067×640)            │
│                            │                                        │
│                ┌───────────┴───────────┐                           │
│                ↓  Downscale (RGA)      ↓                           │
│                │  1067×640 → 240×320   │                           │
│                │  RGA resize (硬件加速) │                           │
│                └───────────┬───────────┘                           │
│                            ↓ display_frame (240×320)                │
│                            │                                        │
│                            └─────────────────────────────→ IDisplay   │
│                                                                   │
│                    本机: OpenCV imshow (无需 downscale)                │
│                    RV1126B: RGA downscale → SPI + DMA → ILI9341      │
└──────────────────────────────────────────────────────────────────────┘
```

#### 3.1.2 异步推理管线（RV1126B 生产部署用）

为实现 25fps 目标帧率，采用**异步推理 + 流水线并行**：

```
┌──────────────────┐     ┌──────────────────┐     ┌──────────────────┐
│  主线程 (采集+显示) │     │  NPU Worker 线程  │     │  LVGL 渲染线程    │
│                  │     │                  │     │                  │
│  acquire frames  │     │                  │     │                  │
│  → register      │     │  取最新帧        │     │  LVGL task_handler│
│  → compose       │     │  → resize+merge  │     │  → draw to canvas │
│  → overlay       │     │  → NPU 推理      │     │  → flush_cb       │
│  → RGA downscale │     │  → postprocess   │     │  → SPI DMA 传输   │
│  → LVGL display  │     │  → ByteTrack     │     │                  │
│                  │     │  → 更新结果      │     │                  │
│  ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄  │     │  ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄  │     │  ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄  │
│  用 last_results │     │  存结果到 shared  │     │  独立刷新        │
│  (上次推理结果)  │     │  queue/atomic    │     │  (~5ms周期)      │
└──────────────────┘     └──────────────────┘     └──────────────────┘
```

**三线程模型**：

| 线程 | 职责 | 耗时目标 | 状态 |
|---|---|---|---|
| 主线程 | 采集→配准→合成→叠加→RGA 缩放→显示 | < 10ms/帧 | 循环运行 |
| NPU Worker | 取帧→预处理→NPU 推理→后处理→ByteTrack | < 40ms/帧 | 循环运行，产出结果 |
| LVGL 渲染 | LVGL task_handler → SPI DMA | < 5ms/帧 | LVGL 内部驱动 |

**数据同步**：
- 主线程用 `last_results`（最近一次推理结果）做叠加
- NPU Worker 每完成一次推理，更新 `last_results`
- 主线程永远不阻塞等待 NPU，始终显示最新推理结果
- NPU 结果延迟 ≤ 1 帧（~40ms），25fps 下可接受

**流水线并行**：
```
时刻 T:   主线程处理帧 N    NPU Worker 处理帧 N-1    LVGL 显示帧 N-2
时刻 T+1: 主线程处理帧 N+1  NPU Worker 处理帧 N      LVGL 显示帧 N-1
```

**25fps 性能预算**：

| 环节 | 耗时 | 可用时间 | 说明 |
|---|---|---|---|
| 摄像头采集 | ~5ms | 33ms (25fps) | 双摄像头帧同步 |
| 图像配准 (RGA) | <1ms | 剩余 | RGA 硬件，不占 CPU |
| SideBySideCompositor | <1ms | 剩余 | memcpy + 缩放 |
| TrackingOverlay | 1-3ms | 剩余 | 检测框 + 伪彩 + 箭头 |
| RGA downscale | <1ms | 剩余 | 1067×640 → 240×320 |
| LVGL 渲染 + SPI | <5ms | 剩余 | LVGL task_handler + DMA |
| **主线程合计** | **~12ms** | **33ms** | 充裕 21ms |
| NPU 推理 + 后处理 + ByteTrack | ~20-40ms | 40ms | NPU Worker 可接受 ≤ 40ms |

**在异步模式下，主线程不受 NPU 推理时间限制**，只要主线程 < 33ms 就能稳定 25fps。NPU Worker 只需稳定产出结果，即使偶尔掉帧，主线程仍保持流畅。

### 3.2 图像配准设计

#### 设计原则

| 结论 | 说明 |
|---|---|
| **先同步后配准** | 同步解决时间对齐，配准解决空间对齐，两者正交。配准需要同一时刻的两路帧才有意义，必须在同步之后进行 |
| **配准独立运行** | 配准作为独立管线步骤，不嵌入采集线程，不嵌入 Synchronizer，不嵌入 IDetector 预处理 |
| **配准在预处理前** | 配准产出 `RegisteredFrames`，是 IDetector 预处理的输入。预处理接收的 visible 帧已经是配准后的 |
| **不参与推理过程** | IDetector 预处理只做 resize/normalize/merge，不做空间变换。配准在进入 IDetector 之前完成 |
| **可用硬件加速** | 本机用 OpenCV warpPerspective/crop；RV1126B 用 RGA 硬件做 crop+resize+CSC，与 NPU 并行 |

#### 配准与同步的关系

```
                    时间对齐              空间对齐
                    ↓                    ↓
IrCamera  ──→ Synchronizer ──→ ImageRegistrator ──→ RegisteredFrames
VisCamera ──→                          │
                                       │
              同步: 取两路最新帧          配准: 可见光帧 warp 到 IR 视角
              依赖: 帧时间戳              依赖: 两路帧的空间内容
              开销: 微秒级 (队列取帧)      开销: 毫秒级 (warp/crop)
```

- **不放在同步前**：采集线程只管采集，不耦合配准逻辑；避免采集线程被配准操作阻塞影响帧率
- **不放在同步中**：Synchronizer 保持轻量（微秒级），不因配准变重
- **不放在预处理中**：配准结果同时供检测和显示使用；放在预处理内只有推理路径能看到配准结果，显示路径需要重复配准

#### 配准方式

| 方式 | 适用场景 | 实现 | RV1126B 硬件加速 |
|---|---|---|---|
| **crop+resize** | 双摄光轴大致平行，仅 FOV 不同 | 按 IR 的 FOV 裁剪 visible 中心区域，resize 到 IR 分辨率 | RGA 一次 blit 操作完成 |
| **warpPerspective** | 双摄有角度偏差 | 用标定的 3×3 homography 矩阵做透视变换 | RGA2 仅支持仿射(2×3)，需近似或回退 CPU |
| **自适应配准** | 相机位置可能变化 | 每帧 ORB/FAST 特征匹配 → 动态计算 homography | CPU 特征匹配，RGA 做 warp |

默认采用 **crop+resize** 模式（简单、快、可硬件加速），标定参数可在运行时通过配置文件指定。

#### 标定流程（一次性，离线完成）

1. 双摄同时对准棋盘格标定板
2. OpenCV `findChessboardCorners` + `calibrateCamera` 分别标定两个相机
3. `stereoCalibrate` 得到双目外参
4. 计算可见光→IR 的配准参数（crop 区域或 homography 矩阵）
5. 保存到配置文件 `calibration.json`，运行时加载

#### RV1126B 硬件加速管线

```
SyncedFrames
    │
    ├─ visible BgrFrame → RGA 硬件 (crop+resize+CSC) → registered visible
    │                      ↑ 非阻塞，与 CPU 并行
    │
    └─ IR BgrFrame ──→ (无需配准，直接使用)
                                    │
                                    ↓
                          RegisteredFrames
                                    │
                       ┌────────────┼────────────┐
                       ↓            ↓            ↓
                   NPU 推理    RGA 显示缩放    (流水线并行)
                   (帧 N-1)   (帧 N-2)
```

- RGA 是独立硬件单元，提交 crop+resize+CSC 操作后立即返回，不阻塞 CPU
- 可流水线化：RGA 配准帧 N 时，NPU 推理帧 N-1，显示帧 N-2
- 三硬件并行，吞吐量接近最慢阶段（通常是 NPU 推理）

### 3.3 显示布局

```
┌───────────────────────────┬──────────────────────┐
│                           │                      │
│     红外主画面              │    可见光副画面        │
│   (Side-by-Side 左半)      │  (Side-by-Side 右半)  │
│                           │                      │
│   ┌─────→ Track #7 ←────┐ │   ┌────→ Track #7 ←──┐│
│   │   检测框 + 跟踪ID     │ │   │  检测框 + 跟踪ID ││
│   │   ↑ 运动箭头          │ │   │  ↑ 运动箭头      ││
│   │   │  (速度→长度)     │ │   │  │               ││
│   │   ├─ 框内伪彩区域     │ │   │  ├─ 半透明伪彩   ││
│   │   │  (热力图效果)    │ │   │  │  (alpha=0.5)  ││
│   │   └──────────────────┘ │   │  └──────────────┘│
│   │                        │   │                  │
│   │   ┌→ Track #12 ←┐     │   │  ┌→ Track #12 ←┐ │
│   │   │ 检测框 + 箭头 │     │   │  │ 检测框 + 箭头 │ │
│   │   └─────────────┘     │   │  └─────────────┘ │
│   │                        │   │                  │
│   │   ┄┄┄ 轨迹路径 ┄┄┄    │   │  ┄┄ 轨迹路径 ┄┄  │
│   │   (历史运动轨迹)        │   │  (历史运动轨迹)  │
│                           │                      │
└───────────────────────────┴──────────────────────┘
         IR (主)              Visible (副)
```

**显示元素说明**：

| 元素 | IR 主画面 | 可见光副画面 | 说明 |
|---|---|---|---|
| 检测框 | ✅ | ✅ | YOLO 检测框，双画面同步显示 |
| 跟踪 ID 标签 | ✅ | ✅ | 框上方显示 `#TrackID`，如 `#7` |
| 运动箭头 | ✅ | ✅ | 框底部显示速度方向箭头，长度与速度成正比 |
| 轨迹路径 | ✅ | ✅ | 近 N 帧质心连线，显示历史运动轨迹 |
| 框内伪彩 | ✅ 完整覆盖 | ✅ 半透明覆盖 | IR: 框内区域完整伪彩（热力图）；可见光: 框内区域半透明伪彩叠加（alpha≈0.5），保留原始色彩可辨识 |

**半透明伪彩实现**（可见光画面）：
```
伪彩区域 (BgrFrame ROI) 与 原始可见光区域 进行 alpha blending:
  blended[i] = alpha * pseudocolor[i] + (1 - alpha) * original[i]
  alpha = 0.5f (可调参数)
```

- IR 和可见光并排显示，IR 占左侧约 60%，可见光占右侧约 40%
- 比例可通过 `--side-ratio` 参数调节（默认 0.6）
- YOLO 检测框按各自子画面区域比例映射
- 跟踪 ID、运动箭头、轨迹路径在 640×640 坐标系计算，映射到各子画面
- IR 画面中检测框**内部**区域应用完整伪彩着色（热力图效果）
- 可见光画面中检测框**内部**区域应用半透明伪彩叠加（保留场景可辨识）

### 3.4 多目标跟踪 + 运动意图识别

#### 3.4.1 设计原则

| 结论 | 说明 |
|---|---|
| **跟踪在检测之后** | ITracker 接收 IDetector 的 `vector<Detection>`，输出 `vector<TrackedObject>`。跟踪不影响检测 |
| **跟踪在显示之前** | TrackingOverlay 消费 `TrackedObject`（含 track_id、velocity、trail），不是 `Detection` |
| **Kalman 滤波在跟踪内部** | ByteTrack 内部已用 Kalman 做状态预测（位置、速度），不需要额外的 Kalman 步骤 |
| **运动意图从跟踪数据提取** | 速度向量 → 箭头；轨迹历史 → 路径线。不需要独立的"意图识别器"，在 Overlay 渲染时直接计算 |
| **C++ 原生实现** | 不依赖 boxmot/Python。ByteTrack 算法核心（Kalman + Hungarian + Matching）约 500 行 C++ |

> **经验教训**：Python 版 ByteTrack 依赖 boxmot 会引入 numpy/scipy/scikit-learn 等依赖，容易与 ultralytics/torch 产生版本冲突。C++ 原生实现可完全规避此问题。

#### 3.4.2 ByteTrack 算法核心（C++ 原生实现）

ByteTrack 的核心流程分为四步：

```
输入: detections (vector<Detection>, 640×640 坐标系)
  │
  ├─ Step 1: 用当前帧 detections 的 bbox 和 score，做一次 score thresholding
  │   → split into: high_score_dets (score ≥ thresh), low_score_dets (score < thresh)
  │
  ├─ Step 2: 对每个活跃 track 的 Kalman 滤波器做 predict()
  │   → 预测当前帧的 track 位置 (x, y, w, h, vx, vy)
  │
  ├─ Step 3: 第一轮匹配 (high_score_dets ↔ 活跃 tracks)
  │   → 用 IoU (bbox IoU) 做 Hungarian 匹配
  │   → matched: track 更新 (Kalman update), unmatched_track 保留
  │
  ├─ Step 4: 第二轮匹配 (low_score_dets ↔ unmatched_tracks)
  │   → 用 IoU + 额外约束 (中心距离) 做 Hungarian 匹配
  │   → matched: track 更新, 其余 track 标记为 "lost"
  │
  └─ 输出: 每个 track 含: track_id, bbox, velocity (vx,vy), 连续 N 帧质心历史
```

#### 3.4.3 数据结构

```cpp
// src/tracker.h
#pragma once
#include "detector.h"
#include <vector>
#include <deque>

struct TrackedObject {
    int track_id;               // 分配的跟踪 ID (全局唯一, 从 1 递增)
    float x, y, w, h;           // 边界框 (640×640 坐标系, 与 Detection 一致)
    float confidence;            // 置信度
    int class_id;                // 类别 ID
    float vx, vy;                // 速度向量 (像素/帧, Kalman 预测)
    int age;                     // 连续被跟踪帧数
    bool confirmed;              // 是否稳定确认 (age ≥ track_thresh)
    std::deque<std::pair<float, float>> trail;  // 历史质心 [(cx, cy), ...]
                                                // 最多保留 TRAIL_LENGTH 帧
};

// 新目标确认回调: track 首次达到 confirmed 时调用 (3.4.8 新目标提示音)
typedef void (*NewTargetCallback)(int track_id, void* user_data);

struct TrackerConfig {
    int track_thresh = 5;             // 确认跟踪所需连续帧数
    int track_buffer = 30;            // 丢失后保留帧数
    float match_thresh = 0.8f;       // 匹配 IoU 阈值
    float low_thresh = 0.1f;         // 低置信度阈值
    int max_age = 30;                // 最大丢失帧数
    int trail_length = 15;           // 轨迹历史保留长度
    float iou_weight = 0.9f;        // IoU 匹配权重 (vs 中心距离)
    bool alert_on_new_target = true; // 新目标确认时触发提示音回调
};

class ITracker {
public:
    virtual ~ITracker() {}
    // 初始化 (第一帧, 返回初始跟踪结果)
    virtual std::vector<TrackedObject> init(
        const std::vector<Detection>& detections) = 0;
    // 更新 (后续帧)
    virtual std::vector<TrackedObject> update(
        const std::vector<Detection>& detections) = 0;
    // 重置
    virtual void reset() = 0;
};

// ByteTrack C++ 原生实现
class ByteTrack : public ITracker {
public:
    explicit ByteTrack(const TrackerConfig& config = TrackerConfig());
    std::vector<TrackedObject> init(const std::vector<Detection>& detections) override;
    std::vector<TrackedObject> update(
        const std::vector<Detection>& detections) override;
    void reset() override;
    // 新目标提示音回调注册 (3.4.8)
    void set_new_target_callback(NewTargetCallback callback,
                                 void* user_data = NULL);
private:
    // Kalman filter state (8维: x, y, a, h, vx, vy, va, vh)
    struct KalmanState {
        float x, y, a, h;       // 位置 (cx, cy, aspect_ratio, height)
        float vx, vy, va, vh;   // 速度
    };
    struct Tracklet {
        int track_id;
        KalmanState kf;          // Kalman 滤波器状态
        float time_since_update;
        int age;
        bool confirmed;
        std::deque<std::pair<float, float>> trail;
    };
    TrackerConfig config_;
    std::vector<Tracklet> tracklets_;
    int next_id_;
    NewTargetCallback new_target_callback_;   // 新目标提示音回调 (3.4.8)
    void* new_target_user_data_;
};
```

#### 3.4.4 运动意图识别

运动意图**不经过独立管线步骤**，而是在 TrackingOverlay 渲染时直接从 TrackedObject 数据计算：

**坐标映射**（关键）：

`TrackedObject` 中的 `x, y, w, h, vx, vy` 全部在 **640×640 坐标系**下。但 IR 子画面和可见光副画面在合成帧中的缩放比例不同，因此箭头和轨迹在映射到子画面时需要分别缩放：

```
IR 子画面缩放:   scale_x = ir_width  / 640,   scale_y = ir_height / 640
可见光子画面缩放: scale_x = vis_width / 640,  scale_y = vis_height / 640

映射公式:
  子画面 x = 子画面区域起始x + (640_x / 640) * 子画面宽度
  子画面 y = 子画面区域起始y + (640_y / 640) * 子画面高度
  子画面 vx = vx_640 * scale_x  (箭头长度也按子画面缩放)
  子画面 vy = vy_640 * scale_y
```

**箭头渲染逻辑**（在子画面坐标系中计算）：

```
TrackedObject 数据:
  - vx, vy: Kalman 预测的速度向量 (640×640 坐标系, 像素/帧)
  - trail: 近 N 帧质心历史 (640×640 坐标系)

渲染时 (对每个子画面):
  1. 运动箭头:
     - cx_screen = region_x + (obj.x / 640.0) * region_width
     - cy_screen = region_y + ((obj.y + obj.h) / 640.0) * region_height
     - dir_x = obj.vx * scale_x,  dir_y = obj.vy * scale_y
     - arrow_len = min(sqrt(dir_x² + dir_y²) * arrow_scale, max_arrow_length)
     - 颜色按速度绝对值分级 (见 5.11)

  2. 轨迹路径:
     - 每个 trail 点按 (scale_x, scale_y) 映射到子画面坐标
     - drawPolyline 连线 (半透明, 颜色渐变)
     - 长度衰减: 越早的点越淡

  3. 速度标签 (可选):
     - 在 ID 标签旁显示 "v=5.2" 表示速度 (640 坐标系)
```

**不需要独立的 IIntentRecognizer 组件**。运动意图的计算是纯渲染逻辑，放在 TrackingOverlay 中实现即可。如果后续需要更复杂的意图（如"向左走"、"停下"等语义判断），可以在 ITracker 之后新增 IIntentRecognizer，但现阶段过度设计。

#### 3.4.5 Tracker 与 Overlay 的关系

```
IDetector → detections (vector<Detection>)
                │
                ↓
          ITracker::update(detections)
                │
                ↓
          tracked_objects (vector<TrackedObject>)
                │
                ↓
          TrackingOverlay::draw(frame, tracked_objects, layout, ...)
                │
                ├─ 画检测框 (用 TrackedObject.x/y/w/h)
                ├─ 画跟踪 ID 标签 (用 TrackedObject.track_id)
                ├─ 画运动箭头 (用 TrackedObject.vx/vy)
                ├─ 画轨迹路径 (用 TrackedObject.trail)
                ├─ IR: 框内完整伪彩
                └─ 可见光: 框内半透明伪彩 (alpha blending)
```

#### 3.4.6 性能考虑

| 环节 | 预估开销 (帧处理时间) | 说明 |
|---|---|---|
| ByteTrack update() | < 1ms | Kalman predict + Hungarian match，纯 CPU，track 数量通常 < 20 |
| 运动箭头计算 | < 0.1ms | 从 vx/vy 计算方向，纯数学运算 |
| 伪彩叠加 (IR) | 0.5-2ms | applyColorMap，检测框数量 × 框内像素数 |
| 半透明伪彩 (可见光) | 0.5-2ms | alpha blending，同上 |

总体跟踪+伪彩开销 < 5ms，远小于 NPU 推理开销，不会成为瓶颈。

#### 3.4.7 可选优化：检测间隔策略

当 NPU 推理成为瓶颈时（RV1126B 上推理可能需要 15ms+），可采用**检测间隔策略**提升吞吐量：

```
每 N 帧执行一次 YOLO 推理，中间帧复用上次检测框，仅做 ByteTrack update:

帧 1: YOLO 推理 → ByteTrack.update() → 跟踪结果
帧 2: 跳过 YOLO → 复用帧 1 检测框 → ByteTrack.update() → 跟踪结果
帧 3: 跳过 YOLO → 复用帧 1 检测框 → ByteTrack.update() → 跟踪结果
帧 4: YOLO 推理 → ByteTrack.update() → 跟踪结果
...
```

**实现方式**：

```cpp
// 在主循环中
static int frame_counter = 0;
static std::vector<Detection> cached_detections;

if (frame_counter % detection_interval == 0) {
    // 正常推理
    detector->detect(..., cached_detections);
    tracked_objects = tracker.update(cached_detections);
} else {
    // 跳过推理, 复用缓存检测框
    tracked_objects = tracker.update(cached_detections);
}
frame_counter++;
```

**Trade-off**：
- 优点：推理频率降低 N 倍，吞吐量提升接近 N 倍
- 缺点：检测框滞后 N-1 帧，快速移动目标可能漏检
- 建议：`detection_interval=2`（每 2 帧推理一次），兼顾精度和速度

此策略在 `sample_config.h` 中以 `DETECTION_INTERVAL` 宏控制，默认为 1（每帧都推理）。

**与异步管线的关系**：检测间隔策略和异步管线是**互补**的优化手段：
- 异步管线：解决吞吐量问题（NPU 推理时间不阻塞主线程）
- 检测间隔：降低 NPU Worker 的负载（每 N 帧才做一次推理）
- 两者可组合使用：异步管线 + detection_interval=2，实现极低 NPU 负载下的高帧率

#### 3.4.8 新目标提示音（New Target Alert）

**功能**：ByteTrack 跟踪过程中，当一个新目标从"未确认"变为"已确认"（`age ≥ track_thresh`，即连续 `track_thresh` 帧被稳定跟踪）时，产生一次简单提示音，提示"有新目标进入画面"。

**设计原则**：

| 结论 | 说明 |
|---|---|
| **事件检测在 ByteTrack 内部** | "新目标确认"是跟踪算法的状态迁移（unconfirmed → confirmed），只有 tracker 知道某个 track_id 首次达到 confirmed。检测点在 `update()` 完成两轮匹配、更新 age 之后 |
| **声音播放在 AlertNotifier** | tracker 是纯算法组件（<1ms 预算），不直接操作音频设备。通过回调把事件抛出，由独立的 `AlertNotifier` 组件播放，职责分离 |
| **非阻塞** | 提示音播放（约 120ms）不能阻塞跟踪/显示主循环。AlertNotifier 内部用独立线程播放 + 最短间隔限流 |
| **可配置开关** | 双层控制：`TrackerConfig.alert_on_new_target` 运行时开关（是否触发回调）+ `sample_config.h` 的 `NEW_TARGET_ALERT_ENABLE` 编译期宏（是否注册回调/编译组件） |

**事件触发时序**（在 `ByteTrack::update()` 内部）：

```
Step 4: 第二轮匹配完成, 所有 tracklet 的 age/time_since_update 已更新
    │
    ├─ 遍历活跃 tracklet:
    │    if (!track.confirmed && track.age >= config_.track_thresh):
    │        track.confirmed = true                    ← 状态迁移
    │        if (config_.alert_on_new_target && new_target_callback_):
    │            new_target_callback_(track.track_id,
    │                                  new_target_user_data_)   ← 触发提示音事件
    │
    └─ confirmed 一旦置位不回退: 目标短暂丢失后找回 (track 未删除)
       不会重复触发; 只有全新 track_id 首次确认才触发
```

**AlertNotifier（提示音播放组件）**：

```cpp
// src/alert_notifier.h
class AlertNotifier {
public:
    bool start();   // 打开音频设备 (启动时探测可用后端)
    void stop();
    // 播放"新目标"提示音: 单频短音 (默认 880Hz / 120ms)
    // 非阻塞: 提交到内部播放线程; 限流: 距上次播放不足
    // NEW_TARGET_ALERT_MIN_INTERVAL_MS 时合并为一次
    void play_new_target_tone();
};
```

提示音实现方式（启动时探测，按平台自动回退）：

| 平台 | 实现方式 | 说明 |
|---|---|---|
| 本机 Linux（ALSA 可用） | 手写 880Hz 方波 PCM 写入 default 设备（单声道 8kHz） | 无第三方依赖 |
| 本机（无 ALSA） | `/dev/console` ioctl(KIOCSOUND, 880) | PC 蜂鸣器，需 udev 权限 |
| RV1126B（阶段三） | GPIO 有源蜂鸣器拉高 120ms / I2S 功放 | 嵌入式首选，零 CPU 开销 |
| 全部不可用 | 仅输出日志 `[ALERT] new target #<id>` | 功能降级不崩溃 |

**主循环集成**：

```cpp
// 初始化
ByteTrack tracker;
AlertNotifier alert;
#if NEW_TARGET_ALERT_ENABLE
alert.start();
tracker.set_new_target_callback(
    [](int track_id, void* user) {
        static_cast<AlertNotifier*>(user)->play_new_target_tone();
    }, &alert);
#endif

while (...) {
    tracked_objects = tracker.update(detections);  // 新目标确认时内部触发回调
    ...
}
```

**防抖规则**：
- 单帧多个新目标同时确认 → 只播一次（AlertNotifier 合并）
- `NEW_TARGET_ALERT_MIN_INTERVAL_MS`（默认 500ms）内重复触发 → 只播一次
- 已确认目标丢失后找回 → 不重复触发（confirmed 状态不回退）

## 4. 分阶段实施计划

### 阶段一：模型导出（ONNX）

**目标**：将 YOLOFuse 的 `best.pt` 导出为 ONNX 模型

**操作步骤**：
1. 在 YOLOFuse 的 venv 中执行导出：
   ```python
   from ultralytics import YOLO
   model = YOLO("/mnt/hgfs/share/code/yolofuse-mid/weights/best.pt")
   model.export(format="onnx", dynamic=False, simplify=True)
   ```
2. 导出参数说明：
   - `dynamic=False`：固定输入尺寸 `[1, 6, 640, 640]`，便于 RV1126B 部署
   - `simplify=True`：用 onnxslim 简化图，减少算子数量
   - 输入名：`images`，输出名：`output0`
3. 验证 ONNX 模型：
   ```python
   import onnxruntime as ort
   import numpy as np
   sess = ort.InferenceSession("best.onnx")
   dummy = np.random.rand(1, 6, 640, 640).astype(np.float32)
   out = sess.run(["output0"], {"images": dummy})
   print(out[0].shape)  # 预期 [1, 84, 8400] (nc=80: 4 box + 80 cls)
   ```
4. 产出文件：`best.onnx`，复制到 `twocameraOnMAC/models/` 目录

### 阶段二：本机 C++ ONNX 集成

**目标**：在 twocameraOnMAC 中嵌入 ONNX Runtime，实现实时推理

#### 4.2.1 新增文件

| 文件 | 职责 |
|---|---|
| `src/detector.h` | `IDetector` 抽象接口 + `Detection` 结构体 |
| `src/detector_onnx.h/cpp` | ONNX Runtime 推理实现 |
| `src/yolo_postprocess.h/cpp` | YOLO 输出解码 + NMS + 画框 |
| `src/side_by_side_compositor.h/cpp` | 并排合成器（IR 主画面 + 可见光副画面） |
| `src/detection_overlay.h/cpp` | 检测框绘制 + IR 框内伪彩叠加 |
| `src/pseudo_color.h/cpp` | 红外灰度→伪彩着色（OpenCV applyColorMap 封装） |

#### 4.2.2 接口设计

```cpp
// src/detector.h
#pragma once
#include "frame.h"
#include <vector>

struct Detection {
    float x, y, w, h;      // 边界框 (像素坐标，基于 640×640)
    float confidence;       // 置信度
    int class_id;           // 类别 ID
};

class IDetector {
public:
    virtual ~IDetector() {}
    virtual bool load(const std::string& model_path) = 0;
    virtual bool detect(const BgrFrame& visible, const BgrFrame& infrared,
                        std::vector<Detection>& results) = 0;
    virtual int model_size() const = 0;  // 640
};
```

```cpp
// src/side_by_side_compositor.h
#pragma once
#include "frame.h"
#include "image_registrator.h"

struct SideBySideConfig {
    float ir_ratio = 0.6f;     // IR 画面占比（0.0~1.0）
    int gap = 4;               // 中间分割线宽度（像素）
    uint8_t gap_color[3] = {128, 128, 128}; // 分割线颜色 BGR
};

struct SideBySideLayout {
    int ir_x = 0, ir_y = 0;
    int ir_width = 0, ir_height = 0;
    int vis_x = 0, vis_y = 0;
    int vis_width = 0, vis_height = 0;
    bool valid = false;
};

class SideBySideCompositor {
public:
    explicit SideBySideCompositor(const SideBySideConfig& config = SideBySideConfig());
    bool compose(const RegisteredFrames& input, int output_width,
                 int output_height, BgrFrame& output) const;
    const SideBySideLayout& last_layout() const { return last_layout_; }
private:
    SideBySideConfig config_;
    mutable SideBySideLayout last_layout_;
};
```

```cpp
// src/tracking_overlay.h
#pragma once
#include "frame.h"
#include "side_by_side_compositor.h"
#include "tracker.h"
#include <vector>

struct TrackingOverlayConfig {
    // 基础绘制
    bool draw_on_ir = true;
    bool draw_on_visible = true;
    int box_thickness = 2;
    // 标签
    bool draw_track_id = true;       // 画跟踪 ID (#7)
    bool draw_class_label = true;    // 画类别标签
    bool draw_confidence = true;    // 画置信度
    // 运动意图
    bool draw_motion_arrow = true;   // 画运动方向箭头
    bool draw_trail = true;          // 画轨迹路径
    float arrow_scale = 2.0f;        // 箭头长度系数 (像素/单位速度)
    float max_arrow_length = 60.0f;  // 箭头最大长度
    int trail_length = 15;           // 轨迹最大保留点数
    // 伪彩
    bool ir_pseudo_color = true;     // IR: 框内完整伪彩
    bool visible_pseudo_color = true; // 可见光: 框内半透明伪彩
    float visible_pseudo_alpha = 0.5f; // 可见光伪彩透明度 (0.0~1.0)
    int colormap = 2;                // cv::COLORMAP_JET
    // 速度阈值
    float speed_threshold = 0.5f;    // 低于此速度不画箭头 (静止判定)
};

class TrackingOverlay {
public:
    explicit TrackingOverlay(
        const TrackingOverlayConfig& config = TrackingOverlayConfig());

    // 在并排合成帧上绘制: 检测框 + 跟踪ID + 运动箭头 + 轨迹 + 伪彩
    // tracked_objects 是 640×640 坐标系
    void draw(BgrFrame& frame,
              const std::vector<TrackedObject>& tracked_objects,
              const SideBySideLayout& layout,
              int ir_original_w, int ir_original_h,
              int vis_original_w, int vis_original_h) const;

private:
    TrackingOverlayConfig config_;

    // 子画面区域内的绘制
    void draw_on_region(BgrFrame& frame, int region_x, int region_y,
                        int region_w, int region_h,
                        int orig_w, int orig_h,
                        const std::vector<TrackedObject>& objects,
                        bool is_ir_region) const;

    // 伪彩
    void apply_ir_pseudo_color(BgrFrame& frame, int region_x, int region_y,
                               int region_w, int region_h,
                               const std::vector<TrackedObject>& objects) const;
    void apply_visible_pseudo_color(BgrFrame& frame, int region_x, int region_y,
                                    int region_w, int region_h,
                                    const std::vector<TrackedObject>& objects) const;

    // 运动箭头
    void draw_motion_arrow(BgrFrame& frame, int cx, int cy,
                           float vx, float vy, float max_len) const;

    // 轨迹路径
    void draw_trail(BgrFrame& frame, int cx, int cy,
                    const std::deque<std::pair<float, float>>& trail,
                    float scale_x, float scale_y) const;

    // 标签
    void draw_track_label(BgrFrame& frame, int x, int y,
                          const TrackedObject& obj) const;
};
```

```cpp
// src/pseudo_color.h
#pragma once
#include "frame.h"

// 将灰度区域转换为伪彩（热力图效果）
// 在 BgrFrame 的指定矩形区域内，提取灰度值 → applyColorMap → 覆写回去
void apply_pseudo_color_in_rect(BgrFrame& frame,
                                int x, int y, int width, int height,
                                int colormap = 2 /* cv::COLORMAP_JET */);

// 半透明伪彩叠加 (可见光画面使用)
// alpha=0.5 表示伪彩和原始图像各占 50%
void apply_pseudo_color_blend(BgrFrame& frame,
                              int x, int y, int width, int height,
                              float alpha = 0.5f,
                              int colormap = 2);
```

#### 4.2.5.1 RV1126B 上 OpenCV 替代方案

RV1126B 上 OpenCV 可能不可用或过重，以下组件提供纯 C++ 替代实现：

| OpenCV 函数 | RV1126B 替代实现 | 说明 |
|---|---|---|
| `cv::applyColorMap(gray, color, COLORMAP_JET)` | **LUT 查色表**（64 条颜色预计算） | JET 伪彩只有 64 种颜色，用 `color = jet_lut[gray_value >> 2]` 查表即可 |
| `cv::addWeighted(src1, a, src2, b, 0)` | **逐像素循环** | `dst[i] = a * src1[i] + b * src2[i]`，SIMD 友好 |
| `cv::resize(src, dst, size)` | **RGA 硬件** | RV1126B RGA 支持 resize，零 CPU 开销 |
| `cv::cvtColor(src, dst, BGR2RGB)` | **RGA 硬件** | RGA 支持 BGR↔RGB CSC |
| `cv::cvtColor(src, dst, BGR2GRAY)` | **手写 Y 公式** | `gray = 0.299*R + 0.587*G + 0.114*B`，定点整数运算 |
| `cv::drawRect/putText/drawArrow` | **手写绘图基元** | Framebuffer 直接像素操作 |
| `cv::drawPolyline` | **Bresenham 算法** | 轨迹路径绘制 |

**JET 伪彩 LUT 实现**（仅 64 条，预计算一次）：

```cpp
// src/pseudo_color_lut.h
struct JetLUT {
    // 64 级 JET 伪彩, RGB 各 8bit
    static constexpr int LUT_SIZE = 64;
    struct RGB { uint8_t r, g, b; };
    RGB table[LUT_SIZE];

    JetLUT(); // 构造时预计算

    // 输入: 0-255 灰度值, 输出 BGR 颜色
    inline void lookup(uint8_t gray, uint8_t& b, uint8_t& g, uint8_t& r) const {
        int idx = gray >> 2; // 0-63
        r = table[idx].r;
        g = table[idx].g;
        b = table[idx].b;
    }
};
```

**性能对比**：

| 操作 | OpenCV | RV1126B 纯 C++ | 加速比 |
|---|---|---|---|
| JET 伪彩 (640×640) | ~1.5ms | ~0.3ms | 5x |
| alpha blending (640×640) | ~1.2ms | ~0.4ms | 3x |
| BGR→GRAY (640×640) | ~0.8ms | ~0.2ms | 4x |

在 RV1126B 上，用纯 C++ 替代 OpenCV 伪彩管线，总体可节省约 3ms/帧的 CPU 时间。

**CMake 条件编译**：
```cmake
if(TWOCAMERA_HAS_OPENCV)
    # 使用 OpenCV 实现
    target_compile_definitions(twocamera PRIVATE TWOCAMERA_USE_OPENCV_PSEUDO=1)
else()
    # 使用 RV1126B 纯 C++ 实现
    list(APPEND TWOCAMERA_SOURCES src/pseudo_color_lut.cpp)
endif()
```

```cpp
// src/image_registrator.h
#pragma once
#include "frame.h"

struct RegistrationConfig {
    // crop+resize 模式: 按 IR 的 FOV 裁剪 visible 中心区域
    int crop_x = 0, crop_y = 0;
    int crop_width = 256, crop_height = 192;
    // warpPerspective 模式: 用标定的 homography 矩阵
    float homography[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // 默认单位矩阵
    bool use_homography = false;  // true=warp, false=crop
    bool use_rga = false;         // RV1126B: true=用RGA硬件, false=用OpenCV
};

struct RegisteredFrames {
    BgrFrame infrared;       // IR 帧 (无需配准, 直接传递)
    BgrFrame visible;        // 可见光帧 (已配准到 IR 视角)
    int64_t timestamp_us;    // 同步时间戳
    bool valid = false;
};

class ImageRegistrator {
public:
    explicit ImageRegistrator(const RegistrationConfig& config = RegistrationConfig());
    // 将 visible 帧配准到 infrared 帧的坐标系
    // 输出: RegisteredFrames, visible 已变换, infrared 直接传递
    bool register_frames(const SyncedFrames& input, RegisteredFrames& output) const;
private:
    RegistrationConfig config_;
};
```

#### 4.2.3 预处理流程（C++）

```
输入: registered_visible BgrFrame (已配准, 与 IR 同视角), infrared BgrFrame (256×192 BGR)
  │
  ├─ 0. 注: 配准在预处理之前由 ImageRegistrator 完成, 预处理只做 resize/normalize/merge
  │
  ├─ 1. Resize: 两个帧都用 cv::resize 缩放到 640×640
  │    registered_visible: → 640×640 (BGR)
  │    infrared: 256×192 → 640×640 (BGR)
  │
  ├─ 2. BGR → RGB: cv::cvtColor(BGR2RGB)（ONNX 模型训练时用 RGB）
  │
  ├─ 3. 归一化: pixel / 255.0 → float32, 范围 [0, 1]
  │
  ├─ 4. HWC → CHW: 转置内存布局
  │    visible: [640, 640, 3] → [3, 640, 640]
  │    infrared: [640, 640, 3] → [3, 640, 640]
  │
  ├─ 5. 通道拼接: memcpy 两个 [3, 640, 640] → [6, 640, 640]
  │    通道顺序: [R_ir, G_ir, B_ir, R_vis, G_vis, B_vis]
  │    （与 YOLOFuse 的 cv2.merge((ir, rgb)) 一致：前3ch=IR, 后3ch=RGB）
  │
  └─ 6. 组装 batch: [1, 6, 640, 640] float32 tensor
```

> **通道顺序注意**：YOLOFuse 训练时 `cv2.merge((ir, rgb))`，IR 在前 3 通道，RGB 在后 3 通道。C++ 预处理必须保持一致。

#### 4.2.4 后处理流程（C++）

```
ONNX 输出: [1, 84, 8400] (nc=80 时: 4 box + 80 cls)
  │
  ├─ 1. 转置 + 遍历 8400 个 anchor
  │
  ├─ 2. 过滤: max_conf > conf_threshold (0.25)
  │
  ├─ 3. NMS: IoU > 0.45 的低分框抑制
  │    （需要自实现 NMS 或用 OpenCV::dnn::NMSBoxes）
  │
  ├─ 4. 坐标保持 640×640 坐标系 (不在此处还原到原始分辨率)
  │    Detection.x/y/w/h 仍为 640×640 坐标
  │    各子画面绘制时按各自缩放比映射
  │
  └─ 5. 返回 vector<Detection> (640×640 坐标系)
```

#### 4.2.5 跟踪 + 伪彩 + 运动意图叠加流程

```
TrackingOverlay::draw(composed_frame, tracked_objects, layout, ir_w, ir_h, vis_w, vis_h)
  │
  ├─ 前置: ITracker::update(detections) → tracked_objects
  │    （ByteTrack 内部: Kalman predict → Hungarian match → 分配 track_id）
  │
  ├─ IR 主画面区域 (layout.ir_x, ir_y, ir_width, ir_height)
  │    │
  │    ├─ 伪彩着色（完整覆盖）:
  │    │    1. 每个 TrackedObject 的 bbox
  │    │    2. 提取 ROI → cv::cvtColor(BGR2GRAY) → applyColorMap(JET)
  │    │    3. 覆写回 composed_frame 的 ROI
  │    │    → 框内呈现热力图效果（红=高温, 蓝=低温）
  │    │
  │    ├─ 画检测框 (在伪彩上方, 确保可见)
  │    ├─ 画跟踪 ID 标签 (#7)
  │    ├─ 画运动箭头 (框底部, 方向=速度向量)
  │    └─ 画轨迹路径 (历史质心连线, 透明度衰减)
  │
  └─ 可见光副画面区域 (layout.vis_x, vis_y, vis_width, vis_height)
       │
       ├─ 半透明伪彩 (alpha blending, alpha=0.5):
       │    1. 每个 TrackedObject 的 bbox
       │    2. 提取 ROI → gray → applyColorMap(JET)
       │    3. cv::addWeighted(original ROI, 1-alpha, pseudocolor, alpha, 0)
       │    4. 覆写回 composed_frame
       │    → 框内半透明伪彩叠加，保留可见光原始色彩可辨识
       │
       ├─ 画检测框
       ├─ 画跟踪 ID 标签 (#7)
       ├─ 画运动箭头
       └─ 画轨迹路径
```

#### 4.2.6 主循环集成

##### 同步模式（本机开发用）

在 `sample.cpp` 的 `run_once()` 捕获循环中插入推理+跟踪：

```cpp
// 初始化
SideBySideConfig sbs_config;
sbs_config.ir_ratio = side_ratio;
SideBySideCompositor compositor(sbs_config);
TrackingOverlay overlay;
ByteTrack tracker;
RegistrationConfig reg_config = load_calibration("calibration.json");
ImageRegistrator registrator(reg_config);
std::unique_ptr<IDetector> detector = create_detector();
detector->load("models/best.onnx");
FramePool<> frame_pool;  // 预分配帧池
AsyncPipeline async_pipeline;  // 可选: 异步管线
bool use_async = false;  // CMake 配置决定
bool tracker_initialized = false;

while (!stop_requested) {
    if (synchronizer.acquire(synced_frames)) {
        // ★ 1. 图像配准
        RegisteredFrames registered;
        if (!registrator.register_frames(synced_frames, registered)) continue;

        // ★ 2. YOLO 推理 + ByteTrack (同步)
        std::vector<Detection> detections;
        detector->detect(registered.visible, registered.infrared, detections);
        std::vector<TrackedObject> tracked_objects;
        if (!tracker_initialized) {
            tracked_objects = tracker.init(detections);
            tracker_initialized = true;
        } else {
            tracked_objects = tracker.update(detections);
        }

        // ★ 3. 并排合成
        compositor.compose(registered, display->width(),
                           display->height(), composed_frame);

        // ★ 4. 跟踪 + 伪彩 + 运动意图叠加
        overlay.draw(composed_frame, tracked_objects,
                     compositor.last_layout(),
                     registered.infrared.width, registered.infrared.height,
                     registered.visible.width, registered.visible.height);

        // ★ 5. Downscale (仅 SPI 屏需要, 本机 OpenCV 不需要)
        BgrFrame display_frame;
        if (display->requires_downscale()) {
            rga_downscale(composed_frame, display_frame, 240, 320);
        } else {
            display_frame = composed_frame;
        }

        // ★ 6. 显示
        display->present(display_frame);
    }
}
```

##### 异步模式（RV1126B 生产部署用，实现 25fps）

```cpp
// 初始化
AsyncPipeline pipeline;
pipeline.start(detector, tracker);  // 启动 NPU Worker 线程

while (!stop_requested) {
    if (synchronizer.acquire(synced_frames)) {
        // ★ 1. 图像配准
        RegisteredFrames registered;
        if (!registrator.register_frames(synced_frames, registered)) continue;

        // ★ 2. 将帧送入 NPU Worker (非阻塞)
        pipeline.submit(registered);

        // ★ 3. 取最新推理结果 (可能为上一帧)
        std::vector<TrackedObject> tracked_objects;
        pipeline.get_latest_results(tracked_objects);

        // ★ 4. 并排合成 (主线程零推理阻塞)
        compositor.compose(registered, ..., composed_frame);

        // ★ 5. 叠加 + Downscale + 显示
        overlay.draw(composed_frame, tracked_objects, ...);
        rga_downscale(composed_frame, display_frame, 240, 320);
        display->present(display_frame);
    }
}
```

**同步主循环步骤表**：

| 步骤 | 组件 | 输入 | 输出 | 耗时 | RV1126B 硬件 |
|---|---|---|---|---|---|
| 1 | ImageRegistrator | SyncedFrames | RegisteredFrames | <1ms | RGA crop+resize |
| 2 | IDetector | visible/infrared | vector\<Detection\> | 5-15ms | NPU 推理 |
| 3 | ByteTrack | detections | vector\<TrackedObject\> | <1ms | CPU |
| 4 | SideBySideCompositor | RegisteredFrames | composed_frame | <1ms | CPU memcpy |
| 5 | TrackingOverlay | frame + tracked | annotated_frame | 1-3ms | CPU (LUT 伪彩) |
| 6 | Downscale (RGA) | composed_frame | display_frame (240×320) | <1ms | RGA resize |
| 7 | IDisplay | display_frame | 屏幕显示 | <5ms | LVGL + SPI DMA |
| **合计** | | | | **8-26ms** | |

**异步模式下主线程耗时**：步骤 1 + 4 + 5 + 6 + 7 = **3-10ms**，远低于 33ms（25fps 帧间隔）。

#### 4.2.7 构建系统修改

CMakeLists.txt 新增：

```cmake
# ONNX Runtime 选项
option(TWOCAMERA_ENABLE_YOLO "Enable YOLO detection" OFF)
option(TWOCAMERA_ENABLE_TRACKER "Enable ByteTrack tracking" ON)
set(TWOCAMERA_ONNXRUNTIME_DIR "" CACHE PATH "ONNX Runtime directory")
set(TWOCAMERA_LAYOUT_MODE "SIDEBYSIDE" CACHE STRING "Layout: SIDEBYSIDE or PIP")

if(TWOCAMERA_ENABLE_YOLO)
    # 查找 ONNX Runtime
    find_path(ONNXRUNTIME_INCLUDE_DIR
        onnxruntime_cxx_api.h
        PATHS "${TWOCAMERA_ONNXRUNTIME_DIR}/include")
    find_library(ONNXRUNTIME_LIB
        onnxruntime
        PATHS "${TWOCAMERA_ONNXRUNTIME_DIR}/lib")
    # ...
    list(APPEND TWOCAMERA_SOURCES
        src/detector_onnx.cpp
        src/yolo_postprocess.cpp
        src/tracking_overlay.cpp
        src/pseudo_color.cpp
        src/pseudo_color_lut.cpp
        src/image_registrator.cpp
        src/tracker_bytetrack.cpp
        src/kalman_filter.cpp
        src/frame_pool.cpp
        src/async_pipeline.cpp)
    if(TWOCAMERA_ENABLE_RGA)
        list(APPEND TWOCAMERA_SOURCES src/image_registrator_rga.cpp)
        target_compile_definitions(twocamera PRIVATE TWOCAMERA_HAS_RGA=1)
    endif()
    if(TWOCAMERA_ENABLE_TRACKER)
        target_compile_definitions(twocamera PRIVATE TWOCAMERA_HAS_TRACKER=1)
    endif()
    if(TWOCAMERA_HAS_OPENCV)
        target_compile_definitions(twocamera PRIVATE TWOCAMERA_USE_OPENCV_PSEUDO=1)
    endif()
    if(TWOCAMERA_LAYOUT_MODE STREQUAL "SIDEBYSIDE")
        list(APPEND TWOCAMERA_SOURCES src/side_by_side_compositor.cpp)
        target_compile_definitions(twocamera PRIVATE TWOCAMERA_LAYOUT_SIDEBYSIDE=1)
    else()
        list(APPEND TWOCAMERA_SOURCES src/frame_compositor.cpp)
    endif()
    target_compile_definitions(twocamera PRIVATE TWOCAMERA_HAS_YOLO=1)
    target_link_libraries(twocamera PRIVATE ${ONNXRUNTIME_LIB})
endif()
```

编译命令：
```bash
cmake -B build/native \
  -DTWOCAMERA_DISPLAY_BACKEND=OPENCV \
  -DTWOCAMERA_SDK_LIB_DIR=<sdk_path> \
  -DTWOCAMERA_ENABLE_YOLO=ON \
  -DTWOCAMERA_ONNXRUNTIME_DIR=/path/to/onnxruntime \
  -DTWOCAMERA_HAS_OPENCV=ON \
  -DTWOCAMERA_LAYOUT_MODE=SIDEBYSIDE
```

#### 4.2.8 ONNX Runtime 获取

本机为 aarch64 Linux，下载对应预编译包：
- 下载地址：`https://github.com/microsoft/onnxruntime/releases`
- 选择：`onnxruntime-linux-aarch64-<version>.tar.gz`
- 解压后目录结构：
  ```
  onnxruntime-linux-aarch64/
  ├── include/    # onnxruntime_cxx_api.h 等
  └── lib/        # libonnxruntime.so
  ```

### 阶段三：RV1126B 部署

#### 4.3.1 平台概述

RV1126B 是 Rockchip 的视觉 SoC，内置 NPU（2.0 TOPS），支持 INT8/INT16 量化推理。

#### 4.3.2 模型转换链路

```
best.pt → best.onnx → best.rknn (INT8 量化)
```

**转换步骤**（在 x86 PC 上用 rknn-toolkit2）：
```python
from rknn.api import RKNN
rknn = RKNN()
rknn.config(
    mean_values=[[0, 0, 0]],
    std_values=[[255, 255, 255]],
    target_platform='rv1126'
)
rknn.load_onnx(model='best.onnx')
rknn.build(do_quantization=True, dataset='dataset.txt')  # dataset.txt 含校准图片路径
rknn.export_rknn('best.rknn')
```

> 注意：RV1126B 的 RKNN SDK 可能对某些 ONNX 算子支持有限，需要实际测试。ModalitySelector 的 slice 操作、Concat 操作通常支持良好。

#### 4.3.3 C++ RKNN 推理实现

新增 `src/detector_rknn.h/cpp`：

```cpp
// 同样实现 IDetector 接口
class RknnDetector : public IDetector {
    bool load(const std::string& model_path) override;
    bool detect(const BgrFrame& visible, const BgrFrame& infrared,
                std::vector<Detection>& results) override;
    // 内部使用 rknn_api.h 的 C 接口:
    // rknn_init(), rknn_inputs_set(), rknn_run(), rknn_outputs_get()
};
```

#### 4.3.4 构建配置

```cmake
option(TWOCAMERA_YOLO_BACKEND "ONNX or RKNN" "ONNX")

if(TWOCAMERA_YOLO_BACKEND STREQUAL "RKNN")
    set(TWOCAMERA_RKNN_DIR "" CACHE PATH "RKNN SDK directory")
    list(APPEND TWOCAMERA_SOURCES src/detector_rknn.cpp src/yolo_postprocess.cpp)
    target_include_directories(twocamera PRIVATE "${TWOCAMERA_RKNN_DIR}/include")
    target_link_libraries(twocamera PRIVATE "${TWOCAMERA_RKNN_DIR}/lib/librknn_api.so")
    target_compile_definitions(twocamera PRIVATE TWOCAMERA_HAS_YOLO=1 TWOCAMERA_YOLO_RKNN=1)
endif()
```

#### 4.3.5 RV1126B 交叉编译

新增工具链文件 `cmake/toolchains/rv1126-toolchain.cmake`（参考现有 T113 工具链格式）。

编译命令：
```bash
cmake -B build/rv1126 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/rv1126-toolchain.cmake \
  -DTWOCAMERA_DISPLAY_BACKEND=FBDEV \
  -DTWOCAMERA_SDK_LIB_DIR=<rv1126_sdk_libs> \
  -DTWOCAMERA_ENABLE_YOLO=ON \
  -DTWOCAMERA_YOLO_BACKEND=RKNN \
  -DTWOCAMERA_RKNN_DIR=<rknn_sdk_path> \
  -DTWOCAMERA_NO_JPEG=OFF
```

## 5. 关键设计决策

### 5.1 为什么用 C++ 内嵌推理而非 Python 子进程

| 方案 | 延迟 | 实时性 | 部署复杂度 | RV1126B 适配 |
|---|---|---|---|---|
| Python 子进程（IPC） | 100~300ms | 差 | 需维护两个进程 | RV1126B 无 Python |
| C++ ONNX Runtime | 20~80ms | 好 | 单进程 | 可切换 RKNN |
| C++ OpenCV DNN | 50~200ms | 一般 | 无额外依赖 | OpenCV DNN 不支持 RKNN |

选择 C++ 内嵌方案：延迟低、单进程、便于移植到 RV1126B。

### 5.2 通道顺序对齐

YOLOFuse 训练时的通道拼接代码（[loaders.py:431](ultralytics/data/loaders.py#L431)）：
```python
cv2.merge((cv2.imread(ir_path), cv2.imread(path)))  # IR 在前, RGB 在后
```

C++ 预处理必须保持：**IR 的 3 通道在前 [0:3]，RGB 的 3 通道在后 [3:6]**。

### 5.3 红外帧来源选择

IrCamera 有两种输出模式：
- **BGR 伪彩/灰度图**（通过 `copy_latest(BgrFrame&)`）→ 与训练数据一致
- **原始 Y14/Y16 温度数据**（通过 `copy_latest_temperature(TemperatureFrame&)`）→ 与训练数据不一致

选择 `copy_latest(BgrFrame&)` 输出的 BGR 帧，因为 YOLOFuse 训练数据（LLVIP）的红外图就是 BGR 格式。

### 5.4 抽象 IDetector 接口

通过抽象接口隔离推理后端：
- 本机：`OnnxDetector`（ONNX Runtime）
- RV1126B：`RknnDetector`（RKNN API）
- 预处理和后处理代码共享（`yolo_postprocess.h/cpp`）

### 5.5 显示布局：并排 vs PiP

原 `FrameCompositor` 采用 PiP（画中画）布局，IR 作为小窗叠加在可见光画面上。
本方案改为 **`SideBySideCompositor` 并排布局**：IR 主画面占左侧 60%，可见光副画面占右侧 40%。

两者共存：通过 CMake 选项 `TWOCAMERA_LAYOUT_MODE` 切换，`SIDEBYSIDE` 为默认模式，`PIP` 为兼容旧模式。

### 5.6 IR 框内伪彩着色

红外画面中检测框内部区域应用伪彩（热力图），增强目标区域的可视性：

```
原始 IR BGR 帧 → 框内 ROI 提取 → BGR2GRAY → applyColorMap(JET) → 覆写回 ROI
```

- 使用 OpenCV `cv::applyColorMap(gray, colored, cv::COLORMAP_JET)`
- 只对检测框**内部**像素着色，框外保持原始 IR 图像
- 框线在伪彩**之后**绘制，确保边线可见
- 可见光画面**不**做伪彩，保留原始色彩

### 5.7 检测框坐标映射

检测框在 640×640 坐标系中输出，需映射到并排合成帧的两个子区域：

```
IR 子画面映射:
  frame_x = ir_x + (det.x / 640) * ir_width
  frame_y = ir_y + (det.y / 640) * ir_height

可见光子画面映射:
  frame_x = vis_x + (det.x / 640) * vis_width
  frame_y = vis_y + (det.y / 640) * vis_height
```

由于 IR 原始分辨率（256×192）和可见光（640×480）不同，但都被 resize 到 640×640 送入模型，所以检测结果在 640×640 坐标系是统一的，映射到各自子画面时按比例缩放即可。

### 5.8 图像配准：独立管线步骤

**设计结论**：

1. **先同步后配准**：同步（时间对齐）和配准（空间对齐）是正交的两个维度。配准需要同一时刻的两路帧，必须在同步之后进行。不提前在采集线程中做配准，避免阻塞采集影响帧率。

2. **配准独立运行**：作为 `ImageRegistrator` 独立组件，不嵌入采集线程、不嵌入 Synchronizer、不嵌入 IDetector 预处理。输出 `RegisteredFrames` 供下游检测和显示共用。

3. **配准在预处理前**：IDetector 预处理接收的 visible 帧已经是配准后的。预处理只做 resize/normalize/merge，不做空间变换。配准与推理职责分离。

4. **不参与推理过程**：ONNX 模型只接收 `[1, 6, 640, 640]` tensor，配准发生在 tensor 构造之前。模型不需要知道输入是否配准过——配准是管线步骤，不是模型参数。

5. **可用硬件加速**：本机用 OpenCV `warpPerspective`/crop；RV1126B 用 RGA 硬件做 crop+resize+CSC，与 NPU 推理并行执行，可流水线化。

### 5.9 显示后端：SPI 屏驱动方案

#### 方案对比

| | 方案 A（优先） | 方案 B（备选） |
|---|---|---|
| **名称** | LVGL + 自定义 flush_cb + SPI DMA | LVGL + DRM/KMS |
| **路径** | LVGL → disp_flush_cb → SPI DMA → ILI9341 | LVGL → lv_drivers/drm → /dev/dri/card0 → 内核显示管线 |
| **驱动依赖** | SPI 驱动（/dev/spidev）+ DMA | DRM/KMS 驱动（Rockchip DRM） |
| **不需要** | DRM/KMS、Linux FB、显示子系统驱动 | 自行管 SPI 时序、ILI9341 初始化 |
| **用户态职责** | ILI9341 初始化序列、窗口设置、SPI DMA 启动、完成回调 | 仅调 DRM ioctl（drmModeAtomicCommit 等） |
| **内核职责** | SPI DMA 传输 | page flip、vsync 同步、多图层混合、硬件光标 |
| **CPU 开销** | LVGL 渲染（CPU）+ BGR→RGB565 转换（可用 RGA 替代）+ SPI DMA 回调 | LVGL 渲染（CPU）+ DRM ioctl（轻量） |
| **SPI 带宽** | 全屏 153.6KB/帧，40MHz≈26FPS；局部刷新只传脏区域 | 不适用（DRM 走 RGB/MIPI 接口，非 SPI） |
| **适用屏** | 2.4 寸 SPI 屏（ILI9341/ST7789） | RGB 并口屏 / MIPI 屏 / HDMI |
| **撕裂** | 可能撕裂（无 vsync 同步，除非屏控制器有 TE 引脚） | 无撕裂（内核 vsync page flip） |

#### 方案 A（优先）：LVGL → 自定义 flush_cb → SPI + DMA → ILI9341

```
┌──────────────┐
│  YOLO 管线    │
│  (配准→检测   │
│   →合成→叠加) │
└──────┬───────┘
       │ BgrFrame
       ↓
┌──────────────┐
│  RGA (硬件)   │  BGR24 → RGB565 + 缩放到 240×320
│  (可选, DMA)  │
└──────┬───────┘
       │ RGB565 (240×320)
       ↓
┌──────────────┐
│  LVGL        │  渲染到 draw_buf (部分缓冲)
│  (CPU 渲染)   │  → lv_canvas 画面 + UI 控件
└──────┬───────┘
       │ 脏区域像素
       ↓
┌──────────────┐
│  disp_flush_cb│  自定义回调:
│  (用户态)     │  1. 设置 ILI9341 窗口 (dirty rect)
│              │  2. 启动 SPI DMA 传输
│              │  3. DMA 完成后调 lv_disp_flush_ready()
└──────┬───────┘
       │ SPI DMA
       ↓
┌──────────────┐
│  ILI9341     │  2.4 寸 SPI LCD
└──────────────┘
```

核心实现（伪代码）:

```cpp
// display_spi_lvgl.cpp
static void disp_flush_cb(lv_disp_drv_t* drv,
                          const lv_area_t* area,
                          lv_color_t* color) {
    // 1. 设置 ILI9341 显示窗口（只刷脏区域）
    lcd_set_window(area->x1, area->y1,
                   area->x2, area->y2);
    // 2. 计算传输字节数
    size_t len = (area->x2 - area->x1 + 1) *
                 (area->y2 - area->y1 + 1) * 2; // RGB565 = 2 bytes
    // 3. 启动 SPI DMA（非阻塞）
    spi_dma_transfer_async(color, len, []() {
        // 4. DMA 完成中断回调
        lv_disp_flush_ready(drv);
    });
}
```

lv_drv_conf.h 配置:

```c
// 关掉 fbdev 和 drm
#define USE_FBDEV    0
#define USE_DRM      0
// 不使用任何内置显示驱动，由 display_spi_lvgl.cpp
// 自行注册 disp_drv_.flush_cb
```

**优点**:
- 不依赖显示子系统驱动（DRM/FB），内核只需 SPI 驱动
- 部分刷新：只传输 LVGL 脏区域，SPI 带宽利用率高
- 完全用户态控制，调试方便

**缺点**:
- 无 vsync 同步，可能画面撕裂（可接 ILI9341 TE 引脚缓解）
- ILI9341 初始化序列由用户态管理

#### 方案 B（备选）：LVGL + DRM/KMS

```
┌──────────────┐
│  YOLO 管线    │
└──────┬───────┘
       │ BgrFrame
       ↓
┌──────────────┐
│  RGA (硬件)   │  BGR24 → RGB565 + 缩放
└──────┬───────┘
       │ RGB565
       ↓
┌──────────────┐
│  LVGL        │  → lv_drivers/drm
│  (CPU 渲染)   │  → drmModeAtomicCommit
└──────┬───────┘
       │
       ↓
┌──────────────┐
│  内核 DRM    │  page flip (vsync 同步)
│  (Rockchip)  │  多图层混合 (UI + video)
│              │  硬件光标
└──────┬───────┘
       │ RGB/MIPI 接口
       ↓
┌──────────────┐
│  屏幕硬件    │  RGB 并口屏 / MIPI 屏
└──────────────┘
```

lv_drv_conf.h 配置:

```c
#define USE_FBDEV    0
#define USE_DRM      1   // 启用 lv_drivers 的 DRM 后端
```

**优点**:
- vsync 同步，无撕裂
- 内核管理 page flip、图层叠加、硬件光标，CPU 开销低
- 多图层：video layer（摄像头帧）+ overlay layer（UI 控件）硬件叠加

**缺点**:
- 需要内核 DRM 驱动支持（RV1126B 的 Rockchip DRM 通常有，但 SPI 屏一般不接 DRM）
- SPI 屏通常无 DRM 驱动，方案 B 不适用于 SPI 屏

#### 选型决策

```
屏幕是 SPI 屏 (ILI9341/ST7789)?
  ├─ 是 → 方案 A（SPI 屏通常无 DRM 驱动，方案 A 是唯一选择）
  └─ 否 (RGB/MIPI/HDMI 屏)
      └─ 方案 B（DRM/KMS，享受 vsync + 硬件叠加）
```

本项目 RV1126B 平台使用 2.4 寸 SPI 屏（ILI9341），**优先使用方案 A**。

切方案 B 的触发条件:
1. SPI 屏更换为 RGB 并口屏 / MIPI 屏
2. 出现严重画面撕裂且 TE 引脚无法解决
3. 需要多图层硬件叠加（视频流 + UI 分层）

### 5.10 多目标跟踪：C++ 原生 ByteTrack

**设计结论**：

1. **跟踪在检测之后**：ITracker 接收 `vector<Detection>`，输出 `vector<TrackedObject>`。跟踪不修改检测结果，只关联跨帧的检测。

2. **C++ 原生实现**：不依赖 boxmot/Python。ByteTrack 算法核心（Kalman + Hungarian + Matching）约 500 行 C++。规避 Python 依赖链污染风险。

3. **Kalman 滤波在跟踪内部**：ByteTrack 内部已用 Kalman 做状态预测（位置 x/y + 宽高 w/h + 速度 vx/vy），不需要额外的 Kalman 步骤。

4. **运动意图在渲染时计算**：不需要独立的 IIntentRecognizer 组件。TrackingOverlay 从 TrackedObject.vx/vy 直接计算运动箭头方向和长度，从 TrackedObject.trail 绘制轨迹路径。

5. **可见光伪彩用 alpha blending**：与 IR 的完整伪彩不同，可见光画面的伪彩叠加采用半透明（alpha≈0.5），保留原始场景可辨识。实现用 `cv::addWeighted(roi, 1-alpha, pseudocolor_roi, alpha, 0)`。

6. **跟踪可开关**：通过 `TRACKER_ENABLE` 宏和 CMake `TWOCAMERA_ENABLE_TRACKER` 选项控制是否编译和启用跟踪功能。

### 5.11 运动意图可视化

**设计结论**：

| 可视化元素 | 数据来源 | 渲染位置 |
|---|---|---|
| 跟踪 ID 标签 | `TrackedObject.track_id` | 框上方，格式 `#7` |
| 运动箭头 | `TrackedObject.vx, vy` | 框底部，起点为框底部中心，方向=速度单位向量，长度=min(vx²+vy² * scale, max_len) |
| 轨迹路径 | `TrackedObject.trail` | 框附近，半透明折线，最近点最亮，最远点最淡 |
| 速度标签 (可选) | `TrackedObject.vx, vy` | ID 标签旁，格式 `v=5.2` |

**运动箭头颜色编码**：

```
速度 < 0.5 px/帧 → 灰色 (静止/低速)
速度 0.5-2.0 px/帧 → 绿色 (正常速度)
速度 2.0-5.0 px/帧 → 黄色 (较快)
速度 > 5.0 px/帧 → 红色 (高速)
```

## 6. 文件变更清单

### 新增文件

| 文件 | 说明 |
|---|---|
| `twocameraOnMAC/src/detector.h` | IDetector 接口 + Detection 结构体 |
| `twocameraOnMAC/src/detector_onnx.h` | OnnxDetector 声明 |
| `twocameraOnMAC/src/detector_onnx.cpp` | ONNX Runtime 推理实现 |
| `twocameraOnMAC/src/detector_rknn.h` | RknnDetector 声明（阶段三） |
| `twocameraOnMAC/src/detector_rknn.cpp` | RKNN 推理实现（阶段三） |
| `twocameraOnMAC/src/yolo_postprocess.h` | 后处理函数声明 |
| `twocameraOnMAC/src/yolo_postprocess.cpp` | YOLO 解码 + NMS |
| `twocameraOnMAC/src/side_by_side_compositor.h` | 并排合成器声明（IR 主 + 可见光副） |
| `twocameraOnMAC/src/side_by_side_compositor.cpp` | 并排合成实现 + 布局计算 |
| `twocameraOnMAC/src/detection_overlay.h` | 改为 `tracking_overlay.h`：新增跟踪 ID、运动箭头、轨迹路径、可见光半透明伪彩 |
| `twocameraOnMAC/src/detection_overlay.cpp` | 改为 `tracking_overlay.cpp`：实现跟踪标签、箭头、轨迹、alpha blending 伪彩 |
| `twocameraOnMAC/src/tracker.h` | ITracker 接口 + TrackedObject 结构体 + TrackerConfig |
| `twocameraOnMAC/src/tracker_bytetrack.h` | ByteTrack 声明 (C++ 原生实现) |
| `twocameraOnMAC/src/tracker_bytetrack.cpp` | ByteTrack 实现 (Kalman + Hungarian 匹配 + 新目标确认回调) |
| `twocameraOnMAC/src/alert_notifier.h` | 新目标提示音组件声明 (3.4.8: PCM/KIOCSOUND 后端探测) |
| `twocameraOnMAC/src/alert_notifier.cpp` | 提示音播放实现 (独立线程 + 最短间隔限流) |
| `twocameraOnMAC/src/kalman_filter.h` | Kalman 滤波器独立实现 (可复用) |
| `twocameraOnMAC/src/kalman_filter.cpp` | Kalman 滤波器实现 |
| `twocameraOnMAC/src/hungarian.h` | Hungarian 匹配算法实现 (仅头文件, 模板实现) |
| `twocameraOnMAC/src/pseudo_color.h` | 伪彩工具函数声明 (新增 alpha blending 支持) |
| `twocameraOnMAC/src/pseudo_color.cpp` | OpenCV applyColorMap 封装 |
| `twocameraOnMAC/src/pseudo_color_lut.h` | JET 伪彩 LUT 查找表 (RV1126B 纯 C++ 替代) |
| `twocameraOnMAC/src/pseudo_color_lut.cpp` | LUT 预计算 + 查表实现 |
| `twocameraOnMAC/src/frame_pool.h` | BgrFrame 预分配池头文件 (零拷贝传递) |
| `twocameraOnMAC/src/frame_pool.cpp` | BgrFrame 预分配池实现 |
| `twocameraOnMAC/src/async_pipeline.h` | 异步推理管线接口 (NPU Worker 线程) |
| `twocameraOnMAC/src/async_pipeline.cpp` | 异步推理管线实现 (三线程模型) |
| `twocameraOnMAC/src/image_registrator.h` | 图像配准接口声明 |
| `twocameraOnMAC/src/image_registrator.cpp` | 配准实现（OpenCV crop/warp） |
| `twocameraOnMAC/src/image_registrator_rga.h` | RGA 硬件配准声明（RV1126B） |
| `twocameraOnMAC/src/image_registrator_rga.cpp` | RGA 硬件配准实现（RV1126B） |
| `twocameraOnMAC/calibration.json` | 标定参数配置文件 |
| `twocameraOnMAC/src/display_spi_lvgl.h` | SPI 屏 LVGL 显示后端声明（方案 A） |
| `twocameraOnMAC/src/display_spi_lvgl.cpp` | disp_flush_cb + SPI DMA + ILI9341 驱动（方案 A） |
| `twocameraOnMAC/src/display_drm_lvgl.cpp` | DRM/KMS LVGL 显示后端（方案 B，备选） |
| `twocameraOnMAC/src/ili9341.h` | ILI9341 LCD 控制器命令定义 + 初始化序列 |
| `twocameraOnMAC/src/spi_dma.h` | SPI DMA 传输接口声明 |
| `twocameraOnMAC/src/spi_dma.cpp` | SPI DMA 实现（/dev/spidev + ioctl） |
| `twocameraOnMAC/models/best.onnx` | 导出的 ONNX 模型 |
| `twocameraOnMAC/cmake/toolchains/rv1126-toolchain.cmake` | RV1126 交叉编译链（阶段三） |

### 修改文件

| 文件 | 修改内容 |
|---|---|
| `twocameraOnMAC/CMakeLists.txt` | 新增 YOLO 编译选项、ONNX/RKNN 链接、布局模式选项、tracking 源文件 |
| `twocameraOnMAC/src/sample.cpp` | 主循环新增 ImageRegistrator + ByteTrack 步骤；改用 TrackingOverlay；新增 `--side-ratio`、`--calibration`、`--track` 参数 |
| `twocameraOnMAC/src/sample_config.h` | 新增 YOLO 配置宏（模型路径、conf 阈值、伪彩开关等） |

### 不修改的文件

| 文件 | 原因 |
|---|---|
| `twocameraOnMAC/src/frame.h` | 数据结构已满足需求 |
| `twocameraOnMAC/src/synchronizer.h` | 同步逻辑不变 |
| `twocameraOnMAC/src/ir_camera.*` | IR 采集不变 |
| `twocameraOnMAC/src/visible_camera.*` | 可见光采集不变 |
| `twocameraOnMAC/src/frame_compositor.*` | 保留作为 PIP 兼容模式，不修改 |

## 7. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| ONNX 算子在 RV1126B 不全支持 | RKNN 转换失败 | 测试转换，必要时修改模型导出 opset 或用 onnxsim 简化 |
| aarch64 ONNX Runtime 推理速度 | 本机帧率可能 < 10fps | 先测基准性能；可考虑 INT8 量化加速 |
| 红外与可见光分辨率不一致 | 坐标映射偏差 | 检测框画在 640×640 坐标系，按各自区域比例还原 |
| USB 带宽竞争（推理 + 双摄像头） | 丢帧 | 推理放独立线程，异步处理 |
| 模型类别数 nc=80 (COCO) | 实际场景类别可能不同 | 需要基于实际场景数据微调或重新训练 |
| IR 伪彩计算开销 | 每帧多个 ROI 做 applyColorMap | 仅对检测框内 ROI 操作，帧数少时开销可控 |
| 并排布局总分辨率过大 | 显示帧过大影响帧率 | 限制输出帧宽高，如 IR 640×640 + 可见光 427×640 |
| ByteTrack 跟踪 ID 跳变 | 画面抖动/遮挡导致 ID 切换 | 调整 track_thresh 增大确认帧数；Kalman 预测保持丢失帧连续性 |
| ByteTrack 多目标匹配错误 | 重叠目标 ID 分配错误 | 调大 match_thresh；使用额外特征（外观）辅助匹配 |
| 运动箭头方向误判 | 噪声导致速度计算不稳定 | 速度阈值过滤；对 vx/vy 做 EMA 平滑 |
| 可见光半透明伪彩 alpha 不合适 | 伪彩太亮/太暗影响视觉 | 提供配置宏可调 alpha (0.3~0.7) |
| Hungarian 匹配计算量 | 目标数量多时匹配耗时 | 仅对 top-K 检测框匹配（K=30） |
| 同步模式推理阻塞主线程 | RV1126B 上 NPU 推理 15ms+ 时帧间隔超时 | 切换异步模式（AsyncPipeline），主线程用上次结果显示 |
| RV1126B 无 OpenCV | 伪彩/绘图不可用 | 用纯 C++ LUT 伪彩 + 手写绘图基元替代 |
| SPI downscale 质量 | RGA 缩放到 240×320 可能引入锯齿 | RGA 使用双线性/双三次缩放滤波器；必要时回退 OpenCV |
| IR 放大 8 倍伪影 | 检测框精度受限于 256×192 原始分辨率 | INTER_CUBIC 插值；后续考虑重新训练 256×192 输入模型 |
| 异步模式结果延迟 | 主线程显示上一帧结果 | NPU Worker ≤ 40ms 产出，延迟 ≤ 1 帧，25fps 下可接受 |
| BgrFrame 内存碎片 | RV1126B 上频繁 malloc/free 导致内存碎片 | 启动时预分配 FramePool，运行时零 malloc |
| 检测间隔策略下检测框滞后 | 快速移动目标可能漏检 | detection_interval=2 折中；跟踪 Kalman 补偿中间帧 |
| 新目标提示音平台无音频设备 | 无声（功能缺失，非崩溃） | AlertNotifier 启动探测回退 KIOCSOUND/GPIO；全不可用时仅日志提示 |
| 多目标同时进入导致提示音连续鸣叫 | 听觉干扰 | AlertNotifier 最短间隔限流（500ms 内合并为一次） |

## 8. sample_config.h 新增配置宏

```cpp
// YOLO 推理配置
#define YOLO_MODEL_PATH "models/best.onnx"
#define YOLO_INPUT_SIZE 640
#define YOLO_CONF_THRESHOLD 0.25f
#define YOLO_IOU_THRESHOLD 0.45f

// 显示布局配置
#define LAYOUT_SIDE_RATIO 0.6f        // IR 主画面占比
#define LAYOUT_GAP_WIDTH 4             // 分割线宽度

// 跟踪配置 (ByteTrack)
#define TRACKER_ENABLE 1               // 是否启用跟踪
#define TRACKER_THRESH 5                // 确认跟踪所需连续帧数
#define TRACKER_BUFFER 30               // 丢失后保留帧数
#define TRACKER_MATCH_THRESH 0.8f      // 匹配 IoU 阈值
#define TRACKER_LOW_THRESH 0.1f        // 低置信度阈值
#define TRACKER_MAX_AGE 30             // 最大丢失帧数
#define TRACKER_TRAIL_LENGTH 15        // 轨迹历史保留长度
#define DETECTION_INTERVAL 1           // 检测间隔 (1=每帧推理, 2=隔帧推理)

// 运动意图显示配置
#define MOTION_ARROW_ENABLE 1          // 是否画运动箭头
#define MOTION_ARROW_SCALE 2.0f        // 箭头长度系数
#define MOTION_ARROW_MAX_LEN 60.0f     // 箭头最大长度
#define MOTION_SPEED_THRESH 0.5f       // 静止判定阈值
#define MOTION_TRAIL_ENABLE 1          // 是否画轨迹路径

// 新目标提示音配置 (3.4.8: ByteTrack 新目标确认时触发)
#define NEW_TARGET_ALERT_ENABLE 1          // 总开关 (0=不注册回调, 提示音组件不生效)
#define NEW_TARGET_ALERT_FREQ_HZ 880       // 提示音频率
#define NEW_TARGET_ALERT_DURATION_MS 120   // 提示音时长
#define NEW_TARGET_ALERT_MIN_INTERVAL_MS 500 // 防抖: 两次提示音最短间隔

// 检测框绘制配置
#define DETECTION_BOX_THICKNESS 2
#define DETECTION_DRAW_LABELS 1
#define DETECTION_DRAW_CONFIDENCE 1
#define DETECTION_DRAW_TRACK_ID 1      // 画跟踪 ID 标签
#define DETECTION_BOX_COLOR_R 0       // 绿色框
#define DETECTION_BOX_COLOR_G 255
#define DETECTION_BOX_COLOR_B 0

// IR 伪彩配置
#define IR_PSEUDO_COLOR_ENABLE 1
#define IR_PSEUDO_COLOR_MAP 2         // cv::COLORMAP_JET = 2

// 可见光半透明伪彩配置
#define VISIBLE_PSEUDO_COLOR_ENABLE 1
#define VISIBLE_PSEUDO_COLOR_ALPHA 0.5f  // 半透明系数
```

## 9. IDetector 对照测试方案

### 9.1 设计原则

参考 YOLOFuse 项目 `tests/` 目录的组织方式（`__init__.py` 集中定义测试常量、`conftest.py` 管理 fixture、各 `test_*.py` 按功能拆分），设计 Python 基准 + C++ 对照的端到端测试。

**核心思路**：用同一对 LLVIP 测试图，分别走 Python 推理和 C++ 推理，对比检测结果是否一致，从而验证 C++ 预处理/后处理实现的正确性。

### 9.2 测试数据

使用 YOLOFuse 仓库自带的 LLVIP 测试样本：

```
YOLOFuse/ultralytics/assets/LLVIP/
├── images/
│   └── 120270.jpg    (可见光 RGB, 3ch BGR)
└── imagesIR/
    └── 120270.jpg    (红外 IR, 3ch BGR)
```

- LLVIP 数据集仅 1 个类别：`0: person`
- 测试数据仅一对图像，体积小，适合纳入 CI

### 9.3 测试流程

```
┌─────────────────────────────────────────────────────────────┐
│                    测试数据 (共享)                             │
│  YOLOFuse/ultralytics/assets/LLVIP/images/120270.jpg        │
│  YOLOFuse/ultralytics/assets/LLVIP/imagesIR/120270.jpg      │
│  YOLOFuse/models/best.onnx                                   │
└──────────────────┬──────────────────────────────────────────┘
                   │
        ┌──────────┴──────────┐
        ↓                     ↓
┌───────────────┐    ┌───────────────┐
│ Python 基准     │    │ C++ 对照       │
│ (YOLOFuse)     │    │ (twocameraOnMAC)│
│               │    │               │
│ tests/        │    │ tests/        │
│ test_dual_    │    │ test_detector │
│ detector.py   │    │ .cpp          │
│               │    │               │
│ 用 onnxruntime │    │ 用 IDetector   │
│ 直接推理       │    │ (ONNX Runtime) │
│               │    │               │
│ 输出:         │    │ 输出:         │
│ python_       │    │ cpp_          │
│ results.json  │    │ results.json  │
└───────┬───────┘    └───────┬───────┘
        │                     │
        └──────────┬──────────┘
                   ↓
          ┌───────────────┐
          │ 结果对比脚本    │
          │ compare_       │
          │ results.py    │
          │               │
          │ 判据:         │
          │ 1. 检测框数量   │
          │ 2. IoU > 0.95 │
          │ 3. 类别一致    │
          │ 4. 置信度误差   │
          │    < 0.01     │
          └───────────────┘
```

### 9.4 新增文件清单

#### YOLOFuse 侧（Python 基准）

| 文件 | 说明 |
|---|---|
| `YOLOFuse/tests/test_dual_detector.py` | Python ONNX 推理基准，输出 JSON |
| `YOLOFuse/tests/test_dual_detector_data/` | 测试输出目录（JSON、对比图） |

#### twocameraOnMAC 侧（C++ 对照）

| 文件 | 说明 |
|---|---|
| `twocameraOnMAC/tests/test_detector.cpp` | C++ IDetector 推理测试程序 |
| `twocameraOnMAC/tests/CMakeLists.txt` | 测试目标构建配置 |

#### 对比脚本

| 文件 | 说明 |
|---|---|
| `YOLOFuse/tests/compare_results.py` | Python vs C++ 结果对比 |

### 9.5 Python 基准测试设计

参考 YOLOFuse `tests/__init__.py` 的常量集中管理模式和 `test_exports.py` 的导出验证模式：

```python
# YOLOFuse/tests/test_dual_detector.py
"""
IDetector 对照测试 — Python 基准
用 onnxruntime 直接加载 best.onnx，对 LLVIP 测试图推理，
输出检测结果到 JSON，供 C++ 侧对比。

参考 test_exports.py::test_export_onnx 的模式：
  1. 导出/加载 ONNX 模型
  2. 用测试数据推理
  3. 验证输出
"""
import json
import os
import cv2
import numpy as np
import onnxruntime as ort
from pathlib import Path
from tests import ASSETS  # 复用 YOLOFuse tests/__init__.py 的常量

# 测试常量 (参考 tests/__init__.py 集中定义模式)
LLVIP_DIR = ASSETS / "LLVIP"
RGB_PATH = LLVIP_DIR / "images" / "120270.jpg"
IR_PATH = LLVIP_DIR / "imagesIR" / "120270.jpg"
ONNX_PATH = Path("models/best.onnx")
OUTPUT_DIR = Path("tests/test_dual_detector_data")
CONF_THRESHOLD = 0.25
IOU_THRESHOLD = 0.45
INPUT_SIZE = 640


def preprocess(rgb_path, ir_path):
    """与 C++ 预处理完全一致的流程，用于对照"""
    rgb = cv2.imread(str(rgb_path))
    ir = cv2.imread(str(ir_path))
    # resize 到 640×640
    rgb_resized = cv2.resize(rgb, (INPUT_SIZE, INPUT_SIZE))
    ir_resized = cv2.resize(ir, (INPUT_SIZE, INPUT_SIZE))
    # BGR → RGB
    rgb_rgb = cv2.cvtColor(rgb_resized, cv2.COLOR_BGR2RGB)
    ir_rgb = cv2.cvtColor(ir_resized, cv2.COLOR_BGR2RGB)
    # 归一化 [0, 1]
    rgb_f = rgb_rgb.astype(np.float32) / 255.0
    ir_f = ir_rgb.astype(np.float32) / 255.0
    # HWC → CHW
    rgb_chw = rgb_f.transpose(2, 0, 1)
    ir_chw = ir_f.transpose(2, 0, 1)
    # 通道拼接: IR 在前, RGB 在后 (与 loaders.py:431 一致)
    merged = np.concatenate([ir_chw, rgb_chw], axis=0)
    # 组装 batch
    return merged[np.newaxis, ...]  # [1, 6, 640, 640]


def postprocess(output, conf_thres, iou_thres):
    """YOLO 输出解码 + NMS, 返回 Detection 列表"""
    # output shape: [1, 84, 8400] → [8400, 84]
    detections = output[0].T  # [8400, 84]
    # 过滤低置信度
    scores = detections[:, 4:].max(axis=1)
    mask = scores > conf_thres
    filtered = detections[mask]
    if len(filtered) == 0:
        return []
    # NMS
    boxes = filtered[:, :4].tolist()  # xywh
    scores = filtered[:, 4:].max(axis=1)
    class_ids = filtered[:, 4:].argmax(axis=1)
    indices = cv2.dnn.NMSBoxes(boxes, scores, conf_thres, iou_thres)
    results = []
    for i in indices:
        x, y, w, h = boxes[i]
        results.append({
            "class_id": int(class_ids[i]),
            "confidence": float(scores[i]),
            "x": float(x), "y": float(y),
            "w": float(w), "h": float(h),
        })
    return results


def test_dual_detector_onnx():
    """Python ONNX 推理基准 — 输出 JSON 供 C++ 对照"""
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    sess = ort.InferenceSession(str(ONNX_PATH))
    input_tensor = preprocess(RGB_PATH, IR_PATH)
    output = sess.run(["output0"], {"images": input_tensor})[0]

    results = postprocess(output, CONF_THRESHOLD, IOU_THRESHOLD)

    out_json = OUTPUT_DIR / "python_results.json"
    with open(out_json, "w") as f:
        json.dump({
            "model": str(ONNX_PATH),
            "rgb": str(RGB_PATH),
            "ir": str(IR_PATH),
            "input_size": INPUT_SIZE,
            "num_detections": len(results),
            "detections": results,
        }, f, indent=2)

    # 基本断言: 应至少检测到 1 个 person
    assert len(results) >= 1, "Expected at least 1 detection"
    assert all(d["class_id"] == 0 for d in results), "LLVIP only has class 0: person"
    print(f"Python baseline: {len(results)} detections → {out_json}")
```

### 9.6 C++ 对照测试设计

参考 YOLOFuse `test_exports.py` 的简洁模式（导出→推理→验证）：

```cpp
// twocameraOnMAC/tests/test_detector.cpp
/*
 * IDetector 对照测试 — C++ 侧
 * 用与 Python 基准完全相同的 LLVIP 测试图，
 * 验证 C++ 预处理 + ONNX 推理 + 后处理的结果一致性。
 *
 * 编译: 见 tests/CMakeLists.txt
 * 运行: ./test_detector <onnx_path> <llvip_dir>
 * 输出: cpp_results.json
 */
#include "detector.h"        // IDetector + Detection
#include "yolo_postprocess.h"

#include <opencv2/opencv.hpp>
#include <cstdio>
#include <vector>

// 将 cv::Mat 转为 BgrFrame (与 sample.cpp 中摄像头帧格式一致)
static BgrFrame mat_to_bgrframe(const cv::Mat& mat) {
    BgrFrame frame;
    frame.width = mat.cols;
    frame.height = mat.rows;
    frame.stride = mat.step;
    frame.data.assign(mat.datastart, mat.dataend);
    return frame;
}

int main(int argc, char* argv[]) {
    const char* onnx_path = argc > 1 ? argv[1] : "models/best.onnx";
    const char* llvip_dir = argc > 2 ? argv[2] :
        "../YOLOFuse/ultralytics/assets/LLVIP";

    // 加载模型
    std::unique_ptr<IDetector> detector = create_onnx_detector();
    if (!detector->load(onnx_path)) {
        fprintf(stderr, "Failed to load ONNX: %s\n", onnx_path);
        return 1;
    }
    printf("Model loaded: %s, input_size=%d\n", onnx_path, detector->model_size());

    // 读取测试图 (与 Python 基准完全相同)
    std::string rgb_path = std::string(llvip_dir) + "/images/120270.jpg";
    std::string ir_path  = std::string(llvip_dir) + "/imagesIR/120270.jpg";
    cv::Mat rgb = cv::imread(rgb_path);
    cv::Mat ir  = cv::imread(ir_path);
    if (rgb.empty() || ir.empty()) {
        fprintf(stderr, "Failed to read test images\n");
        return 1;
    }

    BgrFrame vis_frame = mat_to_bgrframe(rgb);
    BgrFrame ir_frame  = mat_to_bgrframe(ir);

    // 推理
    std::vector<Detection> detections;
    detector->detect(vis_frame, ir_frame, detections);

    // 输出 JSON (与 python_results.json 格式一致)
    printf("{\n");
    printf("  \"num_detections\": %zu,\n", detections.size());
    printf("  \"detections\": [\n");
    for (size_t i = 0; i < detections.size(); ++i) {
        const auto& d = detections[i];
        printf("    {\"class_id\": %d, \"confidence\": %.6f, "
               "\"x\": %.6f, \"y\": %.6f, \"w\": %.6f, \"h\": %.6f}%s\n",
               d.class_id, d.confidence, d.x, d.y, d.w, d.h,
               i + 1 < detections.size() ? "," : "");
    }
    printf("  ]\n}\n");

    // 基本断言
    if (detections.empty()) {
        fprintf(stderr, "FAIL: expected at least 1 detection\n");
        return 1;
    }
    for (const auto& d : detections) {
        if (d.class_id != 0) {
            fprintf(stderr, "FAIL: expected class 0 (person), got %d\n", d.class_id);
            return 1;
        }
    }
    printf("OK: %zu detections, all class 0 (person)\n", detections.size());
    return 0;
}
```

```cmake
# twocameraOnMAC/tests/CMakeLists.txt
# 仅在本机编译，不参与交叉编译 (RV1126B 不需要测试程序)
if(NOT CMAKE_CROSSCOMPILING AND TWOCAMERA_ENABLE_YOLO)
    add_executable(test_detector test_detector.cpp)
    target_link_libraries(test_detector PRIVATE
        twocamera           # 链接主库的 detector/yolo_postprocess
        ${OpenCV_LIBS}
        ${ONNXRUNTIME_LIB}
    )
    target_include_directories(test_detector PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        ${OpenCV_INCLUDE_DIRS}
        ${ONNXRUNTIME_INCLUDE_DIR}
    )
endif()
```

### 9.7 结果对比脚本

```python
# YOLOFuse/tests/compare_results.py
"""
对比 Python 基准与 C++ 对照的检测结果。
判据:
  1. 检测框数量一致
  2. 逐框 IoU > 0.95 (允许亚像素级误差)
  3. 类别一致
  4. 置信度误差 < 0.01

用法:
  python tests/compare_results.py \
    --python tests/test_dual_detector_data/python_results.json \
    --cpp   tests/test_dual_detector_data/cpp_results.json
"""
import json
import argparse
from pathlib import Path


def iou(box_a, box_b):
    """计算 xywh 格式的 IoU"""
    ax1, ay1 = box_a["x"] - box_a["w"]/2, box_a["y"] - box_a["h"]/2
    ax2, ay2 = box_a["x"] + box_a["w"]/2, box_a["y"] + box_a["h"]/2
    bx1, by1 = box_b["x"] - box_b["w"]/2, box_b["y"] - box_b["h"]/2
    bx2, by2 = box_b["x"] + box_b["w"]/2, box_b["y"] + box_b["h"]/2
    inter = max(0, min(ax2, bx2) - max(ax1, bx1)) * \
            max(0, min(ay2, by2) - max(ay1, by1))
    area_a = box_a["w"] * box_a["h"]
    area_b = box_b["w"] * box_b["h"]
    return inter / (area_a + area_b - inter + 1e-9)


def match_detections(python_dets, cpp_dets):
    """贪心匹配: 按 IoU 最大配对"""
    matched = []
    used_cpp = [False] * len(cpp_dets)
    for pd in python_dets:
        best_iou, best_j = 0, -1
        for j, cd in enumerate(cpp_dets):
            if used_cpp[j]:
                continue
            v = iou(pd, cd)
            if v > best_iou:
                best_iou, best_j = v, j
        if best_j >= 0 and best_iou > 0.5:
            matched.append((pd, cpp_dets[best_j], best_iou))
            used_cpp[best_j] = True
    return matched


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--python", required=True)
    ap.add_argument("--cpp", required=True)
    args = ap.parse_args()

    with open(args.python) as f:
        py = json.load(f)
    with open(args.cpp) as f:
        cpp = json.load(f)

    py_dets = py["detections"]
    cpp_dets = cpp["detections"]

    print(f"Python: {len(py_dets)} detections")
    print(f"C++:    {len(cpp_dets)} detections")

    if len(py_dets) != len(cpp_dets):
        print(f"FAIL: detection count mismatch ({len(py_dets)} vs {len(cpp_dets)})")
        return 1

    matched = match_detections(py_dets, cpp_dets)
    if len(matched) != len(py_dets):
        print(f"FAIL: only {len(matched)}/{len(py_dets)} detections matched")
        return 1

    all_ok = True
    for i, (pd, cd, iou_val) in enumerate(matched):
        cls_ok = pd["class_id"] == cd["class_id"]
        iou_ok = iou_val > 0.95
        conf_diff = abs(pd["confidence"] - cd["confidence"])
        conf_ok = conf_diff < 0.01
        status = "OK" if (cls_ok and iou_ok and conf_ok) else "FAIL"
        print(f"  [{i}] {status}: cls={pd['class_id']}=={cd['class_id']} "
              f"IoU={iou_val:.4f} conf_diff={conf_diff:.6f}")
        if not (cls_ok and iou_ok and conf_ok):
            all_ok = False

    print(f"\n{'ALL PASS' if all_ok else 'SOME FAILED'}")
    return 0 if all_ok else 1


if __name__ == "__main__":
    exit(main())
```

### 9.8 测试运行流程

```bash
# 1. Python 基准 (在 YOLOFuse 目录)
cd /mnt/hgfs/share/code/YOLOFuse
.venv/bin/python -m pytest tests/test_dual_detector.py -v
# 产出: tests/test_dual_detector_data/python_results.json

# 2. C++ 对照 (在 twocameraOnMAC 目录)
cd /mnt/hgfs/share/code/twocameraOnMAC
cmake --build build/native --target test_detector
./build/native/test_detector models/best.onnx \
    ../YOLOFuse/ultralytics/assets/LLVIP \
    > tests/test_dual_detector_data/cpp_results.json

# 3. 结果对比 (在 YOLOFuse 目录)
cd /mnt/hgfs/share/code/YOLOFuse
.venv/bin/python tests/compare_results.py \
    --python tests/test_dual_detector_data/python_results.json \
    --cpp   tests/test_dual_detector_data/cpp_results.json
# 预期: ALL PASS
```

### 9.9 对照测试的验证范围

| 验证项 | Python 基准覆盖 | C++ 对照覆盖 | 说明 |
|---|---|---|---|
| ONNX 模型加载 | ✅ | ✅ | 两端都能加载同一 onnx |
| 预处理 resize | ✅ | ✅ | 640×640 缩放一致 |
| 预处理 BGR→RGB | ✅ | ✅ | 颜色空间转换一致 |
| 预处理 归一化 | ✅ | ✅ | /255.0 一致 |
| 预处理 通道拼接 | ✅ | ✅ | IR 前 3ch + RGB 后 3ch 顺序一致 |
| 推理结果 | ✅ | ✅ | 相同输入 → 相同输出 |
| 后处理 NMS | ✅ | ✅ | 过滤/抑制逻辑一致 |
| 坐标格式 | ✅ | ✅ | xywh 640×640 坐标系一致 |
| 坐标映射到显示 | ❌ | ✅ | 仅 C++ 有显示，在主循环集成时验证 |

> 最后一项"坐标映射到显示"不在对照测试范围内，因为它依赖 SideBySideLayout 和实际显示区域，需在集成联调阶段验证。

## 10. 实施优先级

1. **第一步**：导出 ONNX 模型 + Python 验证推理正确性
2. **第二步**：C++ 实现 IDetector + OnnxDetector + 后处理
3. **第三步**：**IDetector 对照测试** — Python 基准 vs C++ 结果对比通过
4. **第四步**：实现 SideBySideCompositor（并排合成）+ DetectionOverlay（画框 + 伪彩）
5. **第五步**：集成到 sample.cpp 主循环，本机联调
6. **第六步**：性能优化（异步推理、量化）
7. **第七步**：RV1126B 交叉编译 + RKNN 转换 + 部署
