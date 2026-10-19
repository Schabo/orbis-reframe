#include "Insta360Reader.h"

#include <InsvImu.h>
#include <InsvModels.h>
#include <InsvTrailer.h>

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>

namespace camera {
namespace {

// Enough IMU samples for a rate estimate without reading a long clip's whole
// 0x300 record (tens of MB per hour).
constexpr std::size_t kImuProbeSamples = 4000;

// VID_<date>_<time>_00_<n>.insv -> LRV_<date>_<time>_01_<n>.lrv, as the X5
// names them on the card (docs/formats/insta360-x5.md §1).
std::string findProxy(const std::filesystem::path& insv) {
    std::string name = insv.filename().string();
    if (name.rfind("VID_", 0) != 0)
        return "";
    const auto lensPos = name.find("_00_");
    if (lensPos == std::string::npos)
        return "";
    name.replace(0, 3, "LRV");
    name.replace(lensPos, 4, "_01_");
    std::filesystem::path lrv = insv.parent_path() / name;
    lrv.replace_extension(".lrv");
    std::error_code ec;
    return std::filesystem::exists(lrv, ec) ? lrv.string() : "";
}

std::optional<insv::LensProfile> profile(const insv::InsvMakerNotes& notes, const char* name) {
    if (!name)
        return std::nullopt;
    if (const insv::LensProfile* p = insv::FindLensProfile(notes, name))
        return *p;
    return std::nullopt;
}

} // namespace

CameraReader::Result Insta360Reader::read(const std::vector<insv::InsvStreamInfo>& streams,
                                          ClipMetadata& out, std::string& err) const {
    const auto notes = insv::ReadInsvMakerNotes(out.path);
    if (!notes)
        return Result::NotMine;
    out.maker = "Insta360";
    out.model = notes->model;
    out.firmware = notes->firmware;
    out.serial = notes->serial_number;
    bool known = false;
    const insv::InsvModel& model = insv::FindInsvModel(notes->model, &known);
    if (!known)
        std::fprintf(stderr, "[camera] no table entry for \"%s\"; using the %s's\n",
                     notes->model.c_str(), model.name);

    if (streams.size() < 2) {
        err = "expected 2 fisheye video streams, found " + std::to_string(streams.size());
        return Result::Failed;
    }
    out.projection = Projection::DualFisheye;
    out.lenses.resize(2);
    for (int i = 0; i < 2; ++i)
        out.lenses[i].source = LensSource{out.path, i};
    // The extended MEI calibration (what Insta360 Studio stitches with) --
    // every X5 clip carries it.
    insv::InsvCalibration calib;
    if (const auto ext = insv::ReadInsvExtendedCalibrationString(out.path);
        ext && insv::ParseExtendedCalibrationString(*ext, calib) && calib.lensesExt.size() == 2) {
        // fx / fy are given in field 27's sensor window.
        double focalRefW = notes->crop_dst_w;
        if (!(focalRefW > 0)) {
            focalRefW = model.focalRefW;
            std::fprintf(stderr, "[camera] no window_crop_info (field 27) in %s; taking the %s's "
                                 "focal reference, %g\n",
                         out.path.c_str(), model.name, focalRefW);
        }
        out.calibrated = true;
        for (int i = 0; i < 2; ++i) {
            out.lenses[i].calibration = calib.lensesExt[i];
            out.lenses[i].calibration.focalRefW = focalRefW;
        }
    }
    if (streams[0].width == streams[0].height)
        out.captureMode = model.captureMode(streams[0].width);

    out.bareLens = profile(*notes, "bare");
    out.standardGuard = profile(*notes, model.standardGuard);
    out.premiumGuard = profile(*notes, model.premiumGuard);
    out.proxyPath = findProxy(out.path);

    if (const auto tele = insv::ReadInsvTelemetry(out.path, kImuProbeSamples)) {
        if (tele->imu.size() >= 2) {
            out.hasImu = true;
            // Raw IMU timestamps are ~microseconds (docs/formats/insta360-x5.md §6.2).
            const double spanSec =
                (tele->imu.back().timestamp - tele->imu.front().timestamp) / 1e6;
            if (spanSec > 0)
                out.imuRateHz = (tele->imu.size() - 1) / spanSec;
        }
        out.exposureRecords = static_cast<int64_t>(tele->exposure.size());
        out.rollingShutterMs = tele->rolling_shutter_ms;
        // Field 22 (§3.3): the stream itself is tagged the same either way.
        out.colorProfile = tele->gamma_mode;
        out.logProfile = tele->gamma_mode == "I_Log" ? "I-Log" : "";
        out.gpsRecords = static_cast<int64_t>(tele->gps_records);
        out.gpsFixes = static_cast<int64_t>(tele->gps_fixes);
        out.gpsRateHz = tele->gps_rate_hz;
        out.gpsTrackSec = tele->gps_track_s;
    }
    return Result::Read;
}

std::optional<insv::ImuTrack> Insta360Reader::readImu(const ClipMetadata& clip) const {
    auto tele = insv::ReadInsvTelemetry(clip.path);
    if (!tele || tele->imu.size() < 2)
        return std::nullopt;
    return insv::MakeInsvImuTrack(std::move(*tele), clip.fps,
                                  insv::FindInsvModel(clip.model).imuAxes);
}

std::optional<Thumbnail> Insta360Reader::readThumbnail(const std::string& path) const {
    auto thumb = insv::ReadInsvThumbnail(path);
    if (!thumb)
        return std::nullopt;
    return Thumbnail{thumb->width, thumb->height, std::move(thumb->nv12)};
}

} // namespace camera
