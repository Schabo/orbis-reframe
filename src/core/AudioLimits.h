#pragma once

// The level range every audio gain in the edit model is clamped to: a video
// clip's own sound (ClipItem::audioGainDb), a clip on an audio track
// (TrackClip::gainDb) and a whole track (Track::gainDb). The Audio panel's
// sliders use the same range.
namespace audiolimits {

constexpr double kMinGainDb = -24.0;
constexpr double kMaxGainDb = 12.0;

} // namespace audiolimits
