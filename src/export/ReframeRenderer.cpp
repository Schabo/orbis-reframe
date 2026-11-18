#include "ReframeRenderer.h"

#include "Sharpen.h"

#include <cstring>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

namespace exportpipe {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

int wrapIndex(int v, int n)
{
    // Positive-result modulo, for the horizontal equirect seam.
    int m = v % n;
    return m < 0 ? m + n : m;
}

// Bilinear RGBA fetch: U wraps, V clamps. `out[4]` receives the sample.
void sampleEquirect(const uint8_t* eq, int w, int h, float u, float v, float out[4])
{
    const float fx = u * static_cast<float>(w) - 0.5f;
    const float fy = v * static_cast<float>(h) - 0.5f;
    const float x0f = std::floor(fx);
    const float y0f = std::floor(fy);
    const float tx = fx - x0f;
    const float ty = fy - y0f;
    const int x0 = static_cast<int>(x0f);
    const int y0 = static_cast<int>(y0f);

    const int xs[2] = {wrapIndex(x0, w), wrapIndex(x0 + 1, w)};
    const int ys[2] = {std::clamp(y0, 0, h - 1), std::clamp(y0 + 1, 0, h - 1)};

    for (int c = 0; c < 4; ++c) {
        const float p00 = eq[(static_cast<size_t>(ys[0]) * w + xs[0]) * 4 + c];
        const float p10 = eq[(static_cast<size_t>(ys[0]) * w + xs[1]) * 4 + c];
        const float p01 = eq[(static_cast<size_t>(ys[1]) * w + xs[0]) * 4 + c];
        const float p11 = eq[(static_cast<size_t>(ys[1]) * w + xs[1]) * 4 + c];
        const float top = p00 + (p10 - p00) * tx;
        const float bot = p01 + (p11 - p01) * tx;
        out[c] = top + (bot - top) * ty;
    }
}

// Colour grade, applied in [0,255] rgb space -- the C++ twin of
// src/render/shaders/color_grade.glsl's gradeColor() (which can't be included
// here): change both together, constants included. Order: the LUT (mixed
// with the input by lutMix), temperature, exposure, contrast, saturation,
// then the colour look (mixed in by lookMix) -- which the shaders apply first
// thing in clip_effects.glsl's applyEffects(), on the graded picture, so the
// same place in the order. Leaves alpha untouched.
constexpr float kGradeTemperaturePush = 40.0f; // levels at temperature +-100%
constexpr float kGradeContrastPivot = 128.0f;
constexpr float kGradeWhite = 255.0f;

// A grade's factors, worked out once per picture rather than per pixel.
struct Grade {
    float t, ef, cf, sf;
    const color::Lut3D* lut; // nullptr = none
    float lutMix;
    const color::Lut3D* look; // nullptr = none
    float lookMix;
    explicit Grade(const color::Grade& g)
        : t(g.adjust[3] / 100.0f), ef(std::pow(2.0f, g.adjust[0])),
          cf(1.0f + g.adjust[1] / 100.0f), sf(1.0f + g.adjust[2] / 100.0f),
          lut(g.hasLut() ? g.lut.get() : nullptr), lutMix(std::min(g.lutMix, 1.0f)),
          look(g.hasLook() ? g.look.get() : nullptr), lookMix(std::min(g.lookMix, 1.0f))
    {
    }
};

// `lut`'s output for rgb in [0, kGradeWhite], mixed in by `mix`.
inline void applyLut(float rgb[3], const color::Lut3D& lut, float mix)
{
    float in[3], out[3];
    for (int c = 0; c < 3; ++c)
        in[c] = std::clamp(rgb[c] / kGradeWhite, 0.0f, 1.0f);
    lut.apply(in, out);
    for (int c = 0; c < 3; ++c)
        rgb[c] += (std::clamp(out[c], 0.0f, 1.0f) * kGradeWhite - rgb[c]) * mix;
}

inline void gradeRgb(float rgba[4], const Grade &g)
{
    if (g.lut)
        applyLut(rgba, *g.lut, g.lutMix);
    float r = rgba[0] + g.t * kGradeTemperaturePush;
    float gr = rgba[1];
    float b = rgba[2] - g.t * kGradeTemperaturePush;

    const float ef = g.ef;
    r *= ef;
    gr *= ef;
    b *= ef;

    const float cf = g.cf;
    r = (r - kGradeContrastPivot) * cf + kGradeContrastPivot;
    gr = (gr - kGradeContrastPivot) * cf + kGradeContrastPivot;
    b = (b - kGradeContrastPivot) * cf + kGradeContrastPivot;

    const float luma = 0.299f * r + 0.587f * gr + 0.114f * b;
    const float sf = g.sf;
    r = luma + (r - luma) * sf;
    gr = luma + (gr - luma) * sf;
    b = luma + (b - luma) * sf;

    rgba[0] = std::clamp(r, 0.0f, kGradeWhite);
    rgba[1] = std::clamp(gr, 0.0f, kGradeWhite);
    rgba[2] = std::clamp(b, 0.0f, kGradeWhite);
    if (g.look)
        applyLut(rgba, *g.look, g.lookMix);
}

// Runs fn(rowBegin, rowEnd) over h rows across the cores; small pictures
// on this thread alone.
template <typename Fn>
void acrossRows(int h, size_t pixels, Fn fn)
{
    const unsigned hw = std::thread::hardware_concurrency();
    const int workers =
        pixels < 64 * 1024 ? 1 : std::min(h, int(std::max(1u, hw == 0 ? 4u : hw)));
    const int band = (h + workers - 1) / workers;
    std::vector<std::thread> pool;
    for (int b = band; b < h; b += band)
        pool.emplace_back(fn, b, std::min(b + band, h));
    fn(0, std::min(band, h));
    for (std::thread& t : pool)
        t.join();
}

// The per-pixel effects and the rounding to 8 bits, after the sharpen:
// `graded` (w x h, 4 floats a pixel) into `out`. A pixel without a picture
// (label 0) as it is. The pixel (col, row)'s centre in the frame is
// (x0 + (col + 0.5) * sx, y0 + (row + 0.5) * sy).
void finishSharpened(const std::vector<float>& graded, const uint8_t* labels, uint8_t* out,
                     int w, int h, const fx::Render& effects, float x0, float y0, float sx,
                     float sy, float frameW, float frameH)
{
    acrossRows(h, size_t(w) * h, [&](int rowBegin, int rowEnd) {
        for (int row = rowBegin; row < rowEnd; ++row)
            for (int col = 0; col < w; ++col) {
                const size_t i = size_t(row) * w + col;
                float c[4] = {graded[i * 4], graded[i * 4 + 1], graded[i * 4 + 2],
                              graded[i * 4 + 3]};
                if (effects.perPixel() && (!labels || labels[i] != 0))
                    fx::applyEffects(c, x0 + (float(col) + 0.5f) * sx,
                                     y0 + (float(row) + 0.5f) * sy, frameW, frameH, effects,
                                     kGradeWhite);
                for (int k = 0; k < 4; ++k)
                    out[i * 4 + k] = static_cast<uint8_t>(std::clamp(c[k] + 0.5f, 0.0f, 255.0f));
            }
    });
}

// With `graded` (outW x outH x 4 floats) and `labels`, the rows are left
// graded there for the sharpen -- the look on, the other effects not yet,
// a pixel's label 1 where the picture is, 0 outside a globe's disc -- and
// `out` isn't written.
void renderRows(const uint8_t* eq, int eqW, int eqH, uint8_t* out, int outW, int outH,
                const ReframeView& view, int rowBegin, int rowEnd, float* graded = nullptr,
                uint8_t* labels = nullptr)
{
    const Grade grade(view.colorGrade);
    const fx::Render& effects = view.colorGrade.effects;
    const bool hasEffects = effects.perPixel() && !graded;
    const bool ownFrame = view.frame[2] <= 0.0f;
    const float frameX = ownFrame ? 0.0f : view.frame[0];
    const float frameY = ownFrame ? 0.0f : view.frame[1];
    const float frameW = ownFrame ? float(outW) : view.frame[2];
    const float frameH = ownFrame ? float(outH) : view.frame[3];
    const view::Lens lens = view::lens(view.pose, double(outW) / double(outH));
    const float h = view::halfHeight(lens.fovY, lens.distortion, lens.panini);
    const float cosR = std::cos(view.pose.roll);
    const float sinR = std::sin(view.pose.roll);
    const float cosP = std::cos(view.pose.pitch);
    const float sinP = std::sin(view.pose.pitch);
    const float cosY = std::cos(view.pose.yaw);
    const float sinY = std::sin(view.pose.yaw);

    for (int py = rowBegin; py < rowEnd; ++py) {
        const float ndcY = (static_cast<float>(py) + 0.5f) / static_cast<float>(outH) * 2.0f - 1.0f;
        for (int px = 0; px < outW; ++px) {
            const float ndcX =
                (static_cast<float>(px) + 0.5f) / static_cast<float>(outW) * 2.0f - 1.0f;

            // View-space ray (matches equirect.frag: +Y up, -Z forward).
            float ray[3];
            const size_t at = static_cast<size_t>(py) * outW + px;
            if (!view::ndcToRay(ndcX, ndcY, h, view.aspect, lens.distortion, ray, lens.panini,
                                lens.fisheye)) {
                // Outside a globe's disc, past Fisheye's: black.
                if (graded) {
                    std::fill(graded + at * 4, graded + at * 4 + 3, 0.0f);
                    graded[at * 4 + 3] = 255.0f;
                    labels[at] = 0;
                } else {
                    uint8_t* dst = out + at * 4;
                    dst[0] = dst[1] = dst[2] = 0;
                    dst[3] = 255;
                }
                continue;
            }
            const float vx = ray[0], vy = ray[1], vz = ray[2];

            // Roll about Z (view axis), then pitch about X, then yaw about Y.
            const float rx = vx * cosR - vy * sinR;
            const float ry = vx * sinR + vy * cosR;
            const float px_ = rx;
            const float py_ = ry * cosP - vz * sinP;
            const float pz_ = ry * sinP + vz * cosP;
            const float qx = px_ * cosY + pz_ * sinY;
            const float qy = py_;
            const float qz = -px_ * sinY + pz_ * cosY;
            // Stabilization in the sphere frame, after the pose.
            const float* m = view.stab;
            const float wx = m[0] * qx + m[1] * qy + m[2] * qz;
            const float wy = m[3] * qx + m[4] * qy + m[5] * qz;
            const float wz = m[6] * qx + m[7] * qy + m[8] * qz;

            const float longitude = std::atan2(wx, -wz);          // [-pi, pi]
            const float latitude = std::asin(std::clamp(wy, -1.0f, 1.0f)); // [-pi/2, pi/2]
            const float u = longitude / (2.0f * kPi) + 0.5f;
            const float v = 0.5f - latitude / kPi;

            float rgba[4];
            sampleEquirect(eq, eqW, eqH, u, v, rgba);
            gradeRgb(rgba, grade);
            if (graded) {
                std::copy(rgba, rgba + 4, graded + at * 4);
                labels[at] = 1;
                continue;
            }
            if (hasEffects)
                fx::applyEffects(rgba, float(px) + 0.5f - frameX, float(py) + 0.5f - frameY,
                                 frameW, frameH, effects, kGradeWhite);

            uint8_t* dst = out + at * 4;
            for (int c = 0; c < 4; ++c)
                dst[c] = static_cast<uint8_t>(std::clamp(rgba[c] + 0.5f, 0.0f, 255.0f));
        }
    }
}

} // namespace

