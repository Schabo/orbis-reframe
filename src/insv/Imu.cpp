// SPDX-License-Identifier: MIT OR Apache-2.0
#include "Imu.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace insv {

namespace {

// The track's earliest time, which its own times count from; UINT64_MAX when
// it has nothing timed.
uint64_t earliest(const ImuTrack& t)
{
    uint64_t e = UINT64_MAX;
    for (const ImuSample& s : t.samples)
        e = std::min(e, s.timestamp);
    if (t.samples.empty())
        for (const OrientationSample& o : t.orientation)
            e = std::min(e, o.timestamp);
    return e;
}

template <class Sample>
uint64_t firstOf(const std::vector<Sample>& v)
{
    uint64_t e = UINT64_MAX;
    for (const Sample& s : v)
        e = std::min(e, s.timestamp);
    return e;
}

} // namespace

ImuTrack JoinImuTracks(std::vector<ImuTrackPart>&& parts)
{
    if (parts.empty())
        return {};
    if (parts.size() == 1)
        return std::move(parts.front().track);

    const ImuTrack& first = parts.front().track;
    ImuTrack out;
    out.axisMap = first.axisMap;
    out.hasOffset = first.hasOffset;
    out.captureScale = first.captureScale;
    out.clockRate = first.clockRate;
    out.readoutSec = first.readoutSec;

    uint64_t t0 = UINT64_MAX;
    for (const ImuTrackPart& p : parts)
        t0 = std::min(t0, earliest(p.track));
    if (t0 == UINT64_MAX)
        return std::move(parts.front().track);

    for (size_t k = 0; k < parts.size(); ++k) {
        const ImuTrack& t = parts[k].track;
        const bool last = k + 1 == parts.size();
        const uint64_t sampleEnd = last ? UINT64_MAX : firstOf(parts[k + 1].track.samples);
        const uint64_t orientationEnd = last ? UINT64_MAX : firstOf(parts[k + 1].track.orientation);
        for (const ImuSample& s : t.samples)
            if (s.timestamp < sampleEnd)
                out.samples.push_back(s);
        for (const OrientationSample& o : t.orientation)
            if (o.timestamp < orientationEnd)
                out.orientation.push_back(o);

        const uint64_t own = earliest(t);
        if (own == UINT64_MAX || !t.hasOffset) {
            std::fprintf(stderr, "insv: part %zu of the clip has no motion data lined up with its "
                                 "video; its frames follow the part before\n",
                         k + 1);
            continue;
        }
        // The part's own times count from its earliest sample.
        const double shift = static_cast<double>(own - t0) * 1e-6;
        const double start = parts[k].startSec;
        const double end = parts[k].durationSec;
        if (k == 0)
            out.offsetSec = t.offsetSec + shift;
        if (!t.frameTimes.empty()) {
            // The part's last frame is a frame before `end`; the file's
            // records may run on past it. The margin keeps a frame time that
            // rounds to `end` out.
            for (const FrameTime& f : t.frameTimes)
                if (f.videoSec < end - 1e-4)
                    out.frameTimes.push_back({f.videoSec + start, f.trackSec + shift});
        } else {
            out.frameTimes.push_back({start, t.offsetSec + shift});
            out.frameTimes.push_back(
                {start + end,
                 t.offsetSec + end * t.captureScale * (1.0 + t.clockRate) + shift});
        }
    }
    return out;
}

} // namespace insv
