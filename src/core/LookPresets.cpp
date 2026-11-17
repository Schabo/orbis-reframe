#include "LookPresets.h"

#include "ColorLut.h"

#include <algorithm>
#include <cmath>

namespace color::look {

namespace {

// Entries per side of a built-in look's LUT: the size camera makers ship,
// and fine enough for these smooth curves under trilinear filtering.
constexpr int kLutSize = 33;

// The picture is Rec.709 for a BT.1886 display: a 2.4 power to and from
// linear light, so the channels mix as light does.
constexpr float kGamma = 2.4f;

// Black & White's channel weights in linear light. Rec.709's luminance is
// 0.2126 / 0.7152 / 0.0722; these lean toward red and away from blue, as a
// yellow filter does on black-and-white film.
constexpr float kBwWeights[3] = {0.30f, 0.65f, 0.05f};
// How much of a smoothstep S-curve is mixed into the encoded grey: a gentle
// toe and shoulder (slope 0.75 at black and white, 1.125 in the middle), so
// the picture has some contrast without clipping.
constexpr float kBwCurve = 0.25f;

std::shared_ptr<const Lut3D> sample(const char *title, void (*formula)(const float *, float *))
{
    auto lut = std::make_shared<Lut3D>();
    lut->title = title;
    lut->size = kLutSize;
    lut->rgb.resize(std::size_t(kLutSize) * kLutSize * kLutSize * 3);
    std::size_t i = 0;
    for (int b = 0; b < kLutSize; ++b)
        for (int g = 0; g < kLutSize; ++g)
            for (int r = 0; r < kLutSize; ++r) { // red fastest, as in a .cube
                const float in[3] = {float(r) / (kLutSize - 1), float(g) / (kLutSize - 1),
                                     float(b) / (kLutSize - 1)};
                formula(in, &lut->rgb[i]);
                i += 3;
            }
    return lut;
}

} // namespace

void blackWhite(const float in[3], float out[3])
{
    float y = 0.0f;
    for (int k = 0; k < 3; ++k)
        y += kBwWeights[k] * std::pow(std::clamp(in[k], 0.0f, 1.0f), kGamma);
    const float v = std::pow(y, 1.0f / kGamma);
    const float s = v * v * (3.0f - 2.0f * v);
    out[0] = out[1] = out[2] = std::clamp(v + kBwCurve * (s - v), 0.0f, 1.0f);
}

std::shared_ptr<const Lut3D> presetLut(std::string_view id)
{
    if (id == kBlackWhite) {
        // Built once, thread-safely, on first use.
        static const std::shared_ptr<const Lut3D> lut = sample("Black & White", blackWhite);
        return lut;
    }
    return nullptr;
}

} // namespace color::look
