// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace insv {

// A clip's waveform for the timeline: the peak level of each 1/perSec slice of
// an audio stream, in source time. Mono (the louder of the channels), linear
// 0..1.
struct AudioPeaks {
    static constexpr int kPerSec = 100; // 10 ms slices
    std::vector<float> peaks;           // empty: no audio (or the file couldn't be read)
};

// Decodes the whole stream (`stream` as InsvAudioDemux::open's track; -1 =
// the default) and returns its peaks. Slow-ish (a pass over all the audio,
// decoded at a reduced rate) -- run it off the GUI thread. `cancel`, if
// given, stops it early (returning what's done). FFmpeg-only, no Qt.
AudioPeaks ComputeAudioPeaks(const std::string& path, int stream,
                             const std::atomic<bool>* cancel = nullptr);

} // namespace insv
