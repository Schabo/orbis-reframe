#include "PlateScanData.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <numbers>

namespace privacy {

namespace {

constexpr char kMagic[8] = {'O', 'R', 'P', 'L', 'A', 'T', 'E', 'S'};
constexpr uint32_t kVersion = 3; // 2: a track id per region; 3: the stitch timing per span

double halfFrameMs(double fps)
{
    return 500.0 / fps + 1.0;
}

struct FileCloser {
    void operator()(FILE* f) const
    {
        if (f)
            std::fclose(f);
    }
};
using File = std::unique_ptr<FILE, FileCloser>;

template <typename T>
bool put(FILE* f, const T& v)
{
    return std::fwrite(&v, sizeof v, 1, f) == 1;
}
template <typename T>
bool get(FILE* f, T& v)
{
    return std::fread(&v, sizeof v, 1, f) == 1;
}

// A region's centre: its corners' mean, normalised.
void centreOf(const insv::BlurQuad& q, float out[3])
{
    double c[3] = {0, 0, 0};
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 3; ++i)
            c[i] += q.corner[k][i];
    const double n = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    for (int i = 0; i < 3; ++i)
        out[i] = static_cast<float>(n > 0 ? c[i] / n : 0.0);
}

// A removal's path point every this many frames (and the last one).
constexpr int kPathStep = 3;
// A track follows a removal's path where its centre is this close.
const double kMatchCos = std::cos(2.5 * std::numbers::pi / 180.0);
// ... over at least this share of the path points within the track's time.
constexpr double kMatchShare = 0.6;

} // namespace

bool quadContains(const insv::BlurQuad& q, const float d[3])
{
    // A convex quad on the sphere: `d` lies on the same side of every edge's
    // great circle, and in the quad's hemisphere.
    float c[3];
    centreOf(q, c);
    if (c[0] * d[0] + c[1] * d[1] + c[2] * d[2] <= 0.0f)
        return false;
    int pos = 0, neg = 0;
    for (int k = 0; k < 4; ++k) {
        const float* a = q.corner[k];
        const float* b = q.corner[(k + 1) % 4];
        const float n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        const float s = n[0] * d[0] + n[1] * d[1] + n[2] * d[2];
        pos += s > 0.0f;
        neg += s < 0.0f;
    }
    return pos == 0 || neg == 0;
}

const BlurAddition::Frame* addedFrameAt(const BlurAddition& a, int64_t ptsMs, double fps)
{
    const auto it = std::lower_bound(a.frames.begin(), a.frames.end(), ptsMs,
                                     [](const BlurAddition::Frame& f, int64_t t) { return f.tMs < t; });
    const BlurAddition::Frame* best = it != a.frames.end() ? &*it : nullptr;
    if (it != a.frames.begin() && (!best || ptsMs - (it - 1)->tMs < best->tMs - ptsMs))
        best = &*(it - 1);
    return best && std::abs(static_cast<double>(best->tMs - ptsMs)) <= halfFrameMs(fps) ? best : nullptr;
}

void addedQuadsAt(const std::vector<BlurAddition>& adds, int64_t ptsMs, double fps,
                  std::vector<insv::BlurQuad>& out)
{
    for (const BlurAddition& a : adds)
        if (const BlurAddition::Frame* f = addedFrameAt(a, ptsMs, fps)) {
            insv::BlurQuad q;
            std::memcpy(q.corner, f->corner, sizeof q.corner);
            out.push_back(q);
        }
}

int addedAt(const std::vector<BlurAddition>& adds, int64_t ptsMs, double fps, const float dir[3])
{
    for (int i = static_cast<int>(adds.size()) - 1; i >= 0; --i)
        if (const BlurAddition::Frame* f = addedFrameAt(adds[i], ptsMs, fps)) {
            insv::BlurQuad q;
            std::memcpy(q.corner, f->corner, sizeof q.corner);
            if (quadContains(q, dir))
                return i;
        }
    return -1;
}

