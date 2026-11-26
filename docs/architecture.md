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
- **`lib/orbis-core/` stays independent**: no Qt, editor or Vulkan-loader
  dependency, licensed MIT OR Apache-2.0, so it can become its own library.
  It is built through its own `CMakeLists.txt`, with its tests and tools
  beside it, and is included as `<orbis-core/...>`. Its core is
  camera-agnostic: each file format has its own folder (`insta360/`,
  `dji/`) that fills the core's types (the lens model in
  `stitch/LensModel.h`), and only that folder includes its headers. The
  `orbiscore_layering` test checks this, and that the library includes
  nothing of Qt or the app.
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
  an AppImage for every distribution with glibc 2.39 or newer (tested from
  2.41), built on
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
| `lib/orbis-core/` | orbis-core, the camera-file library: `.insv` trailer, calibration, IMU timing, the Insta360 per-model table, demux, Vulkan video decode, the fisheye model, gyro stabilization, seam measurement, audio filters | std C++, FFmpeg (demux / decode parts) -- no Qt |
| `src/camera/` | The common clip description (`ClipMetadata`, read once per file and cached), a reader per camera family that fills it, and the Media information probe | `orbis-core`, FFmpeg -- no Qt, no Vulkan |
| `src/core/` | The project model: media pool, timeline, clips, keyframe tracks, titles, gauges, transitions, the colour grade, the `.orp` file, undo, autosave | Qt Core, nlohmann-json, zlib (LUT zips) -- no widgets, no Vulkan |
| `src/stitch/` | `IStitchSource` ("the frame at time T of clip X"), `NativeStitchSource` (dual fisheye), clip probing, clip sync, per-clip seam | `camera`, `orbis-core`, `render` |
| `src/render/` | Vulkan: the fisheye stitch, the viewport renderer and its shaders, LUTs on the GPU | Vulkan |
| `src/export/` | Export: the CPU reproject (`ReframeRenderer`), flattening the timeline into the encoder, flat-video reading, inset compositing | FFmpeg |
| `src/privacy/` | Number-plate scan, tracking and blur regions | `orbis-core`, ncnn |
| `src/track/` | Track subject | OpenCV |
| `src/calib/` | Lens calibration from a clip of the chart: ChArUco corners in both lenses, the stitch's lens model fitted to them | `camera`, `orbis-core`, OpenCV (objdetect) |
| `src/ui/` | The Qt Widgets app: main window, panels, timeline widget, playback, background jobs | everything above |

`tools/` holds command-line tools that link the same libraries, for checking
a change on real clips without the GUI ([`tools/README.md`](../tools/README.md)).

## The picture's path

