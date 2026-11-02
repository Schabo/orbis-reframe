#pragma once

// The chart's OpenCV board and detector (internal to src/calib).

#include "ChartBoard.h"

#include <opencv2/objdetect/charuco_detector.hpp>

namespace calib {

inline cv::aruco::CharucoBoard MakeChartBoard()
{
    // The square size here is the board's own unit; corners are placed by
    // ChartCornerPosition times the measured square.
    return cv::aruco::CharucoBoard(
        cv::Size(kChartSquaresX, kChartSquaresY), 1.0f, float(kChartMarkerRatio),
        cv::aruco::getPredefinedDictionary(cv::aruco::DICT_5X5_100));
}

inline cv::aruco::CharucoDetector MakeChartDetector()
{
    cv::aruco::CharucoParameters cp;
    cp.tryRefineMarkers = true;
    cv::aruco::DetectorParameters dp;
    dp.cornerRefinementMethod = cv::aruco::CORNER_REFINE_SUBPIX;
    return cv::aruco::CharucoDetector(MakeChartBoard(), cp, dp);
}

} // namespace calib
