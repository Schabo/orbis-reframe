# GoPro MAX / MAX2 — container, streams, telemetry

Status: **measured on GoPro MAX clips** (four takes, five `.360` segments, firmware
`H19.03.02.02.00`), not yet implemented. The clips are kept outside the repository
and are not published
under licence terms still to be confirmed (📖 rule: "ask before adding").

Markers: ✅ verified against a real file · ⚠️ third-party claim · 📖 third-party
claim, not yet checked here · ❓ unknown.

The short version: **GoPro has already done the stitching.** A `.360` is not a raw
dual-fisheye recording like the other two cameras here — the camera dewarps each lens
and writes the result as an **equi-angular cubemap** (EAC): six cube faces, three per
lens, packed into two HEVC tracks. What a reframe editor has to add is the
cube→sphere assembly, the blend across the one seam, and the IMU work. The geometry is
not ours to invent, and `libavcodec` already carries a reference implementation of the
inverse map (`v360_vulkan`'s `gopromax` input, which is the same map as trek-view's
`max2sphere`).

The one thing worth knowing before reading the rest: **the faces are equi-angular, not
pinhole.** Treating them as rectilinear gives a sphere that looks plausible and is
wrong in a way that shows up as a curved horizon and a bad seam. (This note previously
reported a pinhole fit; that fit was the model's error, not the camera's — see
[[geometry]](#geometry).)

## Container ✅

`ftyp` is `qt  ` (QuickTime), not MP4, and the file is written as
`moov → mdat → mdat`: **the metadata sits before the media**, so a reader gets the
track table without seeking to the end. `free` boxes carry the maker note
(`GoPro Hero…`-style metadata, here `GoPro Max`).

Seven streams, all in one file:

| index | codec | what |
|---|---|---|
| 0 | HEVC Main, **4096×1344**, yuvj420p (8-bit, full range) | lens A: three cube faces side by side |
| 5 | HEVC Main, **4096×1344**, yuvj420p | lens B: the other three faces |
| 1 | AAC-LC, stereo, 48 kHz | audio |
| 6 | PCM 32-bit float, 4ch, 48 kHz | audio (the camera's 4-mic array) |
| 2 | `bin_data` GPMF, timescale 1/1000 | **the IMU track** — see [[telemetry]](#telemetry) |
| 3, 4 | `unknown` GPMF | per-frame records and the global settings block |

Two details that will bite:

- **The two video tracks are not adjacent** (0 and 5), and the audio and telemetry
  tracks sit between them. Selecting "the first two video streams" gets it wrong. ✅
- **8-bit, not 10-bit.** The MAX records 8-bit HEVC in every 360 mode, so the
  `tenbit` path the Insta360 reader uses is never taken here. ✅

The `.LRV` proxy is a different animal: **1408×704 H.264 holding the two raw fisheye
circles** side by side, radius 352, centres at (352, 352) and (1056, 352). So the proxy
is the *undistorted* path and the `.360` is the *distorted-then-cubemap* path to the
same sphere — which makes the proxy a useful independent oracle for checking a stitch,
and it means `v360=input=dfisheye` is right for the `.LRV` and wrong for the `.360`. ✅

Segmented recording splits a take into `GSnnnnnnN.360` files (the trailing digit is
the part). Parts are **not** listed in the container: there is no edit list, no
`chpl`, no link from one file to the next. What does link them is the IMU clock, see
[[split files]](#split-files). ✅

## Geometry

Each stream is one strip of three cube faces, `side | centre | side`, 4096 = 1376 +
1344 + 1376 at 5.6K. Axes: x right, y forward (the front lens), z up.

```
stream 0:  LEFT  | FRONT | RIGHT     (FRONT upright; the strip runs on left to right)
stream 1:  DOWN  | BACK  | TOP       (turned: BACK's right edge meets TOP, its left DOWN)
```

✅ measured on `GS012267.360` at 5 s against FFmpeg 9.1's own stitch
(`v360_vulkan input=gopromax`): for each face of our equirect, every strip region of
both streams, all eight square symmetries and the face's position in the region were
tried, and the picture correlated with FFmpeg's at NCC 0.998 (FRONT) / 0.999 (BACK)
and ≥ 0.9994 for each half of the side faces; the runner-up of each face was ≤ 0.62
(go_pro_2 `tools/eac_layout_fit.py`). In the face's (u, v) ∈ [-1, 1], u along the
strip and v down it, with e = atan(component / dominant) · 4/π:
FRONT (e_x, −e_z), RIGHT (−e_y, −e_z), LEFT (e_y, −e_z), BACK (e_z, −e_x),
TOP (e_y, −e_x), DOWN (−e_y, −e_x).

**The centre face** is 1344 px for 90°. **A side face** is 1312 px for 90°, cut in
half at the lens boundary with a 64 px overlap between the halves: the half nearer the
centre face at 0–656 of its region, the outer half at 720–1376 (mirrored the same way
in the right region). Each half runs on into the overlap — its lens's view past the
boundary — so the stitch cross-fades them there. ✅ (same measurement: each half fits
at exactly that scale and offset.) In terms of the stream size: centre = H, side =
(W − H)/2, overlap = 2·(side − centre), side span = side − overlap; only 5.6K is
measured.

An earlier version of this document, and the reference it was checked against
(max2sphere's layout reproduced in go_pro_2 `tools/eac_stitch.py`), had the side faces
as square 1376 px faces with a 32 px band and a different orientation for each face;
a stitch built on it put face edges through the scene, which only showed once a real
frame was looked at next to FFmpeg's.

Each face is **equi-angular**: the direction of a pixel is the gnomonic projection of
its position on the cube face, and the face's half-angle spans 45° at the edge —
`q = atan(q) · 4/π` in `max2sphere`'s form, i.e. the standard EAC inverse
(`libavutil`'s `flat`/`eac` pair does the same map). The faces are **not** rectilinear
crops of a fisheye, and there is no per-face focal length to fit. ✅

The cube is **exact 90°**: the six face directions are the six axis directions, and
the two lenses are 180° apart (`ANGY = 180.19°` in the calibration, see
[[calibration]](#calibration)). ✅ (A pinhole model fitted to the same pixels reported
inter-face angles of 80–96° and a 100° face FOV; those were the wrong model talking.)

**The seam.** The two lenses meet in the plane y = 0: through the middle of LEFT,
RIGHT, TOP and DOWN, where each side face is split into its two halves. The lenses are
~22.75 mm apart laterally and ~36.4 mm along the optical axis (`SHFX`/`SHFZ`), so near
objects disagree across that boundary by an amount no blend can hide — that part is
parallax. The cross-fade over the 64 px overlap makes the far field continuous: on
`GS012267.360` at 5 s the luminance gradient across the boundary is the same as 3–10°
away from it (ratio 1.00, ours and FFmpeg's alike), and the face edges of the hard cut
measure 0.46 on the seam metric (median jump across a face boundary over the median
busy gradient; the wrong layout measured 12–14). ✅ The seam numbers in earlier notes
(7–12 hard cut, 0.22–0.37 blended) were measured on the wrong layout and don't stand.

## Telemetry — GPMF

The IMU track is GoPro's GPMF: a big-endian KLV tree. The framing is worth stating
exactly, because the obvious reading is wrong for the high-rate streams:

```
tag(4) | type(1) | bytes_per_sample(1) | count(u16 BE) | payload (padded to 4)
```

The `count` is 16-bit and its **high byte shares the field a naive reader calls
`size`** (u16 LE). The two readings agree whenever the count fits in 8 bits, which is
most tags — and disagree exactly where it matters. Measured over the sample tree's 18
extracted blobs: the `size:u16le × repeat:u8` reading lands each tag's end on the next
tag **527** times, this reading **829** times, with the same failure count. The
difference is the gyroscope: at 833 rows per sample the naive reading mis-sizes the
payload, and a parser that validates payload ends then stops walking and reports the
gyro as present once instead of 63 times. ✅

Each GPMF sample is one packet of the `bin_data` track (63 packets for a 64.56 s clip,
one per ~1.04 s), and each sample carries **eleven** streams: `Accelerometer`,
`Gyroscope`, `Magnetometer`, `CameraOrientation`, `ImageOrientation`, `Sensor ISO`,
`Shutter`, `WB RG gains`, `WB Kelvin`, `Image uniformity`, plus per-frame exposure.
The two that matter here, measured: ✅

| stream | rate | `SCAL` | units | notes |
|---|---|---|---|---|
| `Gyroscope` | **~800 Hz** (790–801 across clips) | 939 | rad/s = raw / 939 | |
| `Accelerometer` | **~200 Hz** (197–200) | 417 | m/s² = raw / 417 | a resting camera measures 9.94 m/s², so 1.013 g — the unit is m/s², and `ImuSample::accel` wants g |

Both carry `MTRX = [0,-1,0, 1,0,0, 0,0,1]` — the sensor axes expressed in the camera
body, as a real matrix rather than a guess. `ORIN`/`ORIO` (`XzY`/`ZXY`) and `SIUN`
(`rad/s`, `m/s²`) say the same thing in words. **Use `MTRX` for the axis map** rather
than a hard-coded string: it is the file telling us, and it is per-stream. ✅

`CameraOrientation` at ~25 Hz is GoPro's own per-frame estimate of where the camera
is pointing. ❓ its convention and accuracy are unknown, but it is the natural thing
to compare a horizon lock against, and it is free.

### Timing — the alignment is measurable, and it is good

Each stream carries `STMP` (the time of its **first** row, microseconds on the
camera's clock) and `TSMP` (that stream's rows **so far**, cumulative). So a row's
time runs forward from `STMP`:

```
t(row n) = STMP + n / rate        rate = median of the per-chunk ΔTSMP / ΔSTMP
```

(The rows run *forward* from `STMP`, measured: that placement keeps every join
strictly increasing with a ~1 ms margin, while the backward reading `STMP −
(TSMP − n) / rate` puts all 62 joins of a 64 s clip out of order. And the rate is
the per-chunk **median**: the camera stops mid-sample at the end of a clip — the
last gyro chunk carries 128 rows against a full ~1.04 s STMP span — which drags a
global first-to-last fit down to 790.6 Hz where the median gives 801.6. A chunk's
row count can also differ from its `TSMP` delta by 1, so nothing should derive
timing from chunk spans.)

and the camera clock is lined up with the video by fitting `STMP` against the packet
PTS of the same sample. Measured on four clips, including an 8-minute part with 464
samples (`tools/imu_align.py` in the sample tree): ✅

| clip | slope | offset (video 0 in camera time) | gyro residual | accel residual |
|---|---|---|---|---|
| GS012267 (64 s) | 1.000000 | +171.5 ms | max 0.61 ms, rms 0.36 ms | max 2.58 ms, rms 1.45 ms |
| GS012268 (58 s) | 0.999999 | +171.7 ms | max 0.62 ms, rms 0.36 ms | max 2.60 ms, rms 1.45 ms |
| GS022266 (482 s) | 1.000000 | +171.4 ms | max 0.63 ms, rms 0.36 ms | max 2.58 ms, rms 1.44 ms |
| GS062269 (part 6) | 1.000000 | +2412.97 s | max 0.62 ms, rms 0.36 ms | max 2.46 ms, rms 1.44 ms |

Read that table as: **the camera clock and the video clock are the same clock** (slope
1.000000, under 5 ppm over eight minutes — no `clockRate` correction needed), the gyro
aligns to well under a millisecond, and the offset is a constant ~171.5 ms. The
accelerator's residual is ~1.4 ms because its own sample boundaries are coarser, not
because the fit is bad.

The right way to hand this to `ImuTrack` is **not** one global offset: emit
`frameTimes` with one entry per GPMF sample (one per ~1.04 s), each pair being exact by
construction, and let `timeToFrame` interpolate between them. That uses the existing
model as designed and leaves nothing to the fit.

### Split files

The camera clock runs continuously across a segmented take, and that is the link the
container does not provide. On part 6 of a take, `STMP` at video 0 is **+2412.97 s**,
and the five missing parts of 482.56 s each account for 2412.80 s — leaving +0.17 s,
which is the same constant offset every first part has. ✅ So a part's position in its
take is recoverable from its own telemetry, to ~0.2 s, without the file naming it and
without the file names. That is exactly `Part::startUs` / `JoinImuTracks`' "the parts
run on one device clock", and here the clock is in the file.

## Calibration

The global-settings GPMF block (the `unknown` track at index 3/4, and `udta/GPMF`)
carries the per-camera lens calibration, and it is **constant across every clip from
this camera** — so no profile database and no extraction from the desktop app is
needed. ✅ Measured: `KLNS` (a 5-term radial polynomial per lens), `CTRX`/`CTRY`
(principal-point offsets), `MFOV = 100`, `SFTR`, and the lens pose `SHFX = 22.75`,
`SHFY = -0.13`, `SHFZ = -36.43`, `ANGX = 0.05`, `ANGY = 180.19`, `ANGZ = -0.97`
millimetres/degrees, `CALW/CALH = 4056×3040`.

These are the fisheye-domain calibration of the *lenses*, which is what the `.LRV`
path needs and what a parallax warp across the seam needs. The `.360` EAC path needs
none of it: the cube geometry is fixed by the projection.

`PMOD = [32, 1408, 1376]` looks like the stitch geometry — 1376 is the side region's
width in the strip (measured, see [[geometry]](#geometry)), 32 is side region − centre
face (half the 64 px overlap), and 1408 = 1376 + 32. ❓ unconfirmed which field is which, but it is the file stating the numbers
FFmpeg currently infers from the frame size, and worth a look before hard-wiring any
of them. `PRJT` is `'FSFB'` on two of these clips and `'EACO'` on two others — so the
claim that it is "`GPRO` or `EACO`" in 360 modes is incomplete, and what the values
mean is ❓ (GoPro's tag dictionary is behind a 403 from here).

## Plan

Each step names the measurement that says it worked, and none of them are committed to
until that measurement is written down. Steps 1–2 are the whole of "open a `.360`";
step 3 is where the project's own machinery takes over.

**1. `GoproReader` — container and pictures.** Two HEVC tracks (0 and 5) decoded in
parallel, each split into three 1344×1344 faces, assembled into a cube frame with the
64 px margins overlapped for blending. Deliverable: `ClipMetadata` with the right
streams, and frames that match a reference. Measurement: **the assembled sphere
matches `max2sphere`'s output and `v360_vulkan input=gopromax` on the same frame** —
all three are independent, and agreement between them is the strongest check available
without GoPro's own app. Cost: the decode is 2× 4096×1344 HEVC, which is the same
pixel rate as one 8192×1344 frame, so the existing 8K path is the budget.

**2. `GoproReader::readImu()` — the telemetry.** Parse the `bin_data` track with the
framing above; emit `ImuSample`s at the measured rates, `accel` in g, `gyro` in rad/s;
fill `axisMap` from `MTRX`; fill `frameTimes` from the per-sample `STMP`↔PTS pairs.
Measurement: **the residuals in the table above, reproduced by the C++ reader and
asserted in a test** — gyro rms under 1 ms against the packet PTS, slope within 10 ppm
of 1.0. This is the step that decides whether the IMU work is trustworthy, and it is
already measured here in Python.

**3. The cube→sphere stitch behind `IStitchSource`.** The roadmap's shape is right: a
cubemap picture source, not a new lens model. The per-pixel map is the EAC inverse
(textbook, and `libavutil` has the forward one), and the seam is a cross-fade over the
overlap with the parallax warp from `SHF*` as the quality step. Measurement: **the seam
metric** — hard cut 7–12, cross-fade 0.22–0.37, and the number to beat is whatever
FFmpeg's blend leaves on the same frame. Report same-lens and cross-lens boundaries
separately: only the cross-lens one is parallax-limited, and a single blended number
hides which one got worse.

*Built:* the map in `orbiscore::EacCubeStitcher` (CPU) and in the GPU stitch's
shared shader (`cubeSample` in `fisheye_stitch_common.glsl`, switched on by
`StitchGpuParams::eacCube`), so `stitch::NativeStitchSource` opens a clip whose reader
says `Projection::EquiAngularCube` on every path a fisheye clip has: Vulkan video
decode, the GPU equirect, the fused view (playback, export), the viewport's lens
frames, the privacy blur; the CPU stitch where there's no GPU (no privacy blur there).
The seam machinery stands down: there is no lens model to measure. Measured on
`GS012267.360` @ 5 s: the GPU and CPU stitches agree at PSNR 50.41 dB, and against
FFmpeg's stitch at 37.97 dB (MAD 1.10 more than 15° from the lens boundary,
resampling and colour conversion; 4.90 within 3° of it, the two cross-fades
differing). `tools/playback_bench` on an RTX 5070: decode 3.3 ms a pair, a 5376-wide
equirect 20.9 ms, the fused 1920 view 5.2 ms, an exact seek 51 ms — real time at
25 fps with room to spare. `tools/view_compare`: the fused view matches the equirect
path at 56–61 dB. `reframe_export` writes a 10 s 1080p HEVC (NVENC) from it.

**4. Reframe and export.** Nothing GoPro-specific: the sphere is a sphere, and the
existing `Viewer`/`ExportPipeline` take it from there. The one thing to check is the
horizon: `SROT = 20.74` (constant across clips) is presumably the camera's mount
rotation and `CameraOrientation` is GoPro's own pose estimate — both are candidates for
what the desktop app uses, and neither is understood yet. ❓

## What we still don't know

- ❓ `PMOD`'s three fields: which is the blend, which is the face width, and whether
  they change with the recording mode. Check against a 3K clip.
- ❓ `PRJT`'s values and what distinguishes `FSFB` from `EACO` on otherwise identical
  clips from the same camera.
- ❓ The per-frame `GP` record track (9363 fixed-size records of 1456 B for a 1614-frame
  clip, ids `0x0004/0x001b/0x0300/0x0310`) — exposure/shutter/ISO, presumably, for
  rolling-shutter work. Not decoded.
- ❓ `SROT`, `ZFOV`, `CameraOrientation`'s convention.
- ❓ MAX2. Nothing here has been seen on a MAX2 file; the mode table and the face
  layout for MAX2's 6.4K are 📖 from FFmpeg's filter, and its frame size is unverified.
- ❓ Whether the `.360`'s two video tracks are guaranteed to be frame-locked. They
  decode to the same frame count here, and `max2sphere` assumes it, but nothing in the
  container promises it.
