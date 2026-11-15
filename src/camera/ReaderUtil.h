#pragma once

#include "ClipMetadata.h"

#include <orbis-core/Image.h>

#include <string>

// Small helpers the camera readers share.

namespace camera {

// An RGB24 picture as a Thumbnail: NV12, BT.601 limited range (the X5's own
// preview's coding), not flat. Odd sizes lose their last row / column.
Thumbnail ThumbnailFromRgb(const insv::Image& rgb);

// "YYYY-MM-DDThh:mm:ss" from the 14 digits YYYYMMDDhhmmss, "" unless they
// make a date and time.
std::string LocalTimeFromDigits(const std::string& digits);

} // namespace camera
