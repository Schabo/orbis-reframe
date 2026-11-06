#include "SeamFlowEstimator.h"

#include <opencv2/core.hpp>
#include <opencv2/video/tracking.hpp>

#include <cmath>
#include <numbers>

namespace insv {

namespace {
// Columns wrapped round onto either end of a strip before the flow, so it
// sees across azimuth 0 / 360 like anywhere else: 4 deg.
constexpr int kWrapDeg = 4;
} // namespace

struct SeamFlowEstimator::Impl {
    // DIS at its fast preset: ~3 ms per direction on the strips (a
    // ~2500 x 77 grey image), against ~13 ms at the medium one, which in
    // the measurements joined near things no better and moved more.
    cv::Ptr<cv::DISOpticalFlow> dis = cv::DISOpticalFlow::create(cv::DISOpticalFlow::PRESET_FAST);
};

SeamFlowEstimator::SeamFlowEstimator() : d_(std::make_unique<Impl>()) {}
SeamFlowEstimator::~SeamFlowEstimator() = default;

bool SeamFlowEstimator::estimate(const SeamFlowStrips& s, SeamFlowField& out)
{
    out.v.clear();
    const SeamFlowGrid& g = s.grid;
    if (!g.valid() || s.grey[0].size() != static_cast<size_t>(g.rows) * g.cols ||
        s.grey[1].size() != s.grey[0].size())
        return false;
    const int pad = static_cast<int>(std::lround(kWrapDeg * g.cols / 360.0));
    SeamFlowStrips anchored;
    AnchorSeamFlowStrips(s, anchored);
    cv::Mat strip[2];
    for (int i = 0; i < 2; ++i) {
        const cv::Mat src(g.rows, g.cols, CV_8U, anchored.grey[i].data());
        cv::copyMakeBorder(src, strip[i], 0, 0, pad, pad, cv::BORDER_WRAP);
    }
    cv::Mat flow[2]; // flow[0] = F01: strip0(p) ~ strip1(p + F01(p)); flow[1] = F10
    for (int i = 0; i < 2; ++i)
        d_->dis->calc(strip[i], strip[1 - i], flow[i]);

    out.grid = g;
    out.v.resize(static_cast<size_t>(g.rows) * g.cols * 4);
    const double perX = 2.0 * std::numbers::pi / g.cols;    // rad of phi per column
    const double perY = -2.0 * g.stripRad / (g.rows - 1);   // rad of e per row (down)
    for (int y = 0; y < g.rows; ++y) {
        const cv::Vec2f* f01 = flow[0].ptr<cv::Vec2f>(y) + pad;
        const cv::Vec2f* f10 = flow[1].ptr<cv::Vec2f>(y) + pad;
        float* o = out.v.data() + static_cast<size_t>(y) * g.cols * 4;
        for (int x = 0; x < g.cols; ++x, o += 4) {
            o[0] = static_cast<float>(f10[x][0] * perX);
            o[1] = static_cast<float>(f10[x][1] * perY);
            o[2] = static_cast<float>(f01[x][0] * perX);
            o[3] = static_cast<float>(f01[x][1] * perY);
        }
    }
    return true;
}

} // namespace insv
