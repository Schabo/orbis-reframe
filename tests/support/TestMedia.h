#pragma once

// Small media files written by the tests themselves, so no test needs
// footage: a 16-bit stereo 48 kHz WAV, a short MPEG-4 video, an H.264 picture.

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
    int frames = 50;
    int rotationCw = 0; // tags the stream with a display matrix
    bool hlg = false;   // tags the stream HDR: HLG transfer, BT.2020
    int streams = 1;    // video streams carrying the same picture (a lens each)
    // Pixel (x, y)'s luma in frame `frame`; chroma is neutral. Empty = mid grey.
    std::function<uint8_t(int frame, int x, int y)> luma;
};

bool writeVideo(const std::string &path, const VideoSpec &spec);

// One H.264 keyframe of a flat grey `luma` picture, as an Annex B elementary
// stream (what a camera keeps as a preview frame). Empty when FFmpeg has no
// H.264 encoder here.
std::vector<uint8_t> encodeH264Picture(int width, int height, uint8_t luma);

} // namespace testmedia
