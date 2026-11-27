// SPDX-License-Identifier: MIT OR Apache-2.0
#include <orbis-core/stitch/EacCubeStitcher.h>

#include <algorithm>
#include <array>
#include <cmath>

// The ray -> face -> strip map. A ray's dominant axis picks its face; the
// two other components over the dominant one are the hit on the unit cube,
// remapped equi-angularly (atan * 4/pi) into (u, v) in [-1, 1], u along the
// strip, v down it. Each face's (u, v) in terms of the ray's (ex, ey, ez)
// -- x right, y forward, z up -- was measured against FFmpeg's stitch (see
// the header); the strip then runs on across every face boundary: FRONT's
// right edge (ex = 1) is RIGHT's left (ey = 1), BACK's right edge TOP's
// left, and so on.

namespace orbiscore {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFourOverPi = 4.0 / kPi;

enum Region { kRegionLeft = 0, kRegionCenter = 1, kRegionRight = 2 };

} // namespace

bool EacCubeStitcher::configure(int streamW, int streamH, const EacLayout& layout, int outW,
                                int outH) {
    if (streamH <= 0 || outW < 2 || outH < 2 || !layout.fits(streamW))
        return false;
    stream_w_ = streamW;
    stream_h_ = streamH;
    out_w_ = outW;
    out_h_ = outH;
    center_ = layout.centre;
    side_ = layout.side;
    overlap_ = layout.overlap;
    span_ = layout.side - layout.overlap;

    // Inside a side face, where both lenses' halves reach: |u| < 2 * overlap
    // / span (each half runs on `overlap` px past the boundary at u = 0).
    // The cross-fade takes all of it.
    const double band = 2.0 * overlap_ / span_;

    px_.assign(static_cast<size_t>(outW) * outH, Pixel{});
    for (int y = 0; y < outH; ++y) {
        const double lat = 0.5 * kPi - (y + 0.5) / outH * kPi;
        const double cl = std::cos(lat), sl = std::sin(lat);
        for (int x = 0; x < outW; ++x) {
            const double lon = ((x + 0.5) / outW - 0.5) * 2.0 * kPi;
            const double px = cl * std::sin(lon), py = cl * std::cos(lon), pz = sl;
            const double ax = std::fabs(px), ay = std::fabs(py), az = std::fabs(pz);
            const double m = std::max(std::max(ax, ay), az);
            const double ex = std::atan(px / m) * kFourOverPi;
            const double ey = std::atan(py / m) * kFourOverPi;
            const double ez = std::atan(pz / m) * kFourOverPi;

            int face, stream, region;
            double u, v;
            if (ax >= std::max(ay, az)) {
                stream = 0;
                if (px > 0) { face = kRight; region = kRegionRight; u = -ey; v = -ez; }
                else        { face = kLeft;  region = kRegionLeft;  u = ey;  v = -ez; }
            } else if (ay >= az) {
                region = kRegionCenter;
                if (py > 0) { face = kFront; stream = 0; u = ex; v = -ez; }
                else        { face = kBack;  stream = 1; u = ez; v = -ex; }
            } else {
                stream = 1;
                if (pz > 0) { face = kTop;  region = kRegionRight; u = ey;  v = -ex; }
                else        { face = kDown; region = kRegionLeft;  u = -ey; v = -ex; }
            }

            Pixel& o = px_[static_cast<size_t>(y) * outW + x];
            o.stream = static_cast<uint8_t>(stream);
            o.face = static_cast<uint8_t>(face);
            o.y = static_cast<float>((v + 1.0) * 0.5 * stream_h_ - 0.5);
            if (region == kRegionCenter) {
                o.x1 = static_cast<float>(side_ + (u + 1.0) * 0.5 * center_ - 0.5);
                o.x2 = o.x1;
                o.alpha = 1.0f;
                continue;
            }
            // A side face: the half before the lens boundary (u < 0) from the
            // region's start, the half after it `overlap` px further on, each
            // running on into the overlap.
            const double base = region == kRegionLeft ? 0.0 : side_ + center_;
            const double first = base + (u + 1.0) * 0.5 * span_ - 0.5;
            const double second = first + overlap_;
            o.x1 = static_cast<float>(first);
            o.x2 = static_cast<float>(second);
            if (u <= -band)
                o.alpha = 1.0f;
            else if (u >= band)
                o.alpha = 0.0f;
            else
                o.alpha = static_cast<float>((band - u) / (2.0 * band));
        }
    }
    return true;
}

