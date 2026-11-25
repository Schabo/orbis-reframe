#include "Insta360Reader.h"

#include "ReaderUtil.h"

#include <orbis-core/insta360/Calibration.h>
#include <orbis-core/stitch/FisheyeProjection.h>
#include <orbis-core/insta360/InsvImu.h>
#include <orbis-core/insta360/InsvModels.h>
#include <orbis-core/insta360/InsvTrailer.h>
#include <orbis-core/media/LensDemuxer.h>
#include <orbis-core/media/StillPicture.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

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
std::filesystem::path sibling(const std::filesystem::path& insv, const orbiscore::InsvModel& model,
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

// Whether the file at `p` is from the same take as the clip `clip` came
// from: the same camera and first-frame time (field 24), which a clip and
// its proxy share whatever their names.
// Video ms of a moment `camSec` seconds of the camera's clock after the
// first recorded frame. The camera's clock runs `captureScale` times the
// video's; in a timelapse the moment lies between two frames by their own
// times (`frameSec`, as orbiscore::InsvGps::frame_sec; they needn't be evenly
// spaced), past either end by the capture scale.
int64_t videoMsAt(double camSec, const ClipMetadata& clip, const std::vector<double>& frameSec) {
    if (clip.captureScale == 1.0)
        return std::llround(camSec * 1e3);
    const std::vector<double>& frames = frameSec;
    if (frames.size() < 2 || !(clip.fps > 0.0) || camSec <= frames.front())
        return std::llround((camSec - (frames.empty() ? 0.0 : frames.front())) * 1e3 /
                            clip.captureScale);
    const size_t last = frames.size() - 1;
    if (camSec >= frames.back())
        return std::llround((static_cast<double>(last) / clip.fps +
                             (camSec - frames.back()) / clip.captureScale) * 1e3);
    const size_t hi = static_cast<size_t>(
        std::upper_bound(frames.begin(), frames.end(), camSec) - frames.begin());
    const double f = (camSec - frames[hi - 1]) / (frames[hi] - frames[hi - 1]);
    return std::llround((static_cast<double>(hi - 1) + f) / clip.fps * 1e3);
}

bool sameTake(const std::filesystem::path& p, const orbiscore::InsvMakerNotes& clip) {
    if (clip.first_frame_timestamp == 0)
        return false;
    const auto other = orbiscore::ReadInsvMakerNotes(p.string());
    return other && other->serial_number == clip.serial_number &&
           other->first_frame_timestamp == clip.first_frame_timestamp;
}

// The camera's proxy beside the clip (the X5's LRV_…_01_….lrv, the ONE X2's
// LRV_…_11_….insv, the A1's PRX_….prx), "" if it isn't there. Named from
// the clip's name; for a renamed clip from the name it holds (`heldName`,
// orbiscore::ReadInsvHeldName), or the clip's own stem with the proxy's
// extension (both renamed alike). Those two are taken only when the file is
// the same take's (sameTake).
std::string findProxy(const std::filesystem::path& insv, const orbiscore::InsvModel& model,
                      const orbiscore::InsvMakerNotes& notes,
                      const std::optional<std::string>& heldName) {
    std::error_code ec;
    const auto there = [&](const std::filesystem::path& p) {
        return !p.empty() && std::filesystem::exists(p, ec) &&
               !std::filesystem::equivalent(p, insv, ec);
    };
    const std::filesystem::path lrv =
        sibling(insv, model, model.proxyToken, model.proxyPrefix, model.proxyExt);
    if (there(lrv))
        return lrv.string();
    std::vector<std::filesystem::path> renamed;
    if (heldName)
        renamed.push_back(sibling(insv.parent_path() / *heldName, model, model.proxyToken,
                                  model.proxyPrefix, model.proxyExt));
    renamed.push_back(std::filesystem::path(insv).replace_extension(model.proxyExt));
    for (const std::filesystem::path& p : renamed)
        if (there(p) && sameTake(p, notes))
            return p.string();
    return {};
}

// The lenses in the proxy `proxy` ("" for none): one stream, both fisheyes
// side by side, lens 0 in the half the model says.
std::vector<LensSource> proxyLensesOf(const std::string& proxy, const orbiscore::InsvModel& model) {
    if (proxy.empty())
        return {};
    const double lens0 = model.proxyLens0Left ? 0.0 : 0.5;
    return {LensSource{proxy, 0, {lens0, 0.0, 0.5, 1.0}},
            LensSource{proxy, 0, {0.5 - lens0, 0.0, 0.5, 1.0}}};
}

// The second lens's file of a clip whose own file holds lens 0 only (the
// ONE X2 at 5.7K, docs/formats/insta360-one-x2.md §1). Found by its name
// beside the clip; taken only if it is a one-lens file of the same shape and
// length (it may end one frame early) without a trailer of its own -- a file
// with one is another clip's. False with `err` set otherwise; else `other`
// is its path and `info` its stream.
bool findSecondLens(const std::string& path, const orbiscore::InsvStreamInfo& lens0,
                    const orbiscore::InsvModel& model, std::string& other,
                    orbiscore::InsvStreamInfo& info, std::string& err) {
    const std::filesystem::path p = sibling(path, model, model.secondLensToken);
    std::error_code ec;
    if (p.empty() || !std::filesystem::exists(p, ec)) {
        err = "the second lens's file is missing" +
              (p.empty() ? std::string(" (the clip's name doesn't say which)")
                         : " (" + p.filename().string() + ")");
        return false;
    }
    other = p.string();
    orbiscore::LensDemuxer demux;
    std::string e;
    if (!demux.open(
            other, [](const auto*, const auto*, auto*, std::string&) { return true; }, e)) {
        err = "the second lens's file can't be read (" + e + "): " + other;
        return false;
    }
    const orbiscore::InsvStreamInfo& s = demux.lens(0);
    info = s;
    const double frameMs = lens0.fps > 0 ? 1000.0 / lens0.fps : 0.0;
    if (demux.lensCount() != 1 || s.width != lens0.width || s.height != lens0.height ||
        std::abs(s.fps - lens0.fps) > 1e-3 ||
        std::abs(static_cast<double>(s.durationMs - lens0.durationMs)) > frameMs + 1.0) {
        err = "the second lens's file doesn't match the clip (streams, size, rate or length): " +
              other;
        return false;
    }
    if (orbiscore::ReadInsvMakerNotes(other)) {
        err = "the second lens's file is a clip of its own: " + other;
        return false;
    }
    return true;
}


// A preview for a file whose trailer has no stitched one but each lens's
// first frame as an H.264 picture (the ONE X2: records 0x200 and 0x500,
// docs/formats/insta360-one-x2.md §4, §5): stitched here, small, with the
// clip's own calibration and the bare lens, as the playback stitch does.
constexpr int kThumbLensSide = 720; // a lens picture, decoded down to this
constexpr int kThumbWidth = 1024;   // the equirect preview's width
std::optional<Thumbnail> stitchedThumbnail(const std::string& path) {
    const auto rec0 = orbiscore::ReadInsvRecord(path, 0x200);
    const auto rec1 = rec0 ? orbiscore::ReadInsvRecord(path, 0x500) : std::nullopt;
    if (!rec1)
        return std::nullopt;
    const auto clip = ReadClipMetadata(path);
    if (!clip || !clip->calibrated || clip->lenses.size() != 2)
        return std::nullopt;
    orbiscore::Image lens[2];
    std::string err;
    if (!orbiscore::DecodeStillPicture(*rec0, "h264", kThumbLensSide, lens[0], err) ||
        !orbiscore::DecodeStillPicture(*rec1, "h264", kThumbLensSide, lens[1], err)) {
        std::fprintf(stderr, "[camera] no preview from the lens pictures in %s: %s\n",
                     path.c_str(), err.c_str());
        return std::nullopt;
    }
    if (lens[0].w != lens[1].w || lens[0].h != lens[1].h)
        return std::nullopt;
    orbiscore::CameraCalibration calib;
    for (const Lens& l : clip->lenses)
        calib.lensesExt.push_back(l.calibration);
    orbiscore::StitchParams params;
    orbiscore::ApplyLensProfile(params, clip->bareLens ? &*clip->bareLens : nullptr, nullptr);
    for (int i = 0; i < 2; ++i)
        params.lensPose[i] = clip->lenses[i].pose;
    params.outYawDeg = clip->mountYawDeg;
    params.outPitchDeg = clip->mountPitchDeg;
    params.outRollDeg = clip->mountRollDeg;
    orbiscore::FisheyeStitcher stitcher;
    if (!stitcher.configure(calib, lens[0].w, lens[0].h, params))
        return std::nullopt;
    return ThumbnailFromRgb(stitcher.renderEquirect(lens[0], lens[1], kThumbWidth));
}


// The recording start in the file's name, `<prefix>_YYYYMMDD_hhmmss_…`
// (VID_, PRO_VID_; docs/formats/insta360-x5.md §1), "" when it isn't named so.
std::string nameTime(const std::filesystem::path& p) {
    const std::string name = p.filename().string();
    for (std::size_t i = name.find('_'); i != std::string::npos; i = name.find('_', i + 1)) {
        if (i + 17 <= name.size() && name[i + 9] == '_') {
            const std::string t = LocalTimeFromDigits(name.substr(i + 1, 8) + name.substr(i + 10, 6));
            if (!t.empty())
                return t;
        }
    }
    return {};
}

// The lenses of the clip file `path` whose video streams are `streams`:
// both as streams of it, or lens 0 there and lens 1 in a file of its own
// beside it (findSecondLens), whose length then caps the pair's (`durationUs`,
// `durationMs`). False with `err` set when neither fits.
bool lensSourcesOf(const std::string& path, const std::vector<orbiscore::InsvStreamInfo>& streams,
                   const orbiscore::InsvModel& model, std::vector<LensSource>& lenses,
                   int64_t& durationUs, int64_t& durationMs, std::string& err) {
    durationUs = streams.empty() ? 0 : streams[0].durationUs;
    durationMs = streams.empty() ? 0 : streams[0].durationMs;
    if (streams.size() >= 2) {
        lenses = {LensSource{path, 0}, LensSource{path, 1}};
        return true;
    }
    if (streams.size() == 1 && model.secondLensToken) {
        std::string other;
        orbiscore::InsvStreamInfo info;
        if (!findSecondLens(path, streams[0], model, other, info, err))
            return false;
        lenses = {LensSource{path, 0}, LensSource{other, 0}};
        // A pair plays as long as both have frames; the second may end one
        // frame early.
        durationUs = std::min(durationUs, info.durationUs);
        durationMs = std::min(durationMs, info.durationMs);
        return true;
    }
    err = "expected 2 fisheye video streams, found " + std::to_string(streams.size());
    return false;
}

// The video streams of `path`; false with `err` set when it can't be opened.
bool videoStreams(const std::string& path, std::vector<orbiscore::InsvStreamInfo>& streams,
                  std::string& err) {
    orbiscore::LensDemuxer demux;
    if (!demux.open(
            path, [](const auto*, const auto*, auto*, std::string&) { return true; }, err))
        return false;
    streams.clear();
    for (int i = 0; i < demux.lensCount(); ++i)
        streams.push_back(demux.lens(i));
    return true;
}

// A camera file's name around its counter: "VID_20250927_144221_00_", "019"
// and what follows, "-trim.insv" on a copy Insta360 software re-wrote after
// a trim, ".insv" as the camera wrote it. Nothing when no digits follow the
// last '_'.
struct CounterName {
    std::string stem;
    std::string digits;
    std::string rest;
};
std::optional<CounterName> splitCounter(const std::string& name) {
    const auto under = name.rfind('_');
    if (under == std::string::npos)
        return std::nullopt;
    size_t end = under + 1;
    while (end < name.size() && name[end] >= '0' && name[end] <= '9')
        ++end;
    if (end == under + 1)
        return std::nullopt;
    return CounterName{name.substr(0, under + 1), name.substr(under + 1, end - under - 1),
                       name.substr(end)};
}

// The name of the file a recording starts in, as it would be beside `self`:
// field 26's file (`identify`, the camera's path) named the way `self` is
// past its counter -- a re-written copy's first part is a copy too. Field
// 26's name when either has no counter.
std::string firstFileName(const std::string& self, const std::string& identify) {
    const std::string first = std::filesystem::path(identify).filename().string();
    const auto own = splitCounter(std::filesystem::path(self).filename().string());
    const auto camera = splitCounter(first);
    if (!own || !camera)
        return first;
    return camera->stem + camera->digits + own->rest;
}

// The files that continue the recording `out`'s file starts or continues
// (docs/formats/insta360-x5.md §1, §5.1 field 26), as `out.parts` after its
// own: beside it, named like it but for the counter (a copy Insta360
// software re-wrote takes its other copies, not the camera's files), from
// the same camera,
// naming the same first file and further on by field 26's index, in that
// order, each starting where the one before ends by the camera's clock --
// or later, a part between them missing, which leaves a gap. A re-written
// copy's video starts its time map's offset after field 24's frame. The first that
// doesn't follow on ends the clip there: streams not like the first part's,
// or starting before the part before ends. Then, when the last part is as
// long as the camera writes a file, the name the next would have
// (`mayContinueIn`).
void findParts(ClipMetadata& out, const orbiscore::InsvModel& model,
               const orbiscore::InsvMakerNotes& notes,
               const orbiscore::InsvStreamInfo& s0) {
    if (notes.file_group_identify.empty())
        return;
    namespace fs = std::filesystem;
    const fs::path self(out.path);
    const std::string name = self.filename().string();
    const auto counter = splitCounter(name);
    if (name.rfind("VID_", 0) != 0 || !counter)
        return;
    struct Candidate {
        uint64_t index = 0;
        std::string path;
        uint64_t firstFrame = 0;
    };
    std::vector<Candidate> candidates;
    std::error_code ec;
    const fs::path dir = self.has_parent_path() ? self.parent_path() : fs::path(".");
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        const auto other = n == name ? std::nullopt : splitCounter(n);
        if (!other || other->stem != counter->stem || other->rest != counter->rest)
            continue;
        const auto theirs = orbiscore::ReadInsvMakerNotes(e.path().string());
        if (!theirs || theirs->serial_number != notes.serial_number ||
            theirs->file_group_identify != notes.file_group_identify ||
            theirs->file_group_index <= notes.file_group_index)
            continue;
        candidates.push_back({theirs->file_group_index, e.path().string(),
                              theirs->first_frame_timestamp});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.index < b.index; });

    uint64_t prevFirstFrame = notes.first_frame_timestamp;
    const auto ownMap = orbiscore::ReadInsvTimeMap(out.path);
    double prevOffsetMs = ownMap ? ownMap->offsetMs : 0.0;
    for (const Candidate& c : candidates) {
        const auto map = orbiscore::ReadInsvTimeMap(c.path);
        const double offsetMs = map ? map->offsetMs : 0.0;
        std::string err;
        std::vector<orbiscore::InsvStreamInfo> streams;
        Part part;
        part.path = c.path;
        int64_t durationMs = 0;
        if (!videoStreams(c.path, streams, err) || streams.empty() ||
            !lensSourcesOf(c.path, streams, model, part.lenses, part.durationUs, durationMs, err)) {
            std::fprintf(stderr, "[camera] %s continues %s but can't be read (%s); the clip ends "
                                 "before it\n",
                         c.path.c_str(), out.path.c_str(), err.c_str());
            break;
        }
        if (streams[0].width != s0.width || streams[0].height != s0.height ||
            std::abs(streams[0].fps - s0.fps) > 1e-3) {
            std::fprintf(stderr, "[camera] %s continues %s but its video isn't like it; the clip "
                                 "ends before it\n",
                         c.path.c_str(), out.path.c_str());
            break;
        }
        if (const auto theirs = orbiscore::ReadInsvMakerNotes(c.path))
            part.proxyLenses = proxyLensesOf(
                findProxy(c.path, model, *theirs, orbiscore::ReadInsvHeldName(c.path)), model);
        const Part& prev = out.parts.back();
        // By the camera's clock the next part starts where this one ends, to
        // the frame clock's drift from the nominal rate (tens of ppm).
        part.startUs = prev.startUs + prev.durationUs;
        if (prevFirstFrame != 0 && c.firstFrame != 0) {
            // From video start to video start: field 24 names the
            // recording's frame, a re-written copy's video starts later.
            const double gapSec = static_cast<double>(static_cast<int64_t>(c.firstFrame - prevFirstFrame)) *
                                      model.telemetryTickUs * 1e-6 +
                                  (offsetMs - prevOffsetMs) * 1e-3 * out.captureScale;
            const double expectSec = static_cast<double>(prev.durationUs) * 1e-6 * out.captureScale;
            const double slackSec = std::max(1.0, 0.01 * expectSec);
            if (gapSec < expectSec - slackSec) {
                std::fprintf(stderr, "[camera] %s starts before %s ends (%.3f s on, not %.3f); "
                                     "the clip ends before it\n",
                             c.path.c_str(), prev.path.c_str(), gapSec, expectSec);
                break;
            }
            if (gapSec > expectSec + slackSec) {
                // A part between them isn't here: this one goes where the
                // camera's clock puts it, on the frame grid. Off by the
                // missing part's drift (tens of ms in 30 min), which only
                // moves where the gap ends.
                const double videoSec = gapSec / out.captureScale;
                part.startUs = prev.startUs +
                               (s0.fps > 0.0 ? std::llround(std::round(videoSec * s0.fps) * 1e6 / s0.fps)
                                             : std::llround(videoSec * 1e6));
                std::fprintf(stderr, "[camera] %s starts %.3f s after %s, not %.3f: a part "
                                     "between them isn't here; a gap in the clip\n",
                             c.path.c_str(), gapSec, prev.path.c_str(), expectSec);
            }
        }
        out.parts.push_back(std::move(part));
        prevFirstFrame = c.firstFrame;
        prevOffsetMs = offsetMs;
    }
    // The last part as long as the camera writes a file: the recording may
    // go on in the next counter's file, which isn't here.
    if (const Part& last = out.parts.back();
        model.fileLimitSec > 0 &&
        static_cast<double>(last.durationUs) * 1e-6 >= model.fileLimitSec - 1.0) {
        if (const auto lastName = splitCounter(fs::path(last.path).filename().string())) {
            std::string next = std::to_string(std::stoull(lastName->digits) + 1);
            if (next.size() < lastName->digits.size())
                next.insert(0, lastName->digits.size() - next.size(), '0');
            out.mayContinueIn = lastName->stem + next + lastName->rest;
        }
    }
    if (out.parts.size() > 1) {
        const Part& last = out.parts.back();
        out.durationMs = (last.startUs + last.durationUs) / 1000;
        std::fprintf(stderr, "[camera] %s: a recording in %zu files, %.3f s\n", out.path.c_str(),
                     out.parts.size(), static_cast<double>(last.startUs + last.durationUs) * 1e-6);
    }
}

