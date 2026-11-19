// Colour LUTs: the .cube reader (sizes, domain, comments, what it refuses),
// the trilinear lookup the GPU's sampler matches, the per-path cache, and a
// clip's ColorGrade carrying one (render(), equality), and the colour looks:
// the built-in Black & White and a clip's look in its grade.
// Standalone; the exit code is the failure count.

#include "support/CommaLocale.h"
#include "support/ModelTest.h"

#include "ColorLut.h"
#include "LookPresets.h"

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <string>

namespace {

// A size^3 .cube whose output is f(r, g, b) at each grid point.
template <typename F>
std::string cubeText(int size, F f, const std::string &header = {})
{
    std::string s = "# made by color_lut_test\nTITLE \"test\"\n" + header +
                    "LUT_3D_SIZE " + std::to_string(size) + "\n";
    for (int b = 0; b < size; ++b)
        for (int g = 0; g < size; ++g)
            for (int r = 0; r < size; ++r) {
                float out[3];
                f(r / float(size - 1), g / float(size - 1), b / float(size - 1), out);
                s += std::to_string(out[0]) + " " + std::to_string(out[1]) + " " +
                     std::to_string(out[2]) + "\n";
            }
    return s;
}

bool near(float a, float b, float tol = 1e-4f)
{
    return std::abs(a - b) <= tol;
}

void testParse()
{
    std::string err;
    // Red fastest: entry (r=1, g=0, b=0) is the second line.
    auto lut = color::parseCube(cubeText(2, [](float r, float g, float b, float *o) {
                                    o[0] = r, o[1] = g, o[2] = b;
                                }),
                                &err);
    CHECK(lut && err.empty());
    CHECK(lut->size == 2 && lut->title == "test" && lut->rgb.size() == 8 * 3);
    CHECK(lut->rgb[3] == 1.0f && lut->rgb[4] == 0.0f && lut->rgb[5] == 0.0f);
    CHECK(lut->domainMin == 0.0f && lut->domainMax == 1.0f);

    // CRLF line ends, comments after values, blank lines, keywords we skip.
    std::string crlf = "TITLE \"x\"\r\nLUT_IN_VIDEO_RANGE\r\n\r\nLUT_3D_SIZE 2\r\n";
    for (int i = 0; i < 8; ++i)
        crlf += "0.5 0.5 0.5 # grey\r\n";
    lut = color::parseCube(crlf, &err);
    CHECK(lut && lut->rgb[0] == 0.5f);

    // A domain, the same for all three.
    lut = color::parseCube(cubeText(2, [](float r, float g, float b, float *o) {
                               o[0] = r, o[1] = g, o[2] = b;
                           },
                                    "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n"),
                           &err);
    CHECK(lut && lut->domainMax == 2.0f);

    // Refused, with a reason.
    CHECK(!color::parseCube("LUT_1D_SIZE 4\n0 0 0\n", &err) && err.find("1D") != std::string::npos);
    CHECK(!color::parseCube("TITLE \"empty\"\n", &err) && !err.empty());
    CHECK(!color::parseCube("LUT_3D_SIZE 2\n0 0 0\n", &err) &&
          err.find("expected 8") != std::string::npos);
    CHECK(!color::parseCube("LUT_3D_SIZE 1\n", &err));
    CHECK(!color::parseCube("LUT_3D_SIZE 2\n0 0\n", &err) && err.find("line 2") == 0);
    CHECK(!color::parseCube("0 0 0\nLUT_3D_SIZE 2\n", &err));
    CHECK(!color::parseCube(cubeText(2, [](float, float, float, float *o) { o[0] = o[1] = o[2] = 0; },
                                     "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 2 1\n"),
                            &err) &&
          err.find("DOMAIN") != std::string::npos);
    std::string extra = cubeText(2, [](float, float, float, float *o) { o[0] = o[1] = o[2] = 0; });
    CHECK(!color::parseCube(extra + "0 0 0\n", &err));

    // The file's '.' decimals parse the same under a decimal-comma locale.
    // Literal text: std::to_string would itself write commas there.
    const testlocale::CommaLocale comma;
    if (comma.name().empty()) {
        std::printf("  (no decimal-comma locale installed; skipping the locale check)\n");
        return;
    }
    lut = color::parseCube("DOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.5 1.5 1.5\nLUT_3D_SIZE 2\n"
                           "0.25 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 +1.0\n",
                           &err);
    CHECK(lut && near(lut->rgb[0], 0.25f) && near(lut->domainMax, 1.5f));
}

void testApply()
{
    std::string err;
    // Identity at any size: the lookup gives the input back.
    auto id = color::parseCube(cubeText(5, [](float r, float g, float b, float *o) {
                                   o[0] = r, o[1] = g, o[2] = b;
                               }),
                               &err);
    float in[3] = {0.3f, 0.71f, 0.05f}, out[3];
    id->apply(in, out);
    CHECK(near(out[0], 0.3f) && near(out[1], 0.71f) && near(out[2], 0.05f));
    // Out of range clamps to the edge.
    float hi[3] = {1.5f, -0.2f, 1.0f};
    id->apply(hi, out);
    CHECK(near(out[0], 1.0f) && near(out[1], 0.0f) && near(out[2], 1.0f));

    // Trilinear: a LUT that's linear in its input is exact between points.
    auto lin = color::parseCube(cubeText(3, [](float r, float g, float b, float *o) {
                                    o[0] = 1.0f - r;
                                    o[1] = 0.5f * g + 0.25f * b;
                                    o[2] = (r + g + b) / 3.0f;
                                }),
                                &err);
    lin->apply(in, out);
    CHECK(near(out[0], 0.7f) && near(out[1], 0.5f * 0.71f + 0.25f * 0.05f) &&
          near(out[2], (0.3f + 0.71f + 0.05f) / 3.0f));

    // A domain of [0, 2]: input 1 is the middle of the grid.
    auto wide = color::parseCube(cubeText(3, [](float r, float g, float b, float *o) {
                                     o[0] = r, o[1] = g, o[2] = b;
                                 },
                                          "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n"),
                                 &err);
    float one[3] = {1.0f, 1.0f, 0.5f};
    wide->apply(one, out);
    CHECK(near(out[0], 0.5f) && near(out[2], 0.25f));
}

void testCacheAndGrade()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString path = tmp.path() + "/Invert.cube";
    auto write = [&](float scale) {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(QByteArray::fromStdString(cubeText(2, [scale](float r, float g, float b, float *o) {
            o[0] = (1 - r) * scale, o[1] = (1 - g) * scale, o[2] = (1 - b) * scale;
        })));
    };
    write(1.0f);
    std::string err;
    auto a = color::cachedCube(path.toStdString(), &err);
    CHECK(a && color::cachedCube(path.toStdString()) == a); // read once
    CHECK(!color::cachedCube((tmp.path() + "/none.cube").toStdString(), &err) &&
          err.find("missing") != std::string::npos);

