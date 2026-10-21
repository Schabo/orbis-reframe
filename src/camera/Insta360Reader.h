#pragma once

#include "CameraReader.h"

// Insta360's .insv files (docs/formats/insta360-x5.md): the trailer after the
// MP4 `moov` box carries the model, the lens calibration, the IMU and the
// preview picture. One file holds both lenses as video streams 0 and 1 (the
// X5, X6), or -- the ONE X2 at 5.7K -- that file holds lens 0 and a file
// beside it lens 1. What differs per model is insv::InsvModel's table.

namespace camera {

class Insta360Reader : public CameraReader {
public:
    Result read(const std::vector<insv::InsvStreamInfo>& streams, ClipMetadata& out,
                std::string& err) const override;
    std::optional<insv::ImuTrack> readImu(const ClipMetadata& clip) const override;
    std::optional<Thumbnail> readThumbnail(const std::string& path) const override;
    std::optional<std::string> clipFileFor(const std::string& path) const override;
};

} // namespace camera
