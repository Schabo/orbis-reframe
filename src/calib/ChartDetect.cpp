#include "ChartDetect.h"

#include "ChartDetector.h"

#include <ClipMetadata.h>
#include <InsvDemux.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <thread>

namespace calib {

namespace {

// One sampled frame of one lens, grey, waiting for a detector.
struct Job {
    int lens = 0;
    int frame = 0;
    cv::Mat grey;
};

// A bounded queue: the decoder waits when the detectors fall behind, so the
// frames in flight stay a few hundred MB at 8K.
class JobQueue {
public:
    explicit JobQueue(size_t cap) : cap_(cap) {}
    void push(Job j)
    {
        std::unique_lock<std::mutex> lk(m_);
        notFull_.wait(lk, [&] { return q_.size() < cap_; });
        q_.push_back(std::move(j));
        notEmpty_.notify_one();
    }
    bool pop(Job& j)
    {
        std::unique_lock<std::mutex> lk(m_);
        notEmpty_.wait(lk, [&] { return !q_.empty() || closed_; });
        if (q_.empty())
            return false;
        j = std::move(q_.front());
        q_.pop_front();
        notFull_.notify_one();
        return true;
    }
    void close()
    {
        std::lock_guard<std::mutex> lk(m_);
        closed_ = true;
        notEmpty_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable notFull_, notEmpty_;
    std::deque<Job> q_;
    size_t cap_;
    bool closed_ = false;
};

cv::Mat Grey(const insv::Image& img)
{
    const cv::Mat rgb(img.h, img.w, CV_8UC3, const_cast<uint8_t*>(img.rgb.data()));
    cv::Mat g;
    cv::cvtColor(rgb, g, cv::COLOR_RGB2GRAY);
    return g;
}

// Fewer corners than this are too few to place the board.
constexpr int kMinCorners = 8;

// What to add to OpenCV's ChArUco corners to put them in the stitch's pixel
// frame, where pixel (x, y)'s centre is at (x, y) (insv::Image::sample, the
// GPU stitch's (u + 0.5) / w). OpenCV 4.7 to 4.13 and 5.0.0-alpha return
// them half a pixel down and right of that, 4.14 and 5.0 don't; a
// distribution may carry either. So it is measured once, on the chart's own
// picture, where every corner sits exactly on a pixel boundary: between
// pixels k - 1 and k is k - 0.5.
cv::Point2f CornerFrameOffset()
{
    static const cv::Point2f offset = [] {
        const ChartImage c = MakeChartImage(32 * (kChartSquaresX + 1), 32 * (kChartSquaresY + 1));
        const cv::Mat img(c.height, c.width, CV_8UC1, const_cast<uint8_t*>(c.grey.data()));
        std::vector<cv::Point2f> corners;
        std::vector<int> ids;
        MakeChartDetector().detectBoard(img, corners, ids);
        const int bx0 = (c.width - kChartSquaresX * c.squarePx) / 2;
        const int by0 = (c.height - kChartSquaresY * c.squarePx) / 2;
        double du = 0, dv = 0;
        for (size_t k = 0; k < corners.size(); ++k) {
            double x, y;
            ChartCornerPosition(ids[k], x, y);
            du += corners[k].x - (bx0 + x * c.squarePx - 0.5);
            dv += corners[k].y - (by0 + y * c.squarePx - 0.5);
        }
        const double n = double(std::max<size_t>(1, corners.size()));
        du /= n, dv /= n;
        // A frame convention is a whole number of half pixels, the same on
        // both axes; anything else is not one this code knows.
        const double half = std::round(2 * du) / 2;
        if (corners.size() != size_t(kChartCorners) || std::fabs(du - half) > 0.05 ||
            std::fabs(dv - half) > 0.05) {
            std::fprintf(stderr,
                         "lens calibration: can't tell OpenCV %s's chart corner frame (%zu "
                         "corners, off by %.3f, %.3f px); using its corners as they are\n",
                         CV_VERSION, corners.size(), du, dv);
            return cv::Point2f(0, 0);
        }
        return cv::Point2f(float(-half), float(-half));
    }();
    return offset;
}

// The chart's corners in one picture, in the stitch's pixel frame; none
// when too few are found.
std::vector<ChartCorner> Detect(cv::aruco::CharucoDetector& det, const cv::Mat& grey)
{
    const cv::Point2f offset = CornerFrameOffset();
    std::vector<cv::Point2f> corners;
    std::vector<int> ids;
    det.detectBoard(grey, corners, ids);
    std::vector<ChartCorner> out;
    if (int(corners.size()) < kMinCorners)
        return out;
    out.reserve(corners.size());
    for (size_t k = 0; k < corners.size(); ++k)
        out.push_back({ids[k], corners[k].x + offset.x, corners[k].y + offset.y});
    return out;
}

} // namespace

std::vector<ChartCorner> DetectChartInPicture(const uint8_t* grey, int width, int height)
{
    cv::aruco::CharucoDetector det = MakeChartDetector();
    return Detect(det, cv::Mat(height, width, CV_8UC1, const_cast<uint8_t*>(grey)));
}

bool DetectChart(const std::string& clipPath, ChartObservations& out, std::string& err,
                 const std::function<void(double)>& progress, const std::atomic<bool>* cancel,
                 int step)
{
    out = ChartObservations{};
    const auto clip = camera::ReadClipMetadata(clipPath, &err);
    if (!clip)
        return false;
    insv::InsvDemux demux;
    if (!demux.open(clip->lensSources(), err))
        return false;
    if (demux.lensCount() < 2) {
        err = "the clip has one lens stream";
        return false;
    }
    out.lensWidth = demux.lens(0).width;
    out.lensHeight = demux.lens(0).height;
    out.fps = demux.lens(0).fps;
    const int64_t durationMs = std::max<int64_t>(1, demux.lens(0).durationMs);
    step = std::max(1, step);

    JobQueue queue(12);
    std::mutex resultsMutex;
    const int nWorkers = std::max(1, int(std::thread::hardware_concurrency()) / 2);
    std::vector<std::thread> workers;
    for (int w = 0; w < nWorkers; ++w)
        workers.emplace_back([&] {
            cv::aruco::CharucoDetector det = MakeChartDetector();
            Job j;
            while (queue.pop(j)) {
                ChartView v;
                v.frame = j.frame;
                v.corners = Detect(det, j.grey);
                if (v.corners.empty())
                    continue;
                std::lock_guard<std::mutex> lk(resultsMutex);
                out.lens[j.lens].push_back(std::move(v));
            }
        });

    bool ok = true;
    if (!demux.seekTo(0, err))
        ok = false;
    insv::Image l0, l1;
    insv::FramePts pts;
    for (int n = 0; ok; ++n) {
        if (cancel && cancel->load()) {
            err = "cancelled";
            ok = false;
            break;
        }
        std::string e;
        if (!demux.readNextPair(l0, l1, &pts, e)) {
            if (!e.empty()) {
                err = e;
                ok = false;
            }
            break;
        }
        ++out.framesRead;
        const int frame = int(std::lround(double(pts.ms) * out.fps / 1000.0));
        if (frame % step == 0) {
            queue.push({0, frame, Grey(l0)});
            queue.push({1, frame, Grey(l1)});
        }
        if (progress && n % 30 == 0)
            progress(std::clamp(double(pts.ms) / double(durationMs), 0.0, 1.0));
    }
    queue.close();
    for (std::thread& t : workers)
        t.join();
    for (auto& l : out.lens)
        std::sort(l.begin(), l.end(),
                  [](const ChartView& a, const ChartView& b) { return a.frame < b.frame; });
    if (ok && progress)
        progress(1.0);
    return ok;
}

// 2: corners in the stitch's pixel frame (version 1 files may be half a
// pixel off it, depending on the OpenCV that found them).
constexpr int kObservationsVersion = 2;

bool WriteChartObservations(const std::string& path, const ChartObservations& obs)
{
    std::ofstream f(path);
    if (!f)
        return false;
    f.imbue(std::locale::classic()); // '.' decimals whatever the user's locale
    f << std::fixed << std::setprecision(6) << "chart-observations " << kObservationsVersion << ' '
      << obs.lensWidth << ' ' << obs.lensHeight << ' ' << obs.fps << ' ' << obs.framesRead << '\n'
      << std::setprecision(3);
    for (int l = 0; l < 2; ++l)
        for (const ChartView& v : obs.lens[l]) {
            f << "F " << l << ' ' << v.frame << ' ' << v.corners.size() << '\n';
            for (const ChartCorner& c : v.corners)
                f << c.id << ' ' << c.u << ' ' << c.v << '\n';
        }
    f.close();
    return bool(f);
}

bool ReadChartObservations(const std::string& path, ChartObservations& obs)
{
    obs = ChartObservations{};
    std::ifstream in(path);
    in.imbue(std::locale::classic());
    std::string tag;
    int version = 0;
    if (!(in >> tag >> version) || tag != "chart-observations" || version != kObservationsVersion)
        return false;
    in >> obs.lensWidth >> obs.lensHeight >> obs.fps >> obs.framesRead;
    while (in >> tag) {
        if (tag != "F")
            return false;
        int lens = 0;
        size_t n = 0;
        ChartView v;
        if (!(in >> lens >> v.frame >> n) || lens < 0 || lens > 1)
            return false;
        v.corners.resize(n);
        for (ChartCorner& c : v.corners)
            if (!(in >> c.id >> c.u >> c.v))
                return false;
        obs.lens[lens].push_back(std::move(v));
    }
    return bool(in.eof());
}

} // namespace calib
