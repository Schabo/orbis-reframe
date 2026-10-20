#pragma once

#include <IStitchSource.h>

#include <string>

// A stitch source with no picture of its own: a tiny black equirect per frame
// at a clip's rate, so PlaybackController's clock, cuts and buffering run over
// it as over any clip. Playback opens one for
// - a flat video on the main track (ClipItem::flat): the view behind it stays
//   black, and the picture itself is drawn over that as a full-frame layer by
//   InsetFeed, the overlays' decoder;
// - a main-track clip whose file is missing or can't be opened: black for its
//   span, so playback goes on to the next clip with the playhead and the
//   sound in step.
class BlankClipSource : public stitch::IStitchSource {
public:
    // `fps` and `durationMs` from the clip (the media probe); nothing is read
    // from the file. With `needsFile`, Open fails when the file isn't there
    // (a flat clip's stand-in: the file is what the layer plays).
    BlankClipSource(double fps, int64_t durationMs, bool needsFile);

    bool Open(const std::string &clipPath, const stitch::PreviewStitchConfig &config) override;
    const stitch::ClipInfo &GetInfo() const override { return m_info; }
    stitch::Frame GetFrame(int64_t timestampMs) override;
    bool SeekTo(int64_t timestampMs) override;
    stitch::Frame ReadFrame() override;
    const std::string &GetLastError() const override { return m_error; }

private:
    int64_t msOfFrame(int64_t frame) const;

    stitch::ClipInfo m_info;
    bool m_needsFile;
    int64_t m_next = 0; // index of the frame ReadFrame() returns next
    std::string m_error;
};
