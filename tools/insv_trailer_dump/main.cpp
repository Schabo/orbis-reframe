// Quick standalone check of src/insv/InsvTrailer.{h,cpp} against a real .insv
// file.
//
//   insv_trailer_dump <input.insv>              maker notes + telemetry summary
//   insv_trailer_dump <input.insv> --imu        every IMU sample, one per line
//   insv_trailer_dump <input.insv> --exposure   every exposure sample
//
// The --imu / --exposure output is laid out to diff against `exiftool -ee`:
//   exiftool "Time Code"        == column 2 here (timestamp / 1000, 3 decimals)
//   exiftool "Accelerometer"    == columns 3..5   (X5 uses the 20-byte layout,
//   exiftool "Angular Velocity" == columns 6..8    which %g reproduces exactly)
//   exiftool "Exposure Time"    == column 3 of --exposure

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
}

const char kUsage[] =
    "usage: insv_trailer_dump <input.insv> [--imu | --exposure]\n"
    "  (no option)   maker notes + telemetry summary\n"
    "  --imu         every IMU sample, one per line\n"
    "  --exposure    every exposure sample, one per line\n";

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
    for (const auto& lp : calib->lens_profiles) {
        std::cout << "Lens profile: " << lp.name << " fov=" << lp.fov_deg << " poly=";
        for (double c : lp.poly) std::cout << " " << c;
        std::cout << "\n";
    }

    const auto tel = insv::ReadInsvTelemetry(path);
    if (tel) PrintSummary(*tel);
    return 0;
}
