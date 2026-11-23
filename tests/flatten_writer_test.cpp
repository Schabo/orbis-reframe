// FlattenWriter's container timing: every frame written is a frame a player
// shows. The MP4 muxer takes a sample's length from the packet's duration
// and the last one has nothing after it to infer it from, so a packet
// without a duration used to end the stream (and its edit list) at the last
// frame's start -- players honouring the edit list dropped the last frame.
// Checked for H.265 and H.264 at 29.97 and 25 fps, with and without audio.
// And its failures (tests/support/FfmpegFaults): an allocation, a refused
// packet, a failed close or conversion fails the export with an error --
// not a crash, not "done" -- and leaves no half-written file behind.
// Standalone; the exit code is the failure count, 77 (skipped) without a
// hardware encoder.

#include "ReframeFlatten.h"
#include "support/FfmpegFaults.h"

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

exportpipe::FlattenConfig faultConfig(const std::filesystem::path& dir, bool audio)
{
    exportpipe::FlattenConfig cfg;
    cfg.out_path = (dir / "fault.mp4").string();
    cfg.out_w = kW;
    cfg.out_h = kH;
    cfg.fps = 25.0;
    cfg.h265 = true;
    cfg.bitrate_kbps = 2000;
    if (audio)
        cfg.audio_sample_rate = 48000;
    return cfg;
}

// Writes kFrames frames (and their sound) and finishes. False at the first
// call that fails, with `err`.
bool writeAll(exportpipe::FlattenWriter& w, bool audio, std::string& err)
{
    std::vector<uint8_t> nv12(static_cast<size_t>(kW) * kH * 3 / 2, 128);
    std::vector<float> pcm(1920 * 2, 0.0f);
    for (int i = 0; i < kFrames; ++i) {
        if (!w.addNv12Frame(nv12.data(), kW, kH, err))
            return false;
        if (audio && !w.addAudio(pcm.data(), 1920, err))
            return false;
    }
    return w.finish(err) == kFrames;
}

// Each allocation the writer makes at open() failing: open() fails with an
// error and leaves no file, or -- when another encoder took over -- the
// export works. Never a crash.
void testOpenFaults(const std::filesystem::path& dir)
{
    using ffmpegfaults::Call;
    const struct {
        Call call;
        int after;
        const char* name;
    } faults[] = {
        {Call::AllocContext, 0, "the video encoder context"},
        {Call::AllocContext, 1, "the AAC encoder context"},
        {Call::BufferRef, 0, "a hardware reference"},
        {Call::NewStream, 0, "the video stream"},
        {Call::NewStream, 1, "the audio stream"},
        {Call::FrameAlloc, 0, "the audio frame"},
        {Call::FrameAlloc, 1, "the video frame"},
        {Call::FrameBuffer, 0, "the audio frame buffer"},
        {Call::FrameBuffer, 1, "the video frame buffer"},
        {Call::PacketAlloc, 0, "the packet"},
        {Call::ScalerSetup, 0, "the RGBA scaler"},
    };
    for (const auto& f : faults) {
        const exportpipe::FlattenConfig cfg = faultConfig(dir, true);
        std::filesystem::remove(cfg.out_path);
        ffmpegfaults::reset();
        ffmpegfaults::arm(f.call, f.after, AVERROR(ENOMEM));
        std::string err;
        bool ok = false;
        {
            exportpipe::FlattenWriter w;
            ok = w.open(cfg, err);
            if (!ok) {
                std::printf("open, no %-24s: %s\n", f.name, err.c_str());
                CHECK(!err.empty());
                CHECK(!std::filesystem::exists(cfg.out_path));
            } else {
                ok = writeAll(w, true, err);
                CHECK(ok);
            }
        }
        const bool fired = ffmpegfaults::failed(f.call) > 0;
        std::printf("open, no %-24s: fault %s, export %s\n", f.name, fired ? "hit" : "not reached",
                    ok ? "written" : "failed");
        CHECK(fired || ok); // a fault not reached changes nothing
        ffmpegfaults::reset();
    }
}

// A failure while writing: the call fails with an error, the writer
// removes the file when it goes, and a packet the file refused isn't
// blamed on the encoder.
void testWriteFaults(const std::filesystem::path& dir)
{
    using ffmpegfaults::Call;
    const struct {
        Call call;
        int after;
        const char* name;
        bool audio;
    } faults[] = {
        {Call::WritePacket, 5, "a packet refused", false},
        {Call::WritePacket, 0, "an audio packet refused", true},
        {Call::CloseOutput, 0, "the close failing", false},
        {Call::MakeWritable, 3, "a frame not writable", false},
        {Call::MakeWritable, 0, "an audio frame not writable", true},
    };
    for (const auto& f : faults) {
        const exportpipe::FlattenConfig cfg = faultConfig(dir, f.audio);
        ffmpegfaults::reset();
        std::string err;
        {
            exportpipe::FlattenWriter w;
            if (!w.open(cfg, err)) {
                std::printf("FAIL  %s: open: %s\n", f.name, err.c_str());
                ++g_failures;
                continue;
            }
            CHECK(std::filesystem::exists(cfg.out_path));
            ffmpegfaults::arm(f.call, f.after, AVERROR(EIO));
            const bool ok = writeAll(w, f.audio, err);
            std::printf("%-28s: %s\n", f.name, ok ? "written" : err.c_str());
            CHECK(!ok);
            CHECK(!err.empty());
            CHECK(ffmpegfaults::failed(f.call) == 1);
            CHECK(!w.encoderFailed());
        }
        CHECK(!std::filesystem::exists(cfg.out_path));
        ffmpegfaults::reset();
    }

    // Given up on before finish() (a cancel): no file either.
    const exportpipe::FlattenConfig cfg = faultConfig(dir, false);
    {
        exportpipe::FlattenWriter w;
        std::string err;
        CHECK(w.open(cfg, err));
        std::vector<uint8_t> nv12(static_cast<size_t>(kW) * kH * 3 / 2, 128);
        CHECK(w.addNv12Frame(nv12.data(), kW, kH, err));
    }
    CHECK(!std::filesystem::exists(cfg.out_path));
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
    testOpenFaults(dir);
    testWriteFaults(dir);

    std::filesystem::remove_all(dir);
    if (g_failures)
        std::printf("flatten_writer_test: %d failure(s)\n", g_failures);
    return g_failures;
}
