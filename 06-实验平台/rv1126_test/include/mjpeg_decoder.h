#pragma once

#include "frame.h"

#include <cstddef>
#include <cstdint>

class MjpegDecoder {
public:
    bool decode(const uint8_t* compressed_data, size_t compressed_size,
                BgrFrame& destination);
};