// The lens profile `name` from the file's table, or for a file that carries
// none (an X5 photo) from the model's (InsvModel::lensProfiles).
std::optional<orbiscore::LensProfile> profile(const orbiscore::InsvMakerNotes& notes,
                                              const orbiscore::InsvModel& model, const char* name) {
    if (!name)
        return std::nullopt;
    if (const orbiscore::LensProfile* p = orbiscore::FindLensProfile(notes, name))
        return *p;
    if (notes.lens_profiles.empty())
        for (const orbiscore::LensProfile& p : model.lensProfiles)
            if (p.name == name)
                return p;
    return std::nullopt;
}

// The lens profiles of `out` (bare and the model's guards), the model's when
// the file has no table of its own (said so).
void readLensProfiles(ClipMetadata& out, const orbiscore::InsvMakerNotes& notes,
                      const orbiscore::InsvModel& model) {
    out.bareLens = profile(notes, model, "bare");
    out.standardGuard = profile(notes, model, model.standardGuard);
    out.premiumGuard = profile(notes, model, model.premiumGuard);
    if (notes.lens_guard_setting == 0 || notes.lens_guard_setting == 5)
        out.guardFitted = notes.lens_guard_setting == 5;
    else if (notes.lens_guard_setting >= 0)
        std::fprintf(stderr, "[camera] lens-guard setting %lld in %s not known; not used\n",
                     static_cast<long long>(notes.lens_guard_setting), out.path.c_str());
    if (notes.lens_profiles.empty() && !model.lensProfiles.empty())
        std::fprintf(stderr, "[camera] no lens-guard profiles in %s; taking the %s's\n",
                     out.path.c_str(), model.name);
}

