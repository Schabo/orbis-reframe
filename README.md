# orbis-reframe

A native Linux video editor for **360° camera footage**: stitching,
gyro stabilization and, above all, **reframing**. Reframing means steering a
virtual camera through the sphere over time to get a normal flat video out of a
360° recording. It opens Insta360 `.insv` clips directly: the **X5** is
fully supported, the **X6**, **ONE X2** and **Antigravity A1** (a 360
drone) experimentally.

[![The orbis-reframe editor: a reframed ride on a country road with a picture-in-picture inset, the Media sidebar on the left, the Picture in picture tab on the right, a player bar under the video and a timeline with video, camera mic, music and overlay tracks below](site/screenshot-1400.jpg)](site/screenshot-full.jpg)

▶ **Demo video:** [plate blur and stabilization on a motorcycle ride](https://youtu.be/BER8GgtOpjQ)
(YouTube, 3:41)

> **AI disclaimer:** most of the code in this project was written with AI,
> using [Claude Code](https://claude.com/claude-code) with Claude Opus 5.5.
> The design, direction and testing on real footage are human.

## Status: "Works on my computer"

> Status: work in progress, built and tested on one machine (Debian Testing,
> an NVIDIA RTX 5070, Wayland, Sway; the Debian 13 and Ubuntu 26.04 packages
> and the AppImage in containers of those and other distributions on it).
> AMD and Intel GPUs are supported best-effort:
> a contributor has run it on an AMD Radeon; Intel hasn't been tried. Other
> setups that meet the requirements should work but haven't been tried;
> what users have confirmed is under [Confirmed working](#confirmed-working).

**Download:** a `.deb` and a tarball for Debian Testing, Debian 13 and
Ubuntu 26.04 LTS on the
[releases page](https://github.com/Schabo/orbis-reframe/releases), and an
AppImage for other distributions with glibc 2.41 or newer (Fedora, Arch,
openSUSE Tumbleweed, ...). The
[user manual](docs/manual/README.md) starts with the
[getting-started guide](docs/manual/getting-started.md): what your computer
needs and how to install. **Found a problem?**
[Open an issue](https://github.com/Schabo/orbis-reframe/issues).

**Insta360 X5:** fully supported, the camera orbis-reframe is built and
tested on: every capture mode tried so far (8K, 5.7K/5.7K+, 5.7K 60,
4K 120), I-Log through the camera maker's LUT, stabilization with its motion
axes measured, lens guards, and GPS from a connected GPS remote.

**Insta360 X6:** experimental -- clips open, stitch and export, including the
10-bit I-Log and Dolby Vision (HLG) modes through LUTs, but only a
contributor has tried real X6 files so far. Stabilization may turn the
picture the wrong way until an X6 clip confirms its motion axes; how the
HLG LUT handles highlights on real Dolby Vision footage, real-time playback
of 8K 50, and lens guards (the clips so far carry no lens-guard profiles)
are unchecked.

**Insta360 ONE X2:** experimental -- 5.7K clips (one file per lens, added
from any of them) open,
stitch, stabilize and export, but the stitch hasn't been checked against
Insta360 Studio's yet, the motion axes are measured on a few walking and
riding clips only, and Horizon lock can leave the horizon tilted on some
clips.

**Antigravity A1:** experimental -- 8K clips open, stitch, stabilize and
export, from one contributor's flights; the horizon is leveled by the
drone's own flight controller. Its GPS shows in titles and gauges once the
drone has moved (a hover or a clip shorter than about 8 seconds gives
nothing to line it up with the video by), and the 5.2K and 4K modes are
untried.

**Have another 360 camera?** Support for other Insta360, DJI and GoPro
cameras is planned, and needs real footage to build on:
[sample clips wanted](docs/formats/README.md#sample-clips-wanted).


## Confirmed working

Setups other people have run orbis-reframe on and told us about, until
there are enough that a list stops being useful. Each row says what was
confirmed, not more. Run it somewhere else? Tell us in
[Discussions](https://github.com/Schabo/orbis-reframe/discussions) -- what
works is as useful to know as what doesn't.

| System | Graphics | orbis-reframe | Confirmed |
|---|---|---|---|
| Fedora 44 | NVIDIA | 0.6.1 AppImage | NVIDIA hardware acceleration, sound, stitching |
| Manjaro | AMD Radeon 8060S (Strix Halo), Mesa RADV 26.2.4 | built from source, before 0.5.0 | Vulkan video decode, stitch, the view; export with VA-API and Vulkan video encoders; Insta360 X6 clips (8K30, 6K, 4K; firmware 1.2.36) open and stitch |

## Why

Insta360's own editor, **Insta360 Studio**, runs on Windows and macOS only.
Linux has no way to edit 360° camera footage properly:

- Cameras like the X5 record **dual-fisheye `.insv` files**. They need lens-calibrated
  stitching into a sphere before you can do anything with them. General
  players and editors can't read the calibration or the telemetry.
- The camera's **gyro/accelerometer data** sits in a proprietary trailer at
  the end of each file. Without it there is no stabilization or horizon
  leveling, and 360° footage from a moving vehicle is hard to watch without
  that.
- The most useful part, **interactive reframing**, doesn't exist in Linux
  editors. Kdenlive and Shotcut are built on MLT, which has no concept of
  spherical video or of keyframing a viewing direction. A plugin wouldn't be
  enough, so this is a standalone application.

My use case is **motorcycle footage** from an X5 on a selfie stick. That
footage needs lean-preserving stabilization that removes the shake but keeps
the bike's roll in corners, a level horizon when you want one, and a quick way
to pick where the camera looks at each moment.

Orbis Reframe doesn't use Insta360's Media SDK. An early prototype was built
on it, but its Linux support (an Ubuntu 22.04 / CUDA build and a file-to-file
stitch API) didn't fit an interactive editor on current Linux. So the
container and trailer parser, lens calibration, fisheye→equirect stitch and
gyro stabilization were **reimplemented independently** (`src/insv/`), from
public documentation and from studying camera-produced files. The editor has
no closed-source dependencies.

## Features so far

**Import and playback**
- Opens X5 `.insv` files directly, including every capture mode tested so far
  (8K, 5.7K/5.7K+, 5.7K 60, 4K 120); X6 (including its 10-bit I-Log and
  Dolby Vision modes), ONE X2 and Antigravity A1 clips experimentally.
- Real-time playback: Vulkan video decode, then a Vulkan compute
  fisheye→view pass on the GPU, with CPU fallbacks.
- Interactive 360° viewport: drag to look around during playback and while
  scrubbing.
- `.orp` projects and `.insv` clips open from the file manager.
- The viewport renders straight from the fisheyes at full source detail.

**Reframing**
- Keyframed virtual camera: yaw, pitch, roll, field of view and distortion,
  interpolated between keyframes. Set keyframes by dragging the viewport or
  by typing values into the sidebar. Click a keyframe's diamond on the
  timeline to go to it.
- Wide views drawn like a fisheye lens so the edges don't stretch, set
  automatically from the field of view, all the way to **tiny planets**
  (a keyframe move zooms smoothly from a normal view into one).
- Easing per keyframe (Linear, Fast to slow, Slow to fast), and moves that
  start and stop gently instead of abruptly.
- **Track subject**: draw a box around a rider, car or anything else in the
  view, and the camera follows it through the sphere. Tracking plays back live
  in the viewport, with the box green while the subject is found and red when
  it's lost, and you stop it when you've seen enough. The result is ordinary
  keyframes you can edit. It works in a gyro-stabilized flat view around the
  subject, so the camera shake and the 360° seam don't get in the way.
- **Split screen**: a clip in two or four panes, each looking its own way
  with its own keyframes, e.g. the road ahead and behind at once.

**Timeline**
- Multiple clips with trim in/out, split, duplicate and reorder, from the
  keyboard or the edit toolbar above the timeline.
- Flat videos (from a phone or an action camera) on the video track too,
  mixed with 360° clips, shown whole with bars or filling the frame.
- Transitions at cuts: crossfade or dip to black.
- Per-clip playback speed (0.1–4×) with pitch-preserving audio time-stretch.
- Multiple tracks: any number of audio and video (overlay) tracks, which can
  be renamed, reordered, muted, zoomed and scrolled. Tracks keep to fixed
  groups, and a video track folds its sound tracks away under it.
- Music clips with trim, level and fades. Add them from the menu or drag and
  drop them onto the timeline, where their waveforms are drawn.
- Every audio stream in a clip gets its own track, linked to the video: the
  camera's mic and an external mic such as the Insta360 Mic Air.
- Keyboard shortcuts for frame stepping, jumping between cuts, split and
  delete (Help ▸ Keyboard shortcuts, F1).

**Picture-in-picture, titles and gauges**
- Flat video files on overlay tracks, drawn as insets over the reframed view:
  position, size, opacity, rounded corners and a border, set in the Picture in
  picture tab or by dragging the inset in the viewport.
- Keyframe an inset's position, size and opacity, and bring an overlay forward
  or send it back. A PiP's own sound comes along on its own track, linked
  like a video clip's.
- Text titles on overlay tracks, started from ready-made tiles in the
  Overlays tab: font (from a list of the installed fonts, the last three
  used on top), colour, alignment, outline, shadow, box and spacing,
  keyframed and faded like an inset, and sliding, popping or typing in and
  out.
- Titles show the recording time as a running clock and the date, and,
  from the camera's GPS, the speed, altitude and heading; from its motion
  sensor, the lean angle and G-force. On the X5 the GPS is lined up with
  the video by the motion sensor.
- **Gauges** draw those values as a dial, a bar, a heading tape, a lean
  dial or a G circle, and a **route** map: the road around you, turning as
  you ride, or the whole trip seen from above.

**Stabilization** (native, from the camera's own IMU data)
- Modes: **Off / Lean follow / Horizon lock / Direction lock / Custom**
  (per-axis strength), à la Gyroflow. Direction lock keeps the horizon
  level and the view pointing one way through bends.
- Accelerometer-fused horizon reference, gyro↔video time sync and optional
  rolling-shutter correction. Antigravity A1 clips are leveled by the
  drone's own flight controller.

**Stitching**
- Stitch based on the MEI fisheye model, using the per-camera calibration
  stored in each file.
- Lens-guard profiles and a seam fine-tune per clip, so clips filmed with and
  without lens guards mix in one project. The fine-tune is measured from the
  video: how the back lens is turned, where its centre is, and its scale.
- Depth-aware seam: the stitch distance is measured along the seam every
  frame, so near objects (the bike, handlebars, hands) aren't doubled; the
  seam is cut where the lenses agree, and their colour matched along it.

**Look and sound**
- Per-clip colour grading: exposure, contrast, saturation, temperature. The
  same formula runs in the viewport and in export.
- A 3D LUT (`.cube`) per clip, with a strength: the camera maker's LUT for
  log footage (Insta360 I-Log; load the downloaded zip as it is) or a look.
  Log clips are recognised and ask for their LUT; HDR (HLG) clips are
  converted to normal video with the LUT that comes with the app.
- Audio playback: level, mute and fades for every audio clip, a level and
  mute per track, and a mixer used in both playback and export.
- Per-clip noise reduction: wind (a generic mode, and a motorcycle mode that
  tracks the engine's harmonics and turns down the wind between them), hiss
  and mains hum for quiet scenes, and voice isolation ("Voice only", or
  "Voice + ambience" that turns the rest down instead) with DeepFilterNet 3.

**Privacy blur**
- Blur number plates per clip: "Scan clip" in the Privacy tab finds them
  faster than real time on the GPU, and the blur shows in the viewport, export
  and screenshots. "Show regions" outlines what the scan found.
- Right-click a blur to remove a false hit, or use "Blur a plate…" to box a
  plate the scan missed; it's followed forwards and backwards from the box.

**Output**
- Flat reframed export of the whole timeline to H.264/H.265 on the GPU's
  hardware encoder (NVENC on NVIDIA, VA-API or Vulkan video on AMD and
  Intel), with the overlays, titles and gauges composited in and the audio
  mixed.
- Project settings: aspect ratio (16:9, 9:16, 1:1, 4:3), resolution, frame
  rate and bit depth (8-bit, or 10-bit as H.265 Main 10). The viewport is
  letterboxed to the chosen aspect.
- Screenshot of the current view at project resolution (PNG), from the
  player bar.

**Editor**
- A project library to start from: one folder per project, with its exports
  and caches; rename and duplicate projects there.
- Save/load (`.orp`, JSON), undo/redo, and autosave to a recovery file.
- A Media tab with the project's files, which notices files going missing
  and helps find moved ones, and a Media information section with what a
  clip's file says: its mode, lenses, frame rate, bitrate, recording time,
  camera and more.
- A media sidebar in tabs (Media, Overlays, Transitions), settings in tabs
  (Video, Audio, Picture in picture, Overlay, Transition, Privacy) with
  collapsible sections, a player bar under the video, and the timeline
  across the full window.
- Three themes: Ember, Light and Neutral dark.

Planned: more 360° cameras as sample clips come in (Insta360 X4 and X3, DJI
Osmo 360, GoPro MAX), a second 360° clip as an inset, and an effects
library. Further out: face and object blur, and HDR export. The
[roadmap](docs/roadmap.md) has the details.

## Building

Linux only, Wayland only, a GPU with Vulkan: NVIDIA (tested), AMD or Intel
on Mesa (best-effort). Playback uses Vulkan video decode where the GPU has it
and falls back to (slower) CPU decoding where it doesn't; export needs a
hardware encoder: NVENC on NVIDIA, VA-API or Vulkan video on AMD and Intel.
The app tells you at startup what's missing instead of failing. Dependencies are the Debian Testing (or Debian 13, Ubuntu 26.04) packages for Qt 6,
Vulkan, glslang, nlohmann-json, zlib and FFmpeg; sound goes through miniaudio,
which is in the repository (`external/miniaudio/`). OpenCV (analysis tools;
with the contrib modules also subject tracking), ONNX Runtime (voice noise
modes) and ncnn (the plate scan; `scripts/fetch_ncnn.sh`) are optional. [`docs/dev-environment.md`](docs/dev-environment.md) has the full
list and the pinned versions.

```sh
./scripts/build.sh          # configure + build
./scripts/build.sh -r       # build and run
./scripts/package.sh        # release tarball + .deb into dist/debian-testing/ (all features)
./scripts/package.sh --target debian-13      # the same for Debian 13 (or ubuntu-26.04), in a container
./scripts/package.sh --target appimage       # the AppImage into dist/appimage/, built in a Debian 13 container
./scripts/fetch_release.sh vX.Y.Z               # or fetch a draft release's packages into dist/
./scripts/release_test.sh [--target debian-13]  # test them in GPU containers (podman, docs/RELEASE.md; --target appimage too)
```

## Documentation

| Doc | Contents |
|---|---|
| [`docs/manual/`](docs/manual/README.md) | User manual: [getting started](docs/manual/getting-started.md), [the main window](docs/manual/main-window.md), [timeline](docs/manual/timeline.md), [reframing](docs/manual/reframing.md), [stabilization](docs/manual/stabilization.md), [stitching](docs/manual/stitching.md), [audio](docs/manual/audio.md), [colour](docs/manual/colour.md), [picture in picture](docs/manual/picture-in-picture.md), [titles and gauges](docs/manual/titles.md), [transitions](docs/manual/transitions.md), [privacy blur](docs/manual/privacy.md), [projects and export](docs/manual/projects-and-export.md), [media information](docs/manual/media-information.md), [keyboard shortcuts](docs/manual/keyboard-shortcuts.md) |
| [`docs/architecture.md`](docs/architecture.md) | How the pieces fit, the standing decisions and the rules every change keeps |
| [`docs/roadmap.md`](docs/roadmap.md) | What's planned and how each piece is meant to work |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Reporting bugs, building, commit messages, licensing of contributions |
| [`AGENTS.md`](AGENTS.md) | Instructions for AI coding agents (and people) working on the code |
| [`docs/dev-environment.md`](docs/dev-environment.md) | Build dependencies, toolchain, environment switches for diagnosing problems |
| [`docs/formats/`](docs/formats/README.md) | Camera file formats: everything known about the X5 `.insv`/`.lrv` format, and what's known about other cameras |
| [`docs/RELEASE.md`](docs/RELEASE.md) | How to make a release, step by step |
| [`tools/README.md`](tools/README.md) | The developer tools: what each is for, when to use it |

## Sources and thanks

Everything about the cameras' file formats (Insta360 X5, X6, ONE X2,
Antigravity A1) was learned from **interoperability documentation,
published research and camera-produced files**. No vendor's SDK or software
was ever disassembled. [`docs/formats/`](docs/formats/README.md) marks
every fact with where it came from and how well it's verified. We're grateful
to these projects and people:

- **[Gyroflow](https://gyroflow.xyz/) and
  [telemetry-parser](https://github.com/AdrianEddy/telemetry-parser)** by
  Adrian Eddy and contributors. telemetry-parser's Insta360 reader
  (`src/insta360/`) gave us the trailer record framing and type names, the
  protobuf schema of the metadata record, the raw-gyro scaling, the timestamp
  alignment, the X5 IMU orientation and the lens-profile construction.
  Gyroflow's approach to gyro stabilization (smoothing, per-axis strength,
  horizon lock, rolling-shutter readout time) is the model for our
  stabilization modes.
- **[ExifTool](https://exiftool.org/)** by Phil Harvey. `QuickTimeStream.pl`
  (`ProcessInsta360`) gave an independent decode of the `inst` trailer, which
  we checked our parser against byte for byte. The
  [ExifTool forum thread on the "Parameters" field](https://exiftool.org/forum/index.php?msg=78942)
  covers the short calibration string.
- **[insv-stitch](https://github.com/BenjaminHenriksson/insv-stitch)** by
  Benjamin Henriksson: the 56-token MEI calibration layout, sensor→video
  scaling, the `.insv.pb` sidecar and the IMU→camera matrix for the X5.
- **Insta360**:
  the [X5 online manual](https://onlinemanual.insta360.com/x5/en-us/operating-tutorials/storage/fileformat)
  (file naming, segmented recording) and Insta360 Studio, which we used as a
  visual reference for stitching and horizon leveling.
- **TDK InvenSense**: the
  [ICM-40609-D datasheet](https://invensense.tdk.com/products/motion-tracking/6-axis/icm-40609-d/)
  for the IMU's full-scale ranges.
- **Published methods**: the unified omnidirectional (MEI) camera model
  (C. Mei & P. Rives, "Single View Point Omnidirectional Camera Calibration
  from Planar Grids", ICRA 2007) for the fisheye lens model, and the Mahony
  complementary filter (R. Mahony, T. Hamel & J.-M. Pflimlin, "Nonlinear
  Complementary Filters on the Special Orthogonal Group", IEEE TAC 2008) for
  fusing the accelerometer into the horizon reference.
- **[DeepFilterNet](https://github.com/Rikorose/DeepFilterNet)** by Hendrik
  Schröter and contributors (MIT / Apache-2.0; H. Schröter et al.,
  "DeepFilterNet: Perceptually Motivated Real-Time Speech Enhancement",
  Interspeech 2023): its DeepFilterNet 3 model runs the voice noise modes
  (`resources/models/deepfilternet3`, with ONNX Runtime). FFmpeg's `afftdn`
  filter was a comparison reference in `tools/audio_filter_bench`, not built in.
- **Subject tracking** combines OpenCV's CSRT tracker (A. Lukežič, T. Vojíř,
  L. Čehovin Zajc, J. Matas & M. Kristan, "Discriminative Correlation Filter
  with Channel and Spatial Reliability", CVPR 2017), which follows the subject,
  with the **VitTrack** model from [OpenCV Zoo](https://github.com/opencv/opencv_zoo)
  (Apache-2.0, `resources/models/vittrack`), which checks each frame that it's
  still the same subject.
- **Number-plate detection** uses the YOLOv9-s plate model from
  [open-image-models](https://github.com/ankandrew/open-image-models) by
  ankandrew (MIT, `resources/models/plates`; C.-Y. Wang, I.-H. Yeh &
  H.-Y. M. Liao, "YOLOv9: Learning What You Want to Learn Using Programmable
  Gradient Information", ECCV 2024), run on the GPU by ncnn.

Built with [Qt 6](https://www.qt.io/), [Vulkan](https://www.vulkan.org/),
[FFmpeg](https://ffmpeg.org/) (demux, decode, hardware encode, audio, time-stretch),
[glslang](https://github.com/KhronosGroup/glslang) (shader compiler),
[nlohmann/json](https://github.com/nlohmann/json),
[miniaudio](https://miniaud.io/) (David Reid, Unlicense OR MIT-0; sound
output), optionally
[OpenCV](https://opencv.org/) (analysis tools, subject tracking) and
[ONNX Runtime](https://onnxruntime.ai/) (the voice noise modes) and
[ncnn](https://github.com/Tencent/ncnn) (Tencent, BSD-3-Clause; the plate
scan), and [Lucide](https://lucide.dev/) icons (ISC license; the parts
derived from [Feather](https://feathericons.com/) MIT).

Insta360, Insta360 Studio, X5, X6 and ONE X2 are trademarks of Arashi
Vision Inc.; Antigravity and A1 are trademarks of their owner. This project
is not affiliated with or endorsed by Insta360 or Antigravity.

## License

Copyright (C) 2026 Markku Kokko

The editor is licensed under the **GNU General Public License v3.0 or later**
([`LICENSE`](LICENSE)).

The `.insv` library in [`src/insv/`](src/insv/) (container and trailer parsing,
lens calibration, stitching, gyro stabilization, audio processing) is licensed
more permissively, under **MIT OR Apache-2.0** at your option
([`src/insv/LICENSE-MIT`](src/insv/LICENSE-MIT),
[`src/insv/LICENSE-APACHE`](src/insv/LICENSE-APACHE)). Every file there carries
an SPDX header saying so. That way other projects can use it, and it can move to
its own repository later. Contributions to `src/insv/` are accepted under the
same dual license.

Third-party components, their licenses and where the license texts are (in the
source tree and in the packages): [`THIRD_PARTY.md`](THIRD_PARTY.md).
