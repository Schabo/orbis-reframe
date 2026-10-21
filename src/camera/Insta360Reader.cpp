#include "Insta360Reader.h"

#include <Calibration.h>
#include <InsvImu.h>
#include <InsvModels.h>
#include <InsvTrailer.h>
#include <LensDemuxer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>

namespace camera {
namespace {

// Enough IMU samples for a rate estimate without reading a long clip's whole
// 0x300 record (tens of MB per hour).
constexpr std::size_t kImuProbeSamples = 4000;

// `insv`'s name with its `_00_` lens token replaced by `token` and, if
// given, its prefix (`VID`) by `prefix` and its extension by `ext`; "" when
// the name isn't a camera's `VID_…_00_…` (docs/formats/insta360-x5.md §1).
std::filesystem::path sibling(const std::filesystem::path& insv, const char* token,
                              const char* prefix = nullptr, const char* ext = nullptr) {
    std::string name = insv.filename().string();
    if (name.rfind("VID_", 0) != 0)
        return {};
    const auto lensPos = name.find("_00_");
    if (lensPos == std::string::npos)
        return {};
    name.replace(lensPos, 4, token);
    if (prefix)
        name.replace(0, 3, prefix);
    std::filesystem::path p = insv.parent_path() / name;
    if (ext)
        p.replace_extension(ext);
    return p;
}

// The camera's proxy beside the clip (the X5's LRV_…_01_….lrv, the ONE X2's
// LRV_…_11_….insv), "" if it isn't there.
std::string findProxy(const std::filesystem::path& insv, const insv::InsvModel& model) {
    const std::filesystem::path lrv = sibling(insv, model.proxyToken, "LRV", model.proxyExt);
    std::error_code ec;
    return !lrv.empty() && std::filesystem::exists(lrv, ec) ? lrv.string() : "";
}

// The second lens's file of a clip whose own file holds lens 0 only (the
// ONE X2 at 5.7K, docs/formats/insta360-one-x2.md §1). Found by its name
// beside the clip; taken only if it is a one-lens file of the same shape and
// length (it may end one frame early) without a trailer of its own -- a file
// with one is another clip's. False with `err` set otherwise; else `other`
// is its path and `info` its stream.
bool findSecondLens(const std::string& path, const insv::InsvStreamInfo& lens0,
                    const insv::InsvModel& model, std::string& other, insv::InsvStreamInfo& info,
                    std::string& err) {
    const std::filesystem::path p = sibling(path, model.secondLensToken);
    std::error_code ec;
    if (p.empty() || !std::filesystem::exists(p, ec)) {
        err = "the second lens's file is missing" +
              (p.empty() ? std::string(" (the clip's name doesn't say which)")
                         : " (" + p.filename().string() + ")");
        return false;
    }
    other = p.string();
    insv::LensDemuxer demux;
    std::string e;
    if (!demux.open(
            other, [](const auto*, const auto*, auto*, std::string&) { return true; }, e)) {
        err = "the second lens's file can't be read (" + e + "): " + other;
        return false;
    }
    const insv::InsvStreamInfo& s = demux.lens(0);
    info = s;
    const double frameMs = lens0.fps > 0 ? 1000.0 / lens0.fps : 0.0;
    if (demux.lensCount() != 1 || s.width != lens0.width || s.height != lens0.height ||
        std::abs(s.fps - lens0.fps) > 1e-3 ||
        std::abs(static_cast<double>(s.durationMs - lens0.durationMs)) > frameMs + 1.0) {
        err = "the second lens's file doesn't match the clip (streams, size, rate or length): " +
              other;
        return false;
    }
    if (insv::ReadInsvMakerNotes(other)) {
        err = "the second lens's file is a clip of its own: " + other;
        return false;
    }
    return true;
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

    // Both lenses as streams of this file, or lens 0 here and lens 1 in a
    // file of its own beside it.
    out.projection = Projection::DualFisheye;
    out.lenses.resize(2);
    if (streams.size() >= 2) {
        for (int i = 0; i < 2; ++i)
            out.lenses[i].source = LensSource{out.path, i};
    } else if (streams.size() == 1 && model.secondLensToken) {
        std::string other;
        insv::InsvStreamInfo info;
        if (!findSecondLens(out.path, streams[0], model, other, info, err))
            return Result::Failed;
        out.lenses[0].source = LensSource{out.path, 0};
        out.lenses[1].source = LensSource{other, 0};
        // A pair plays as long as both have frames; the second may end one
        // frame early.
        out.durationMs = std::min(out.durationMs, info.durationMs);
    } else {
        err = "expected 2 fisheye video streams, found " + std::to_string(streams.size());
        return Result::Failed;
    }
    // The MEI calibration Insta360 Studio stitches with: the 56-token string
    // (every X5 clip carries it), else the shorter offset_v3 (field 54; the
    // ONE X2's only one).
    insv::InsvCalibration calib;
    const auto ext = insv::ReadInsvExtendedCalibrationString(out.path);
    if (((ext && insv::ParseExtendedCalibrationString(*ext, calib)) ||
         insv::ParseCalibrationV3String(notes->offset_v3, calib)) &&
        calib.lensesExt.size() == 2) {
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
    out.proxyPath = findProxy(out.path, model);

    if (const auto tele =
            insv::ReadInsvTelemetry(out.path, kImuProbeSamples, model.telemetryTickUs)) {
        if (tele->imu.size() >= 2) {
            out.hasImu = true;
            // Microseconds, whatever the model's tick (ReadInsvTelemetry).
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
    const insv::InsvModel& model = insv::FindInsvModel(clip.model);
    auto tele = insv::ReadInsvTelemetry(clip.path, 0, model.telemetryTickUs);
    if (!tele || tele->imu.size() < 2)
        return std::nullopt;
    return insv::MakeInsvImuTrack(std::move(*tele), clip.fps, model.imuAxes);
}

std::optional<Thumbnail> Insta360Reader::readThumbnail(const std::string& path) const {
    auto thumb = insv::ReadInsvThumbnail(path);
    if (!thumb)
        return std::nullopt;
    return Thumbnail{thumb->width, thumb->height, std::move(thumb->nv12)};
}

} // namespace camera
