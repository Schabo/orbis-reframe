#pragma once

#include "CameraReader.h"

// Insta360's .insv files (docs/formats/insta360-x5.md): the trailer after the
// MP4 `moov` box carries the model, the lens calibration, the IMU and the
// preview picture; one file holds both lenses as video streams 0 and 1. What
// differs per model is insv::InsvModel's table.

namespace camera {

class Insta360Reader : public CameraReader {
public:
    Result read(const std::vector<insv::InsvStreamInfo>& streams, ClipMetadata& out,
                std::string& err) const override;
    std::optional<insv::ImuTrack> readImu(const ClipMetadata& clip) const override;
    std::optional<Thumbnail> readThumbnail(const std::string& path) const override;
};

} // namespace camera
