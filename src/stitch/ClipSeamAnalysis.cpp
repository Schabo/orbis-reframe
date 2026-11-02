#include "ClipSeamAnalysis.h"

#include "NativeStitchSource.h"

#include "../core/Log.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstring>
#include <deque>
#include <filesystem>
#include <system_error>

#include <unistd.h> // ftruncate

namespace stitch {

namespace {

constexpr char kMagic[8] = {'O', 'R', 'B', 'S', 'E', 'A', 'M', '\n'};
constexpr uint8_t kFrameRecord = 1;
constexpr uint8_t kDoneRecord = 2;
constexpr int kB = insv::kSeamDepthBins;
// q, cut, fade, local RGB, veil RGB per bin; the residual RGB; the depth
// confidence.
constexpr size_t kStateHalves = static_cast<size_t>(kB) * 9 + 3 + 1;
constexpr size_t kFrameBytes = 8 + kStateHalves * 2;

void Pack(const insv::SeamState& s, uint16_t* h)
{
    size_t i = 0;
    for (int b = 0; b < kB; ++b) {
        h[i++] = insv::FloatToHalf(s.q[b]);
        h[i++] = insv::FloatToHalf(s.blend.cut[b]);
        h[i++] = insv::FloatToHalf(s.blend.fade[b]);
        for (int k = 0; k < 3; ++k)
            h[i++] = insv::FloatToHalf(s.blend.local[b][k]);
        for (int k = 0; k < 3; ++k)
            h[i++] = insv::FloatToHalf(s.blend.veil[b][k]);
    }
    for (int k = 0; k < 3; ++k)
        h[i++] = insv::FloatToHalf(s.blend.colour[k]);
    h[i++] = insv::FloatToHalf(s.depthConf);
}

void Unpack(const uint16_t* h, insv::SeamState& s)
{
    size_t i = 0;
    for (int b = 0; b < kB; ++b) {
        s.q[b] = insv::HalfToFloat(h[i++]);
        s.blend.cut[b] = insv::HalfToFloat(h[i++]);
        s.blend.fade[b] = insv::HalfToFloat(h[i++]);
        for (int k = 0; k < 3; ++k)
            s.blend.local[b][k] = insv::HalfToFloat(h[i++]);
        for (int k = 0; k < 3; ++k)
            s.blend.veil[b][k] = insv::HalfToFloat(h[i++]);
    }
    for (int k = 0; k < 3; ++k)
        s.blend.colour[k] = insv::HalfToFloat(h[i++]);
    s.depthConf = insv::HalfToFloat(h[i++]);
}

// 64-bit FNV-1a, for the cache file's name.
uint64_t Fnv1a(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

void Merge(std::vector<std::pair<int64_t, int64_t>>& v)
{
    std::sort(v.begin(), v.end());
    std::vector<std::pair<int64_t, int64_t>> out;
    for (const auto& r : v) {
        if (!out.empty() && r.first <= out.back().second)
            out.back().second = std::max(out.back().second, r.second);
        else
            out.push_back(r);
    }
    v = std::move(out);
}

struct Registry {
    std::mutex mx;
    std::string dir;
    std::map<std::string, std::shared_ptr<SeamAnalysis>> open;
};

Registry& TheRegistry()
{
    static Registry r;
    return r;
}

} // namespace

std::string SeamAnalysisKey::id() const
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    const auto mtime = std::filesystem::last_write_time(path, ec);
    const long long ticks = ec ? 0 : static_cast<long long>(mtime.time_since_epoch().count());
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "v%d|%s|%llu|%lld|g%d|seam %.17g %.17g %.17g %.17g %.17g %.17g|"
                  "colour %.17g %.17g %.17g %.17g %.17g %.17g %.17g",
                  kSeamAnalysisVersion, path.c_str(), static_cast<unsigned long long>(ec ? 0 : size),
                  ticks, static_cast<int>(guard), seam.lens1RotDeg[0], seam.lens1RotDeg[1],
                  seam.lens1RotDeg[2], seam.lens1Centre[0], seam.lens1Centre[1], seam.radialScale,
                  colour.logRatio[0], colour.logRatio[1], colour.logRatio[2], colour.slope[0],
                  colour.slope[1], colour.slope[2], colour.spread);
    std::string out = buf;
    if (calibration) {
        // Which calibration: when it was made, and its lenses' focal lengths
        // (a recalibration changes both).
        std::snprintf(buf, sizeof(buf), "|cal %s %s %.17g %.17g", calibration->serial.c_str(),
                      calibration->created.c_str(), calibration->lenses[0].fx,
                      calibration->lenses[1].fx);
        out += buf;
    }
    return out;
}

SeamAnalysis::SeamAnalysis(std::string id, std::string file)
    : m_id(std::move(id)), m_file(std::move(file))
{
    if (!m_file.empty())
        load();
}

