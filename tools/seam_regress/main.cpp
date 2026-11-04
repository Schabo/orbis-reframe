// seam_regress -- the stitch's seam on a fixed set of frames, measured the
// same way every time, so a seam change can be checked against the last run.
//
//   seam_regress LIST --out DIR [--baseline DIR] [--only TEXT] [--width N]
//                [--warmup S] [--eq-width N] [--no-cpu] [--no-images] [--live]
//
// LIST has one frame per line, tab-separated: a name (the output files'),
// the clip, the time in seconds, then any of
//   guard=none|standard|premium       (default premium, as the app)
//   method=depth|new                  (the stitcher: the depth-aware seam,
//                                      the default, or the new stitcher with
//                                      its seam colour blend and seam flow,
//                                      which takes no seam correction, lens
//                                      colour or seam analysis)
//   seam=Y,P,R,CU,CV,S                (the clip's seam correction; measured
//                                      over the whole clip as the app does
//                                      when not given)
//   colour=R,G,B,SR,SG,SB,SPREAD      (its lens colour; likewise)
//   ref=FILE.ppm refrot=Z,Y,X         (a reference equirect of the same
//                                      frame, e.g. Insta360 Studio's, and the
//                                      rotation from ours to it)
// and anything after a '#' is a note. Lines starting with '#' are comments.
//
// Per frame the seam of the stretch from --warmup seconds before it is
// analysed (stitch::AnalyseClipSeam, as the app does after opening a clip;
// --live and the new stitcher leave it out), then the clip is stitched through
// stitch::NativeStitchSource three times: on the GPU played into from the
// start of the stretch (as playback reaches it), on the GPU seeked to cold
// (as a step back, a screenshot or an export's first frame), and on the CPU
// fallback, cold. Written to DIR:
//   NAME.json         the measurements (below); "summary" holds the scalars
//                     --baseline compares
//   NAME_strip.ppm    the seam unwrapped, +-12 deg across it, 20 px/deg, five
//                     rows: lens 0, lens 1 (as decoded, each lined up by the
//                     depth table), the played frame, |lens 0 - lens 1| x 4
//                     (after the colour correction), the cold frame
//   NAME_eq.ppm       the played frame, --eq-width wide
// Measured: the lenses' offset left after the depth table per 5 deg block
// (an NCC search, deg; far blocks are those the table puts at infinity), the
// depth table and its near bins, the colour difference where the lenses meet
// per channel (before and after the correction; for the new stitcher on the
// seam circle at the template's geometry, with its seam colour blend --
// the seam flow moves no colour), cold against played (table,
// PSNR in the band and whole), CPU against GPU (cold), the frame-to-frame
// change over the played run's last second in the band and beside it, and
// with a reference, PSNR against it.

#include <ClipLensColour.h>
#include <ClipMeasure.h>
#include <ClipSeam.h>
#include <ClipSeamAnalysis.h>
#include <NativeStitchSource.h>

#include <FisheyeProjection.h>
#include <Image.h>
#include <InsvDemux.h>
#include <LensColour.h>
#include <SeamCut.h>
#include <SeamDepth.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;

const char kUsage[] =
    "usage: seam_regress LIST --out DIR [--baseline DIR] [--only TEXT] [--width N]\n"
    "       [--warmup S] [--eq-width N] [--no-cpu] [--no-images] [--live]\n"
    "  LIST: name<TAB>clip<TAB>seconds[<TAB>guard=..|method=..|seam=..|colour=..|ref=..|refrot=..]...\n"
    "        [# note]\n";

// Runs fn(i) for i in [0, n) on all cores.
void ParallelFor(int n, const std::function<void(int)>& fn)
{
    const int threads = std::max(1, std::min<int>(n, std::thread::hardware_concurrency()));
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (int i = next++; i < n; i = next++)
                fn(i);
        });
    for (auto& t : pool)
        t.join();
}

struct Entry {
    std::string name, clip, note;
    int64_t timeMs = 0;
    stitch::PreviewStitchConfig::LensGuard guard = stitch::PreviewStitchConfig::LensGuard::Premium;
    bool newStitcher = false;
    std::optional<insv::SeamCorrection> seam;
    std::optional<insv::LensColour> colour;
    std::string ref;
    double refRotDeg[3] = {0, 0, 0}; // z, y, x
};

bool ParseList(const std::string& path, std::vector<Entry>& out, std::string& err)
{
    std::ifstream in(path);
    if (!in) {
        err = "can't read " + path;
        return false;
    }
    std::string line;
    int no = 0;
    while (std::getline(in, line)) {
        ++no;
        const auto hash = line.find('#');
        Entry e;
        if (hash != std::string::npos) {
            e.note = line.substr(hash + 1);
            e.note.erase(0, e.note.find_first_not_of(" \t"));
            line.erase(hash);
        }
        std::vector<std::string> f;
        std::stringstream ss(line);
        for (std::string tok; std::getline(ss, tok, '\t');) {
            tok.erase(0, tok.find_first_not_of(' '));
            tok.erase(tok.find_last_not_of(' ') + 1);
            if (!tok.empty())
                f.push_back(tok);
        }
        if (f.empty())
            continue;
        const std::string where = path + ":" + std::to_string(no) + ": ";
        if (f.size() < 3) {
            err = where + "want name, clip and seconds, tab-separated";
            return false;
        }
        e.name = f[0];
        e.clip = f[1];
        e.timeMs = std::llround(std::atof(f[2].c_str()) * 1000.0);
        for (size_t k = 3; k < f.size(); ++k) {
            const auto eq = f[k].find('=');
            const std::string key = f[k].substr(0, eq), v = eq == std::string::npos ? "" : f[k].substr(eq + 1);
            if (key == "guard") {
                using G = stitch::PreviewStitchConfig::LensGuard;
                e.guard = v == "none" ? G::None : v == "standard" ? G::Standard : G::Premium;
            } else if (key == "method") {
                if (v != "depth" && v != "new") {
                    err = where + "method= wants depth or new";
                    return false;
                }
                e.newStitcher = v == "new";
            } else if (key == "seam") {
                double a[6] = {0, 0, 0, 0, 0, 0};
                if (std::sscanf(v.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf", &a[0], &a[1], &a[2], &a[3],
                                &a[4], &a[5]) != 6) {
                    err = where + "seam= wants Y,P,R,CU,CV,S";
                    return false;
                }
                insv::SeamCorrection s;
                for (int i = 0; i < 3; ++i)
                    s.lens1RotDeg[i] = a[i];
                s.lens1Centre[0] = a[3] * 0.01;
                s.lens1Centre[1] = a[4] * 0.01;
                s.radialScale = 1.0 + a[5] * 0.01;
                e.seam = s;
            } else if (key == "colour") {
                double a[7] = {0, 0, 0, 0, 0, 0, 0};
                if (std::sscanf(v.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf,%lf", &a[0], &a[1], &a[2],
                                &a[3], &a[4], &a[5], &a[6]) != 7) {
                    err = where + "colour= wants R,G,B,SR,SG,SB,SPREAD";
                    return false;
                }
                insv::LensColour c;
                for (int i = 0; i < 3; ++i) {
                    c.logRatio[i] = std::log1p(a[i] * 0.01);
                    c.slope[i] = a[3 + i] * 0.01;
                }
                c.spread = a[6];
                e.colour = c;
            } else if (key == "ref") {
                e.ref = v;
            } else if (key == "refrot") {
                if (std::sscanf(v.c_str(), "%lf,%lf,%lf", &e.refRotDeg[0], &e.refRotDeg[1],
                                &e.refRotDeg[2]) != 3) {
                    err = where + "refrot= wants Z,Y,X";
                    return false;
                }
            } else {
                err = where + "unknown field " + f[k];
                return false;
            }
        }
        out.push_back(e);
    }
    return true;
}

