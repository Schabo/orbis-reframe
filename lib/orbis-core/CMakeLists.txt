# orbis-core: the camera-file and sphere library orbis-reframe edits on --
# file metadata and telemetry, lens models, the stitch's seam work, the
# stabilizer, demux / decode and audio. No Qt and no editor code. Sources are
# under orbis-core/, one folder per area and one per file format (insta360/,
# dji/); users include them as <orbis-core/stitch/SeamFlow.h>.
#
# Built from the app's CMakeLists.txt with add_subdirectory(); it relies on
# what the app finds there: Threads, Vulkan, FFmpeg (FFMPEG_FOUND,
# PkgConfig::FFMPEG) and OpenCV (OpenCV_FOUND, ORBIS_OPENCV_FEATURES). It
# links none of the app's targets.
#
# Users link the orbis-core:: aliases (orbis-core::orbiscore, ::metadata,
# ::media, ::optical), the names an installed package will export.

# --- orbiscore_metadata: file metadata and telemetry -- the .insv trailer
# (calibration + IMU / exposure / GPS records), DJI's djmd track, the IMU
# and GPS types. No FFmpeg / Qt dependency. Its own tiny lib (rather than
# folded into `orbiscore`) so targets that only need trailer parsing don't pull
# the rest of the library.
add_library(orbiscore_metadata STATIC
    orbis-core/metadata/Gps.h
    orbis-core/metadata/Gps.cpp
    orbis-core/metadata/Imu.h
    orbis-core/metadata/Imu.cpp
    orbis-core/QuatMatrix.h
    orbis-core/Smooth.h
    orbis-core/insta360/InsvTrailer.h
    orbis-core/insta360/InsvTrailer.cpp
    # The protobuf wire format, which Insta360's records and DJI's djmd
    # track are written in.
    orbis-core/Protobuf.h
    orbis-core/Protobuf.cpp
    # DJI's djmd metadata samples (docs/formats/dji-360.md).
    orbis-core/dji/DjiMeta.h
    orbis-core/dji/DjiMeta.cpp
    # Lining the trailer's IMU up with the video, and what differs per model.
    orbis-core/insta360/InsvImu.h
    orbis-core/insta360/InsvImu.cpp
    orbis-core/insta360/InsvModels.h
    orbis-core/insta360/InsvModels.cpp
)
target_include_directories(orbiscore_metadata PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_library(orbis-core::metadata ALIAS orbiscore_metadata)

add_executable(insv_trailer_dump tools/insv_trailer_dump/main.cpp)
target_link_libraries(insv_trailer_dump PRIVATE orbiscore_metadata)

# --- orbiscore: the core. GyroStabilizer + the stitch core: calibration-string
# parse and the CPU fisheye->equirect projection. No Qt / Vulkan / FFmpeg --
# consumes only the raw calibration string via orbiscore_metadata. The app's
# project model links this for GyroStabilizer, so it must stay
# dependency-free.
add_library(orbiscore STATIC
    orbis-core/stitch/BlurQuads.h
    orbis-core/stitch/BlurQuads.cpp
    orbis-core/metadata/FramePts.h
    orbis-core/stab/GyroStabilizer.h
    orbis-core/stab/GyroStabilizer.cpp
    orbis-core/Image.h
    orbis-core/stab/Motion.h
    orbis-core/stab/Motion.cpp
    orbis-core/insta360/Calibration.h
    orbis-core/insta360/Calibration.cpp
    orbis-core/stitch/LensSource.h
    orbis-core/stitch/FisheyeProjection.h
    orbis-core/stitch/FisheyeProjection.cpp
    orbis-core/stitch/SeamAlignment.h
    orbis-core/stitch/SeamCorrection.h
    orbis-core/stitch/SeamAlignment.cpp
    orbis-core/stitch/SeamDepth.h
    orbis-core/stitch/SeamDepth.cpp
    orbis-core/stitch/SeamCut.h
    orbis-core/stitch/SeamCut.cpp
    orbis-core/stitch/SeamWindow.h
    orbis-core/stitch/SeamWindow.cpp
    orbis-core/stitch/LensColour.h
    orbis-core/stitch/LensColour.cpp
    orbis-core/stitch/SeamColourBlend.h
    orbis-core/stitch/SeamColourBlend.cpp
    orbis-core/stitch/SeamFade.h
    orbis-core/stitch/SeamFade.cpp
    orbis-core/stitch/SeamFlow.h
    orbis-core/stitch/SeamFlow.cpp
    orbis-core/Ycbcr.h
    orbis-core/Ycbcr.cpp
)
target_include_directories(orbiscore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orbiscore PUBLIC orbiscore_metadata Threads::Threads)
add_library(orbis-core::orbiscore ALIAS orbiscore)

# --- orbiscore_media: the FFmpeg-dependent piece (decode a fisheye frame from a
# clip at time T, and the audio). Separate lib so the projection/gyro core
# above stays dependency-free. No Qt.
if(FFMPEG_FOUND)
    add_library(orbiscore_media STATIC
        orbis-core/media/InsvDemux.h
        orbis-core/media/InsvDemux.cpp
        # The demux / decode / seek core of InsvDemux and VulkanVideoDecoder.
        orbis-core/media/LensDemuxer.h
        orbis-core/media/LensDemuxer.cpp
        # One picture from an elementary stream (a camera's preview frame).
        orbis-core/media/StillPicture.h
        orbis-core/media/StillPicture.cpp
        # GPU-resident decode via FFmpeg's Vulkan hwaccel.
        orbis-core/media/VulkanVideoDecoder.h
        orbis-core/media/VulkanVideoDecoder.cpp
        # Audio playback: its own file handle, independent
        # of the fisheye decode above -- no stitching involved.
        orbis-core/audio/InsvAudioDemux.h
        orbis-core/audio/InsvAudioDemux.cpp
        orbis-core/audio/AudioPart.h
        orbis-core/audio/ClipAudioReader.h
        orbis-core/audio/ClipAudioReader.cpp
        orbis-core/audio/AudioMixer.h
        orbis-core/audio/AudioMixer.cpp
        # Audio noise reduction, per mic clip: "MC: Wind reduction"
        # (WindReducer), "Wind reduction" (GeneralWindReducer) and
        # "Hiss / hum" (HissHumReducer); the voice modes (VoiceIsolator)
        # below, with ONNX Runtime.
        orbis-core/audio/NoiseReduction.h
        orbis-core/audio/NoiseFilter.h
        orbis-core/audio/NoiseFilter.cpp
        orbis-core/audio/Rdft.h
        orbis-core/audio/StftStream.h
        orbis-core/audio/StftStream.cpp
        orbis-core/audio/WindReducer.h
        orbis-core/audio/WindReducer.cpp
        orbis-core/audio/GeneralWindReducer.h
        orbis-core/audio/GeneralWindReducer.cpp
        orbis-core/audio/HissHumReducer.h
        orbis-core/audio/HissHumReducer.cpp
        orbis-core/audio/AudioPeaks.h
        orbis-core/audio/AudioPeaks.cpp
    )
    target_include_directories(orbiscore_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(orbiscore_media PUBLIC orbiscore Vulkan::Vulkan PRIVATE PkgConfig::FFMPEG)
    add_library(orbis-core::media ALIAS orbiscore_media)

    # The "Voice only" / "Voice + ambience" noise modes: DeepFilterNet 3
    # (resources/models/deepfilternet3) run with ONNX Runtime -- optional
    # (libonnxruntime-dev); without it those modes are hidden. Only its
    # headers are needed to build: VoiceIsolator dlopen()s the library on
    # first use (so the app runs without it, and Debian's build's load-time
    # log spam can be silenced). ONNXRUNTIME_ROOT points at an unpacked copy
    # instead of the system one.
    find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
        HINTS ${ONNXRUNTIME_ROOT}/include PATH_SUFFIXES onnxruntime)
    find_library(ONNXRUNTIME_LIBRARY onnxruntime
        HINTS ${ONNXRUNTIME_ROOT}/lib ${ONNXRUNTIME_ROOT}/lib/x86_64-linux-gnu)
    if(ONNXRUNTIME_INCLUDE_DIR)
        message(STATUS "ONNX Runtime headers: ${ONNXRUNTIME_INCLUDE_DIR} -- voice noise modes on")
        target_sources(orbiscore_media PRIVATE orbis-core/audio/VoiceIsolator.h orbis-core/audio/VoiceIsolator.cpp)
        target_include_directories(orbiscore_media PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
        target_link_libraries(orbiscore_media PRIVATE ${CMAKE_DL_LIBS})
        target_compile_definitions(orbiscore_media PUBLIC ORBISCORE_HAVE_VOICE)
        # The headers' ORT_API_VERSION is the runtime's 1.x minor version,
        # after which Debian names it: libonnxruntime1.23, .so.1.23 (Debian
        # 13: 1.21). The .deb's Depends and the "install it" hint use it,
        # hence PARENT_SCOPE.
        file(STRINGS ${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_c_api.h ORT_API_LINE
            REGEX "^#define ORT_API_VERSION [0-9]+")
        string(REGEX MATCH "[0-9]+$" ORT_API_MINOR "${ORT_API_LINE}")
        set(ORBIS_ORT_PACKAGE libonnxruntime1.${ORT_API_MINOR})
        set(ORBIS_ORT_PACKAGE ${ORBIS_ORT_PACKAGE} PARENT_SCOPE)
        target_compile_definitions(orbiscore_media PUBLIC ORBISCORE_ORT_PACKAGE="${ORBIS_ORT_PACKAGE}")
        # The library found here is tried first (an ONNXRUNTIME_ROOT copy
        # isn't on the loader's path). A bundle that carries its own (the
        # AppImage) turns it off, so the bundled one loads, not the build
        # machine's path on the user's system.
        option(ORBIS_ORT_BUILD_PATH
            "Try the ONNX Runtime library found at build time before the loader's search" ON)
        if(ONNXRUNTIME_LIBRARY AND ORBIS_ORT_BUILD_PATH)
            target_compile_definitions(orbiscore_media PRIVATE
                ORBISCORE_ORT_LIBRARY="${ONNXRUNTIME_LIBRARY}")
        endif()
    else()
        message(STATUS "ONNX Runtime not found (libonnxruntime-dev) -- voice noise modes off")
    endif()

    # --- tools/orbis_stitch: smoke CLI. Turns an .insv into an equirect
    # through this library alone -- the standalone proof that it builds and
    # works without the app. Links orbiscore + orbiscore_media +
    # orbiscore_metadata, nothing else.
    add_executable(orbis_stitch tools/orbis_stitch/main.cpp)
    target_link_libraries(orbis_stitch PRIVATE orbiscore orbiscore_media orbiscore_metadata)
endif()

# --- orbiscore_optical: the OpenCV-dependent piece -- residual 2-D
# stabilization (ResidualPanEstimator: feature tracking + optical flow -> a
# per-frame yaw/pitch correction), the gyro sync's fine-tune and the CPU
# seam flow. Separate lib so the core stays OpenCV-free; the apply side
# (ResidualTrack::sample) is header-only in ResidualStab.h.
if(OpenCV_FOUND)
    add_library(orbiscore_optical STATIC
        orbis-core/stab/ResidualStab.h
        orbis-core/stab/ResidualPanEstimator.h
        orbis-core/stab/ResidualPanEstimator.cpp
        orbis-core/stab/SyncEstimator.h
        orbis-core/stab/SyncEstimator.cpp
        orbis-core/stitch/SeamFlowEstimator.h
        orbis-core/stitch/SeamFlowEstimator.cpp
    )
    target_include_directories(orbiscore_optical PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(orbiscore_optical PUBLIC orbiscore
        PRIVATE opencv_core opencv_imgproc opencv_video ${ORBIS_OPENCV_FEATURES})
    add_library(orbis-core::optical ALIAS orbiscore_optical)
endif()

# --- tests/orbiscore_test: the stitch core (calibration parse, radial model,
# linear algebra, FisheyeStitcher geometry, the trailer thumbnail). All
# synthetic -- links only the dependency-free `orbiscore` and
# `orbiscore_metadata` libs, no Qt / FFmpeg / .insv file needed.
add_executable(orbiscore_test tests/orbiscore_test.cpp)
target_link_libraries(orbiscore_test PRIVATE orbiscore orbiscore_metadata)
add_test(NAME orbiscore_test COMMAND orbiscore_test)
