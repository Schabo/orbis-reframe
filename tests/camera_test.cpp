// camera::ReadClipMetadata on a ONE X2-style take: a lens per file, written
// here as small videos named as the camera names them, the lens-0 file
// carrying a synthetic trailer. The second lens's file is found beside it and
// checked; the clip's metadata follows a change to either file. Exit code is
// the failure count.

#include "ClipMetadata.h"
#include "support/TestMedia.h"
#include "support/TestTrailer.h"

#include <cstdio>
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
// and 24, and a little IMU stamped in ms, 2 ms apart (500 Hz).
std::vector<uint8_t> oneX2Trailer()
{
    Bytes meta;
    meta.pbString(1, "IXSE0000000001").pbString(2, "Insta360 ONE X2").pbString(3, "v1.0.92_build1");
    meta.pbVarint(24, 4407);
    meta.pbDouble(25, 36.94);
    Bytes crop;
    crop.pbVarint(1, 3040).pbVarint(2, 3040).pbVarint(3, 2880).pbVarint(4, 2880);
    meta.pbBytes(27, crop.b);
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
    return testtrailer::insvTrailer({{0x300, imu.b}, {0x101, meta.b}}, false);
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
// camera writes it), the proxy.
Take writeTake(const std::filesystem::path& dir)
{
    Take t{(dir / "VID_20250816_103807_00_001.insv").string(),
           (dir / "VID_20250816_103807_10_001.insv").string(),
           (dir / "LRV_20250816_103807_11_001.insv").string()};
    CHECK(writeLens(t.lead, 64, 50));
    appendTo(t.lead, oneX2Trailer());
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

} // namespace

int main()
{
    const auto dir = std::filesystem::temp_directory_path() / "orbis_camera_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const Take t = writeTake(dir);
    testPair(t);
    testSecondLensChecked(t);
    if (!g_failures)
        std::filesystem::remove_all(dir);
    std::printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures;
}
