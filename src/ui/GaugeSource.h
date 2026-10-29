#pragma once

#include "GaugeRender.h"

#include "../core/Gauge.h"
#include "../core/TitleClock.h"

#include <StillSource.h> // exportpipe::StillSource

#include <cstdint>
#include <memory>

// A gauge clip's pictures over its time, for playback (InsetFeed) and the
// export (InsetCompositor) alike: its reading at a clip-local time -- the
// GPS or motion value there, from `clock` (gaugerender::readingOf); for a
// route the whole timeline's track, taken from `clock` once -- and the
// picture for it, rendered by gaugerender on the calling thread and kept in
// its cache. Placed like a title's: its look's width is its height.
//
// Thread-safe (gaugerender is); made on the GUI thread from the clip.
class GaugeSource : public exportpipe::StillSource {
public:
    // `heightPx`: the size it is rendered at in an output of outW x outH.
    // The clip starts at timeline ms `startMs` and lasts `spanMs`.
    GaugeSource(gauge::Style style, int64_t spanMs, int heightPx, int outW, int outH,
                title::Clock clock, int64_t startMs);

    exportpipe::Still at(int64_t localMs) override;
    exportpipe::Still ready(int64_t localMs) override;
    // Only within kLookaheadMs, as a title's GPS fields: -1 when nothing
    // changes before then, though it may later.
    int64_t nextChange(int64_t localMs) override;

    gaugerender::Reading readingAt(int64_t localMs) const;

    static constexpr int64_t kLookaheadMs = 1000;
    static constexpr int64_t kStepMs = 20;

private:
    exportpipe::Still still(const gaugerender::Picture &pic) const;

    const gauge::Style m_style;
    const int64_t m_spanMs;
    const int m_heightPx;
    const double m_widthPerSize; // the picture's width per its height, in output widths
    const title::Clock m_clock;
    const int64_t m_startMs;
    const std::shared_ptr<const gaugerender::Route> m_route; // a route gauge's
};