1. **Demux and decode** (`orbis-core/media/FisheyeDemux`, `orbis-core/media/VulkanVideoDecoder`): the
   two fisheye lens streams, decoded on the GPU through FFmpeg's Vulkan
   hwaccel into NV12 (8-bit) or P010 (10-bit) images, with a CPU decode
   fallback when the GPU path can't be used. A series of photos (JPEG,
   which Vulkan video can't decode) is read ahead on the CPU, several
   photos at once (`LensDemuxer`). For playback only, the
   picture can come from the camera's low-res proxy instead
   (`PreviewStitchConfig::proxy`, the app setting *Play from preview
   files*): both lenses in one stream, decoded once, each lens a rectangle
   of the frame (`orbiscore::LensSource::rect`, which the readers fill in;
   `StitchGpuParams::lensRect` in the stitch, a crop on the CPU path).
   `NativeStitchSource` hands its frames out on the clip's own frame times,
   each showing the proxy frame on screen then, so steps, seeks and trims
   work as on the full file; the frame's microsecond time is the proxy
   frame's, which the stabilization follows. Calibration, motion, seam
   analysis, export and screenshots stay on the full files.
   The app offers the setting when a clip with a proxy decodes on the CPU
   or its decoder falls behind the clip's frame rate
   (`PlaybackController::proxyOffer`, `DecodePace`); display skips don't
   count.
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
   [the roadmap](roadmap.md)); the new stitcher's seam colour blend (`orbis-core/stitch/SeamColourBlend`)
   matches the lenses' colour per stretch of the seam from what they see in
   the overlap over each frame and the quarter second before it, and its seam
   flow (`orbis-core/stitch/SeamFlow`) warps each lens near the seam by its share of
   the optical flow between them in the overlap, averaged over the frame
   and the four before it -- a texture beside the stitch's parameters on
   every GPU path, the viewport's lens frames included. On the GPU the
   strips, the flow (DIS, `render/shaders/seam_flow.comp`) and the average
   stay there, in the frame's measuring submission; the CPU stitch
   fallback estimates it with OpenCV's DIS (`orbis-core/stitch/SeamFlowEstimator`).
   Both run on the strips anchored first: bright featureless parts get the
   same fixed pattern in both lenses, so the flow stays zero there
   (`orbiscore::AnchorSeamFlowStrips`, the shader's anchor passes). With both
   on, the lenses meet in a narrow fade on the seam, widened per stretch
   where they still differ near the seam after the flow (`orbis-core/stitch/SeamFade`):
   each frame's patch means of the warped raw strips are kept with its
   flow, and the share of the band per strip column is solved with the
   frame's colour table into the flow texture's last row -- on the GPU in
   the stitch's own submission (`recordSeamFade`), since the colour comes
   back to the CPU only after the measuring one. A seek measures the
   frames all three need before the frame it goes to, so they need no analysis.
   The depth-aware seam's state
   (the distance per stretch of the seam, the cut across the overlap, the
   colour correction there) is a function of the measurements of the frames
   either side of a frame (`orbis-core/stitch/SeamWindow`), worked out once per clip in a
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
4. **Grade and effects**: the clip's LUT and the Colour panel's adjustments
   (`color::Grade`), applied to the flat picture, then the clip's video
   effects (`fx::ClipEffects`, `src/core/ClipEffects.h`) in the same pass.
   A clip has at most one of each effect, run in a fixed order: the colour
   look (a creative 3D LUT, `fx::Look`; the camera's conversion LUT is the
   grade's, before the adjustments), the sharpen, the vignette, the grain.
   The sharpen needs its neighbours, so it is a pass of its own (below).
   Per-pixel effects are placed against the whole output frame, not the pane or
   inset being drawn; the picture outside the clip's (a split's divider, a
   flat clip's bars) is left alone. The subject tracker and the plate scan
   see the picture without them.
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
the diagonal field of view, the distortion or Auto, Natural, Fisheye), what
it renders with at an aspect (`view::lens`: the vertical angle, the
distortion, Natural and Fisheye), and how its pixels become rays and back
-- is written once, in
`src/core/ViewProjection.h` and its GLSL twin
`src/render/shaders/view_projection.glsl`; every implementation above, and
viewport picking and overlays, goes through them, each turning the pose
into a lens at the aspect it draws. Keep the two in step.

Each of the four view shaders is built twice: as it is, and with
`VIEW_SHAPE` defined, the view-shape build, for a view whose Natural or
Fisheye is above 0 (`view::Lens::shaped`; the renderer picks the pipeline
per view). With Natural's code in the one shader, even behind a branch,
NVIDIA's compiler changed the last bit of existing pictures; a view without
a shape runs code that doesn't contain it. A new per-keyframe shape goes
into the view-shape build; adding one there can move the last bit of that
build's existing pictures (Fisheye did, for some Natural frames), so
measure it and say so.

The four view shaders and `inset.frag` (a flat clip's picture) also have an
effects build (`EFFECTS`, `clip_effects.glsl`, with and without the view
shape), for a clip with video effects; a clip without them is drawn by the
builds above, unchanged. The effects' settings don't fit the push
constants (full at Vulkan's guaranteed 128 bytes): they come in a small
uniform buffer, set 2 (`render::VulkanEffectsRing`); the colour look's 3D
LUT is set 3, from the same `render::VulkanLutStore` as the grade's (set 1),
the identity one for a clip without a look. A built-in look
(`src/core/LookPresets.*`) is a formula sampled into a LUT, so it takes the
same path as a `.cube` file. The CPU export's twin is `fx::applyEffects()`,
with the look applied by `exportpipe::gradeRgb()`; change both together. An effect that changes
every frame (film grain) takes a frame number from whoever draws the frame
(`fx::Render::setFrame`): the export uses the output frame on the timeline,
a transition's layer its source frame, and the viewport counts the pictures it
has shown, so a paused frame keeps its grain. Its noise is an integer hash,
the same bits in GLSL and C++, so the paths agree at the same output size.

The sharpen (`fx::Sharpen`, an unsharp mask on the luma) can't be drawn
pixel by pixel. A clip with one draws its views into a canvas instead of
its frame (the effects block's `canvas`): the graded picture with its
look, and each pixel's label -- 0 where there's no picture (a split's
divider, a flat clip's bars, outside a globe's disc), another number per
pane. `src/render/shaders/sharpen.comp` (`render::VulkanSharpen`) then
blurs the canvas's luma across and down, each pixel only from pixels of
its own label, adds the difference back to red, green and blue alike,
runs the vignette and grain, and writes the frame (NV12, P010 or RGBA8).
So no halo crosses a pane's edge and the divider stays as it is. The GPU
export's canvas is a buffer the view compute shaders write; the viewport's
is an RGBA16F image (`EquirectViewportRendererSharpen.cpp`) drawn before
the window's render pass and copied into the same buffer, the sharpened
frame drawn into the window as a full-output inset, with the overlays on
top. The CPU export grades a whole picture first, then sharpens it
(`src/core/Sharpen.h`, the shader's twin), then runs the per-pixel
effects. Its radius is against the output frame's height, so the viewport
sharpens at its own size. A clip without a sharpen is drawn as before.

A split clip's picture is several views, one per pane: where each pane
goes, and so its aspect, comes from `pane::rects` (`src/core/PaneLayout.h`)
in every one of them, computed at the output size.

Anything that changes how a pose becomes a picture (a new projection, a
field-of-view limit, a new per-keyframe parameter) changes all of them in the
same pull request, and is checked by comparing the paths on the same frame:
`tools/view_compare` for the direct view against the equirect path,
`tools/reframe_export` (both GPU paths; `ORBIS_FLAT_VIA_EQUIRECT=1` for the
equirect one) against a screenshot of the viewport.

### Stabilization

The camera's gyro (from the file) gives a rotation per frame; the stabilizer
(`orbis-core/stab/GyroStabilizer`, cached per clip in `ui/StabilizerCache`) smooths it
and hands the view a de-rotation matrix. It is rotation only. Lining the gyro
up with the video (timing, axis map, rolling-shutter readout) is
camera-specific: the camera's reader does it and hands the stabilizer an
`orbiscore::ImuTrack` (`camera::ReadClipImu`); the camera's format document
describes it. The track is gyro samples, or, where a camera stores its
orientation instead (the X5's timelapse), that orientation alone; either
way video time maps to the track's time by the reader's timing, frame by
frame where the file gives each frame's own time.

Every path takes the de-rotation (and the stitch's capture-time correction,
and the sync fine-tune's measurement) at the frame's exact time from the
stream -- `timestamp_us` on the decoded frame, `orbiscore::FramePts::us` from the
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

Today the code reads `.insv` files: the Insta360 X5, and experimentally the
X6, the ONE X2 and the Antigravity A1 (a 360° drone that writes Insta360's
format); the X5's Interval photos (`.insp`), a series as one clip, a photo
a part (`orbiscore::ClipPart::stillUs`); and experimentally the DJI Avata
360's `.OSV`. Everything known about a format is in its document in
[`formats/`](formats/README.md).

The app reads clips only through one description, `camera::ClipMetadata`
(`src/camera/`), from the cached `camera::ReadClipMetadata(path)`, with the
IMU (`ReadClipImu`), the GPS track and the motion the overlays read
(`ReadClipTelemetry`) and the camera's preview picture (`ReadClipThumbnail`)
read on demand. A reader per camera family fills it (`Insta360Reader`,
`DjiReader`), picked by asking each in turn whether the file is its own:
what differs between cameras -- lens layout and which file and stream each
lens comes from, the files a long recording is split into, calibration and
its lens model (MEI, or DJI's theta-polynomial), where each lens sits in
the camera (`orbiscore::LensPose`; the stitch assumes none) and how the camera holds them (the mount),
where a lens's picture shows the camera itself (`orbiscore::LensMask`),
IMU timing and axes, colour profile, lens-guard
profiles, capture-mode names -- is data the reader fills in, not a code path
per camera. Differences between models of one family are that reader's
table (`orbiscore::InsvModel` for Insta360, `DjiReader`'s for DJI). A model
of a known family without a row of its own opens on a general row -- what
the measured rows agree on, never one model's own measurements -- and the
clip is marked untested (`ClipMetadata::untestedModel`), which the app
tells the user. What's still to come is under
*Common camera model* in [`roadmap.md`](roadmap.md).

## Testing

- `./scripts/build.sh` builds and runs `ctest`: the model, the project file,
  the trailer parser, audio, the timeline, and the main window offscreen
  through stand-in clips (`tests/support/TestMedia`). Widgets and actions in
  tests are found by `objectName`, so give new ones a name.
- CI (`.github/workflows/ci.yml`) builds and tests on every supported target
  without a GPU, and on Debian 13 without the optional dependencies. `gpu.yml` runs the GPU checks and a smoke test of the whole
  app.
- GPU output (stitch, view, export) isn't covered by the offscreen tests:
  check it on real clips with the tools and say how in the pull request.
