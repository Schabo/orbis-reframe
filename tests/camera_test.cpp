// camera::ReadClipMetadata on a ONE X2-style take: a lens per file, written
// here as small videos named as the camera names them, the lens-0 file
// carrying a synthetic trailer. The second lens's file is found beside it and
// checked; the clip's metadata follows a change to either file; every file of
// the take names the clip. And an Antigravity A1 clip: both lenses in one
// file, a slotted trailer, the proxy named without a lens token; renamed,
// found again by the names its files hold. And an X5 recording the camera
// split into two files: one clip, its parts found from either file, its IMU
// joined, its frames read on and seeked across the join. And, where the
// sample footage is there (the first argument), the DJI Avata 360 clip.
// Exit code is the failure count.

#include "ClipMetadata.h"
#include "MediaInfo.h"
#include "NativeStitchSource.h"
#include "ProbeClip.h"
#include <orbis-core/stab/GyroStabilizer.h>
#include <orbis-core/media/FisheyeDemux.h>
#include <orbis-core/insta360/InsvModels.h>
#include <orbis-core/insta360/InsvTrailer.h>
#include "support/FfmpegFaults.h"
#include "support/TestMedia.h"
#include "support/TestTrailer.h"

extern "C" {
#include <libavutil/error.h>
}

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
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

using testtrailer::Bytes;

constexpr int kFps = 25;

// The ONE X2's trailer, cut down: the model, the 40-token offset_v3, field 27
// and 24, and a little IMU stamped in ms, 2 ms apart (500 Hz). The real one
// has no field 7 (the recording time); `creationTime` adds it, as the X5
// writes it. `gps` adds a GPS record, and `firstFrameUnixMs` field 98 to line
// it up with the video (neither is on the ONE X2: the X5 writes them).
// The lens pictures' greys in the trailer's previews (records 0x200 / 0x500).
constexpr uint8_t kLens0Grey = 220;
constexpr uint8_t kLens1Grey = 40;

std::vector<uint8_t> oneX2Trailer(bool lensPictures = false, uint64_t creationTime = 0,
                                  const std::vector<uint8_t>& gps = {},
                                  uint64_t firstFrameUnixMs = 0,
                                  const std::vector<uint8_t>& anchors = {})
{
    Bytes meta;
    meta.pbString(1, "IXSE0000000001").pbString(2, "Insta360 ONE X2").pbString(3, "v1.0.92_build1");
    if (creationTime)
        meta.pbVarint(7, creationTime);
    meta.pbVarint(24, 4407);
    meta.pbDouble(25, 36.94);
    Bytes crop;
    crop.pbVarint(1, 3040).pbVarint(2, 3040).pbVarint(3, 2880).pbVarint(4, 2880);
    meta.pbBytes(27, crop.b);
    if (firstFrameUnixMs) {
        Bytes times;
        times.pbVarint(1, 4407).pbVarint(3, firstFrameUnixMs);
        meta.pbBytes(98, times.b);
    }
    meta.pbString(54, "2_1.728590_2322.610_2322.410_1528.430_1521.450_0.799_-0.157_178.977_"
                      "0_0_0_0.2279_-0.1489_-0.9684_-0.0019_0.0012_6080_3040_41_"
                      "1.728590_2318.480_2318.720_4554.650_1542.810_-0.848_-0.368_1.421_"
                      "0.0002_-0.0019_-0.0211_0.2436_-0.2279_-0.8443_0.0013_-0.0004_6080_3040_"
                      "41_199680");
    Bytes imu;
    for (int i = 0; i < 101; ++i) {
        imu.u64(2800 + uint64_t(i) * 2);
        for (double v : {0.0, 0.0, 1.0, 0.0, 0.0, 0.0}) imu.f64(v);
    }
    std::vector<testtrailer::TrailerRecord> records = {{0x300, imu.b}};
    if (!gps.empty())
        records.push_back({0x700, gps});
    if (!anchors.empty())
        records.push_back({0xa00, anchors});
    if (lensPictures) {
        records.push_back({0x200, testmedia::encodeH264Picture(64, 64, kLens0Grey)});
        records.push_back({0x500, testmedia::encodeH264Picture(64, 64, kLens1Grey)});
    }
    records.push_back({0x101, meta.b});
    return testtrailer::insvTrailer(records, false);
}

// An MP4 under the camera's name (the writer goes by the extension).
bool writeLens(const std::string& path, int size, int frames, int fps = kFps)
{
    testmedia::VideoSpec spec;
    spec.width = spec.height = size;
    spec.fps = fps;
    spec.frames = frames;
    if (!testmedia::writeVideo(path + ".mp4", spec))
        return false;
    std::filesystem::rename(path + ".mp4", path);
    return true;
}

