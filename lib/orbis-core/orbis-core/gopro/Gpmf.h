// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// GoPro's GPMF telemetry: a big-endian KLV tree, carried in a `.360`'s
// `gpmd` data track one sample per packet (docs/formats/gopro-max.md,
// *Telemetry*). No FFmpeg: the samples come from the caller
// (camera::GoproReader demuxes them and lines them up with the video).
//
// The framing is GoPro's (its GPMF documentation, *KLV Design*): every tag,
// a nest included, is
//
//     tag(4) | type(1) | structure size(1) | repeat(u16 BE) | payload
//
// with size × repeat payload bytes, padded to a multiple of 4. Type 0 is a
// nest: its payload is more tags. Every other type is a scalar of `repeat`
// structures of `size` bytes, so the walk steps over a tag by its size
// whether or not it knows the type.

namespace orbiscore {

// One tag of a GPMF buffer, as the walk finds it.
struct GpmfEvent {
    std::string_view tag;
    // A nest (type 0): `payload` is empty and its tags follow, at `depth` + 1.
    // A scalar: `payload` is its bytes (unpadded), `count` its repeat count
    // (rows, for a sensor's data), `type` the GPMF type char, and values
    // inside it are big endian.
    bool isStruct = false;
    int depth = 0;
    char type = 0;
    uint64_t count = 0;
    std::string_view payload;
};

// Every tag of a GPMF buffer, in file order, nests descended into. Return
// false to stop. Returns true when the walk reached the buffer's end, false
// when it stopped early: told to, or on a tag that doesn't fit its nest.
using GpmfVisitor = std::function<bool(const GpmfEvent&)>;
bool WalkGpmf(const uint8_t* data, size_t size, const GpmfVisitor& visit);

// One sensor stream of one sample: the accelerometer, the gyroscope, the
// magnetometer, and the rest: one `STRM`, a sibling of the others in the
// sample's `DEVC`.
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
// the track out of the container in one go has it. Each top-level `DEVC` is
// one sample.
std::vector<GpmfSample> ParseGpmfSensorTrack(const uint8_t* data, size_t size);

} // namespace orbiscore
