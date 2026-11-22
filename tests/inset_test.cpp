// exportpipe::FlatVideoReader and InsetCompositor on generated video, plus the
// CPU export's colour grade (ReframeRenderer). Writes small MPEG-4 files whose
// frames each carry their index in their colour, then checks frame timing,
// seeking, rotation and the NV12 blend. Exit code is the failure count.

#include "FlatVideoReader.h"
#include "InsetCompositor.h"
#include "InsetGeometry.h"
#include "ReframeRenderer.h"
#include "Sharpen.h"
#include "support/FfmpegFaults.h"
#include "support/TestMedia.h"

extern "C" {
#include <libavutil/error.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
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
    testmedia::VideoSpec spec;
    spec.width = kW;
    spec.height = kH;
    spec.fps = kFps;
    spec.frames = kFrames;
    spec.rotationCw = rotationCw;
    spec.luma = [split](int i, int x, int) {
        return uint8_t(split ? (x < kW / 2 ? 40 : 220) : lumaFor(i));
    };
    return testmedia::writeVideo(path, spec);
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

// readNext() to the end through any errors (at most a few in a row).
struct Run {
    int frames = 0;
    int errors = 0;
    std::string lastErr;
    bool inOrder = true; // frame n is the file's frame n
};

Run readThrough(exportpipe::FlatVideoReader &r)
{
    Run run;
    exportpipe::VideoFrame v;
    for (int errorsInARow = 0;;) {
        if (r.readNext(v)) {
            run.inOrder = run.inOrder && frameIndexOf(v) == run.frames;
            ++run.frames;
            errorsInARow = 0;
            continue;
        }
        if (r.lastError().empty())
            break;
        run.lastErr = r.lastError();
        ++run.errors;
        if (++errorsInARow > 3)
            break;
    }
    return run;
}

// Read and decode errors made on purpose (support/FfmpegFaults.h): each is
// reported, not taken for the end of the file, and the reader carries on
// after it; frameAt() never hands out an earlier frame for one that failed.
void testReaderErrors(const std::string &dir)
{
    using ffmpegfaults::Call;
    const std::string path = dir + "/solid.mp4"; // testReader's
    std::string err;
    ffmpegfaults::reset();
    int all = 0; // the frames the file decodes to (see testReader)
    {
        exportpipe::FlatVideoReader r;
        CHECK(r.open(path, err));
        const Run run = readThrough(r);
        CHECK(run.errors == 0 && run.inOrder);
        all = run.frames;
        CHECK(all >= kFrames - 1);
    }
    const auto faulty = [&](Call call, int after, int rc) {
        exportpipe::FlatVideoReader r;
        ffmpegfaults::reset();
        CHECK(r.open(path, err));
        ffmpegfaults::arm(call, after, rc);
        return readThrough(r);
    };

    Run run = faulty(Call::ReadFrame, 20, AVERROR(EIO));
    CHECK(run.errors == 1 && run.lastErr.find("Input/output error") != std::string::npos);
    CHECK(run.frames == all && run.inOrder);

    run = faulty(Call::ReceiveFrame, 30, AVERROR_INVALIDDATA);
    CHECK(run.errors == 1 && run.frames == all && run.inOrder);

    run = faulty(Call::SendFlush, 0, AVERROR(ENOMEM));
    CHECK(run.errors == 1 && run.frames == all && run.inOrder);

    // The refused packet's frame is lost; the rest still read to the end.
    run = faulty(Call::SendPacket, 20, AVERROR_INVALIDDATA);
    CHECK(run.errors == 1 && run.frames >= all - 1);

    // A decoder that wants input after the flush: an error, not a hang.
    {
        exportpipe::FlatVideoReader r;
        ffmpegfaults::reset();
        CHECK(r.open(path, err));
        ffmpegfaults::arm(Call::ReceiveDraining, 0, AVERROR(EAGAIN), -1);
        const Run stalled = readThrough(r);
        CHECK(stalled.errors > 0 && stalled.lastErr.find("stalled") != std::string::npos);
        ffmpegfaults::reset();
        const Run rest = readThrough(r);
        CHECK(rest.errors == 0 && stalled.frames + rest.frames == all);
    }

    // frameAt() with every read failing from frame 1 on: the frames already
    // decoded show, each at its own time, then null with the error -- not
    // the frame before -- and once the reads work again, the frame due.
    {
        exportpipe::FlatVideoReader r;
        ffmpegfaults::reset();
        CHECK(r.open(path, err));
        const exportpipe::VideoFrame *f = r.frameAt(0);
        CHECK(f && f->ptsMs == 0);
        ffmpegfaults::arm(Call::ReadFrame, 0, AVERROR(EIO), -1);
        int64_t failedAt = -1;
        for (int64_t ms = 40; ms < 2000; ms += 40) {
            f = r.frameAt(ms);
            if (!f) {
                failedAt = ms;
                break;
            }
            CHECK(f->ptsMs == ms && frameIndexOf(*f) == ms / 40);
        }
        CHECK(failedAt > 0 && r.lastError().find("Input/output error") != std::string::npos);
        ffmpegfaults::reset();
        f = r.frameAt(failedAt);
        CHECK(f && f->ptsMs == failedAt && r.lastError().empty());
    }

    // A rewind that fails after frames were read: an error, not a later
    // frame passed off as frame 0 -- through frameAt() going back and
    // through seek(). Before any read the start needs no seek.
    {
        exportpipe::FlatVideoReader r;
        ffmpegfaults::reset();
        CHECK(r.open(path, err));
        const exportpipe::VideoFrame *f = r.frameAt(400);
        CHECK(f && frameIndexOf(*f) == 10);
        ffmpegfaults::arm(Call::SeekFrame, 0, AVERROR(EIO), -1);
        CHECK(!r.frameAt(0) && r.lastError().find("seek to 0 ms") != std::string::npos);
        CHECK(!r.seek(0) && !r.lastError().empty());
        ffmpegfaults::reset();
        f = r.frameAt(0); // seeks again, and can
        CHECK(f && f->ptsMs == 0 && frameIndexOf(*f) == 0 && r.lastError().empty());
    }
    {
        exportpipe::FlatVideoReader r;
        ffmpegfaults::reset();
        CHECK(r.open(path, err));
        ffmpegfaults::arm(Call::SeekFrame, 0, AVERROR(ENOSYS), -1); // a file that can't seek
        const exportpipe::VideoFrame *f = r.frameAt(0);
        CHECK(f && f->ptsMs == 0 && frameIndexOf(*f) == 0 && r.lastError().empty());
        exportpipe::VideoFrame v;
        CHECK(r.readNext(v) && frameIndexOf(v) == 1);
        CHECK(!r.seek(0)); // read from now: the start is gone
        CHECK(!r.seek(800) && r.lastError().find("seek to 800 ms") != std::string::npos);
    }
    ffmpegfaults::reset();
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
    pic.rgba.assign(4 * 4 * 4, 0); // black (limited luma 16), opaque
    for (size_t i = 3; i < pic.rgba.size(); i += 4)
        pic.rgba[i] = 255;
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

    // The same into P010 (the 10-bit export): the same values, 10-bit codes
    // (x4) in the high bits of 16; strides in bytes.
    {
        const auto p010 = [](int v8) { return uint16_t((v8 * 4) << 6); };
        std::vector<uint16_t> y16(size_t(W) * H, p010(200)), uv16(size_t(W) * H / 2, p010(128));
        exportpipe::InsetCompositor::blendP010(pic.rgba.data(), 4, 4, rect, 0.5f, 0xffffff,
                                               reinterpret_cast<uint8_t *>(y16.data()), W * 2,
                                               reinterpret_cast<uint8_t *>(uv16.data()), W * 2,
                                               W, H);
        CHECK(y16[size_t(120) * W + 150] == p010(108));
        CHECK(y16[size_t(120) * W + 100] == p010(154));
        CHECK(y16[size_t(120) * W + 99] == p010(200));
        CHECK(uv16[size_t(60) * W + 150] == p010(128)); // black on grey: chroma stays
    }

    // A border: its colour at the edge, the picture inside.
    std::vector<uint8_t> y3(size_t(W) * H, 128);
    rect.x = 100;
    rect.border = 4;
    exportpipe::InsetCompositor::blendNv12(pic.rgba.data(), 4, 4, rect, 1.0f, 0xffffff, y3.data(),
                                           W, uv.data(), W, W, H);
    CHECK(y3[size_t(120) * W + 101] == 235); // white border
    CHECK(y3[size_t(120) * W + 150] == 16);  // black picture

    // A title's picture: premultiplied, transparent around the letters. A
    // white left half over a clear right half leaves the right untouched.
    {
        exportpipe::InsetClipJob t;
        t.startMs = 0;
        t.trimInMs = 0;
        t.trimOutMs = 1000;
        t.aspect = 2.0;
        auto still = std::make_shared<std::vector<uint8_t>>(size_t(8) * 4 * 4, 0);
        for (int py = 0; py < 4; ++py)
            for (int px = 0; px < 4; ++px) {
                uint8_t *p = still->data() + (size_t(py) * 8 + px) * 4;
                p[0] = p[1] = p[2] = p[3] = 255;
            }
        exportpipe::Still pic;
        pic.rgba = still;
        pic.width = 8;
        pic.height = 4;
        t.still = std::make_shared<exportpipe::FixedStill>(pic);
        t.lookAt = [](int64_t) {
            inset::Look l;
            l.centerX = 0.5f;
            l.centerY = 0.5f;
            l.width = 0.5f; // rect (160, 110) .. (480, 270)
            return l;
        };
        std::vector<uint8_t> y4(size_t(W) * H, 100), uv4(size_t(W) * H / 2, 128);
        exportpipe::InsetCompositor tc({t}, W, H);
        CHECK(tc.composite(500, y4.data(), W, uv4.data(), W, err));
        CHECK(y4[size_t(H / 2) * W + 200] == 235);          // white, opaque
        CHECK(y4[size_t(H / 2) * W + 440] == 100);          // clear: what was under it
        CHECK(std::abs(int(uv4[size_t(H / 4) * W + 440]) - 128) <= 1);
        CHECK(tc.composite(1500, y4.data(), W, uv4.data(), W, err)); // past it: no file to read

        // Into P010: 10-bit white (940), the clear part untouched.
        std::vector<uint16_t> y16(size_t(W) * H, uint16_t(400 << 6));
        std::vector<uint16_t> uv16(size_t(W) * H / 2, uint16_t(512 << 6));
        CHECK(tc.composite(500, reinterpret_cast<uint8_t *>(y16.data()), W * 2,
                           reinterpret_cast<uint8_t *>(uv16.data()), W * 2, err, true));
        CHECK(y16[size_t(H / 2) * W + 200] == uint16_t(940 << 6));
        CHECK(y16[size_t(H / 2) * W + 440] == uint16_t(400 << 6));
        CHECK(uv16[size_t(H / 4) * W + 440] == uint16_t(512 << 6));
    }

    // A clip whose file is gone fails the export with the path.
    exportpipe::InsetClipJob bad = j;
    bad.path = dir + "/missing.mp4";
    exportpipe::InsetCompositor comp2({bad}, W, H);
    CHECK(!comp2.composite(1500, y.data(), W, uv.data(), W, err) && !err.empty());
}

// NV12 frames (Format::Nv12): via RGBA for a small clip (untagged, so
// BT.601 by its size), straight from the decoder for an HD one (BT.709),
// and turned for a rotated one.
void testReaderNv12(const std::string &dir)
{
    std::string err;
    exportpipe::FlatVideoReader r;
    CHECK(r.open(dir + "/solid.mp4", err));
    r.setOutputFormat(exportpipe::FlatVideoReader::Format::Nv12);
    const exportpipe::VideoFrame *f = r.frameAt(1000);
    CHECK(f && f->width == kW && f->height == kH && f->nv12.size() >= size_t(kW) * kH * 3 / 2);
    if (f) {
        CHECK(std::abs(f->nv12[size_t(kH / 2) * kW + kW / 2] - lumaFor(25)) <= 2); // grey
        CHECK(std::abs(f->nv12[size_t(kW) * kH + size_t(kH / 4) * kW + kW / 2] - 128) <= 2);
    }
    r.setOutputSize(161, 91); // NV12 wants even
    f = r.frameAt(1000);
    CHECK(f && f->width == 162 && f->height == 92);

    const std::string hd = dir + "/hd.mp4";
    testmedia::VideoSpec spec;
    spec.width = 1280;
    spec.height = 720;
    spec.frames = 5;
    spec.luma = [](int, int x, int) { return uint8_t(x < 640 ? 40 : 220); };
    CHECK(testmedia::writeVideo(hd, spec));
    exportpipe::FlatVideoReader h;
    CHECK(h.open(hd, err));
    h.setOutputFormat(exportpipe::FlatVideoReader::Format::Nv12);
    f = h.frameAt(0);
    CHECK(f && f->width == 1280 && f->height == 720);
    if (f) {
        CHECK(std::abs(f->nv12[360 * 1280 + 100] - 40) <= 3);
        CHECK(std::abs(f->nv12[360 * 1280 + 1200] - 220) <= 3);
        CHECK(std::abs(f->nv12[1280 * 720 + 180 * 1280 + 200] - 128) <= 2);
    }

    exportpipe::FlatVideoReader rot;
    CHECK(rot.open(dir + "/rot90.mp4", err));
    rot.setOutputFormat(exportpipe::FlatVideoReader::Format::Nv12);
    f = rot.frameAt(0);
    CHECK(f && f->width == kH && f->height == kW);
    if (f) { // the dark left half on top
        CHECK(f->nv12[size_t(20) * kH + kH / 2] < 60);
        CHECK(f->nv12[size_t(kW - 20) * kH + kH / 2] > 200);
    }
}

// A flat clip filling the frame is copied in as NV12, not blended: the same
// picture to its edges, graded too, and a fill's overhang cropped.
void testFullFrameCopy(const std::string &dir)
{
    constexpr int W = 640, H = 360;
    auto job = [](const std::string &path, double aspect, bool fill) {
        exportpipe::InsetClipJob j;
        j.path = path;
        j.trimOutMs = 2000;
        j.aspect = aspect;
        const inset::Look look = inset::fitted(aspect, W, H, fill);
        j.lookAt = [look](int64_t) { return look; };
        return j;
    };
    std::string err;
    std::vector<uint8_t> y(size_t(W) * H, 200), uv(size_t(W) * H / 2, 60);
    exportpipe::InsetCompositor comp({job(dir + "/solid.mp4", double(kW) / kH, false)}, W, H);
    CHECK(comp.composite(1000, y.data(), W, uv.data(), W, err));
    for (const size_t at : {size_t(0), size_t(W - 1), size_t(H / 2) * W + W / 2, size_t(H) * W - 1})
        CHECK(std::abs(y[at] - lumaFor(25)) <= 3);
    CHECK(std::abs(uv[0] - 128) <= 2 && std::abs(uv.back() - 128) <= 2);

    // Graded: what the screenshot's blend from RGBA gives, in NV12's luma.
    exportpipe::InsetClipJob g = job(dir + "/solid.mp4", double(kW) / kH, false);
    g.colorGrade.adjust = {1.0f, 0.0f, 0.0f, 0.0f}; // +1 EV
    exportpipe::InsetCompositor graded({g}, W, H);
    CHECK(graded.composite(1000, y.data(), W, uv.data(), W, err));
    std::vector<uint8_t> rgb(size_t(W) * H * 3, 0);
    exportpipe::InsetCompositor gradedRgb({g}, W, H);
    CHECK(gradedRgb.compositeRgb(1000, rgb.data(), W * 3, 3, err));
    const uint8_t *px = &rgb[(size_t(H / 2) * W + W / 2) * 3];
    const double luma709 =
        16.0 + 219.0 * (0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2]) / 255.0;
    CHECK(y[size_t(H / 2) * W + W / 2] > lumaFor(25) + 10);
    // (The copy decodes at the rect's size, the blend at the source's:
    // swscale's scaled YUV -> RGB comes out ~2 levels darker -- the ffmpeg
    // command line's too -- doubled by the +1 EV.)
    CHECK(std::abs(y[size_t(H / 2) * W + W / 2] - luma709) <= 5);

    // A 4:3 clip filling the 16:9 frame: 640 x 480, 60 rows cropped off the
    // top and bottom -- its dark top half ends at output row 180.
    const std::string tall = dir + "/tall.mp4";
    testmedia::VideoSpec spec;
    spec.width = 240;
    spec.height = 180;
    spec.frames = 5;
    spec.luma = [](int, int, int py) { return uint8_t(py < 90 ? 40 : 220); };
    CHECK(testmedia::writeVideo(tall, spec));
    exportpipe::InsetCompositor fill({job(tall, 4.0 / 3.0, true)}, W, H);
    std::fill(y.begin(), y.end(), 0);
    CHECK(fill.composite(0, y.data(), W, uv.data(), W, err));
    CHECK(std::abs(y[W / 2] - 40) <= 4 && std::abs(y[size_t(170) * W + W / 2] - 40) <= 4);
    CHECK(std::abs(y[size_t(190) * W + W / 2] - 220) <= 4);
    CHECK(std::abs(y[size_t(H - 1) * W + W / 2] - 220) <= 4);
}

