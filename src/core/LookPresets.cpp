#include "LookPresets.h"

#include "ColorLut.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <mutex>

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

// Rec.709's luminance weights, for linear light.
constexpr float kLuma[3] = {0.2126f, 0.7152f, 0.0722f};

// Deep sky: the hues it works on (centre and half-width at full strength, in
// degrees, then a fade to nothing), the saturation a colour needs before it
// counts as sky (in linear HSV, where a white cloud's tint is ~0.2 and a
// blue sky's 0.6 or more), and what it does there: saturation up, and the
// encoded brightness down.
constexpr float kSkyHue = 210.0f, kSkyHalf = 30.0f, kSkyFade = 40.0f;
constexpr float kSkySatFrom = 0.15f, kSkySatTo = 0.5f;
constexpr float kSkySaturate = 0.35f, kSkyDarken = 0.10f;

// Teal & orange (hues and saturation of the encoded picture): a hue curve
// through these points (degrees in, out; straight between them, the rest of
// the circle unchanged) -- greens a good part of the way to teal, blues only a
// little (a sky moved as far turns cyan), no stretch steeper than 1.6x so a
// foliage gradient doesn't band; skin and orange hues move a touch toward
// orange and gain saturation; the split tone tints by up to this much a
// channel; the S-curve on the luma.
constexpr float kTealCurve[][2] = {{55, 55},   {90, 112},  {125, 160}, {160, 178},
                                   {185, 185}, {220, 212}, {255, 250}, {290, 290}};
constexpr float kSkinHue = 25.0f, kSkinHalf = 15.0f, kSkinFade = 25.0f;
constexpr float kOrangeHue = 30.0f, kSkinPull = 0.15f, kSkinSaturate = 0.22f;
constexpr float kToSatFrom = 0.1f, kToSatTo = 0.4f;
constexpr float kSplit = 0.08f;
constexpr float kToCurve = 0.2f;
// Where Teal & orange's highlight shoulder starts (linear light; encoded
// ~0.98): above it the brightest channel rolls off toward 1.
constexpr float kToKnee = 0.95f;

// Faded film: the black and white it maps to, how much it lifts the middle
// on the way (a soft shoulder rather than a straight line), the saturation it
// keeps, and the warm tint in the shadows (per channel at black).
constexpr float kFadeBlack = 0.06f, kFadeWhite = 0.94f, kFadeBow = 0.2f;
constexpr float kFadeSaturation = 0.8f, kFadeWarm = 0.012f;

float smoothstep(float e0, float e1, float x)
{
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void toLinear(const float in[3], float lin[3])
{
    for (int k = 0; k < 3; ++k)
        lin[k] = std::pow(std::clamp(in[k], 0.0f, 1.0f), kGamma);
}

void toEncoded(const float lin[3], float out[3])
{
    for (int k = 0; k < 3; ++k)
        out[k] = std::pow(std::clamp(lin[k], 0.0f, 1.0f), 1.0f / kGamma);
}

float luminance(const float lin[3])
{
    return kLuma[0] * lin[0] + kLuma[1] * lin[1] + kLuma[2] * lin[2];
}

// Hue in degrees (0..360), saturation and value of linear RGB, and back.
void toHsv(const float c[3], float &h, float &s, float &v)
{
    const float mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]});
    const float d = mx - mn;
    v = mx;
    s = mx > 0.0f ? d / mx : 0.0f;
    if (d <= 0.0f) {
        h = 0.0f;
        return;
    }
    if (mx == c[0])
        h = 60.0f * std::fmod((c[1] - c[2]) / d + 6.0f, 6.0f);
    else if (mx == c[1])
        h = 60.0f * ((c[2] - c[0]) / d + 2.0f);
    else
        h = 60.0f * ((c[0] - c[1]) / d + 4.0f);
}

void fromHsv(float h, float s, float v, float c[3])
{
    h = std::fmod(h + 360.0f, 360.0f) / 60.0f;
    for (int k = 0; k < 3; ++k) {
        // r, g, b peak at 0, 120 and 240 degrees.
        const float n = std::fmod(h + (k == 0 ? 5.0f : k == 1 ? 3.0f : 1.0f), 6.0f);
        c[k] = v - v * s * std::clamp(std::min(n, 4.0f - n), 0.0f, 1.0f);
    }
}

// 1 within `half` degrees of `centre` on the hue circle, fading to 0 over the
// next `fade` degrees.
float hueWindow(float h, float centre, float half, float fade)
{
    float d = std::fabs(h - centre);
    d = std::min(d, 360.0f - d);
    return 1.0f - smoothstep(half, half + fade, d);
}

// Teal & orange's hue curve at hue `h` (0..360).
float tealCurve(float h)
{
    constexpr std::size_t n = std::size(kTealCurve);
    if (h <= kTealCurve[0][0] || h >= kTealCurve[n - 1][0])
        return h;
    std::size_t i = 1;
    while (h > kTealCurve[i][0])
        ++i;
    const float t = (h - kTealCurve[i - 1][0]) / (kTealCurve[i][0] - kTealCurve[i - 1][0]);
    return kTealCurve[i - 1][1] + t * (kTealCurve[i][1] - kTealCurve[i - 1][1]);
}

// The signed shortest turn from hue `h` to `to`, in degrees (-180..180).
float hueToward(float h, float to)
{
    return std::remainder(to - h, 360.0f);
}

// Saturation down to `k` (0..1) around the colour's own luminance, so it
// keeps its brightness (and stays in 0..1).
void desaturate(float lin[3], float k)
{
    const float y = luminance(lin);
    for (int c = 0; c < 3; ++c)
        lin[c] = y + (lin[c] - y) * k;
}

