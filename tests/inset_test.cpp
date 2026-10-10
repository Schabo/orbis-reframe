// Exportpipe::FlatVideoReader and InsetCompositor on generated video, plus the
// CPU export's colour grade (ReframeRenderer). Writes small MPEG-4 files whose
// frames each carry their index in their colour, then checks frame timing,
// seeking, rotation and the NV12 blend. Exit code is the failure count.

#include "FlatVideoReader.h"
#include "InsetCompositor.h"
#include "ReframeRenderer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

constexpr int kW = 320, kH = 180, kFps = 25, kFrames = 50;

// Frame i's luma: distinct steps a lossy encode can't blur together.
int lumaFor(int i) { return 20 + i * 4; }

// `rotationCw`: tag the stream with a display matrix. `split`: left half dark,
// right half bright (to see which way a rotation went) instead of solid.
bool writeVideo(const std::string &path, int rotationCw, bool split)
{
    AVFormatContext *fmt = nullptr;
    avformat_alloc_output_context2(&fmt, nullptr, nullptr, path.c_str());
    if (!fmt)
        return false;
    const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!enc)
        return false;
    AVCodecContext *ctx = avcodec_alloc_context3(enc);
    ctx->width = kW;
    ctx->height = kH;
    ctx->time_base = AVRational{1, kFps};
    ctx->framerate = AVRational{kFps, 1};
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->gop_size = 10;
    ctx->max_b_frames = 0;
    ctx->flags |= AV_CODEC_FLAG_QSCALE;
    ctx->global_quality = FF_QP2LAMBDA * 2;
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(ctx, enc, nullptr) < 0)
        return false;
    AVStream *st = avformat_new_stream(fmt, nullptr);
    avcodec_parameters_from_context(st->codecpar, ctx);
    st->time_base = ctx->time_base;
    if (rotationCw) {
        AVPacketSideData *sd = av_packet_side_data_new(&st->codecpar->coded_side_data,
                                                       &st->codecpar->nb_coded_side_data,
                                                       AV_PKT_DATA_DISPLAYMATRIX, 9 * 4, 0);
        av_display_rotation_set(reinterpret_cast<int32_t *>(sd->data), rotationCw); // clockwise
    }
    if (avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE) < 0 ||
        avformat_write_header(fmt, nullptr) < 0)
        return false;
    AVFrame *f = av_frame_alloc();
    f->format = ctx->pix_fmt;
    f->width = kW;
    f->height = kH;
    av_frame_get_buffer(f, 0);
    AVPacket *pkt = av_packet_alloc();
    auto drain = [&](AVFrame *in) {
        avcodec_send_frame(ctx, in);
        while (avcodec_receive_packet(ctx, pkt) == 0) {
            av_packet_rescale_ts(pkt, ctx->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(fmt, pkt);
        }
    };
    for (int i = 0; i < kFrames; ++i) {
        av_frame_make_writable(f);
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x)
                f->data[0][y * f->linesize[0] + x] =
                    uint8_t(split ? (x < kW / 2 ? 40 : 220) : lumaFor(i));
        for (int y = 0; y < kH / 2; ++y)
            for (int x = 0; x < kW / 2; ++x) {
                f->data[1][y * f->linesize[1] + x] = 128;
                f->data[2][y * f->linesize[2] + x] = 128;
            }
        f->pts = i;
        drain(f);
    }
    drain(nullptr);
    av_write_trailer(fmt);
    av_packet_free(&pkt);
    av_frame_free(&f);
    avcodec_free_context(&ctx);
    avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    return true;
}

// Which frame a decoded solid picture is, from its (full-range RGB) grey.
int frameIndexOf(const exportpipe::VideoFrame &f)
{
    const uint8_t *p = &f.rgba[(size_t(f.height / 2) * f.width + f.width / 2) * 4];
    // Limited-range luma L -> full-range grey (L - 16) * 255 / 219.
    const double luma = 16.0 + p[1] * 219.0 / 255.0;
    return static_cast<int>(std::lround((luma - 20.0) / 4.0));
}

