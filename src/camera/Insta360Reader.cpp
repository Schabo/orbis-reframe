#include "Insta360Reader.h"

#include <Calibration.h>
#include <FisheyeProjection.h>
#include <InsvImu.h>
#include <InsvModels.h>
#include <InsvTrailer.h>
#include <LensDemuxer.h>
#include <StillPicture.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

namespace camera {
namespace {

// Enough IMU samples for a rate estimate without reading a long clip's whole
// 0x300 record (tens of MB per hour).
constexpr std::size_t kImuProbeSamples = 4000;

// `insv`'s name with its lens token (the model's `clipToken`, `_00_`)
// replaced by `token` and, if given, its prefix (`VID`) by `prefix` and its
// extension by `ext`; "" when the name isn't the model's `VID_…` clip name
// (docs/formats/insta360-x5.md §1). A model whose names carry no lens token
// takes no `token`: only the prefix and extension change.
std::filesystem::path sibling(const std::filesystem::path& insv, const insv::InsvModel& model,
                              const char* token, const char* prefix = nullptr,
                              const char* ext = nullptr) {
    std::string name = insv.filename().string();
    if (name.rfind("VID_", 0) != 0)
        return {};
    if (model.clipToken) {
        const auto lensPos = name.find(model.clipToken);
        if (!token || lensPos == std::string::npos)
            return {};
        name.replace(lensPos, std::strlen(model.clipToken), token);
    } else if (token) {
        return {};
    }
    if (prefix)
        name.replace(0, 3, prefix);
    std::filesystem::path p = insv.parent_path() / name;
    if (ext)
        p.replace_extension(ext);
    return p;
}

// The camera's proxy beside the clip (the X5's LRV_…_01_….lrv, the ONE X2's
// LRV_…_11_….insv, the A1's PRX_….prx), "" if it isn't there.
std::string findProxy(const std::filesystem::path& insv, const insv::InsvModel& model) {
    const std::filesystem::path lrv =
        sibling(insv, model, model.proxyToken, model.proxyPrefix, model.proxyExt);
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
    const std::filesystem::path p = sibling(path, model, model.secondLensToken);
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

// RGB24 -> NV12, BT.601 limited range (the X5's own preview's coding).
Thumbnail toNv12(const insv::Image& rgb) {
    Thumbnail t{rgb.w & ~1, rgb.h & ~1, false, {insv::YcbcrMatrix::Bt601, false}, {}};
    t.nv12.resize(static_cast<size_t>(t.width) * t.height * 3 / 2);
    uint8_t* y = t.nv12.data();
    uint8_t* uv = y + static_cast<size_t>(t.width) * t.height;
    const auto px = [&](int x, int yy) { return &rgb.rgb[(static_cast<size_t>(yy) * rgb.w + x) * 3]; };
    for (int yy = 0; yy < t.height; ++yy)
        for (int x = 0; x < t.width; ++x) {
            const uint8_t* p = px(x, yy);
            y[static_cast<size_t>(yy) * t.width + x] =
                static_cast<uint8_t>(((66 * p[0] + 129 * p[1] + 25 * p[2] + 128) >> 8) + 16);
        }
    for (int yy = 0; yy < t.height; yy += 2)
        for (int x = 0; x < t.width; x += 2) {
            int r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const uint8_t* p = px(x + dx, yy + dy);
                    r += p[0];
                    g += p[1];
                    b += p[2];
                }
            r = (r + 2) / 4;
            g = (g + 2) / 4;
            b = (b + 2) / 4;
            uint8_t* o = uv + static_cast<size_t>(yy / 2) * t.width + x;
            o[0] = static_cast<uint8_t>(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            o[1] = static_cast<uint8_t>(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    return t;
}

// A preview for a file whose trailer has no stitched one but each lens's
// first frame as an H.264 picture (the ONE X2: records 0x200 and 0x500,
// docs/formats/insta360-one-x2.md §4, §5): stitched here, small, with the
// clip's own calibration and the bare lens, as the playback stitch does.
constexpr int kThumbLensSide = 720; // a lens picture, decoded down to this
constexpr int kThumbWidth = 1024;   // the equirect preview's width
std::optional<Thumbnail> stitchedThumbnail(const std::string& path) {
    const auto rec0 = insv::ReadInsvRecord(path, 0x200);
    const auto rec1 = rec0 ? insv::ReadInsvRecord(path, 0x500) : std::nullopt;
    if (!rec1)
        return std::nullopt;
    const auto clip = ReadClipMetadata(path);
    if (!clip || !clip->calibrated || clip->lenses.size() != 2)
        return std::nullopt;
    insv::Image lens[2];
    std::string err;
    if (!insv::DecodeStillPicture(*rec0, "h264", kThumbLensSide, lens[0], err) ||
        !insv::DecodeStillPicture(*rec1, "h264", kThumbLensSide, lens[1], err)) {
        std::fprintf(stderr, "[camera] no preview from the lens pictures in %s: %s\n",
                     path.c_str(), err.c_str());
        return std::nullopt;
    }
    if (lens[0].w != lens[1].w || lens[0].h != lens[1].h)
        return std::nullopt;
    insv::InsvCalibration calib;
    for (const Lens& l : clip->lenses)
        calib.lensesExt.push_back(l.calibration);
    insv::StitchParams params;
    insv::ApplyLensProfile(params, clip->bareLens ? &*clip->bareLens : nullptr, nullptr);
    params.outYawDeg = clip->mountYawDeg;
    params.outPitchDeg = clip->mountPitchDeg;
    params.outRollDeg = clip->mountRollDeg;
    insv::FisheyeStitcher stitcher;
    if (!stitcher.configure(calib, lens[0].w, lens[0].h, params))
        return std::nullopt;
    return toNv12(stitcher.renderEquirect(lens[0], lens[1], kThumbWidth));
}

// "YYYY-MM-DDThh:mm:ss" from the 14 digits YYYYMMDDhhmmss, "" unless they
// make a date and time.
std::string localTime(const std::string& digits) {
    if (digits.size() != 14 ||
        !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return {};
    const auto num = [&](int pos, int n) { return std::stoi(digits.substr(pos, n)); };
    const int month = num(4, 2), day = num(6, 2), hour = num(8, 2), minute = num(10, 2),
              second = num(12, 2);
    if (num(0, 4) < 2000 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
        minute > 59 || second > 59)
        return {};
    return digits.substr(0, 4) + '-' + digits.substr(4, 2) + '-' + digits.substr(6, 2) + 'T' +
           digits.substr(8, 2) + ':' + digits.substr(10, 2) + ':' + digits.substr(12, 2);
}

// The recording start in the file's name, `<prefix>_YYYYMMDD_hhmmss_…`
// (VID_, PRO_VID_; docs/formats/insta360-x5.md §1), "" when it isn't named so.
std::string nameTime(const std::filesystem::path& p) {
    const std::string name = p.filename().string();
    for (std::size_t i = name.find('_'); i != std::string::npos; i = name.find('_', i + 1)) {
        if (i + 17 <= name.size() && name[i + 9] == '_') {
            const std::string t = localTime(name.substr(i + 1, 8) + name.substr(i + 10, 6));
            if (!t.empty())
                return t;
        }
    }
    return {};
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
    bool known = false;
    const insv::InsvModel& model = insv::FindInsvModel(notes->model, &known);
    if (!known)
        std::fprintf(stderr, "[camera] no table entry for \"%s\"; using the %s's\n",
                     notes->model.c_str(), model.name);
    out.maker = model.maker;
    out.model = notes->model;
    out.firmware = notes->firmware;
    out.serial = notes->serial_number;

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
    out.mountYawDeg = model.mountDeg[0];
    out.mountPitchDeg = model.mountDeg[1];
    out.mountRollDeg = model.mountDeg[2];
    if (streams[0].width == streams[0].height)
        out.captureMode = model.captureMode(streams[0].width);

    out.bareLens = profile(*notes, "bare");
    out.standardGuard = profile(*notes, model.standardGuard);
    out.premiumGuard = profile(*notes, model.premiumGuard);
    out.proxyPath = findProxy(out.path, model);
    // By the camera's clock: field 7 where the model writes it, else the
    // file's name, which the camera gives the same time.
    out.recordedLocal = localTime(std::to_string(notes->creation_time));
    if (out.recordedLocal.empty()) {
        out.recordedLocal = nameTime(out.path);
        std::fprintf(stderr, "[camera] no creation_time (field 7) in %s; recording time from "
                             "the file name: %s\n",
                     out.path.c_str(),
                     out.recordedLocal.empty() ? "none" : out.recordedLocal.c_str());
    }

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
    return insv::MakeInsvImuTrack(std::move(*tele), clip.fps, model);
}

std::optional<insv::GpsTrack> Insta360Reader::readGps(const ClipMetadata& clip) const {
    if (clip.gpsFixes == 0)
        return std::nullopt;
    const auto gps = insv::ReadInsvGps(clip.path);
    if (!gps || gps->fixes.empty())
        return std::nullopt;
    // The fixes carry Unix time; field 98.3 says when the first frame was
    // recorded on the same scale (docs/formats/insta360-x5.md §8). The file
    // gives no other anchor, so without it the track isn't used.
    if (gps->first_frame_unix_ms == 0) {
        std::fprintf(stderr, "[camera] GPS in %s but no first-frame time (field 98); not used\n",
                     clip.path.c_str());
        return std::nullopt;
    }
    insv::GpsTrack track;
    track.fixes.reserve(gps->fixes.size());
    // A heading of -1 is the camera's "don't know", written while standing
    // still (§8).
    for (const insv::InsvGpsFix& f : gps->fixes)
        track.fixes.push_back({f.unix_ms - gps->first_frame_unix_ms, f.latitude, f.longitude,
                               f.speed_mps, f.heading_deg < 0.0 ? std::nan("") : f.heading_deg,
                               f.altitude_m});
    return track;
}

std::optional<std::string> Insta360Reader::clipFileFor(const std::string& path) const {
    // VID_<date>_<time>_<lens>_<n>.insv -> the clip's VID_…_00_…, and
    // LRV_…_<token>_<n>.<ext> likewise, by each model's names; a proxy whose
    // name carries no lens token (the A1's PRX_….prx) -> VID_….insv. The
    // lens token is the last `_xx_` (the date and time are longer runs).
    const std::filesystem::path p(path);
    const std::string name = p.filename().string();
    const auto clipName = [&](const insv::InsvModel& m, const char* token) {
        std::string n = name;
        if (token)
            n.replace(n.rfind(token), std::strlen(token), m.clipToken);
        n.replace(0, 3, "VID");
        std::filesystem::path c = p.parent_path() / n;
        c.replace_extension(".insv");
        return c.string();
    };
    for (const insv::InsvModel& m : insv::InsvModels()) {
        if (m.secondLensToken && m.clipToken && name.rfind("VID_", 0) == 0 &&
            name.ends_with(".insv") && name.rfind(m.secondLensToken) != std::string::npos)
            return clipName(m, m.secondLensToken);
        if (name.rfind(std::string(m.proxyPrefix) + "_", 0) == 0 && p.extension() == m.proxyExt &&
            (m.proxyToken ? m.clipToken && name.rfind(m.proxyToken) != std::string::npos
                          : !m.clipToken))
            return clipName(m, m.proxyToken);
    }
    return std::nullopt;
}

std::optional<Thumbnail> Insta360Reader::readThumbnail(const std::string& path) const {
    auto thumb = insv::ReadInsvThumbnail(path);
    if (!thumb)
        return stitchedThumbnail(path);
    return Thumbnail{thumb->width, thumb->height, thumb->flat, thumb->coding,
                     std::move(thumb->nv12)};
}

} // namespace camera
