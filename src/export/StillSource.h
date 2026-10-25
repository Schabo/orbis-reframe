#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

// Where a layer's picture comes from when nothing is decoded: a title's text
// (src/ui/TitleSource) or a dip's black. A picture can change over the clip
// -- a title typing on -- but only at moments the source knows ahead
// (nextChange), so playback can have the next one ready. The export's
// InsetCompositor asks for the picture at each frame (at()); playback's
// InsetFeed shows what is ready() and has the rest rendered on its decoder
// thread.
//
// Thread-safe: the same source may be asked from several threads.

namespace exportpipe {

struct Still {
    // Premultiplied RGBA, width * height * 4; null = nothing to draw.
    std::shared_ptr<const std::vector<uint8_t>> rgba;
    int width = 0, height = 0;
    // A title's picture: its width over the look's width (the text size),
    // and where across it the look's centreX sits (inset::anchored); it is
    // drawn at its own shape. 0: not a title -- the picture fills the look
    // at the layer's aspect.
    double widthPerSize = 0.0;
    double anchorX = 0.5;

    bool empty() const { return !rgba; }
};

class StillSource {
public:
    virtual ~StillSource() = default;
    // The picture at clip-local timeline time `localMs`, made on the calling
    // thread if it isn't ready.
    virtual Still at(int64_t localMs) = 0;
    // The same if it is ready, else an empty Still; never makes one.
    virtual Still ready(int64_t localMs) = 0;
    // The first clip-local time after `localMs` whose picture differs from
    // the one at `localMs`, or -1 if it never changes again.
    virtual int64_t nextChange(int64_t localMs) = 0;
};

// One picture all along.
class FixedStill : public StillSource {
public:
    explicit FixedStill(Still s) : m_still(std::move(s)) {}
    Still at(int64_t) override { return m_still; }
    Still ready(int64_t) override { return m_still; }
    int64_t nextChange(int64_t) override { return -1; }

private:
    const Still m_still;
};

} // namespace exportpipe
