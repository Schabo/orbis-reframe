#pragma once

// Finding the calibration chart (ChartBoard.h) in a clip's lens pictures:
// every few frames, each lens on its own, the corners to a fraction of a
// pixel. What the lens fit (LensFit.h) works from.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace calib {

struct ChartCorner {
    int id = 0;
    // Lens picture pixels, the stream's full size, in the stitch's frame:
    // pixel (x, y)'s centre is at (x, y).
    float u = 0, v = 0;
};
struct ChartView {
    int frame = 0; // the clip's frame number
    std::vector<ChartCorner> corners;
};
struct ChartObservations {
    int lensWidth = 0, lensHeight = 0;
    double fps = 0;
    int framesRead = 0;
    std::vector<ChartView> lens[2]; // frames where the lens saw the chart
};

// The chart's corners in one grey picture (`width` x `height`, row stride
// `width`); none when fewer than a handful are found.
std::vector<ChartCorner> DetectChartInPicture(const uint8_t* grey, int width, int height);

// Every `step`th frame of the clip's two lenses. `progress` (0..1) is called
// from the calling thread. False with `err` set when the clip can't be read
// or `cancel` was set.
bool DetectChart(const std::string& clipPath, ChartObservations& out, std::string& err,
                 const std::function<void(double)>& progress = {},
                 const std::atomic<bool>* cancel = nullptr, int step = 3);

// The detections as text, to keep a run's and fit it again (tools/lens_calib):
// a header line, then per lens and frame "F lens frame n" and n lines
// "id u v". Reading refuses files of another version.
bool WriteChartObservations(const std::string& path, const ChartObservations& obs);
bool ReadChartObservations(const std::string& path, ChartObservations& obs);

} // namespace calib
