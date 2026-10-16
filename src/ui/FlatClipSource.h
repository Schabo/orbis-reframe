#pragma once

#include <IStitchSource.h>

#include <string>

// What playback opens for a flat video on the main track (ClipItem::flat): a
// stitch source with no picture of its own -- a tiny black equirect per frame
// at the clip's rate, so PlaybackController's clock, cuts and buffering run
// over it as over any clip, and the view behind the picture stays black.
// The picture itself is drawn over that as a full-frame layer by InsetFeed,
// the overlays' decoder.
class FlatClipSource : public stitch::IStitchSource {
public:
    // `fps` and `durationMs` from the clip (the media probe); nothing is read
    // from the file.
    FlatClipSource(double fps, int64_t durationMs);

    bool Open(const std::string &clipPath, const stitch::PreviewStitchConfig &config) override;
    const stitch::ClipInfo &GetInfo() const override { return m_info; }
    stitch::Frame GetFrame(int64_t timestampMs) override;
    bool SeekTo(int64_t timestampMs) override;
    stitch::Frame ReadFrame() override;
    const std::string &GetLastError() const override { return m_error; }

private:
    int64_t msOfFrame(int64_t frame) const;

    stitch::ClipInfo m_info;
    int64_t m_next = 0; // index of the frame ReadFrame() returns next
    std::string m_error;
};