void RenderRectilinear(const uint8_t* eqRgba, int eqW, int eqH, uint8_t* outRgba, int outW, int outH,
                       const ReframeView& view)
{
    if (eqW <= 0 || eqH <= 0 || outW <= 0 || outH <= 0)
        return;

    const fx::Render& effects = view.colorGrade.effects;
    if (effects.sharpens()) {
        // Graded first, the whole picture, as the sharpen needs its
        // neighbours; then the per-pixel effects on the sharpened picture.
        const size_t pixels = size_t(outW) * outH;
        std::vector<float> graded(pixels * 4);
        std::vector<uint8_t> labels(pixels);
        acrossRows(outH, pixels, [&](int rowBegin, int rowEnd) {
            renderRows(eqRgba, eqW, eqH, outRgba, outW, outH, view, rowBegin, rowEnd,
                       graded.data(), labels.data());
        });
        const bool ownFrame = view.frame[2] <= 0.0f;
        const float frameW = ownFrame ? float(outW) : view.frame[2];
        const float frameH = ownFrame ? float(outH) : view.frame[3];
        fx::sharpen(graded.data(), 4, labels.data(), outW, outH, frameH, effects, kGradeWhite);
        finishSharpened(graded, labels.data(), outRgba, outW, outH, effects,
                        ownFrame ? 0.0f : -view.frame[0], ownFrame ? 0.0f : -view.frame[1],
                        1.0f, 1.0f, frameW, frameH);
        return;
    }

    unsigned hw = std::thread::hardware_concurrency();
    int workers = static_cast<int>(hw == 0 ? 4u : hw);
    workers = std::clamp(workers, 1, outH);
    // Small frames aren't worth the thread spin-up.
    if (outH < 64)
        workers = 1;

    if (workers == 1) {
        renderRows(eqRgba, eqW, eqH, outRgba, outW, outH, view, 0, outH);
        return;
    }

    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(workers - 1));
    const int band = (outH + workers - 1) / workers;
    for (int w = 0; w < workers; ++w) {
        const int begin = w * band;
        const int end = std::min(begin + band, outH);
        if (begin >= end)
            break;
        if (w == workers - 1) {
            renderRows(eqRgba, eqW, eqH, outRgba, outW, outH, view, begin, end);
        } else {
            pool.emplace_back(renderRows, eqRgba, eqW, eqH, outRgba, outW, outH, view, begin, end,
                              nullptr, nullptr);
        }
    }
    for (auto& t : pool)
        t.join();
}

