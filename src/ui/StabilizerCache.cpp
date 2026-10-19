#include "StabilizerCache.h"

#include <algorithm>
#include <cmath>

#include <ClipMetadata.h> // camera::ReadClipImu

#include "ReframeExportController.h" // captureTimingFor
#include "../core/ClipItem.h"
#include "../core/Timeline.h"

namespace {

double orZero(double syncMs) { return std::isnan(syncMs) ? 0.0 : syncMs; }

} // namespace

StabilizerCache::StabilizerCache() : m_shared(std::make_shared<Shared>()) {}

PlaybackController::StabProvider StabilizerCache::provider() const
{
    return [shared = m_shared](int clipId, qint64 sourceMs, float m[9]) {
        std::lock_guard<std::mutex> lk(shared->mx);
        const auto it = shared->clips.find(clipId);
        if (it == shared->clips.end())
            return false; // clip not built yet -- keep the current matrix
        if (!it->second) {
            const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            std::copy(ident, ident + 9, m);
            return true;
        }
        float r[3][3];
        it->second->correctionMatrix(sourceMs / 1000.0, r);
        std::copy(&r[0][0], &r[0][0] + 9, m);
        return true;
    };
}

const StabilizerCache::Built *StabilizerCache::find(const ClipItem &c, double syncMs) const
{
    if (c.stabilization.mode == insv::StabMode::Off)
        return nullptr;
    syncMs = orZero(syncMs);
    for (const Built &b : m_built)
        if (b.path == c.sourcePath && b.config == c.stabilization && b.syncMs == syncMs)
            return &b;
    return nullptr;
}

bool StabilizerCache::ensure(const ClipItem &c, double syncMs)
{
    if (c.stabilization.mode == insv::StabMode::Off || c.mediaMissing)
        return false;
    if (const Built *b = find(c, syncMs)) { // most recently used goes last
        std::rotate(m_built.begin() + (b - m_built.data()),
                    m_built.begin() + (b - m_built.data()) + 1, m_built.end());
        return false;
    }
    Built built;
    built.path = c.sourcePath;
    built.config = c.stabilization;
    built.syncMs = orZero(syncMs);
    // An .insv's IMU read is a seek to the trailer + ~12 MB, not the whole
    // file -- fast enough for a clip switch / settings change on the GUI
    // thread.
    if (const auto imu = camera::ReadClipImu(c.sourcePath.toStdString())) {
        auto stab = std::make_shared<insv::GyroStabilizer>();
        stab->build(*imu, c.stabilization, built.syncMs / 1000.0);
        if (stab->ready())
            built.stab = std::move(stab);
    }
    if (m_built.size() >= kSize)
        m_built.erase(m_built.begin());
    m_built.push_back(std::move(built));
    return true;
}

std::map<int, stitch::CaptureTiming> StabilizerCache::publish(const Timeline &timeline)
{
    std::unordered_map<int, std::shared_ptr<const insv::GyroStabilizer>> clips;
    std::map<int, stitch::CaptureTiming> timings;
    for (int i = 0; i < timeline.count(); ++i) {
        const ClipItem &c = timeline.clipAt(i);
        if (c.stabilization.mode == insv::StabMode::Off) {
            clips.emplace(c.id, nullptr); // identity
            continue;
        }
        const Built *b = find(c, timeline.sourceSyncAdjust(c.sourcePath));
        if (!b)
            continue; // not built yet
        clips.emplace(c.id, b->stab);
        // The capture-time correction follows the camera's own rotation, the
        // same under every stabilization setting -- so it's there whichever
        // of a file's stabilizers is built.
        if (b->stab)
            timings.emplace(c.id, ReframeExportController::captureTimingFor(b->stab));
    }
    std::lock_guard<std::mutex> lk(m_shared->mx);
    m_shared->clips = std::move(clips);
    return timings;
}

void StabilizerCache::correctionFor(const ClipItem &c, double syncMs, qint64 sourceMs,
                                    float m[3][3]) const
{
    const float ident[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    std::copy(&ident[0][0], &ident[0][0] + 9, &m[0][0]);
    if (const Built *b = find(c, syncMs); b && b->stab)
        b->stab->correctionMatrix(sourceMs / 1000.0, m);
}