void EacCubeStitcher::renderEquirect(const Image& a, const Image& b, Image& out, int16_t* owner,
                                     bool blend) const {
    const Image* streams[2] = {&a, &b};
    out.alloc(out_w_, out_h_);
    const size_t n = static_cast<size_t>(out_w_) * out_h_;
    for (size_t i = 0; i < n; ++i) {
        const Pixel& p = px_[i];
        const Image& s = *streams[p.stream];
        std::array<float, 3> c;
        if (p.alpha >= 1.0f) {
            c = s.sample(p.x1, p.y);
        } else if (p.alpha <= 0.0f || !blend) {
            // Hard cut: each lens's half up to the boundary at u = 0.
            c = s.sample(!blend && p.alpha > 0.5f ? p.x1 : p.x2, p.y);
        } else {
            const std::array<float, 3> c1 = s.sample(p.x1, p.y);
            const std::array<float, 3> c2 = s.sample(p.x2, p.y);
            const float w = p.alpha;
            for (int k = 0; k < 3; ++k)
                c[k] = c1[k] * w + c2[k] * (1.0f - w);
        }
        uint8_t* o = &out.rgb[i * 3];
        for (int k = 0; k < 3; ++k)
            o[k] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, c[k] + 0.5f)));
        if (owner)
            owner[i] = p.face;
    }
}

double EacCubeStitcher::SeamRatio(const Image& a, const Image& b, const EacLayout& layout) {
    EacCubeStitcher st;
    if (!st.configure(a.w, a.h, layout, 512, 256) || b.w != a.w || b.h != a.h)
        return std::nan("");
    Image eq;
    std::vector<int16_t> owner(static_cast<size_t>(st.outWidth()) * st.outHeight());
    st.renderEquirect(a, b, eq, owner.data(), /*blend=*/false);

    const int W = st.outWidth(), H = st.outHeight();
    auto grey = [&](int x, int y) {
        const uint8_t* p = &eq.rgb[(static_cast<size_t>(y) * W + x) * 3];
        return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
    };
    std::vector<float> seam, inside;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x + 1 < W; ++x) {
            const size_t i = static_cast<size_t>(y) * W + x;
            if (owner[i] == owner[i + 1])
                continue;
            seam.push_back(std::fabs(grey(x + 1, y) - grey(x, y)));
            if (x + 9 < W)
                inside.push_back(std::fabs(grey(x + 9, y) - grey(x + 8, y)));
        }
    for (int x = 0; x < W; ++x)
        for (int y = 0; y + 1 < H; ++y) {
            const size_t i = static_cast<size_t>(y) * W + x;
            if (owner[i] == owner[i + W])
                continue;
            seam.push_back(std::fabs(grey(x, y + 1) - grey(x, y)));
            if (y + 9 < H)
                inside.push_back(std::fabs(grey(x, y + 9) - grey(x, y + 8)));
        }
    if (seam.size() < 50 || inside.size() < 50)
        return std::nan("");
    auto median = [](std::vector<float> v) {
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    std::vector<float> sorted = inside;
    std::sort(sorted.begin(), sorted.end());
    const float thresh = sorted[sorted.size() * 60 / 100];
    std::vector<float> busy;
    for (float g : inside)
        if (g >= thresh)
            busy.push_back(g);
    const float ref = median(busy);
    if (ref < 1e-3f)
        return std::nan("");
    return median(seam) / ref;
}

} // namespace orbiscore