// The CPU export's colour grade (ReframeRenderer's gradeRgb(), the twin of
// src/render/shaders/color_grade.glsl): a flat equirect reprojects to the same
// flat colour, graded. Expected values worked out by hand from the formula.
std::array<int, 3> gradedFlat(std::array<uint8_t, 3> rgb, std::array<float, 4> grade,
                              std::shared_ptr<const color::Lut3D> lut = nullptr, float mix = 1.0f)
{
    constexpr int kEqW = 16, kEqH = 8, kOut = 4;
    std::vector<uint8_t> eq(size_t(kEqW) * kEqH * 4);
    for (size_t i = 0; i < eq.size(); i += 4)
        eq[i] = rgb[0], eq[i + 1] = rgb[1], eq[i + 2] = rgb[2], eq[i + 3] = 255;
    std::vector<uint8_t> out(size_t(kOut) * kOut * 4);
    exportpipe::ReframeView view;
    view.colorGrade.adjust = grade;
    view.colorGrade.lut = std::move(lut);
    view.colorGrade.lutMix = mix;
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

    // A LUT (2^3, inverting), mixed by its strength, and before the sliders.
    std::string invert = "LUT_3D_SIZE 2\n";
    for (int b = 0; b < 2; ++b)
        for (int g = 0; g < 2; ++g)
            for (int r = 0; r < 2; ++r)
                invert += std::to_string(1 - r) + " " + std::to_string(1 - g) + " " +
                          std::to_string(1 - b) + "\n";
    const auto lut = color::parseCube(invert);
    CHECK(lut != nullptr);
    CHECK((gradedFlat({60, 90, 120}, {0, 0, 0, 0}, lut) == RGB{195, 165, 135}));
    CHECK((gradedFlat({60, 90, 120}, {0, 0, 0, 0}, lut, 0.5f) == RGB{128, 128, 128}));
    // Inverted, then grey: the luma of (195, 165, 135).
    CHECK((gradedFlat({60, 90, 120}, {0, 0, -100, 0}, lut) == RGB{171, 171, 171}));
    // GradeRgba (a flat clip's frame) applies it too.
    uint8_t px[4] = {60, 90, 120, 255};
    color::Grade g;
    g.lut = lut;
    exportpipe::GradeRgba(px, 1, g);
    CHECK(px[0] == 195 && px[1] == 165 && px[2] == 135 && px[3] == 255);
}

