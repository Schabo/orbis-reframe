#include "ClipMetadata.h"

#include "Insta360Reader.h"

#include <LensDemuxer.h>

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

struct Cached {
    std::filesystem::file_time_type mtime;
    std::uintmax_t size = 0;
    std::shared_ptr<const ClipMetadata> clip;
    const CameraReader* reader = nullptr;
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
            it != g_cache.end() && it->second.mtime == mtime && it->second.size == size)
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
            Cached c{mtime, size, std::move(clip), r};
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

std::optional<insv::ImuTrack> ReadClipImu(const std::string& path) {
    const Cached c = lookup(path, nullptr, nullptr);
    if (!c.clip || !c.clip->hasImu)
        return std::nullopt;
    return c.reader->readImu(*c.clip);
}

std::optional<Thumbnail> ReadClipThumbnail(const std::string& path) {
    for (const CameraReader* r : readers())
        if (auto t = r->readThumbnail(path))
            return t;
    return std::nullopt;
}

} // namespace camera
