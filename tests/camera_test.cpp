// camera::ReadClipMetadata on a ONE X2-style take: a lens per file, written
// here as small videos named as the camera names them, the lens-0 file
// carrying a synthetic trailer. The second lens's file is found beside it and
// checked; the clip's metadata follows a change to either file; every file of
// the take names the clip. And an Antigravity A1 clip: both lenses in one
// file, a slotted trailer, the proxy named without a lens token. Exit code is
// the failure count.

#include "ClipMetadata.h"
#include "support/TestMedia.h"
#include "support/TestTrailer.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
                                  uint64_t firstFrameUnixMs = 0)
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
    CHECK(clip->hasImu);
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
    // A bright half and a dark one. (Not the greys themselves: the stitch's
    // photometric seam compensation pulls the two lenses' brightness
    // together, as in playback.)
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
    const auto track = camera::ReadClipGps(lined.lead);
    CHECK(track && track->fixes.size() == 3);
    if (track && track->fixes.size() == 3) {
        CHECK(track->fixes[0].timeMs == 592 && track->fixes[2].timeMs == 2592);
        CHECK(track->fixes[0].latitude == 45.5 && track->fixes[0].longitude == 7.25);
        CHECK(std::isnan(track->fixes[0].headingDeg)); // -1: standing still
        CHECK(track->fixes[2].headingDeg == 90.0 && track->fixes[2].speedMps == 5.0);
    }
    const Take unaligned = take("gps_no98", 0);
    CHECK(camera::ReadClipMetadata(unaligned.lead));
    CHECK(!camera::ReadClipGps(unaligned.lead));
    CHECK(!camera::ReadClipGps((dir / "missing.insv").string()));
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
    Bytes meta;
    meta.pbString(1, "A1000000000000000001").pbString(2, "Antigravity A1");
    meta.pbString(3, "10.0.397.3_5.5").pbVarint(24, 1000300).pbDouble(25, 21.247);
    Bytes crop; // window_crop_info: src 5248², dst 5248²
    crop.pbVarint(1, 5248).pbVarint(2, 5248).pbVarint(3, 5248).pbVarint(4, 5248);
    meta.pbBytes(27, crop.b);
    meta.pbString(54, "2_1.5_3742.5_3741.8_2620.9_2617.6_-177.5_-89.95_90.4_0_0_0_"
                      "-0.056_0.204_-0.534_0.0008_0.00004_10496_5248_155_"
                      "1.5_3754.8_3755.1_7875.9_2606.2_-179.4_-89.9_89.4_-0.0001_0.00002_"
                      "-0.071_-0.050_0.170_-0.499_0.0001_0.00003_10496_5248_155_197632");
    appendTo(vid, testtrailer::insvTrailer({{0x101, meta.b}}, true, 256));
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
}

} // namespace

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
    testAntigravityA1(dir);
    if (!g_failures)
        std::filesystem::remove_all(dir);
    std::printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures;
}