// A flat clip's effects: placed against the output frame from where its
// picture sits in it, so a picture in the frame's middle is untouched and
// one in its corner darkened -- as fx::applyEffects() says.
void testEffects()
{
    fx::ClipEffects e;
    e.vignette = fx::Vignette{true, -80.0f, 20.0f, 0.0f};
    color::Grade g;
    g.effects = e.render();
    // Without where it sits: the grade alone (a neutral one: untouched).
    uint8_t one[4] = {200, 100, 50, 255};
    exportpipe::GradeRgba(one, 1, g);
    CHECK(one[0] == 200 && one[1] == 100 && one[2] == 50);
    // A 2 x 2 picture covering a 20 x 20 rect in the middle of a 1000 x 500
    // frame, and one in its top-left corner.
    const auto pic = [] { return std::vector<uint8_t>(16, 200); };
    std::vector<uint8_t> mid = pic(), corner = pic();
    const float midRect[4] = {490, 240, 20, 20}, cornerRect[4] = {0, 0, 20, 20};
    exportpipe::GradeRgba(mid.data(), 2, 2, g, midRect, 1000, 500);
    exportpipe::GradeRgba(corner.data(), 2, 2, g, cornerRect, 1000, 500);
    CHECK(mid[0] == 200 && mid[15] == 200); // alpha untouched
    float want[3] = {200, 200, 200};
    fx::applyEffects(want, 5.0f, 5.0f, 1000.0f, 500.0f, g.effects, 255.0f);
    CHECK(corner[0] == uint8_t(want[0] + 0.5f) && corner[0] < 100 && corner[3] == 200);
}