void appendTo(const std::string& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(path, std::ios::binary | std::ios::app);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

struct Take {
    std::string lead, second, proxy;
};

// Writes the take: lens 0 with the trailer, lens 1 one frame shorter (as the
// camera writes it), the proxy. `stamp` is the name's date and time.
Take writeTake(const std::filesystem::path& dir, const std::string& stamp = "20250816_103807",
               uint64_t creationTime = 0, int fps = kFps)
{
    Take t{(dir / ("VID_" + stamp + "_00_001.insv")).string(),
           (dir / ("VID_" + stamp + "_10_001.insv")).string(),
           (dir / ("LRV_" + stamp + "_11_001.insv")).string()};
    CHECK(writeLens(t.lead, 64, 50, fps));
    appendTo(t.lead, oneX2Trailer(false, creationTime));
    CHECK(writeLens(t.second, 64, 49, fps));
    CHECK(writeLens(t.proxy, 32, 49, fps));
    return t;
}

// A seek of the take that fails for the second lens's file (the first's has
// moved): the next read seeks again rather than reading on from before it,
// and fails while that does too -- no frame from the old place.
void testFailedSeek(const Take& t)
{
    using ffmpegfaults::Call;
    std::printf("  a failed seek: the next read goes to its target, not on from before\n");
    setenv("ORBIS_NO_GPU_STITCH", "1", 1); // the same everywhere, GPU or not
    stitch::NativeStitchSource s;
    ffmpegfaults::reset();
    CHECK(s.Open(t.lead, stitch::PreviewStitchConfig{}));
    CHECK(s.ReadFrame().timestamp_ms == 0);
    CHECK(s.ReadFrame().timestamp_ms == 40);
    ffmpegfaults::arm(Call::SeekFrame, 1, AVERROR(EIO));
    CHECK(!s.SeekTo(1000));
    ffmpegfaults::reset();
    stitch::Frame f = s.ReadFrame();
    CHECK(!f.rgba.empty() && f.timestamp_ms == 1000);
    CHECK(s.ReadFrame().timestamp_ms == 1040);

    // The seeks keep failing: so do the reads, until they work.
    ffmpegfaults::arm(Call::SeekFrame, 1, AVERROR(EIO), -1);
    CHECK(!s.SeekTo(400));
    f = s.ReadFrame();
    CHECK(f.rgba.empty() && !s.GetLastError().empty());
    ffmpegfaults::reset();
    f = s.ReadFrame();
    CHECK(!f.rgba.empty() && f.timestamp_ms == 400);

    // A read that fails in the frames the seek measures the seam from
    // first: the seek fails, not a frame stitched from fewer of them; the
    // next read seeks again and has the frame a clean seek has.
    std::printf("  a read error before a seek's target fails the seek\n");
    CHECK(s.SeekTo(1000));
    const stitch::Frame clean = s.ReadFrame();
    CHECK(!clean.rgba.empty() && clean.timestamp_ms == 1000);
    ffmpegfaults::arm(Call::ReadFrame, 2, AVERROR(EIO));
    CHECK(!s.SeekTo(1000));
    CHECK(ffmpegfaults::failed(Call::ReadFrame) == 1 && !s.GetLastError().empty());
    ffmpegfaults::reset();
    f = s.ReadFrame();
    CHECK(f.timestamp_ms == 1000 && f.rgba == clean.rgba);
    unsetenv("ORBIS_NO_GPU_STITCH");
}

// Frame times that aren't whole ms (30 fps: 33.333, 66.667 ms, ...), read
// as ms rounded. A seek to the time a read reported finds that frame, from
// the full files and in proxy mode (where the clip's frames are a grid of
// their own); a seek a ms past it finds the next one. A source given back
// and taken up again after any frame -- frame 0 too -- carries on with the
// next frame, never that one again.
void testExactFrameTimes(const std::filesystem::path& dir)
{
    std::printf("  30 fps: seeks find the frame a read reported, a resume the next one\n");
    setenv("ORBIS_NO_GPU_STITCH", "1", 1); // the same everywhere, GPU or not
    std::filesystem::create_directories(dir / "thirty");
    const Take t = writeTake(dir / "thirty", "20250816_103807", 0, 30);
    {
        // A proxy proxy mode takes: the two lenses side by side.
        testmedia::VideoSpec spec;
        spec.width = 64;
        spec.height = 32;
        spec.fps = 30;
        spec.frames = 49;
        CHECK(testmedia::writeVideo(t.proxy + ".mp4", spec));
        std::filesystem::rename(t.proxy + ".mp4", t.proxy);
    }
    constexpr int kRead = 12;
    std::vector<int64_t> want; // frame n's time as reads report it
    for (int n = 0; n < kRead; ++n)
        want.push_back(orbiscore::MsOfUs(std::llround(n * 1e6 / 30.0)));
    for (const bool proxy : {false, true}) {
        stitch::PreviewStitchConfig sc;
        sc.proxy = proxy;
        stitch::NativeStitchSource s;
        CHECK(s.Open(t.lead, sc));
        CHECK(s.UsingProxy() == proxy);
        for (int n = 0; n < kRead; ++n)
            CHECK(s.ReadFrame().timestamp_ms == want[n]);
        for (int n = 0; n < kRead; ++n) {
            CHECK(s.SeekTo(want[n]));
            const int64_t got = s.ReadFrame().timestamp_ms;
            if (got != want[n])
                std::printf("    %s: seek to %lld ms gave %lld\n", proxy ? "proxy" : "full",
                            static_cast<long long>(want[n]), static_cast<long long>(got));
            CHECK(got == want[n]);
        }
        for (int n = 0; n + 1 < kRead; ++n) {
            CHECK(s.SeekTo(want[n] + 1));
            CHECK(s.ReadFrame().timestamp_ms == want[n + 1]);
        }
    }
    unsetenv("ORBIS_NO_GPU_STITCH");
    // Given back after every frame read: a source on the GPU, or fallen back
    // from it (not one told to stay on the CPU, which holds nothing to give).
    for (const bool proxy : {false, true}) {
        stitch::PreviewStitchConfig sc;
        sc.proxy = proxy;
        stitch::NativeStitchSource s;
        CHECK(s.Open(t.lead, sc));
        int released = 0;
        for (int n = 0; n < kRead; ++n) {
            const int64_t got = s.ReadFrame().timestamp_ms;
            if (got != want[n])
                std::printf("    %s: after a resume frame %d read as %lld ms, not %lld\n",
                            proxy ? "proxy" : "full", n, static_cast<long long>(got),
                            static_cast<long long>(want[n]));
            CHECK(got == want[n]);
            released += s.ReleaseGpu();
        }
        CHECK(released == kRead);
    }
}

void testPair(const Take& t)
{
    std::printf("  a lens per file: the take is one clip\n");
    std::string err;
    const auto clip = camera::ReadClipMetadata(t.lead, &err);
    CHECK(clip);
    if (!clip) {
        std::printf("  %s\n", err.c_str());
        return;
    }
    CHECK(clip->model == "Insta360 ONE X2");
    CHECK(clip->lenses.size() == 2);
    if (clip->lenses.size() == 2) {
        CHECK(clip->lenses[0].source.path == t.lead && clip->lenses[0].source.stream == 0);
        CHECK(clip->lenses[1].source.path == t.second && clip->lenses[1].source.stream == 0);
    }
    CHECK(clip->calibrated);
    CHECK(clip->lenses[0].calibration.focalRefW == 2880.0);
    CHECK(clip->lensWidth == 64 && clip->fps == kFps);
    CHECK(clip->durationMs == 49 * 1000 / kFps); // the shorter lens
    CHECK(clip->proxyPath == t.proxy);
    // The proxy's lenses: one stream, the _00_ lens the left half.
    const auto proxy = clip->proxySources();
    CHECK(proxy.size() == 1 && proxy[0].lenses.size() == 2);
    if (proxy.size() == 1 && proxy[0].lenses.size() == 2) {
        const auto& l = proxy[0].lenses;
        CHECK(l[0].path == t.proxy && l[0].stream == 0 && l[1].path == t.proxy && l[1].stream == 0);
        CHECK(l[0].rect.x == 0.0 && l[1].rect.x == 0.5 && l[0].rect.w == 0.5 &&
              l[1].rect.w == 0.5 && l[0].rect.h == 1.0);
    }
    CHECK(clip->hasMotion);
    CHECK(clip->imuRateHz > 499.0 && clip->imuRateHz < 501.0); // ms ticks read as such
}

// When recording started, by the camera's clock: field 7 where the file has
// it, else the file's name; nothing when neither makes a date and time.
void testRecordingTime(const Take& t, const std::filesystem::path& dir)
{
    std::printf("  the recording time: field 7, else the file name\n");
    const auto named = camera::ReadClipMetadata(t.lead);
    CHECK(named && named->recordedLocal == "2025-08-16T10:38:07");

    std::filesystem::create_directories(dir / "field7");
    const Take withField = writeTake(dir / "field7", "20250816_103807", 20250817123456ull);
    const auto field = camera::ReadClipMetadata(withField.lead);
    CHECK(field && field->recordedLocal == "2025-08-17T12:34:56");

    // Month 13 isn't a date: the name's time then, and a name without one
    // gives none.
    std::filesystem::create_directories(dir / "bad");
    const Take bad = writeTake(dir / "bad", "20250816_103807", 20251316123456ull);
    const auto badField = camera::ReadClipMetadata(bad.lead);
    CHECK(badField && badField->recordedLocal == "2025-08-16T10:38:07");
    std::filesystem::create_directories(dir / "renamed");
    const Take renamed = writeTake(dir / "renamed", "ride");
    const auto none = camera::ReadClipMetadata(renamed.lead);
    CHECK(none && none->recordedLocal.empty());
}

void testSecondLensChecked(const Take& t)
{
    std::printf("  the second lens's file is checked, and followed\n");
    std::string err;
    camera::ReadFailure why = camera::ReadFailure::NotCamera;
    const auto expectFailure = [&](const char* what) {
        err.clear();
        const bool none = !camera::ReadClipMetadata(t.lead, &err, &why);
        CHECK(none);
        CHECK(why == camera::ReadFailure::Unreadable);
        if (err.find(what) == std::string::npos) {
            std::printf("  expected \"%s\", got \"%s\"\n", what, err.c_str());
            CHECK(false);
        }
    };

    // Gone: the cached clip notices.
    CHECK(camera::ReadClipMetadata(t.lead));
    std::filesystem::rename(t.second, t.second + ".away");
    expectFailure("the second lens's file is missing (VID_20250816_103807_10_001.insv)");
    std::filesystem::rename(t.second + ".away", t.second);
    CHECK(camera::ReadClipMetadata(t.lead));

    // Another size.
    CHECK(writeLens(t.second, 32, 49));
    expectFailure("doesn't match the clip");
    // Much shorter.
    CHECK(writeLens(t.second, 64, 30));
    expectFailure("doesn't match the clip");
    // A clip of its own (it has a trailer).
    CHECK(writeLens(t.second, 64, 49));
    appendTo(t.second, oneX2Trailer());
    expectFailure("a clip of its own");

    CHECK(writeLens(t.second, 64, 49));
    CHECK(camera::ReadClipMetadata(t.lead));
}

void testClipFileOf(const Take& t)
{
    std::printf("  any file of the take names its clip\n");
    std::string err;
    CHECK(camera::ClipFileOf(t.lead) == t.lead);
    CHECK(camera::ClipFileOf(t.second, &err) == t.lead);
    CHECK(camera::ClipFileOf(t.proxy, &err) == t.lead);
    // Without its clip file: a reason, not a clip.
    std::filesystem::rename(t.lead, t.lead + ".away");
    CHECK(camera::ClipFileOf(t.second, &err).empty());
    CHECK(err == "part of the clip VID_20250816_103807_00_001.insv, which isn't in the same "
                 "folder");
    std::filesystem::rename(t.lead + ".away", t.lead);
    // Any other name is its own clip, read or not.
    const std::string other = (std::filesystem::path(t.lead).parent_path() / "clip.insv").string();
    CHECK(camera::ClipFileOf(other) == other);
}

// No stitched preview in the trailer, but each lens's first frame: the
// preview is stitched from them, so both lenses' pictures are in it.
void testStitchedThumbnail(const Take& t)
{
    std::printf("  the preview stitched from the lens pictures\n");
    if (testmedia::encodeH264Picture(64, 64, 128).empty()) {
        std::printf("  (skipped: no H.264 encoder in this FFmpeg)\n");
        return;
    }
    CHECK(!camera::ReadClipThumbnail(t.lead)); // no lens pictures yet
    CHECK(writeLens(t.lead, 64, 50));
    appendTo(t.lead, oneX2Trailer(true));
    const auto thumb = camera::ReadClipThumbnail(t.lead);
    CHECK(thumb.has_value());
    if (!thumb)
        return;
    CHECK(thumb->width == 1024 && thumb->height == 512);
    CHECK(thumb->nv12.size() == size_t(1024 * 512 * 3 / 2));
    // A bright half and a dark one (not just the greys themselves: the
    // stitch fades one into the other across the seam).
    int bright = 0, dark = 0;
    for (int i = 0; i < 1024 * 512; ++i) {
        const int y = thumb->nv12[size_t(i)];
        bright += y > 150;
        dark += y < 70;
    }
    CHECK(bright > 1024 * 512 / 5 && dark > 1024 * 512 / 5);
}

// The GPS track in the clip's video time: field 98.3 is its first frame.
// Without field 98 it isn't used; without GPS there's none.
void testGps(const std::filesystem::path& dir)
{
    std::printf("  the GPS track, lined up with the video\n");
    constexpr uint64_t t0 = 1700000000000ull;
    Bytes gps;
    for (int i = 0; i < 30; ++i) // a fix a second, each over 10 records
        gps.raw(testtrailer::gpsRecord(t0 + uint64_t(i / 10) * 1000 + 996, 'A', 45.5, 'N', 7.25,
                                       'E', i < 20 ? 0.1 : 5.0, i < 20 ? -1.0 : 90.0, 8.0));
    const auto take = [&](const char* sub, uint64_t firstFrame) {
        std::filesystem::create_directories(dir / sub);
        const Take t = writeTake(dir / sub);
        CHECK(writeLens(t.lead, 64, 50));
        appendTo(t.lead, oneX2Trailer(false, 0, gps.b, firstFrame));
        return t;
    };
    const Take lined = take("gps", t0 + 404);
    const auto clip = camera::ReadClipMetadata(lined.lead);
    CHECK(clip && clip->gpsFixes == 30);
    const camera::ClipTelemetry tele = camera::ReadClipTelemetry(lined.lead);
    const auto& track = tele.gps;
    CHECK(track && track->fixes.size() == 3 && track->clockStamped); // nothing for the IMU to match: as stamped
    if (track && track->fixes.size() == 3) {
        CHECK(track->fixes[0].timeMs == 592 && track->fixes[2].timeMs == 2592);
        CHECK(track->fixes[0].latitude == 45.5 && track->fixes[0].longitude == 7.25);
        CHECK(std::isnan(track->fixes[0].headingDeg)); // -1: standing still
        CHECK(track->fixes[2].headingDeg == 90.0 && track->fixes[2].speedMps == 5.0);
    }
    const Take unaligned = take("gps_no98", 0);
    CHECK(camera::ReadClipMetadata(unaligned.lead));
    // The trailer's IMU spans 200 us, less than a step of motion (orbiscore_test
    // has BuildMotion on a ride).
    CHECK(tele.motion.empty());
    CHECK(!camera::ReadClipTelemetry(unaligned.lead).gps);
    const camera::ClipTelemetry missing =
        camera::ReadClipTelemetry((dir / "missing.insv").string());
    CHECK(!missing.gps && missing.motion.empty());
}

void testHighlights(const std::filesystem::path& dir)
{
    std::printf("  the highlight marks, as video time\n");
    // Field 24 is 4407 in the ONE X2's ms ticks; the take is 2 s (50 frames
    // at 25 fps). A mark before the first frame and one past the end are
    // left out.
    Bytes anchors;
    anchors.u8(1).u32(4).u64(4407 + 1500).u64(4000).u64(4407 + 500).u64(4407 + 60000);
    for (uint8_t t : {2, 3, 4, 16, 17, 18})
        anchors.u8(t).u32(0);
    std::filesystem::create_directories(dir / "marks");
    const Take t = writeTake(dir / "marks");
    CHECK(writeLens(t.lead, 64, 50));
    appendTo(t.lead, oneX2Trailer(false, 0, {}, 0, anchors.b));
    const auto clip = camera::ReadClipMetadata(t.lead);
    CHECK(clip && clip->highlightMs == std::vector<int64_t>({500, 1500}));
    // A take without the record has none.
    std::filesystem::create_directories(dir / "marks_none");
    const auto plain = camera::ReadClipMetadata(writeTake(dir / "marks_none").lead);
    CHECK(plain && plain->highlightMs.empty());
}

// The A1's trailer, cut down: the model, field 24 (`firstFrame`), the
// calibration, field 69, a little IMU and the flight controller's attitude:
// level, then the nose 26 deg down (fast forward flight).
constexpr double kA1NoseDeg = -26.0;

std::vector<uint8_t> a1Trailer(uint64_t firstFrame = 1000300)
{
    Bytes meta;
    meta.pbString(1, "A1000000000000000001").pbString(2, "Antigravity A1");
    meta.pbString(3, "10.0.397.3_5.5").pbVarint(24, firstFrame).pbDouble(25, 21.247);
    Bytes crop; // window_crop_info: src 5248², dst 5248²
    crop.pbVarint(1, 5248).pbVarint(2, 5248).pbVarint(3, 5248).pbVarint(4, 5248);
    meta.pbBytes(27, crop.b);
    meta.pbString(54, "2_1.5_3742.5_3741.8_2620.9_2617.6_-177.5_-89.95_90.4_0_0_0_"
                      "-0.056_0.204_-0.534_0.0008_0.00004_10496_5248_155_"
                      "1.5_3754.8_3755.1_7875.9_2606.2_-179.4_-89.9_89.4_-0.0001_0.00002_"
                      "-0.071_-0.050_0.170_-0.499_0.0001_0.00003_10496_5248_155_197632");
    for (float v : {0.5f, 0.5f, -0.5f, 0.5f}) // field 69
        meta.pbFloat(69, v);
    Bytes imu; // 56-byte doubles, 1 ms apart
    for (int i = 0; i < 100; ++i) {
        imu.u64(1000000 + uint64_t(i) * 1000);
        for (double v : {0.0, 1.0, 0.0, 0.0, 0.0, 0.0})
            imu.f64(v);
    }
    // The attitude, x, y, z, w (the nose turns about body y).
    const double nose = kA1NoseDeg * std::numbers::pi / 180.0;
    Bytes attitude;
    attitude.u64(1000000).f32(0).f32(0).f32(0).f32(1).f32(0).f32(0).f32(0);
    attitude.u64(1020000)
        .f32(0)
        .f32(float(std::sin(nose / 2)))
        .f32(0)
        .f32(float(std::cos(nose / 2)))
        .f32(15)
        .f32(0)
        .f32(0);
    return testtrailer::insvTrailer({{0x101, meta.b}, {0x300, imu.b}, {0x2500, attitude.b}}, true,
                                    256);
}

// An Antigravity A1 clip (docs/formats/antigravity-a1.md): both lenses as
// streams of one VID_<date>_<time>_<n>.insv, the trailer in 256 KiB slots
// with no record headers (cut down here), the proxy PRX_….prx with the
// clip's own name.
void testAntigravityA1(const std::filesystem::path& dir)
{
    std::printf("  Antigravity A1: one file, a proxy named without a lens token\n");
    const std::string vid = (dir / "VID_20261028_165044_011.insv").string();
    const std::string prx = (dir / "PRX_20261028_165044_011.prx").string();
    testmedia::VideoSpec spec;
    spec.width = spec.height = 64;
    spec.fps = kFps;
    spec.frames = 50;
    spec.streams = 2;
    CHECK(testmedia::writeVideo(vid + ".mp4", spec));
    std::filesystem::rename(vid + ".mp4", vid);
    appendTo(vid, a1Trailer());
    const double nose = kA1NoseDeg * std::numbers::pi / 180.0;
    CHECK(writeLens(prx, 32, 50));

    std::string err;
    const auto clip = camera::ReadClipMetadata(vid, &err);
    CHECK(clip);
    if (!clip) {
        std::printf("  %s\n", err.c_str());
        return;
    }
    CHECK(clip->maker == "Antigravity" && clip->model == "Antigravity A1");
    CHECK(clip->lenses.size() == 2);
    if (clip->lenses.size() == 2) {
        CHECK(clip->lenses[0].source.path == vid && clip->lenses[0].source.stream == 0);
        CHECK(clip->lenses[1].source.path == vid && clip->lenses[1].source.stream == 1);
    }
    CHECK(clip->calibrated && clip->lenses[0].calibration.focalRefW == 5248.0);
    // Its lenses look down and up: turned so the picture is level and faces
    // the drone's nose.
    CHECK(clip->mountYawDeg == -90.0 && clip->mountPitchDeg == 90.0 && clip->mountRollDeg == 0.0);
    CHECK(clip->proxyPath == prx);
    // Lens 0 is the proxy's left half, as on the ONE X2 (not the X5).
    CHECK(clip->parts[0].proxyLenses.size() == 2 &&
          clip->parts[0].proxyLenses[0].path == prx && clip->parts[0].proxyLenses[0].rect.x == 0.0 &&
          clip->parts[0].proxyLenses[1].rect.x == 0.5);
    CHECK(camera::ClipFileOf(vid) == vid);
    CHECK(camera::ClipFileOf(prx, &err) == vid);

    // Up and north from the attitude in the IMU's axes: body <- IMU is field
    // 69's permutation and the camera's 9.7 deg pitch up in the airframe, so
    // a body facing north pitched by p puts up at (0, cos(p + 9.7),
    // sin(p + 9.7)) and north at (0, -sin(p + 9.7), cos(p + 9.7)).
    const auto track = camera::ReadClipImu(vid);
    CHECK(track && track->orientation.size() == 2);
    if (track && track->orientation.size() == 2) {
        for (int k = 0; k < 2; ++k) {
            const double p = (k == 0 ? 0.0 : nose) + 9.7 * std::numbers::pi / 180.0;
            const orbiscore::OrientationSample& u = track->orientation[k];
            CHECK(u.timestamp == (k == 0 ? 1000000u : 1020000u));
            CHECK(std::fabs(u.up[0]) < 1e-6 && std::fabs(u.up[1] - std::cos(p)) < 1e-6 &&
                  std::fabs(u.up[2] - std::sin(p)) < 1e-6);
            CHECK(std::fabs(u.north[0]) < 1e-6 && std::fabs(u.north[1] + std::sin(p)) < 1e-6 &&
                  std::fabs(u.north[2] - std::cos(p)) < 1e-6);
        }
    }
}

// A renamed A1 take (docs/formats/antigravity-a1.md): the name each file
// holds gives the clip its recording time to the second (field 7 has
// minutes only) and leads it to its proxy; a proxy renamed alike is found by
// the stem, and both ways only when its trailer is the same take's.
void testA1Renamed(const std::filesystem::path& dir)
{
    std::printf("  Antigravity A1, renamed: the names the files hold, the proxy by its take\n");
    const auto sub = dir / "a1_renamed";
    std::filesystem::create_directories(sub);
    // A clip and a proxy named `vid` / `prx` on disk, `stamp` in the names
    // they hold; `proxyFirstFrame` is the proxy's field 24.
    const auto write = [&](const std::string& vid, const std::string& prx, const std::string& stamp,
                           uint64_t firstFrame, uint64_t proxyFirstFrame) {
        testmedia::VideoSpec spec;
        spec.width = spec.height = 64;
        spec.fps = kFps;
        spec.frames = 50;
        spec.streams = 2;
        CHECK(testmedia::writeVideo(vid + ".mp4", spec));
        std::filesystem::rename(vid + ".mp4", vid);
        CHECK(testtrailer::holdName(vid, "VID_" + stamp + ".insv"));
        appendTo(vid, a1Trailer(firstFrame));
        CHECK(writeLens(prx, 32, 50));
        CHECK(testtrailer::holdName(prx, "PRX_" + stamp + ".prx"));
        appendTo(prx, a1Trailer(proxyFirstFrame));
    };
    std::string err;

    // Both renamed alike, as the contributor's second set came.
    const std::string vid = (sub / "Video8K30.insv").string();
    const std::string prx = (sub / "Video8K30.prx").string();
    write(vid, prx, "20261029_170732_021", 659569622, 659569622);
    CHECK(orbiscore::ReadInsvHeldName(vid) == "VID_20261029_170732_021.insv");
    CHECK(orbiscore::ReadInsvHeldName(prx) == "PRX_20261029_170732_021.prx");
    const auto clip = camera::ReadClipMetadata(vid, &err);
    CHECK(clip && clip->recordedLocal == "2026-10-29T17:07:32");
    CHECK(clip && clip->proxyPath == prx);
    CHECK(camera::ClipFileOf(prx, &err) == vid);

    // Only the clip renamed: its proxy by the name the clip holds.
    const std::string flight = (sub / "flight.insv").string();
    const std::string kept = (sub / "PRX_20261029_170608_020.prx").string();
    write(flight, kept, "20261029_170608_020", 575986304, 575986304);
    const auto renamed = camera::ReadClipMetadata(flight, &err);
    CHECK(renamed && renamed->recordedLocal == "2026-10-29T17:06:08");
    CHECK(renamed && renamed->proxyPath == kept);

    // Another take's proxy under the clip's stem: not its proxy, and it
    // doesn't lead to the clip.
    const std::string other = (sub / "other.insv").string();
    const std::string stray = (sub / "other.prx").string();
    write(other, stray, "20261029_170123_013", 291816338, 320568766);
    const auto lone = camera::ReadClipMetadata(other, &err);
    CHECK(lone && lone->proxyPath.empty());
    CHECK(camera::ClipFileOf(stray, &err).empty());

    // A file that holds no name (FFmpeg's own MP4).
    CHECK(!orbiscore::ReadInsvHeldName((dir / "VID_20261028_165044_011.insv").string()));
}

} // namespace

