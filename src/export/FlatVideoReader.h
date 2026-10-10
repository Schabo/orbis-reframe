#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Decodes a plain ("flat") video file -- an mp4 / mov / mkv from a phone, an
// action cam, a screen recording -- to RGBA frames at a chosen size, for the
// picture-in-picture insets on a video track. CPU decode through FFmpeg
// (threaded), no stitching or reprojection. The container's rotation (a phone's
// portrait clip) and sample aspect ratio are applied, so frames come out the
// way a player shows them.
//
// Not thread-safe: one reader per thread. Links system FFmpeg only.

namespace exportpipe {

struct VideoFrame {
    int64_t ptsMs = -1; // source time of the frame, from the stream's start
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // width * height * 4, opaque (alpha 255)
};

class FlatVideoReader {
public:
    FlatVideoReader();
    ~FlatVideoReader();
    FlatVideoReader(const FlatVideoReader &) = delete;
    FlatVideoReader &operator=(const FlatVideoReader &) = delete;

    // What a file holds, without keeping it open. False (with `err`) if it
    // has no decodable video stream.
    struct Info {
        int width = 0; // display size: rotation and sample aspect applied
        int height = 0;
        int64_t durationMs = 0;
        double fps = 0.0;
    };
    static bool probe(const std::string &path, Info &info, std::string &err);

    bool open(const std::string &path, std::string &err);
    bool isOpen() const;
    const Info &info() const;

    // Size of the RGBA frames handed out (defaults to the display size).
    // Takes effect from the next frame converted.
    void setOutputSize(int width, int height);

    // Position so the next readNext() returns the frame showing at `ms` (the
    // last one at or before it; the first frame if `ms` precedes it).
    bool seek(int64_t ms);
    // The next frame in decode order, converted. False at the end or on error.
    bool readNext(VideoFrame &out);

    // The frame showing at source time `ms`: decodes forward from the current
    // position, and seeks only when `ms` is behind it or well ahead. The
    // pointer stays valid until the next call. Null if there is no frame (an
    // empty file, or an error).
    const VideoFrame *frameAt(int64_t ms);

    const std::string &lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace exportpipe
