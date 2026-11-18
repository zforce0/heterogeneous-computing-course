#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct BgrFrame {
    int width = 0;
    int height = 0;
    int stride = 0;
    uint64_t timestamp_us = 0;
    uint64_t sequence = 0;
    std::vector<uint8_t> data;

    bool valid() const
    {
        return width > 0 && height > 0 && stride >= width * 3 &&
               data.size() >= static_cast<size_t>(stride) * height;
    }

    void resize(int new_width, int new_height)
    {
        width = new_width;
        height = new_height;
        stride = new_width * 3;
        data.resize(static_cast<size_t>(stride) * height);
    }
};

struct TemperatureFrame {
    int width = 0;
    int height = 0;
    uint64_t timestamp_us = 0;
    uint64_t sequence = 0;
    std::vector<uint16_t> data;

    bool valid() const
    {
        return width > 0 && height > 0 &&
               data.size() >=
                   static_cast<size_t>(width) * height;
    }

    void resize(int new_width, int new_height)
    {
        width = new_width;
        height = new_height;
        data.resize(static_cast<size_t>(width) * height);
    }
};

class IFrameSource {
public:
    virtual ~IFrameSource() {}
    virtual bool copy_latest(BgrFrame& destination) = 0;
};

class ITemperatureSource {
public:
    virtual ~ITemperatureSource() {}
    virtual bool copy_latest_temperature(
        TemperatureFrame& destination) = 0;
};
