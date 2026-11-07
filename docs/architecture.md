# Architecture

How orbis-reframe is put together, the decisions that bound all work on it,
and the rules a change has to keep. For what's planned next and how it is
meant to work, see [`roadmap.md`](roadmap.md); for the camera files,
[`formats/`](formats/README.md).

## Standing decisions

These hold for any new work. A change that needs one of them changed is a
discussion in an issue first, not a pull request.

- **Stack**: C++20, Qt 6 Widgets (not QML), CMake on the distribution's own
  packages. Vulkan for rendering, the compute stitch and video decode; no
  CUDA. FFmpeg for demux, decode and encode. A custom timeline engine, not
  MLT. A standalone app, not a plugin or library.
- **Neural networks run in-process**: ncnn on Vulkan for detection (number
  plates), OpenCV for Track subject (CSRT + VitTrack), ONNX Runtime on the
  CPU for DeepFilterNet. No CUDA, no ONNX Runtime GPU.
- **`src/insv/` stays independent**: no Qt, editor or Vulkan-loader
  dependency, licensed MIT OR Apache-2.0, so it can become its own library.
- **Where format knowledge comes from**: files the cameras recorded, public
  documentation, published research, and format facts other projects
  published (cited and checked against real files). Never disassembling a
  vendor's software. Vendor tuning (seam widths, default curves, ...) is
  measured, not copied. Details: [`formats/README.md`](formats/README.md).
  No closed-source or EULA-bound dependency anywhere.
- **GPUs**: NVIDIA (Vulkan video decode + NVENC) is what is tested and
  released on. AMD and Intel run on Mesa (RADV / ANV, VA-API or Vulkan video
  encode) and are best-effort: fixed when someone reports a problem, not
  tested by the maintainer.
- **Wayland only**: Sway is the bar, GNOME a secondary check; no X11 /
  XWayland. Prefer standard Wayland protocols over compositor-specific
  behaviour, but don't take on real complexity for other compositors.
- **Local only**: editing, playback and export never touch the network. The
  one planned exception is an opt-in royalty-free music library panel.
- **Numbers in text for machines are locale-free**: `main()` sets
  `LC_NUMERIC` back to "C" right after `QApplication` has applied the
  user's locale, so the C library, FFmpeg and the other libraries read and
  write `.` decimals on every desktop. Code that parses or writes numbers
  in files, FFmpeg option strings and the like uses `std::from_chars` /
  `std::to_chars` or `QString::number` anyway, so it holds in the tools,
  the tests and any other host. Numbers shown to people go through
  `QLocale`, which still follows the user's language.
- **Export is a flat reframe**: the product is the flat video you steer
  through the sphere. Full 360 (equirect) export is ruled out.
- **Colour conversion is the clip's LUT**: log and HDR footage is converted
  by a `.cube` LUT in the Colour panel, which the user picks, adjusts
  (strength) or removes. The stitch and decode deliver the camera's picture
  as recorded; they don't convert transfer curves themselves.
- **Releases**: one build per target on its own system libraries (Debian
  Testing, Debian 13, Ubuntu 26.04), as a tarball and a `.deb`; beside them
  an AppImage for every distribution with glibc 2.41 or newer, built on
  Debian 13 with the libraries that differ between distributions bundled
  (Qt, OpenCV and ONNX Runtime from Debian 13, FFmpeg our own LGPL build;
  the drivers, Vulkan loader, GL / EGL and fonts from the system). Every
  bundled library is mapped to its package and checked against a reviewed
  licence list at build time, and a weekly job fails when one has a
  security update, which then ships as a patch release
  (`docs/dev-environment.md`, "The AppImage"). No Flatpak.

## Modules

| Folder | What it is | Depends on |
|---|---|---|
| `src/insv/` | The camera-file library: `.insv` trailer, calibration, IMU timing, the Insta360 per-model table, demux, Vulkan video decode, the fisheye model, gyro stabilization, seam measurement, audio filters | std C++, FFmpeg (demux / decode parts) -- no Qt |
| `src/camera/` | The common clip description (`ClipMetadata`, read once per file and cached), a reader per camera family that fills it, and the Media information probe | `insv`, FFmpeg -- no Qt, no Vulkan |
| `src/core/` | The project model: media pool, timeline, clips, keyframe tracks, titles, gauges, transitions, the colour grade, the `.orp` file, undo, autosave | Qt Core, nlohmann-json, zlib (LUT zips) -- no widgets, no Vulkan |
| `src/stitch/` | `IStitchSource` ("the frame at time T of clip X"), `NativeStitchSource` (dual fisheye), clip probing, clip sync, per-clip seam | `camera`, `insv`, `render` |
| `src/render/` | Vulkan: the fisheye stitch, the viewport renderer and its shaders, LUTs on the GPU | Vulkan |
| `src/export/` | Export: the CPU reproject (`ReframeRenderer`), flattening the timeline into the encoder, flat-video reading, inset compositing | FFmpeg |
| `src/privacy/` | Number-plate scan, tracking and blur regions | `insv`, ncnn |
| `src/track/` | Track subject | OpenCV |
| `src/calib/` | Lens calibration from a clip of the chart: ChArUco corners in both lenses, the stitch's lens model fitted to them | `camera`, `insv`, OpenCV (objdetect) |
| `src/ui/` | The Qt Widgets app: main window, panels, timeline widget, playback, background jobs | everything above |

