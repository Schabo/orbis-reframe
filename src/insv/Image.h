// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Tiny interleaved 8-bit RGB image, the currency of the stitch path
// (src/insv/). Deliberately trivial and dependency-free -- InsvDemux fills one
// per fisheye frame, FisheyeStitcher reads two and writes an equirect one. For
// an RGBA consumer (e.g. a future NativeStitchSource feeding IStitchSource::
// Frame) convert at the boundary; the projection math is all RGB.

namespace insv {

struct Image {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgb; // w*h*3, row-major, interleaved R,G,B

    void alloc(int width, int height)
    {
        w = width;
        h = height;
        rgb.assign(static_cast<std::size_t>(width) * height * 3, 0);
    }

    bool empty() const { return w <= 0 || h <= 0 || rgb.empty(); }

    // Bilinear sample at pixel coords (u,v). Out-of-range coords clamp to the
    // edge (fisheye content near the rim is already vignetted, and the
    // equirect sampler wants clamp-to-edge at the poles); non-finite coords
    // return black rather than indexing garbage.
    std::array<float, 3> sample(double u, double v) const
    {
        if (w <= 0 || h <= 0 || !std::isfinite(u) || !std::isfinite(v))
            return {0.0f, 0.0f, 0.0f};
        u = std::clamp(u, 0.0, w - 1.0);
        v = std::clamp(v, 0.0, h - 1.0);
        const int x0 = static_cast<int>(u);
        const int y0 = static_cast<int>(v);
        const int x1 = std::min(x0 + 1, w - 1);
        const int y1 = std::min(y0 + 1, h - 1);
        const double fx = u - x0;
        const double fy = v - y0;
        std::array<float, 3> out{};
        for (int c = 0; c < 3; ++c) {
            const double a = rgb[(static_cast<std::size_t>(y0) * w + x0) * 3 + c];
            const double b = rgb[(static_cast<std::size_t>(y0) * w + x1) * 3 + c];
            const double cc = rgb[(static_cast<std::size_t>(y1) * w + x0) * 3 + c];
            const double d = rgb[(static_cast<std::size_t>(y1) * w + x1) * 3 + c];
            out[c] = static_cast<float>((a * (1 - fx) + b * fx) * (1 - fy) +
                                        (cc * (1 - fx) + d * fx) * fy);
        }
        return out;
    }
};

} // namespace insv
