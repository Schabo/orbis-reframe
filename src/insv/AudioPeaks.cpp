// SPDX-License-Identifier: MIT OR Apache-2.0
#include "AudioPeaks.h"

#include "InsvAudioDemux.h"

#include <algorithm>
#include <cmath>

namespace insv {

AudioPeaks ComputeAudioPeaks(const std::string& path, int stream, const std::atomic<bool>* cancel)
{
    // Decoded at 16 kHz stereo: plenty for a picture of the level, and a
    // third of the work of the source rate.
    constexpr int kRate = 16000;
    constexpr int kCh = 2;
    AudioPeaks out;
    InsvAudioDemux d;
    std::string err;
    if (!d.open(path, kRate, kCh, err, stream))
        return out;
    if (d.durationMs() > 0)
        out.peaks.reserve(static_cast<size_t>(d.durationMs() / (1000 / AudioPeaks::kPerSec) + 1));

    std::vector<float> chunk;
    for (;;) {
        if (cancel && cancel->load(std::memory_order_relaxed))
            break;
        double sec = 0.0;
        int64_t ms = 0;
        if (!d.readNext(chunk, &ms, err, &sec))
            break; // end of stream, or a decode error: keep what we have
        const int64_t first = std::llround(sec * kRate); // the chunk's first frame
        const size_t frames = chunk.size() / kCh;
        for (size_t i = 0; i < frames; ++i) {
            const int64_t slice = (first + static_cast<int64_t>(i)) * AudioPeaks::kPerSec / kRate;
            if (slice < 0)
                continue;
            if (static_cast<size_t>(slice) >= out.peaks.size())
                out.peaks.resize(static_cast<size_t>(slice) + 1, 0.0f);
            const float v = std::max(std::abs(chunk[i * kCh]), std::abs(chunk[i * kCh + 1]));
            float& p = out.peaks[static_cast<size_t>(slice)];
            p = std::max(p, std::min(v, 1.0f));
        }
    }
    return out;
}

} // namespace insv
