// Quick standalone check of src/insv/InsvTrailer.{h,cpp} against a real .insv
// file.
//
//   insv_trailer_dump <input.insv>              maker notes + telemetry summary
//   insv_trailer_dump <input.insv> --imu        every IMU sample, one per line
//   insv_trailer_dump <input.insv> --exposure   every exposure sample
//   insv_trailer_dump <input.insv> --gps        every GPS fix
//   insv_trailer_dump <input.insv> --quats      a timelapse's orientation
//                                               record and frame times
//
// The --imu / --exposure output is laid out to diff against `exiftool -ee`:
//   exiftool "Time Code"        == column 2 here (timestamp / 1000, 3 decimals)
//   exiftool "Accelerometer"    == columns 3..5   (X5 uses the 20-byte layout,
//   exiftool "Angular Velocity" == columns 6..8    which %g reproduces exactly)
//   exiftool "Exposure Time"    == column 3 of --exposure
// --gps prints each fix once (the file repeats it over ~10 records), lined
// up with the video by field 98.3 (on a drone without it, by matching the
// fixes' speeds against the flight controller's); with `exiftool -ee -n`:
//   exiftool "GPS Date/Time"    == column 3 (Unix seconds, 3 decimals)
//   exiftool "GPS Latitude", "GPS Longitude", "GPS Speed" (m/s),
//   "GPS Track", "GPS Altitude" == columns 4..8

#include <InsvModels.h>
#include <InsvTrailer.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace {

void PrintSummary(const insv::InsvTelemetry& t) {
    std::cout << "IMU samples:      " << t.imu.size();
    if (t.imu.size() >= 2) {
        const double span = (t.imu.back().timestamp - t.imu.front().timestamp) / 1000.0;
        const double hz = span > 0 ? (t.imu.size() - 1) / (span / 1000.0) : 0.0;
        std::printf("  (%.3f .. %.3f, ~%.0f Hz)", t.imu.front().timestamp / 1000.0,
                    t.imu.back().timestamp / 1000.0, hz);
    }
    std::cout << "\n";
    std::cout << "Exposure samples: " << t.exposure.size();
    if (t.exposure.size() >= 2) {
        std::printf("  (%.3f .. %.3f)", t.exposure.front().timestamp / 1000.0,
                    t.exposure.back().timestamp / 1000.0);
    }
    std::cout << std::endl;
    if (!t.frame_timestamps.empty())
        std::printf("Frame times:      %zu  (%.3f .. %.3f)\n", t.frame_timestamps.size(),
                    t.frame_timestamps.front() / 1000.0, t.frame_timestamps.back() / 1000.0);
    if (!t.timelapse_quats.empty())
        std::printf("Orientation:      %zu  (timelapse, record 0x1800)\n", t.timelapse_quats.size());
}

