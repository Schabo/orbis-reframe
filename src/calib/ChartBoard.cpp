#include "ChartBoard.h"

#include "ChartDetector.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cstring>

namespace calib {

ChartImage MakeChartImage(int width, int height)
{
    ChartImage out;
    out.width = width;
    out.height = height;
    out.grey.assign(static_cast<size_t>(std::max(0, width)) * std::max(0, height), 255);
    const int sq = std::min(width / (kChartSquaresX + 1), height / (kChartSquaresY + 1));
    if (sq < 8)
        return out;
    out.squarePx = sq;
    const cv::aruco::CharucoBoard board = MakeChartBoard();
    cv::Mat b;
    board.generateImage(cv::Size(kChartSquaresX * sq, kChartSquaresY * sq), b, 0, 1);
    cv::Mat img(height, width, CV_8UC1, out.grey.data());
    b.copyTo(img(cv::Rect((width - b.cols) / 2, (height - b.rows) / 2, b.cols, b.rows)));
    return out;
}

} // namespace calib