// A flat clip's colour look: after the grade, on both CPU grade paths (with
// and without where the picture sits), mixed in by its strength.
void testLook()
{
    color::Grade g;
    g.adjust = {0.0f, 0.0f, 0.0f, 0.0f};
    g.setLook(color::look::presetLut(color::look::kBlackWhite), 1.0f);
    CHECK(g.hasLook() && g.hasEffects());
    float bw[3];
    const float in[3] = {200 / 255.0f, 100 / 255.0f, 50 / 255.0f};
    color::look::presetLut(color::look::kBlackWhite)->apply(in, bw);
    const uint8_t grey = uint8_t(bw[0] * 255.0f + 0.5f);
    uint8_t one[4] = {200, 100, 50, 255};
    exportpipe::GradeRgba(one, 1, g);
    CHECK(one[0] == grey && one[1] == grey && one[2] == grey && one[3] == 255);
    std::vector<uint8_t> pic = {200, 100, 50, 255, 200, 100, 50, 255,
                                200, 100, 50, 255, 200, 100, 50, 255};
    const float rect[4] = {490, 240, 20, 20};
    exportpipe::GradeRgba(pic.data(), 2, 2, g, rect, 1000, 500);
    CHECK(pic[0] == grey && pic[13] == grey && pic[15] == 255);
    // Half strength: halfway between the colour and the grey.
    g.setLook(g.look, 0.5f);
    uint8_t half[4] = {200, 100, 50, 255};
    exportpipe::GradeRgba(half, 1, g);
    CHECK(std::abs(int(half[0]) - int(std::lround((200 + bw[0] * 255.0f) / 2))) <= 1);
    CHECK(std::abs(int(half[2]) - int(std::lround((50 + bw[0] * 255.0f) / 2))) <= 1);
    // After the grade: exposure first, then the look.
    g.setLook(g.look, 1.0f);
    g.adjust = {1.0f, 0.0f, 0.0f, 0.0f};
    uint8_t lit[4] = {100, 50, 25, 255};
    exportpipe::GradeRgba(lit, 1, g);
    CHECK(lit[0] == grey && lit[2] == grey);
}