const char kUsage[] =
    "usage: insv_trailer_dump <input.insv> [--imu | --exposure | --gps | --quats | --marks]\n"
    "  (no option)   maker notes + telemetry summary\n"
    "  --imu         every IMU sample, one per line\n"
    "  --exposure    every exposure sample, one per line\n"
    "  --gps         every GPS fix, one per line: index, video ms, Unix s, latitude,\n"
    "                longitude, speed m/s, heading, altitude m\n"
    "  --quats       a timelapse's frames and orientation, one per line: `F index ms` for\n"
    "                each frame time (record 0x600), `Q index ms w x y z` for each\n"
    "                orientation entry (record 0x1800, world -> camera axes)\n"
    "  --marks       the anchors record's lists (0xa00, device ms), then each highlight\n"
    "                mark: index, seconds of the camera's clock from the first frame\n";

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::fputs(kUsage, stdout);
            return 0;
        }
    if (argc < 2) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    const char* path = argv[1];
    const bool want_imu = argc > 2 && std::strcmp(argv[2], "--imu") == 0;
    const bool want_exposure = argc > 2 && std::strcmp(argv[2], "--exposure") == 0;
    const bool want_gps = argc > 2 && std::strcmp(argv[2], "--gps") == 0;
    const bool want_quats = argc > 2 && std::strcmp(argv[2], "--quats") == 0;
    const bool want_marks = argc > 2 && std::strcmp(argv[2], "--marks") == 0;

    if (want_marks) {
        const auto notes = insv::ReadInsvMakerNotes(path);
        const double tick = notes ? insv::FindInsvModel(notes->model).telemetryTickUs : 1.0;
        const auto marks = insv::ReadInsvHighlights(path, tick);
        if (!marks) {
            std::cerr << "No Insta360 trailer found in " << path << std::endl;
            return 1;
        }
        const auto record = insv::ReadInsvRecord(path, 0xa00);
        if (!record)
            std::printf("# no anchors record (0xa00)\n");
        else
            for (const insv::InsvAnchorList& l : insv::ParseInsvAnchors(*record)) {
                std::printf("# list %u: %zu", unsigned(l.type), l.timestamps.size());
                for (const uint64_t t : l.timestamps)
                    std::printf(" %.3f", static_cast<double>(t) * tick / 1000.0);
                std::printf("\n");
            }
        for (size_t i = 0; i < marks->since_first_frame_sec.size(); ++i)
            std::printf("%zu %.3f\n", i, marks->since_first_frame_sec[i]);
        return 0;
    }

    if (want_gps) {
        const auto notes = insv::ReadInsvMakerNotes(path);
        const auto gps = insv::ReadInsvGps(
            path, notes ? insv::FindInsvModel(notes->model).telemetryTickUs : 1.0);
        if (!gps) {
            std::cerr << "No Insta360 trailer found in " << path << std::endl;
            return 1;
        }
        if (gps->first_frame_unix_ms == 0)
            std::printf("# no first-frame time (field 98.3, or a flight controller's speed to "
                        "match): video ms not known\n");
        else if (gps->aligned_by_speed)
            std::printf("# no field 98: video ms from the flight controller's speed\n");
        for (size_t i = 0; i < gps->fixes.size(); ++i) {
            const auto& f = gps->fixes[i];
            const long long video = gps->first_frame_unix_ms ? f.unix_ms - gps->first_frame_unix_ms : 0;
            std::printf("%zu %lld %.3f %.15g %.15g %.15g %.15g %.15g\n", i, video,
                        f.unix_ms / 1000.0, f.latitude, f.longitude, f.speed_mps,
                        f.heading_deg, f.altitude_m);
        }
        return 0;
    }

    if (want_quats) {
        const auto notes = insv::ReadInsvMakerNotes(path);
        const auto tel = insv::ReadInsvTelemetry(
            path, 1, notes ? insv::FindInsvModel(notes->model).telemetryTickUs : 1.0);
        if (!tel) {
            std::cerr << "No Insta360 trailer found in " << path << std::endl;
            return 1;
        }
        for (size_t i = 0; i < tel->frame_timestamps.size(); ++i)
            std::printf("F %zu %.3f\n", i, tel->frame_timestamps[i] / 1000.0);
        for (size_t i = 0; i < tel->timelapse_quats.size(); ++i) {
            const auto& e = tel->timelapse_quats[i];
            std::printf("Q %zu %.3f %.9f %.9f %.9f %.9f\n", i, e.timestamp / 1000.0, e.q[0], e.q[1],
                        e.q[2], e.q[3]);
        }
        return 0;
    }

    if (want_imu || want_exposure) {
        const auto tel = insv::ReadInsvTelemetry(path);
        if (!tel) {
            std::cerr << "No Insta360 trailer found in " << path << std::endl;
            return 1;
        }
        if (want_imu) {
            for (size_t i = 0; i < tel->imu.size(); ++i) {
                const auto& s = tel->imu[i];
                std::printf("%zu %.3f %g %g %g %g %g %g\n", i, s.timestamp / 1000.0,
                            s.accel[0], s.accel[1], s.accel[2],
                            s.gyro[0], s.gyro[1], s.gyro[2]);
            }
        } else {
            for (size_t i = 0; i < tel->exposure.size(); ++i) {
                const auto& s = tel->exposure[i];
                std::printf("%zu %.3f %.15g\n", i, s.timestamp / 1000.0, s.exposure_time);
            }
        }
        return 0;
    }

    const auto calib = insv::ReadInsvMakerNotes(path);
    if (!calib) {
        std::cerr << "No Insta360 trailer/maker-notes record found in " << path << std::endl;
        return 1;
    }
    std::cout << "Serial:     " << calib->serial_number << "\n"
              << "Model:      " << calib->model << "\n"
              << "Firmware:   " << calib->firmware << "\n"
              << "Parameters: " << calib->parameters << "\n";
    if (!calib->offset_v3.empty())
        std::cout << "offset_v3:  " << calib->offset_v3 << "\n";
    for (const auto& lp : calib->lens_profiles) {
        std::cout << "Lens profile: " << lp.name << " fov=" << lp.fov_deg << " poly=";
        for (double c : lp.poly) std::cout << " " << c;
        std::cout << "\n";
    }

    // At the model's clock tick (ms on the ONE X2), so the rate is in Hz.
    const auto tel =
        insv::ReadInsvTelemetry(path, 0, insv::FindInsvModel(calib->model).telemetryTickUs);
    if (tel) PrintSummary(*tel);
    return 0;
}
