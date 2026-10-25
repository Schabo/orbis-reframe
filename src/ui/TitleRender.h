#pragma once

#include "../core/Title.h"

#include <cstdint>
#include <memory>
#include <vector>

// A title's text as a picture: rendered with QPainter into a transparent
// RGBA image (premultiplied alpha -- an opaque video frame reads the same
// either way), which playback and the export then place and blend like any
// overlay's frame. Thread-safe: playback's decoder thread and the export
// render titles too.
namespace titlerender {

struct Picture {
    std::shared_ptr<const std::vector<uint8_t>> rgba; // width * height * 4; null = nothing to draw
    int width = 0;
    int height = 0;
    // The picture's width over the font's pixel size: at text size `s`
    // (title::Style, a fraction of the output height) the picture covers
    // s * outH * widthPerFontPx pixels across.
    double widthPerFontPx = 0.0;
    // How far across the picture its anchor is (title::drawnLook): the left
    // or right edge of the text -- or of its box -- or the middle.
    double anchorX = 0.5;

    bool empty() const { return !rgba; }
};

// The text at a font of `fontPx` pixels, one picture per (style, size,
// visible, digits) kept for reuse. Empty for blank text. With `visible` >= 0
// only its first `visible` graphemes are drawn (a title typing on): the
// picture is otherwise the same -- its size, anchor, box and where each
// letter sits in a line typed in full -- and the line being typed lines up
// by its typed part. `tabularDigits`: every digit as wide as the others (the
// font's "tnum" feature, where it has one), so a clock doesn't shift as it
// ticks.
Picture render(const title::Style &style, int fontPx, int visible = -1,
               bool tabularDigits = false);
// The same if it has been made and is still kept, else an empty picture.
Picture cached(const title::Style &style, int fontPx, int visible = -1,
               bool tabularDigits = false);

// How many graphemes (letters as a reader counts them; spaces and line
// breaks count too) `text` has.
int graphemeCount(const QString &text);
// How many of a title's `count` graphemes show at clip-local time `localMs`
// of a clip `spanMs` long with `motion`: all (-1) but while it types on or
// off (inset::Motion::Kind::Type), spread over that part's length.
int visibleAt(int count, const inset::Motion &motion, int64_t localMs, int64_t spanMs);

// The font size to render a title at: its largest text size (`maxSize`, a
// fraction of `outH`) in pixels, at most `capPx`.
int fontPxFor(float maxSize, int outH, int capPx);

} // namespace titlerender