// The sharpen on the CPU export's two paths. A 360 view: a flat picture
// stays as it is (and still gets the vignette after it), an edge
// overshoots on both sides, and the pixels outside a globe's disc stay
// black. A flat clip's picture: sharpened in its own pixels, the radius
// against the frame's height -- as fx::sharpen() on the graded picture.
void testSharpen()
{
    fx::ClipEffects e;
    e.sharpen = fx::Sharpen{true, 150.0f, 100.0f, 0.0f};
    exportpipe::ReframeView view;
    view.colorGrade.effects = e.render();
    constexpr int kEqW = 1024, kEqH = 512, kOut = 64;
    std::vector<uint8_t> eq(size_t(kEqW) * kEqH * 4, 255);
    const auto paint = [&](int split) { // columns before `split` dark, the rest light
        for (int y = 0; y < kEqH; ++y)
            for (int x = 0; x < kEqW; ++x)
                for (int k = 0; k < 3; ++k)
                    eq[(size_t(y) * kEqW + x) * 4 + k] = x < split ? 60 : 180;
    };
    std::vector<uint8_t> plain(size_t(kOut) * kOut * 4), sharp(plain.size());
    paint(0);
    exportpipe::RenderRectilinear(eq.data(), kEqW, kEqH, sharp.data(), kOut, kOut, view);
    CHECK(std::all_of(sharp.begin(), sharp.end(), [](uint8_t v) { return v == 180 || v == 255; }));
    // with a vignette: the same as the vignette alone
    e.vignette = fx::Vignette{true, -60.0f, 20.0f, 50.0f};
    view.colorGrade.effects = e.render();
    exportpipe::RenderRectilinear(eq.data(), kEqW, kEqH, sharp.data(), kOut, kOut, view);
    exportpipe::ReframeView vig = view;
    vig.colorGrade.effects.sharpen = {};
    exportpipe::RenderRectilinear(eq.data(), kEqW, kEqH, plain.data(), kOut, kOut, vig);
    CHECK(sharp == plain && sharp[0] < 180);
    e.vignette.reset();
    view.colorGrade.effects = e.render();

    // An edge straight ahead (the view's middle column looks at the
    // equirect's middle), sharpened at a 1080-line frame.
    paint(kEqW / 2);
    view.frame[0] = view.frame[1] = 0.0f;
    view.frame[2] = 1920.0f;
    view.frame[3] = 1080.0f;
    vig.colorGrade.effects = {};
    vig.pose.fov = view.pose.fov = 0.7; // 40 degrees
    exportpipe::RenderRectilinear(eq.data(), kEqW, kEqH, plain.data(), kOut, kOut, vig);
    exportpipe::RenderRectilinear(eq.data(), kEqW, kEqH, sharp.data(), kOut, kOut, view);
    const auto at = [](const std::vector<uint8_t> &px, int x) {
        return int(px[(size_t(kOut / 2) * kOut + x) * 4 + 1]);
    };
    int lo = 255, hi = 0;
    for (int x = 0; x < kOut; ++x) {
        lo = std::min(lo, at(sharp, x));
        hi = std::max(hi, at(sharp, x));
    }
    CHECK(lo < 55 && hi > 185 && at(sharp, 2) == at(plain, 2) && at(sharp, kOut - 3) == 180);

    // A globe: the corners outside its disc stay black.
    exportpipe::ReframeView globe = view;
    globe.pose.fov = 3.0;
    globe.pose.distortion = 2.0f; // the orthographic end: a disc
    exportpipe::RenderRectilinear(eq.data(), kEqW, kEqH, sharp.data(), kOut, kOut, globe);
    CHECK(sharp[0] == 0 && sharp[1] == 0 && sharp[2] == 0 && sharp[3] == 255);

    // A flat clip's 32 x 8 picture covering 64 x 16 pixels of a 1080-line
    // frame: sharpened at half the radius in its own pixels.
    constexpr int kW = 32, kH = 8;
    std::vector<uint8_t> pic(size_t(kW) * kH * 4);
    std::vector<float> want(size_t(kW) * kH * 4);
    for (int i = 0; i < kW * kH; ++i)
        for (int k = 0; k < 4; ++k) {
            const uint8_t v = k == 3 ? 255 : (i % kW < kW / 2 ? 60 : 180);
            pic[size_t(i) * 4 + k] = v;
            want[size_t(i) * 4 + k] = v;
        }
    color::Grade g;
    g.effects = e.render();
    const float rect[4] = {100.0f, 100.0f, 64.0f, 16.0f};
    exportpipe::GradeRgba(pic.data(), kW, kH, g, rect, 1920, 1080);
    fx::sharpen(want.data(), 4, nullptr, kW, kH, 540.0f, g.effects, 255.0f);
    bool same = true;
    for (size_t i = 0; i < pic.size(); ++i)
        same = same && std::abs(int(pic[i]) - int(std::lround(want[i]))) <= 1;
    CHECK(same && pic[(kW / 2 - 1) * 4] < 60 && pic[(kW / 2) * 4] > 180);
}

} // namespace

