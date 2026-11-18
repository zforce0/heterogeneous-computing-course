#pragma once

#include <ctime>

// 统一计时时钟:CLOCK_MONOTONIC_RAW(单调、不受 NTP/调频影响)
// 返回毫秒精度的浮点时间戳
inline double now_ms()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}