// HSV saturation `s` raised by about `k` (exactly k-fold near grey, less
// toward full saturation, never past it): smooth everywhere, so the look
// has no edge where a colour reaches the gamut's.
float moreSaturated(float s, float k)
{
    return 1.0f - std::pow(1.0f - s, k);
}

// A tint direction with no luminance of its own, scaled so its largest
// channel is 1: added to a colour, it changes the hue, not the brightness.
std::array<float, 3> tint(float r, float g, float b)
{
    const float d[3] = {r, g, b};
    const float y = luminance(d);
    std::array<float, 3> t{r - y, g - y, b - y};
    const float m = std::max({std::fabs(t[0]), std::fabs(t[1]), std::fabs(t[2])});
    for (float &x : t)
        x /= m;
    return t;
}

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

struct Preset {
    const char *id;
    const char *title;
    void (*formula)(const float *, float *);
};

const Preset kTable[] = {
    {kBlackWhite, "Black & White", blackWhite},
    {kDeepSky, "Deep sky", deepSky},
    {kTealOrange, "Teal & orange", tealOrange},
    {kFaded, "Faded film", faded},
};
static_assert(std::size(kTable) == kPresets.size());

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

void deepSky(const float in[3], float out[3])
{
    float lin[3], h, s, v;
    toLinear(in, lin);
    toHsv(lin, h, s, v);
    const float a = hueWindow(h, kSkyHue, kSkyHalf, kSkyFade) * smoothstep(kSkySatFrom, kSkySatTo, s);
    fromHsv(h, moreSaturated(s, 1.0f + kSkySaturate * a), v, lin);
    const float dim = std::pow(1.0f - kSkyDarken * a, kGamma);
    for (float &c : lin)
        c *= dim;
    toEncoded(lin, out);
}

void tealOrange(const float in[3], float out[3])
{
    // The hue moves on the encoded picture: closer to how far apart colours
    // look, and smooth where a saturated colour turns -- in linear light a
    // channel near 0 steps hard once encoded.
    float enc[3], lin[3], h, s, v;
    for (int c = 0; c < 3; ++c)
        enc[c] = std::clamp(in[c], 0.0f, 1.0f);
    toHsv(enc, h, s, v);
    const float colourful = smoothstep(kToSatFrom, kToSatTo, s);
    const float skin = colourful * hueWindow(h, kSkinHue, kSkinHalf, kSkinFade);
    h += (tealCurve(h) - h) * colourful + hueToward(h, kOrangeHue) * kSkinPull * skin;
    s = moreSaturated(s, 1.0f + kSkinSaturate * skin);
    fromHsv(h, s, v, enc);
    toLinear(enc, lin);

    // Split tone by the encoded luma: teal toward black, warm toward white.
    static const std::array<float, 3> teal = tint(0.0f, 0.65f, 1.0f), warm = tint(1.0f, 0.6f, 0.0f);
    const float y = std::pow(luminance(lin), 1.0f / kGamma);
    const float sh = (1.0f - y) * (1.0f - y), hi = y * y;
    for (int c = 0; c < 3; ++c)
        lin[c] *= 1.0f + kSplit * (sh * teal[c] + hi * warm[c]);

    // The S-curve on the luma, the colour scaled with it.
    const float l = std::pow(std::max(luminance(lin), 0.0f), 1.0f / kGamma);
    if (l > 0.0f) {
        const float curved = l + kToCurve * (l * l * (3.0f - 2.0f * l) - l);
        const float gain = std::pow(curved / l, kGamma);
        for (float &c : lin)
            c *= gain;
    }

    // The warm highlights and the curve take a bright colour past white; ease
    // its brightest channel toward 1 instead of clipping it (a bright ramp
    // keeps its gradation), the colour scaled with it so it keeps its hue.
    const float m = std::max({lin[0], lin[1], lin[2]});
    if (m > kToKnee) {
        const float room = 1.0f - kToKnee;
        const float soft = kToKnee + room * (1.0f - std::exp(-(m - kToKnee) / room));
        for (float &c : lin)
            c *= soft / m;
    }
    toEncoded(lin, out);
}

void faded(const float in[3], float out[3])
{
    float lin[3];
    toLinear(in, lin);
    const float y = std::pow(luminance(lin), 1.0f / kGamma);
    desaturate(lin, kFadeSaturation);
    toEncoded(lin, out);
    static const std::array<float, 3> warm = tint(1.0f, 0.5f, 0.0f);
    const float sh = (1.0f - y) * (1.0f - y);
    for (int c = 0; c < 3; ++c) {
        const float e = out[c] + kFadeBow * out[c] * (1.0f - out[c]);
        out[c] = std::clamp(kFadeBlack + (kFadeWhite - kFadeBlack) * e + kFadeWarm * sh * warm[c],
                            0.0f, 1.0f);
    }
}

std::shared_ptr<const Lut3D> presetLut(std::string_view id)
{
    // Each built once, thread-safely, on first use.
    static std::array<std::once_flag, std::size(kTable)> once;
    static std::array<std::shared_ptr<const Lut3D>, std::size(kTable)> luts;
    for (std::size_t i = 0; i < std::size(kTable); ++i)
        if (id == kTable[i].id) {
            std::call_once(once[i], [i] { luts[i] = sample(kTable[i].title, kTable[i].formula); });
            return luts[i];
        }
    return nullptr;
}

} // namespace color::look