// The MEI calibration Insta360 Studio stitches with, into `out.lenses`
// (sized 2): the 56-token string (every X5 file carries it), else the
// shorter offset_v3 (field 54; the ONE X2's only one). Sets `calibrated`.
void readCalibration(ClipMetadata& out, const orbiscore::InsvMakerNotes& notes,
                     const orbiscore::InsvModel& model) {
    orbiscore::CameraCalibration calib;
    const auto ext = orbiscore::ReadInsvExtendedCalibrationString(out.path);
    if (((ext && orbiscore::ParseExtendedCalibrationString(*ext, calib)) ||
         orbiscore::ParseCalibrationV3String(notes.offset_v3, calib)) &&
        calib.lensesExt.size() == 2) {
        // fx / fy are given in field 27's sensor window.
        double focalRefW = notes.crop_dst_w;
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
            out.lenses[i].pose = orbiscore::Insta360LensPose(i);
        }
    }
}

// The rate a series of photos plays at, a photo a frame: a photo has none
// of its own. The clip's speed sets the pace.
constexpr double kPhotoSeriesFps = 30.0;

// A photo of an Interval series and when it was taken (field 24, raw ticks).
struct SeriesPhoto {
    std::string path;
    uint64_t taken = 0;
};

// Whether `notes` is a photo of the same Interval series as `series`'s: the
// same camera, naming the same first photo.
bool sameSeries(const orbiscore::InsvMakerNotes& notes, const orbiscore::InsvMakerNotes& series) {
    return notes.file_group_type == orbiscore::kInsvGroupIntervalPhotos &&
           notes.serial_number == series.serial_number &&
           notes.file_group_identify == series.file_group_identify;
}

