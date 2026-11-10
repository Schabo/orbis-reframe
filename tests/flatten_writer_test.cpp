// FlattenWriter's container timing: every frame written is a frame a player
// shows. The MP4 muxer takes a sample's length from the packet's duration
// and the last one has nothing after it to infer it from, so a packet
// without a duration used to end the stream (and its edit list) at the last
// frame's start -- players honouring the edit list dropped the last frame.
// Checked for H.265 and H.264 at 29.97 and 25 fps, with and without audio.
// Standalone; the exit code is the failure count, 77 (skipped) without a
// hardware encoder.

#include "ReframeFlatten.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

constexpr int kW = 320;
constexpr int kH = 240;
constexpr int kFrames = 30;

// What a player sees of the file: the video packets the demuxer hands out
// with the edit list applied (not flagged to discard), and the stream's
// duration in seconds.
struct Seen {
    int videoPackets = 0;
    double videoSeconds = -1.0;
};

bool probe(const std::string& path, Seen& seen)
{
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return false;
    }
    const int v = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (v < 0) {
        avformat_close_input(&fmt);
        return false;
    }
    const AVStream* st = fmt->streams[v];
    if (st->duration != AV_NOPTS_VALUE)
        seen.videoSeconds = st->duration * av_q2d(st->time_base);
    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == v && !(pkt->flags & AV_PKT_FLAG_DISCARD))
            ++seen.videoPackets;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    return true;
}

void runCase(const std::filesystem::path& dir, bool h265, double fps, bool audio)
{
    const std::string name = std::string(h265 ? "h265" : "h264") + "-" +
                             std::to_string(static_cast<int>(std::lround(fps * 100))) +
                             (audio ? "-audio" : "") + ".mp4";
    exportpipe::FlattenConfig cfg;
    cfg.out_path = (dir / name).string();
    cfg.out_w = kW;
    cfg.out_h = kH;
    cfg.fps = fps;
    cfg.h265 = h265;
    cfg.bitrate_kbps = 2000;
    if (audio)
        cfg.audio_sample_rate = 48000;

    exportpipe::FlattenWriter w;
    std::string err;
    if (!w.open(cfg, err)) {
        std::printf("FAIL  %s: open: %s\n", name.c_str(), err.c_str());
        ++g_failures;
        return;
    }
    std::vector<uint8_t> nv12(static_cast<size_t>(kW) * kH * 3 / 2, 128);
    const int64_t samplesPerFrame = static_cast<int64_t>(std::lround(48000 / fps));
    std::vector<float> pcm(static_cast<size_t>(samplesPerFrame) * 2, 0.0f);
    for (int i = 0; i < kFrames; ++i) {
        // A moving bar, so the encoder makes real P / B frames.
        std::fill(nv12.begin(), nv12.begin() + kW * kH, uint8_t{60});
        for (int y = 0; y < kH; ++y)
            for (int x = i * 8; x < i * 8 + 16 && x < kW; ++x)
                nv12[static_cast<size_t>(y) * kW + x] = 200;
        CHECK(w.addNv12Frame(nv12.data(), kW, kH, err));
        if (audio)
            CHECK(w.addAudio(pcm.data(), samplesPerFrame, err));
    }
    CHECK(w.finish(err) == kFrames);

    Seen seen;
    CHECK(probe(cfg.out_path, seen));
    const double want = kFrames / fps;
    std::printf("%-20s %d packets shown, video %.6f s (want %d, %.6f s)\n", name.c_str(),
                seen.videoPackets, seen.videoSeconds, kFrames, want);
    CHECK(seen.videoPackets == kFrames);
    CHECK(std::abs(seen.videoSeconds - want) < 0.5 / fps);
}

} // namespace

int main()
{
    if (!exportpipe::hwEncoderAvailable()) {
        std::printf("flatten_writer_test: no hardware encoder, skipped\n");
        return 77;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "orbis_flatten_writer_test";
    std::filesystem::create_directories(dir);

    for (bool h265 : {true, false})
        for (double fps : {30000.0 / 1001.0, 25.0})
            runCase(dir, h265, fps, false);
    runCase(dir, true, 30000.0 / 1001.0, true);

    std::filesystem::remove_all(dir);
    if (g_failures)
        std::printf("flatten_writer_test: %d failure(s)\n", g_failures);
    return g_failures;
}