// An X5 trailer, cut down, for one file of a split recording (§1, field 26):
// the model, field 24 (`firstFrame`, us), the group's `index` and first file
// `identify`, and IMU samples 1 ms apart over [imuFrom, imuTo) us whose gyro
// x is `gyroX` (so a test can tell the parts' samples apart).
// `timeMapOffsetMs` > 0: a copy Insta360 software re-wrote after a trim,
// its video starting that far into the original (record 128, TimeMap);
// `cutOutMs` > 0: made of two stretches, that much cut out after frame 5.
// `model`: the model field, another camera's in the X5's layout.
std::vector<uint8_t> x5PartTrailer(const std::string& serial, uint64_t firstFrame, uint64_t index,
                                   const std::string& identify, uint64_t imuFrom, uint64_t imuTo,
                                   double gyroX, double timeMapOffsetMs = 0.0,
                                   double cutOutMs = 0.0,
                                   const std::string& model = "Insta360 X5")
{
    Bytes meta;
    meta.pbString(1, serial).pbString(2, model).pbString(3, "v1.5.6_build1");
    meta.pbVarint(7, 20250927144221).pbVarint(24, firstFrame);
    Bytes group; // {1: type, 2: index, 3: identify, 4: total}
    group.pbVarint(1, 20).pbVarint(2, index).pbString(3, identify).pbVarint(4, 0);
    meta.pbBytes(26, group.b);
    Bytes imu;
    for (uint64_t t = imuFrom; t < imuTo; t += 1000) {
        imu.u64(t);
        for (double v : {0.0, 1.0, 0.0, gyroX, 0.0, 0.0})
            imu.f64(v);
    }
    if (timeMapOffsetMs <= 0.0)
        return testtrailer::insvTrailer({{0x101, meta.b}, {0x300, imu.b}}, true);
    // {u32 header size, trims, pair bytes, frames, 16 B zero}, a trim
    // {duration, source start, source end}, then (video ms, original ms).
    constexpr int kPairs = 10;
    Bytes map;
    const double frameMs = 1000.0 / kFps;
    map.u32(32).u32(cutOutMs > 0.0 ? 2 : 1).u32(16 * kPairs).u32(kPairs).u64(0).u64(0);
    if (cutOutMs > 0.0) {
        map.f64(6 * frameMs).f64(timeMapOffsetMs).f64(timeMapOffsetMs + 6 * frameMs);
        const double second = timeMapOffsetMs + 6 * frameMs + cutOutMs;
        map.f64((kPairs - 6) * frameMs).f64(second).f64(second + (kPairs - 6) * frameMs);
    } else {
        map.f64(kPairs * frameMs).f64(timeMapOffsetMs).f64(timeMapOffsetMs + kPairs * frameMs);
    }
    for (int i = 0; i < kPairs; ++i)
        map.f64(i * frameMs).f64(i * frameMs + timeMapOffsetMs + (i >= 6 ? cutOutMs : 0.0));
    return testtrailer::insvTrailer({{0x101, meta.b}, {0x300, imu.b}, {0x8000, map.b}}, true);
}