// The photos of the Interval series the photo `self` (maker notes `notes`)
// belongs to, beside it, in the order they were taken: the files with its
// extension of the same series (field 26, docs/formats/insta360-x5.md §3.6).
std::vector<SeriesPhoto> photoSeries(const std::string& self, const orbiscore::InsvMakerNotes& notes) {
    namespace fs = std::filesystem;
    const fs::path p(self);
    std::vector<SeriesPhoto> photos{{self, notes.first_frame_timestamp}};
    std::error_code ec;
    const fs::path dir = p.has_parent_path() ? p.parent_path() : fs::path(".");
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != p.extension() || e.path().filename() == p.filename())
            continue;
        const auto theirs = orbiscore::ReadInsvMakerNotes(e.path().string());
        if (theirs && sameSeries(*theirs, notes))
            photos.push_back({e.path().string(), theirs->first_frame_timestamp});
    }
    std::sort(photos.begin(), photos.end(), [](const SeriesPhoto& a, const SeriesPhoto& b) {
        return a.taken != b.taken ? a.taken < b.taken : a.path < b.path;
    });
    return photos;
}

// An Interval series of 360 photos (docs/formats/insta360-x5.md §3.6) as
// one clip from the photo `out.path` on, a photo a part (Part::still) at
// kPhotoSeriesFps. Each photo is one JPEG with both lenses side by side,
// each lens's half where its calibration puts its centre.
CameraReader::Result readPhotoSeries(const std::vector<orbiscore::InsvStreamInfo>& streams,
                                     ClipMetadata& out, const orbiscore::InsvMakerNotes& notes,
                                     const orbiscore::InsvModel& model, std::string& err) {
    const orbiscore::InsvStreamInfo& pic = streams[0];
    if (streams.size() != 1 || pic.width != 2 * pic.height) {
        err = "a 360 photo with both lenses side by side in one picture expected, found " +
              std::to_string(streams.size()) + " picture(s), the first " +
              std::to_string(pic.width) + "x" + std::to_string(pic.height);
        return CameraReader::Result::Failed;
    }
    out.projection = Projection::DualFisheye;
    out.lenses.resize(2);
    readCalibration(out, notes, model);
    // The calibration's centres are given in the side-by-side frame
    // (LensCalibExt::cx): lens 1's in the right half on the X5.
    bool left[2] = {true, false};
    if (out.calibrated) {
        for (int i = 0; i < 2; ++i) {
            const orbiscore::LensCalibExt& c = out.lenses[i].calibration;
            left[i] = c.cx < c.w * 0.5;
        }
        if (left[0] == left[1]) {
            std::fprintf(stderr, "[camera] both lenses' calibrated centres in one half of %s; "
                                 "taking lens 0 as the left\n",
                         out.path.c_str());
            left[0] = true;
            left[1] = false;
        }
    }
    const auto lensesOf = [&](const std::string& path) {
        std::vector<LensSource> l;
        for (int i = 0; i < 2; ++i)
            l.push_back({path, 0, {left[i] ? 0.0 : 0.5, 0.0, 0.5, 1.0}});
        return l;
    };

    // From this photo on: an earlier one opens the series from there.
    std::vector<SeriesPhoto> photos = photoSeries(out.path, notes);
    photos.erase(photos.begin(),
                 std::find_if(photos.begin(), photos.end(),
                              [&](const SeriesPhoto& s) { return s.path == out.path; }));
    out.parts.clear();
    const auto startUs = [](size_t i) {
        return std::llround(static_cast<double>(i) * 1e6 / kPhotoSeriesFps);
    };
    for (size_t i = 0; i < photos.size(); ++i) {
        Part part;
        part.path = photos[i].path;
        part.lenses = lensesOf(part.path);
        part.startUs = startUs(i);
        part.durationUs = startUs(i + 1) - part.startUs;
        part.still = true;
        out.parts.push_back(std::move(part));
    }
    for (int i = 0; i < 2; ++i)
        out.lenses[i].source = out.parts[0].lenses[i];
    out.photoCount = static_cast<int>(photos.size());
    out.fps = kPhotoSeriesFps;
    out.durationMs = startUs(photos.size()) / 1000;
    out.lensWidth = out.lensHeight = pic.height;
    // A series whose first photo isn't beside it: it starts here.
    if (const std::string first = std::filesystem::path(notes.file_group_identify).filename().string();
        !first.empty() && first != std::filesystem::path(out.path).filename().string()) {
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::path(out.path).parent_path() / first, ec))
            out.missingFirst = first;
    }
    // The time between photos, the camera's clock per photo: the median gap
    // (they're 9.9 to 23.1 s apart on the samples, 10.3 s typical).
    if (photos.size() >= 2) {
        std::vector<double> gaps;
        for (size_t i = 1; i < photos.size(); ++i)
            gaps.push_back(static_cast<double>(photos[i].taken - photos[i - 1].taken) *
                           model.telemetryTickUs * 1e-6);
        std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
        out.timelapseIntervalSec = gaps[gaps.size() / 2];
        out.captureScale = out.timelapseIntervalSec * kPhotoSeriesFps;
    }
    out.mountYawDeg = model.mountDeg[0];
    out.mountPitchDeg = model.mountDeg[1];
    out.mountRollDeg = model.mountDeg[2];
    out.lensCorrection = model.lensCorrection;
    readLensProfiles(out, notes, model);
    out.recordedLocal = LocalTimeFromDigits(std::to_string(notes.creation_time));
    if (out.recordedLocal.empty())
        out.recordedLocal = nameTime(out.path);
    // Each photo's gravity (record 0x300), where the model's axes for it are
    // known: levelled photo by photo, no heading (readImu).
    if (const auto tele = orbiscore::ReadInsvTelemetry(out.path, 1, model.telemetryTickUs);
        tele && !tele->imu.empty() && model.photoGravityAxes && photos.size() >= 2) {
        out.hasMotion = out.orientationPerFrame = true;
        out.heading = false;
    }
    std::fprintf(stderr, "[camera] %s: an Interval series of %zu photos from here, %.1f s apart\n",
                 out.path.c_str(), photos.size(), out.timelapseIntervalSec);
    return CameraReader::Result::Read;
}

} // namespace

