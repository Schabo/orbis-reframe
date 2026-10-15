# GoPro MAX and MAX2 file format — `.360`

**Desk research (2026-10-15), no `.360` file examined.** From GoPro's own
open-source GPMF documentation, community write-ups of the `.360` layout,
an FFmpeg filter patch and retailer specs. Markers as in
[`README.md`](README.md).

GoPro's 360 cameras are the **MAX** (2019) and the **MAX2** (2025). Unlike
Insta360 and DJI, they **stitch in the camera**: a `.360` file holds an
already-projected sphere, not two fisheye pictures. GoPro's HERO cameras are
flat (`.MP4`) and not covered here.

## 1. Streams

- 📖 MP4 container with **two HEVC video tracks**, each a strip of three
  cube faces in an **equi-angular cubemap (EAC)**; together they're the two
  rows of a 3×2 cubemap. Usually streams 0 and 5 (0 and 4 in TimeWarp /
  timelapse modes), so find them with ffprobe rather than by index.
- 📖 MAX, 5.6K mode: each track **4096×1344** (cube face 1344). Two faces
  per track are "pure"; the other four (left, right, up, down) are made of
  both lenses and have **not been blended**: each carries a ~64 px overlap
  (32 px each side of the face's centre line) that the reader has to
  cross-fade. Hence 4096 = 3 × 1344 + 64.
- 📖 Face layout (from the FFmpeg `gopromax_opencl` filter's code): the first
  track (top row) holds right, front, left; the second (bottom row) down,
  back, up, with back rotated 90° and up / down 270°.
- ⚠️ MAX2, 8K mode: tracks reported as **5952×1920** (one community source),
  stitched to 7680×3840; that fits 3 × 1920 + 192, i.e. a wider overlap or
  a different packing. ❓ Unconfirmed.
- 📖 MAX2 360 modes: 8K 30 / 25 / 24, 5.6K 60 / 50 / 30 / 25, 4K 100 / 90 /
  60 / 50; H.265; **10-bit** and **GP-Log** (with GoPro's LUT); up to
  120 Mb/s by default (300 Mb/s with GoPro Labs firmware).
- 📖 MAX: 5.6K 30 and 3K 60. ⚠️ 8-bit (no 10-bit mode is advertised).
- ❓ Audio streams (the MAX has six mics for 360 audio), proxies.

## 2. Telemetry — GPMF

- 📖 GPMF is GoPro's own format, **documented and open-sourced by GoPro**
  (`gpmf-parser`, Apache-2.0 or MIT): a timed `meta` track named
  "GoPro MET" with a `gpmd` sample description.
- 📖 MAX adds to the HERO streams: **CORI** (camera orientation quaternions
  since capture start), **IORI** (image orientation relative to the body),
  **GRAV** (gravity vector), **DISP** (a 1-D disparity map between the two
  lenses, 360 modes), **MAGN** (magnetometer). The header's `PRJT` (lens
  projection) is `GPRO` or `EACO` in 360 modes.
- 📖 MAX2: all HERO13 streams, plus raw magnetometer, **MNOR** (fused
  magnetic north) and its confidence.
- ❓ Whether GYRO / ACCL are present on the MAX (the documentation implies
  it via "supports all HERO metadata" without saying so).
- ❓ Whether the picture is already stabilized in the camera (then CORI and
  IORI say how), which changes what our gyro stabilization should do.

## 3. What reading it would take

- No stitch and no lens calibration: assemble the 3×2 EAC cubemap from the
  two tracks, blend the overlap bands, and sample it as the sphere -- a new
  input projection in the GPU path beside the fisheye stitch.
- The 10-bit / log path for the MAX2 (as for the Insta360 X6 and DJI).
- A GPMF reader for orientation and gravity; the quaternions and gravity
  vector may make horizon leveling simpler than on the X5.

## 4. Sources

- GoPro `gpmf-parser` (GPMF format, MAX / MAX2 streams):
  <https://github.com/gopro/gpmf-parser>
- FFmpeg-devel, the `gopromax_opencl` filter patch (layout, overlap,
  face order): <https://ffmpeg.org/pipermail/ffmpeg-devel/2024-July/331814.html>
- Paul Bourke, GoPro MAX movie layout and MAX2sphere:
  <https://paulbourke.net/panorama/gopromax2sphere>
- Trek View, reverse engineering the GoPro 360 file format (part 2: track
  sizes) <https://www.trekview.org/blog/reverse-engineering-gopro-360-file-format-part-2/>
  and processing with ffmpeg (stream indices)
  <https://www.trekview.org/blog/using-ffmpeg-process-gopro-max-360/>
- Mapillary forum, MAX2 `.360` support (5952×1920 tracks):
  <https://forum.mapillary.com/t/new-seamless-support-for-360-video-files-gopro-max2/10119>
- Videomaker, GoPro MAX2 announcement (10-bit, GP-Log):
  <https://www.videomaker.com/news/gopro-announces-max2-8k-360-camera/>
- Cathay Photo, MAX2 specs (modes, codec, bitrate):
  <https://www.cathayphoto.com.sg/cameras/action-cameras/gopro-max-2>