// --- images -------------------------------------------------------------

struct Rgb {
    int w = 0, h = 0;
    std::vector<uint8_t> px; // RGB
    void alloc(int W, int H)
    {
        w = W;
        h = H;
        px.assign(size_t(W) * H * 3, 0);
    }
    uint8_t* at(int x, int y) { return &px[(size_t(y) * w + x) * 3]; }
    const uint8_t* at(int x, int y) const { return &px[(size_t(y) * w + x) * 3]; }
};

Rgb FromFrame(const stitch::Frame& f)
{
    Rgb r;
    r.alloc(f.width, f.height);
    for (size_t i = 0, n = size_t(f.width) * f.height; i < n; ++i)
        std::memcpy(&r.px[i * 3], &f.rgba[i * 4], 3);
    return r;
}

bool WritePpm(const std::string& path, const Rgb& im)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    std::fprintf(f, "P6\n%d %d\n255\n", im.w, im.h);
    std::fwrite(im.px.data(), 1, im.px.size(), f);
    std::fclose(f);
    return true;
}

bool ReadPpm(const std::string& path, Rgb& im, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    std::string magic;
    int w = 0, h = 0, maxv = 0;
    in >> magic >> w >> h >> maxv;
    in.get();
    if (!in || magic != "P6" || w <= 0 || h <= 0 || maxv != 255) {
        err = path + ": not an 8-bit binary PPM (P6)";
        return false;
    }
    im.alloc(w, h);
    in.read(reinterpret_cast<char*>(im.px.data()), std::streamsize(im.px.size()));
    if (!in) {
        err = path + ": short read";
        return false;
    }
    return true;
}

// Bilinear sample of an equirect at longitude / latitude (radians, the
// stitch's convention: x = 0 at -pi, y = 0 at +pi/2), wrapping in x.
void SampleEquirect(const Rgb& im, double lon, double lat, double out[3])
{
    const double fx = (lon / (2 * kPi) + 0.5) * im.w - 0.5;
    const double fy = (0.5 - lat / kPi) * im.h - 0.5;
    const int x0 = int(std::floor(fx)), y0 = std::clamp(int(std::floor(fy)), 0, im.h - 1);
    const int y1 = std::min(y0 + 1, im.h - 1);
    const double tx = fx - std::floor(fx), ty = std::clamp(fy - y0, 0.0, 1.0);
    const int xa = ((x0 % im.w) + im.w) % im.w, xb = (xa + 1) % im.w;
    for (int c = 0; c < 3; ++c) {
        const double a = im.at(xa, y0)[c] * (1 - tx) + im.at(xb, y0)[c] * tx;
        const double b = im.at(xa, y1)[c] * (1 - tx) + im.at(xb, y1)[c] * tx;
        out[c] = a * (1 - ty) + b * ty;
    }
}

// An equirect pixel's direction on the sphere (renderEquirect's).
insv::Vec3 PixelSphere(int x, int y, int W, int H)
{
    const double lat = (0.5 - (y + 0.5) / H) * kPi;
    const double lon = ((x + 0.5) / W - 0.5) * 2.0 * kPi;
    return {std::cos(lat) * std::sin(lon), std::sin(lat), -std::cos(lat) * std::cos(lon)};
}

void SphereLonLat(const insv::Vec3& s, double& lon, double& lat)
{
    lon = std::atan2(s.x, -s.z);
    lat = std::asin(std::clamp(s.y, -1.0, 1.0));
}

// Area average down by an integer factor, else bilinear.
Rgb Downscale(const Rgb& im, int outW)
{
    if (outW <= 0 || outW >= im.w)
        return im;
    Rgb o;
    o.alloc(outW, outW / 2);
    const int f = im.w / outW;
    const bool area = f * outW == im.w && f * (outW / 2) == im.h;
    ParallelFor(o.h, [&](int y) {
        for (int x = 0; x < o.w; ++x) {
            double s[3] = {0, 0, 0};
            if (area) {
                for (int dy = 0; dy < f; ++dy)
                    for (int dx = 0; dx < f; ++dx)
                        for (int c = 0; c < 3; ++c)
                            s[c] += im.at(x * f + dx, y * f + dy)[c];
                for (int c = 0; c < 3; ++c)
                    s[c] /= f * f;
            } else {
                double lon = ((x + 0.5) / o.w - 0.5) * 2 * kPi, lat = (0.5 - (y + 0.5) / o.h) * kPi;
                SampleEquirect(im, lon, lat, s);
            }
            for (int c = 0; c < 3; ++c)
                o.at(x, y)[c] = uint8_t(std::lround(std::clamp(s[c], 0.0, 255.0)));
        }
    });
    return o;
}

