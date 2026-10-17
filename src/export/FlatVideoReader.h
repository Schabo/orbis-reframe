#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Decodes a plain ("flat") video file -- an mp4 / mov / mkv from a phone, an
// action cam, a screen recording -- to RGBA or NV12 frames at a chosen size,
// for the flat clips and the picture-in-picture insets on a video track. CPU
// decode and conversion through FFmpeg (both threaded), no stitching or
// reprojection. The container's rotation (a phone's
// portrait clip) and sample aspect ratio are applied, so frames come out the
// way a player shows them.
//
// Not thread-safe: one reader per thread. Links system FFmpeg only.

namespace exportpipe {

struct VideoFrame {
    int64_t ptsMs = -1; // source time of the frame, from the stream's start
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // Format::Rgba: width * height * 4, opaque (alpha 255)
    // Format::Nv12 (width and height even): width * height luma, then
    // height / 2 rows of interleaved Cb Cr, BT.709 limited range like the
    // export's; Nv12Converter::kPadding spare bytes after them.
    std::vector<uint8_t> nv12;
};

// RGBA -> NV12 (VideoFrame::nv12's), same size, threaded swscale. Not
// thread-safe: one per thread.
class Nv12Converter {
public:
    // swscale's SIMD code reads and writes whole blocks, past the last
    // pixel when a row isn't a multiple of one: the spare bytes it needs
    // after a picture.
    static constexpr size_t kPadding = 256;

    Nv12Converter();
    ~Nv12Converter();
    Nv12Converter(const Nv12Converter &) = delete;
    Nv12Converter &operator=(const Nv12Converter &) = delete;
    // `w` and `h` even; `rgba` followed by kPadding readable bytes, `nv12`
    // sized with them too. False if swscale can't.
    bool convert(const uint8_t *rgba, int w, int h, std::vector<uint8_t> &nv12);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
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

    // Size of the frames handed out (defaults to the display size; rounded
    // up to even for Nv12). Takes effect from the next frame converted.
    void setOutputSize(int width, int height);
    // What the frames handed out hold (Rgba by default): VideoFrame::rgba or
    // ::nv12. Takes effect from the next frame converted.
    enum class Format { Rgba, Nv12 };
    void setOutputFormat(Format format);

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
