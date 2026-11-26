// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// GoPro's GPMF telemetry: a big-endian KLV tree, carried in a `.360`'s
// `bin_data` track one sample per packet (docs/formats/gopro-max.md
// [[telemetry]]). No FFmpeg: the samples come from the caller
// (camera::GoproReader demuxes them and lines them up with the video).
//
// The framing, as the Max writes it:
//
//     tag(4) | type(1) | bytes_per_sample(1) | count(u16 BE) | payload
//     tag(4) | size(u32 BE)                                    | nested tags
//
// Two things about that are easy to get wrong and both bite on real files.
// The count is 16-bit and its high byte shares the field a naive reader calls
// `size` (u16 LE); the two readings agree while the count fits in 8 bits and
// disagree on the gyroscope, which carries 833 rows per sample — mis-sized
// there, a parser that validates payload ends stops walking and reports the
// gyro once instead of once per sample. Measured over the sample tree's 18
// extracted blobs: the `size:u16le x repeat:u8` reading lands each tag's end
// on the following tag 527 times, this one 829 times.
//
// And a struct's declared size is an aggregate for the whole track, not for
// the sample: a 10 KB sample declares a 76 KB `DEVC`. Struct sizes are
// therefore clamped to the buffer, which is what the files ask for.

namespace orbiscore {

// One tag of a GPMF buffer, as the walk finds it.
struct GpmfEvent {
    std::string_view tag;
    // A struct: `payload` is empty and its tags follow, at `depth` + 1. A
    // scalar: `payload` is its bytes (unpadded), `count` its elements, `type`
    // the GPMF type char, and values inside it are big endian.
    bool isStruct = false;
    int depth = 0;
    char type = 0;
    uint64_t count = 0;
    std::string_view payload;
};

// Every tag of a GPMF buffer, in file order, structs descended into. Return
// false to stop.
using GpmfVisitor = std::function<bool(const GpmfEvent&)>;
bool WalkGpmf(const uint8_t* data, size_t size, const GpmfVisitor& visit);

// One sensor stream of one sample: the accelerometer, the gyroscope, the
// magnetometer, and the rest. A sample carries eleven of them.
struct GpmfStream {
    std::string name;   // STNM: "Gyroscope", "Accelerometer", ...
    std::string unit;   // SIUN: "rad/s", "m/s^2"
    int64_t stmpUs = 0; // STMP: the camera clock's time of this stream's first row
    uint64_t tsmp = 0;  // TSMP: this stream's rows so far, cumulative across the take
    double scal = 1.0;  // SCAL: raw / scal is in `unit`
    bool hasMtrx = false;
    // MTRX: this stream's sensor axes in the camera body, row-major. A real
    // matrix rather than a guess, and per stream.
    double mtrx[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    // The rows themselves, as stored: `count` elements of `type`, big endian,
    // three components each for the motion streams. They live under the
    // stream's own tag (`GYRO`, `ACCL`, `MAGN`, …), which is `dataTag`.
    std::string dataTag;
    char type = 0;
    uint64_t count = 0;
    std::vector<uint8_t> payload;

    // The rows as int16 (the motion streams' shape): count/3 of them per axis.
    std::vector<int16_t> rowsI16() const;
};

// One GPMF sample: one packet of the sensor track, covering ~1.04 s of video.
using GpmfSample = std::vector<GpmfStream>;

// One sample — one packet's payload, which is what a demuxer hands over.
GpmfSample ParseGpmfSensorSample(const uint8_t* data, size_t size);

// A whole track at once: the samples concatenated, as a reader that pulled
// the track out of the container in one go has it. Split on the `DEVC` that
// starts each sample.
std::vector<GpmfSample> ParseGpmfSensorTrack(const uint8_t* data, size_t size);

} // namespace orbiscore