double Psnr(double sse, double n)
{
    if (n <= 0)
        return NAN;
    const double mse = sse / n;
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// --- the stitch geometry ---------------------------------------------------

struct Geometry {
    insv::FisheyeStitcher st;
    double bandRad = 0;

    insv::Vec3 toSphere(const insv::Vec3& world) const
    {
        const insv::Mat3& R = st.outRotation(); // world = R * sphere
        return {R.m[0][0] * world.x + R.m[1][0] * world.y + R.m[2][0] * world.z,
                R.m[0][1] * world.x + R.m[1][1] * world.y + R.m[2][1] * world.z,
                R.m[0][2] * world.x + R.m[1][2] * world.y + R.m[2][2] * world.z};
    }
    // Elevation off the seam circle of an equirect pixel, radians.
    double pixelSeamE(int x, int y, int W, int H) const
    {
        double e, t;
        int i0, i1;
        st.seamPosition(insv::apply(st.outRotation(), PixelSphere(x, y, W, H)), e, i0, i1, t);
        return e;
    }
    void sampleWorld(const Rgb& eq, const insv::Vec3& world, double out[3]) const
    {
        double lon, lat;
        SphereLonLat(toSphere(world), lon, lat);
        SampleEquirect(eq, lon, lat, out);
    }
};

// PSNR of b against a over the whole frame and over the band (|e| <= the
// blend's half-width). Same size.
void ComparePsnr(const Geometry& g, const Rgb& a, const Rgb& b, double& whole, double& band)
{
    std::vector<double> sw(a.h, 0), sb(a.h, 0), nb(a.h, 0);
    ParallelFor(a.h, [&](int y) {
        for (int x = 0; x < a.w; ++x) {
            double d = 0;
            for (int c = 0; c < 3; ++c) {
                const double v = double(a.at(x, y)[c]) - b.at(x, y)[c];
                d += v * v;
            }
            sw[y] += d;
            if (std::fabs(g.pixelSeamE(x, y, a.w, a.h)) <= g.bandRad) {
                sb[y] += d;
                nb[y] += 3;
            }
        }
    });
    double W = 0, B = 0, N = 0;
    for (int y = 0; y < a.h; ++y) {
        W += sw[y];
        B += sb[y];
        N += nb[y];
    }
    whole = Psnr(W, 3.0 * a.w * a.h);
    band = Psnr(B, N);
}

// --- the seam strip and what's measured on it ------------------------------

constexpr double kStripPxDeg = 20.0;
constexpr double kStripElDeg = 12.0;

// How a stitcher treats each lens at a direction, as
// FisheyeStitcher::renderEquirect does: its log gains and veiling light
// (lensLogGains' layout), where the lenses meet per bin, and the depth
// table the lenses are lined up by. The depth-aware seam: its blend at its
// cut. The new stitcher: its seam colour blend's table on the seam circle,
// at the template's geometry.
struct Treatment {
    const insv::FisheyeStitcher* st = nullptr;
    const insv::SeamBlend* blend = nullptr; // the depth-aware seam's
    const float* seamColour = nullptr;      // the new stitcher's (StitchGpuParams::seamColour)
    const float* q = nullptr;

    void gains(const insv::Vec3& d, double lg[2][3], double veil[2][3]) const
    {
        st->lensLogGains(d, blend, lg, veil);
        if (seamColour) {
            double L[3];
            st->seamColourLog(d, seamColour, L);
            for (int k = 0; k < 3; ++k) {
                lg[0][k] -= 0.5 * L[k];
                lg[1][k] += 0.5 * L[k];
            }
        }
    }
    double cutRad(int bin) const { return blend ? blend->cut[bin] : 0.0; }
};

struct LensStrips {
    int W = 0, H = 0;
    std::vector<float> raw[2];     // RGB as decoded, -1 = the lens doesn't see it
    std::vector<float> treated[2]; // after the veiling light and colour correction
};

LensStrips MakeLensStrips(const Geometry& g, const insv::Image (&lens)[2], const Treatment& tr)
{
    LensStrips s;
    s.W = int(360 * kStripPxDeg);
    s.H = int(2 * kStripElDeg * kStripPxDeg);
    for (int i = 0; i < 2; ++i) {
        s.raw[i].assign(size_t(s.W) * s.H * 3, -1.f);
        s.treated[i].assign(size_t(s.W) * s.H * 3, -1.f);
    }
    ParallelFor(s.H, [&](int y) {
        const double e = (kStripElDeg - (y + 0.5) / kStripPxDeg) * kDeg;
        for (int x = 0; x < s.W; ++x) {
            const double phi = (x + 0.5) / kStripPxDeg * kDeg;
            const insv::Vec3 d = g.st.seamDir(phi, e);
            const double qq = g.st.seamQAt(d, tr.q);
            double lg[2][3], veil[2][3];
            tr.gains(d, lg, veil);
            for (int i = 0; i < 2; ++i) {
                double u, v, th;
                if (!g.st.project(i, g.st.lensRay(i, d, qq), u, v, th) || u < 0 || v < 0 ||
                    u > lens[i].w - 1 || v > lens[i].h - 1)
                    continue;
                const auto c = lens[i].sample(u, v);
                const size_t o = (size_t(y) * s.W + x) * 3;
                for (int k = 0; k < 3; ++k) {
                    s.raw[i][o + k] = c[k];
                    const double r = insv::LensVeilRemove(c[k], veil[i][k]);
                    s.treated[i][o + k] = float(insv::LensColourGain(lg[i][k], r) * r);
                }
            }
        }
    });
    return s;
}

// The equirect resampled on the strip's grid.
std::vector<float> EquirectStrip(const Geometry& g, const Rgb& eq, int W, int H, double pxDeg,
                                 double elDeg)
{
    std::vector<float> out(size_t(W) * H * 3);
    ParallelFor(H, [&](int y) {
        const double e = (elDeg - (y + 0.5) / pxDeg) * kDeg;
        for (int x = 0; x < W; ++x) {
            double c[3];
            g.sampleWorld(eq, g.st.seamDir((x + 0.5) / pxDeg * kDeg, e), c);
            for (int k = 0; k < 3; ++k)
                out[(size_t(y) * W + x) * 3 + k] = float(c[k]);
        }
    });
    return out;
}

struct Block {
    double phiDeg = 0, tex = 0, ncc0 = 0, best = 0, dphi = 0, de = 0, q = 0;
    bool textured = false;
};

// Per 5 deg block inside the band: the shift of lens 1 against lens 0 (after
// the depth table) with the best NCC of their grey, searched +-1 deg.
std::vector<Block> MeasureOffsets(const LensStrips& s, double bandDeg, const float* q)
{
    const int W = s.W, H = s.H;
    std::vector<float> grey[2];
    for (int i = 0; i < 2; ++i) {
        grey[i].assign(size_t(W) * H, -1.f);
        for (size_t p = 0; p < size_t(W) * H; ++p)
            if (s.treated[i][p * 3] >= 0)
                grey[i][p] = 0.299f * s.treated[i][p * 3] + 0.587f * s.treated[i][p * 3 + 1] +
                             0.114f * s.treated[i][p * 3 + 2];
    }
    const int blk = int(5 * kStripPxDeg);
    const int y0 = std::max(0, int((kStripElDeg - bandDeg) * kStripPxDeg));
    const int y1 = std::min(H, int((kStripElDeg + bandDeg) * kStripPxDeg));
    const int sMax = int(kStripPxDeg);
    std::vector<Block> blocks(W / blk);
    ParallelFor(int(blocks.size()), [&](int bi) {
        const int b0 = bi * blk;
        auto ncc = [&](int sx, int sy, double* tex) {
            double a = 0, bb = 0, aa = 0, b2 = 0, ab = 0, n = 0;
            for (int y = y0; y < y1; ++y) {
                const int yy = y + sy;
                if (yy < 0 || yy >= H)
                    continue;
                for (int x = b0; x < b0 + blk; ++x) {
                    const float p = grey[0][size_t(y) * W + x];
                    const float r = grey[1][size_t(yy) * W + (x + sx + W) % W];
                    if (p < 0 || r < 0)
                        continue;
                    a += p;
                    bb += r;
                    aa += p * p;
                    b2 += r * r;
                    ab += p * r;
                    ++n;
                }
            }
            if (n < 100)
                return -2.0;
            const double ma = a / n, mb = bb / n, va = aa / n - ma * ma, vb = b2 / n - mb * mb;
            if (tex)
                *tex = std::sqrt(std::max(0.0, std::min(va, vb)));
            return (va > 1e-6 && vb > 1e-6) ? (ab / n - ma * mb) / std::sqrt(va * vb) : 0.0;
        };
        Block& B = blocks[bi];
        B.phiDeg = (b0 + blk / 2.0) / kStripPxDeg;
        B.q = q[std::min(insv::kSeamDepthBins - 1, int(B.phiDeg / 360.0 * insv::kSeamDepthBins))];
        B.ncc0 = ncc(0, 0, &B.tex);
        double best = B.ncc0;
        int bx = 0, by = 0;
        for (int sy = -sMax; sy <= sMax; sy += 2) // coarse, then +-2 px around it
            for (int sx = -sMax; sx <= sMax; sx += 2) {
                const double v = ncc(sx, sy, nullptr);
                if (v > best) {
                    best = v;
                    bx = sx;
                    by = sy;
                }
            }
        const int cx = bx, cy = by;
        for (int sy = cy - 2; sy <= cy + 2; ++sy)
            for (int sx = cx - 2; sx <= cx + 2; ++sx) {
                if (std::abs(sx) > sMax || std::abs(sy) > sMax)
                    continue;
                const double v = ncc(sx, sy, nullptr);
                if (v > best) {
                    best = v;
                    bx = sx;
                    by = sy;
                }
            }
        double fx = bx, fy = by;
        if (std::abs(bx) < sMax && std::abs(by) < sMax) {
            double l = ncc(bx - 1, by, nullptr), r = ncc(bx + 1, by, nullptr);
            double d = l - 2 * best + r;
            if (d < 0)
                fx += 0.5 * (l - r) / d;
            l = ncc(bx, by - 1, nullptr);
            r = ncc(bx, by + 1, nullptr);
            d = l - 2 * best + r;
            if (d < 0)
                fy += 0.5 * (l - r) / d;
        }
        B.best = best;
        B.dphi = fx / kStripPxDeg;
        B.de = -fy / kStripPxDeg; // strip rows run down, elevation up
        B.textured = B.tex > 8 && best > 0.6;
    });
    return blocks;
}

// The colour difference where the lenses meet: per bin, each lens's mean per
// channel over a patch at the fade's centre (unclipped samples), lens 0
// against lens 1 in %.
struct ColourBin {
    bool ok[3] = {false, false, false};
    double raw[3] = {0, 0, 0}, treated[3] = {0, 0, 0};
};

std::vector<ColourBin> MeasureColour(const Geometry& g, const insv::Image (&lens)[2],
                                     const Treatment& tr)
{
    std::vector<ColourBin> bins(insv::kSeamDepthBins);
    const double binRad = 2 * kPi / insv::kSeamDepthBins;
    ParallelFor(insv::kSeamDepthBins, [&](int b) {
        double sum[2][2][3] = {}, n[3] = {};
        for (int ei = 0; ei < insv::kSeamCutPatchEl; ++ei)
            for (int ai = 0; ai < insv::kSeamCutPatchAz; ++ai) {
                const double phi = (b + (ai + 0.5) / insv::kSeamCutPatchAz) * binRad;
                const double e = tr.cutRad(b) +
                                 ((ei + 0.5) / insv::kSeamCutPatchEl - 0.5) * 2 *
                                     insv::kSeamCutPatchElDeg * kDeg;
                const insv::Vec3 d = g.st.seamDir(phi, e);
                const double qq = g.st.seamQAt(d, tr.q);
                double lg[2][3], veil[2][3];
                tr.gains(d, lg, veil);
                std::array<float, 3> c[2];
                bool seen = true;
                for (int i = 0; i < 2 && seen; ++i) {
                    double u, v, th;
                    seen = g.st.project(i, g.st.lensRay(i, d, qq), u, v, th) && u >= 0 && v >= 0 &&
                           u <= lens[i].w - 1 && v <= lens[i].h - 1;
                    if (seen)
                        c[i] = lens[i].sample(u, v);
                }
                if (!seen)
                    continue;
                for (int k = 0; k < 3; ++k) {
                    if (c[0][k] > insv::kSeamCutClip || c[1][k] > insv::kSeamCutClip)
                        continue;
                    for (int i = 0; i < 2; ++i) {
                        const double r = insv::LensVeilRemove(c[i][k], veil[i][k]);
                        sum[i][0][k] += c[i][k];
                        sum[i][1][k] += insv::LensColourGain(lg[i][k], r) * r;
                    }
                    n[k] += 1;
                }
            }
        for (int k = 0; k < 3; ++k) {
            if (n[k] < 32 || sum[0][0][k] / n[k] < 8 || sum[1][0][k] / n[k] < 8)
                continue;
            bins[b].ok[k] = true;
            bins[b].raw[k] = 100.0 * (sum[0][0][k] / sum[1][0][k] - 1.0);
            bins[b].treated[k] = 100.0 * (sum[0][1][k] / std::max(1e-6, sum[1][1][k]) - 1.0);
        }
    });
    return bins;
}

double Percentile(std::vector<double> v, double p)
{
    if (v.empty())
        return NAN;
    std::sort(v.begin(), v.end());
    const double idx = p * (v.size() - 1);
    const size_t i = size_t(idx);
    return i + 1 < v.size() ? v[i] + (idx - i) * (v[i + 1] - v[i]) : v[i];
}

// --- output -----------------------------------------------------------------

struct Json {
    std::ostringstream s;
    bool first = true;
    void key(const std::string& k)
    {
        s << (first ? "\n" : ",\n") << "  \"" << k << "\": ";
        first = false;
    }
    static std::string num(double v)
    {
        if (!std::isfinite(v))
            return "null";
        char b[32];
        std::snprintf(b, sizeof(b), "%.6g", v);
        return b;
    }
    static std::string str(const std::string& v)
    {
        std::string o = "\"";
        for (char c : v) {
            if (c == '"' || c == '\\')
                o += '\\';
            o += c;
        }
        return o + "\"";
    }
    template <class F> static std::string arr(int n, F f)
    {
        std::string o = "[";
        for (int i = 0; i < n; ++i)
            o += (i ? ", " : "") + f(i);
        return o + "]";
    }
};

// The summary: one "key": number per line, so --baseline reads it back
// without a JSON library.
using Summary = std::vector<std::pair<std::string, double>>;

std::string SummaryJson(const Summary& s)
{
    std::string o = "{\n";
    for (size_t i = 0; i < s.size(); ++i)
        o += "    \"" + s[i].first + "\": " + Json::num(s[i].second) + (i + 1 < s.size() ? ",\n" : "\n");
    return o + "  }";
}

std::map<std::string, double> ReadSummary(const std::string& path)
{
    std::map<std::string, double> out;
    std::ifstream in(path);
    std::string line;
    bool inside = false;
    static const std::regex kv("^\\s*\"(\\w+)\": (\\S+?),?$");
    while (std::getline(in, line)) {
        if (!inside) {
            inside = line.find("\"summary\": {") != std::string::npos;
            continue;
        }
        if (line.find('}') != std::string::npos)
            break;
        std::smatch m;
        if (std::regex_match(line, m, kv))
            out[m[1]] = m[2] == "null" ? NAN : std::atof(m[2].str().c_str());
    }
    return out;
}

const char* GuardName(stitch::PreviewStitchConfig::LensGuard g)
{
    using G = stitch::PreviewStitchConfig::LensGuard;
    return g == G::None ? "none" : g == G::Standard ? "standard" : "premium";
}

// --- one frame ---------------------------------------------------------------

struct Options {
    std::string outDir, baseline, only;
    int width = 0;
    int eqWidth = 3840;
    int64_t warmupMs = 2000;
    bool cpu = true, images = true;
    bool live = false; // no seam analysis: each path measures as it goes
};

struct FlickerStep {
    double band = 0, outside = 0, q = 0, cut = 0, fade = 0, local = 0;
};

bool RunEntry(const Entry& e, const Options& opt, std::string& err)
{
    std::printf("== %s: %s at %.3f s%s%s\n", e.name.c_str(), e.clip.c_str(), e.timeMs / 1000.0,
                e.note.empty() ? "" : " -- ", e.note.c_str());
    std::fflush(stdout);

    // The stitch as NativeStitchSource sets it up, for the strips.
    Geometry g;
    insv::InsvCalibration calib;
    insv::StitchParams params;
    std::shared_ptr<const camera::ClipMetadata> meta;
    if (stitch::PrepareClipMeasurement(e.clip, e.guard, nullptr, calib, params, err, &meta) !=
        stitch::MeasureFailure::None)
        return false;

    // The new stitcher takes neither the seam correction nor the lens
    // colour: it joins the lenses from the lens model, with the camera
    // model's lens correction.
    insv::SeamCorrection seam;
    std::string seamFrom = "list";
    if (e.newStitcher) {
        seamFrom = "not used: the new stitcher";
    } else if (e.seam) {
        seam = *e.seam;
    } else {
        const auto r = stitch::MeasureClipSeam(e.clip, 0, meta->durationMs, e.guard, nullptr);
        seamFrom = r.ok ? "measured" : "none (" + r.message + ")";
        if (r.ok)
            seam = r.correction;
    }
    insv::LensColour colour;
    std::string colourFrom = "list";
    if (e.newStitcher) {
        colourFrom = "not used: the new stitcher";
    } else if (e.colour) {
        colour = *e.colour;
    } else {
        const auto r = stitch::MeasureClipLensColour(e.clip, 0, meta->durationMs, e.guard, nullptr, seam);
        colourFrom = r.ok ? "measured" : "none (" + r.message + ")";
        if (r.ok)
            colour = r.colour;
    }
    char seamArg[160], colourArg[200];
    std::snprintf(seamArg, sizeof(seamArg), "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f", seam.lens1RotDeg[0],
                  seam.lens1RotDeg[1], seam.lens1RotDeg[2], seam.lens1Centre[0] * 100,
                  seam.lens1Centre[1] * 100, (seam.radialScale - 1) * 100);
    std::snprintf(colourArg, sizeof(colourArg), "%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f",
                  std::expm1(colour.logRatio[0]) * 100, std::expm1(colour.logRatio[1]) * 100,
                  std::expm1(colour.logRatio[2]) * 100, colour.slope[0] * 100,
                  colour.slope[1] * 100, colour.slope[2] * 100, colour.spread);
    std::printf("  seam=%s (%s)\n  colour=%s (%s)\n", seamArg, seamFrom.c_str(), colourArg,
                colourFrom.c_str());

    params.seam = seam;
    params.lensColour = colour;
    params.depthSeam = !e.newStitcher;
    if (e.newStitcher)
        params.modelCorrection = meta->lensCorrection;
    if (!g.st.configure(calib, meta->lensWidth, meta->lensHeight, params)) {
        err = "FisheyeStitcher::configure failed";
        return false;
    }
    g.bandRad = g.st.gpuParams().bandRad;
    const double bandDeg = g.bandRad / kDeg;

    stitch::PreviewStitchConfig config;
    config.width = opt.width > 0 ? opt.width : stitch::FullSphereWidth(meta->lensWidth);
    config.height = config.width / 2;
    config.lensGuard = e.guard;
    config.method = e.newStitcher ? stitch::PreviewStitchConfig::Method::New
                                  : stitch::PreviewStitchConfig::Method::DepthSeam;
    auto open = [&](stitch::NativeStitchSource& src) {
        if (!src.Open(e.clip, config)) {
            err = "open: " + src.GetLastError();
            return false;
        }
        src.SetSeamCorrection(seam);
        src.SetLensColour(colour);
        return true;
    };

    // The seam analysis of the stretch, as the app runs it after opening a
    // clip (held in memory here); every path below then reads it. Without
    // it (--live) each measures the seam as it goes.
    double analysisSec = 0;
    if (!opt.live && !e.newStitcher) {
        const auto t0 = std::chrono::steady_clock::now();
        const stitch::SeamAnalysisResult r = stitch::AnalyseClipSeam(
            {e.clip, e.guard, seam, colour}, std::max<int64_t>(0, e.timeMs - opt.warmupMs), e.timeMs);
        analysisSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  seam analysis: %s\n", r.message.c_str());
        if (!r.ok) {
            err = "seam analysis: " + r.message;
            return false;
        }
    }

    // Played into, as playback reaches the frame.
    stitch::NativeStitchSource gpu;
    if (!open(gpu))
        return false;
    const bool onGpu = gpu.GpuViewAvailable();
    if (!onGpu)
        std::printf("  (no GPU stitch: the \"GPU\" runs are the CPU fallback)\n");
    if (!gpu.SeekTo(std::max<int64_t>(0, e.timeMs - opt.warmupMs))) {
        err = "seek: " + gpu.GetLastError();
        return false;
    }
    const double frameMs = meta->fps > 0 ? 1000.0 / meta->fps : 33.3;
    const int sW = int(360 * 5), sH = int(2 * (bandDeg + 11) * 5); // 5 px/deg, the band and beside it
    std::vector<float> prevStrip;
    std::vector<float> prevQ;
    insv::SeamBlend prevBlend;
    std::vector<float> prevColour;
    std::vector<FlickerStep> flicker;
    stitch::Frame played;
    int runFrames = 0;
    for (;;) {
        stitch::Frame f = gpu.ReadFrame();
        if (f.rgba.empty()) {
            err = "the clip ended before the frame";
            return false;
        }
        ++runFrames;
        const bool last = f.timestamp_ms >= e.timeMs; // GetFrame's frame: the first at or after
        if (f.timestamp_ms >= e.timeMs - 1000 - frameMs / 2) {
            const Rgb eq = FromFrame(f);
            std::vector<float> strip = EquirectStrip(g, eq, sW, sH, 5.0, bandDeg + 11);
            const float* q = gpu.SeamDepthTable();
            const insv::SeamBlend& bl = gpu.SeamBlendState();
            const float* sc = gpu.SeamColourTable();
            if (!prevStrip.empty()) {
                FlickerStep st;
                double nb = 0, no = 0;
                for (int y = 0; y < sH; ++y) {
                    const double el = std::fabs(bandDeg + 11 - (y + 0.5) / 5.0);
                    const bool inBand = el <= bandDeg, beside = el >= bandDeg + 3;
                    if (!inBand && !beside)
                        continue;
                    for (int x = 0; x < sW * 3; ++x) {
                        const double d = std::fabs(strip[size_t(y) * sW * 3 + x] -
                                                   prevStrip[size_t(y) * sW * 3 + x]);
                        (inBand ? st.band : st.outside) += d;
                        (inBand ? nb : no) += 1;
                    }
                }
                st.band /= std::max(1.0, nb);
                st.outside /= std::max(1.0, no);
                for (int b = 0; b < insv::kSeamDepthBins; ++b) {
                    st.q += std::fabs(q[b] - prevQ[b]);
                    st.cut += std::fabs(bl.cut[b] - prevBlend.cut[b]) / kDeg;
                    st.fade += std::fabs(bl.fade[b] - prevBlend.fade[b]) / kDeg;
                    for (int k = 0; k < 3; ++k) // one of the two is 0: the stitcher's own
                        st.local += std::fabs(bl.local[b][k] - prevBlend.local[b][k]) * 100 / 3 +
                                    std::fabs(sc[b * 3 + k] - prevColour[b * 3 + k]) * 100 / 3;
                }
                st.q /= insv::kSeamDepthBins;
                st.cut /= insv::kSeamDepthBins;
                st.fade /= insv::kSeamDepthBins;
                st.local /= insv::kSeamDepthBins;
                flicker.push_back(st);
            }
            prevStrip = std::move(strip);
            prevQ.assign(q, q + insv::kSeamDepthBins);
            prevBlend = bl;
            prevColour.assign(sc, sc + insv::kSeamDepthBins * 3);
        }
        if (last) {
            played = std::move(f);
            break;
        }
    }
    const int64_t pts = played.timestamp_ms;
    std::vector<float> qPlayed(gpu.SeamDepthTable(), gpu.SeamDepthTable() + insv::kSeamDepthBins);
    const insv::SeamBlend blendPlayed = gpu.SeamBlendState();
    const std::vector<float> colourPlayed(gpu.SeamColourTable(),
                                          gpu.SeamColourTable() + insv::kSeamDepthBins * 3);
    const double confPlayed = gpu.SeamDepthConfidence();
    std::printf("  frame %lld ms, played in over %d frames\n", static_cast<long long>(pts), runFrames);

    // Seeked to cold, on the same source.
    const stitch::Frame cold = gpu.GetFrame(e.timeMs);
    if (cold.rgba.empty()) {
        err = "cold frame: " + gpu.GetLastError();
        return false;
    }
    if (cold.timestamp_ms != pts)
        std::printf("  note: the cold frame is at %lld ms\n", static_cast<long long>(cold.timestamp_ms));
    std::vector<float> qCold(gpu.SeamDepthTable(), gpu.SeamDepthTable() + insv::kSeamDepthBins);
    const insv::SeamBlend blendCold = gpu.SeamBlendState();
    const double confCold = gpu.SeamDepthConfidence();

    // The CPU fallback, cold (the app's path without a GPU stitch).
    stitch::Frame cpuFrame;
    std::vector<float> qCpu;
    if (opt.cpu) {
        setenv("INSV_NO_GPU_STITCH", "1", 1);
        stitch::NativeStitchSource cpu;
        const bool ok = open(cpu);
        unsetenv("INSV_NO_GPU_STITCH");
        if (!ok)
            return false;
        cpuFrame = cpu.GetFrame(e.timeMs);
        if (cpuFrame.rgba.empty()) {
            err = "CPU frame: " + cpu.GetLastError();
            return false;
        }
        qCpu.assign(cpu.SeamDepthTable(), cpu.SeamDepthTable() + insv::kSeamDepthBins);
        if (cpuFrame.timestamp_ms != cold.timestamp_ms)
            std::printf("  note: the CPU frame is at %lld ms\n",
                        static_cast<long long>(cpuFrame.timestamp_ms));
    }

    // The lenses as decoded, for the strips.
    insv::Image lens[2];
    {
        insv::InsvDemux demux;
        if (!demux.open(meta->lensSources(), err) || !demux.seekTo(pts, err))
            return false;
        int64_t p = -1;
        while (p < pts)
            if (!demux.readNextPair(lens[0], lens[1], &p, err)) {
                err = "lens frames: " + (err.empty() ? std::string("end of stream") : err);
                return false;
            }
    }

    const Rgb eqPlayed = FromFrame(played), eqCold = FromFrame(cold);
    Treatment tr;
    tr.st = &g.st;
    tr.q = qPlayed.data();
    if (e.newStitcher)
        tr.seamColour = colourPlayed.data();
    else
        tr.blend = &blendPlayed;
    const LensStrips ls = MakeLensStrips(g, lens, tr);
    const std::vector<Block> blocks = MeasureOffsets(ls, bandDeg, qPlayed.data());
    const std::vector<ColourBin> colourBins = MeasureColour(g, lens, tr);

    Summary sum;
    sum.emplace_back("frame_ms", double(pts));
    {
        std::vector<double> far, all;
        for (const Block& b : blocks)
            if (b.textured) {
                const double o = std::hypot(b.dphi, b.de);
                all.push_back(o);
                if (b.q < 0.05)
                    far.push_back(o);
            }
        auto mean = [](const std::vector<double>& v) {
            double s = 0;
            for (double x : v)
                s += x;
            return v.empty() ? NAN : s / v.size();
        };
        sum.emplace_back("offset_blocks", double(all.size()));
        sum.emplace_back("offset_mean_deg", mean(all));
        sum.emplace_back("offset_p90_deg", Percentile(all, 0.9));
        sum.emplace_back("far_blocks", double(far.size()));
        sum.emplace_back("far_offset_mean_deg", mean(far));
        sum.emplace_back("far_offset_p90_deg", Percentile(far, 0.9));
    }
    {
        int near = 0;
        double qMax = 0;
        for (float q : qPlayed) {
            near += q > 0.2f;
            qMax = std::max(qMax, double(q));
        }
        sum.emplace_back("depth_confidence", confPlayed);
        sum.emplace_back("cold_depth_confidence", confCold);
        sum.emplace_back("near_bins", near);
        sum.emplace_back("q_max", qMax);
        double fade = 0;
        for (float f : blendPlayed.fade)
            fade += f / kDeg;
        sum.emplace_back("fade_mean_deg", fade / insv::kSeamDepthBins);
    }
    {
        const char* ch = "rgb";
        int nBins = 0;
        for (const ColourBin& b : colourBins)
            nBins += b.ok[0] && b.ok[1] && b.ok[2];
        sum.emplace_back("colour_bins", nBins);
        for (int k = 0; k < 3; ++k) {
            std::vector<double> t, ta, ra;
            for (const ColourBin& b : colourBins)
                if (b.ok[k]) {
                    t.push_back(b.treated[k]);
                    ta.push_back(std::fabs(b.treated[k]));
                    ra.push_back(std::fabs(b.raw[k]));
                }
            const std::string c(1, ch[k]);
            sum.emplace_back("colour_" + c + "_median_pct", Percentile(t, 0.5));
            sum.emplace_back("colour_" + c + "_abs_median_pct", Percentile(ta, 0.5));
            sum.emplace_back("colour_" + c + "_abs_p90_pct", Percentile(ta, 0.9));
            sum.emplace_back("raw_colour_" + c + "_abs_median_pct", Percentile(ra, 0.5));
        }
    }
    auto tableDiff = [&](const std::vector<float>& a, const std::vector<float>& b,
                         const std::string& prefix) {
        double s = 0, m = 0;
        int n = 0;
        for (int i = 0; i < insv::kSeamDepthBins; ++i) {
            const double d = std::fabs(a[i] - b[i]);
            s += d;
            m = std::max(m, d);
            n += d > 0.1;
        }
        sum.emplace_back(prefix + "_q_mean_abs_diff", s / insv::kSeamDepthBins);
        sum.emplace_back(prefix + "_q_max_abs_diff", m);
        sum.emplace_back(prefix + "_bins_differ", n);
    };
    tableDiff(qPlayed, qCold, "cold");
    {
        double whole, band;
        ComparePsnr(g, eqPlayed, eqCold, whole, band);
        sum.emplace_back("cold_psnr", whole);
        sum.emplace_back("cold_band_psnr", band);
    }
    if (opt.cpu) {
        tableDiff(qCold, qCpu, "cpu");
        const Rgb eqCpu = FromFrame(cpuFrame);
        double whole = NAN, band = NAN;
        if (eqCpu.w == eqCold.w && eqCpu.h == eqCold.h)
            ComparePsnr(g, eqCold, eqCpu, whole, band);
        sum.emplace_back("cpu_psnr", whole);
        sum.emplace_back("cpu_band_psnr", band);
    }
    {
        FlickerStep m;
        for (const FlickerStep& f : flicker) {
            m.band += f.band;
            m.outside += f.outside;
            m.q += f.q;
            m.cut += f.cut;
            m.fade += f.fade;
            m.local += f.local;
        }
        const double n = std::max<size_t>(1, flicker.size());
        sum.emplace_back("flicker_frames", double(flicker.size()));
        sum.emplace_back("flicker_band", m.band / n);
        sum.emplace_back("flicker_beside", m.outside / n);
        sum.emplace_back("flicker_ratio", m.outside > 0 ? m.band / m.outside : NAN);
        sum.emplace_back("flicker_q", m.q / n);
        sum.emplace_back("flicker_cut_deg", m.cut / n);
        sum.emplace_back("flicker_fade_deg", m.fade / n);
        sum.emplace_back("flicker_local_pct", m.local / n);
    }
    if (!e.ref.empty()) {
        Rgb ref;
        if (!ReadPpm(e.ref, ref, err))
            return false;
        // The reference's direction for ours is R times it, R = Rz Ry Rx
        // (refrot's z, y, x angles) in the frame x at the image centre
        // (longitude 0), y at longitude +90 (half way to the right edge), z
        // up.
        const double a = e.refRotDeg[0] * kDeg, b = e.refRotDeg[1] * kDeg, c = e.refRotDeg[2] * kDeg;
        const double ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b),
                     cc = std::cos(c), sc = std::sin(c);
        const double R[3][3] = {{ca * cb, ca * sb * sc - sa * cc, ca * sb * cc + sa * sc},
                                {sa * cb, sa * sb * sc + ca * cc, sa * sb * cc - ca * sc},
                                {-sb, cb * sc, cb * cc}};
        Rgb aligned;
        aligned.alloc(eqPlayed.w, eqPlayed.h);
        ParallelFor(aligned.h, [&](int y) {
            for (int x = 0; x < aligned.w; ++x) {
                const insv::Vec3 s = PixelSphere(x, y, aligned.w, aligned.h);
                const double P[3] = {-s.z, s.x, s.y}; // ours -> x forward, y left, z up
                double Q[3];
                for (int i = 0; i < 3; ++i)
                    Q[i] = R[i][0] * P[0] + R[i][1] * P[1] + R[i][2] * P[2];
                double lon, lat, v[3];
                SphereLonLat({Q[1], Q[2], -Q[0]}, lon, lat);
                SampleEquirect(ref, lon, lat, v);
                for (int k = 0; k < 3; ++k)
                    aligned.at(x, y)[k] = uint8_t(std::lround(std::clamp(v[k], 0.0, 255.0)));
            }
        });
        double whole, band;
        ComparePsnr(g, aligned, eqPlayed, whole, band);
        sum.emplace_back("ref_psnr", whole);
        sum.emplace_back("ref_band_psnr", band);
        if (opt.images)
            WritePpm(opt.outDir + "/" + e.name + "_ref.ppm", Downscale(aligned, opt.eqWidth));
    }

    // Images.
    if (opt.images) {
        const int W = ls.W, H = ls.H, gap = 2;
        Rgb strip;
        strip.alloc(W, 5 * H + 4 * gap);
        std::fill(strip.px.begin(), strip.px.end(), uint8_t(96));
        auto put = [&](int row, const std::function<void(int, int, double[3])>& px) {
            ParallelFor(H, [&](int y) {
                for (int x = 0; x < W; ++x) {
                    double c[3] = {0, 0, 0};
                    px(x, y, c);
                    for (int k = 0; k < 3; ++k)
                        strip.at(x, row * (H + gap) + y)[k] =
                            uint8_t(std::lround(std::clamp(c[k], 0.0, 255.0)));
                }
            });
        };
        for (int i = 0; i < 2; ++i)
            put(i, [&](int x, int y, double c[3]) {
                for (int k = 0; k < 3; ++k)
                    c[k] = std::max(0.f, ls.raw[i][(size_t(y) * W + x) * 3 + k]);
            });
        const std::vector<float> bp = EquirectStrip(g, eqPlayed, W, H, kStripPxDeg, kStripElDeg);
        const std::vector<float> bc = EquirectStrip(g, eqCold, W, H, kStripPxDeg, kStripElDeg);
        put(2, [&](int x, int y, double c[3]) {
            for (int k = 0; k < 3; ++k)
                c[k] = bp[(size_t(y) * W + x) * 3 + k];
        });
        put(3, [&](int x, int y, double c[3]) {
            const size_t o = (size_t(y) * W + x) * 3;
            if (ls.treated[0][o] < 0 || ls.treated[1][o] < 0)
                return;
            for (int k = 0; k < 3; ++k)
                c[k] = 4.0 * std::fabs(ls.treated[0][o + k] - ls.treated[1][o + k]);
        });
        put(4, [&](int x, int y, double c[3]) {
            for (int k = 0; k < 3; ++k)
                c[k] = bc[(size_t(y) * W + x) * 3 + k];
        });
        WritePpm(opt.outDir + "/" + e.name + "_strip.ppm", strip);
        if (opt.eqWidth > 0)
            WritePpm(opt.outDir + "/" + e.name + "_eq.ppm", Downscale(eqPlayed, opt.eqWidth));
    }

    // The JSON.
    Json j;
    j.key("name");
    j.s << Json::str(e.name);
    j.key("clip");
    j.s << Json::str(e.clip);
    j.key("time_ms");
    j.s << e.timeMs;
    j.key("note");
    j.s << Json::str(e.note);
    j.key("guard");
    j.s << Json::str(GuardName(e.guard));
    j.key("method");
    j.s << Json::str(e.newStitcher ? "new" : "depth");
    j.key("gpu");
    j.s << (onGpu ? "true" : "false");
    j.key("width");
    j.s << config.width;
    j.key("warmup_ms");
    j.s << opt.warmupMs;
    j.key("seam_analysis");
    j.s << (opt.live ? "false" : "true");
    j.key("seam_analysis_s");
    j.s << Json::num(analysisSec);
    j.key("seam");
    j.s << Json::str(std::string(seamArg) + " (" + seamFrom + ")");
    j.key("colour");
    j.s << Json::str(std::string(colourArg) + " (" + colourFrom + ")");
    j.key("band_deg");
    j.s << Json::num(bandDeg);
    j.key("summary");
    j.s << SummaryJson(sum);
    auto table = [](const std::vector<float>& q) {
        return Json::arr(int(q.size()), [&](int i) { return Json::num(q[i]); });
    };
    j.key("q_played");
    j.s << table(qPlayed);
    j.key("q_cold");
    j.s << table(qCold);
    if (opt.cpu) {
        j.key("q_cpu");
        j.s << table(qCpu);
    }
    j.key("cut_deg");
    j.s << Json::arr(insv::kSeamDepthBins, [&](int i) { return Json::num(blendPlayed.cut[i] / kDeg); });
    j.key("fade_deg");
    j.s << Json::arr(insv::kSeamDepthBins, [&](int i) { return Json::num(blendPlayed.fade[i] / kDeg); });
    j.key("residual_pct");
    j.s << Json::arr(3, [&](int k) { return Json::num(std::expm1(blendPlayed.colour[k]) * 100); });
    j.key("local_pct");
    j.s << Json::arr(insv::kSeamDepthBins, [&](int i) {
        return Json::arr(3, [&](int k) { return Json::num(std::expm1(blendPlayed.local[i][k]) * 100); });
    });
    j.key("seam_colour_pct");
    j.s << Json::arr(insv::kSeamDepthBins, [&](int i) {
        return Json::arr(3, [&](int k) { return Json::num(std::expm1(colourPlayed[i * 3 + k]) * 100); });
    });
    j.key("veil");
    j.s << Json::arr(insv::kSeamDepthBins, [&](int i) {
        return Json::arr(3, [&](int k) { return Json::num(blendPlayed.veil[i][k]); });
    });
    j.key("cold_cut_deg");
    j.s << Json::arr(insv::kSeamDepthBins, [&](int i) { return Json::num(blendCold.cut[i] / kDeg); });
    j.key("blocks");
    j.s << Json::arr(int(blocks.size()), [&](int i) {
        const Block& b = blocks[i];
        return "{\"phi\": " + Json::num(b.phiDeg) + ", \"tex\": " + Json::num(b.tex) +
               ", \"ncc0\": " + Json::num(b.ncc0) + ", \"ncc\": " + Json::num(b.best) +
               ", \"dphi\": " + Json::num(b.dphi) + ", \"de\": " + Json::num(b.de) +
               ", \"q\": " + Json::num(b.q) + ", \"textured\": " + (b.textured ? "true" : "false") + "}";
    });
    j.key("colour_pct");
    j.s << Json::arr(insv::kSeamDepthBins, [&](int i) {
        const ColourBin& b = colourBins[i];
        return Json::arr(3, [&](int k) { return b.ok[k] ? Json::num(b.treated[k]) : "null"; });
    });
    j.key("raw_colour_pct");
    j.s << Json::arr(insv::kSeamDepthBins, [&](int i) {
        const ColourBin& b = colourBins[i];
        return Json::arr(3, [&](int k) { return b.ok[k] ? Json::num(b.raw[k]) : "null"; });
    });
    j.key("flicker");
    j.s << Json::arr(int(flicker.size()), [&](int i) {
        const FlickerStep& f = flicker[i];
        return "{\"band\": " + Json::num(f.band) + ", \"beside\": " + Json::num(f.outside) +
               ", \"q\": " + Json::num(f.q) + ", \"cut_deg\": " + Json::num(f.cut) +
               ", \"fade_deg\": " + Json::num(f.fade) + ", \"local_pct\": " + Json::num(f.local) + "}";
    });
    const std::string jsonPath = opt.outDir + "/" + e.name + ".json";
    {
        std::ofstream out(jsonPath);
        out << "{" << j.s.str() << "\n}\n";
    }

    // The summary, against the baseline's if there is one.
    std::map<std::string, double> base;
    if (!opt.baseline.empty())
        base = ReadSummary(opt.baseline + "/" + e.name + ".json");
    if (!opt.baseline.empty() && base.empty())
        std::printf("  (no baseline for %s)\n", e.name.c_str());
    for (const auto& [k, v] : sum) {
        const auto it = base.find(k);
        const std::string now = Json::num(v);
        if (it == base.end()) {
            std::printf("  %-26s %s\n", k.c_str(), now.c_str());
            continue;
        }
        const std::string was = Json::num(it->second);
        if (was == now)
            std::printf("  %-26s %s\n", k.c_str(), now.c_str());
        else
            std::printf("  %-26s %s  (was %s)  *\n", k.c_str(), now.c_str(), was.c_str());
    }
    std::fflush(stdout);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    std::string list;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            std::fputs(kUsage, stdout);
            return 0;
        } else if (a == "--out")
            opt.outDir = next();
        else if (a == "--baseline")
            opt.baseline = next();
        else if (a == "--only")
            opt.only = next();
        else if (a == "--width")
            opt.width = std::atoi(next().c_str());
        else if (a == "--eq-width")
            opt.eqWidth = std::atoi(next().c_str());
        else if (a == "--warmup")
            opt.warmupMs = std::llround(std::atof(next().c_str()) * 1000.0);
        else if (a == "--no-cpu")
            opt.cpu = false;
        else if (a == "--live")
            opt.live = true;
        else if (a == "--no-images")
            opt.images = false;
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n%s", a.c_str(), kUsage);
            return 2;
        } else
            list = a;
    }
    if (list.empty() || opt.outDir.empty()) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    std::vector<Entry> entries;
    std::string err;
    if (!ParseList(list, entries, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    std::error_code ec;
    std::filesystem::create_directories(opt.outDir, ec);
    if (ec) {
        std::fprintf(stderr, "can't create %s: %s\n", opt.outDir.c_str(), ec.message().c_str());
        return 1;
    }
    int failed = 0, ran = 0;
    for (const Entry& e : entries) {
        if (!opt.only.empty() && e.name.find(opt.only) == std::string::npos)
            continue;
        ++ran;
        std::string why;
        if (!RunEntry(e, opt, why)) {
            std::printf("  FAILED: %s\n", why.c_str());
            ++failed;
        }
    }
    std::printf("%d frame(s), %d failed\n", ran, failed);
    return failed ? 1 : 0;
}