void testReader(const std::string &dir)
{
    const std::string path = dir + "/solid.mp4";
    CHECK(writeVideo(path, 0, false));
    exportpipe::FlatVideoReader::Info info;
    std::string err;
    CHECK(exportpipe::FlatVideoReader::probe(path, info, err));
    CHECK(info.width == kW && info.height == kH);
    CHECK(std::abs(info.durationMs - 2000) <= 40 && std::abs(info.fps - kFps) < 0.01);

    exportpipe::FlatVideoReader r;
    CHECK(r.open(path, err));
    const exportpipe::VideoFrame *f = r.frameAt(0);
    CHECK(f && f->ptsMs == 0 && frameIndexOf(*f) == 0);
    f = r.frameAt(1000); // forward
    CHECK(f && f->ptsMs == 1000 && frameIndexOf(*f) == 25);
    f = r.frameAt(1030); // same frame still showing
    CHECK(f && f->ptsMs == 1000);
    f = r.frameAt(500); // backwards: seeks
    CHECK(f && f->ptsMs == 480 && frameIndexOf(*f) == 12);
    f = r.frameAt(1900);
    CHECK(f && frameIndexOf(*f) == 47);
    // Past the end the last frame holds. (The encoder's last frame doesn't
    // decode -- ffprobe misses it too -- so that's frame 48.)
    f = r.frameAt(5000);
    CHECK(f && frameIndexOf(*f) >= 48);

    CHECK(r.seek(800));
    exportpipe::VideoFrame v;
    CHECK(r.readNext(v) && v.ptsMs == 800 && frameIndexOf(v) == 20);
    CHECK(r.readNext(v) && v.ptsMs == 840 && frameIndexOf(v) == 21);
    f = r.frameAt(900); // continues from where readNext left off
    CHECK(f && f->ptsMs == 880);

    r.setOutputSize(160, 90);
    f = r.frameAt(900);
    CHECK(f && f->width == 160 && f->height == 90 && f->rgba.size() == 160u * 90 * 4);
}

void testRotation(const std::string &dir)
{
    const std::string path = dir + "/rot90.mp4";
    CHECK(writeVideo(path, 90, true));
    exportpipe::FlatVideoReader r;
    std::string err;
    CHECK(r.open(path, err));
    CHECK(r.info().width == kH && r.info().height == kW); // portrait
    const exportpipe::VideoFrame *f = r.frameAt(0);
    CHECK(f && f->width == kH && f->height == kW);
    // Turned clockwise: the dark left half is now on top.
    auto grey = [&](int x, int y) { return f->rgba[(size_t(y) * f->width + x) * 4 + 1]; };
    CHECK(grey(kH / 2, 20) < 60 && grey(kH / 2, kW - 20) > 200);
}

void testCompositor(const std::string &dir)
{
    const std::string path = dir + "/solid.mp4";
    constexpr int W = 640, H = 360;
    std::vector<uint8_t> y(size_t(W) * H, 200), uv(size_t(W) * H / 2, 128);

    exportpipe::InsetClipJob j;
    j.path = path;
    j.startMs = 1000;
    j.trimInMs = 0;
    j.trimOutMs = 2000;
    j.aspect = double(kW) / kH;
    j.lookAt = [](int64_t) {
        inset::Look l;
        l.centerX = 0.5f;
        l.centerY = 0.5f;
        l.width = 0.5f;
        return l;
    };
    exportpipe::InsetCompositor comp({j}, W, H);
    std::string err;

    // Before the clip starts: untouched.
    CHECK(comp.composite(500, y.data(), W, uv.data(), W, err));
    CHECK(y[size_t(H / 2) * W + W / 2] == 200);

    // 500 ms in: frame 12 fills the rect (160, 90) .. (480, 270).
    CHECK(comp.composite(1500, y.data(), W, uv.data(), W, err));
    const int centre = y[size_t(H / 2) * W + W / 2];
    CHECK(std::abs(centre - lumaFor(12)) <= 3);
    CHECK(y[size_t(H / 2) * W + 100] == 200 && y[size_t(40) * W + W / 2] == 200); // outside
    CHECK(std::abs(y[size_t(91) * W + 161] - lumaFor(12)) <= 3); // just inside the corner
    CHECK(std::abs(int(uv[size_t(H / 4) * W + W / 2]) - 128) <= 2); // grey stays grey

    // Half opacity blends halfway; an edge pixel is part-covered.
    std::vector<uint8_t> y2(size_t(W) * H, 200);
    exportpipe::VideoFrame pic;
    pic.width = pic.height = 4;
    pic.rgba.assign(4 * 4 * 4, 0); // black (limited luma 16)
    inset::Rect rect;
    rect.x = 100.5;
    rect.y = 100;
    rect.w = 100;
    rect.h = 50;
    exportpipe::InsetCompositor::blendNv12(pic.rgba.data(), 4, 4, rect, 0.5f, 0xffffff, y2.data(),
                                           W, uv.data(), W, W, H);
    CHECK(y2[size_t(120) * W + 150] == 108); // (200 + 16) / 2
    CHECK(y2[size_t(120) * W + 100] == 154); // half-covered: 200 - 0.5 * 0.5 * 184
    CHECK(y2[size_t(120) * W + 99] == 200);

    // A border: its colour at the edge, the picture inside.
    std::vector<uint8_t> y3(size_t(W) * H, 128);
    rect.x = 100;
    rect.border = 4;
    exportpipe::InsetCompositor::blendNv12(pic.rgba.data(), 4, 4, rect, 1.0f, 0xffffff, y3.data(),
                                           W, uv.data(), W, W, H);
    CHECK(y3[size_t(120) * W + 101] == 235); // white border
    CHECK(y3[size_t(120) * W + 150] == 16);  // black picture

    // A clip whose file is gone fails the export with the path.
    exportpipe::InsetClipJob bad = j;
    bad.path = dir + "/missing.mp4";
    exportpipe::InsetCompositor comp2({bad}, W, H);
    CHECK(!comp2.composite(1500, y.data(), W, uv.data(), W, err) && !err.empty());
}