void RenderRectilinearPanes(const uint8_t* eqRgba, int eqW, int eqH, uint8_t* outRgba, int outW,
                            int outH, const std::vector<PaneView>& panes, uint32_t fillRgba,
                            const ReframeView& view)
{
    if (outW <= 0 || outH <= 0)
        return;
    uint32_t* px = reinterpret_cast<uint32_t*>(outRgba);
    std::fill(px, px + static_cast<size_t>(outW) * outH, fillRgba); // little-endian: R first
    std::vector<uint8_t> pane;
    for (const PaneView& p : panes) {
        const int x0 = std::clamp(p.x, 0, outW), y0 = std::clamp(p.y, 0, outH);
        const int w = std::min(p.w, outW - x0), h = std::min(p.h, outH - y0);
        if (w <= 0 || h <= 0)
            continue;
        ReframeView v = view;
        v.pose = p.pose;
        v.aspect = static_cast<float>(p.w) / static_cast<float>(p.h);
        // The effects against the whole frame, not the pane.
        v.frame[0] = -float(p.x);
        v.frame[1] = -float(p.y);
        v.frame[2] = float(outW);
        v.frame[3] = float(outH);
        pane.resize(static_cast<size_t>(p.w) * p.h * 4);
        RenderRectilinear(eqRgba, eqW, eqH, pane.data(), p.w, p.h, v);
        for (int row = 0; row < h; ++row)
            std::memcpy(outRgba + (static_cast<size_t>(y0 + row) * outW + x0) * 4,
                        pane.data() + static_cast<size_t>(row) * p.w * 4,
                        static_cast<size_t>(w) * 4);
    }
}

