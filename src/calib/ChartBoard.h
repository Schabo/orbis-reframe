#pragma once

// The lens-calibration chart: a ChArUco board (OpenCV's chessboard with
// ArUco markers in the white squares, so every corner is identified even
// when only part of the board is in view). 13 x 7 squares, DICT_5X5_100,
// markers 0.75 of a square: 72 inner corners. Shown full screen on a
// monitor, it is filmed with the camera turning through it, so every part
// of both lenses sees it.

#include <cstdint>
#include <vector>

namespace calib {

constexpr int kChartSquaresX = 13;
constexpr int kChartSquaresY = 7;
constexpr int kChartCorners = (kChartSquaresX - 1) * (kChartSquaresY - 1);
constexpr double kChartMarkerRatio = 0.75;

// Corner `id`'s position on the board, in units of a square: x along the
// rows of 12, y down the board, z = 0.
inline void ChartCornerPosition(int id, double& x, double& y)
{
    x = id % (kChartSquaresX - 1) + 1;
    y = id / (kChartSquaresX - 1) + 1;
}

// The chart as a picture for a screen of `width` x `height` pixels: the
// board centred at the largest whole square size that leaves at least half
// a square of white around it. Grey, 8 bits.
struct ChartImage {
    int width = 0, height = 0;
    int squarePx = 0;
    std::vector<uint8_t> grey; // width * height
};
ChartImage MakeChartImage(int width, int height);

} // namespace calib