const ScanFrame* PlateScan::frameAt(int64_t ptsMs) const
{
    if (frames.empty())
        return nullptr;
    const auto it = std::lower_bound(frames.begin(), frames.end(), ptsMs,
                                     [](const ScanFrame& f, int64_t t) { return f.tMs < t; });
    const ScanFrame* best = nullptr;
    if (it != frames.end())
        best = &*it;
    if (it != frames.begin()) {
        const ScanFrame* prev = &*(it - 1);
        if (!best || ptsMs - prev->tMs < best->tMs - ptsMs)
            best = prev;
    }
    return best && std::abs(static_cast<double>(best->tMs - ptsMs)) <= halfFrameMs(fps) ? best : nullptr;
}

void PlateScan::quadsAt(int64_t ptsMs, std::vector<insv::BlurQuad>& out, const RemovedSpans* removed) const
{
    if (const ScanFrame* f = frameAt(ptsMs))
        for (const BlurRegion& r : f->regions)
            if (!isRemoved(r, f->tMs, removed))
                out.push_back(r.quad);
}

bool PlateScan::isRemoved(const BlurRegion& r, int64_t tMs, const RemovedSpans* removed)
{
    if (removed)
        for (const RemovedSpan& s : *removed)
            if (s.track == r.track && tMs >= s.fromMs && tMs <= s.toMs)
                return true;
    return false;
}

BlurRemoval PlateScan::describeRemoval(int32_t track, int64_t fromMs) const
{
    BlurRemoval rm;
    int64_t first = INT64_MAX;
    int n = 0;
    const ScanFrame* lastFrame = nullptr;
    const BlurRegion* lastRegion = nullptr;
    for (const ScanFrame& f : frames)
        for (const BlurRegion& r : f.regions) {
            if (r.track != track)
                continue;
            first = std::min(first, f.tMs);
            if (f.tMs < fromMs)
                continue;
            if (n++ % kPathStep == 0) {
                BlurRemoval::Point p;
                p.tMs = f.tMs;
                centreOf(r.quad, p.dir);
                rm.path.push_back(p);
            }
            lastFrame = &f;
            lastRegion = &r;
        }
    if (rm.path.empty())
        return rm;
    if (rm.path.back().tMs != lastFrame->tMs) {
        BlurRemoval::Point p;
        p.tMs = lastFrame->tMs;
        centreOf(lastRegion->quad, p.dir);
        rm.path.push_back(p);
    }
    rm.wholeTrack = fromMs <= first;
    rm.fromMs = rm.path.front().tMs;
    rm.toMs = rm.path.back().tMs;
    return rm;
}

RemovedSpans PlateScan::resolveRemovals(const std::vector<BlurRemoval>& removals) const
{
    RemovedSpans out;
    if (removals.empty() || frames.empty())
        return out;
    // Each track's first and last frame time.
    std::map<int32_t, std::pair<int64_t, int64_t>> extent;
    for (const ScanFrame& f : frames)
        for (const BlurRegion& r : f.regions) {
            auto [it, fresh] = extent.try_emplace(r.track, f.tMs, f.tMs);
            if (!fresh) {
                it->second.first = std::min(it->second.first, f.tMs);
                it->second.second = std::max(it->second.second, f.tMs);
            }
        }
    const int64_t h = static_cast<int64_t>(halfFrameMs(fps));
    for (const BlurRemoval& rm : removals) {
        std::vector<int64_t> framed; // the path points this scan has a frame for
        std::map<int32_t, int> close; // track -> path points it follows
        for (const BlurRemoval::Point& p : rm.path) {
            const ScanFrame* f = frameAt(p.tMs);
            if (!f)
                continue;
            framed.push_back(f->tMs);
            int32_t counted = INT32_MIN;
            for (const BlurRegion& r : f->regions) {
                float c[3];
                centreOf(r.quad, c);
                if (r.track != counted && c[0] * p.dir[0] + c[1] * p.dir[1] + c[2] * p.dir[2] >= kMatchCos) {
                    ++close[r.track];
                    counted = r.track;
                }
            }
        }
        for (const auto& [track, n] : close) {
            const auto [first, last] = extent.at(track);
            const auto inTrack = std::count_if(framed.begin(), framed.end(),
                                               [first, last](int64_t t) { return t >= first && t <= last; });
            if (inTrack == 0 || n < kMatchShare * static_cast<double>(inTrack))
                continue;
            out.push_back({track, rm.wholeTrack ? first - h : std::max(first, rm.fromMs) - h, last + h});
        }
    }
    return out;
}