// The CPU export's colour grade (ReframeRenderer's gradeRgb(), the twin of
// src/render/shaders/color_grade.glsl): a flat equirect reprojects to the same
// flat colour, graded. Expected values worked out by hand from the formula.
std::array<int, 3> gradedFlat(std::array<uint8_t, 3> rgb, std::array<float, 4> grade)
{
    constexpr int kEqW = 16, kEqH = 8, kOut = 4;
    std::vector<uint8_t> eq(size_t(kEqW) * kEqH * 4);
    for (size_t i = 0; i < eq.size(); i += 4)
        eq[i] = rgb[0], eq[i + 1] = rgb[1], eq[i + 2] = rgb[2], eq[i + 3] = 255;
    std::vector<uint8_t> out(size_t(kOut) * kOut * 4);
    exportpipe::ReframeView view;
    std::copy(grade.begin(), grade.end(), view.colorGrade);
    exportpipe::RenderRectilinear(eq.data(), kEqW, kEqH, out.data(), kOut, kOut, view);
    return {out[0], out[1], out[2]};
}

void testColorGrade()
{
    using RGB = std::array<int, 3>;
    CHECK((gradedFlat({60, 90, 120}, {0, 0, 0, 0}) == RGB{60, 90, 120})); // neutral
    // exposure +1, contrast +30, saturation -40, temperature +60
    CHECK((gradedFlat({60, 90, 120}, {1, 30, -40, 60}) == RGB{185, 194, 204}));
    // exposure -1, contrast -50, saturation +100, temperature -100
    CHECK((gradedFlat({60, 90, 120}, {-1, -50, 100, -100}) == RGB{55, 90, 125}));
    CHECK((gradedFlat({100, 150, 200}, {1, 30, -40, 60}) == RGB{255, 255, 255})); // clamps
}

} // namespace

int main()
{
    const char *tmp = std::getenv("TMPDIR");
    std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/inset_test_XXXXXX";
    if (!mkdtemp(dir.data())) {
        std::printf("inset_test: cannot create a temp dir\n");
        return 1;
    }
    testReader(dir);
    testRotation(dir);
    testCompositor(dir);
    testColorGrade();
    if (!std::getenv("INSET_TEST_KEEP")) {
        for (const char *f : {"/solid.mp4", "/rot90.mp4"})
            std::remove((dir + f).c_str());
        std::remove(dir.c_str());
    } else {
        std::printf("inset_test: files kept in %s\n", dir.c_str());
    }

    if (g_failures == 0)
        std::printf("inset_test: all checks passed\n");
    else
        std::printf("inset_test: %d check(s) failed\n", g_failures);
    return g_failures;
}
