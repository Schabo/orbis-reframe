#pragma once

#include "ClipMetadata.h"

#include <InsvDemux.h> // insv::InsvStreamInfo

#include <optional>
#include <string>
#include <vector>

// A camera family's reader: knows its files and fills the common clip
// description (ClipMetadata.h). One per family, picked by asking each in turn;
// the per-model differences within a family are its own data.

namespace camera {

class CameraReader {
public:
    virtual ~CameraReader() = default;

    enum class Result {
        NotMine, // not this family's file: the next reader is asked
        Read,
        Failed,  // this family's file, but unusable (`err` says why)
    };

    // Fills `out` from the file at `out.path`, whose video streams are
    // `streams` (in file order; `out`'s stream facts are already set from
    // them).
    virtual Result read(const std::vector<insv::InsvStreamInfo>& streams, ClipMetadata& out,
                        std::string& err) const = 0;

    virtual std::optional<insv::ImuTrack> readImu(const ClipMetadata& clip) const = 0;
    // The GPS track lined up with the video; nullopt when the clip has none
    // or the file doesn't say how it lines up.
    virtual std::optional<insv::GpsTrack> readGps(const ClipMetadata& clip) const = 0;
    // By its name alone: the clip file `path` would belong to if it is
    // another file of a take (a lens's own file, the camera's proxy);
    // nullopt for any other name. ClipFileOf checks the answer.
    virtual std::optional<std::string> clipFileFor(const std::string& path) const = 0;
    // The camera's own preview from the file, without decoding the video
    // (the media pool asks for many at once): stored ready (the X5), or made
    // from the lens pictures the file keeps (the ONE X2: a small stitch,
    // tens of ms). nullopt when it isn't this family's or has none.
    virtual std::optional<Thumbnail> readThumbnail(const std::string& path) const = 0;
};

} // namespace camera
