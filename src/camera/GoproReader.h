#pragma once

#include "CameraReader.h"

// GoPro's 360 cameras (the MAX; docs/formats/gopro-max.md): a QuickTime-ish
// file with the sphere already dewarped by the camera and written as an
// equi-angular cubemap -- two HEVC streams of three cube faces each, the
// second stream not adjacent to the first -- plus the IMU as GPMF samples in
// a `gpmd` data track (orbis-core/gopro/Gpmf.h) and a .THM JPEG preview and
// .LRV proxy beside it. The cube->sphere assembly is the stitch's job, not
// the reader's; what differs per model and mode is this reader's table
// (GoproMode), and only the MAX's 5.6K is measured so far.

namespace camera {

// What the reader knows per camera model and recording mode, measured on
// that mode's clips. The cube layout is used only when the file doesn't
// state it (PMOD); the IMU axes and the picture's lag aren't in the file.
struct GoproMode {
    const char* model;           // as the file names it, "GoPro Max"
    int width, height;           // each cube stream's size
    const char* name;            // the camera's name for it (ClipMetadata::captureMode)
    orbiscore::EacLayout layout; // ClipMetadata::cubeLayout
    const char* imuAxes;         // orbiscore::ImuTrack::axisMap
    double pictureLagSec;        // the picture's motion after the frame times
};

// The row for `model` (any case) at `width` x `height`; one with the MAX's
// IMU axes and lag and no layout or name when nothing was measured there
// (`known` false).
const GoproMode& FindGoproMode(const std::string& model, int width, int height,
                               bool* known = nullptr);

// The cube layout the global settings' PMOD states for `width` x `height`
// strips, if they can hold it with the centre face square (as measured).
std::optional<orbiscore::EacLayout> LayoutFromPmod(const std::vector<int64_t>& pmod, int width,
                                                   int height);

class GoproReader : public CameraReader {
public:
    Result read(const std::vector<orbiscore::InsvStreamInfo>& streams, ClipMetadata& out,
                std::string& err) const override;
    std::optional<orbiscore::ImuTrack> readImu(const ClipMetadata& clip) const override;
    std::optional<orbiscore::GpsTrack> readGps(const ClipMetadata& clip) const override;
    std::optional<Thumbnail> readThumbnail(const std::string& path) const override;
    std::optional<std::string> clipFileFor(const std::string& path) const override;
};

} // namespace camera
