#include "ClipMetadata.h"

#include "Insta360Reader.h"

#include <LensDemuxer.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>

namespace camera {
namespace {

// Asked in turn; the first that knows the file reads it.
const std::vector<const CameraReader*>& readers() {
    static const Insta360Reader insta360;
    static const std::vector<const CameraReader*> all = {&insta360};
    return all;
}

// A file as it was read.
struct Stamp {
    std::string path;
    std::filesystem::file_time_type mtime;
    std::uintmax_t size = 0;
};

struct Cached {
    std::filesystem::file_time_type mtime;
    std::uintmax_t size = 0;
    std::shared_ptr<const ClipMetadata> clip;
    const CameraReader* reader = nullptr;
    // The clip's other files (a lens in a file of its own): a change to one
    // is a change to the clip.
    std::vector<Stamp> others;
};

std::mutex g_mx;
std::map<std::string, Cached> g_cache;

// The file as it is now; false if it can't be looked at.
bool stat(const std::string& path, std::filesystem::file_time_type& mtime, std::uintmax_t& size) {
    std::error_code ec;
    mtime = std::filesystem::last_write_time(path, ec);
    if (ec)
        return false;
    size = std::filesystem::file_size(path, ec);
    return !ec;
}

// The cached entry for `path`, read if it's missing or the file changed.
// Reads run outside the lock; two threads may read the same new file once
// each.
Cached lookup(const std::string& path, std::string* err, ReadFailure* why) {
    if (why)
        *why = ReadFailure::Unreadable;
    std::filesystem::file_time_type mtime;
    std::uintmax_t size = 0;
    if (!stat(path, mtime, size)) {
        if (err)
            *err = "file not found";
        return {};
    }
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (auto it = g_cache.find(path);
            it != g_cache.end() && it->second.mtime == mtime && it->second.size == size &&
            std::all_of(it->second.others.begin(), it->second.others.end(), [](const Stamp& o) {
                std::filesystem::file_time_type m;
                std::uintmax_t n = 0;
                return stat(o.path, m, n) && m == o.mtime && n == o.size;
            }))
            return it->second;
    }

    std::string e;
    insv::LensDemuxer demux;
    if (!demux.open(
            path, [](const auto*, const auto*, auto*, std::string&) { return true; }, e)) {
        if (err)
            *err = "demux open failed: " + e;
        return {};
    }
    std::vector<insv::InsvStreamInfo> streams;
    for (int i = 0; i < demux.lensCount(); ++i)
        streams.push_back(demux.lens(i));
    demux.close();

    auto clip = std::make_shared<ClipMetadata>();
    clip->path = path;
    const insv::InsvStreamInfo& s0 = streams.front(); // open() fails without a video stream
    clip->lensWidth = s0.width;
    clip->lensHeight = s0.height;
    clip->fps = s0.fps;
    clip->durationMs = s0.durationMs;
    for (const CameraReader* r : readers()) {
        switch (r->read(streams, *clip, e)) {
        case CameraReader::Result::NotMine:
            continue;
        case CameraReader::Result::Failed:
            if (err)
                *err = e;
            return {};
        case CameraReader::Result::Read: {
            Cached c{mtime, size, std::move(clip), r, {}};
            for (const Lens& l : c.clip->lenses) {
                Stamp o{l.source.path, {}, 0};
                if (o.path != path &&
                    std::none_of(c.others.begin(), c.others.end(),
                                 [&](const Stamp& x) { return x.path == o.path; }) &&
                    stat(o.path, o.mtime, o.size))
                    c.others.push_back(std::move(o));
            }
            std::lock_guard<std::mutex> lk(g_mx);
            g_cache[path] = c;
            return c;
        }
        }
    }
    if (err)
        *err = "not a recognised 360 camera file";
    if (why)
        *why = ReadFailure::NotCamera;
    return {};
}

} // namespace

std::shared_ptr<const ClipMetadata> ReadClipMetadata(const std::string& path, std::string* err,
                                                     ReadFailure* why) {
    return lookup(path, err, why).clip;
}

std::string ClipFileOf(const std::string& path, std::string* err) {
    for (const CameraReader* r : readers()) {
        const std::optional<std::string> clipFile = r->clipFileFor(path);
        if (!clipFile)
            continue;
        const std::string name = std::filesystem::path(*clipFile).filename().string();
        std::error_code ec;
        if (!std::filesystem::exists(*clipFile, ec)) {
            if (err)
                *err = "part of the clip " + name + ", which isn't in the same folder";
            return {};
        }
        std::string e;
        const auto clip = ReadClipMetadata(*clipFile, &e);
        if (!clip) {
            if (err)
                *err = "part of the clip " + name + ", which can't be read (" + e + ")";
            return {};
        }
        const auto same = [&](const std::string& other) {
            std::error_code ec2;
            return !other.empty() && std::filesystem::equivalent(other, path, ec2);
        };
        bool claimed = same(clip->proxyPath);
        for (const Lens& l : clip->lenses)
            claimed = claimed || same(l.source.path);
        if (!claimed) {
            if (err)
                *err = "named as part of the clip " + name + ", which doesn't match it";
            return {};
        }
        return *clipFile;
    }
    return path;
}

std::optional<insv::ImuTrack> ReadClipImu(const std::string& path) {
    const Cached c = lookup(path, nullptr, nullptr);
    if (!c.clip || !c.clip->hasImu)
        return std::nullopt;
    return c.reader->readImu(*c.clip);
}

ClipTelemetry ReadClipTelemetry(const std::string& path) {
    ClipTelemetry out;
    const Cached c = lookup(path, nullptr, nullptr);
    if (!c.clip)
        return out;
    if (c.clip->hasImu)
        if (const auto imu = c.reader->readImu(*c.clip))
            out.motion = insv::BuildMotion(*imu);
    if (c.clip->gpsFixes == 0)
        return out;
    out.gps = c.reader->readGps(*c.clip);
    if (!out.gps || !out.gps->clockStamped || !c.clip->hasImu)
        return out;
    const auto fit = insv::AlignGpsByAcceleration(*out.gps, out.motion.forwardG,
                                                  out.motion.stepSec);
    if (!fit) {
        std::fprintf(stderr, "[camera] GPS in %s: its speed doesn't match the IMU clearly; timed "
                             "by the camera's clock alone\n",
                     path.c_str());
        return out;
    }
    std::fprintf(stderr, "[camera] GPS in %s lined up with the IMU: it ran %.2f s behind the video "
                         "at the start, %.2f s at the end\n",
                 path.c_str(), fit->shifts.front().second, fit->shifts.back().second);
    for (insv::GpsFix& f : out.gps->fixes)
        f.timeMs = fit->videoMs(f.timeMs);
    return out;
}

std::optional<Thumbnail> ReadClipThumbnail(const std::string& path) {
    for (const CameraReader* r : readers())
        if (auto t = r->readThumbnail(path))
            return t;
    return std::nullopt;
}

} // namespace camera
