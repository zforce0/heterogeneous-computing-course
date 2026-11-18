# twocameraOnMAC

双摄像头（红外 + 可见光）实时采集 + YOLOFuse 双模态目标检测，本机（aarch64 Ubuntu）OpenCV 显示。

整体方案见 [camerayolo.md](camerayolo.md)。

## 管线

```
IrCamera (USB SDK, 256×192)  ─┐
                               ├→ Synchronizer → ImageRegistrator → OnnxDetector (YOLOFuse)
VisibleCamera (V4L2, 640×480) ─┘      (时间同步)     (crop+resize 配准)   (640×640, ONNX Runtime)
                                                                  ↓
                                    SideBySideCompositor → DetectionOverlay → OpenCV 显示
                                    (IR 左 60% + 可见光右 40%)   (检测框 + IR 框内伪彩)
```

## 依赖

| 依赖 | 说明 |
|---|---|
| libir SDK (aarch64) | 红外相机，默认取 `/mnt/hgfs/share/code/linux_ac010/aarch64-linux-gnu_libs`（`libs/native` 为 x86-64 版，供 Mac 主机使用） |
| ONNX Runtime (aarch64) | YOLO 推理，默认 `/tmp/onnxruntime-linux-aarch64-1.21.0` |
| OpenCV | `core highgui` 组件（imshow 显示） |
| libjpeg | 可见光 MJPG 解码，系统安装即可 |

## 本地编译

方式一（推荐，Makefile 已带上 SDK / ONNX Runtime 路径）：

```bash
make native                        # 默认启用 YOLO 检测
make native YOLO_CMAKE_ARGS=       # 纯采集预览（无检测，不需要 ONNX Runtime）
```

方式二（直接 cmake）：

```bash
cmake -S . -B build/native \
    -DTWOCAMERA_SDK_LIB_DIR=/mnt/hgfs/share/code/linux_ac010/aarch64-linux-gnu_libs \
    -DTWOCAMERA_ENABLE_YOLO=ON \
    -DTWOCAMERA_ONNXRUNTIME_DIR=/tmp/onnxruntime-linux-aarch64-1.21.0
cmake --build build/native
```

产物：`build/native/sample`（SDK so 自动拷贝到 `build/native/libs/`，rpath 已配置，无需 `LD_LIBRARY_PATH`）。

## 启动

一键编译 + 启动：

```bash
./build/run.sh                     # 编译(增量) + 启动
NO_YOLO=1 ./build/run.sh           # 纯采集模式
./build/run.sh --visible /dev/video1
```

直接运行（需在项目根目录，模型为相对路径 `models/best.onnx`）：

```bash
make run                           # 等价于 make native && ./build/native/sample
./build/native/sample
```

退出：Ctrl+C、关闭 OpenCV 窗口、按 `q` / `Esc`。

### 常用参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--visible PATH` | 自动探测 1d6c:0103 | 可见光相机节点 |
| `--width/--height/--fps` | 640/480/30 | 可见光采集参数（MJPG） |
| `--side-ratio F` | 0.6 | 并排画面中 IR 主画面宽度占比 |
| `--temp-threshold C` | 30 | 红外温度显著性叠加阈值（摄氏度） |
| `--model PATH` | models/best.onnx | YOLOFuse ONNX 模型 |

### USB 权限

红外相机走 libusb，普通用户需安装 udev 规则并重新插拔：

```bash
sudo cp udev/70-twocamera.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

## 目录结构

```
src/        管线源码（采集/同步/配准/检测/合成/叠加/显示）
include/    libir SDK 头文件
models/     best.onnx (YOLOFuse 导出模型)
test/       detector_test（检测对照测试）、ir_temperature_test（测温验证）
udev/       USB 权限规则
camerayolo.md   集成方案与架构设计文档
```

## 测试

```bash
cd test/detector_test && ./build.sh && ./build/detector_test
# 用 LLVIP 测试图离线验证 IDetector 推理 + 画框
```
