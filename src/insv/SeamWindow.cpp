// SPDX-License-Identifier: MIT OR Apache-2.0
#include "SeamWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace insv {

namespace {
// A frame within `halfMs` of `ptsMs` (frame times are whole milliseconds;
// one more keeps a frame exactly at the edge in, whichever way it rounded).
bool Within(int64_t framePts, int64_t ptsMs, double halfMs)
{
    return static_cast<double>(std::llabs(framePts - ptsMs)) <= halfMs + 1.0;
}
} // namespace

uint16_t FloatToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exp = static_cast<int32_t>((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffffu;
    if (((x >> 23) & 0xff) == 0xff) // inf / nan
        return static_cast<uint16_t>(sign | 0x7c00u | (mant ? 0x200u : 0u));
    if (exp >= 31)
        return static_cast<uint16_t>(sign | 0x7c00u);
    if (exp <= 0) { // subnormal or zero
        if (exp < -10)
            return static_cast<uint16_t>(sign);
        mant |= 0x800000u;
        const int shift = 14 - exp;
        uint32_t h = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (h & 1u)))
            ++h;
        return static_cast<uint16_t>(sign | h);
    }
    uint32_t h = (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u)))
        ++h; // may carry into the exponent: still right
    return static_cast<uint16_t>(sign | h);
}

