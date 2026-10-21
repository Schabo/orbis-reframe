// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "Image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace insv {

// Decodes the one picture in `bitstream` -- an elementary stream of codec
// `codec` (an FFmpeg decoder name, "h264"), as a camera stores a preview frame
// in its trailer (the ONE X2's records 0x200 / 0x500: one H.264 keyframe
// each) -- into RGB24, scaled down to fit `maxSide` (never up). The frame's
// own matrix and range; a full-range (JPEG) format as such. False with `err`
// set when nothing decodes.
bool DecodeStillPicture(const std::vector<uint8_t>& bitstream, const char* codec, int maxSide,
                        Image& out, std::string& err);

} // namespace insv
