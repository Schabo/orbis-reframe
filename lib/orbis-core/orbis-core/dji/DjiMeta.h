// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// DJI's 360 metadata: the protobuf samples of a `.OSV`'s `djmd` track, one
// per video frame (docs/formats/dji-360.md §2). The schema is DJI's
// (`dvtm_*.proto`, as published with Gyroflow's telemetry-parser); field
// numbers below are paths from its `ProductMeta`. Only the first sample
// carries the clip and stream metadata, every sample the frame's. No
// FFmpeg: the samples come from the caller (camera::DjiReader reads them).

namespace orbiscore {

// How a DJI product numbers its metadata: the schema differs by product and
// is chosen by product name, not by the proto file name a clip declares.
struct DjiSchema {
    const char* name; // the proto file it follows
    // StreamMeta's PanoDewarpParams field: 5 in dvtm_eagle4_wa530.proto
    // (Osmo 360 II, Osmo 360 Air, Avata 360), 6 in dvtm_oq101.proto (Osmo
    // 360). Parsed with the other, the calibration silently drops.
    uint32_t panoField;
};
const DjiSchema& DjiSchemaEagle4();
const DjiSchema& DjiSchemaOq101();

// One lens's calibration as DJI stores it (DewarpParams): the
// theta-polynomial with its tangential pair, in pixels of a `width` x
// `height` calibration frame, and the pose, degrees, camera from rig.
struct DjiDewarp {
    double fx = 0, fy = 0, cx = 0, cy = 0;
    double k[5] = {0, 0, 0, 0, 0}; // k1..k5 (fields 5-8, 15)
    double p[2] = {0, 0};          // field 20: p1, p2
    double width = 0, height = 0;
    double yaw = 0, pitch = 0, roll = 0;
    double lensModel = 0; // field 24, as stored (8 on the Avata 360)
    // Field 26, cam_imu_extri_q: the rotation between this lens's camera
    // and the IMU the fused attitude is in, w x y z; identity when absent.
    double imuQ[4] = {1, 0, 0, 0};
    bool hasImuQ = false;
    // A calibration, not one of the empty records: a focal length and a
    // frame.
    bool filled() const { return fx > 0 && fy > 0 && width > 0 && height > 0; }
};

// The calibration slots PanoDewarpParams holds, slave and master each.
enum class DjiSlot {
    NativeRefineFar, // fields 3 / 4: the only pair the Avata 360 fills
    NativeRefine,    // 1 / 2
    Native,          // 11 / 12
};
const char* DjiSlotName(DjiSlot s);

// The first sample's clip and stream metadata (ClipMeta, StreamMeta).
struct DjiClipMeta {
    std::string protoFileName;   // 1.1.1, what the clip declares
    std::string productName;     // 1.1.10, "DJI Avata360"
    std::string productSerial;   // 1.1.5
    std::string firmware;        // 1.1.6
    uint64_t clipTimestampUs = 0; // 1.1.9, device us: the first frame's
    uint32_t imuRateHz = 0;      // 1.8.1
    double sensorFps = 0;        // 1.9.1
    uint32_t lroNsPerLine = 0;   // 1.11.1
    uint32_t sensorWidth = 0, sensorHeight = 0; // 1.12
    std::string deviceName;      // 2.2.1.4, the camera module ("DJI FCA188")
    std::string deviceSerial;    // 2.2.3.1
    bool hasColorMode = false;
    uint32_t colorMode = 0;      // 2.2.4.1, ColorModeType (19 = D-Log M)
    uint32_t width = 0, height = 0; // 2.3.1 / 2.3.2, per lens track
    double framerate = 0;        // 2.3.3
    uint32_t bitDepth = 0;       // 2.3.5
    // The lens pair from the first filled slot (DjiSlot order).
    bool calibrated = false;
    DjiSlot slot = DjiSlot::NativeRefineFar;
    DjiDewarp slave, master;
};

// Parses the clip and stream metadata from a `ProductMeta` sample by
// `schema`. nullopt when the sample has no clip metadata (not the first).
std::optional<DjiClipMeta> ParseDjiClipMeta(const std::vector<uint8_t>& sample,
                                            const DjiSchema& schema);

// The product's name ("DJI Avata360") from a first sample, whatever the
// schema: the clip header is the same in every one. Empty when absent.
std::string DjiProductName(const std::vector<uint8_t>& sample);

// A ColorModeType's name ("D-Log M"), "" for one we don't name.
std::string DjiColorModeName(uint32_t mode);
// The log profile a ColorModeType is, by DJI's name ("D-Log M"); "" when it
// isn't one.
std::string DjiLogProfile(uint32_t mode);

// What one sample says about its frame (FrameMeta, field 3).
struct DjiFrameMeta {
    uint64_t timestampUs = 0; // 3.1.2, device us
    // The drone's part (3.4), where the sample has it: it changes at 10 Hz
    // on the Avata 360.
    bool hasDrone = false;
    bool hasPosition = false;
    double latitude = 0, longitude = 0; // 3.4.4.1.2 / .3, degrees
    double altitudeM = 0;               // 3.4.4.2, mm in the file
    // 3.4.2, m/s in NED: north, east, down.
    double velocityN = 0, velocityE = 0, velocityD = 0;
    // 3.4.3, degrees, NED.
    double pitchDeg = 0, rollDeg = 0, yawDeg = 0;
    // The camera's fused attitude over the frame (3.3.2.1, current_frame):
    // the time of its first sample, device us, and the samples, w x y z,
    // IMU -> world (docs/formats/dji-360.md §4). 40 at 4 kHz on the Avata
    // 360. Empty where the sample has none.
    uint64_t attitudeUs = 0;
    std::vector<std::array<float, 4>> attitude;
};
std::optional<DjiFrameMeta> ParseDjiFrameMeta(const std::vector<uint8_t>& sample);

// The file's own name as the camera gave it, from the card path DJI writes
// into the MP4 header (`moov` / `udta` / `fsid`,
// `/mnt/…/DCIM/DJI_001/DJI_…_D_2026.OSV`; the .OSV and its .LRF each have
// their own). Just the name. It survives a rename, so a renamed clip finds
// its proxy and recording time by it. std::nullopt when the file holds none.
std::optional<std::string> ReadDjiHeldName(const std::string& file_path);

} // namespace orbiscore
