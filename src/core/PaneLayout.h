#pragma once

// Split screen: a 360 clip's picture divided into 2 or 4 panes, each the
// same sphere seen through its own camera (ClipItem::reframe for pane 0,
// ClipItem::panes for the others). The one place the pane geometry lives:
// the viewport, both GPU export paths, the CPU export, a transition's other
// clip and the subject tracker all place panes with rects() below, so every
// pane has the same rectangle and aspect -- and so the same field of view --
// wherever it's drawn.
//
// Header-only and free of Qt / FFmpeg / Vulkan, like InsetGeometry.h.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pane {

static constexpr int kMaxPanes = 4;

enum class Layout {
    Single,     // the whole frame, one camera (no split)
    SideBySide, // two panes, left and right
    Stacked,    // two panes, top and bottom
    Grid,       // four panes, 2 x 2: top left, top right, bottom left, bottom right
};

inline int count(Layout l)
{
    switch (l) {
    case Layout::Single:
        break;
    case Layout::SideBySide:
    case Layout::Stacked:
        return 2;
    case Layout::Grid:
        return 4;
    }
    return 1;
}

// The project file's name for a layout, and back (an unknown name, from a
// newer build: no split).
inline const char *layoutName(Layout l)
{
    switch (l) {
    case Layout::Single:
        break;
    case Layout::SideBySide:
        return "sideBySide";
    case Layout::Stacked:
        return "stacked";
    case Layout::Grid:
        return "grid";
    }
    return "single";
}
inline Layout layoutFromName(const char *name)
{
    if (!name)
        return Layout::Single;
    for (Layout l : {Layout::SideBySide, Layout::Stacked, Layout::Grid})
        if (std::strcmp(name, layoutName(l)) == 0)
            return l;
    return Layout::Single;
}

// A clip's split: the layout and the divider between its panes.
struct Split {
    static constexpr float kDefaultGap = 0.004f; // ~4 px at 1080p
    static constexpr float kMaxGap = 0.05f;

    Layout layout = Layout::Single;
    float gap = kDefaultGap; // divider width, fraction of the output height; 0 = touching
    uint32_t gapRgb = 0x000000;

    int count() const { return pane::count(layout); }
    bool isSplit() const { return layout != Layout::Single; }

    Split clamped() const
    {
        Split s = *this;
        s.gap = std::isfinite(s.gap) ? std::clamp(s.gap, 0.0f, kMaxGap) : kDefaultGap;
        s.gapRgb &= 0xffffff;
        return s;
    }
    bool operator==(const Split &o) const
    {
        return layout == o.layout && gap == o.gap && gapRgb == o.gapRgb;
    }
    bool operator!=(const Split &o) const { return !(*this == o); }
};

// A pane's rectangle in output pixels (x, y = top-left).
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    double aspect() const { return h > 0 ? double(w) / double(h) : 16.0 / 9.0; }
};

namespace detail {
// [0, a) and [b, size) split around the middle with a gap of `gapPx`, both
// edges on multiples of `align` (the frame's own edges stay where they are).
inline void cut(int size, double gapPx, int align, int &a, int &b)
{
    const auto snap = [align](double v) {
        return static_cast<int>(std::lround(v / align)) * align;
    };
    a = snap((size - gapPx) / 2.0);
    b = snap((size + gapPx) / 2.0);
    if (gapPx > 0.0 && b <= a)
        b = a + align; // a gap asked for never vanishes in the rounding
    a = std::clamp(a, align, std::max(align, size - align));
    b = std::clamp(b, a, std::max(a, size - align));
}
} // namespace detail

// The panes of `split` in an outW x outH frame, in pane order. Inner edges
// lie on multiples of 4 horizontally and of 2 vertically: the GPU export
// writes 4 pixels per invocation and NV12 / P010 chroma per 2 x 2 block, so
// a pane renders straight into its part of the frame. (Export sizes are
// multiples of 4 x 2, so every pane's width and height are too.) The gap's
// rounding absorbs the snap, so two panes may differ by a few pixels; each
// pane's aspect is its own rectangle's. A frame too small to split has one
// pane.
inline std::vector<Rect> rects(const Split &split, int outW, int outH)
{
    if (outW <= 0 || outH <= 0)
        return {};
    const int n = split.count();
    if (n == 1 || outW < 16 || outH < 8)
        return {Rect{0, 0, outW, outH}};
    const double gapPx = double(split.clamped().gap) * outH;
    int ax = outW, bx = 0, ay = outH, by = 0; // left / right, top / bottom edges
    if (split.layout != Layout::Stacked)
        detail::cut(outW, gapPx, 4, ax, bx);
    if (split.layout != Layout::SideBySide)
        detail::cut(outH, gapPx, 2, ay, by);
    switch (split.layout) {
    case Layout::SideBySide:
        return {Rect{0, 0, ax, outH}, Rect{bx, 0, outW - bx, outH}};
    case Layout::Stacked:
        return {Rect{0, 0, outW, ay}, Rect{0, by, outW, outH - by}};
    default:
        return {Rect{0, 0, ax, ay}, Rect{bx, 0, outW - bx, ay}, Rect{0, by, ax, outH - by},
                Rect{bx, by, outW - bx, outH - by}};
    }
}

// The pane under output pixel (px, py), or -1 (in a gap or outside).
inline int at(const std::vector<Rect> &panes, double px, double py)
{
    for (size_t i = 0; i < panes.size(); ++i) {
        const Rect &r = panes[i];
        if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h)
            return static_cast<int>(i);
    }
    return -1;
}

// "Fill panes": the yaw offset of pane `index` from pane 0's, so the panes
// show front / back (two) or front / right / back / left (the grid, in its
// reading order: top left front, top right right, bottom left left, bottom
// right back -- opposite views diagonal).
inline float fillYawOffset(Layout l, int index)
{
    constexpr float kPi = 3.14159265358979f;
    if (count(l) == 2)
        return index == 1 ? kPi : 0.0f;
    if (l == Layout::Grid) {
        static constexpr float kOffsets[4] = {0.0f, kPi / 2.0f, -kPi / 2.0f, kPi};
        return kOffsets[std::clamp(index, 0, 3)];
    }
    return 0.0f;
}

// The diagonal field of view (radians, view::Pose::fov) of a rectilinear
// pane of `aspect` (width / height) that spans `hfov` across.
inline double fovFor(double hfov, double aspect)
{
    if (!(aspect > 0.0))
        aspect = 16.0 / 9.0;
    return 2.0 * std::atan(std::tan(hfov / 2.0) * std::sqrt(1.0 + 1.0 / (aspect * aspect)));
}

} // namespace pane