// Both lenses as streams of one file, `frames` frames whose grey tells the
// part (`grey0`) and the frame.
bool writeX5Part(const std::string& path, int frames, int grey0, int fps = kFps)
{
    testmedia::VideoSpec spec;
    spec.width = spec.height = 64;
    spec.fps = fps;
    spec.frames = frames;
    spec.streams = 2;
    spec.luma = [grey0](int frame, int, int) { return uint8_t(grey0 + 2 * frame); };
    if (!testmedia::writeVideo(path + ".mp4", spec))
        return false;
    std::filesystem::rename(path + ".mp4", path);
    return true;
}

// An X5 Interval photo's trailer, cut down (docs/formats/insta360-x5.md §3.6):
// records in the order the X5 writes them, chained (no offsets table), the
// preview before the metadata; in the metadata the model, field 24 (`taken`,
// us), field 26 (`type`, the series' first photo `identify`), field 27's
// whole 5376 window and the X5's calibration string (lens 1's centre in the
// right half); the IMU record one gravity vector, repeated, no gyro (in the
// 56-byte samples of the video tests; the X5 writes 20-byte ones).
// `guard`: field 68, the camera's lens-guard setting (0 bare, 5 fitted); -1
// leaves it out. `model`: the model field, another camera's in the X5's
// layout; `window` false leaves field 27 out.
std::vector<uint8_t> x5PhotoTrailer(const std::string& serial, uint64_t taken, uint64_t type,
                                    const std::string& identify, int guard = 5,
                                    const std::string& model = "Insta360 X5", bool window = true)
{
    Bytes meta;
    meta.pbString(1, serial).pbString(2, model).pbString(3, "v1.11.10_build1");
    meta.pbVarint(7, 20260815081108).pbVarint(24, taken);
    if (guard >= 0)
        meta.pbVarint(68, static_cast<uint64_t>(guard));
    Bytes group; // {1: type, 2: index, 3: identify, 4: total}
    group.pbVarint(1, type).pbVarint(2, 1).pbString(3, identify).pbVarint(4, 0);
    meta.pbBytes(26, group.b);
    if (window) {
        Bytes crop;
        crop.pbVarint(1, 5376).pbVarint(2, 5376).pbVarint(3, 5376).pbVarint(4, 5376);
        meta.pbBytes(27, crop.b);
    }
    meta.pbString(111,
                  "2_2.000000_4294.030_4293.860_2703.310_2684.030_0.241_0.068_89.993_0.000000_"
                  "0.000000_0.000000_0.22969893_1.60199785_-1.24233508_-3.16975212_0.00000000_"
                  "-0.00144594_0.00040147_-0.00395632_-0.00326463_0.00195414_0.00044626_"
                  "0.01116405_0.00953672_10752_5376_113_2.000000_4277.660_4277.270_8073.210_"
                  "2692.040_-0.088_-0.141_88.580_0.000774_0.000174_-0.031796_0.24695949_"
                  "1.25187707_0.26544470_-5.21495152_0.00000000_-0.00144929_-0.00059055_"
                  "0.00577204_0.01037207_-0.00314844_-0.00378038_-0.00842754_-0.02442369_10752_"
                  "5376_113_394240");
    // 99 samples, not the X5's 100: 56-byte samples in a record whose size
    // 20 doesn't divide too (ParseImu tells the layouts apart by it).
    Bytes imu;
    for (int i = 0; i < 99; ++i) {
        imu.u64(taken - 49000 + 1000 * i);
        for (double v : {-0.99, 0.12, -0.06, 0.0, 0.0, 0.0})
            imu.f64(v);
    }
    Bytes preview;
    for (int i = 0; i < 4096; ++i)
        preview.u8(0x80);
    return testtrailer::insvTrailer({{0x300, imu.b}, {0x101, meta.b}, {0x200, preview.b}}, false);
}

// A photo, both lenses side by side; its grey tells it apart. With a
// megabyte of Exif-like data ahead of the picture, as the X5's: FFmpeg's
// probe then sees only the photo's start, as on a real one.
bool writeX5Photo(const std::string& path, int grey, const std::vector<uint8_t>& trailer)
{
    return testmedia::writeJpeg(
        path, 256, 128, [grey](int x, int y) { return uint8_t(grey + ((x * 7 + y * 13) % 16) - 8); },
        trailer, 1100000);
}

