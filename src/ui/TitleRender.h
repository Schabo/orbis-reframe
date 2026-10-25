#pragma once

#include "../core/Title.h"

#include <cstdint>
#include <memory>
#include <vector>

// A title's text as a picture: rendered with QPainter into a transparent
// RGBA image (premultiplied alpha -- an opaque video frame reads the same
// either way), which playback and the export then place and blend like any
// overlay's frame. GUI thread (the fonts).
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

// The text at a font of `fontPx` pixels, one picture per (style, size) kept
// for reuse. Empty for blank text.
Picture render(const title::Style &style, int fontPx);

// The font size to render a title at: its largest text size (`maxSize`, a
// fraction of `outH`) in pixels, at most `capPx`.
int fontPxFor(float maxSize, int outH, int capPx);

} // namespace titlerender
