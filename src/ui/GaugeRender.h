#pragma once

#include "../core/Gauge.h"
#include "../core/Telemetry.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

// A gauge as a picture: drawn with QPainter into a transparent RGBA image
// (premultiplied alpha), which playback and the export then place and blend
// like a title's. Thread-safe: playback's decoder thread and the export
// render gauges too.
namespace gaugerender {

// What a gauge shows at one moment. Its number is the value rounded as a
// title's GPS field rounds it, so the two agree; its needle, bar or tape
// sits at `position`, the value rounded only as finely as the picture can
// show (quantized), so a picture can be reused.
struct Reading {
    bool hasValue = false; // false: no data, the gauge drawn empty with "--"
    int number = 0;        // as written: whole km/h, metres, degrees
    double position = 0.0; // in the gauge's unit (degrees for a tape)
    bool operator==(const Reading &) const = default;
};

// The reading of `style` from the GPS values `gps` (none: no data) on a
// picture `heightPx` high: a dial or a bar its speed or altitude in its unit,
// a tape the heading.
Reading readingOf(const gauge::Style &style, const std::optional<Telemetry::Values> &gps,
                  int heightPx);

struct Picture {
    std::shared_ptr<const std::vector<uint8_t>> rgba; // width * height * 4
    int width = 0;
    int height = 0;

    bool empty() const { return !rgba; }
};

// The gauge `heightPx` high (as wide as its kind's aspect makes it) showing
// `reading`; one picture per (style, height, reading) kept for reuse.
Picture render(const gauge::Style &style, int heightPx, const Reading &reading);
// The same if it has been made and is still kept, else an empty picture.
Picture cached(const gauge::Style &style, int heightPx, const Reading &reading);

} // namespace gaugerender
