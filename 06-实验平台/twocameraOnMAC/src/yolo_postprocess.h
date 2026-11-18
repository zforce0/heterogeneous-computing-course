#pragma once

#include "detector.h"

#include <vector>

struct YoloPostProcessConfig;

std::vector<Detection> yolo_postprocess(const float* output,
                                        int batch,
                                        int nc_plus_4,
                                        int num_anchors,
                                        float conf_threshold,
                                        float iou_threshold);

void draw_detections(const BgrFrame& frame,
                     const std::vector<Detection>& detections,
                     int box_thickness = 2);

/* 将 model_size 坐标系的检测框按比例映射到 BgrFrame 的子区域后绘制。
 * 用于把 640×640 坐标系的检测结果映射到合成帧的 visible 主画面 /
 * IR pip 子画面 (camerayolo.md 5.7 检测框坐标映射)。纯像素绘制,
 * 不依赖 OpenCV, 任意显示后端可用。 */
void draw_detections_in_region(const BgrFrame& frame,
                               const std::vector<Detection>& detections,
                               int model_size,
                               int region_x, int region_y,
                               int region_width, int region_height,
                               int box_thickness = 2);