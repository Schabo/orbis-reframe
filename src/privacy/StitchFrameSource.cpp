#include "StitchFrameSource.h"

#include <GyroStabilizer.h>
#include <NativeStitchSource.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace privacy {

namespace {
constexpr double kPi = std::numbers::pi;
constexpr double kDegToRad = kPi / 180.0;

// A view centred at (lon, lat) is the viewport's yaw = -lon, pitch = lat
// (equirect.frag's ray convention), the levelling as its stab.
stitch::NativeStitchSource::ViewRect viewRect(const ScanView& v)
{
    stitch::NativeStitchSource::ViewRect r;
    r.pose[0] = static_cast<float>(-v.lonDeg * kDegToRad);
    r.pose[1] = static_cast<float>(v.latDeg * kDegToRad);
    r.pose[2] = 0.0f;
    r.pose[3] = static_cast<float>(2.0 * std::atan(std::tan(0.5 * v.hfovDeg * kDegToRad) * v.h / std::max(1, v.w)));
    r.x = v.x;
    r.y = v.y;
    r.w = v.w;
    r.h = v.h;
    return r;
}
} // namespace

StitchFrameSource::StitchFrameSource(stitch::NativeStitchSource& src, int64_t fromMs, int64_t toMs, double fps,
                                     std::shared_ptr<const insv::GyroStabilizer> level)
    : m_src(src), m_fromMs(fromMs), m_toMs(toMs), m_halfFrameMs(500.0 / fps),
      m_level(std::move(level)), m_gpu(src.GpuViewAvailable())
{
}

bool StitchFrameSource::next(int64_t& tMs, float level[9], std::string& err)
{
    for (;;) {
        int64_t pts = 0;
        if (m_gpu) {
            if (!m_src.HoldNextPair(pts)) {
                err = m_src.GetLastError();
                return false;
            }
        } else {
            stitch::Frame f = m_src.ReadFrame();
            if (f.rgba.empty()) {
                err = m_src.GetLastError();
                return false;
            }
            pts = f.timestamp_ms;
            m_eq = std::move(f.rgba);
            m_eqW = f.width;
            m_eqH = f.height;
        }
        if (pts + m_halfFrameMs < m_fromMs)
            continue; // a straggler before the in-point
        if (pts > m_toMs + m_halfFrameMs) {
            release();
            err.clear();
            return false;
        }
        float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        if (m_level && m_level->ready())
            m_level->correctionMatrix(pts / 1000.0, m);
        std::copy(&m[0][0], &m[0][0] + 9, m_M);
        std::copy(m_M, m_M + 9, level);
        tMs = pts;
        return true;
    }
}

bool StitchFrameSource::render(int canvasW, int canvasH, const std::vector<ScanView>& views,
                               std::vector<uint8_t>& rgba, std::string& err)
{
    if (m_gpu) {
        std::vector<stitch::NativeStitchSource::ViewRect> vr(views.size());
        for (size_t i = 0; i < views.size(); ++i)
            vr[i] = viewRect(views[i]);
        if (!m_src.RenderHeldViews(canvasW, canvasH, vr, m_M, 0xFF727272u, rgba)) {
            err = m_src.GetLastError();
            return false;
        }
        return true;
    }

    // CPU: remap each view out of the equirect.
    if (m_eq.empty()) {
        err = "no frame";
        return false;
    }
    rgba.assign(size_t(canvasW) * canvasH * 4, 114);
    cv::Mat canvas(canvasH, canvasW, CV_8UC4, rgba.data());
    canvas.setTo(cv::Scalar(114, 114, 114, 255));
    const cv::Mat eq(m_eqH, m_eqW, CV_8UC4, m_eq.data());
    const float* M = m_M;
    for (const ScanView& v : views) {
        const double lon = v.lonDeg * kDegToRad, lat = v.latDeg * kDegToRad;
        const double fw[3] = {std::cos(lat) * std::sin(lon), std::sin(lat), -std::cos(lat) * std::cos(lon)};
        const double rt[3] = {std::cos(lon), 0.0, std::sin(lon)};
        const double up[3] = {rt[1] * fw[2] - rt[2] * fw[1], rt[2] * fw[0] - rt[0] * fw[2],
                              rt[0] * fw[1] - rt[1] * fw[0]};
        const double f = 0.5 * v.w / std::tan(0.5 * v.hfovDeg * kDegToRad);
        cv::Mat mx(v.h, v.w, CV_32F), my(v.h, v.w, CV_32F);
        for (int y = 0; y < v.h; ++y)
            for (int x = 0; x < v.w; ++x) {
                double d[3];
                for (int k = 0; k < 3; ++k)
                    d[k] = fw[k] * f + rt[k] * (x + 0.5 - 0.5 * v.w) - up[k] * (y + 0.5 - 0.5 * v.h);
                const double n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                const double wx = (M[0] * d[0] + M[1] * d[1] + M[2] * d[2]) / n;
                const double wy = (M[3] * d[0] + M[4] * d[1] + M[5] * d[2]) / n;
                const double wz = (M[6] * d[0] + M[7] * d[1] + M[8] * d[2]) / n;
                mx.at<float>(y, x) = static_cast<float>((std::atan2(wx, -wz) / (2 * kPi) + 0.5) * m_eqW - 0.5);
                my.at<float>(y, x) =
                    static_cast<float>((0.5 - std::asin(std::clamp(wy, -1.0, 1.0)) / kPi) * m_eqH - 0.5);
            }
        cv::Mat dst = canvas(cv::Rect(v.x, v.y, v.w, v.h));
        cv::remap(eq, dst, mx, my, cv::INTER_LINEAR, cv::BORDER_WRAP);
    }
    return true;
}

