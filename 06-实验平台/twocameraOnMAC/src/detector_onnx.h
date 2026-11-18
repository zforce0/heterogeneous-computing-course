#pragma once

#include "detector.h"

#include <memory>
#include <string>
#include <vector>

struct YoloPostProcessConfig {
    float conf_threshold = 0.25f;
    float iou_threshold = 0.45f;
    int model_size = 640;
    int num_classes = 1;
};

class OnnxDetector : public IDetector {
public:
    OnnxDetector();
    ~OnnxDetector() override;

    bool load(const std::string& model_path) override;
    bool detect(const BgrFrame& visible,
                const BgrFrame& infrared,
                std::vector<Detection>& results) override;
    int model_size() const override { return input_size_; }

    void set_conf_threshold(float v) { config_.conf_threshold = v; }
    void set_iou_threshold(float v) { config_.iou_threshold = v; }

private:
    struct Impl;
    Impl* impl_;
    int input_size_;
    YoloPostProcessConfig config_;
};