// Interval series of X5 photos (§3.6): a photo opens its series from there
// on, a photo a frame at 30 a second, in the order they were taken; each
// lens its half; any photo of it names the first; another series beside it
// and a single photo are no part of it; the first gone, the earliest left.
void testIntervalPhotos(const std::filesystem::path& dir)
{
    std::printf("  X5 Interval photos: a series as one clip, a photo a frame\n");
    const auto sub = dir / "interval";
    std::filesystem::create_directories(sub);
    const std::string serial = "IAHEA25088AYDQ";
    const auto name = [](int n) {
        char b[64];
        std::snprintf(b, sizeof b, "IMG_20260815_0811%02d_00_%03d.insp", n, n);
        return std::string(b);
    };
    const std::string identify = "./DCIM/Camera01/" + name(3);
    // Six photos from _003, 10.3 s apart but one 11.5 s gap; their names
    // out of the order they were taken in for _005 and _006 (field 24 rules).
    const uint64_t takenUs[] = {115121549, 125405226, 146122259, 135779862, 156582634, 168082634};
    std::vector<std::string> photos;
    for (int k = 0; k < 6; ++k) {
        const std::string path = (sub / name(3 + k)).string();
        if (!writeX5Photo(path, 40 + 30 * k, x5PhotoTrailer(serial, takenUs[k], 5, identify))) {
            std::printf("  (no MJPEG encoder: skipped)\n");
            return;
        }
        photos.push_back(path);
    }
    // Another series (_001, _002) and a single photo (_020) beside them.
    const std::string other1 = (sub / name(1)).string(), other2 = (sub / name(2)).string();
    const std::string otherId = "./DCIM/Camera01/" + name(1);
    CHECK(writeX5Photo(other1, 200, x5PhotoTrailer(serial, 81712530, 5, otherId, 0)));
    CHECK(writeX5Photo(other2, 210, x5PhotoTrailer(serial, 92012198, 5, otherId, 0)));
    const std::string single = (sub / name(20)).string();
    CHECK(writeX5Photo(single, 220, x5PhotoTrailer(serial, 300000000, 0, "")));

    const auto notes = orbiscore::ReadInsvMakerNotes(photos[0]);
    CHECK(notes && notes->file_group_type == orbiscore::kInsvGroupIntervalPhotos &&
          notes->file_group_identify == identify && notes->first_frame_timestamp == takenUs[0]);
    std::string err;
    const auto clip = camera::ReadClipMetadata(photos[0], &err);
    CHECK(clip);
    if (!clip) {
        std::printf("  %s\n", err.c_str());
        return;
    }
    // In the order taken: _003 _004 _006 _005 _007 _008.
    const int order[] = {0, 1, 3, 2, 4, 5};
    CHECK(clip->photoCount == 6 && clip->parts.size() == 6);
    for (size_t i = 0; i < clip->parts.size() && i < 6; ++i) {
        const camera::Part& p = clip->parts[i];
        CHECK(p.path == photos[order[i]] && p.still);
        CHECK(p.startUs == std::llround(i * 1e6 / 30.0) &&
              p.startUs + p.durationUs == std::llround((i + 1) * 1e6 / 30.0));
        CHECK(p.lenses.size() == 2 && p.lenses[0].path == p.path && p.lenses[0].rect.x == 0.0 &&
              p.lenses[1].rect.x == 0.5 && p.lenses[0].rect.w == 0.5);
    }
    CHECK(clip->fps == 30.0 && clip->durationMs == 200 && clip->lensWidth == 128 &&
          clip->lensHeight == 128 && !clip->untestedModel);
    // The extended string's, with the X5's radial models exchanged
    // (InsvModel::radialModelsExchanged): lens 0 gets lens 1's focal length.
    CHECK(clip->calibrated && clip->lenses[0].calibration.focalRefW == 5376 &&
          std::abs(clip->lenses[0].calibration.fx - 4277.66) < 1e-6 &&
          std::abs(clip->lenses[1].calibration.fx - 4294.03) < 1e-6);
    // The model's lens correction for photos (InsvModel::photoLensCorrection),
    // where its videos have none.
    CHECK(clip->lensCorrection == *orbiscore::FindInsvModel("Insta360 X5").photoLensCorrection &&
          !(clip->lensCorrection == orbiscore::SeamCorrection{}));
    CHECK(std::abs(clip->timelapseIntervalSec - 10.374636) < 1e-6 && // the median gap
          std::abs(clip->captureScale - 30 * clip->timelapseIntervalSec) < 1e-9);
    CHECK(clip->recordedLocal == "2026-08-15T08:11:08" && clip->missingFirst.empty());
    // No lens-guard table in a photo: the X5's own (InsvModel::lensProfiles).
    CHECK(clip->bareLens && clip->premiumGuard && clip->standardGuard &&
          clip->premiumGuard->name == "ProtectorA" && clip->premiumGuard->fov_deg == 195 &&
          clip->premiumGuard->poly[1] == 0.032190650500000001 && clip->bareLens->fov_deg == 200);
    // The camera's lens-guard setting (field 68): fitted on the series, bare
    // on the single photo, unknown without the field.
    CHECK(clip->guardFitted == true);
    {
        const auto bare = camera::ReadClipMetadata(other1, &err);
        CHECK(bare && bare->guardFitted == false);
        const std::string u1 = (sub / name(21)).string(), u2 = (sub / name(22)).string();
        const std::string uId = "./DCIM/Camera01/" + name(21);
        CHECK(writeX5Photo(u1, 230, x5PhotoTrailer(serial, 310000000, 5, uId, -1)));
        CHECK(writeX5Photo(u2, 240, x5PhotoTrailer(serial, 320000000, 5, uId, -1)));
        const auto none = camera::ReadClipMetadata(u1, &err);
        CHECK(none && !none->guardFitted.has_value());
        std::filesystem::remove(u1);
        std::filesystem::remove(u2);
    }
    // The pictures in that order, a photo a frame.
    {
        orbiscore::FisheyeDemux d;
        CHECK(d.open(clip->partSources(), err));
        CHECK(std::abs(d.lens(0).fps - 30.0) < 1e-9 && d.lens(0).durationUs == 200000);
        bool inOrder = true;
        int n = 0;
        const auto mean = [](const orbiscore::Image& img) {
            double sum = 0;
            for (size_t i = 1; i < img.rgb.size(); i += 3)
                sum += img.rgb[i];
            return sum / (img.rgb.size() / 3.0);
        };
        for (orbiscore::Image a, b; d.readNextPair(a, b, nullptr, err); ++n)
            inOrder = inOrder && n < 6 && std::abs(mean(a) - (40 + 30 * order[n])) < 3 &&
                      std::abs(mean(b) - (40 + 30 * order[n])) < 3;
        CHECK(err.empty() && inOrder && n == 6);
    }
    // Levelled by each photo's gravity, timed by when it was taken; no heading.
    CHECK(clip->hasMotion && clip->orientationPerFrame && !clip->heading);
    {
        const auto track = camera::ReadClipImu(photos[0]);
        CHECK(track && track->samples.empty() && track->orientation.size() == 6 &&
              track->axisMap == std::string("-z,-x,y"));
        if (track && track->frameTimes.size() == 6) {
            for (size_t i = 0; i < 6; ++i)
                CHECK(std::abs(track->frameTimes[i].videoSec - std::llround(i * 1e6 / 30.0) * 1e-6) <
                          1e-9 &&
                      std::abs(track->frameTimes[i].trackSec -
                               (takenUs[order[i]] - takenUs[0]) * 1e-6) < 1e-9);
            const orbiscore::OrientationSample& o = track->orientation[0];
            CHECK(o.up[0] == -0.99 && o.up[1] == 0.12 && o.up[2] == -0.06 && o.north[0] == 0.0 &&
                  o.north[1] == 0.0 && o.north[2] == 0.0);
        }
    }
    // Probed for the media pool, the photos follow on to the ms: no gap.
    {
        stitch::ClipProbe probe;
        CHECK(stitch::ProbeClip(photos[2], probe, err) && probe.clip_path == photos[0] &&
              probe.parts.size() == 6);
        bool joined = true;
        for (size_t i = 1; i < probe.parts.size(); ++i)
            joined = joined && probe.parts[i].start_ms ==
                                   probe.parts[i - 1].start_ms + probe.parts[i - 1].duration_ms;
        CHECK(joined && probe.parts.back().start_ms + probe.parts.back().duration_ms == 200);
    }
    // Any photo of it names the first; from a later one, the series from there.
    CHECK(camera::ClipFileOf(photos[4], &err) == photos[0]);
    CHECK(camera::ClipFileOf(photos[0], &err) == photos[0]);
    const auto fromThird = camera::ReadClipMetadata(photos[3], &err);
    CHECK(fromThird && fromThird->photoCount == 4 && fromThird->parts[0].path == photos[3]);
    // The other series is its own; a single photo doesn't open, and says why.
    const auto other = camera::ReadClipMetadata(other2, &err);
    CHECK(other && other->photoCount == 1);
    CHECK(camera::ClipFileOf(other2, &err) == other1);
    CHECK(!camera::ReadClipMetadata(single, &err) && err.find("single 360 photo") != std::string::npos);
    // The first photo gone: the series from the earliest left, which knows.
    std::filesystem::remove(photos[0]);
    const auto rest = camera::ReadClipMetadata(photos[1], &err);
    CHECK(rest && rest->photoCount == 5 && rest->missingFirst == name(3));
    CHECK(camera::ClipFileOf(photos[2], &err) == photos[1]);
}

