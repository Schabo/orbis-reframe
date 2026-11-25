#pragma once

// Small media files written by the tests themselves, so no test needs
// footage: a 16-bit stereo 48 kHz WAV, a short MPEG-4, H.264 or 10-bit FFV1
// video, an H.264 picture.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace testmedia {

constexpr int kWavRate = 48000;
constexpr int kWavChannels = 2;

// `frames` frames; `sample(i)` gives frame i's value (-1..1, both channels).
void writeWav(const std::string &path, int frames, const std::function<double(int)> &sample);

struct VideoSpec {
    int width = 320;
    int height = 180;
    int fps = 25;
    int fpsDen = 1; // the rate is fps / fpsDen: 30000 / 1001 for 29.97
    int frames = 50;
    int rotationCw = 0; // tags the stream with a display matrix
    bool hlg = false;   // tags the stream HDR: HLG transfer, BT.2020
    int streams = 1;    // video streams carrying the same picture (a lens each)
    bool h264 = false;  // H.264 (what GPUs decode), not MPEG-4 Part 2; fails without the encoder
    bool tenBit = false; // 10-bit 4:2:0 (yuv420p10), as lossless FFV1: write it to a .mkv
    // Pixel (x, y)'s luma in frame `frame`; chroma is neutral. Empty = mid grey.
    std::function<uint8_t(int frame, int x, int y)> luma;
    // Whether stream `stream` gets frame `frame`; empty = every frame in every
    // stream. Keyframes come every 10 frames, so a stream left without the
    // frame before one (9, 19, ...) still decodes the rest as written.
    std::function<bool(int stream, int frame)> keep;
};

bool writeVideo(const std::string &path, const VideoSpec &spec);

// One H.264 keyframe of a flat grey `luma` picture, as an Annex B elementary
// stream (what a camera keeps as a preview frame). Empty when FFmpeg has no
// H.264 encoder here.
std::vector<uint8_t> encodeH264Picture(int width, int height, uint8_t luma);

// A baseline JPEG, full-range 4:2:0 (yuvj420p, as cameras write them), of
// `width` x `height` with pixel (x, y)'s luma `luma(x, y)` and neutral
// chroma, followed by `trailer` (an Insta360 photo's `inst` record block).
// `metadataBytes`: about that much of an application block ahead of the
// picture, as a camera's Exif: a real photo's runs past FFmpeg's probe, so
// the probe never reaches the trailer (in a small file the bytes after the
// picture make it refuse the file). False when FFmpeg has no MJPEG encoder
// or the file can't be written.
bool writeJpeg(const std::string &path, int width, int height,
               const std::function<uint8_t(int x, int y)> &luma,
               const std::vector<uint8_t> &trailer = {}, size_t metadataBytes = 0);

} // namespace testmedia
