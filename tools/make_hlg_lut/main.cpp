// Writes the HLG -> Rec.709 LUT the app ships (resources/luts/), from the
// ITU-R BT.2100 / BT.2408 formulas. HLG footage (e.g. the Insta360 X6's
// Dolby Vision mode) looks dim and flat as SDR; like a camera maker's log
// LUT, this turns it into normal Rec.709 colours, in the Colour panel where
// the user can change or remove it.
//
// For each grid point (an HLG R'G'B' signal, BT.2020, 0..1):
//   1. the HLG inverse OETF to scene light (BT.2100);
//   2. the HLG OOTF for the 1000 cd/m2 reference display (system gamma 1.2):
//      display light, relative to the HDR reference white of 203 cd/m2
//      (BT.2408; HLG 75 %, a white card);
//   3. BT.2020 -> BT.709 primaries (BT.2087), negatives clipped (colours
//      outside BT.709);
//   4. a highlight roll-off on the largest of R, G, B (hue kept): linear up to
//      kKnee of SDR white, then a smooth curve that reaches exactly 1.0 at the
//      brightest colour the grid holds, so nothing clips. The scale is set so
//      the reference white lands at kRefWhite (display light);
//   5. Rec.709 display encoding (inverse BT.1886, gamma 2.4).
//
// usage: make_hlg_lut [--size N] OUT.cube   write it (default N = 33)
//        make_hlg_lut --check FILE.cube     exit 1 unless FILE is what this
//                                           writes (ctest keeps the shipped
//                                           file in step with this source)
// Both print the curve at a few known signals.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using Rgb = std::array<double, 3>;

// HLG reference white (BT.2408) on the 1000 cd/m2 reference display.
constexpr double kPeakNits = 1000.0;
constexpr double kRefWhiteNits = 203.0;
constexpr double kSystemGamma = 1.2; // BT.2100 at 1000 cd/m2
// The SDR picture, in display light (0..1 = black..white): where the HDR
// reference white goes (0.8 = Rec.709 signal 0.91), and where the roll-off
// starts.
constexpr double kRefWhite = 0.8;
constexpr double kKnee = 0.6;

// BT.2100 HLG inverse OETF: signal 0..1 -> scene light 0..1.
double hlgToScene(double v)
{
    constexpr double a = 0.17883277, b = 0.28466892, c = 0.55991073;
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.5 ? v * v / 3.0 : (std::exp((v - c) / a) + b) / 12.0;
}

// Steps 1-3: an HLG signal to BT.709 display light, 1 = the HDR reference white.
Rgb hdrDisplay709(const Rgb& hlg)
{
    const Rgb e = {hlgToScene(hlg[0]), hlgToScene(hlg[1]), hlgToScene(hlg[2])};
    const double ys = 0.2627 * e[0] + 0.6780 * e[1] + 0.0593 * e[2];
    const double k = ys > 0.0 ? kPeakNits * std::pow(ys, kSystemGamma - 1.0) / kRefWhiteNits : 0.0;
    const Rgb d = {k * e[0], k * e[1], k * e[2]};
    static constexpr double m[3][3] = {{1.6605, -0.5876, -0.0728},
                                       {-0.1246, 1.1329, -0.0083},
                                       {-0.0182, -0.1006, 1.1187}};
    Rgb out;
    for (int i = 0; i < 3; ++i)
        out[i] = std::max(0.0, m[i][0] * d[0] + m[i][1] * d[1] + m[i][2] * d[2]);
    return out;
}

// Step 4's curve: x (scaled display light) -> 0..1. Linear to the knee, then
// (1 + p) t / (1 + p t) over the rest: concave, the same slope at the knee,
// exactly 1 at x = top.
double rollOff(double x, double top)
{
    if (x <= kKnee)
        return x;
    const double span = top - kKnee;
    const double t = std::min((x - kKnee) / span, 1.0);
    const double p = span / (1.0 - kKnee) - 1.0;
    return kKnee + (1.0 - kKnee) * (1.0 + p) * t / (1.0 + p * t);
}