// A camera without a row of its own (orbiscore::FindInsvModel's general
// one), in the X5's layout: its clips open, untested, with none of the X5's
// own corrections -- an Interval series without field 27 and a video.
void testUntestedModel(const std::filesystem::path& dir)
{
    std::printf("  an untested Insta360 model: general values, none of the X5's\n");
    const auto sub = dir / "untested";
    std::filesystem::create_directories(sub);
    const std::string serial = "IAXXX00000000", model = "Insta360 X9";
    const std::string first = (sub / "IMG_20261120_100000_00_001.insp").string();
    const std::string second = (sub / "IMG_20261120_100010_00_002.insp").string();
    const std::string identify = "./DCIM/Camera01/IMG_20261120_100000_00_001.insp";
    if (!writeX5Photo(first, 60, x5PhotoTrailer(serial, 10000000, 5, identify, 5, model, false))) {
        std::printf("  (no MJPEG encoder: skipped)\n");
        return;
    }
    CHECK(writeX5Photo(second, 90, x5PhotoTrailer(serial, 20000000, 5, identify, 5, model, false)));
    std::string err;
    const auto clip = camera::ReadClipMetadata(first, &err);
    CHECK(clip);
    if (!clip) {
        std::printf("  %s\n", err.c_str());
        return;
    }
    CHECK(clip->untestedModel && clip->model == model && clip->photoCount == 2);
    // The strings as they stand (no exchanged radial models), fx in half
    // the calibration's frame: no field 27, and no focal reference known.
    CHECK(clip->calibrated && clip->lenses[0].calibration.focalRefW == 5376 &&
          std::abs(clip->lenses[0].calibration.fx - 4294.03) < 1e-6 &&
          std::abs(clip->lenses[1].calibration.fx - 4277.66) < 1e-6);
    CHECK(clip->lensCorrection == orbiscore::SeamCorrection{});
    // No guard table in the file, none taken from the X5: a bare lens.
    CHECK(!clip->bareLens && !clip->premiumGuard && !clip->standardGuard);
    // Not levelled: the photos' gravity axes aren't known.
    CHECK(!clip->hasMotion && !camera::ReadClipImu(first));
    CHECK(std::abs(clip->timelapseIntervalSec - 10.0) < 1e-9);
    {
        camera::MediaInfo info;
        CHECK(camera::ProbeMediaInfo(first, info, err) && info.untestedModel &&
              info.cameraModel == model);
    }

    // A video, untested too.
    const std::string video = (sub / "VID_20261120_100100_00_003.insv").string();
    CHECK(writeX5Part(video, 10, 50));
    appendTo(video, x5PartTrailer(serial, 1000000, 0, "", 1000000, 1400000, 0.1, 0.0, 0.0, model));
    const auto v = camera::ReadClipMetadata(video, &err);
    CHECK(v && v->untestedModel && v->mayContinueIn.empty());
    std::filesystem::remove_all(sub);
}

// A recording the X5 split into `_019` (2 s) and `_020` (1.2 s), as the
// camera writes them: the same date and time in the names, the next
// counter, field 26 naming the first file, field 24 on one clock (part 2's
// first frame 2 s + 3 ms of drift after part 1's), the IMU overlapping a
// second at the join. Beside them a `_021` from another camera, which
// isn't part of it.
void testSplitRecording(const std::filesystem::path& dir)
{
    std::printf("  X5 split recording: one clip of two files, IMU and frames across the join\n");
    const auto sub = dir / "split";
    std::filesystem::create_directories(sub);
    const std::string p1 = (sub / "VID_20250927_144221_00_019.insv").string();
    const std::string p2 = (sub / "VID_20250927_144221_00_020.insv").string();
    const std::string other = (sub / "VID_20250927_144221_00_021.insv").string();
    const std::string identify = "/DCIM/Camera01/VID_20250927_144221_00_019.insv";
    constexpr uint64_t kFirst1 = 10000000, kFirst2 = kFirst1 + 2003000;
    CHECK(writeX5Part(p1, 50, 20));
    appendTo(p1, x5PartTrailer("IAHEA2500000001", kFirst1, 0, identify, kFirst1 - 500000,
                               kFirst1 + 2500000, 0.1));
    CHECK(writeX5Part(p2, 30, 140));
    appendTo(p2, x5PartTrailer("IAHEA2500000001", kFirst2, 2, identify, kFirst2 - 1000000,
                               kFirst2 + 1500000, 0.2));
    CHECK(writeX5Part(other, 30, 140));
    appendTo(other, x5PartTrailer("IAHEA2599999999", kFirst2 + 1300000, 2, identify,
                                  kFirst2 + 300000, kFirst2 + 2800000, 0.3));
    // A proxy beside the second part only.
    const std::string lrv2 = (sub / "LRV_20250927_144221_01_020.lrv").string();
    CHECK(writeLens(lrv2, 32, 30));

    std::string err;
    const auto clip = camera::ReadClipMetadata(p1, &err);
    CHECK(clip);
    if (!clip) {
        std::printf("  %s\n", err.c_str());
        return;
    }
    CHECK(clip->parts.size() == 2);
    if (clip->parts.size() != 2)
        return;
    CHECK(clip->parts[0].path == p1 && clip->parts[1].path == p2);
    CHECK(clip->parts[0].startUs == 0 && clip->parts[1].startUs == 2000000);
    CHECK(clip->parts[1].lenses.size() == 2 && clip->parts[1].lenses[1].path == p2 &&
          clip->parts[1].lenses[1].stream == 1);
    // The second part's proxy, lens 0 its right half; with none for the
    // first, the clip can't play from proxies.
    CHECK(clip->parts[0].proxyLenses.empty() && clip->proxySources().empty());
    CHECK(clip->parts[1].proxyLenses.size() == 2 && clip->parts[1].proxyLenses[0].path == lrv2 &&
          clip->parts[1].proxyLenses[0].rect.x == 0.5 && clip->parts[1].proxyLenses[1].rect.x == 0.0);
    CHECK(clip->durationMs == 3200);
    // Either part names the clip; the first part alone is still a clip.
    CHECK(camera::ClipFileOf(p2) == p1);
    CHECK(camera::ClipFileOf(p1) == p1);
    CHECK(camera::ClipFileOf(other) == other);
    const auto second = camera::ReadClipMetadata(p2);
    CHECK(second && second->parts.size() == 1 && second->durationMs == 1200);

    // The IMU: part 1's samples up to part 2's first, then part 2's; each
    // part's frames by its own first frame (no exposure record here: each
    // part's line from its start to its end).
    const auto imu = camera::ReadClipImu(p1);
    CHECK(imu);
    if (imu) {
        // Part 2's samples start 1 s before its first frame, 1.003 s after
        // part 1's.
        CHECK(imu->samples.size() == 1503 + 2500);
        bool rising = true;
        for (size_t i = 1; i < imu->samples.size(); ++i)
            rising = rising && imu->samples[i].timestamp > imu->samples[i - 1].timestamp;
        CHECK(rising);
        CHECK(imu->samples[1502].gyro[0] == 0.1 && imu->samples[1503].gyro[0] == 0.2);
        CHECK(imu->frameTimes.size() == 4);
        if (imu->frameTimes.size() == 4) {
            // Track time counts from the earliest sample, 0.5 s before part
            // 1's first frame; the X5 model's capture lag is -0.5 ms.
            const double lag = -0.5e-3;
            CHECK(std::fabs(imu->frameTimes[0].videoSec) < 1e-9 &&
                  std::fabs(imu->frameTimes[0].trackSec - (0.5 + lag)) < 1e-9);
            CHECK(std::fabs(imu->frameTimes[2].videoSec - 2.0) < 1e-9 &&
                  std::fabs(imu->frameTimes[2].trackSec - (2.503 + lag)) < 1e-9);
            CHECK(std::fabs(imu->frameTimes[3].videoSec - 3.2) < 1e-9 &&
                  std::fabs(imu->frameTimes[3].trackSec - (3.703 + lag)) < 1e-9);
        }
    }

    // The frames: read on through the join, and seeked to either side.
    orbiscore::FisheyeDemux demux;
    CHECK(demux.open(clip->partSources(), err));
    CHECK(demux.lens(0).durationMs == 3200);
    orbiscore::Image a, b;
    orbiscore::FramePts pts;
    int frames = 0;
    int64_t last = -1;
    bool ordered = true;
    int greyAtJoin = -1;
    while (demux.readNextPair(a, b, &pts, err)) {
        ordered = ordered && pts.ms > last;
        last = pts.ms;
        if (frames == 50)
            greyAtJoin = a.rgb[(32 * a.w + 32) * 3];
        ++frames;
    }
    CHECK(err.empty());
    CHECK(frames == 80 && ordered && last == 3160);
    // The test video's limited-range grey as the decode's RGB.
    const auto rgb = [](int luma) { return int(std::lround((luma - 16) * 255.0 / 219.0)); };
    CHECK(std::abs(greyAtJoin - rgb(140)) <= 3); // part 2's first frame
    const auto greyAt = [&](int64_t ms, int64_t& gotMs) {
        if (!demux.seekTo(ms, err) || !demux.readNextPair(a, b, &pts, err))
            return -1;
        gotMs = pts.ms;
        return int(a.rgb[(32 * a.w + 32) * 3]);
    };
    int64_t got = 0;
    CHECK(std::abs(greyAt(2000, got) - rgb(140)) <= 3 && got == 2000);
    CHECK(std::abs(greyAt(1960, got) - rgb(20 + 2 * 49)) <= 3 && got == 1960);
    CHECK(std::abs(greyAt(2440, got) - rgb(140 + 2 * 11)) <= 3 && got == 2440);
    CHECK(std::abs(greyAt(400, got) - rgb(20 + 2 * 10)) <= 3 && got == 400);
    // Keyframes on both sides of the join: part 2 starts with one.
    int64_t before = 0, after = 0;
    demux.keyframesAroundMs(1990, before, after);
    CHECK(before >= 0 && before < 2000 && after == 2000);
    demux.keyframesAroundMs(2000, before, after);
    CHECK(before == 2000);
}

