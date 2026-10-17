#pragma once

#include <QString>

#include <cstdint>

// A title: text drawn over the output, a clip on an overlay track that has no
// file behind it. The text is rendered into a transparent picture
// (src/ui/TitleRender) and placed like any overlay (inset::Look: centre,
// opacity, fades, keyframes); for a title the look's `width` is the text size
// instead -- the font's pixel size as a fraction of the output height -- so
// the box follows the text, and editing the text doesn't change its size.
namespace title {

struct Style {
    QString text;
    QString font = QStringLiteral("Sans Serif"); // family
    bool bold = true;
    bool italic = false;
    uint32_t rgb = 0xffffff;

    bool operator==(const Style &o) const
    {
        return text == o.text && font == o.font && bold == o.bold && italic == o.italic &&
               rgb == o.rgb;
    }
    bool operator!=(const Style &o) const { return !(*this == o); }
};

// A title has no source file, but trims and keyframes work in source time
// like a video's: it gets a long made-up source and starts in its middle, so
// either edge can be dragged out.
constexpr qint64 kSourceMs = 4LL * 3600 * 1000;
constexpr qint64 kDefaultSpanMs = 5000;
// Its look when added: centred, text 8% of the frame high.
constexpr float kDefaultSize = 0.08f;

// A title dragged from the Titles tab onto the timeline.
constexpr const char *kMimeType = "application/x-orbis-reframe-title";

} // namespace title