`tools/` holds command-line tools that link the same libraries, for checking
a change on real clips without the GUI ([`tools/README.md`](../tools/README.md)).

## The picture's path

1. **Demux and decode** (`insv/InsvDemux`, `insv/VulkanVideoDecoder`): the
   two fisheye lens streams, decoded on the GPU through FFmpeg's Vulkan
   hwaccel into NV12 (8-bit) or P010 (10-bit) images, with a CPU decode
   fallback when the GPU path can't be used.
2. **Stitch** (`render/VulkanFisheyeStitcher`, `fisheye_stitch*.comp`): the
   lens calibration from the file (or the clip's own, made with Calibrate
   lenses, which has the guard in it), the lens-guard profile and the
   clip's stitcher turn the two fisheyes into the sphere. Privacy blur
   regions are applied here, on the sphere. The stitcher is chosen per
   clip (`stitch::PreviewStitchConfig::Method`): the new stitcher, a new
   clip's default, which takes no per-clip measurement; or the old one,
   with the depth-aware seam or a fixed one, both with the clip's measured
   seam alignment and lens colour -- what a project file that names no
   stitcher loads as, `Project::kOlderFileStitchMethod` (see
   [the roadmap](roadmap.md)); the new stitcher's seam colour blend (`insv/SeamColourBlend`)
   matches the lenses' colour per stretch of the seam from what they see in
   the overlap over each frame and the quarter second before it, and its seam
   flow (`insv/SeamFlow`) warps each lens near the seam by its share of
   the optical flow between them in the overlap, averaged over the frame
   and the four before it -- a texture beside the stitch's parameters on
   every GPU path, the viewport's lens frames included. On the GPU the
   strips, the flow (DIS, `render/shaders/seam_flow.comp`) and the average
   stay there, in the frame's measuring submission; the CPU stitch
   fallback estimates it with OpenCV's DIS (`insv/SeamFlowEstimator`).
   Both run on the strips anchored first: bright featureless parts get the
   same fixed pattern in both lenses, so the flow stays zero there
   (`insv::AnchorSeamFlowStrips`, the shader's anchor passes). With both
   on, the lenses meet in a narrow fade on the seam, widened per stretch
   where they still differ near the seam after the flow (`insv/SeamFade`):
   each frame's patch means of the warped raw strips are kept with its
   flow, and the share of the band per strip column is solved with the
   frame's colour table into the flow texture's last row -- on the GPU in
   the stitch's own submission (`recordSeamFade`), since the colour comes
   back to the CPU only after the measuring one. A seek measures the
   frames all three need before the frame it goes to, so they need no analysis.
   The depth-aware seam's state
   (the distance per stretch of the seam, the cut across the overlap, the
   colour correction there) is a function of the measurements of the frames
   either side of a frame (`insv/SeamWindow`), worked out once per clip in a
   background pass and kept in the project's cache
   (`stitch/ClipSeamAnalysis`): every path that stitches a frame --
   playback, stepping, screenshots, export -- stitches it the same way.
   Until a clip is analysed, playback measures the seam as it plays, from
   the frames so far; an export analyses what it needs first.
3. **View**: a virtual camera (yaw, pitch, roll, field of view and
   distortion from the clip's reframe keyframes, plus the stabilizer's
   per-frame rotation) looks into the sphere and produces the flat picture.
   For speed, the viewport and the GPU export usually go straight from the
   fisheyes to the view (`fisheye_view.frag`, `fisheye_view_nv12.comp`)
   instead of building an equirect first.
4. **Grade**: the clip's LUT and the Colour panel's adjustments
   (`color::Grade`), applied to the flat picture.
5. **Compose and encode**: overlays, titles, gauges and transitions on top, then the
   encoder (NVENC, else VA-API, else Vulkan encode). The export is 8-bit
   NV12, or P010 into HEVC Main 10 when the project asks for 10-bit
   (`OutputSettings::tenBit`); everything up to this point works in float,
   so the fused GPU pass's 10-bit output is real 10-bit. The CPU path and
   the equirect-first GPU path stay 8-bit.

### One view, several implementations

The same view math exists in several places, and they must agree:

- the viewport: `src/render/shaders/equirect.frag` and `fisheye_view.frag`;
- the GPU export: `fisheye_view_nv12.comp` and `reframe_reproject.comp`;
- the CPU export: `src/export/ReframeRenderer`;
- the subject tracker's crop (`src/track/`).

The projection itself -- a view's pose (`view::Pose`: yaw, pitch, roll,
the diagonal field of view, the distortion or Auto), what it renders with
at an aspect (`view::lens`: the vertical angle and the distortion), and how
its pixels become rays and back -- is written once, in
`src/core/ViewProjection.h` and its GLSL twin
`src/render/shaders/view_projection.glsl`; every implementation above, and
viewport picking and overlays, goes through them, each turning the pose
into a lens at the aspect it draws. Keep the two in step.