// Parts that aren't here. A recording in three files whose middle one is
// gone: the third goes where the camera's clock puts it, leaving a gap. The
// third alone: the clip starts there, and says which file the recording
// starts in. A last part as long as the camera writes a file: it says
// which file the recording may go on in.
void testMissingParts(const std::filesystem::path& dir)
{
    std::printf("  X5 split recording with parts missing: a gap, the first, maybe a next\n");
    const auto sub = dir / "gap";
    std::filesystem::create_directories(sub);
    const std::string p1 = (sub / "VID_20250927_144221_00_019.insv").string();
    const std::string p3 = (sub / "VID_20250927_144221_00_021.insv").string();
    const std::string identify = "/DCIM/Camera01/VID_20250927_144221_00_019.insv";
    // Part 1 2 s, the missing part 2 1.2 s, each with 3 ms of drift.
    constexpr uint64_t kFirst1 = 10000000, kFirst3 = kFirst1 + 3206000;
    CHECK(writeX5Part(p1, 50, 20));
    appendTo(p1, x5PartTrailer("IAHEA2500000001", kFirst1, 0, identify, kFirst1 - 500000,
                               kFirst1 + 2500000, 0.1));
    CHECK(writeX5Part(p3, 25, 140));
    appendTo(p3, x5PartTrailer("IAHEA2500000001", kFirst3, 4, identify, kFirst3 - 1000000,
                               kFirst3 + 1500000, 0.3));

    const auto clip = camera::ReadClipMetadata(p1);
    CHECK(clip && clip->parts.size() == 2);
    if (clip && clip->parts.size() == 2) {
        CHECK(clip->parts[1].path == p3 && clip->parts[1].startUs == 3200000);
        CHECK(clip->durationMs == 4200 && clip->missingFirst.empty() &&
              clip->mayContinueIn.empty());
        // The frames jump over the gap.
        orbiscore::FisheyeDemux demux;
        std::string err;
        CHECK(demux.open(clip->partSources(), err));
        orbiscore::Image a, b;
        orbiscore::FramePts pts;
        int frames = 0;
        int64_t afterJoin = -1;
        while (demux.readNextPair(a, b, &pts, err))
            if (++frames == 51)
                afterJoin = pts.ms;
        CHECK(frames == 75 && afterJoin == 3200);
        // So do the IMU's frame times: part 3's by its own first frame.
        const auto imu = camera::ReadClipImu(p1);
        CHECK(imu && imu->frameTimes.size() == 4);
        if (imu && imu->frameTimes.size() == 4)
            CHECK(std::fabs(imu->frameTimes[2].videoSec - 3.2) < 1e-9 &&
                  std::fabs(imu->frameTimes[2].trackSec - imu->frameTimes[0].trackSec - 3.206) <
                      1e-9);
    }
    CHECK(camera::ClipFileOf(p3) == p1);

    // Without the first file, the last one is a clip of its own, from its start.
    std::filesystem::remove(p1);
    const auto alone = camera::ReadClipMetadata(p3);
    CHECK(alone && alone->parts.size() == 1 && alone->durationMs == 1000 &&
          alone->missingFirst == "VID_20250927_144221_00_019.insv");
    CHECK(camera::ClipFileOf(p3) == p3);

    // A file as long as the X5 writes (29 min 59 s; 1 fps keeps it small)
    // may go on in the next counter's.
    const std::string full = (sub / "VID_20250927_150000_00_099.insv").string();
    CHECK(writeX5Part(full, 1800, 20, 1));
    appendTo(full, x5PartTrailer("IAHEA2500000001", kFirst1, 0,
                                 "/DCIM/Camera01/VID_20250927_150000_00_099.insv", kFirst1,
                                 kFirst1 + 1000, 0.1));
    const auto longOne = camera::ReadClipMetadata(full);
    CHECK(longOne && longOne->mayContinueIn == "VID_20250927_150000_00_100.insv");
}

// A split recording beside the copies Insta360 software re-wrote after a
// trim (`-trim`; §1, record 128): each copy keeps field 24 and 26 of the
// file it was made from. `_019-trim` is `_019` from 1.2 s on (0.8 s long),
// `_020-trim` is `_020` cut 0.2 s short. The originals take each other,
// the copies each other, placed by where their video starts; a copy alone
// names the copy it starts in.
void testTrimmedCopies(const std::filesystem::path& dir)
{
    std::printf("  X5 split recording and its -trim copies in one folder\n");
    const auto sub = dir / "trim";
    std::filesystem::create_directories(sub);
    const auto at = [&](const char* name) { return (sub / name).string(); };
    const std::string p1 = at("VID_20250927_144221_00_019.insv");
    const std::string p2 = at("VID_20250927_144221_00_020.insv");
    const std::string t1 = at("VID_20250927_144221_00_019-trim.insv");
    const std::string t2 = at("VID_20250927_144221_00_020-trim.insv");
    const std::string identify = "/DCIM/Camera01/VID_20250927_144221_00_019.insv";
    constexpr uint64_t kFirst1 = 10000000, kFirst2 = kFirst1 + 2003000;
    CHECK(writeX5Part(p1, 50, 20));
    appendTo(p1, x5PartTrailer("IAHEA2500000001", kFirst1, 0, identify, kFirst1 - 500000,
                               kFirst1 + 2500000, 0.1));
    CHECK(writeX5Part(p2, 30, 140));
    appendTo(p2, x5PartTrailer("IAHEA2500000001", kFirst2, 2, identify, kFirst2 - 1000000,
                               kFirst2 + 1500000, 0.2));
    CHECK(writeX5Part(t1, 20, 68));
    appendTo(t1, x5PartTrailer("IAHEA2500000001", kFirst1, 0, identify, kFirst1 - 500000,
                               kFirst1 + 2500000, 0.1, 1200.0));
    CHECK(writeX5Part(t2, 25, 140));
    appendTo(t2, x5PartTrailer("IAHEA2500000001", kFirst2, 2, identify, kFirst2 - 1000000,
                               kFirst2 + 1500000, 0.2));

    const auto original = camera::ReadClipMetadata(p1);
    CHECK(original && original->parts.size() == 2);
    if (original && original->parts.size() == 2)
        CHECK(original->parts[1].path == p2 && original->durationMs == 3200);
    // Without the time map's 1.2 s, `_020-trim` would start 1.2 s after
    // `_019-trim` ends: a gap.
    const auto copy = camera::ReadClipMetadata(t1);
    CHECK(copy && copy->parts.size() == 2);
    if (copy && copy->parts.size() == 2) {
        CHECK(copy->parts[1].path == t2 && copy->parts[1].startUs == 800000);
        CHECK(copy->durationMs == 1800 && copy->mayContinueIn.empty());
        // Part 2's first frame 0.803 s after part 1's on the camera's clock.
        const auto imu = camera::ReadClipImu(t1);
        CHECK(imu && imu->frameTimes.size() == 4);
        if (imu && imu->frameTimes.size() == 4)
            CHECK(std::fabs(imu->frameTimes[2].videoSec - 0.8) < 1e-9 &&
                  std::fabs(imu->frameTimes[2].trackSec - imu->frameTimes[0].trackSec - 0.803) <
                      1e-6);
    }
    CHECK(camera::ClipFileOf(p2) == p1);
    CHECK(camera::ClipFileOf(t2) == t1);
    // A plain trim: lined up right, nothing to warn about.
    const auto map = orbiscore::ReadInsvTimeMap(t1);
    CHECK(map && map->cuts == 1 && map->offsetSpreadMs == 0.0 && map->offsetMs == 1200.0 &&
          map->plainTrim());
    CHECK(!orbiscore::ReadInsvTimeMap(p1));
    CHECK(original && !original->unevenTimeMap && copy && !copy->unevenTimeMap);

    // A copy with 3 s cut out of its middle: one offset can't line it all
    // up, which the clip says.
    const std::string cuts = (dir / "VID_20250927_150000_00_030-trim.insv").string();
    CHECK(writeX5Part(cuts, 10, 20));
    appendTo(cuts, x5PartTrailer("IAHEA2500000001", kFirst1, 0,
                                 "/DCIM/Camera01/VID_20250927_150000_00_030.insv", kFirst1 - 500000,
                                 kFirst1 + 5000000, 0.1, 400.0, 3000.0));
    const auto cutMap = orbiscore::ReadInsvTimeMap(cuts);
    CHECK(cutMap && cutMap->cuts == 2 && cutMap->offsetSpreadMs == 3000.0 &&
          !cutMap->plainTrim());
    const auto cutClip = camera::ReadClipMetadata(cuts);
    CHECK(cutClip && cutClip->unevenTimeMap);

    // A copy without its first copy: that's the one it names, not the
    // camera's file beside it.
    std::filesystem::remove(t1);
    const auto alone = camera::ReadClipMetadata(t2);
    CHECK(alone && alone->parts.size() == 1 &&
          alone->missingFirst == "VID_20250927_144221_00_019-trim.insv");
    CHECK(camera::ClipFileOf(t2) == t2);
}