float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1fu;
    uint32_t mant = h & 0x3ffu;
    uint32_t x;
    if (exp == 0) {
        if (mant == 0) {
            x = sign;
        } else { // subnormal: normalise
            int e = -1;
            do {
                ++e;
                mant <<= 1;
            } while (!(mant & 0x400u));
            x = sign | (static_cast<uint32_t>(127 - 15 - e) << 23) | ((mant & 0x3ffu) << 13);
        }
    } else if (exp == 31) {
        x = sign | 0x7f800000u | (mant << 13);
    } else {
        x = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

void QuantiseSeamState(SeamState& s)
{
    auto q = [](float& v) { v = HalfToFloat(FloatToHalf(v)); };
    for (int b = 0; b < kSeamDepthBins; ++b) {
        q(s.q[b]);
        q(s.blend.cut[b]);
        q(s.blend.fade[b]);
        for (int k = 0; k < 3; ++k) {
            q(s.blend.local[b][k]);
            q(s.blend.veil[b][k]);
        }
    }
    for (float& v : s.blend.colour)
        q(v);
    q(s.depthConf);
}

void SeamWindow::configure(double baselineM, double bandRad)
{
    m_baselineM = baselineM > 0 ? baselineM : 0.032;
    m_bandRad = bandRad;
    reset();
}

void SeamWindow::setLensColour(const LensColour& c)
{
    if (c == m_colour)
        return;
    m_colour = c;
    for (Frame& f : m_frames)
        f.residualOk = SeamFrameResidual(*f.stats, m_bandRad, m_colour, f.residual);
}

void SeamWindow::reset()
{
    m_frames.clear();
}

void SeamWindow::measureTable(int64_t ptsMs, float* q) const
{
    constexpr size_t BK = static_cast<size_t>(kSeamDepthBins) * kSeamDepthCands;
    std::vector<double> sum(BK, 0.0);
    int n = 0;
    for (const Frame& f : m_frames)
        if (f.pts < ptsMs && Within(f.pts, ptsMs, tuning.depthHalfMs)) {
            const float* d = &f.depth->data[0][0];
            for (size_t i = 0; i < BK; ++i)
                sum[i] += d[i];
            ++n;
        }
    if (n == 0) {
        std::fill(q, q + kSeamDepthBins, 0.0f);
        return;
    }
    std::vector<float> mean(BK);
    for (size_t i = 0; i < BK; ++i)
        mean[i] = static_cast<float>(sum[i] / n);
    SolveSeamDepth(mean.data(), m_baselineM, tuning.depth, q);
    for (int b = 0; b < kSeamDepthBins; ++b)
        q[b] = HalfToFloat(FloatToHalf(q[b]));
}

void SeamWindow::add(int64_t ptsMs, const SeamCosts& costs, const SeamCutStats& stats)
{
    if (!m_frames.empty() && ptsMs <= m_frames.back().pts)
        reset();
    Frame f;
    f.pts = ptsMs;
    f.depth = std::make_unique<SeamDepthFrame>();
    SeamDepthData(costs, m_baselineM, tuning.depth, *f.depth);
    f.dis.resize(static_cast<size_t>(kSeamDepthBins) * kSeamCutRows);
    SeamCutDisagreement(stats, tuning.cut, f.dis.data());
    f.stats = std::make_unique<SeamCutStats>(stats);
    f.veil.resize(kSeamDepthBins * 3);
    LensVeilPerBin(stats, f.veil.data());
    f.residualOk = SeamFrameResidual(stats, m_bandRad, m_colour, f.residual);
    m_frames.push_back(std::move(f));
}

void SeamWindow::dropBefore(int64_t ptsMs)
{
    while (!m_frames.empty() && m_frames.front().pts < ptsMs)
        m_frames.pop_front();
}

int64_t SeamWindow::lookAheadMs() const
{
    const SeamWindowTuning& T = tuning;
    return static_cast<int64_t>(std::ceil(
               std::max({T.depthHalfMs, T.cutHalfMs, T.localHalfMs, T.colourHalfMs}))) +
           1;
}

int64_t SeamWindow::lookBackMs() const
{
    // A frame's cut statistics depend on the depth table of the frames
    // before it.
    return lookAheadMs() + static_cast<int64_t>(std::ceil(tuning.depthHalfMs)) + 1;
}

bool SeamWindow::solve(int64_t ptsMs, SeamState& out) const
{
    constexpr int B = kSeamDepthBins, K = kSeamDepthCands, R = kSeamCutRows;
    const SeamWindowTuning& T = tuning;
    out = SeamState{};

    // The depth table.
    {
        std::vector<double> sum(static_cast<size_t>(B) * K, 0.0);
        double conf = 0;
        int n = 0;
        for (const Frame& f : m_frames)
            if (Within(f.pts, ptsMs, T.depthHalfMs)) {
                const float* d = &f.depth->data[0][0];
                for (size_t i = 0; i < sum.size(); ++i)
                    sum[i] += d[i];
                conf += f.depth->meanConf;
                ++n;
            }
        if (n == 0)
            return false;
        std::vector<float> mean(sum.size());
        for (size_t i = 0; i < sum.size(); ++i)
            mean[i] = static_cast<float>(sum[i] / n);
        SolveSeamDepth(mean.data(), m_baselineM, T.depth, out.q);
        out.depthConf = static_cast<float>(conf / n);
    }

    // The cut.
    int row[B];
    float width[B];
    {
        std::vector<double> sum(static_cast<size_t>(B) * R, 0.0);
        int n = 0;
        for (const Frame& f : m_frames)
            if (Within(f.pts, ptsMs, T.cutHalfMs)) {
                for (size_t i = 0; i < sum.size(); ++i)
                    sum[i] += f.dis[i];
                ++n;
            }
        std::vector<float> mean(sum.size(), 2.0f);
        if (n > 0)
            for (size_t i = 0; i < sum.size(); ++i)
                mean[i] = static_cast<float>(sum[i] / n);
        SolveSeamCut(mean.data(), m_bandRad, T.cut, row, width);
    }

    // The veiling light round the seam, and the colour residual of the
    // frames that say something about it (none: the clip's colour alone).
    {
        std::vector<double> sum(B * 3, 0.0);
        int n = 0;
        for (const Frame& f : m_frames)
            if (Within(f.pts, ptsMs, T.localHalfMs)) {
                for (int i = 0; i < B * 3; ++i)
                    sum[i] += f.veil[i];
                ++n;
            }
        if (n > 0)
            for (int b = 0; b < B; ++b)
                for (int k = 0; k < 3; ++k)
                    out.blend.veil[b][k] = static_cast<float>(sum[b * 3 + k] / n);
    }
    {
        double sum[3] = {0, 0, 0};
        int n = 0;
        for (const Frame& f : m_frames)
            if (f.residualOk && Within(f.pts, ptsMs, T.colourHalfMs)) {
                for (int k = 0; k < 3; ++k)
                    sum[k] += f.residual[k];
                ++n;
            }
        if (n > 0)
            for (int k = 0; k < 3; ++k)
                out.blend.colour[k] = static_cast<float>(sum[k] / n);
    }

    // The local correction where the lenses meet, each frame's with this
    // state's veiling light taken out and against its residual.
    {
        float meet[B], reach[B];
        SeamCutMeet(row, width, m_bandRad, meet, reach);
        const double res[3] = {out.blend.colour[0], out.blend.colour[1], out.blend.colour[2]};
        std::vector<double> sum(B * 3, 0.0);
        std::vector<float> local(B * 3);
        auto clean = std::make_unique<SeamCutStats>();
        int n = 0;
        for (const Frame& f : m_frames)
            if (Within(f.pts, ptsMs, T.localHalfMs)) {
                *clean = *f.stats;
                RemoveLensVeil(*clean, &out.blend.veil[0][0]);
                SeamLocalCorrection(*clean, m_bandRad, m_colour, res, meet, reach, T.cut,
                                    local.data());
                for (int i = 0; i < B * 3; ++i)
                    sum[i] += local[i];
                ++n;
            }
        if (n > 0)
            for (int b = 0; b < B; ++b)
                for (int k = 0; k < 3; ++k)
                    out.blend.local[b][k] = static_cast<float>(sum[b * 3 + k] / n);
    }

    SeamCutBlend(row, width, m_bandRad, out.blend);
    QuantiseSeamState(out);
    return true;
}

} // namespace insv