SeamAnalysis::~SeamAnalysis()
{
    if (m_fp)
        std::fclose(m_fp);
}

bool SeamAnalysis::create()
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(m_file).parent_path(), ec);
    m_fp = std::fopen(m_file.c_str(), "w+b");
    if (!m_fp)
        return false;
    const uint32_t n = static_cast<uint32_t>(m_id.size());
    std::fwrite(kMagic, 1, sizeof kMagic, m_fp);
    std::fwrite(&n, 4, 1, m_fp);
    std::fwrite(m_id.data(), 1, n, m_fp);
    return std::fflush(m_fp) == 0;
}

bool SeamAnalysis::load()
{
    // Made on the first write: a clip only looked up leaves no file.
    m_fp = std::fopen(m_file.c_str(), "r+b");
    if (!m_fp)
        return true;
    char magic[8];
    uint32_t n = 0;
    std::string id;
    if (std::fread(magic, 1, 8, m_fp) == 8 && std::memcmp(magic, kMagic, 8) == 0 &&
        std::fread(&n, 4, 1, m_fp) == 1 && n < 4096) {
        id.resize(n);
        if (std::fread(id.data(), 1, n, m_fp) != n)
            id.clear();
    }
    if (id != m_id) { // another analysis, or not one at all: written over on the first write
        std::fclose(m_fp);
        m_fp = nullptr;
        return true;
    }
    // The records; a torn one at the end (the app stopped mid-write) is cut
    // off, so what is written next follows the last whole one.
    long good = std::ftell(m_fp);
    for (;;) {
        const long at = std::ftell(m_fp);
        uint8_t type = 0;
        if (std::fread(&type, 1, 1, m_fp) != 1)
            break;
        int64_t v[2];
        if (type == kFrameRecord) {
            if (std::fread(&v[0], 8, 1, m_fp) != 1 ||
                std::fseek(m_fp, static_cast<long>(kStateHalves * 2), SEEK_CUR) != 0)
                break;
            // fseek past the end succeeds: check the record is all there.
            const long end = std::ftell(m_fp);
            std::fseek(m_fp, 0, SEEK_END);
            if (std::ftell(m_fp) < end) {
                std::fseek(m_fp, at, SEEK_SET);
                break;
            }
            std::fseek(m_fp, end, SEEK_SET);
            m_index[v[0]] = at + 1 + 8;
        } else if (type == kDoneRecord) {
            if (std::fread(v, 8, 2, m_fp) != 2)
                break;
            m_done.emplace_back(v[0], v[1]);
        } else {
            break;
        }
        good = std::ftell(m_fp);
    }
    std::fseek(m_fp, 0, SEEK_END);
    if (std::ftell(m_fp) != good) {
        std::fflush(m_fp);
        if (ftruncate(fileno(m_fp), good) != 0) {
            std::fclose(m_fp);
            m_fp = nullptr;
            m_failed = true; // can't append behind it: memory from here
            m_index.clear();
            m_done.clear();
        }
    }
    Merge(m_done);
    return true;
}

void SeamAnalysis::appendRecord(uint8_t type, const void* data, size_t size)
{
    if (!m_fp) {
        if (m_file.empty() || m_failed)
            return;
        if (!create()) {
            m_failed = true;
            core::log::Info("[seam analysis] can't write %s; keeping it in memory\n", m_file.c_str());
            return;
        }
    }
    std::fseek(m_fp, 0, SEEK_END);
    std::fwrite(&type, 1, 1, m_fp);
    std::fwrite(data, 1, size, m_fp);
}

bool SeamAnalysis::stateAt(int64_t ptsMs, insv::SeamState& out) const
{
    std::lock_guard<std::mutex> lk(m_mx);
    std::vector<uint16_t> h(kStateHalves);
    if (inMemory()) {
        auto it = m_memory.lower_bound(ptsMs - 1);
        if (it == m_memory.end() || it->first > ptsMs + 1)
            return false;
        Unpack(it->second.data(), out);
        return true;
    }
    auto it = m_index.lower_bound(ptsMs - 1);
    if (!m_fp || it == m_index.end() || it->first > ptsMs + 1)
        return false;
    if (std::fseek(m_fp, it->second, SEEK_SET) != 0 ||
        std::fread(h.data(), 2, kStateHalves, m_fp) != kStateHalves)
        return false;
    Unpack(h.data(), out);
    return true;
}

bool SeamAnalysis::covers(int64_t inMs, int64_t outMs) const
{
    std::lock_guard<std::mutex> lk(m_mx);
    for (const auto& r : m_done)
        if (r.first <= inMs && r.second >= outMs)
            return true;
    return false;
}