    // A clip's grade: render() loads it; strength becomes the mix.
    ColorGrade g;
    CHECK(g.render().isNeutral());
    g.lut = path;
    g.lutStrength = 40.0f;
    const color::Grade r = g.render();
    CHECK(r.lut == a && near(r.lutMix, 0.4f) && r.hasLut() && !r.isNeutral());
    // Equality: the LUT counts, its strength only with a LUT.
    ColorGrade h = g;
    CHECK(h == g);
    h.lutStrength = 50.0f;
    CHECK(h != g);
    ColorGrade none1, none2;
    none2.lutStrength = 20.0f;
    CHECK(none1 == none2);
    // clamp() keeps the strength in 0..100.
    h.lutStrength = 150.0f;
    h.clamp();
    CHECK(h.lutStrength == 100.0f);
    // A missing file: no LUT applied, and why.
    g.lut = tmp.path() + "/gone.cube";
    std::string why;
    CHECK(!g.render(&why).lut && !why.empty());
}

// Black & White: grey out, black and white kept, brighter in never darker
// out, a blue sky darker than Rec.709's luminance makes it, and the LUT the
// formula is sampled into.
void testBlackWhite()
{
    using color::look::blackWhite;
    CHECK(color::look::isPreset(color::look::kBlackWhite) && !color::look::isPreset("sepia"));
    float out[3];
    const float black[3] = {0, 0, 0}, white[3] = {1, 1, 1};
    blackWhite(black, out);
    CHECK(near(out[0], 0.0f));
    blackWhite(white, out);
    CHECK(near(out[0], 1.0f, 1e-3f));
    const float tints[][3] = {{0.8f, 0.3f, 0.1f}, {0.2f, 0.7f, 0.4f}, {0.3f, 0.5f, 0.9f}};
    for (const auto &t : tints) {
        blackWhite(t, out);
        CHECK(out[0] == out[1] && out[1] == out[2]);
    }
    // Monotonic: more of any channel never makes the grey darker.
    for (int k = 0; k < 3; ++k) {
        float prev = -1.0f;
        for (int i = 0; i <= 100; ++i) {
            float in[3] = {0.4f, 0.4f, 0.4f};
            in[k] = i / 100.0f;
            blackWhite(in, out);
            CHECK(out[0] >= prev);
            prev = out[0];
        }
    }
    // Some contrast: a dark grey darker, a light one lighter.
    const float dark[3] = {0.25f, 0.25f, 0.25f}, light[3] = {0.75f, 0.75f, 0.75f};
    blackWhite(dark, out);
    CHECK(out[0] < 0.25f);
    blackWhite(light, out);
    CHECK(out[0] > 0.75f);
    // A blue sky against the grey of the same Rec.709 luminance.
    const float sky[3] = {0.35f, 0.55f, 0.9f};
    float y709 = 0.0f;
    const float w709[3] = {0.2126f, 0.7152f, 0.0722f};
    for (int k = 0; k < 3; ++k)
        y709 += w709[k] * std::pow(sky[k], 2.4f);
    const float g = std::pow(y709, 1.0f / 2.4f);
    const float grey[3] = {g, g, g};
    float skyOut[3], greyOut[3];
    blackWhite(sky, skyOut);
    blackWhite(grey, greyOut);
    CHECK(skyOut[0] < greyOut[0] - 0.01f);

    // The LUT: built once, the formula at its grid points, close between.
    const auto lut = color::look::presetLut(color::look::kBlackWhite);
    CHECK(lut && lut->size == 33 && lut->rgb.size() == 33u * 33u * 33u * 3u);
    CHECK(color::look::presetLut(color::look::kBlackWhite) == lut);
    CHECK(!color::look::presetLut("sepia"));
    for (const auto &t : tints) {
        float a[3], b[3];
        blackWhite(t, a);
        lut->apply(t, b);
        CHECK(near(a[0], b[0], 2e-3f) && near(b[0], b[1]) && near(b[1], b[2]));
    }
    const float grid[3] = {16 / 32.0f, 8 / 32.0f, 24 / 32.0f};
    float a[3], b[3];
    blackWhite(grid, a);
    lut->apply(grid, b);
    CHECK(near(a[0], b[0], 1e-5f));
}

