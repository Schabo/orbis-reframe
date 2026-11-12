// camera::ReadClipMetadata on a ONE X2-style take: a lens per file, written
// here as small videos named as the camera names them, the lens-0 file
// carrying a synthetic trailer. The second lens's file is found beside it and
// checked; the clip's metadata follows a change to either file; every file of
// the take names the clip. And an Antigravity A1 clip: both lenses in one
// file, a slotted trailer, the proxy named without a lens token; renamed,
// found again by the names its files hold. And an X5 recording the camera
// split into two files: one clip, its parts found from either file, its IMU
// joined, its frames read on and seeked across the join. Exit code is the
// failure count.

#include "ClipMetadata.h"
#include "InsvDemux.h"
#include "InsvTrailer.h"
#include "support/TestMedia.h"
#include "support/TestTrailer.h"

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
bool writeLens(const std::string& path, int size, int frames)
{
    testmedia::VideoSpec spec;
    spec.width = spec.height = size;
    spec.fps = kFps;
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
               uint64_t creationTime = 0)
{
    Take t{(dir / ("VID_" + stamp + "_00_001.insv")).string(),
           (dir / ("VID_" + stamp + "_10_001.insv")).string(),
           (dir / ("LRV_" + stamp + "_11_001.insv")).string()};
    CHECK(writeLens(t.lead, 64, 50));
    appendTo(t.lead, oneX2Trailer(false, creationTime));
    CHECK(writeLens(t.second, 64, 49));
    CHECK(writeLens(t.proxy, 32, 49));
    return t;
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
    // The trailer's IMU spans 200 us, less than a step of motion (insv_test
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
            const insv::OrientationSample& u = track->orientation[k];
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
    CHECK(insv::ReadInsvHeldName(vid) == "VID_20261029_170732_021.insv");
    CHECK(insv::ReadInsvHeldName(prx) == "PRX_20261029_170732_021.prx");
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
    CHECK(!insv::ReadInsvHeldName((dir / "VID_20261028_165044_011.insv").string()));
}

} // namespace

// An X5 trailer, cut down, for one file of a split recording (§1, field 26):
// the model, field 24 (`firstFrame`, us), the group's `index` and first file
// `identify`, and IMU samples 1 ms apart over [imuFrom, imuTo) us whose gyro
// x is `gyroX` (so a test can tell the parts' samples apart).
std::vector<uint8_t> x5PartTrailer(const std::string& serial, uint64_t firstFrame, uint64_t index,
                                   const std::string& identify, uint64_t imuFrom, uint64_t imuTo,
                                   double gyroX)
{
    Bytes meta;
    meta.pbString(1, serial).pbString(2, "Insta360 X5").pbString(3, "v1.5.6_build1");
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
    return testtrailer::insvTrailer({{0x101, meta.b}, {0x300, imu.b}}, true);
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
    insv::InsvDemux demux;
    CHECK(demux.open(clip->partSources(), err));
    CHECK(demux.lens(0).durationMs == 3200);
    insv::Image a, b;
    insv::FramePts pts;
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
        insv::InsvDemux demux;
        std::string err;
        CHECK(demux.open(clip->partSources(), err));
        insv::Image a, b;
        insv::FramePts pts;
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

int main()
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
    testGps(dir);
    testHighlights(dir);
    testAntigravityA1(dir);
    testA1Renamed(dir);
    testSplitRecording(dir);
    testMissingParts(dir);
    if (!g_failures)
        std::filesystem::remove_all(dir);
    std::printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures;
}