bool PlateScan::regionAt(int64_t ptsMs, const float dir[3], int32_t* track) const
{
    const ScanFrame* f = frameAt(ptsMs);
    if (!f)
        return false;
    // The last region is drawn last, on top.
    for (auto it = f->regions.rbegin(); it != f->regions.rend(); ++it)
        if (quadContains(it->quad, dir)) {
            if (track)
                *track = it->track;
            return true;
        }
    return false;
}

bool PlateScan::covers(int64_t from, int64_t to) const
{
    // Walk the spans from `from`: each must start within a frame of where
    // the previous left off.
    const double h = halfFrameMs(fps);
    double reached = static_cast<double>(from);
    for (const Span& s : spans) {
        if (s.fromMs > reached + 2 * h)
            break;
        reached = std::max(reached, static_cast<double>(s.toMs));
        if (reached >= to - 2 * h)
            return true;
    }
    return false;
}

bool PlateScan::staleOver(int64_t from, int64_t to, bool correctedNow) const
{
    return std::any_of(spans.begin(), spans.end(), [&](const Span& s) {
        return s.toMs >= from && s.fromMs <= to && timingStale(s.timing, correctedNow);
    });
}

void PlateScan::merge(const PlateScan& newer)
{
    if (newer.spans.empty())
        return;
    if (frames.empty() && spans.empty()) {
        *this = newer;
        return;
    }
    const double h = halfFrameMs(newer.fps);
    const int64_t a = newer.spans.front().fromMs, b = newer.spans.back().toMs;
    // The newer scan's track ids follow ours, so they stay unique.
    int32_t nextTrack = 0;
    for (const ScanFrame& f : frames)
        for (const BlurRegion& r : f.regions)
            nextTrack = std::max(nextTrack, r.track + 1);
    // Frames: ours outside the newer span, then theirs, in time order.
    std::vector<ScanFrame> out;
    out.reserve(frames.size() + newer.frames.size());
    for (const ScanFrame& f : frames)
        if (f.tMs < a - h || f.tMs > b + h)
            out.push_back(f);
    for (ScanFrame f : newer.frames) {
        for (BlurRegion& r : f.regions)
            r.track += nextTrack;
        out.push_back(std::move(f));
    }
    std::stable_sort(out.begin(), out.end(), [](const ScanFrame& x, const ScanFrame& y) { return x.tMs < y.tMs; });
    frames = std::move(out);
    // Spans: ours cut around the newer one, plus it; touching ones join.
    std::vector<Span> sp;
    for (const Span& s : spans) {
        if (s.toMs < a - h || s.fromMs > b + h) {
            sp.push_back(s);
            continue;
        }
        // The parts of s outside [a, b] keep their frames: the last / first
        // frame of ours on each side.
        int64_t leftEnd = INT64_MIN, rightStart = INT64_MAX;
        for (const ScanFrame& f : frames) {
            if (f.tMs >= s.fromMs && f.tMs < a - h)
                leftEnd = std::max(leftEnd, f.tMs);
            if (f.tMs <= s.toMs && f.tMs > b + h)
                rightStart = std::min(rightStart, f.tMs);
        }
        if (leftEnd != INT64_MIN)
            sp.push_back({s.fromMs, leftEnd, s.timing});
        if (rightStart != INT64_MAX)
            sp.push_back({rightStart, s.toMs, s.timing});
    }
    sp.insert(sp.end(), newer.spans.begin(), newer.spans.end());
    std::sort(sp.begin(), sp.end(), [](const Span& x, const Span& y) { return x.fromMs < y.fromMs; });
    spans.clear();
    for (const Span& s : sp) {
        if (!spans.empty() && s.fromMs <= spans.back().toMs + 2 * h && s.timing == spans.back().timing)
            spans.back().toMs = std::max(spans.back().toMs, s.toMs);
        else
            spans.push_back(s);
    }
    plates += newer.plates;
    fps = newer.fps;
    model = newer.model;
}

