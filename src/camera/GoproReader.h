#pragma once

#include "CameraReader.h"

// GoPro's 360 cameras (the MAX; docs/formats/gopro-max.md): a QuickTime-ish
// file with the sphere already dewarped by the camera and written as an
// equi-angular cubemap -- two HEVC streams of three cube faces each, the
// second stream not adjacent to the first -- plus the IMU as GPMF samples in
// a `gpmd` data track (orbis-core/gopro/Gpmf.h) and a .THM JPEG preview and
// .LRV proxy beside it. The cube->sphere assembly is the stitch's job, not
// the reader's; what differs per product is this reader's table, and only
// the MAX is measured so far.

namespace camera {

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
