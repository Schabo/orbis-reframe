#include "InsetCompositor.h"

#include "FlatVideoReader.h"
#include "ReframeRenderer.h" // GradeRgba

#include <algorithm>
#include <cmath>

namespace exportpipe {

namespace {

// BT.709 limited range, the export's encoding.
constexpr float kKr = 0.2126f, kKb = 0.0722f, kKg = 1.0f - kKr - kKb;

struct Yuv {
    float y, u, v;
};

Yuv toYuv(float r, float g, float b) // [0,1] rgb
{
    const float l = kKr * r + kKg * g + kKb * b;
    return {16.0f + 219.0f * l, 128.0f + 224.0f * (b - l) / (2.0f * (1.0f - kKb)),
            128.0f + 224.0f * (r - l) / (2.0f * (1.0f - kKr))};
}

uint8_t clamp8(float v)
{
    return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
}

// Bilinear sample of an RGBA picture at pixel coordinates (texel centres at
// +0.5, edges clamped) -- what the viewport's linear sampler does.
void sampleBilinear(const uint8_t *rgba, int w, int h, double fx, double fy, float out[3])
{
    fx = std::clamp(fx - 0.5, 0.0, double(w - 1));
    fy = std::clamp(fy - 0.5, 0.0, double(h - 1));
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float tx = static_cast<float>(fx - x0), ty = static_cast<float>(fy - y0);
    const uint8_t *p00 = rgba + (size_t(y0) * w + x0) * 4, *p10 = rgba + (size_t(y0) * w + x1) * 4;
    const uint8_t *p01 = rgba + (size_t(y1) * w + x0) * 4, *p11 = rgba + (size_t(y1) * w + x1) * 4;
    for (int c = 0; c < 3; ++c) {
        const float top = p00[c] + (p10[c] - p00[c]) * tx;
        const float bot = p01[c] + (p11[c] - p01[c]) * tx;
        out[c] = (top + (bot - top) * ty) / 255.0f;
    }
}

} // namespace

struct InsetCompositor::State {
    std::unique_ptr<FlatVideoReader> reader;
    bool failed = false;
    // The decoded frame graded (InsetClipJob::colorGrade), when it has one.
    std::vector<uint8_t> graded;
    const VideoFrame *gradedFrom = nullptr;
    int64_t gradedPts = -1;
};

InsetCompositor::InsetCompositor(std::vector<InsetClipJob> jobs, int outW, int outH)
    : m_jobs(std::move(jobs)), m_outW(outW), m_outH(outH)
{
    for (size_t i = 0; i < m_jobs.size(); ++i)
        m_states.push_back(std::make_unique<State>());
}

InsetCompositor::~InsetCompositor() = default;

void InsetCompositor::blendNv12(const uint8_t *rgba, int picW, int picH, const inset::Rect &r,
                                float opacity, uint32_t borderRgb, uint8_t *yPlane, int yStride,
                                uint8_t *uvPlane, int uvStride, int outW, int outH)
{
    if (r.empty() || opacity <= 0.0f || picW <= 0 || picH <= 0)
        return;
    const Yuv border = toYuv(((borderRgb >> 16) & 0xff) / 255.0f, ((borderRgb >> 8) & 0xff) / 255.0f,
                             (borderRgb & 0xff) / 255.0f);
    // Chroma blocks (2x2 luma) touching the rect.
    const int x0 = std::max(0, static_cast<int>(std::floor(r.x)) & ~1);
    const int y0 = std::max(0, static_cast<int>(std::floor(r.y)) & ~1);
    const int x1 = std::min(outW, static_cast<int>(std::ceil(r.x + r.w)) + 1);
    const int y1 = std::min(outH, static_cast<int>(std::ceil(r.y + r.h)) + 1);
    const double sx = picW / r.w, sy = picH / r.h;
    for (int by = y0; by < y1; by += 2) {
        for (int bx = x0; bx < x1; bx += 2) {
            float aSum = 0.0f, uSum = 0.0f, vSum = 0.0f;
            for (int dy = 0; dy < 2; ++dy) {
                const int py = by + dy;
                if (py >= outH)
                    continue;
                for (int dx = 0; dx < 2; ++dx) {
                    const int px = bx + dx;
                    if (px >= outW)
                        continue;
                    const inset::Coverage c = inset::shade(px + 0.5, py + 0.5, r);
                    const float a = c.alpha * opacity;
                    if (a <= 0.0f)
                        continue;
                    // The picture fills the rect.
                    float rgb[3];
                    sampleBilinear(rgba, picW, picH, (px + 0.5 - r.x) * sx, (py + 0.5 - r.y) * sy, rgb);
                    Yuv col = toYuv(rgb[0], rgb[1], rgb[2]);
                    if (c.border > 0.0f) {
                        col.y += (border.y - col.y) * c.border;
                        col.u += (border.u - col.u) * c.border;
                        col.v += (border.v - col.v) * c.border;
                    }
                    uint8_t &yv = yPlane[size_t(py) * yStride + px];
                    yv = clamp8(yv + (col.y - yv) * a);
                    aSum += a;
                    uSum += a * col.u;
                    vSum += a * col.v;
                }
            }
            if (aSum <= 0.0f)
                continue;
            // The block's chroma: the average coverage, premultiplied.
            const float a = aSum / 4.0f;
            uint8_t *uv = uvPlane + size_t(by / 2) * uvStride + bx;
            uv[0] = clamp8(uv[0] * (1.0f - a) + uSum / 4.0f);
            uv[1] = clamp8(uv[1] * (1.0f - a) + vSum / 4.0f);
        }
    }
}

bool InsetCompositor::layerAt(size_t i, int64_t timelineMs, Layer &layer, std::string &err)
{
    const InsetClipJob &j = m_jobs[i];
    State &s = *m_states[i];
    if (timelineMs >= j.endMs()) {
        s.reader.reset(); // past it: free the decoder
        return false;
    }
    if (timelineMs < j.startMs || s.failed)
        return false;
    const int64_t localMs = timelineMs - j.startMs;
    const int64_t srcMs = j.trimInMs + static_cast<int64_t>(std::llround(localMs * j.speed));
    inset::Look look = j.lookAt ? j.lookAt(srcMs) : inset::Look{};
    look.opacity *= inset::fadeGain(localMs, j.spanMs(), j.fadeInMs, j.fadeOutMs);
    const inset::Rect rect = inset::place(look, j.frame, j.aspect, m_outW, m_outH);
    if (rect.empty() || look.opacity <= 0.0f)
        return false;
    if (!s.reader) {
        s.reader = std::make_unique<FlatVideoReader>();
        if (!s.reader->open(j.path, err)) {
            s.failed = true;
            return false;
        }
    }
    // Decode at the rect's size (at most the source's): sampling is then
    // ~1:1 -- no aliasing from a big source shrunk by a bilinear tap.
    const auto &info = s.reader->info();
    s.reader->setOutputSize(std::min(info.width, std::max(2, static_cast<int>(std::lround(rect.w)))),
                            std::min(info.height, std::max(2, static_cast<int>(std::lround(rect.h)))));
    const VideoFrame *f = s.reader->frameAt(srcMs);
    if (!f) {
        if (!s.reader->lastError().empty())
            err = j.path + ": " + s.reader->lastError();
        return false;
    }
    const uint8_t *rgba = f->rgba.data();
    if (j.colorGrade != std::array<float, 4>{}) {
        if (s.gradedFrom != f || s.gradedPts != f->ptsMs || s.graded.size() != f->rgba.size()) {
            s.graded = f->rgba;
            GradeRgba(s.graded.data(), size_t(f->width) * f->height, j.colorGrade.data());
            s.gradedFrom = f;
            s.gradedPts = f->ptsMs;
        }
        rgba = s.graded.data();
    }
    layer = {rgba, f->width, f->height, rect, look.opacity, j.frame.borderRgb};
    return true;
}

bool InsetCompositor::composite(int64_t timelineMs, uint8_t *y, int yStride, uint8_t *uv,
                                int uvStride, std::string &err)
{
    for (size_t i = 0; i < m_jobs.size(); ++i) {
        Layer l;
        if (layerAt(i, timelineMs, l, err))
            blendNv12(l.rgba, l.w, l.h, l.rect, l.opacity, l.borderRgb, y, yStride, uv, uvStride,
                      m_outW, m_outH);
        else if (!err.empty())
            return false;
    }
    return true;
}

bool InsetCompositor::compositeRgb(int64_t timelineMs, uint8_t *rgb, int stride,
                                   int bytesPerPixel, std::string &err)
{
    for (size_t i = 0; i < m_jobs.size(); ++i) {
        Layer l;
        if (layerAt(i, timelineMs, l, err))
            blendRgb(l.rgba, l.w, l.h, l.rect, l.opacity, l.borderRgb, rgb, stride, bytesPerPixel,
                     m_outW, m_outH);
        else if (!err.empty())
            return false;
    }
    return true;
}

void InsetCompositor::blendRgb(const uint8_t *rgba, int picW, int picH, const inset::Rect &r,
                               float opacity, uint32_t borderRgb, uint8_t *out, int stride,
                               int bpp, int outW, int outH)
{
    if (r.empty() || opacity <= 0.0f || picW <= 0 || picH <= 0)
        return;
    const float border[3] = {((borderRgb >> 16) & 0xff) / 255.0f, ((borderRgb >> 8) & 0xff) / 255.0f,
                             (borderRgb & 0xff) / 255.0f};
    const int x0 = std::max(0, static_cast<int>(std::floor(r.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(r.y)));
    const int x1 = std::min(outW, static_cast<int>(std::ceil(r.x + r.w)) + 1);
    const int y1 = std::min(outH, static_cast<int>(std::ceil(r.y + r.h)) + 1);
    const double sx = picW / r.w, sy = picH / r.h;
    for (int py = y0; py < y1; ++py) {
        uint8_t *row = out + size_t(py) * stride;
        for (int px = x0; px < x1; ++px) {
            const inset::Coverage c = inset::shade(px + 0.5, py + 0.5, r);
            const float a = c.alpha * opacity;
            if (a <= 0.0f)
                continue;
            float col[3];
            sampleBilinear(rgba, picW, picH, (px + 0.5 - r.x) * sx, (py + 0.5 - r.y) * sy, col);
            uint8_t *p = row + size_t(px) * bpp;
            for (int k = 0; k < 3; ++k) {
                const float v = col[k] + (border[k] - col[k]) * c.border;
                p[k] = clamp8(p[k] + (v * 255.0f - p[k]) * a);
            }
        }
    }
}

} // namespace exportpipe
