#pragma once

// 红外输出模式：与旧项目一致，三者只能启用一个。
#define IMAGE_AND_TEMP_OUTPUT
// #define IMAGE_OUTPUT
// #define TEMP_OUTPUT

#if (defined(IMAGE_AND_TEMP_OUTPUT) + defined(IMAGE_OUTPUT) + \
     defined(TEMP_OUTPUT)) != 1
#error "Exactly one infrared output mode must be enabled"
#endif

// 运行时命令线程默认启用，命令编号与旧项目一致。
#define COMMAND_ENABLE

// 以下图像及相机保护算法在旧项目中默认关闭。
// #define IR_IMAGE_ENHANCE
// #define IR_PSEUDO_COLOR
// #define AUTO_GAIN_SWITCH
// #define AVOID_OVEREXPOSURE

// 测温演示默认关闭；启用后每隔指定帧数输出一次结果。
// #define POINT_TEMP_DEMO
// #define LINE_TEMP_DEMO
// #define RECT_TEMP_DEMO
#define TEMPERATURE_PRINT_INTERVAL 25

// 帧率叠加显示: 在合成画面上绘制 visible/IR 两路 FPS
// 仅 OPENCV 显示后端生效(其它后端条件编译跳过)
#define FPS_OVERLAY_ENABLE               1
#define FPS_OVERLAY_EMA_ALPHA            0.1f   // EMA 平滑系数, 越小越稳
#define FPS_OVERLAY_FONT_SCALE           0.6f   // 字号缩放
#define FPS_OVERLAY_FONT_THICKNESS       2      // 字线粗细
#define FPS_OVERLAY_MARGIN               8      // 文字距图像边缘像素
#define FPS_OVERLAY_COLOR_VISIBLE_B      255    // 可见光 FPS 文字颜色(BGR 黄)
#define FPS_OVERLAY_COLOR_VISIBLE_G      255
#define FPS_OVERLAY_COLOR_VISIBLE_R      0
#define FPS_OVERLAY_COLOR_IR_B           255    // 红外 FPS 文字颜色(BGR 黄, 与 IR 灰度图对比明显)
#define FPS_OVERLAY_COLOR_IR_G           255
#define FPS_OVERLAY_COLOR_IR_R           0

// ---------------------------------------------------------------------------
// YOLO 推理配置 (camerayolo.md 第 8 节)
// ---------------------------------------------------------------------------

// 检测后端选择 (暂时切换, YOLOFuse 代码全部保留):
//   1 = YOLOv8n 可见光人像 (CrowdHuman 训练, 半身~全身, 3ch letterbox)
//   0 = YOLOFuse 双模态融合 (IR+RGB 6ch, 原方案)
#define DETECTOR_BACKEND_YOLOV8N 1

#if DETECTOR_BACKEND_YOLOV8N
#define YOLO_MODEL_PATH "models/yolov8n/yolov8n_person.onnx"
#else
#define YOLO_MODEL_PATH "models/best.onnx"
#endif

#define YOLO_INPUT_SIZE 640
#define YOLO_CONF_THRESHOLD 0.25f
#define YOLO_IOU_THRESHOLD 0.45f
#define DETECTION_INTERVAL 1           // 检测间隔 (1=每帧推理, 2=隔帧推理)

// 图像配准 crop 兜底窗口 (仅当 calibration.txt 不存在时使用):
// 将可见光帧的 [X,Y,W,H] 区域缩放到 IR 分辨率。
// 正式配准请用 calib_web.py 标定四边形并保存 calibration.txt
// (主程序启动时自动加载, 优先级高于本宏)
#define REGISTRATION_CROP_X 55
#define REGISTRATION_CROP_Y 26
#define REGISTRATION_CROP_W 585
#define REGISTRATION_CROP_H 439

// 显示布局配置 (并排合成: 双画面统一高度 1:1 等大, 各保持 4:3 宽高比)
#define LAYOUT_GAP_WIDTH 4             // 中间分割线宽度

// 检测框绘制配置
#define DETECTION_BOX_THICKNESS 2
#define DETECTION_BOX_COLOR_R 0        // 绿色框 (BGR)
#define DETECTION_BOX_COLOR_G 255
#define DETECTION_BOX_COLOR_B 0

// IR 子画面框内伪彩 (对红外自身完整覆盖, 热力图效果)
#define IR_PSEUDO_COLOR_ENABLE 1

// 可见光子画面框内伪彩: 显示的是红外帧的热力伪彩,
// 按此不透明度叠加在可见光画面上 (0.5 = 50%)
#define VISIBLE_PSEUDO_COLOR_ENABLE 1
#define VISIBLE_PSEUDO_COLOR_ALPHA 0.5f

// 跟踪配置 (ByteTrack, camerayolo.md 3.4)
#define TRACKER_THRESH 5                // 确认跟踪所需连续帧数
#define TRACKER_BUFFER 30               // 丢失后保留帧数
#define TRACKER_MATCH_THRESH 0.8f       // 第一轮匹配 IoU 阈值
#define TRACKER_LOW_THRESH 0.1f         // 低置信度阈值
#define TRACKER_TRAIL_LENGTH 15         // 轨迹历史保留长度

// 新目标提示音 (camerayolo.md 3.4.8): 新目标稳定存在超过
// NEW_TARGET_STABLE_MS 时通过系统播一声短提示音
#define NEW_TARGET_ALERT_ENABLE 1           // 总开关 (0=不注册回调)
#define NEW_TARGET_STABLE_MS 1000           // 稳定存在时长阈值(可配置)
#define NEW_TARGET_ALERT_FREQ_HZ 880        // 提示音频率
#define NEW_TARGET_ALERT_DURATION_MS 120    // 提示音时长
#define NEW_TARGET_ALERT_MIN_INTERVAL_MS 500 // 防抖: 两次提示音最短间隔

// 运动意图显示配置 (camerayolo.md 3.4.4 / 5.11)
#define DETECTION_DRAW_TRACK_ID 1       // 画跟踪 ID 标签 (#N)
#define MOTION_ARROW_ENABLE 1           // 是否画运动方向箭头
#define MOTION_ARROW_SCALE 2.0f         // 箭头长度系数
#define MOTION_ARROW_MAX_LEN 60.0f      // 箭头最大长度
#define MOTION_SPEED_THRESH 0.5f        // 静止判定阈值
#define MOTION_TRAIL_ENABLE 1           // 是否画轨迹路径

#define IR_SAMPLE_VERSION "twocamera libirsample compatible 1.0.0"
#define STREAM_TIME_SECONDS 600
#define FIRMWARE_FILE_PATH "RS001_FW_new.bin"
#define HIGH_GAIN_TAU_FILE_PATH "tau_H.bin"
#define LOW_GAIN_TAU_FILE_PATH "tau_L.bin"
#define NEW_HIGH_GAIN_TAU_FILE_PATH "new_tau_H.bin"