double SeamAnalysis::coverage(int64_t inMs, int64_t outMs) const
{
    if (outMs <= inMs)
        return covers(inMs, outMs) ? 1.0 : 0.0;
    std::lock_guard<std::mutex> lk(m_mx);
    int64_t sum = 0;
    for (const auto& r : m_done)
        sum += std::max<int64_t>(0, std::min(r.second, outMs) - std::max(r.first, inMs));
    return std::min(1.0, static_cast<double>(sum) / static_cast<double>(outMs - inMs));
}

std::vector<std::pair<int64_t, int64_t>> SeamAnalysis::missing(int64_t inMs, int64_t outMs) const
{
    std::lock_guard<std::mutex> lk(m_mx);
    std::vector<std::pair<int64_t, int64_t>> gaps;
    int64_t at = inMs;
    for (const auto& r : m_done) {
        if (r.second < at)
            continue;
        if (r.first > outMs)
            break;
        if (r.first > at)
            gaps.emplace_back(at, std::min(r.first, outMs));
        // From the last frame done again: frames are some ms apart, so
        // stretches done in separate passes overlap rather than leave a gap.
        at = std::max(at, r.second);
        if (at >= outMs)
            return gaps;
    }
    if (at < outMs || (gaps.empty() && m_done.empty()))
        gaps.emplace_back(at, outMs);
    return gaps;
}

void SeamAnalysis::put(int64_t ptsMs, const insv::SeamState& s)
{
    std::vector<uint16_t> h(kStateHalves);
    Pack(s, h.data());
    std::lock_guard<std::mutex> lk(m_mx);
    if (m_index.count(ptsMs) || m_memory.count(ptsMs))
        return; // the same state again
    std::vector<uint8_t> rec(kFrameBytes);
    std::memcpy(rec.data(), &ptsMs, 8);
    std::memcpy(rec.data() + 8, h.data(), kStateHalves * 2);
    appendRecord(kFrameRecord, rec.data(), rec.size());
    if (inMemory())
        m_memory[ptsMs] = std::move(h);
    else
        m_index[ptsMs] = std::ftell(m_fp) - static_cast<long>(kStateHalves * 2);
}

void SeamAnalysis::markDone(int64_t inMs, int64_t outMs)
{
    std::lock_guard<std::mutex> lk(m_mx);
    const int64_t v[2] = {inMs, outMs};
    appendRecord(kDoneRecord, v, sizeof v);
    if (m_fp)
        std::fflush(m_fp);
    m_done.emplace_back(inMs, outMs);
    Merge(m_done);
}

void SeamAnalysis::flush()
{
    std::lock_guard<std::mutex> lk(m_mx);
    if (m_fp)
        std::fflush(m_fp);
}

void SetSeamAnalysisDir(const std::string& dir)
{
    Registry& r = TheRegistry();
    std::lock_guard<std::mutex> lk(r.mx);
    if (dir == r.dir)
        return;
    r.dir = dir;
    r.open.clear(); // the next lookups open this folder's
}

std::shared_ptr<SeamAnalysis> SeamAnalysisFor(const SeamAnalysisKey& key)
{
    const std::string id = key.id();
    Registry& r = TheRegistry();
    std::lock_guard<std::mutex> lk(r.mx);
    auto it = r.open.find(id);
    if (it != r.open.end())
        return it->second;
    std::string file;
    if (!r.dir.empty()) {
        char name[32];
        std::snprintf(name, sizeof(name), "%016" PRIx64 ".seam", Fnv1a(id));
        file = (std::filesystem::path(r.dir) / name).string();
    }
    auto a = std::make_shared<SeamAnalysis>(id, file);
    r.open.emplace(id, a);
    return a;
}

namespace {

// The id in a cache file's header, or "".
std::string FileId(const std::filesystem::path& p)
{
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f)
        return {};
    char magic[8];
    uint32_t n = 0;
    std::string id;
    if (std::fread(magic, 1, 8, f) == 8 && std::memcmp(magic, kMagic, 8) == 0 &&
        std::fread(&n, 4, 1, f) == 1 && n < 4096) {
        id.resize(n);
        if (std::fread(id.data(), 1, n, f) != n)
            id.clear();
    }
    std::fclose(f);
    return id;
}

// The clip in an id ("v1|<path>|<size>|..."), or "".
std::string IdPath(const std::string& id)
{
    const auto a = id.find('|'), b = id.find('|', a == std::string::npos ? a : a + 1);
    return a == std::string::npos || b == std::string::npos ? std::string() : id.substr(a + 1, b - a - 1);
}

