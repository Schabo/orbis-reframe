#pragma once

#include "../core/Title.h"
#include "../core/TitleClock.h"

#include <StillSource.h> // exportpipe::StillSource

#include <cstdint>

// A title clip's pictures over its time, for playback (InsetFeed) and the
// export (InsetCompositor) alike: the title's *state* at a clip-local time
// -- its text with the clock's and the GPS's fields filled in
// (title::withFields), and
// how much of it is typed -- and the picture for it, rendered by
// titlerender on the calling thread and kept in its cache. A title without
// fields that doesn't type on or off has one state, all of its text: the
// picture titlerender::render has always made.
//
// Thread-safe (titlerender is); made on the GUI thread from the clip.
class TitleSource : public exportpipe::StillSource {
public:
    // `spanMs`: the clip's length on the timeline; `fontPx` the size it is
    // rendered at (titlerender::fontPxFor) in an output of outW x outH.
    // `clock` fills in its fields; the clip starts at timeline ms `startMs`.
    TitleSource(title::Style style, inset::Motion motion, int64_t spanMs, int fontPx, int outW,
                int outH, title::Clock clock = {}, int64_t startMs = 0);

    // All of the text as it is at the clip's start, as the title rests
    // (made now if need be). Empty for blank text.
    exportpipe::Still full();

    exportpipe::Still at(int64_t localMs) override;
    exportpipe::Still ready(int64_t localMs) override;
    // With GPS fields only within kGpsLookaheadMs: -1 when nothing changes
    // before then, though it may later.
    int64_t nextChange(int64_t localMs) override;

    static constexpr int64_t kGpsLookaheadMs = 1000;
    static constexpr int64_t kGpsStepMs = 20; // a value lasts longer than this

    // How many graphemes show at `localMs` (titlerender::visibleAt; -1: all).
    int stateAt(int64_t localMs) const;
    // The text at `localMs`: the style's with its fields filled in.
    QString textAt(int64_t localMs) const;

private:
    exportpipe::Still still(int64_t localMs, int visible, bool make);
    // The first clip-local time after `localMs` with another text, or -1.
    int64_t nextTextChange(int64_t localMs) const;

    const title::Style m_style;
    const inset::Motion m_motion;
    const int64_t m_spanMs;
    const int m_fontPx;
    const double m_widthScale; // output height / width
    const bool m_types;        // types on or off
    const title::Clock m_clock;
    const int64_t m_startMs;
    const bool m_fields;     // has clock or GPS fields
    const bool m_gpsFields;  // has GPS fields
    const int m_clockStepS;  // how often they can change: a second, or a minute
};
