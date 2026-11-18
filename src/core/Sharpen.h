#pragma once

// The sharpen effect (fx::Sharpen) on a picture: an unsharp mask on the
// luma. The C++ twin of src/render/shaders/sharpen.comp -- the CPU export
// runs this, the GPU paths the shader; change both together.
//
// The blur is a Gaussian, run across and then down (two passes). Only the
// clip's own pixels count: each pixel carries a label (0 = no picture there
// -- a split screen's divider, a flat clip's bars, outside a globe's disc;
// another number per pane), a tap on another label is left out and the
// weights that remain are scaled back to 1. So nothing outside the clip's
// picture bleeds into it, a pane is sharpened against its own pixels, and
// pixels without a picture stay as they are.
//
// Then at each pixel: d = luma - blurred luma; the part of |d| over the
// threshold, times the amount, is added to red, green and blue alike, so the
// colours (Cb, Cr) don't change -- only the luma.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "ClipEffects.h"

namespace fx {

// The luma the sharpen works on (BT.709's weights).
inline float sharpenLuma(const float *rgb)
{
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

// The blur for a frame `frameH` pixels high: `taps` either side of the
// centre, weight[i] for the tap i pixels away (not normalised; the passes
// divide by the weights they used).
struct SharpenKernel {
    // A frame of 4320 lines at the largest radius is a spread of 12 pixels;
    // three spreads either side reach where a Gaussian is under 1 %.
    static constexpr int kMaxTaps = 40;

    float sigma = 0.0f; // pixels
    int taps = 0;
    float weight[kMaxTaps + 1] = {};
};

inline SharpenKernel sharpenKernel(const Render &e, float frameH)
{
    SharpenKernel k;
    k.sigma = e.sharpen[1] * frameH;
    if (!(k.sigma > 0.0f))
        return k;
    k.taps = std::min(SharpenKernel::kMaxTaps, int(std::ceil(3.0f * k.sigma)));
    for (int i = 0; i <= k.taps; ++i)
        k.weight[i] = std::exp(-float(i * i) / (2.0f * k.sigma * k.sigma));
    return k;
}

// One pixel's sharpened colour: `rgb` its colour, `blurred` the blurred
// luma there, values in [0, white].
inline void sharpenPixel(float *rgb, float blurred, const Render &e, float white)
{
    const float d = sharpenLuma(rgb) - blurred;
    const float over = std::max(std::fabs(d) - e.sharpen[2] * white, 0.0f);
    const float add = e.sharpen[0] * std::copysign(over, d);
    for (int k = 0; k < 3; ++k)
        rgb[k] = std::clamp(rgb[k] + add, 0.0f, white);
}

// Sharpens `px` in place: w x h pixels, `channels` floats each (red, green,
// blue first), values in [0, white]. `label` holds a pixel's label (see the
// top), or is null when every pixel is the clip's. `frameH`: the output
// frame's height in pixels, which the radius is against (a pane or a flat
// clip's picture is smaller than the frame).
inline void sharpen(float *px, int channels, const uint8_t *label, int w, int h, float frameH,
                    const Render &e, float white)
{
    if (!e.sharpens() || w <= 0 || h <= 0)
        return;
    const SharpenKernel k = sharpenKernel(e, frameH);
    if (k.taps == 0)
        return;
    const auto at = [w](int x, int y) { return size_t(y) * size_t(w) + size_t(x); };
    const auto lab = [label](size_t i) { return label ? label[i] : uint8_t(1); };

    std::vector<float> luma(size_t(w) * h), across(size_t(w) * h);
    for (size_t i = 0; i < luma.size(); ++i)
        luma[i] = sharpenLuma(px + i * size_t(channels));

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t c = at(x, y);
            const uint8_t l = lab(c);
            if (l == 0)
                continue;
            float sum = luma[c] * k.weight[0], wsum = k.weight[0];
            for (int i = 1; i <= k.taps; ++i) {
                if (x - i >= 0 && lab(c - size_t(i)) == l) {
                    sum += luma[c - size_t(i)] * k.weight[i];
                    wsum += k.weight[i];
                }
                if (x + i < w && lab(c + size_t(i)) == l) {
                    sum += luma[c + size_t(i)] * k.weight[i];
                    wsum += k.weight[i];
                }
            }
            across[c] = sum / wsum;
        }

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t c = at(x, y);
            const uint8_t l = lab(c);
            if (l == 0)
                continue;
            float sum = across[c] * k.weight[0], wsum = k.weight[0];
            for (int i = 1; i <= k.taps; ++i) {
                if (y - i >= 0 && lab(at(x, y - i)) == l) {
                    sum += across[at(x, y - i)] * k.weight[i];
                    wsum += k.weight[i];
                }
                if (y + i < h && lab(at(x, y + i)) == l) {
                    sum += across[at(x, y + i)] * k.weight[i];
                    wsum += k.weight[i];
                }
            }
            sharpenPixel(px + c * size_t(channels), sum / wsum, e, white);
        }
}

} // namespace fx
