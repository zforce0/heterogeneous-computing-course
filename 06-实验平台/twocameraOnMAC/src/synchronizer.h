#pragma once

#include "frame.h"

#include <cstdint>

struct SyncedFrames {
    BgrFrame infrared;
    BgrFrame visible;
    int64_t timestamp_delta_us = 0;
};

class LatestFrameSynchronizer {
public:
    LatestFrameSynchronizer();

    void set_sources(IFrameSource* infrared, IFrameSource* visible);
    bool acquire(SyncedFrames& frames);

private:
    IFrameSource* infrared_;
    IFrameSource* visible_;
};
