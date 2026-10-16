#include "FlatClipSource.h"

#include <cmath>
#include <filesystem>
#include <system_error>

namespace {
constexpr int kW = 8, kH = 4; // an equirect's 2:1; the viewport scales it up
} // namespace

FlatClipSource::FlatClipSource(double fps, int64_t durationMs)
{
    m_info.width = kW;
    m_info.height = kH;
    m_info.fps = fps > 0.0 ? fps : 30.0;
    m_info.duration_ms = durationMs;
}

bool FlatClipSource::Open(const std::string &clipPath, const stitch::PreviewStitchConfig &)
{
    std::error_code ec;
    if (!std::filesystem::exists(clipPath, ec)) {
        m_error = "no such file: " + clipPath;
        return false;
    }
    m_next = 0;
    return true;
}

int64_t FlatClipSource::msOfFrame(int64_t frame) const
{
    return static_cast<int64_t>(std::llround(frame * 1000.0 / m_info.fps));
}

stitch::Frame FlatClipSource::GetFrame(int64_t timestampMs)
{
    SeekTo(timestampMs);
    return ReadFrame();
}

bool FlatClipSource::SeekTo(int64_t timestampMs)
{
    // The frame showing at `timestampMs`: the last one at or before it.
    m_next = std::max<int64_t>(0, static_cast<int64_t>(std::floor(timestampMs * m_info.fps / 1000.0 + 1e-6)));
    return true;
}

stitch::Frame FlatClipSource::ReadFrame()
{
    stitch::Frame f;
    const int64_t ms = msOfFrame(m_next);
    if (ms >= m_info.duration_ms)
        return f; // the end
    f.width = kW;
    f.height = kH;
    f.rgba.assign(size_t(kW) * kH * 4, 0);
    for (size_t i = 3; i < f.rgba.size(); i += 4)
        f.rgba[i] = 255;
    f.timestamp_ms = ms;
    ++m_next;
    return f;
}