// Removes the cache folder's other analyses of `id`'s clip that nothing has
// open: those of an earlier seam correction, lens colour, lens guard or
// version, which nothing will read again.
void PruneSuperseded(const std::string& id)
{
    Registry& r = TheRegistry();
    std::lock_guard<std::mutex> lk(r.mx);
    if (r.dir.empty())
        return;
    const std::string path = IdPath(id);
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(r.dir, ec)) {
        if (e.path().extension() != ".seam")
            continue;
        const std::string other = FileId(e.path());
        if (other.empty() || other == id || IdPath(other) != path || r.open.count(other))
            continue;
        std::filesystem::remove(e.path(), ec);
        core::log::Info("[seam analysis] removed an older analysis of %s\n", path.c_str());
    }
}

} // namespace

SeamAnalysisResult AnalyseClipSeam(const SeamAnalysisKey& key, int64_t inMs, int64_t outMs,
                                   const std::atomic<bool>* cancel,
                                   const std::function<void(double)>& progress, bool gpuOnly)
{
    SeamAnalysisResult res;
    const auto t0 = std::chrono::steady_clock::now();
    std::shared_ptr<SeamAnalysis> analysis = SeamAnalysisFor(key);
    const auto gaps = analysis->missing(inMs, outMs);
    if (gaps.empty()) {
        res.ok = true;
        res.message = "already analysed";
        return res;
    }

    PreviewStitchConfig config;
    config.width = 64; // nothing is stitched
    config.height = 32;
    config.lensGuard = key.guard;
    config.lensCalibration = key.calibration;
    config.method = PreviewStitchConfig::Method::DepthSeam;
    NativeStitchSource src;
    if (!src.Open(key.path, config)) {
        res.message = "open failed: " + src.GetLastError();
        return res;
    }
    if (gpuOnly && !src.AllGpu()) {
        res.noGpu = true;
        res.message = "no GPU decode and stitch: measured as it plays";
        return res;
    }
    src.SetSeamCorrection(key.seam);
    src.SetLensColour(key.colour);
    src.SetSeamAnalysisEnabled(false); // measure every frame, whatever is analysed
    insv::SeamWindow w;
    src.ConfigureSeamWindow(w);

    int frames = 0;
    auto lastReport = std::chrono::steady_clock::now();
    for (const auto& [a, b] : gaps) {
        if (!src.SeekTo(std::max<int64_t>(0, a - w.lookBackMs()))) {
            res.message = "seek failed: " + src.GetLastError();
            return res;
        }
        w.reset();
        // Frames of [a, b] measured, not solved yet, and the first after b:
        // a time between two frames shows the one after it.
        std::deque<int64_t> pending;
        bool afterTaken = false;
        int64_t lastSolved = INT64_MIN;
        bool eof = false;
        for (;;) {
            if (cancel && *cancel) {
                res.cancelled = true;
                break;
            }
            int64_t pts = 0;
            if (src.MeasureNextSeamFrame(w, pts)) {
                if (pts >= a && (pts <= b || !afterTaken)) {
                    afterTaken = pts > b;
                    pending.push_back(pts);
                }
            } else {
                if (!src.GetLastError().empty()) {
                    res.message = "decode failed: " + src.GetLastError();
                    if (lastSolved >= a)
                        analysis->markDone(a, lastSolved);
                    return res;
                }
                eof = true;
            }
            // A frame is solved once the frames lookAheadMs() after it are in.
            while (!pending.empty() && (eof || pending.front() + w.lookAheadMs() <= w.latestPts())) {
                insv::SeamState s;
                w.solve(pending.front(), s);
                analysis->put(pending.front(), s);
                lastSolved = pending.front();
                pending.pop_front();
                ++frames;
            }
            if (!w.empty())
                w.dropBefore((pending.empty() ? w.latestPts() : pending.front()) - w.lookBackMs());
            if (eof || (pending.empty() && afterTaken))
                break;
            if (progress && std::chrono::steady_clock::now() - lastReport > std::chrono::milliseconds(500)) {
                lastReport = std::chrono::steady_clock::now();
                analysis->flush();
                progress(analysis->coverage(inMs, outMs) +
                         (lastSolved >= a ? static_cast<double>(lastSolved - a) / std::max<int64_t>(1, outMs - inMs) : 0.0));
            }
        }
        if (res.cancelled) {
            if (lastSolved >= a)
                analysis->markDone(a, lastSolved);
            break;
        }
        analysis->markDone(a, eof ? std::max(b, lastSolved) : b);
    }
    analysis->flush();
    if (progress)
        progress(analysis->coverage(inMs, outMs));
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    char msg[160];
    std::snprintf(msg, sizeof(msg), "%d frames analysed in %.1f s%s", frames, secs,
                  res.cancelled ? " (stopped)" : "");
    res.message = msg;
    res.ok = !res.cancelled;
    if (res.ok)
        PruneSuperseded(analysis->id());
    return res;
}

} // namespace stitch
