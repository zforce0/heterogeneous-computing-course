#pragma once

#include "frame.h"
#include "synchronizer.h"

/* 配准配置 (camerayolo.md 3.2):
 *
 * 模式 A (默认, use_quad=false): 轴对齐 crop+resize
 *   crop_width/height 为 0 表示取 visible 全幅 (仅缩放)。
 *
 * 模式 B (use_quad=true): 四边形透视截取
 *   quad 为红外视野在可见光画面中的四边形顶点 (8 值:
 *   q0x,q0y, q1x,q1y, q2x,q2y, q3x,q3y, 顺序左上/右上/右下/左下),
 *   坐标基于 640x480 可见光帧 (其他分辨率自动等比换算)。
 *   输出 = 四边形内容透视变换到 IR 分辨率; 越界区域为黑色。
 *   参数由 test/camera_capture_test/calib_web.py 选点/拖点标定,
 *   经 calibration.txt 加载。 */
struct RegistrationConfig {
    int crop_x = 0;
    int crop_y = 0;
    int crop_width = 0;
    int crop_height = 0;
    float quad[8] = {0.0f, 0.0f, 640.0f, 0.0f,
                     640.0f, 480.0f, 0.0f, 480.0f};
    bool use_quad = false;
};

struct RegisteredFrames {
    BgrFrame infrared;   /* IR 帧 (无需配准, 直接传递) */
    BgrFrame visible;    /* 可见光帧 (已配准到 IR 视角/分辨率) */
    int64_t timestamp_delta_us = 0;
    bool valid = false;
};

/* 图像配准: 独立管线步骤, 位于 Synchronizer 之后、IDetector 预处理之前。
 * 输出供检测 (IDetector) 与显示 (SideBySideCompositor) 共用。 */
class ImageRegistrator {
public:
    explicit ImageRegistrator(
        const RegistrationConfig& config = RegistrationConfig());

    bool register_frames(const SyncedFrames& input,
                         RegisteredFrames& output);

private:
    RegistrationConfig config_;
    /* 四边形模式的单应: 归一化输出坐标 (0..1)² → quad (640x480) */
    float quad_h_[9];
    bool quad_h_valid_;
    BgrFrame registered_visible_;  /* 复用缓冲, 避免每帧分配 */
};