// A layer whose pictures come from a PictureSource (a transition's other
// clip, a 360 clip's view): asked for at its source time and at the rect's
// size, faded by its opacity ramp; one that can't open fails the frame.
namespace {

struct FakePicture : exportpipe::PictureSource {
    std::vector<int64_t> *asked;
    bool ok;
    int w = 0, h = 0;
    exportpipe::VideoFrame frame;
    std::string error;
    FakePicture(std::vector<int64_t> *a, bool o) : asked(a), ok(o) {}
    bool open(std::string &err) override
    {
        if (!ok)
            err = "no such clip";
        return ok;
    }
    double fps() const override { return 30.0; }
    void setOutputSize(int width, int height) override
    {
        w = width;
        h = height;
    }
    const exportpipe::VideoFrame *frameAt(int64_t ms) override
    {
        asked->push_back(ms);
        frame.ptsMs = ms;
        frame.width = w;
        frame.height = h;
        frame.rgba.assign(size_t(w) * h * 4, 255); // white
        return &frame;
    }
    bool readNext(exportpipe::VideoFrame &) override { return false; }
    const std::string &lastError() const override { return error; }
};

} // namespace

void testPictureLayer()
{
    constexpr int W = 64, H = 36;
    std::vector<int64_t> asked;
    exportpipe::InsetClipJob j;
    j.path = "/c/b.insv";
    j.startMs = 1000;
    j.trimInMs = 4000;
    j.trimOutMs = 6000;
    j.speed = 2.0; // 1 s on the timeline
    j.aspect = double(W) / H;
    j.rampFrom = 0.0f;
    j.rampTo = 0.5f;
    j.lookAt = [](int64_t) { return inset::fitted(double(W) / H, W, H, false); };
    j.picture = [&asked] { return std::make_unique<FakePicture>(&asked, true); };
    exportpipe::InsetCompositor comp({j}, W, H);
    std::vector<uint8_t> y(size_t(W) * H), uv(size_t(W) * H / 2);
    std::string err;
    const auto lumaAt = [&](int64_t ms) {
        std::fill(y.begin(), y.end(), 16); // black
        std::fill(uv.begin(), uv.end(), 128);
        CHECK(comp.composite(ms, y.data(), W, uv.data(), W, err));
        return int(y[size_t(H / 2) * W + W / 2]);
    };
    CHECK(lumaAt(500) == 16 && asked.empty()); // before it
    CHECK(lumaAt(1000) == 16);                 // opacity 0 at its start
    CHECK(std::abs(lumaAt(1500) - (16 + 219 / 4)) <= 2); // a quarter
    CHECK(std::abs(lumaAt(1990) - (16 + 219 / 2)) <= 3); // nearly half at its end
    CHECK(lumaAt(2000) == 16); // past it
    CHECK(asked.size() >= 2 && asked.back() == 4000 + 990 * 2); // its source time
    exportpipe::InsetClipJob bad = j;
    bad.picture = [&asked] { return std::make_unique<FakePicture>(&asked, false); };
    exportpipe::InsetCompositor failing({bad}, W, H);
    CHECK(!failing.composite(1500, y.data(), W, uv.data(), W, err));
    CHECK(err.find("/c/b.insv") != std::string::npos && err.find("no such clip") != std::string::npos);
}

