#pragma once

#include <QString>

#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <GyroStabilizer.h> // insv::GyroStabilizer / StabConfig
#include <IStitchSource.h>  // stitch::CaptureTiming

#include "PlaybackController.h" // PlaybackController::StabProvider

#include "../core/ClipItem.h"

class Timeline;

// Native gyro stabilization, per clip (ClipItem::stabilization), for the
// preview. Built stabilizers are kept per source file + setting + sync
// fine-tune, so clips cut from one file with the same setting share one;
// MainWindow builds the clip under the playhead and the next one ahead.
// Presented frames get their matrix through provider() (per frame, by the
// frame's clip, at its own source time, on the playback thread), the stitch
// its capture-time correction from publish()'s timings; the cache itself
// also serves the paused / settings-changed refresh (correctionFor()).
class StabilizerCache {
public:
    StabilizerCache();

    // Whether the stitch has the capture-time correction for `c` (its
    // stabilization on) -- as publish() hands it to playback and the export
    // builds it. A plate scan / followed plate is found in that stitch.
    static bool captureTimingOn(const ClipItem &c)
    {
        return c.stabilization.mode != insv::StabMode::Off;
    }

    // For PlaybackController::setStabilizationProvider: a clip that publish()
    // hasn't seen built yet keeps the viewport's current matrix.
    PlaybackController::StabProvider provider() const;

    // Build clip `c`'s stabilizer (its source's IMU track under its own
    // stabilization setting and the source's sync fine-tune `syncMs`, NaN =
    // none), unless one is there already or the clip's stabilization is Off
    // or its file missing. Reads the clip's trailer (fast: it seeks to the
    // trailer, not the whole file). Returns true if it built one; the caller
    // then publishes.
    bool ensure(const ClipItem &c, double syncMs);
    // Hand every timeline clip's built stabilizer to provider(); returns
    // their capture-time corrections for PlaybackController::setCaptureTimings.
    // Call after the cache or the clips change.
    std::map<int, stitch::CaptureTiming> publish(const Timeline &timeline);
    // Clip `c`'s correction at `sourceMs` (its source time) into `m`;
    // identity when its stabilization is Off or its stabilizer isn't built.
    void correctionFor(const ClipItem &c, double syncMs, qint64 sourceMs, float m[3][3]) const;

private:
    struct Built {
        QString path;
        insv::StabConfig config;
        double syncMs = 0.0;
        std::shared_ptr<const insv::GyroStabilizer> stab; // null: no IMU track
    };
    static constexpr size_t kSize = 4;
    std::vector<Built> m_built; // least recently used first
    // The entry for clip `c`, or null (stabilization Off, or not built).
    const Built *find(const ClipItem &c, double syncMs) const;

    // What provider() reads on the playback thread: per clip id, its
    // stabilizer, or null for identity (Off, or no IMU track).
    struct Shared {
        std::mutex mx;
        std::unordered_map<int, std::shared_ptr<const insv::GyroStabilizer>> clips;
    };
    std::shared_ptr<Shared> m_shared;
};
