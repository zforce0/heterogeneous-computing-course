#pragma once

#include "detector.h"

#include <memory>
#include <string>
#include <vector>

/* 可见光 YOLOv8n 人像检测器 (暂时替换 YOLOFuse, 原方案代码保留):
 *
 * 模型: models/yolov8n/yolov8n_person.onnx
 *   - YOLOv8n 在 CrowdHuman 数据集训练 (可见光, 全身 + 半身人像标注)
 *   - 输入 [1,3,640,640], 输出 [1,5,8400] (nc=1: person)
 *
 * 使用算法自身的预处理/后处理 (与 detector_onnx.cpp 的 YOLOFuse
 * 6 通道直拉缩放方案相互独立):
 *   预处理: ultralytics LetterBox — 等比缩放 + 114 灰边填充,
 *           BGR→RGB, /255, HWC→CHW
 *   后处理: YOLOv8 [1,nc+4,8400] 解码 + 类别分数过滤 + NMS,
 *           反 LetterBox 还原到源帧像素坐标
 *
 * 坐标约定 (与 OnnxDetector/TrackingOverlay 一致):
 *   检测框最终换算到 model_size×model_size 正方形空间
 *   (x,y 为左上角, w,h 为宽高), 供并排子画面按比例映射。
 *
 * detect() 只使用 visible 帧 (infrared 参数忽略, 保持 IDetector 接口)。 */
class Yolov8OnnxDetector : public IDetector {
public:
    Yolov8OnnxDetector();
    ~Yolov8OnnxDetector() override;

    bool load(const std::string& model_path) override;
    bool detect(const BgrFrame& visible,
                const BgrFrame& infrared,
                std::vector<Detection>& results) override;
    int model_size() const override { return input_size_; }

    void set_conf_threshold(float v) { conf_threshold_ = v; }
    void set_iou_threshold(float v) { iou_threshold_ = v; }

private:
    struct Impl;
    Impl* impl_;
    int input_size_;
    float conf_threshold_;
    float iou_threshold_;
};