CameraReader::Result Insta360Reader::read(const std::vector<orbiscore::InsvStreamInfo>& streams,
                                          ClipMetadata& out, std::string& err) const {
    const auto notes = orbiscore::ReadInsvMakerNotes(out.path);
    if (!notes)
        return Result::NotMine;
    bool known = false;
    const orbiscore::InsvModel& model = orbiscore::FindInsvModel(notes->model, &known);
    if (!known)
        std::fprintf(stderr, "[camera] no table entry for \"%s\"; using the %s's\n",
                     notes->model.c_str(), model.name);
    out.maker = model.maker;
    out.model = notes->model;
    out.firmware = notes->firmware;
    out.serial = notes->serial_number;
    if (notes->file_group_type == orbiscore::kInsvGroupIntervalPhotos)
        return readPhotoSeries(streams, out, *notes, model, err);
    if (std::filesystem::path(out.path).extension() == ".insp") {
        err = "a single 360 photo, which doesn't open yet (an Interval series does)";
        return Result::Failed;
    }

    // Both lenses as streams of this file, or lens 0 here and lens 1 in a
    // file of its own beside it.
    out.projection = Projection::DualFisheye;
    Part first;
    first.path = out.path;
    if (!lensSourcesOf(out.path, streams, model, first.lenses, first.durationUs, out.durationMs,
                       err))
        return Result::Failed;
    out.lenses.resize(2);
    for (int i = 0; i < 2; ++i)
        out.lenses[i].source = first.lenses[i];
    out.parts = {std::move(first)};
    readCalibration(out, *notes, model);
    out.mountYawDeg = model.mountDeg[0];
    out.mountPitchDeg = model.mountDeg[1];
    out.mountRollDeg = model.mountDeg[2];
    out.lensCorrection = model.lensCorrection;
    if (streams[0].width == streams[0].height)
        out.captureMode = model.captureMode(streams[0].width);

    readLensProfiles(out, *notes, model);
    const std::optional<std::string> heldName = orbiscore::ReadInsvHeldName(out.path);
    out.proxyPath = findProxy(out.path, model, *notes, heldName);
    out.parts[0].proxyLenses = proxyLensesOf(out.proxyPath, model);
    // By the camera's clock: field 7 where the model writes it to the second
    // (the ONE X2 doesn't write it, the A1 only to the minute), else the
    // name the camera gave the file, which carries the same time: the one
    // the file holds, which survives a rename, then the one it has.
    out.recordedLocal = LocalTimeFromDigits(std::to_string(notes->creation_time));
    if (out.recordedLocal.empty() && heldName)
        out.recordedLocal = nameTime(*heldName);
    if (out.recordedLocal.empty()) {
        out.recordedLocal = nameTime(out.path);
        std::fprintf(stderr, "[camera] no recording time to the second in %s's metadata or the "
                             "name it holds; from its file name: %s\n",
                     out.path.c_str(),
                     out.recordedLocal.empty() ? "none" : out.recordedLocal.c_str());
    }

    if (const auto tele =
            orbiscore::ReadInsvTelemetry(out.path, kImuProbeSamples, model.telemetryTickUs)) {
        if (tele->imu.size() >= 2) {
            out.hasMotion = true;
            // Microseconds, whatever the model's tick (ReadInsvTelemetry).
            const double spanSec =
                (tele->imu.back().timestamp - tele->imu.front().timestamp) / 1e6;
            if (spanSec > 0)
                out.imuRateHz = (tele->imu.size() - 1) / spanSec;
        } else if (!tele->timelapse_quats.empty() && model.timelapseQuatAxes &&
                   !tele->frame_timestamps.empty()) {
            // A timelapse's orientation per frame (MakeInsvImuTrack's conditions).
            out.hasMotion = out.orientationPerFrame = true;
        }
        out.exposureRecords = static_cast<int64_t>(tele->exposure.size());
        out.rollingShutterMs = tele->rolling_shutter_ms;
        out.captureScale = orbiscore::CaptureScale(*tele, out.fps);
        if (out.captureScale > 1.0)
            out.timelapseIntervalSec = out.fps > 0.0 ? out.captureScale / out.fps : 0.0;
        else if (out.captureScale < 1.0)
            out.captureFps = tele->capture_fps;
        // Field 22 (§3.3): the stream itself is tagged the same either way.
        out.colorProfile = tele->gamma_mode;
        out.logProfile = tele->gamma_mode == "I_Log" ? "I-Log" : "";
        out.gpsRecords = static_cast<int64_t>(tele->gps_records);
        out.gpsFixes = static_cast<int64_t>(tele->gps_fixes);
        out.gpsRateHz = tele->gps_rate_hz;
        out.gpsTrackSec = tele->gps_track_s;
    }
    findParts(out, model, *notes, streams[0]);
    // A later part of a recording whose first file isn't beside it (field
    // 26 names that file): the clip starts here.
    if (notes->file_group_index > 0 && !notes->file_group_identify.empty()) {
        const std::filesystem::path self(out.path);
        const std::string first = firstFileName(out.path, notes->file_group_identify);
        std::error_code ec;
        if (!first.empty() && first != self.filename().string() &&
            !std::filesystem::exists(self.parent_path() / first, ec))
            out.missingFirst = first;
    }
    for (const Part& part : out.parts)
        if (const auto map = orbiscore::ReadInsvTimeMap(part.path); map && !map->plainTrim()) {
            out.unevenTimeMap = true;
            std::fprintf(stderr, "[camera] %s was re-written with %u cut(s), its offset into the "
                                 "recording varying by %.1f ms; lined up by its median %.1f ms, "
                                 "so stabilization, marks and GPS can be off in it\n",
                         part.path.c_str(), map->cuts, map->offsetSpreadMs, map->offsetMs);
        }
    for (size_t k = 1; k < out.parts.size(); ++k)
        if (const auto tele = orbiscore::ReadInsvTelemetry(out.parts[k].path, kImuProbeSamples,
                                                           model.telemetryTickUs)) {
            out.exposureRecords += static_cast<int64_t>(tele->exposure.size());
            out.gpsRecords += static_cast<int64_t>(tele->gps_records);
            out.gpsFixes += static_cast<int64_t>(tele->gps_fixes);
            out.gpsTrackSec += tele->gps_track_s;
            if (out.gpsRateHz == 0.0)
                out.gpsRateHz = tele->gps_rate_hz;
        }
    // The highlight marks set while recording (§4.3), each part's: device
    // time from its first recorded frame, on the camera's clock; a trimmed
    // file's video starts the time map's offset (video time) later.
    for (const Part& part : out.parts) {
        const auto marks = orbiscore::ReadInsvHighlights(part.path, model.telemetryTickUs);
        if (!marks)
            continue;
        const int64_t startMs = part.startUs / 1000;
        const int64_t lengthMs = out.parts.size() == 1 ? out.durationMs : part.durationUs / 1000;
        const double offsetSec = marks->time_map_offset_ms * 1e-3 * out.captureScale;
        for (const double sec : marks->since_first_frame_sec) {
            const int64_t ms = videoMsAt(sec - offsetSec, out, marks->frame_sec);
            if (ms >= 0 && (lengthMs <= 0 || ms <= lengthMs))
                out.highlightMs.push_back(startMs + ms);
        }
    }
    return Result::Read;
}