// inset::moved: a slide from / to an edge of the frame, a pop; the out part
// is the in part backwards; a clip too short for both shares its span.
void testMotion()
{
    using M = inset::Motion;
    inset::Look rest;
    rest.centerX = 0.3f;
    rest.centerY = 0.7f;
    rest.width = 0.2f;
    constexpr double pic = 4.0, out = 16.0 / 9.0; // picture 4:1 on a 16:9 frame
    const auto near = [](double a, double b) { return std::abs(a - b) < 1e-5; };
    CHECK(inset::moved(rest, M{}, 0, 5000, pic, out) == rest);

    M m;
    m.in = {M::Kind::Slide, M::Side::Left, 1000};
    m.out = {M::Kind::Slide, M::Side::Bottom, 500};
    const inset::Look start = inset::moved(rest, m, 0, 5000, pic, out);
    CHECK(near(start.centerX, -0.1) && start.centerY == rest.centerY && start.width == rest.width);
    const inset::Look half = inset::moved(rest, m, 500, 5000, pic, out);
    CHECK(half.centerX > (start.centerX + rest.centerX) / 2 && half.centerX < rest.centerX);
    CHECK(inset::moved(rest, m, 1000, 5000, pic, out) == rest);
    CHECK(inset::moved(rest, m, 4500, 5000, pic, out) == rest);
    // Out at the bottom: just below the frame at the end (height = 0.2 *
    // 16/9 / 4 of the frame's), as far along at 4900 as the in was at 200.
    const inset::Look end = inset::moved(rest, m, 5000, 5000, pic, out);
    CHECK(near(end.centerY, 1.0 + 0.2 * out / pic / 2) && end.centerX == rest.centerX);
    const double inAt200 = (inset::moved(rest, m, 200, 5000, pic, out).centerX - rest.centerX) /
                           (start.centerX - rest.centerX);
    const double outAt4900 = (inset::moved(rest, m, 4900, 5000, pic, out).centerY - rest.centerY) /
                             (end.centerY - rest.centerY);
    CHECK(near(inAt200, outAt4900));
    // The other sides.
    m.in.side = M::Side::Right;
    CHECK(near(inset::moved(rest, m, 0, 5000, pic, out).centerX, 1.1));
    m.in.side = M::Side::Top;
    CHECK(near(inset::moved(rest, m, 0, 5000, pic, out).centerY, -0.2 * out / pic / 2));

    // A pop: from nothing, past full size, back to it; in place.
    m.in = {M::Kind::Pop, M::Side::Left, 400};
    m.out = {};
    CHECK(inset::moved(rest, m, 0, 5000, pic, out).width < 1e-6f);
    float widest = 0.0f;
    for (int t = 0; t <= 400; t += 10) {
        const inset::Look l = inset::moved(rest, m, t, 5000, pic, out);
        widest = std::max(widest, l.width);
        CHECK(l.centerX == rest.centerX && l.centerY == rest.centerY);
    }
    CHECK(widest > rest.width * 1.05f && widest < rest.width * 1.15f);
    CHECK(inset::moved(rest, m, 400, 5000, pic, out) == rest);

    // Too short for both: each gets its share (here half of 600 ms).
    m.in = {M::Kind::Slide, M::Side::Left, 1000};
    m.out = {M::Kind::Slide, M::Side::Left, 1000};
    CHECK(inset::moved(rest, m, 300, 600, pic, out) == rest);
    CHECK(inset::moved(rest, m, 150, 600, pic, out).centerX < rest.centerX);
    CHECK(near(inset::moved(rest, m, 600, 600, pic, out).centerX, -0.1));
}

