#pragma once

#include "frame.h"

#include <cstdint>
#include <string>
#include <vector>

struct Detection {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    float confidence = 0.0f;
    int class_id = -1;
};

class IDetector {
public:
    virtual ~IDetector() {}
    virtual bool load(const std::string& model_path) = 0;
    virtual bool detect(const BgrFrame& visible,
                        const BgrFrame& infrared,
                        std::vector<Detection>& results) = 0;
    virtual int model_size() const = 0;
};