bool savePlateScan(const PlateScan& scan, const std::string& path, std::string& err)
{
    const std::string tmp = path + ".part";
    File f(std::fopen(tmp.c_str(), "wb"));
    if (!f) {
        err = "can't write " + tmp;
        return false;
    }
    bool ok = std::fwrite(kMagic, sizeof kMagic, 1, f.get()) == 1 && put(f.get(), kVersion) &&
              put(f.get(), static_cast<uint32_t>(scan.spans.size())) && put(f.get(), scan.fps) &&
              put(f.get(), static_cast<int32_t>(scan.plates)) &&
              put(f.get(), static_cast<uint32_t>(scan.model.size())) &&
              std::fwrite(scan.model.data(), 1, scan.model.size(), f.get()) == scan.model.size() &&
              put(f.get(), static_cast<uint64_t>(scan.frames.size()));
    for (const PlateScan::Span& sp : scan.spans)
        ok = ok && put(f.get(), sp.fromMs) && put(f.get(), sp.toMs) && put(f.get(), static_cast<uint8_t>(sp.timing));
    for (const ScanFrame& fr : scan.frames) {
        if (!ok)
            break;
        ok = put(f.get(), fr.tMs) && put(f.get(), static_cast<uint32_t>(fr.regions.size()));
        for (const BlurRegion& r : fr.regions)
            ok = ok && put(f.get(), static_cast<uint8_t>(r.kind)) && put(f.get(), r.track) &&
                 put(f.get(), r.quad.corner);
    }
    ok = ok && std::fflush(f.get()) == 0;
    f.reset();
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        err = "can't write " + path;
        return false;
    }
    return true;
}

bool loadPlateScan(const std::string& path, PlateScan& scan, std::string& err)
{
    File f(std::fopen(path.c_str(), "rb"));
    if (!f) {
        err = "no scan at " + path;
        return false;
    }
    char magic[sizeof kMagic];
    uint32_t version = 0;
    if (std::fread(magic, sizeof magic, 1, f.get()) != 1 || std::memcmp(magic, kMagic, sizeof kMagic) ||
        !get(f.get(), version) || version < 2 || version > kVersion) {
        err = path + " is not a plate scan of this version";
        return false;
    }
    PlateScan s;
    int32_t plates = 0;
    uint32_t modelLen = 0, nSpans = 0;
    uint64_t nFrames = 0;
    if (!get(f.get(), nSpans) || nSpans > (1u << 20) || !get(f.get(), s.fps) || !get(f.get(), plates) ||
        !get(f.get(), modelLen) || modelLen > 4096) {
        err = path + " is truncated";
        return false;
    }
    if (!(s.fps > 0.0)) {
        err = path + " has no frame rate";
        return false;
    }
    s.plates = plates;
    s.model.resize(modelLen);
    if (std::fread(s.model.data(), 1, modelLen, f.get()) != modelLen || !get(f.get(), nFrames) ||
        nFrames > (1u << 26)) {
        err = path + " is truncated";
        return false;
    }
    s.spans.resize(nSpans);
    for (PlateScan::Span& sp : s.spans) {
        uint8_t timing = static_cast<uint8_t>(StitchTiming::Corrected); // v2: always scanned corrected
        if (!get(f.get(), sp.fromMs) || !get(f.get(), sp.toMs) || (version >= 3 && !get(f.get(), timing)) ||
            timing > 2) {
            err = path + " is truncated";
            return false;
        }
        sp.timing = static_cast<StitchTiming>(timing);
    }
    s.frames.resize(nFrames);
    for (ScanFrame& fr : s.frames) {
        uint32_t n = 0;
        if (!get(f.get(), fr.tMs) || !get(f.get(), n) || n > 4096) {
            err = path + " is truncated";
            return false;
        }
        fr.regions.resize(n);
        for (BlurRegion& r : fr.regions) {
            uint8_t kind = 0;
            if (!get(f.get(), kind) || !get(f.get(), r.track) || !get(f.get(), r.quad.corner) || kind > 3) {
                err = path + " is truncated";
                return false;
            }
            r.kind = static_cast<RegionKind>(kind);
        }
    }
    scan = std::move(s);
    return true;
}

} // namespace privacy