std::optional<orbiscore::ImuTrack> Insta360Reader::readImu(const ClipMetadata& clip) const {
    const orbiscore::InsvModel& model = orbiscore::FindInsvModel(clip.model);
    if (clip.photoCount > 0) {
        if (!clip.hasMotion)
            return std::nullopt;
        // Each photo's gravity and when it was taken, where it plays.
        std::vector<orbiscore::InsvPhotoGravity> photos;
        for (const Part& part : clip.parts) {
            const auto tele = orbiscore::ReadInsvTelemetry(part.path, 1, model.telemetryTickUs);
            if (!tele || tele->imu.empty() || tele->first_frame_timestamp == 0) {
                std::fprintf(stderr, "[camera] no gravity in %s, a photo of %s; left out of its "
                                     "levelling\n",
                             part.path.c_str(), clip.path.c_str());
                continue;
            }
            orbiscore::InsvPhotoGravity g;
            g.takenUs = tele->first_frame_timestamp;
            std::copy(std::begin(tele->imu[0].accel), std::end(tele->imu[0].accel), g.up);
            g.videoSec = static_cast<double>(part.startUs) * 1e-6;
            photos.push_back(g);
        }
        return orbiscore::MakeInsvPhotoTrack(photos, clip.captureScale, model);
    }
    auto tele = orbiscore::ReadInsvTelemetry(clip.path, 0, model.telemetryTickUs);
    if (!tele || !clip.hasMotion)
        return std::nullopt;
    if (clip.parts.size() <= 1)
        return orbiscore::MakeInsvImuTrack(std::move(*tele), clip.fps, model);
    // Each part's track by its own file, then one.
    std::vector<orbiscore::ImuTrackPart> tracks;
    for (size_t k = 0; k < clip.parts.size(); ++k) {
        const Part& part = clip.parts[k];
        if (k > 0)
            tele = orbiscore::ReadInsvTelemetry(part.path, 0, model.telemetryTickUs);
        if (!tele) {
            std::fprintf(stderr, "[camera] no motion data in %s, part %zu of %s\n",
                         part.path.c_str(), k + 1, clip.path.c_str());
            continue;
        }
        tracks.push_back({orbiscore::MakeInsvImuTrack(std::move(*tele), clip.fps, model),
                          static_cast<double>(part.startUs) * 1e-6,
                          static_cast<double>(part.durationUs) * 1e-6});
    }
    return orbiscore::JoinImuTracks(std::move(tracks));
}

