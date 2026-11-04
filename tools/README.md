# Developer tools

Command-line tools for checking, measuring and debugging the pieces of
orbis-reframe on real `.insv` clips. They use the same code as the app, but
without the user interface. They're for development only: `scripts/build.sh`
builds them into `build/`, and the release packages don't include them.

Run a tool with `--help` to see all of its options. This page says what
each tool is for and when you'd reach for it. The examples use clips from
`sample_footage/` (not in the repository; any X5 `.insv` works).

| Tool | Use it to… | Needs |
|---|---|---|
| [`insv_trailer_dump`](#insv_trailer_dump) | see what's inside a clip's metadata trailer | — |
| [`insv_stitch`](#insv_stitch) | stitch a frame on the CPU, or measure the lens seam | FFmpeg |
| [`stitch_source_smoke`](#stitch_source_smoke) | check that the app's stitch, seek and decode work | GPU |
| [`playback_bench`](#playback_bench) | time each step of playback and export | GPU |
| [`view_compare`](#view_compare) | compare the direct view render with the equirect path | GPU |
| [`seam_regress`](#seam_regress) | measure the seam on a list of frames, against the last run | GPU |
| [`stitch_bench`](#stitch_bench) | score a stitcher's picture over a stretch, against Insta360 Studio's | GPU, OpenCV |
| [`lens_calib`](#lens_calib) | calibrate a camera's lenses from a clip of the chart, without the app | OpenCV |
| [`reframe_export`](#reframe_export) | export a reframed flat video without the app | GPU, hw encoder |
| [`residual_stab`](#residual_stab) | measure the shake left after gyro stabilization | GPU, OpenCV |
| [`track_subject`](#track_subject) | try subject tracking and make a follow-cam | GPU, OpenCV contrib |
| [`plate_scan`](#plate_scan) | scan for number plates, check the blur by eye | GPU, OpenCV, ncnn |
| [`audio_filter_bench`](#audio_filter_bench) | try and score the audio noise filters | FFmpeg |
| [`make_hlg_lut`](#make_hlg_lut) | regenerate the HLG to Rec.709 LUT the app ships | — |

"GPU" means the app's own requirements: Vulkan with video decode, plus
FFmpeg. "OpenCV contrib" is `libopencv-contrib-dev`
(the `tracking` and `dnn` modules). ncnn comes from `scripts/fetch_ncnn.sh`.
`docs/dev-environment.md` lists the packages. CMake skips a tool whose
dependencies are missing and says so.

Angles are in degrees unless an option says otherwise. Times are
milliseconds of clip (source) time unless they're marked `S` (seconds).

---

## insv_trailer_dump

Prints what the X5 writes into a clip's `inst` trailer: camera model,
firmware, lens calibration, capture settings, and a summary of the IMU and
exposure data. `--imu` / `--exposure` print every sample, one per line, in
a layout that lines up with `exiftool -ee` for comparison. `--gps` prints
each GPS fix once (the file repeats it ten times a second): its time in the
clip in ms, Unix seconds, latitude, longitude, speed in m/s, heading (-1:
standing still) and altitude; `exiftool -ee -n` gives the same values, the
speed in km/h. On a clip without field 98 (the Antigravity A1) the time in
the clip comes from matching the GPS speed against the drone's own, and a
first line says so. The times are field 98's alone: the app then lines an
X5 track up against the IMU (it runs 0.5-2.4 s late by field 98; the log
line `GPS in … lined up with the IMU` says by how much).

**When:** a clip behaves oddly (stitch, sync, stabilization, GPS) and you want
to see what the camera recorded; or when working on `src/insv/InsvTrailer`.
`docs/formats/insta360-x5.md` explains the fields.

```sh
build/insv_trailer_dump clip.insv
build/insv_trailer_dump clip.insv --imu > imu.txt
build/insv_trailer_dump clip.insv --gps > gps.txt
```

## insv_stitch

Stitches one frame into an equirect with the CPU stitcher in `src/insv/`
alone. No Qt, no Vulkan. This is the reference the GPU stitch is checked
against, and the app's fallback. It can also measure the seam between the
two lenses:
- `--measure-seam IN OUT` fits the seam correction (lens-1 rotation, its
  centre, the radial scale) on frames spread over a range, as the app does.
- `--seam-offset IN OUT` measures how far the lenses still disagree, in
  degrees, with the correction given by `--seam Y,P,R,CU,CV,S` (as the fit
  and the app's status line print it); pick other frames than the fit's to
  check that it holds. `--seam-frames N` sets the frames for both (8).
- `--seam` also stitches the frame with that correction.
- `--lens-colour IN OUT` measures the clip's lens colour (how the two
  lenses differ per colour channel, and across the overlap) on 24 frames
  spread over a range, at the `--seam` correction, as the app does, and
  stitches the frame with it; `--colour R,G,B[,SR,SG,SB]` stitches with a
  given one (lens 0 against lens 1 in %, the slopes in %/deg, as the fit
  prints them).
- `--depth-seam` / `--depth-dump` show the depth-aware seam; the dump
  gives per bin the best match, how much better it is than any other shift
  (`uniq`) and the confidence the table gives it (`conf`).
- `--baseline` changes the lens spacing, and its sign tests which lens is
  in front.
- `--lens1 FILE` takes lens 1 from its own file, for a camera that writes a
  file per lens (the ONE X2's `VID_…_10_…`); the clip is the `_00_` file.
- `--lens-profile NAME` stitches with a lens-guard profile from the file's
  table (field 145 in the camera's format document). Without it the tool
  stitches bare lenses, but the app starts every project with **Premium
  lens guards** (`ProtectorA` on the X5). To compare with the app,
  `stitch_source_smoke` or an export, give the tool the same profile. A
  guarded clip stitched bare is ~4° off at the seam, which ghosts the
  far scene, and `--depth-seam` reads that as everything being ~0.4 m
  away.

**When:** working on the stitch geometry, the calibration or the seam;
checking that the `.insv` library works on its own.

```sh
build/insv_stitch clip.insv --time-ms 5000 --eq-width 3840 --out eq.ppm
build/insv_stitch clip.insv --time-ms 5000 --lens-profile ProtectorA --depth-dump
build/insv_stitch clip.insv --measure-seam 0 10000
build/insv_stitch clip.insv --seam -0.15,0.28,0.01,-0.077,-0.029,-0.072 \
    --seam-frames 16 --seam-offset 500 9500
build/insv_stitch clip.insv --lens-profile ProtectorA \
    --seam -0.15,0.28,0.01,-0.077,-0.029,-0.072 --lens-colour 0 60000
```

## stitch_source_smoke

Opens a clip through `stitch::NativeStitchSource` (the app's own stitch:
Vulkan video decode and the GPU stitch) and writes the frames at the given
times as PPM images. Times far apart exercise seeking. `--range START END
STEP` reads a run of frames instead and writes one every STEP ms.
`--measure-seam IN OUT` measures the seam correction as the app does and
stitches with it; `--seam Y,P,R[,CU,CV,S]` stitches with a given one.
`--lens-colour IN OUT` and `--colour R,G,B[,SR,SG,SB]` do the same for the
lens colour (as `insv_stitch`'s; measured after the seam, with it).
`--no-depth-seam` and `--new-stitcher` pick the stitcher (as the Stitching
section's *Stitcher*; default the depth-aware seam; `--no-seam-colour-blend`
and `--no-seam-flow` turn the new one's seam colour blend and seam flow
off), `--dump-depth` prints
the depth-aware seam's table per frame, `--calibration FILE.json` takes
a camera's own lens calibration (as `lens_calib --out` writes it and the
app keeps it) in place of the file's, for the stitch and the measurements,
and `--capture-timing` stitches with the capture-time correction from the
clip's gyro, as playback has it with the stabilization on. A frame from a
`--range` and the same frame seeked to must be byte-identical: that is how
the new stitcher's seek pre-roll is checked.

**When:** a quick check that decode, seek and stitch still work after a
change, before starting the app. The release test runs it on a build from
the source archive (`tests/release/dev_test.sh`).

```sh
build/stitch_source_smoke clip.insv 0 5000 60000    # stitch_source_smoke_<ms>.ppm
build/stitch_source_smoke --range 10000 12000 500 clip.insv
```

## playback_bench

Measure-only benchmark of where a frame's time goes, per clip (capture mode)
and per equirect size: decode alone, decode + stitch at each size, the
export paths, the viewport's lens-frame path, and seeks. Writes no files.
The stitcher is the depth-aware seam unless `--no-depth-seam` (a fixed
seam) or `--new-stitcher` (with its seam colour blend and seam flow unless
`--no-seam-colour-blend` / `--no-seam-flow`); the `seam` column is the
depth-aware seam's measuring and solving, or the new stitcher's (the
colour blend's fit and the flow).

**When:** after a change to decoding, stitching or the frame path, to see
whether a mode still plays in real time. The preview-size presets and the
"Auto" preview size are based on its numbers.

```sh
build/playback_bench clip-8k30.insv clip-5k60.insv
build/playback_bench --frames 300 --eq 3840,5760 clip.insv
```

## view_compare

Renders a flat 1920x1080 view of one frame from a few directions -- two of
them through the distorted projection (`wide140_d50`, a wide view half way
to stereographic, and `planet_d100`, a tiny planet) -- in two ways: straight from the fisheyes (the "fused" pass the viewport and export
use) and through an equirect of several sizes. It compares each with an 8K
equirect reference: PSNR, and a sharpness figure. It also renders the
fused view as P010 (the 10-bit export) and checks it against the NV12 one:
within one 8-bit code, and with samples finer than 8 bits (a widened 8-bit
picture has none); it exits 1 if not. `--seam Y,P,R,CU,CV,S` and
`--colour R,G,B,SR,SG,SB` stitch with a seam correction and a lens colour
(as `stitch_source_smoke`'s), and `--new-stitcher` the new stitcher with
its seam colour blend and seam flow (`--no-seam-flow` without the flow),
to check them on every path. `--out DIR` saves the
renders as raw NV12 / P010 images to look at with `ffmpeg`.

**When:** changing the view or stitch shaders, to check that the picture
still matches, hasn't lost detail, and that the 10-bit output still
carries 10 bits.

```sh
build/view_compare --at 5000 clip.insv
```

## seam_regress

Measures the seam between the two lenses on a fixed list of frames, the same
way every time, so a change to the seam can be checked against the run
before it. Each frame is stitched through `stitch::NativeStitchSource` three
times: on the GPU played into from `--warmup S` seconds before (2, as
playback reaches it), on the GPU seeked to cold (as a step back, a
screenshot or an export's first frame do), and on the CPU fallback, cold
(`--no-cpu` leaves that out). First it analyses the seam of the stretch
(`stitch::AnalyseClipSeam`, from `--warmup` before the frame to the frame,
held in memory) as the app does after opening a clip, so all three read the
analysis and the cold frame is the played one; `--live` leaves the analysis
out, so each measures the seam as it goes, as playback does before a clip
is analysed. The seam correction and lens colour are
measured over the whole clip as the app does on open, unless the list gives
them; the run prints them in the list's format, to paste in and save the
time. With `method=new` the frame goes through the new stitcher instead
(its seam colour blend and seam flow on, as new clips have them): it takes
no seam correction, lens colour or seam analysis, so none is measured.

The list has one frame per line, tab-separated: a name, the clip (relative
to where the tool runs), the time in seconds, then any of
`guard=none|standard|premium` (default premium, as the app),
`method=depth|new` (the depth-aware seam, the default, or the new stitcher),
`seam=Y,P,R,CU,CV,S`, `colour=R,G,B,SR,SG,SB,SPREAD`, `ref=FILE.ppm` and
`refrot=Z,Y,X`; anything after `#` is a note. A list names local clips, so
keep it outside the repository (`tmp/seamreg/`).

For each frame `--out DIR` gets:
- `NAME.json`: the measurements. `summary` holds the numbers `--baseline`
  compares; the rest are the detail: the depth tables (played, cold, CPU),
  the cut and fade per bin, the offsets per block, the colour per bin, the
  frame-to-frame changes.
- `NAME_strip.ppm`: the seam unwrapped, ±12° across it at 20 px/°, in five
  rows: lens 0 and lens 1 as decoded (each lined up by the depth table),
  the played frame, |lens 0 − lens 1| × 4 after the colour correction, the
  cold frame.
  Lens 0's side is at the top, so a strip can be upside down.
- `NAME_eq.ppm`: the played frame, `--eq-width` wide (3840; 0 for none).
  `--no-images` writes only the JSON.

What the summary measures:
- `offset_*`: how far the lenses still disagree after the depth table, per
  5° block of the band (the best NCC of lens 1 shifted ±1° against lens 0),
  over the textured blocks; `far_*` only the blocks the table puts at
  infinity, which is the calibration and the seam correction's error.
- `near_bins` (closer than 5 m), `q_max` (1/m), `fade_mean_deg`: the depth
  table and the blend; `depth_confidence` / `cold_depth_confidence`: the
  depth measurement's mean confidence on the played and the cold frame.
  The JSON's detail also has the state's colour residual (`residual_pct`),
  local correction (`local_pct`) and veiling light (`veil`) per bin, and
  the new stitcher's seam colour blend (`seam_colour_pct`).
- `colour_*`: the lenses' colour difference where they meet (the fade's
  centre, per bin, unclipped samples), lens 0 against lens 1 in %, after
  the colour correction; `raw_colour_*` before it. For the new stitcher:
  on the seam circle, at the template's geometry, after its seam colour
  blend (the seam flow moves no colour).
- `cold_*`: the cold frame against the played one: depth table, PSNR over
  the whole frame and in the band (99: the same picture). `cpu_*`: the
  CPU's cold frame against the GPU's.
- `flicker_*`: over the played run's last second, the mean change from one
  frame to the next in the band and beside it (levels; motion changes
  both, flicker only the band), and of the depth table, cut, fade and local
  colour correction (for the new stitcher, its seam colour blend's).
- `ref_psnr`, `ref_band_psnr`: with `ref=`, PSNR against a reference
  equirect of the same frame (an 8-bit PPM, any size, e.g. Insta360 Studio's
  export of it through `ffmpeg -i frame.png ref.ppm`). `refrot=Z,Y,X` is the
  rotation from our sphere to the reference's: R = Rz Ry Rx, about the
  axes x at the image centre, y half way to the right edge, z up (Studio's
  X5 equirect is ~1.85° off ours: `refrot=1.055,-0.505,1.422` on
  `192900`).

`--baseline DIR` reads the summaries of an earlier run and marks each
number that changed with `*` and the old value. `--only TEXT` runs the
frames whose name contains TEXT.

**When:** before and after any change to the seam (the depth table, the
cut, the colour match, the seam correction), and to check that a change
elsewhere left it alone.

```sh
build/seam_regress tmp/seamreg/list.tsv --out tmp/seamreg/before
# ... change, build ...
build/seam_regress tmp/seamreg/list.tsv --out tmp/seamreg/after --baseline tmp/seamreg/before
```

## stitch_bench

Scores a stitcher on the picture it makes, over a stretch of a clip, and
against a reference video of the same stretch: an equirect export from
Insta360 Studio. It never looks inside the stitcher, so the app's stitch, a
new one and Studio's own modes are all measured the same way. Measuring the
reference against itself gives Studio's numbers, which are the bar.
`seam_regress` measures the inside of the app's seam (depth table, cut,
colour per bin); this tool measures only what comes out.

The list has one stretch per line, tab-separated: a name, the clip, the
start and end in seconds, then any of:
- `stitcher=current` (the default: the app's `NativeStitchSource`, set up as
  playback has it) or `stitcher=video:FILE` (a stitched equirect video, for
  Studio's other modes, on the reference's timing and rotation);
- for `current`: `guard=none|standard|premium` (default premium),
  `method=depth|fixed|new` (the stitcher, as the Stitching section's
  *Stitcher*: the old one with the depth-aware seam -- the default -- or a
  fixed seam, or the new one, which takes no seam correction or lens
  colour; `depth=on|off` is the same as `depth` / `fixed`),
  `colourblend=on|off` and `seamflow=on|off` (the new stitcher's seam
  colour blend and seam flow, default on),
  `rs=on|off` (the
  capture-time correction from the gyro, default on), and `seam=` /
  `colour=` as in `seam_regress` (measured over the whole clip when not
  given; `seam=0,0,0,0,0,0` for none), and `calibration=FILE.json` (a
  camera's own lens calibration, as `lens_calib --out` writes it and the
  app keeps it, in place of the file's);
- `ref=FILE` (the reference video), `refstart=S` (the clip time of its first
  frame) and `refrot=Z,Y,X` (the rotation from our sphere to the reference's,
  as in `seam_regress`); or `ref=template`: our own stitch of the same
  frames from the file's data alone (the new stitcher without the
  capture-time correction, the seam colour blend or the seam flow; with the camera
  model's lens correction, as the new stitcher has it), whose geometry and
  colour hold still -- the reference for a clip without a Studio export.

Anything after `#` is a note. Unless `--live` is given, the app's seam
analysis of the stretch runs first, as the app does after opening a clip.

Every frame is measured on its seam strip: the seam circle unwrapped,
±30° across it. The band, and the lenses' axes, come from the file's
calibration with the guard's profile, never from the stitcher, so each
stitcher of a clip is measured on the same grid. Every `--every`'th frame
(10) also gets measured on the whole sphere at `--width` (3840). What the
summary reports:
- `psnr`, `band_psnr`: PSNR against the reference over the sphere and over
  the band. `*_gain` is the same after a per-channel gain on ours, and
  `gain_*_pct` is that gain. It takes out a colour difference, so what's left
  is geometry and sharpness.
- `geom_*`: where ours puts the reference's content (optical flow between the
  two, contrast-normalised), median and p95 in degrees, in the band, up to
  20° from the seam (`near`) and past it (`body`).
  `lens*_radial_50_80` / `lens*_tangential_50_80` are the same split per
  lens, at 50-80° from its axis: + radial means ours is further out. The
  JSON has them in 10° bins. `refrot_fit_*` is the `refrot` that would line
  the two up best, so it shows whether the list's value is right.
- `colour_seam`: how far ours is from the reference's colour round the
  seam, log: per 5° block and side, ours' mean colour over the reference's
  within 12° of the seam (a seam colour correction's full reach), less the
  same 32-38° out (each lens's overall difference, which no seam correction
  touches); the median over blocks, sides and channels (`_p90`: their 90th
  percentile). `colour_control` is the median of that control itself: 0
  against `ref=template` when nothing changed away from the seam.
  `colour_step10` is how much the near-less-control value changes over ten
  frames: against an uncorrected reference (`ref=template`, Studio's
  All-off) the seam colour correction's own movement; Studio's Chromatic
  Calibration against its All-off is the bar.
- `sharp_band`, `sharp_beside`: ours' gradient energy over the reference's,
  in the band and beside it. `own_sharp_band_beside` is ours, band over
  beside, and needs no reference.
- `alive_band`: how much the offset between ours and the reference along
  the seam changes from one frame to the next, in degrees per frame: per 5°
  block and each half of the band, the median offset (optical flow from the
  reference to ours), then the change against the frame before; the mean
  over frames of the median over the band's blocks (`_p90`: of their 90th
  percentile). A stitch whose geometry holds still keeps its offset whatever
  passes through the band; one that re-solves its warp each frame doesn't --
  the picture moving and bending round the seam in playback.
  `alive_control` is the same 25-29.5° from the seam, past where any
  stitcher reaches, so every stitcher is the plain lens there: flow noise
  and changes to the whole picture (a capture-time correction moves it with
  the camera's turn), the floor. `alive_peak` is the 99th percentile over
  frames of the band's 90th percentile: the few frames where a seam jumps,
  which the means hide. Against Studio's All-off, Studio's
  Optical flow export (`stitcher=video:`) sets the bar.

Near things crossing the seam (handlebars, mirrors) show up as
`geom_band_p95` against Studio's Optical-flow export, which follows them.

`--fit-time` first picks the reference frame that matches the first frame
best, within ±6 frames of `refstart`, and reports it (`refstart_fit_s`).
Use it when you don't know how the reference lines up.

For each stretch, `--out DIR` gets `NAME.json` (summary, per-frame series,
profiles) and `NAME_strip.y4m`: the seam strips, ±12° at `--video-px-deg`
(6), as a video in three rows: ours, the reference, and |ours
colour-matched − reference| × 4. `mpv` and `ffplay` play it. `--no-video`
leaves it out. `--baseline DIR` and `--only TEXT` work as in
`seam_regress`. Debug builds are slow; a Release build runs at about 0.5-1 s
a frame.

**When:** comparing a stitcher, or a change to one, with Studio and with the
last run. On X5 clips, Studio's 8K ProRes exports of `192900` (125-135 s,
`refstart=125`) and `_086` (0-30 s, `refstart=0`) line up at
`refrot=1.055,-0.505,1.422`.

```sh
build/stitch_bench tmp/bench/list.tsv --out tmp/bench/before
# ... change, build ...
build/stitch_bench tmp/bench/list.tsv --out tmp/bench/after --baseline tmp/bench/before
mpv tmp/bench/after/x5a_app_strip.y4m
```

## lens_calib

The app's **Calibrate lenses** without the app (`src/calib/`, the user's
side in the manual's *Stitching* chapter): finds the chart's corners in a
clip filmed of it, fits the stitch's lens model to them, prints the figures
the app's result page shows and whether they pass, and writes the
calibration as the app keeps it.

```sh
build/lens_calib CLIP.insv --square 41.96 [--guard none|standard|premium] \
    [--corners corners.txt] [--out calibration.json]
build/lens_calib --chart 2560 1440 chart.pgm   # the chart for a screen
```

- `--square MM`: the chart's square as it was shown.
- `--guard`: what was over the lenses: recorded in the calibration, and the
  file's model is compared with the guard's profile on top, as the app
  stitches without a calibration.
- `--corners FILE`: the detections. Read if the file exists, else found and
  written there, so the fit can be run again without the long part (finding
  the chart takes about twice the clip's length).
- `--out FILE`: the calibration, JSON. `stitch_bench`'s `calibration=` and
  a copy in the app's `calibrations/` folder take it.

It prints the frames used per lens, the fit's error (rms, pixels) against
the file's model's on the same corners, the lens 0 -> 1 transfer (the board's
pose from lens 0 alone, lens 1's corners predicted, median angle), how far
out each lens saw the chart, and how far the new model moves the picture
where the lenses meet. The exit code is 0 when it passes the app's checks,
3 when it doesn't.

**When:** working on the calibration, or checking a chart recording before
going through the app.

## reframe_export

Exports a trimmed range of a clip as a flat H.264 / H.265 video through
the app's export path: GPU stitch, reprojection, hardware encode. The
encoder is picked from NVENC, VAAPI and Vulkan video in that order (the
first one a throwaway open succeeds with; Vulkan video is skipped on
NVIDIA); `ORBIS_ENCODER=nvenc|vaapi|vulkan` pins one backend, any other
value is reported and ignored. With `ORBIS_VERBOSE=1` it prints which
encoder was picked and why the ones before it can't be used; the "no usable
hardware encoder" error says the same. The camera path
comes from a `--keys` file (one line per keyframe:
`time_ms yaw pitch fov [roll] [distortion]`, angles in **radians**, the
field of view corner to corner as the app's keys; distortion 0..1, left out
= automatic), or a fixed `--yaw/--pitch/--fov` (degrees, corner to corner)
and `--distortion D|auto` (default auto): 0 is rectilinear, 1
stereographic (with `--pitch -90` and a wide `--fov`, a tiny planet).
`--no-depth-seam` stitches with a fixed seam, `--new-stitcher` with the
new stitcher (as the Stitching section's *Stitcher*; default the
depth-aware seam), `--no-seam-colour-blend` / `--no-seam-flow` without
its seam colour blend / seam flow. It also takes the colour grade, a 3D LUT (`--lut FILE.cube`,
`--lut-strength PCT`) and a picture-in-picture inset. `--bit-depth 10`
encodes HEVC Main 10 from the fused view's P010 output (H.265 only); it
prints when the encoder falls back to 8-bit, or when the frames are 8-bit
(no fused GPU pass) widened into the 10-bit file.
`--split side|stacked|grid` exports a split screen, as a clip split in the
app does: the `--keys` / fixed view is pane 1, `--pane-keys N FILE` gives
pane N (2-4) its own keys, and a pane without them looks the way the app's
Fill panes sets it (turned from pane 1's first view, 90° across).
`--gap PCT` (of the output height, default 0.4) and `--gap-color RRGGBB`
set the divider.

**When:** testing export without the app, e.g. to script many renders or
to replay keys from `track_subject`. With `--lut`, to check the GPU's LUT
against FFmpeg's own `lut3d` filter on an export without it (both GPU
paths: set `INSV_FLAT_VIA_EQUIRECT=1` for the equirect + reproject one).
With `--split`, to check the panes on every path (`INSV_FLAT_VIA_EQUIRECT=1`,
`INSV_NO_GPU_STITCH=1` for the CPU one) against each other and against a
single-pane export of one pane's view at the pane's size.

```sh
build/reframe_export clip.insv out.mp4 --trim-in 10000 --trim-out 20000 --yaw 30 --fov 120
build/reframe_export clip.insv planet.mp4 --trim-out 3000 --pitch -90 --fov 300
build/reframe_export clip.insv out.mp4 --keys keys.txt --codec h264
build/reframe_export clip.insv out10.mp4 --trim-out 3000 --bit-depth 10
build/reframe_export clip.insv lut.mp4 --trim-in 1000 --trim-out 2000 --lut X5_I-Log_To_Rec.709_V1.0.cube
build/reframe_export clip.insv grid.mp4 --trim-out 3000 --split grid --gap 1 --gap-color ffffff
```

## residual_stab

Measures the camera movement that's left after gyro stabilization, from
the image itself (feature tracking in the forward view). It can render the
result with that rest removed too. `--diagnose` is a measure-only report:
- the IMU's spectra
- what the gyro stabilization removes, sweeping axis map, scale and time
  offset
- the image's shake spectrum against the gyro
- whether what's left is rotation or parallax

**When:** footage still looks shaky after stabilization and you need to
know why. Typical cases: a camera on a stick (the shake is a swing),
a sync or axis problem, or parallax that no rotation can fix. Run
`--diagnose` before changing the stabilizer.

What each `--diagnose` block tells you:

| Block | Shows | Read it as |
|---|---|---|
| [0] | shutter time × gyro rate | the motion blur inside each frame; no stabilization removes it |
| [1] | gyro / accel spectra | a mount resonance, and up to what frequency the IMU still carries real signal |
| [2] | rotation the gyro records vs. the correction applied | ratio well below 1: the stabilizer leaves rotation in |
| [3] | image pan vs. the gyro, sweeping axis map, scale and time offset | which gyro setup matches the picture, and whether the IMU carries the shake at all |
| [4] | forward residual, narrow vs. wide view | ≈ 1: rotation (correctable); ≫ 1: parallax from the camera moving |
| [5] | video↔IMU time sync | the offset that lines the gyro up with the picture |
| [6] | Horizon lock's "up" vs. gravity, roll / pitch per 10 s; on a clip that records its own up (a drone's attitude), vs. that too | ≈ 0: level; a steady offset: gyro bias or a bad gravity start |

If the shake is rotation, the IMU has it, and it still shows after
stabilization, suspect the pipeline before the physics: a sync error of a
millisecond or two, a wrong gyro axis map, or the lens model. Only rule
those out before calling a clip's shake parallax, motion blur or vibration
faster than the frame rate can capture.

```sh
build/residual_stab clip.insv --diagnose --trim-in 0 --trim-out 30000
build/residual_stab clip.insv out.mp4 --render derotate2d --stab-mode horizonLock
build/residual_stab clip.insv out.mp4 --render derotate --stab-mode directionLock --direction-ms 50000
```

`--stab-mode directionLock` holds the heading the camera faced at
`--direction-ms` (video time), as the app's Direction lock does.
`--no-up-ref` ignores the orientation a clip records (the drone's
attitude): it levels by the accelerometer and holds the heading by the
gyro alone, as the app did before it read the attitude; block [6] still
compares against the attitude's up, so it shows what the attitude changes.

## track_subject

Tracks a subject through the 360 sphere with the same tracker as the app's
"Track subject" button, from a box you give it. It writes reframe keys
(`reframe_export --keys` format), or renders a follow-cam video directly.
`--dump-equirect` writes the stabilized sphere with a yaw/pitch grid, to
read the box off. `--tracker` compares the app's hybrid tracker with plain
CSRT, VitTrack and NanoTrack, and `--occlude` tests how it recovers after
the subject is hidden.

**When:** tuning or debugging subject tracking on a clip where the app's
tracking loses the subject.

```sh
build/track_subject clip.insv --t0 60000 --dump-equirect grid.jpg
build/track_subject clip.insv --t0 60000 --dur 15 --box 40,-5,8,10 --render follow.mp4
```

## plate_scan

The privacy blur from the command line: scans a range for number plates the
way the app's "Scan clip" does, and writes the scan in the app's cache
format. It can also render stills of the blurred frame through the GPU
equirect, the fused view and the CPU stitch, to check by eye that no plate
pixels show. `--follow` runs "Blur a plate…" from a box instead of a scan,
`--back` runs it backwards.

**When:** a plate wasn't blurred (or something else was), or after changing
the plate model, the scanner or the blur.

```sh
build/plate_scan --t0 430000 --dur 20 --scan scan.bin clip.insv
build/plate_scan --scan-in scan.bin --still 439300 --stills-dir stills clip.insv
```

## audio_filter_bench

Test bench for the audio noise filters. `extract` decodes one audio stream
of a clip to a WAV, and also runs it through the app's "MC: Wind reduction".
`general` / `hiss` / `voice` / `voiceamb` run the app's other filters on a
WAV. `analyze` scores a filtered WAV against the raw one, per frequency band
and on / between the engine's harmonics. `run_all.sh` runs every filter
(optionally the DeepFilterNet CLI as a reference) on one stream and writes
the WAVs plus a report to listen and compare.

**When:** tuning a noise filter, or checking one on a new kind of audio
(another bike, wind without an engine, speech). Your ears decide; the scores
show what changed.

```sh
build/audio_filter_bench extract clip.insv out --stream 1 --start 60 --dur 20
build/audio_filter_bench analyze out/raw_s1.wav out/wind_s1.wav
tools/audio_filter_bench/run_all.sh clip.insv 1 60 20 out
```

## make_hlg_lut

Writes `resources/luts/HLG to Rec.709.cube`, the LUT the app offers for HLG
clips (the Insta360 X6's Dolby Vision mode), from the BT.2100 / BT.2408
formulas: HLG to display light on the 1000 cd/m² reference display, BT.2020
to BT.709, a highlight roll-off that keeps hue, then Rec.709 (gamma 2.4).
The source says what each step does and holds the curve's two settings
(where the HDR reference white lands, where the roll-off starts). It prints
where a few known signals end up. `--check` compares a file with what it
would write: `ctest` runs it on the shipped LUT, so a change to the curve
fails until the file is regenerated.

**When:** changing the curve. Regenerate the file, commit both, and look at
an HLG clip with the new LUT.

```sh
build/make_hlg_lut "resources/luts/HLG to Rec.709.cube"
build/make_hlg_lut --check "resources/luts/HLG to Rec.709.cube"
```