void StitchFrameSource::release()
{
    if (m_gpu)
        m_src.ReleaseHeldPair();
}

ReverseStitchFrameSource::ReverseStitchFrameSource(stitch::NativeStitchSource& src, int64_t fromMs,
                                                   int64_t startMs, double fps,
                                                   std::shared_ptr<const insv::GyroStabilizer> level,
                                                   int chunkFrames)
    : m_src(src), m_fromMs(fromMs), m_frameMs(1000.0 / fps),
      m_boundary(startMs + 0.5 * m_frameMs), m_level(std::move(level)), m_chunk(chunkFrames)
{
    if (m_chunk <= 0) {
        const size_t pair = std::max<size_t>(src.KeptPairBytes(), 1);
        m_chunk = static_cast<int>(std::clamp<size_t>((size_t(1) << 30) / pair, 8, 60));
    }
}

ReverseStitchFrameSource::~ReverseStitchFrameSource()
{
    releaseAll();
}

void ReverseStitchFrameSource::releaseAll()
{
    release();
    for (const Kept& k : m_kept)
        m_src.ReleaseKeptPair(k.handle);
    m_kept.clear();
}

bool ReverseStitchFrameSource::loadChunk(std::string& err)
{
    // Seek a little before the stretch's first frame (SeekTo lands on the
    // frame at or after it -- so the first frame is the one at or after
    // fromMs, as a forward pass seeked there starts) and keep every frame up
    // to the boundary.
    if (m_boundary <= m_fromMs - 1.0)
        return false;
    const double start = std::max(static_cast<double>(m_fromMs), m_boundary - (m_chunk + 0.5) * m_frameMs);
    if (!m_src.SeekTo(static_cast<int64_t>(std::floor(start)))) {
        err = m_src.GetLastError();
        return false;
    }
    for (;;) {
        int64_t pts = 0;
        const int h = m_src.KeepNextPair(pts);
        if (!h) {
            err = m_src.GetLastError(); // empty at the clip's end
            break;
        }
        if (pts >= m_boundary - 1.0) { // reached what was handed out already
            m_src.ReleaseKeptPair(h);
            break;
        }
        if (pts + 1 < m_fromMs) {
            m_src.ReleaseKeptPair(h); // a straggler before the in-point
            continue;
        }
        m_kept.push_back({h, pts});
    }
    if (m_kept.empty())
        return false;
    m_boundary = static_cast<double>(m_kept.front().tMs);
    if (m_kept.front().tMs - m_frameMs + 1 < m_fromMs)
        m_done = true; // this stretch reaches the in-point
    return err.empty();
}

bool ReverseStitchFrameSource::next(int64_t& tMs, float level[9], std::string& err)
{
    release();
    err.clear();
    if (!m_src.GpuViewAvailable()) {
        err = "following backwards needs the GPU stitch";
        return false;
    }
    if (m_kept.empty() && (m_done || !loadChunk(err)))
        return false;
    const Kept k = m_kept.back();
    m_kept.pop_back();
    m_current = k.handle;
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (m_level && m_level->ready())
        m_level->correctionMatrix(k.tMs / 1000.0, m);
    std::copy(&m[0][0], &m[0][0] + 9, m_M);
    std::copy(m_M, m_M + 9, level);
    tMs = k.tMs;
    return true;
}

bool ReverseStitchFrameSource::render(int canvasW, int canvasH, const std::vector<ScanView>& views,
                                      std::vector<uint8_t>& rgba, std::string& err)
{
    if (!m_current) {
        err = "no frame";
        return false;
    }
    std::vector<stitch::NativeStitchSource::ViewRect> vr(views.size());
    for (size_t i = 0; i < views.size(); ++i)
        vr[i] = viewRect(views[i]);
    if (!m_src.RenderKeptViews(m_current, canvasW, canvasH, vr, m_M, 0xFF727272u, rgba)) {
        err = m_src.GetLastError();
        return false;
    }
    return true;
}

void ReverseStitchFrameSource::release()
{
    if (m_current)
        m_src.ReleaseKeptPair(m_current);
    m_current = 0;
}

} // namespace privacy
