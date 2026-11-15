#pragma once

#include "CameraReader.h"

// DJI's 360 cameras' .OSV files (docs/formats/dji-360.md): an MP4 with each
// lens as a video stream of its own, the master lens's first on the Avata
// 360, and the metadata -- the lens calibration, the colour mode, the
// drone's GPS -- as protobuf samples in a `djmd` data track, one per frame.
// Beside the clip the camera writes a .LRF proxy (both lenses side by side,
// a quarter of the metadata) and a .SRT; the reader needs neither. What differs per product is this reader's
// table; only the Avata 360 is in it so far.

namespace camera {

class DjiReader : public CameraReader {
public:
    Result read(const std::vector<insv::InsvStreamInfo>& streams, ClipMetadata& out,
                std::string& err) const override;
    std::optional<insv::ImuTrack> readImu(const ClipMetadata& clip) const override;
    std::optional<insv::GpsTrack> readGps(const ClipMetadata& clip) const override;
    std::optional<Thumbnail> readThumbnail(const std::string& path) const override;
    std::optional<std::string> clipFileFor(const std::string& path) const override;
};

} // namespace camera
