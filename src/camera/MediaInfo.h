#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Everything the Media information panel shows about a source file: container
// and stream facts from FFmpeg, camera and telemetry facts from the clip's
// description (ClipMetadata.h). No decode, no stitch.

namespace camera {

struct MediaVideoStream {
    std::string codec;   // "hevc"
    std::string profile; // "Main"
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int64_t bitRate = 0; // bits/s, 0 if unknown
    std::string pixelFormat; // "yuvj420p"
    int bitDepth = 0;
    std::string chroma; // "4:2:0"
    bool fullRange = false;
    std::string colorSpace; // "bt709"
    std::string transfer;   // "bt709", "arib-std-b67" (HLG); "" when untagged
    int64_t frameCount = 0;
};

struct MediaAudioStream {
    std::string codec;   // "aac"
    std::string profile; // "LC"
    int sampleRate = 0;
    int channels = 0;
    std::string channelLayout; // "stereo"
    int64_t bitRate = 0;
};

struct MediaInfo {
    std::string path;
    int64_t fileBytes = 0;
    std::string container; // "mov,mp4,m4a,3gp,3g2,mj2"
    int64_t durationMs = 0;
    int64_t totalBitRate = 0;
    std::string creationTime; // ISO 8601 from the container, "" if absent
    // See ClipMetadata::recordedLocal; "" for a file no camera reader knows.
    std::string recordedLocal;

    std::vector<MediaVideoStream> video; // a dual-fisheye clip: one per lens
    std::vector<MediaAudioStream> audio;

    // From the camera's metadata (ClipMetadata); empty / 0 for a file no
    // camera reader knows, or one that doesn't record them.
    std::string cameraModel;
    std::string serialNumber;
    std::string firmware;
    double imuRateHz = 0.0; // 0 if the file has no IMU track
    int64_t exposureRecords = 0;
    double rollingShutterMs = 0.0;
    // See ClipMetadata::colorProfile / logProfile.
    std::string colorProfile;
    std::string logProfile;
    // The HDR format the first video stream is tagged with: "HLG", "PQ", or
    // "" for SDR. Like a log clip, it looks wrong as SDR until a LUT converts it.
    std::string hdrFormat;
    // GPS track: presence only, never positions -- the panel is what people
    // screenshot for bug reports.
    int64_t gpsRecords = 0; // 0 if the file has no GPS track
    int64_t gpsFixes = 0;   // records with an acquired position
    double gpsRateHz = 0.0; // position updates per second
    double gpsTrackSec = 0.0; // seconds of track, gaps left out

    // The camera's low-res proxy next to the clip.
    std::string proxyPath; // "" if not found
    // The take's other lens files (a ONE X2 clip's VID_…_10_…); empty when
    // the clip is one file.
    std::vector<std::string> lensFiles;

    // The camera's name for the resolution it was filmed at ("8K"), or "".
    std::string captureMode;
};

// Returns false with `err` set if the file can't be opened as media.
bool ProbeMediaInfo(const std::string& path, MediaInfo& out, std::string& err);

} // namespace camera
