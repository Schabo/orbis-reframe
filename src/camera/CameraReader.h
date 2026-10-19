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
    // Straight from the file, without reading the rest (the media pool asks
    // for many at once); nullopt when it isn't this family's or has none.
    virtual std::optional<Thumbnail> readThumbnail(const std::string& path) const = 0;
};

} // namespace camera
