// SPDX-License-Identifier: MIT OR Apache-2.0
#include "AudioPeaks.h"

#include "InsvAudioDemux.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace insv {

AudioPeaks ComputeAudioPeaks(const std::vector<AudioPart>& parts, int stream,
                             const std::atomic<bool>* cancel)
{
    // Decoded at 16 kHz stereo: plenty for a picture of the level, and a
    // third of the work of the source rate.
    constexpr int kRate = 16000;
    constexpr int kCh = 2;
    AudioPeaks out;
    for (size_t k = 0; k < parts.size(); ++k) {
        const int64_t start = parts[k].startUs * kRate / 1000000; // in frames, clip time
        const int64_t end = k + 1 < parts.size() ? parts[k + 1].startUs * kRate / 1000000
                                                 : INT64_MAX;
        InsvAudioDemux d;
        std::string err;
        if (!d.open(parts[k].path, kRate, kCh, err, stream))
            continue;
        if (d.durationMs() > 0)
            out.peaks.reserve(static_cast<size_t>(
                (parts[k].startUs / 1000 + d.durationMs()) / (1000 / AudioPeaks::kPerSec) + 1));

        std::vector<float> chunk;
        for (;;) {
            if (cancel && cancel->load(std::memory_order_relaxed))
                return out;
            double sec = 0.0;
            int64_t ms = 0;
            if (!d.readNext(chunk, &ms, err, &sec))
                break; // end of stream, or a decode error: keep what we have
            const int64_t first = start + std::llround(sec * kRate); // the chunk's first frame
            const size_t frames = chunk.size() / kCh;
            for (size_t i = 0; i < frames; ++i) {
                const int64_t f = first + static_cast<int64_t>(i);
                if (f < start || f >= end)
                    continue;
                const int64_t slice = f * AudioPeaks::kPerSec / kRate;
                if (slice < 0)
                    continue;
                if (static_cast<size_t>(slice) >= out.peaks.size())
                    out.peaks.resize(static_cast<size_t>(slice) + 1, 0.0f);
                const float v = std::max(std::abs(chunk[i * kCh]), std::abs(chunk[i * kCh + 1]));
                float& p = out.peaks[static_cast<size_t>(slice)];
                p = std::max(p, std::min(v, 1.0f));
            }
            if (first >= end)
                break; // the next part plays from here
        }
    }
    return out;
}

} // namespace insv
