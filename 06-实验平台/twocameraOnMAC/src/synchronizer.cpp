#include "synchronizer.h"

LatestFrameSynchronizer::LatestFrameSynchronizer()
    : infrared_(NULL), visible_(NULL)
{
}

void LatestFrameSynchronizer::set_sources(IFrameSource* infrared,
                                          IFrameSource* visible)
{
    infrared_ = infrared;
    visible_ = visible;
}

bool LatestFrameSynchronizer::acquire(SyncedFrames& frames)
{
    if (infrared_ == NULL || visible_ == NULL) {
        return false;
    }
    if (!infrared_->copy_latest(frames.infrared) ||
        !visible_->copy_latest(frames.visible)) {
        return false;
    }

    frames.timestamp_delta_us =
        static_cast<int64_t>(frames.visible.timestamp_us) -
        static_cast<int64_t>(frames.infrared.timestamp_us);
    return true;
}