// A title's picture sliding in, in the export: off the frame at its start,
// in place once in.
void testMotionLayer()
{
    constexpr int W = 64, H = 36;
    exportpipe::InsetClipJob j;
    j.startMs = 1000;
    j.trimInMs = 0;
    j.trimOutMs = 3000;
    j.aspect = 2.0;
    exportpipe::Still pic;
    pic.rgba = std::make_shared<const std::vector<uint8_t>>(size_t(8) * 4 * 4, uint8_t(255));
    pic.width = 8;
    pic.height = 4;
    j.still = std::make_shared<exportpipe::FixedStill>(pic);
    j.lookAt = [](int64_t) {
        inset::Look l;
        l.centerX = 0.5f;
        l.centerY = 0.5f;
        l.width = 0.25f;
        return l;
    };
    j.motion.in = {inset::Motion::Kind::Slide, inset::Motion::Side::Left, 1000};
    exportpipe::InsetCompositor comp({j}, W, H);
    std::vector<uint8_t> y(size_t(W) * H), uv(size_t(W) * H / 2);
    std::string err;
    const auto lit = [&](int64_t ms) {
        std::fill(y.begin(), y.end(), 16);
        std::fill(uv.begin(), uv.end(), 128);
        CHECK(comp.composite(ms, y.data(), W, uv.data(), W, err));
        int n = 0;
        for (uint8_t v : y)
            n += v > 128;
        return n;
    };
    CHECK(lit(1000) == 0);                                     // just past the left edge
    CHECK(lit(1100) > 0 && y[size_t(H / 2) * W + W / 2] < 128); // on its way, not there yet
    CHECK(lit(2500) > 0 && y[size_t(H / 2) * W + W / 2] > 200); // in place
}

// inset::fitted: a flat main-track clip's full-frame layer.
void testFitted()
{
    // 16:9 into 16:9: the whole frame either way
    CHECK(inset::fitted(16.0 / 9.0, 1920, 1080, false).width == 1.0f);
    CHECK(inset::fitted(16.0 / 9.0, 1920, 1080, true).width == 1.0f);
    // portrait 9:16 into 16:9: bars at the sides, or as wide as the frame
    const inset::Look bars = inset::fitted(9.0 / 16.0, 1920, 1080, false);
    const inset::Rect r = inset::place(bars, {}, 9.0 / 16.0, 1920, 1080);
    CHECK(std::abs(r.h - 1080) < 0.5 && std::abs(r.x - (1920 - r.w) / 2) < 0.5 && r.y > -0.5);
    const inset::Rect f =
        inset::place(inset::fitted(9.0 / 16.0, 1920, 1080, true), {}, 9.0 / 16.0, 1920, 1080);
    CHECK(std::abs(f.w - 1920) < 0.5 && f.h > 1080 && std::abs(f.y + (f.h - 1080) / 2) < 0.5);
    // 16:9 into a 9:16 output: bars above and below, or cropped at the sides
    const inset::Rect w =
        inset::place(inset::fitted(16.0 / 9.0, 1080, 1920, false), {}, 16.0 / 9.0, 1080, 1920);
    CHECK(std::abs(w.w - 1080) < 0.5 && w.h < 1920);
    CHECK(inset::fitted(16.0 / 9.0, 1080, 1920, true).width > 3.0f);
}

int main()
{
    const char *tmp = std::getenv("TMPDIR");
    std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/inset_test_XXXXXX";
    if (!mkdtemp(dir.data())) {
        std::printf("inset_test: cannot create a temp dir\n");
        return 1;
    }
    testReader(dir);
    testReaderErrors(dir);
    testRotation(dir);
    testCompositor(dir);
    testReaderNv12(dir);
    testFullFrameCopy(dir);
    testColorGrade();
    testEffects();
    testLook();
    testSharpen();
    testFitted();
    testPictureLayer();
    testMotion();
    testMotionLayer();
    if (!std::getenv("INSET_TEST_KEEP")) {
        for (const char *f : {"/solid.mp4", "/rot90.mp4", "/hd.mp4", "/tall.mp4"})
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