struct Curve {
    double scale = 1.0; // display light (1 = reference white) -> before the roll-off
    double top = 1.0;   // the largest scaled channel the grid can give
};

// The scale that puts the reference white at kRefWhite, given the grid's
// brightest channel.
Curve fitCurve(double maxChannel)
{
    double lo = kKnee, hi = 1.0; // the reference white, scaled, sits in the knee
    Curve c;
    for (int i = 0; i < 100; ++i) {
        c.scale = 0.5 * (lo + hi);
        c.top = c.scale * maxChannel;
        (rollOff(c.scale, c.top) < kRefWhite ? lo : hi) = c.scale;
    }
    return c;
}

Rgb toRec709(const Rgb& hlg, const Curve& c)
{
    Rgb d = hdrDisplay709(hlg);
    const double m = std::max({d[0], d[1], d[2]});
    if (m > 0.0) {
        const double g = rollOff(c.scale * m, c.top) / m;
        for (double& v : d)
            v *= g;
    }
    for (double& v : d)
        v = std::pow(std::clamp(v, 0.0, 1.0), 1.0 / 2.4);
    return d;
}

std::string cube(int n, Curve& curve)
{
    double maxChannel = 0.0;
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                const Rgb d = hdrDisplay709({r / (n - 1.0), g / (n - 1.0), b / (n - 1.0)});
                maxChannel = std::max({maxChannel, d[0], d[1], d[2]});
            }
    curve = fitCurve(maxChannel);

    std::ostringstream s;
    s << "# HLG (BT.2100, BT.2020) to Rec.709 (BT.709, gamma 2.4), made by\n"
         "# tools/make_hlg_lut in orbis-reframe; see its source for the steps.\n"
         "# HDR reference white (HLG 75 %) -> Rec.709 display light "
      << kRefWhite << ", roll-off from " << kKnee << ".\n"
      << "TITLE \"HLG to Rec.709\"\n"
      << "LUT_3D_SIZE " << n << "\n";
    char line[64];
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                const Rgb o = toRec709({r / (n - 1.0), g / (n - 1.0), b / (n - 1.0)}, curve);
                std::snprintf(line, sizeof line, "%.6f %.6f %.6f\n", o[0], o[1], o[2]);
                s << line;
            }
    return s.str();
}

void report(const Curve& c)
{
    struct Point { const char* name; double hlg; };
    for (const Point p : {Point{"black", 0.0}, Point{"18 % grey card (HLG 38 %)", 0.38},
                          Point{"reference white (HLG 75 %)", 0.75}, Point{"HLG 90 %", 0.9},
                          Point{"HLG peak", 1.0}})
        std::printf("  %-28s -> Rec.709 %.3f\n", p.name, toRec709({p.hlg, p.hlg, p.hlg}, c)[0]);
}

int usage()
{
    std::fprintf(stderr, "usage: make_hlg_lut [--size N] OUT.cube\n"
                         "       make_hlg_lut --check FILE.cube\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    int n = 33;
    bool check = false;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--size") && i + 1 < argc)
            n = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--check"))
            check = true;
        else if (argv[i][0] == '-' || path)
            return usage();
        else
            path = argv[i];
    }
    if (!path || n < 2 || n > 256)
        return usage();

    Curve curve;
    const std::string text = cube(n, curve);
    report(curve);
    if (check) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream have;
        have << in.rdbuf();
        if (!in || have.str() != text) {
            std::fprintf(stderr, "%s isn't what make_hlg_lut writes: regenerate it\n", path);
            return 1;
        }
        std::printf("%s is up to date\n", path);
        return 0;
    }
    std::ofstream out(path, std::ios::binary);
    out << text;
    if (!out) {
        std::fprintf(stderr, "can't write %s\n", path);
        return 1;
    }
    std::printf("wrote %s (%d^3)\n", path, n);
    return 0;
}