// Hue in degrees and HSV saturation of an encoded colour, for the looks'
// checks.
void hueSat(const float c[3], float &h, float &s)
{
    const float mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]});
    const float d = mx - mn;
    s = mx > 0.0f ? d / mx : 0.0f;
    if (d <= 0.0f)
        h = 0.0f;
    else if (mx == c[0])
        h = 60.0f * std::fmod((c[1] - c[2]) / d + 6.0f, 6.0f);
    else if (mx == c[1])
        h = 60.0f * ((c[2] - c[0]) / d + 2.0f);
    else
        h = 60.0f * ((c[0] - c[1]) / d + 4.0f);
}

float hueDistance(float a, float b)
{
    const float d = std::fabs(a - b);
    return std::min(d, 360.0f - d);
}

float luma709(const float c[3])
{
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

// Every built-in look: known by its id, a 33-point LUT built once and
// shared, its formula at the grid points, the output in 0..1 and no jump
// between neighbouring entries (a smooth look under trilinear filtering).
void testPresets()
{
    using namespace color::look;
    CHECK(kPresets.size() == 4);
    const struct {
        const char *id;
        void (*formula)(const float *, float *);
    } looks[] = {{kBlackWhite, blackWhite}, {kDeepSky, deepSky}, {kTealOrange, tealOrange},
                 {kFaded, faded}};
    for (const auto &l : looks) {
        CHECK(isPreset(l.id));
        const auto lut = presetLut(l.id);
        const bool shaped = lut && lut->size == 33 && lut->rgb.size() == 33u * 33u * 33u * 3u;
        CHECK(shaped);
        if (!shaped) // the scan below reads it as a 33-point cube
            continue;
        CHECK(presetLut(l.id) == lut && !lut->title.empty());
        float worst = 0.0f;
        bool inRange = true;
        const auto at = [&](int r, int g, int b, int c) {
            return lut->rgb[((std::size_t(b) * 33 + g) * 33 + r) * 3 + c];
        };
        for (int b = 0; b < 33; ++b)
            for (int g = 0; g < 33; ++g)
                for (int r = 0; r < 33; ++r)
                    for (int c = 0; c < 3; ++c) {
                        const float v = at(r, g, b, c);
                        inRange = inRange && v >= 0.0f && v <= 1.0f;
                        if (r)
                            worst = std::max(worst, std::fabs(v - at(r - 1, g, b, c)));
                        if (g)
                            worst = std::max(worst, std::fabs(v - at(r, g - 1, b, c)));
                        if (b)
                            worst = std::max(worst, std::fabs(v - at(r, g, b - 1, c)));
                    }
        CHECK(inRange);
        // A grid step is 1/32 (0.031); no look is steeper than ~3x anywhere.
        CHECK(worst < 0.1f);
        const float grid[3] = {16 / 32.0f, 8 / 32.0f, 24 / 32.0f};
        float a[3], b[3];
        l.formula(grid, a);
        lut->apply(grid, b);
        for (int k = 0; k < 3; ++k)
            CHECK(near(a[k], b[k], 1e-5f));
    }
}

// Deep sky: a blue sky more saturated and darker, greys, white clouds, reds,
// skin and greens (nearly) as they were.
void testDeepSky()
{
    using color::look::deepSky;
    float out[3], h0, s0, h1, s1;
    const float skies[][3] = {{0.35f, 0.55f, 0.9f}, {0.55f, 0.7f, 0.9f}, {0.4f, 0.7f, 0.85f}};
    for (const auto &sky : skies) {
        deepSky(sky, out);
        hueSat(sky, h0, s0);
        hueSat(out, h1, s1);
        CHECK(s1 > s0 + 0.02f && luma709(out) < luma709(sky) - 0.02f && hueDistance(h0, h1) < 5.0f);
    }
    const float keep[][3] = {{0.5f, 0.5f, 0.5f},   {1, 1, 1},          {0.9f, 0.92f, 0.96f},
                             {0.8f, 0.2f, 0.15f},  {0.85f, 0.6f, 0.5f}, {0.3f, 0.55f, 0.2f},
                             {0.05f, 0.05f, 0.05f}};
    for (const auto &c : keep) {
        deepSky(c, out);
        for (int k = 0; k < 3; ++k)
            CHECK(near(out[k], c[k], 0.01f));
    }
}

// Teal & orange: a green-blue moves toward teal, a skin tone keeps its hue
// and gains a little saturation, black and white stay put.
void testTealOrange()
{
    using color::look::tealOrange;
    float out[3], h0, s0, h1, s1;
    const float foliage[3] = {0.3f, 0.55f, 0.35f}, sky[3] = {0.35f, 0.55f, 0.9f};
    for (const auto *c : {foliage, sky}) {
        tealOrange(c, out);
        hueSat(c, h0, s0);
        hueSat(out, h1, s1);
        CHECK(hueDistance(h1, 185.0f) < hueDistance(h0, 185.0f) - 5.0f);
    }
    const float skins[][3] = {{0.85f, 0.6f, 0.5f}, {0.6f, 0.4f, 0.3f}, {0.9f, 0.72f, 0.62f}};
    for (const auto &skin : skins) {
        tealOrange(skin, out);
        hueSat(skin, h0, s0);
        hueSat(out, h1, s1);
        CHECK(hueDistance(h0, h1) < 4.0f && s1 > s0);
    }
    const float black[3] = {0, 0, 0}, white[3] = {1, 1, 1};
    tealOrange(black, out);
    CHECK(near(out[0], 0.0f) && near(out[1], 0.0f) && near(out[2], 0.0f));
    tealOrange(white, out);
    CHECK(out[0] > 0.97f && out[1] > 0.97f && out[2] > 0.9f);
    // A bright saturated ramp keeps its gradation to the end: the warm
    // highlights and the curve roll off into white rather than clip.
    float prev = -1.0f;
    for (int i = 26; i <= 32; ++i) {
        const float ramp[3] = {i / 32.0f, 0.59375f, 0.96875f};
        tealOrange(ramp, out);
        CHECK(out[0] > prev + 0.002f);
        prev = out[0];
    }
    // The split tone: a dark grey a little teal, a light one a little warm.
    const float dark[3] = {0.2f, 0.2f, 0.2f}, light[3] = {0.8f, 0.8f, 0.8f};
    tealOrange(dark, out);
    CHECK(out[2] > out[0] && out[0] < 0.2f);
    tealOrange(light, out);
    CHECK(out[0] > out[2] && out[0] > 0.8f);
}

// Faded film: black lifted to ~0.06, white down to ~0.94, a mid grey still
// grey, colours muted.
void testFaded()
{
    using color::look::faded;
    float out[3], h0, s0, h1, s1;
    const float black[3] = {0, 0, 0}, white[3] = {1, 1, 1}, grey[3] = {0.5f, 0.5f, 0.5f};
    faded(black, out);
    for (int k = 0; k < 3; ++k)
        CHECK(near(out[k], 0.06f, 0.015f));
    CHECK(out[0] > out[2]); // warm
    faded(white, out);
    for (int k = 0; k < 3; ++k)
        CHECK(near(out[k], 0.94f, 1e-3f));
    faded(grey, out);
    CHECK(near(out[0], out[1], 0.01f) && near(out[1], out[2], 0.01f));
    const float red[3] = {0.8f, 0.2f, 0.15f};
    faded(red, out);
    hueSat(red, h0, s0);
    hueSat(out, h1, s1);
    CHECK(s1 < s0 - 0.1f && hueDistance(h0, h1) < 6.0f);
}

// A clip's look in what the renderers get: a built-in one or a file, its
// strength as the mix, none when off; a later version's look or a missing
// file left out with why.
void testLookGrade()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString path = tmp.path() + "/Warm.cube";
    {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(QByteArray::fromStdString(cubeText(2, [](float r, float g, float b, float *o) {
            o[0] = r, o[1] = g, o[2] = b * 0.5f;
        })));
    }
    ClipItem c;
    CHECK(!c.renderGrade().hasLook() && c.renderGrade().isNeutral());
    CHECK(c.effects.add(fx::kLook) && c.effects.look->preset == color::look::kBlackWhite);
    color::Grade g = c.renderGrade();
    CHECK(g.look == color::look::presetLut(color::look::kBlackWhite) && near(g.lookMix, 1.0f));
    // The shaders draw it in their effects build: its mix and domain ride in
    // the effects.
    CHECK(g.hasLook() && !g.isNeutral() && g.hasEffects());
    CHECK(g.effects.look == (std::array<float, 4>{1.0f, 0.0f, 1.0f, 0.0f}));
    c.effects.look->strength = 40.0f;
    CHECK(near(c.renderGrade().lookMix, 0.4f) && c.renderGrade() != g);
    c.effects.look->lut = path.toStdString(); // a file wins over the preset
    CHECK(c.renderGrade().look == color::cachedCube(path.toStdString()));
    c.effects.look->on = false;
    CHECK(!c.renderGrade().look && !c.renderGrade().hasEffects());
    c.effects.look->on = true;
    c.effects.look->strength = 0.0f;
    CHECK(!c.renderGrade().look);
    c.effects.look->strength = 150.0f;
    c.effects.clamp();
    CHECK(c.effects.look->strength == 100.0f);
    std::string why;
    c.effects.look->lut = (tmp.path() + "/gone.cube").toStdString();
    CHECK(!c.renderGrade(nullptr, &why).look && !why.empty());
    why.clear();
    c.effects.look->lut.clear();
    c.effects.look->preset = "from-a-later-version";
    CHECK(!c.renderGrade(nullptr, &why).look && why.find("from-a-later-version") != std::string::npos);
    // has / remove, like the other effects.
    CHECK(c.effects.has(fx::kLook) && !c.effects.add(fx::kLook));
    c.effects.remove(fx::kLook);
    CHECK(!c.effects.look && c.effects.isEmpty());
}

} // namespace

int main()
{
    testParse();
    testApply();
    testCacheAndGrade();
    testBlackWhite();
    testPresets();
    testDeepSky();
    testTealOrange();
    testFaded();
    testLookGrade();
    return modeltest::finish("color_lut_test");
}