void GradeRgba(uint8_t* rgba, size_t pixels, const color::Grade& grade)
{
    if (grade.isNeutral())
        return;
    const Grade g(grade);
    auto run = [rgba, &g](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            uint8_t* p = rgba + i * 4;
            float c[4] = {float(p[0]), float(p[1]), float(p[2]), float(p[3])};
            gradeRgb(c, g);
            // lround(), exactly (graded values are >= 0), without the call.
            for (int k = 0; k < 3; ++k)
                p[k] = static_cast<uint8_t>(static_cast<double>(c[k]) + 0.5);
        }
    };
    // Across the cores: a flat clip's every frame goes through here.
    const unsigned hw = std::thread::hardware_concurrency();
    const size_t workers = pixels < 64 * 1024 ? 1 : std::max(1u, hw == 0 ? 4u : hw);
    const size_t band = (pixels + workers - 1) / workers;
    std::vector<std::thread> pool;
    for (size_t b = band; b < pixels; b += band)
        pool.emplace_back(run, b, std::min(b + band, pixels));
    run(0, std::min(band, pixels));
    for (std::thread& t : pool)
        t.join();
}

void GradeRgba(uint8_t* rgba, int w, int h, const color::Grade& grade, const float rect[4],
               int frameW, int frameH)
{
    if (!grade.hasEffects()) {
        GradeRgba(rgba, size_t(w) * h, grade);
        return;
    }
    if (w <= 0 || h <= 0 || frameW <= 0 || frameH <= 0)
        return;
    const Grade g(grade);
    const fx::Render& effects = grade.effects;
    // A picture pixel's centre in the frame: the rect scaled to the picture.
    const float sx = rect[2] / float(w), sy = rect[3] / float(h);
    if (effects.sharpens()) {
        // Graded first, then sharpened -- its radius against the frame's
        // height, in the picture's own pixels -- then the other effects.
        const size_t pixels = size_t(w) * h;
        std::vector<float> graded(pixels * 4);
        acrossRows(h, pixels, [&](int rowBegin, int rowEnd) {
            for (size_t i = size_t(rowBegin) * w; i < size_t(rowEnd) * w; ++i) {
                float* c = &graded[i * 4];
                for (int k = 0; k < 4; ++k)
                    c[k] = float(rgba[i * 4 + k]);
                gradeRgb(c, g);
            }
        });
        fx::sharpen(graded.data(), 4, nullptr, w, h, float(frameH) / sy, effects, kGradeWhite);
        finishSharpened(graded, nullptr, rgba, w, h, effects, rect[0], rect[1], sx, sy,
                        float(frameW), float(frameH));
        return;
    }
    auto run = [&](int rowBegin, int rowEnd) {
        for (int row = rowBegin; row < rowEnd; ++row) {
            const float fy = rect[1] + (float(row) + 0.5f) * sy;
            for (int col = 0; col < w; ++col) {
                uint8_t* p = rgba + (size_t(row) * w + col) * 4;
                float c[4] = {float(p[0]), float(p[1]), float(p[2]), float(p[3])};
                gradeRgb(c, g);
                fx::applyEffects(c, rect[0] + (float(col) + 0.5f) * sx, fy, float(frameW),
                                 float(frameH), effects, kGradeWhite);
                for (int k = 0; k < 3; ++k)
                    p[k] = static_cast<uint8_t>(std::clamp(c[k] + 0.5f, 0.0f, 255.0f));
            }
        }
    };
    const unsigned hw = std::thread::hardware_concurrency();
    const int workers = size_t(w) * h < 64 * 1024 ? 1 : int(std::max(1u, hw == 0 ? 4u : hw));
    const int band = (h + workers - 1) / workers;
    std::vector<std::thread> pool;
    for (int b = band; b < h; b += band)
        pool.emplace_back(run, b, std::min(b + band, h));
    run(0, std::min(band, h));
    for (std::thread& t : pool)
        t.join();
}

} // namespace exportpipe
