#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <LensColour.h>     // insv::LensColour
#include <SeamCorrection.h> // insv::SeamCorrection
#include <SeamWindow.h>     // insv::SeamState

#include "IStitchSource.h" // stitch::PreviewStitchConfig::LensGuard

// The depth-aware seam's state for every frame of a clip, measured once in a
// pass over it and kept: insv::SeamWindow's solve over the frames either side
// of each one, which a frame played into can't have (the frames after it
// aren't decoded yet) and a frame seeked to wouldn't (no frames before it).
// With it, every path that stitches a frame -- playback, a step back, a
// screenshot, an export -- stitches it the same way, and playback skips the
// seam measurement.
//
// The states are kept per clip and stitch set-up (SeamAnalysisKey) in a file
// in the cache folder (SetSeamAnalysisDir): half floats, ~2.3 KB a frame,
// appended as a pass goes, so a pass that stops (playback starts) goes on
// from where it was; regenerable, so nothing of it is in the project. Only
// an index of the frames is held in memory. NativeStitchSource looks its
// clip's analysis up by itself (SeamAnalysisFor) and stitches a frame the
// analysis has with its state, else it measures the seam as it plays.

namespace stitch {

// Raised when the seam's solve or measurement changes: older analyses are
// done again.
constexpr int kSeamAnalysisVersion = 1;

// What a clip's seam states depend on.
struct SeamAnalysisKey {
    std::string path;
    PreviewStitchConfig::LensGuard guard = PreviewStitchConfig::LensGuard::Premium;
    insv::SeamCorrection seam;
    insv::LensColour colour;

    // Canonical text of it with the file's size and time and
    // kSeamAnalysisVersion: two keys are the same analysis when it is.
    std::string id() const;
};

// One clip's analysed frames. Thread-safe.
class SeamAnalysis {
public:
    // `file` empty: held in memory only.
    SeamAnalysis(std::string id, std::string file);
    ~SeamAnalysis();
    SeamAnalysis(const SeamAnalysis&) = delete;
    SeamAnalysis& operator=(const SeamAnalysis&) = delete;

    // The state of the frame at ptsMs, if analysed.
    bool stateAt(int64_t ptsMs, insv::SeamState& out) const;
    // Whether every frame in [inMs, outMs] is analysed; the analysed share of
    // it (0..1).
    bool covers(int64_t inMs, int64_t outMs) const;
    double coverage(int64_t inMs, int64_t outMs) const;
    // The stretches of [inMs, outMs] not analysed yet.
    std::vector<std::pair<int64_t, int64_t>> missing(int64_t inMs, int64_t outMs) const;

    // A pass's results: a frame's state, and a stretch every frame of which
    // it has put.
    void put(int64_t ptsMs, const insv::SeamState& s);
    void markDone(int64_t inMs, int64_t outMs);
    // Writes what is buffered to the file.
    void flush();

    const std::string& id() const { return m_id; }

private:
    bool load();
    bool create(); // the file, new, with its header
    void appendRecord(uint8_t type, const void* data, size_t size); // under m_mx
    // Held in memory: no cache folder, or the file can't be written.
    bool inMemory() const { return m_file.empty() || m_failed; }

    std::string m_id, m_file;
    mutable std::mutex m_mx;
    std::FILE* m_fp = nullptr; // the cache file, read and appended; made on the first write
    bool m_failed = false;     // it can't be written
    std::map<int64_t, long> m_index;                       // pts -> record offset in m_fp
    std::map<int64_t, std::vector<uint16_t>> m_memory;     // pts -> state, without a file
    std::vector<std::pair<int64_t, int64_t>> m_done;       // analysed stretches, merged
};

// Where analyses are kept: a folder (made when needed), or "" for memory
// only. Analyses already handed out keep where they are.
void SetSeamAnalysisDir(const std::string& dir);

// The analysis for `key`: the one already open, else the one in the cache
// folder, else a new empty one.
std::shared_ptr<SeamAnalysis> SeamAnalysisFor(const SeamAnalysisKey& key);

struct SeamAnalysisResult {
    bool ok = false;
    bool cancelled = false;
    bool noGpu = false;  // gpuOnly, and there is no all-GPU path
    std::string message; // what was done, or why not, for the log
};

// Analyses the frames of [inMs, outMs] that `key`'s analysis doesn't have
// yet: decodes each missing stretch from SeamWindow::lookBackMs() before it
// to lookAheadMs() after, measures every frame's seam (on the GPU when it
// can) and puts each frame's state. `cancel` (optional) is polled every
// frame: what is done so far is kept. `progress` (optional) gets the
// covered share of [inMs, outMs] now and then.
// `gpuOnly`: not at all without the all-GPU decode and stitch (the CPU
// measure is too slow to run over whole clips in the background).
SeamAnalysisResult AnalyseClipSeam(const SeamAnalysisKey& key, int64_t inMs, int64_t outMs,
                                   const std::atomic<bool>* cancel = nullptr,
                                   const std::function<void(double)>& progress = {},
                                   bool gpuOnly = false);

} // namespace stitch