// The DJI reader on the Avata 360 sample clip (docs/formats/dji-360.md,
// *Modes and sample clips*), with its .LRF and .SRT beside it.
void testDjiAvata360(const std::filesystem::path& osv, const std::filesystem::path& dir)
{
    std::string err;
    const auto clip = camera::ReadClipMetadata(osv.string(), &err);
    CHECK(clip);
    if (!clip) {
        std::printf("  %s\n", err.c_str());
        return;
    }
    CHECK(clip->maker == "DJI" && clip->model == "DJI Avata 360");
    CHECK(clip->firmware == "01.00.06.00" && !clip->serial.empty());
    CHECK(clip->calibrated && clip->lenses.size() == 2);
    CHECK(clip->lensWidth == 1920 && clip->lensHeight == 1920 && std::abs(clip->fps - 100) < 1e-6);
    CHECK(clip->captureMode == "4K");
    // The cover picture isn't a lens; the master lens is the first track.
    CHECK(clip->lenses[0].source.stream == 0 && clip->lenses[1].source.stream == 1);
    for (const camera::Lens& l : clip->lenses) {
        CHECK(l.model == orbiscore::LensModel::ThetaPoly && l.k5 != 0.0);
        CHECK(l.calibration.w == 7680 && l.calibration.focalRefW == 3840);
        CHECK(l.calibration.p[0] != 0.0 && l.calibration.p[1] != 0.0);
    }
    // The master looks along the rig's axis, the slave the other way.
    CHECK(clip->lenses[0].pose.mount[8] > 0.999 && clip->lenses[1].pose.mount[8] < -0.999);
    // The body behind the drone in both lenses' pictures, up at the top
    // (image angle 270 deg), and nowhere near the bottom.
    for (const camera::Lens& l : clip->lenses) {
        CHECK(l.mask.clearDeg.size() == 180);
        constexpr double deg = std::numbers::pi / 180.0;
        CHECK(l.mask.clearRadAt(270 * deg) < 95 * deg && l.mask.clearRadAt(90 * deg) > 1e6);
    }
    CHECK(clip->colorProfile == "D-Log M" && clip->logProfile == "D-Log M");
    CHECK(clip->recordedLocal == "2026-10-18T15:37:02");
    CHECK(std::filesystem::path(clip->proxyPath).extension() == ".LRF");
    // The proxy's lenses: the first lens track on the left, whichever lens
    // that is.
    CHECK(clip->parts.size() == 1 && clip->parts[0].proxyLenses.size() == 2);
    for (size_t i = 0; i < clip->parts[0].proxyLenses.size(); ++i) {
        const camera::LensSource& l = clip->parts[0].proxyLenses[i];
        CHECK(l.path == clip->proxyPath && l.stream == 0 &&
              l.rect.x == (clip->lenses[i].source.stream == 0 ? 0.0 : 0.5) && l.rect.w == 0.5);
    }
    // Media information knows it for a camera clip, though it isn't an .insv.
    camera::MediaInfo info;
    CHECK(camera::ProbeMediaInfo(osv.string(), info, err) && info.cameraClip &&
          info.proxyPath == clip->proxyPath);
    // The motion: the fused attitude alone, 4 kHz, every frame timed, the
    // readout from DJI's line time; level while the drone hovers at the start
    // (pitch -0.5, roll 1.9 deg by its own attitude).
    CHECK(clip->hasMotion && std::abs(clip->imuRateHz - 4000) < 20);
    CHECK(std::abs(clip->rollingShutterMs - 9.38) < 0.01);
    const auto imu = camera::ReadClipImu(osv.string());
    CHECK(imu && imu->samples.empty() && imu->axisMap == "y,x,-z" && imu->hasOffset);
    if (imu) {
        CHECK(imu->orientation.size() > 340000 && imu->frameTimes.size() == 8552);
        CHECK(std::abs(imu->readoutSec - 0.00938) < 1e-4);
        bool ordered = true;
        for (size_t i = 1; i < imu->orientation.size(); ++i)
            ordered = ordered && imu->orientation[i].timestamp > imu->orientation[i - 1].timestamp;
        CHECK(ordered);
        const double spanUs = static_cast<double>(imu->orientation.back().timestamp -
                                                  imu->orientation.front().timestamp);
        CHECK(std::abs(spanUs / static_cast<double>(imu->orientation.size() - 1) - 250.0) < 0.5);
        if (!imu->orientation.empty()) {
            const orbiscore::AxisMap am = orbiscore::ParseAxisMap(imu->axisMap);
            double up[3];
            am.apply(imu->orientation.front().up, up);
            CHECK(up[1] > std::cos(5.0 * std::numbers::pi / 180.0));
        }
        // Frame 0 sits half the readout after its stamp, on the track's clock.
        CHECK(std::abs(imu->offsetSec - imu->frameTimes.front().trackSec) < 1e-12);
        CHECK(imu->offsetSec > 0.0 && imu->offsetSec < 0.02);
    }
    CHECK(clip->gpsFixes > 500 && std::abs(clip->gpsRateHz - 10) < 1);

    const auto thumb = camera::ReadClipThumbnail(osv.string());
    CHECK(thumb && thumb->flat && thumb->width == 960 && thumb->height == 540);
    const camera::ClipTelemetry tele = camera::ReadClipTelemetry(osv.string());
    CHECK(tele.gps && tele.gps->fixes.size() == static_cast<size_t>(clip->gpsFixes));
    if (tele.gps && !tele.gps->fixes.empty())
        CHECK(tele.gps->fixes.front().timeMs >= 0 && tele.gps->fixes.front().timeMs < 200 &&
              tele.gps->fixes.back().timeMs > 80000);

    // The proxy names the clip.
    std::filesystem::path lrf = osv;
    CHECK(camera::ClipFileOf(lrf.replace_extension(".LRF").string()) == osv.string());

    // The clip on its own reads the same: its metadata comes from the clip,
    // never the proxy.
    const std::filesystem::path aloneDir = dir / "dji_alone";
    std::filesystem::create_directories(aloneDir);
    const std::filesystem::path alone = aloneDir / osv.filename();
    std::filesystem::create_symlink(std::filesystem::absolute(osv), alone);
    const auto clipAlone = camera::ReadClipMetadata(alone.string(), &err);
    CHECK(clipAlone && clipAlone->proxyPath.empty() && clipAlone->gpsFixes == clip->gpsFixes);
    const auto imuAlone = camera::ReadClipImu(alone.string());
    CHECK(imu && imuAlone && imuAlone->orientation.size() == imu->orientation.size() &&
          imuAlone->offsetSec == imu->offsetSec);
    const camera::ClipTelemetry teleAlone = camera::ReadClipTelemetry(alone.string());
    CHECK(teleAlone.gps && tele.gps && teleAlone.gps->fixes.size() == tele.gps->fixes.size());
    if (teleAlone.gps && tele.gps && teleAlone.gps->fixes.size() == tele.gps->fixes.size()) {
        bool same = true;
        for (size_t i = 0; i < tele.gps->fixes.size(); ++i)
            same = same && teleAlone.gps->fixes[i].timeMs == tele.gps->fixes[i].timeMs &&
                   teleAlone.gps->fixes[i].latitude == tele.gps->fixes[i].latitude;
        CHECK(same);
    }

    // A metadata track that can't be read to its end fails the clip and its
    // motion, rather than opening it as if it had less of both. Each under a
    // name of its own: a clip read is kept by its path.
    using ffmpegfaults::Call;
    const auto fresh = [&](const char* sub) {
        const std::filesystem::path d = dir / sub;
        std::filesystem::create_directories(d);
        std::filesystem::create_symlink(std::filesystem::absolute(osv), d / osv.filename());
        return (d / osv.filename()).string();
    };
    // The stream probe before the camera's reader allocates packets of its
    // own: the first allocation that fails past those is the reader's.
    bool noMemory = false;
    for (int after = 0; after < 20 && !noMemory; ++after) {
        const std::string path = fresh(("dji_no_memory_" + std::to_string(after)).c_str());
        ffmpegfaults::reset();
        ffmpegfaults::arm(Call::PacketAlloc, after, 0, -1);
        err.clear();
        const bool read = camera::ReadClipMetadata(path, &err) != nullptr;
        noMemory = !read && err.find("out of memory reading the DJI metadata") != std::string::npos;
        if (read || err.find("demux open failed") == std::string::npos)
            break;
    }
    CHECK(noMemory);
    ffmpegfaults::reset();
    ffmpegfaults::arm(Call::PacketAlloc, 0, 0, -1);
    CHECK(!camera::ReadClipImu(osv.string()));
    const std::string readError = fresh("dji_read_error");
    ffmpegfaults::reset();
    ffmpegfaults::arm(Call::ReadFrame, 1000, AVERROR(EIO), -1);
    err.clear();
    CHECK(!camera::ReadClipMetadata(readError, &err) &&
          err.find("the DJI metadata can't be read") != std::string::npos);
    ffmpegfaults::reset();
    ffmpegfaults::arm(Call::ReadFrame, 1000, AVERROR(EIO), -1);
    CHECK(!camera::ReadClipImu(osv.string()));
    ffmpegfaults::reset();
    CHECK(camera::ReadClipImu(osv.string()));
}

int main(int argc, char** argv)
{
    const auto dir = std::filesystem::temp_directory_path() / "orbis_camera_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const Take t = writeTake(dir);
    testPair(t);
    testRecordingTime(t, dir);
    testSecondLensChecked(t);
    testClipFileOf(t);
    testStitchedThumbnail(t);
    testFailedSeek(t);
    testExactFrameTimes(dir);
    testGps(dir);
    testHighlights(dir);
    testAntigravityA1(dir);
    testA1Renamed(dir);
    testSplitRecording(dir);
    testTrimmedCopies(dir);
    testMissingParts(dir);
    testIntervalPhotos(dir);
    testUntestedModel(dir);
    if (argc > 1) {
        // Found by name: the sample folder is sorted into sub-folders by
        // whoever keeps it, and the test shouldn't stop running when it is.
        std::filesystem::path dji;
        const std::filesystem::path root = std::filesystem::path(argv[1]) / "dji-avata-360";
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end;
             it.increment(ec))
            if (it->path().filename() == "DJI_20261018153702_0001_D_2026.OSV") {
                dji = it->path();
                break;
            }
        if (!dji.empty())
            testDjiAvata360(dji, dir);
        else
            std::printf("  (DJI Avata 360 sample clip not here: skipped)\n");
    }
    if (!g_failures)
        std::filesystem::remove_all(dir);
    std::printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures;
}