std::optional<orbiscore::GpsTrack> Insta360Reader::readGps(const ClipMetadata& clip) const {
    if (clip.gpsFixes == 0)
        return std::nullopt;
    // Each part's fixes by its own file, in clip time.
    std::optional<orbiscore::GpsTrack> track;
    for (const Part& part : clip.parts) {
        const auto gps =
            orbiscore::ReadInsvGps(part.path, orbiscore::FindInsvModel(clip.model).telemetryTickUs,
                                   clip.captureScale);
        if (!gps || gps->fixes.empty())
            continue;
        // The fixes carry Unix time; field 98.3 says when the first frame was
        // recorded on the same scale (docs/formats/insta360-x5.md §8), or, on
        // a drone without it, the fixes' speeds matched against the flight
        // controller's (docs/formats/antigravity-a1.md). Without either the
        // fixes aren't used.
        if (gps->first_frame_unix_ms == 0) {
            std::fprintf(stderr, "[camera] GPS in %s but nothing lines it up with the video (no "
                                 "field 98, no flight-controller speed to match); not used\n",
                         part.path.c_str());
            continue;
        }
        if (gps->aligned_by_speed)
            std::fprintf(stderr, "[camera] GPS in %s lined up with the video by its speed: first "
                                 "frame at Unix %.3f s\n",
                         part.path.c_str(), static_cast<double>(gps->first_frame_unix_ms) * 1e-3);
        if (!track) {
            track.emplace();
            track->clockStamped = !gps->aligned_by_speed;
        }
        track->fixes.reserve(track->fixes.size() + gps->fixes.size());
        const int64_t startMs = part.startUs / 1000;
        // A heading of -1 is the camera's "don't know", written while
        // standing still (§8). Unix time runs with the camera's clock, not
        // the video's.
        for (const orbiscore::InsvGpsFix& f : gps->fixes) {
            const double camSec = static_cast<double>(f.unix_ms - gps->first_frame_unix_ms) * 1e-3;
            track->fixes.push_back({startMs + videoMsAt(camSec, clip, gps->frame_sec), f.latitude,
                                    f.longitude, f.speed_mps,
                                    f.heading_deg < 0.0 ? std::nan("") : f.heading_deg,
                                    f.altitude_m});
        }
    }
    return track;
}

