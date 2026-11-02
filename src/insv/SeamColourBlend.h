// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "FisheyeProjection.h" // kSeamDepthBins

#include <cstdint>
#include <vector>

// The new stitcher's seam colour blend: the two lenses' colour matched where
// they meet, measured from each frame itself.
//
// The lenses of one camera differ in colour by a few per cent, and not by
// one ratio: along the seam it follows what they see (white balance, rim
// falloff, a reflection on one side). Per frame, per stretch of the seam
// (azimuth bin) and channel, the log ratio lens 0 / lens 1 is measured from
// what both lenses see in the overlap band at the template's geometry (the
// seam cut's cells, SeamCutStats, measured with no depth table). The
// frames in the kSeamColourWindowMs before a frame are pooled
// (SeamColourWindow) and smoothed round the seam: one frame's cells are
// too few to hold the correction still. Each lens is moved half of it,
// fully near the seam, fading out by kSeamColourEndDeg from it, so neither
// lens's picture changes further away -- no whole-lens shift.
//
// Measured on Insta360 Studio's own seam colour correction (2026-11-02, X5
// clips, every 10th frame against its uncorrected stitch): per 10 degrees
// of azimuth a gain per lens, zero difference at the seam, full from a few
// degrees to ~12, gone by 25-30, changing smoothly and slowly with the
// content.
//
// Only the frames before it, so playback has them as it plays, and a seek
// measures that second before the frame it goes to (stitch::
// NativeStitchSource::SeekTo): every path stitches a frame the same way.
// Dependency-free (insv lib).

namespace insv {

struct SeamCutStats;

// Most the lenses' log ratio is matched by, per channel and bin (each lens
// gets half).
constexpr double kSeamColourMaxLog = 0.15;
// The fade across the seam: the whole correction to this far from the seam
// circle (degrees), a cosine to none at kSeamColourEndDeg.
constexpr double kSeamColourFullDeg = 12.0;
constexpr double kSeamColourEndDeg = 30.0;

// The share of the correction at elevation eRad (radians) off the seam
// circle, either side.
double SeamColourFade(double eRad);

// The frames pooled for a frame's table: those this far before it and the
// frame itself (ms), each weighted by how near it is.
constexpr int kSeamColourWindowMs = 1000;

struct SeamColourTuning {
    double smoothDeg = 6.0;     // Gaussian sigma round the seam, degrees
    double minSamples = 128.0;  // a bin's unclipped samples for a measurement of its own
    double spreadFactor = 3.0;  // a bin whose cells disagree this many times the frame's typical
    double spreadFloor = 0.02;  // ... (and by more than this, log) is left out
};

// One frame's measurement per bin and channel: the log ratio lens 0 / lens 1
// (`value`) and how much it stands on (`weight`, unclipped samples; 0 = left
// out: too few usable cells, or cells disagreeing far more than the frame's
// typical bin's do -- flare, something close crossing the seam).
struct SeamColourBins {
    float value[kSeamDepthBins][3] = {};
    float weight[kSeamDepthBins][3] = {};
};
void MeasureSeamColourBins(const SeamCutStats& stats, double bandRad, const SeamColourTuning& tuning,
                           SeamColourBins& out);

// The table from pooled measurements -- per bin and channel the sum of
// weight x value (`wv`) and of weight (`w`), bin-major RGB -- into
// table[bin * 3 + channel]: smoothed round the seam by the weights, a
// stretch with none near taking the nearest measured bins' either side,
// interpolated, none at all 0; bounded by kSeamColourMaxLog. False when no
// bin had data.
bool SolveSeamColour(const double* wv, const double* w, const SeamColourTuning& tuning,
                     float* table);

// One frame alone: MeasureSeamColourBins then SolveSeamColour.
bool MeasureSeamColour(const SeamCutStats& stats, double bandRad, const SeamColourTuning& tuning,
                       float* table);

// The frames measured up to a frame, pooled: each frame's bins weighted by
// 1 - dt / (kSeamColourWindowMs + 50) for the frames dt ms before it, within
// kSeamColourWindowMs, so a frame leaving the window changes nothing at
// once and the newest count most.
class SeamColourWindow {
public:
    explicit SeamColourWindow(SeamColourTuning tuning = {}) : m_tuning(tuning) {}

    void reset() { m_frames.clear(); }
    bool empty() const { return m_frames.empty(); }
    int64_t latestPts() const { return m_frames.empty() ? INT64_MIN : m_frames.back().pts; }
    static constexpr int64_t lookBackMs() { return kSeamColourWindowMs; }

    // A frame's measurement (frames in time order; an earlier one than the
    // last starts afresh).
    void add(int64_t ptsMs, const SeamCutStats& stats, double bandRad);
    // The table for the frame at ptsMs from it and the frames before it
    // within the window. False (table all 0) when none had data.
    bool solve(int64_t ptsMs, float* table) const;
    void dropBefore(int64_t ptsMs);

private:
    struct Frame {
        int64_t pts;
        SeamColourBins bins;
    };
    SeamColourTuning m_tuning;
    std::vector<Frame> m_frames;
};

} // namespace insv
