#pragma once

#include "frame.h"

/* 双线性缩放 BgrFrame 的源 ROI (source_x/y/w/h) 并写入目标帧的指定
 * 矩形区域 (destination_x/y/w/h)。源和目标可以是不同尺寸的帧。
 * 用于图像配准 (visible → IR 分辨率) 和并排合成 (子画面缩放)。 */
void resize_bilinear_bgr(const BgrFrame& source,
                         int source_x, int source_y,
                         int source_width, int source_height,
                         BgrFrame& destination,
                         int destination_x, int destination_y,
                         int destination_width,
                         int destination_height);