std::optional<std::string> Insta360Reader::clipFileFor(const std::string& path) const {
    // VID_<date>_<time>_<lens>_<n>.insv -> the clip's VID_…_00_…, and
    // LRV_…_<token>_<n>.<ext> likewise, by each model's names; a proxy whose
    // name carries no lens token (the A1's PRX_….prx) -> VID_….insv. The
    // lens token is the last `_xx_` (the date and time are longer runs).
    const std::filesystem::path p(path);
    const auto byName = [&](const std::string& name) -> std::optional<std::string> {
        const auto clipName = [&](const orbiscore::InsvModel& m, const char* token) {
            std::string n = name;
            if (token)
                n.replace(n.rfind(token), std::strlen(token), m.clipToken);
            n.replace(0, 3, "VID");
            std::filesystem::path c = p.parent_path() / n;
            c.replace_extension(".insv");
            return c.string();
        };
        const std::filesystem::path np(name);
        for (const orbiscore::InsvModel& m : orbiscore::InsvModels()) {
            if (m.secondLensToken && m.clipToken && name.rfind("VID_", 0) == 0 &&
                name.ends_with(".insv") && name.rfind(m.secondLensToken) != std::string::npos)
                return clipName(m, m.secondLensToken);
            if (name.rfind(std::string(m.proxyPrefix) + "_", 0) == 0 &&
                np.extension() == m.proxyExt &&
                (m.proxyToken ? m.clipToken && name.rfind(m.proxyToken) != std::string::npos
                              : !m.clipToken))
                return clipName(m, m.proxyToken);
        }
        return std::nullopt;
    };
    if (auto clip = byName(p.filename().string()))
        return clip;
    // A photo of an Interval series: the series' first photo beside it (the
    // one field 26 names, else the earliest), whose clip holds it.
    if (const auto notes = orbiscore::ReadInsvMakerNotes(path);
        notes && notes->file_group_type == orbiscore::kInsvGroupIntervalPhotos &&
        !notes->file_group_identify.empty()) {
        std::error_code ec;
        const std::filesystem::path named =
            p.parent_path() / std::filesystem::path(notes->file_group_identify).filename();
        std::string first;
        if (named.filename() == p.filename())
            return std::nullopt;
        if (const auto theirs = std::filesystem::exists(named, ec)
                                    ? orbiscore::ReadInsvMakerNotes(named.string())
                                    : std::nullopt;
            theirs && sameSeries(*theirs, *notes))
            first = named.string();
        else
            first = photoSeries(path, *notes).front().path;
        if (std::filesystem::equivalent(first, p, ec))
            return std::nullopt;
        return first;
    }
    // A later part of a split recording (§1, field 26): the recording's
    // first file, when that is beside it and takes it in as a part; else it
    // stands as a clip of its own.
    if (p.extension() == ".insv")
        if (const auto notes = orbiscore::ReadInsvMakerNotes(path);
            notes && notes->file_group_index > 0 && !notes->file_group_identify.empty()) {
            const std::filesystem::path first =
                p.parent_path() / firstFileName(path, notes->file_group_identify);
            std::error_code ec;
            if (first.filename() != p.filename() && std::filesystem::exists(first, ec))
                if (const auto clip = ReadClipMetadata(first.string()))
                    for (const Part& part : clip->parts)
                        if (std::filesystem::equivalent(part.path, p, ec))
                            return first.string();
            return std::nullopt;
        }
    // A renamed proxy: by the name it holds (the clip may have kept its
    // own), else the clip renamed alike, the same stem with `.insv`.
    // ClipFileOf checks that the clip finds this file as its proxy.
    std::vector<std::string> candidates;
    if (const auto held = orbiscore::ReadInsvHeldName(path))
        if (auto clip = byName(*held))
            candidates.push_back(*clip);
    for (const orbiscore::InsvModel& m : orbiscore::InsvModels())
        if (p.extension() == m.proxyExt && p.extension() != ".insv") {
            candidates.push_back(std::filesystem::path(p).replace_extension(".insv").string());
            break;
        }
    std::error_code ec;
    for (const std::string& c : candidates)
        if (std::filesystem::exists(c, ec))
            return c;
    if (!candidates.empty())
        return candidates.front();
    return std::nullopt;
}

std::optional<Thumbnail> Insta360Reader::readThumbnail(const std::string& path) const {
    auto thumb = orbiscore::ReadInsvThumbnail(path);
    if (!thumb)
        return stitchedThumbnail(path);
    return Thumbnail{thumb->width, thumb->height, thumb->flat, thumb->coding,
                     std::move(thumb->nv12)};
}

} // namespace camera
