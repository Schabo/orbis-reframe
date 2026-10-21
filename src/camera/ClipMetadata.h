#pragma once

#include <Calibration.h> // insv::LensCalibExt
#include <Imu.h>         // insv::ImuTrack
#include <InsvTrailer.h> // insv::LensProfile

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// One description of a 360 clip, whichever camera recorded it. Each camera
// family has a reader that fills it from the file (CameraReader.h); the rest
// of the app reads clips only through here, so what differs between cameras
// is data in this description, not a code path per camera.
//
// No Qt, no Vulkan. Needs FFmpeg for the video streams (part of `camera`).

namespace camera {

// How the camera stores the sphere.
enum class Projection {
    DualFisheye, // two opposite fisheye lenses, stitched here
};

// Where a lens's picture comes from: a file and its n-th video stream.
struct LensSource {
    std::string path;
    int stream = 0; // index among the file's video streams
};

struct Lens {
    LensSource source;
    // The lens model (Mei / MEI, insv::LensCalibExt); valid when
    // ClipMetadata::calibrated.
    insv::LensCalibExt calibration;
};

// The camera's own preview picture: an equirectangular frame, NV12 (a
// full-size Y plane, then interleaved U/V at half size), BT.601 limited range.
struct Thumbnail {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> nv12; // width * height * 3 / 2 bytes
};

struct ClipMetadata {
    std::string path;

    std::string maker;  // "Insta360"
    std::string model;  // "Insta360 X5", as the file names it
    std::string firmware;
    std::string serial;

    Projection projection = Projection::DualFisheye;
    std::vector<Lens> lenses; // front lens first
    bool calibrated = false;  // every lens has its calibration from the file

    // Every lens stream's picture: decoded size, rate, length.
    int lensWidth = 0;
    int lensHeight = 0;
    double fps = 0.0;
    int64_t durationMs = 0;
    // The camera's name for the resolution it was filmed at ("8K"), or "".
    std::string captureMode;

    // The colour profile, as the file names it ("standard", "I_Log"; "" when
    // it doesn't say), and the log profile by its maker's name ("I-Log"), ""
    // when it wasn't filmed in log. A log clip looks flat until the maker's
    // LUT for that profile is applied.
    std::string colorProfile;
    std::string logProfile;

    // The lens shape with nothing in front of it and with each lens guard
    // (accessory profiles: insv::ApplyLensProfile); empty when the file has
    // none.
    std::optional<insv::LensProfile> bareLens;
    std::optional<insv::LensProfile> standardGuard;
    std::optional<insv::LensProfile> premiumGuard;

    // The camera's low-res proxy next to the clip, "" if not found.
    std::string proxyPath;

    // What the file records besides the picture. Facts for the Media
    // information panel; the IMU track itself comes from ReadClipImu.
    bool hasImu = false;
    double imuRateHz = 0.0;
    int64_t exposureRecords = 0;
    double rollingShutterMs = 0.0;
    // GPS track: presence only, never positions -- the panel is what people
    // screenshot for bug reports.
    int64_t gpsRecords = 0;
    int64_t gpsFixes = 0;     // records with an acquired position
    double gpsRateHz = 0.0;   // position updates per second
    double gpsTrackSec = 0.0; // seconds of track, gaps left out
};

// Why ReadClipMetadata gave no clip.
enum class ReadFailure {
    Unreadable, // missing, not a video, or a camera reader failed on it
    NotCamera,  // a video, but no camera reader knows it
};

// The clip at `path`, read once and cached (until the file changes). Null
// with `err` (technical words) and `why` set when it can't be read or no
// camera reader knows it. Safe from any thread.
std::shared_ptr<const ClipMetadata> ReadClipMetadata(const std::string& path,
                                                     std::string* err = nullptr,
                                                     ReadFailure* why = nullptr);

// The clip's IMU, lined up with its video. Not cached: a long clip's track is
// tens of MB. nullopt when the clip has none (or can't be read).
std::optional<insv::ImuTrack> ReadClipImu(const std::string& path);

// The camera's own preview picture, nullopt when the file has none.
std::optional<Thumbnail> ReadClipThumbnail(const std::string& path);

} // namespace camera
