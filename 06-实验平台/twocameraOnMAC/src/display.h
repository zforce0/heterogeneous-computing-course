#pragma once

#include "frame.h"

#include <memory>
#include <string>

struct DisplayConfig {
    std::string device = "/dev/fb0";
    std::string window_title = "twocamera";
    int preferred_width = 1920;
    int preferred_height = 1080;
};

class IDisplay {
public:
    virtual ~IDisplay() {}
    virtual bool open(const DisplayConfig& config) = 0;
    virtual bool present(const BgrFrame& frame) = 0;
    virtual void close() = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
};

std::unique_ptr<IDisplay> create_display();