A split clip's picture is several views, one per pane: where each pane
goes, and so its aspect, comes from `pane::rects` (`src/core/PaneLayout.h`)
in every one of them, computed at the output size.

Anything that changes how a pose becomes a picture (a new projection, a
field-of-view limit, a new per-keyframe parameter) changes all of them in the
same pull request, and is checked by comparing the paths on the same frame:
`tools/view_compare` for the direct view against the equirect path,
`tools/reframe_export` (both GPU paths; `INSV_FLAT_VIA_EQUIRECT=1` for the
equirect one) against a screenshot of the viewport.

### Stabilization

The camera's gyro (from the file) gives a rotation per frame; the stabilizer
(`insv/GyroStabilizer`, cached per clip in `ui/StabilizerCache`) smooths it
and hands the view a de-rotation matrix. It is rotation only. Lining the gyro
up with the video (timing, axis map, rolling-shutter readout) is
camera-specific: the camera's reader does it and hands the stabilizer an
`insv::ImuTrack` (`camera::ReadClipImu`); the camera's format document
describes it. The track is gyro samples, or, where a camera stores its
orientation instead (the X5's timelapse), that orientation alone; either
way video time maps to the track's time by the reader's timing, frame by
frame where the file gives each frame's own time.

Every path takes the de-rotation (and the stitch's capture-time correction,
and the sync fine-tune's measurement) at the frame's exact time from the
stream -- `timestamp_us` on the decoded frame, `insv::FramePts::us` from the
demuxer -- never at `timestamp_ms`, which is rounded to the millisecond for
seeks, trims and the timeline. At 29.97 fps the rounding is up to 0.37 ms,
and the correction moves with the camera's rotation in that time: on a
drone clip, half again as much shake left on the horizon.

## The project file

A project is one `.orp` file (JSON), written by `core/Project`.

- **Everything the user decides is in it**: clip trims, keyframes, colour,
  LUTs, stitch settings per clip, stabilization per clip, audio, overlays,
  titles, transitions, privacy regions, output format. A setting that changes
  a project's output and isn't stored here is a bug: it is lost on save,
  ignored by undo, and makes the export disagree with what the user saw.
- **Undo comes from it**: an undo point is a snapshot of the whole project
  document, so anything stored in the project is undoable for free, and
  anything that isn't, isn't.
- **Versions**: `Project::kFormatVersion`. Each feature that adds data to the
  file bumps the version, and a project that doesn't use the new feature is
  still written with the older version, so older builds keep opening it. Old
  files are read and migrated on load (`kOldestFormatVersion` is the oldest
  accepted). A new field needs: a default that reproduces the old behaviour
  when it's absent, the version logic, and a test in `tests/project_test.cpp`.
  A change that breaks old files needs `BREAKING CHANGE:` in the commit.
- **Paths**: media and LUTs inside the project folder are stored relative to
  it, so a project folder can be moved.
- **Copies, not references, of what lives outside it**: a clip's own lens
  calibration (made with Calibrate lenses and kept app-wide per camera) is
  copied into the clip, so the project stitches the same on another
  computer and doesn't change when the camera is calibrated again.

App-wide settings (theme, last folders, window layout) live in the app's
settings, not the project. The test is: would a second person opening the
project expect to see it? Then it's in the project.

## Cameras

Today the code reads Insta360 `.insv` files: the X5, and experimentally the
X6 and the ONE X2. Everything known about a format is in its document in
[`formats/`](formats/README.md).

The app reads clips only through one description, `camera::ClipMetadata`
(`src/camera/`), from the cached `camera::ReadClipMetadata(path)`, with the
IMU (`ReadClipImu`), the GPS track and the motion the overlays read
(`ReadClipTelemetry`) and the camera's preview picture (`ReadClipThumbnail`)
read on demand. A reader per camera family fills it (`Insta360Reader`, the
only one so far), picked by asking each in turn whether the file is its own:
what differs between cameras -- lens layout and which file and stream each
lens comes from, calibration, IMU timing and axes, colour profile, lens-guard
profiles, capture-mode names -- is data the reader fills in, not a code path
per camera. Differences between models of one family are that reader's
table (`insv::InsvModel` for Insta360). What's still to come is under
*Common camera model* in [`roadmap.md`](roadmap.md).

## Testing

- `./scripts/build.sh` builds and runs `ctest`: the model, the project file,
  the trailer parser, audio, the timeline, and the main window offscreen
  through stand-in clips (`tests/support/TestMedia`). Widgets and actions in
  tests are found by `objectName`, so give new ones a name.
- CI (`.github/workflows/ci.yml`) builds and tests on every supported target
  without a GPU. `gpu.yml` runs the GPU checks and a smoke test of the whole
  app.
- GPU output (stitch, view, export) isn't covered by the offscreen tests:
  check it on real clips with the tools and say how in the pull request.
