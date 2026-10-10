#pragma once

// Privacy blur: the result of scanning a clip for number plates -- per frame, the regions
// to mosaic -- and its cache file.
//
// The regions are quads in the equirect (sphere) frame of the stitch, the
// frame a source's pixels live in before any stabilization: the scan levels
// the sphere with the gyro to detect, then maps every region back. So one
// scan serves every stabilization mode, reframe path and export, and the
// stitch blurs it the same way everywhere (insv::BlurQuad,
// stitch::IStitchSource::SetBlurQuads).
//
// Dependency-free (insv's BlurQuad only), so the app can always apply a scan
// it has -- only making one needs the detector (PlateScanner).

#include <BlurQuads.h> // insv::BlurQuad

#include "BlurAddition.h"
#include "BlurRemoval.h"

#include <cstdint>
#include <string>
#include <vector>

namespace privacy {

// What a region is, for the debug overlay and the stats: detected this frame;
// filled in between / around detections; followed by the quad tracker,
// verified against the plate or not.
enum class RegionKind : uint8_t { Detected = 0, Filled = 1, Tracked = 2, Unverified = 3 };

struct BlurRegion {
    insv::BlurQuad quad;
    RegionKind kind = RegionKind::Detected;
    // The plate track it belongs to: unique within its PlateScan (merge()
    // renumbers the newer scan's), not stable across rescans -- a removal
    // finds its track by path instead (BlurRemoval.h).
    int32_t track = 0;
};

// The tracks removed from a clip, as resolved against one scan: drop the
// regions of `track` over [fromMs, toMs].
struct RemovedSpan {
    int32_t track = 0;
    int64_t fromMs = 0;
    int64_t toMs = 0;
};
using RemovedSpans = std::vector<RemovedSpan>;

// Whether `dir` (a unit direction in the sphere frame) lies inside the quad.
bool quadContains(const insv::BlurQuad& q, const float dir[3]);

// Hand-added blurs (BlurAddition.h): the frame of `a` nearest `ptsMs` within
// half a frame at `fps`, else null.
const BlurAddition::Frame* addedFrameAt(const BlurAddition& a, int64_t ptsMs, double fps);
// Appends every addition's quad at `ptsMs`.
void addedQuadsAt(const std::vector<BlurAddition>& adds, int64_t ptsMs, double fps,
                  std::vector<insv::BlurQuad>& out);
// The last addition whose region at `ptsMs` contains `dir`, or -1.
int addedAt(const std::vector<BlurAddition>& adds, int64_t ptsMs, double fps, const float dir[3]);

struct ScanFrame {
    int64_t tMs = 0; // source time of the decoded frame
    std::vector<BlurRegion> regions;
};

// One source's scan: one or more scanned spans (clips cut from different
// parts of a long file are scanned separately, then merged).
struct PlateScan {
    struct Span {
        int64_t fromMs = 0; // the first and last scanned frame's source time
        int64_t toMs = 0;
        // The stitch it was scanned in (a v2 cache: Corrected).
        StitchTiming timing = StitchTiming::Corrected;
    };
    std::vector<Span> spans; // time order, disjoint
    double fps = 30.0;
    int plates = 0; // tracks kept, summed over the spans
    std::string model; // the detector model's file name, for the record
    // Every scanned frame, in time order (empty regions included).
    std::vector<ScanFrame> frames;

    bool empty() const { return frames.empty(); }
    // The frame nearest `ptsMs` within half a frame, else null.
    const ScanFrame* frameAt(int64_t ptsMs) const;
    // Appends the quads for the frame at `ptsMs` (none outside the scan),
    // leaving out those `removed` covers.
    void quadsAt(int64_t ptsMs, std::vector<insv::BlurQuad>& out, const RemovedSpans* removed = nullptr) const;
    // Whether `removed` covers region `r` of the frame at `tMs`.
    static bool isRemoved(const BlurRegion& r, int64_t tMs, const RemovedSpans* removed);

    // Manual removal (BlurRemoval.h). The removal of `track` from `fromMs`
    // to its last region (fromMs <= its first region's time = all of it);
    // an empty path when the track has no region there.
    BlurRemoval describeRemoval(int32_t track, int64_t fromMs) const;
    // The tracks of this scan the removals follow: per removal, every track
    // with regions in its range whose centre stays within ~2.5 deg of the
    // removal's path over most of the samples this scan has frames for. A
    // removal that matches no track (rescanned away, or scanned elsewhere)
    // resolves to nothing.
    RemovedSpans resolveRemovals(const std::vector<BlurRemoval>& removals) const;
    // The topmost region at `ptsMs` containing `dir` (sphere frame); its
    // track in *track. False when none does.
    bool regionAt(int64_t ptsMs, const float dir[3], int32_t* track) const;
    // The scanned spans reach over [fromMs, toMs] (to within a frame).
    bool covers(int64_t fromMs, int64_t toMs) const;
    // Whether a span overlapping [fromMs, toMs] was scanned in a stitch other
    // than the one `correctedNow` gives (timingStale), so its regions may sit
    // off the plates.
    bool staleOver(int64_t fromMs, int64_t toMs, bool correctedNow) const;
    // Fold in a newer scan of (part of) the same source: its span replaces
    // whatever this one had there; spans that touch join when scanned in the
    // same stitch.
    void merge(const PlateScan& newer);
};

// The cache file (binary, versioned). False with `err` on failure; a file of
// another version (v1, or newer) reads as an error, so a stale cache is
// rescanned. A v2 file reads with every span Corrected (how v2 scanned).
bool savePlateScan(const PlateScan& scan, const std::string& path, std::string& err);
bool loadPlateScan(const std::string& path, PlateScan& scan, std::string& err);

} // namespace privacy
