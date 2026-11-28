# GoPro MAX / MAX2 — `.360`

**Measured on a contributor's MAX clips (2026-11); the app opens them,
experimentally** (`camera::GoproReader`, `orbiscore::EacCubeStitcher`, the
stitch shader's `cubeSample`). Markers as in [`README.md`](README.md): ✅ is
read by that code and checked on our clips (*Modes and sample clips*), all
from one MAX on firmware `H19.03.02.02.00`; nothing about the MAX2 has met a
real file yet. Every fact says where it came from; *Sources* lists them.

The short version: **the camera stitches.** A `.360` isn't two fisheye
pictures like the other cameras here: the MAX turns each lens's picture into
three faces of an **equi-angular cubemap** (EAC) and writes the six faces
into two HEVC streams. What the app adds is the cube→sphere map, the blend
across the one lens boundary, and the IMU work.

## 1. Files and streams

- ✅ A take is a `.360` with a `.LRV` proxy and a `.THM` preview beside it,
  one name (`GS012267.360`): `GS`, two digits for the chapter, four for the
  take (ffprobe; the files on the card).
- ✅ The container is QuickTime (`ftyp` `qt  `), written `moov` first, so
  the track table is at the front. The maker note near the start names the
  camera (`GoPro Max`), its serial and firmware (ffprobe; the bytes).
- ✅ Seven streams (ffprobe on all six files):

  | index | what |
  |---|---|
  | 0 | HEVC Main, **4096×1344**, 8-bit full range: lens A's three faces |
  | 1 | AAC, stereo, 48 kHz |
  | 2 | `tmcd` timecode |
  | 3 | `gpmd`: the sensor telemetry, one GPMF sample a packet (§3) |
  | 4 | `fdsc`: per-frame records, not decoded |
  | 5 | HEVC Main, **4096×1344**: lens B's three faces |
  | 6 | PCM float, 4 channels, 48 kHz (the four microphones) |

  The two video streams **aren't adjacent**: "the first two video streams"
  is right, "streams 0 and 1" isn't.
- ✅ The two video streams have the same frame count on every file (ffprobe);
  the reader pairs them frame by frame. ❓ Nothing in the container promises
  it.
- ✅ The `.LRV` holds the **two raw fisheye circles** side by side, H.264
  1408×704: radius 352 px, centres (352, 352) and (1056, 352), touching,
  no padding (measured on its frames). Not the cube, so playback doesn't
  use it (the picture must be the same sphere the same way).
- ✅ The `.THM` is a 704×352 equirectangular JPEG of the whole sphere; the
  media pool shows it.
- 📖 The MAX has GPS (retail spec listings). ✅ None of our files has a GPS
  stream: no `GPS5`, `GPSF` or `GPSU` in any sample, nor a GPS setting in
  the global settings (§6). ⚠️ So GPS was off when they were filmed; a
  camera with GPS on writes `GPS5` even before a fix (GoPro's GPMF
  documentation, for the HERO5 on).

## 2. Geometry

✅ Each stream is one strip of three cube faces, `SIDE | CENTRE | SIDE`,
4096 = 1376 + 1344 + 1376. With x right, y forward (lens A), z up:

```
stream 0:  LEFT  | FRONT | RIGHT     (FRONT upright; the strip runs on left to right)
stream 1:  DOWN  | BACK  | TOP       (turned: BACK's right edge meets TOP, its left DOWN)
```

In a face's (u, v) ∈ [−1, 1], u along the strip and v down it, with
e = atan(component / dominant) · 4/π:
FRONT (e_x, −e_z), RIGHT (−e_y, −e_z), LEFT (e_y, −e_z), BACK (e_z, −e_x),
TOP (e_y, −e_x), DOWN (−e_y, −e_x).

- ✅ **The centre face** is 1344 px for 90°, square.
- ✅ **A side face** is 1312 px for 90°, cut in half where the two lenses
  meet (the plane y = 0), with a **64 px overlap** between the halves: the
  half nearer the centre face at 0–656 of its 1376 px region, the outer half
  at 720–1376. Each half runs on into the overlap — its lens's view past the
  boundary — and the stitch cross-fades them over it.
- ✅ **Equi-angular**: a pixel's direction is linear in angle across the
  face (the e above), not in its tangent. A rectilinear reading gives a
  curved horizon and a bad seam.
- ✅ **The cube is exactly 90°**: the faces' directions are the six axes.

*How it was measured*: on `GS012267.360` at 5 s, FFmpeg 9.1's own stitch of
the same frame (`v360_vulkan`, `input=gopromax`) as the reference, used as
a black box — its output, not its code. For each face of our equirect,
every strip region of both streams, all eight symmetries of a square and the
face's offset and scale in the region were tried, and the one whose picture
correlates best with FFmpeg's kept: FRONT at NCC 0.998, BACK 0.999, each
half of each side face ≥ 0.9994, the runner-up of every face ≤ 0.62. The
side faces' 1312 px span and 64 px overlap are where each half fits. The
whole frame then matches FFmpeg's at 37.97 dB (MAD 1.10 more than 15° from
the lens boundary — resampling and colour conversion — and 4.90 within 3° of
it, where the two cross-fades differ). An earlier layout, max2sphere's
(square 1376 px side faces, a 32 px band), put face edges through the scene;
it is not used.

- ✅ **The file states the layout**: the global settings' `PMOD` is
  `[32, 1408, 1376]` on all six files (§6). ⚠️ Read as half the overlap
  (32) and a side region (1376), the centre being what the side regions
  leave of the strip: that is the measured layout, and it's how the reader
  takes the layout from the file. ❓ `1408` is the centre face plus the
  overlap, a side region plus half of it, and the `.LRV`'s width alike; one
  mode can't tell which. The reader doesn't use it.
- ✅ **The seam.** The cross-fade over the overlap makes the far field
  continuous: on `GS012267.360` at 5 s the luminance gradient across the
  boundary is the same as 3–10° away from it (ratio 1.00, ours and FFmpeg's
  alike), and the hard cut's face edges measure **0.46** on the seam
  metric (`EacCubeStitcher::SeamRatio`: the median jump across a face
  boundary over the median of the busiest gradients inside the faces; the
  wrong layout measured 12–14). Near objects still disagree across the
  boundary: the lenses are ~22.75 mm apart sideways and ~36.4 mm along the
  axis (`SHFX` / `SHFZ`, §6), parallax no blend can hide.

## 3. Telemetry

- 📖 GPMF is a big-endian KLV tree: a FourCC, a type character, a one-byte
  structure size and a two-byte repeat count, then size × repeat payload
  bytes padded to 4. Type 0 (null) is a nest: its payload is more KLV
  (GoPro's GPMF documentation). ✅ Walked by those sizes, the telemetry
  track and the settings block of all six files read to their exact ends
  (`orbiscore::WalkGpmf`, `orbis-core/gopro/Gpmf.h`).
- ✅ Each sample is one `DEVC`, and its streams are sibling `STRM`s in it.
  The MAX writes one type that isn't in the documentation's list, `#`, on
  `DISP`; the walk steps over it by its size.
- ✅ One GPMF sample a `gpmd` packet, ~1.04 s each (63 for `GS012267.360`),
  each with fifteen streams by `STNM`: Accelerometer, Gyroscope,
  Magnetometer, CameraOrientation, ImageOrientation, Gravity Vector,
  DisparityMatrix, Sensor ISO, Exposure time, White Balance RGB gains and
  temperature, Image uniformity, AGC audio level, Microphone Wet, Wind
  Processing (our parse of all six files).
- ✅ The motion streams, measured on every file:

  | stream | rate | `SCAL` | unit |
  |---|---|---|---|
  | Gyroscope (`GYRO`) | 801.6 Hz (median, §3.1) | 939 | rad/s = raw / 939 |
  | Accelerometer (`ACCL`) | ~200 Hz | 417 | m/s² = raw / 417 |

  A resting camera reads 9.94 m/s², so the unit is m/s² and the reader
  divides by g. 📖 `SCAL` is the divisor, `SIUN` the unit, `STNM` the name
  (GoPro's GPMF documentation). ✅ Gyro in rad/s against the camera's own
  `CORI` attitude: median ratio 0.995.
- ✅ Both carry `MTRX = [0,−1,0, 1,0,0, 0,0,1]`, the sensor's axes in the
  camera body, and `ORIN` / `ORIO` say the same in letters; the reader
  applies `MTRX`. ❓ GoPro's documentation doesn't list `MTRX`, `ORIN` or
  `ORIO`; their reading here is that applying `MTRX` makes the gyro agree
  with the picture (§4).
- 📖 `CORI` is the camera's orientation since the start, `IORI` the image's
  relative to the camera body, both quaternions a frame; `GRAV` gravity;
  `DISP` a 1-D depth map of what both lenses see (GoPro's documentation,
  for the MAX). ✅ `IORI` is the identity on every frame of every file (0.000°
  at most) while `CORI` turns up to 180°: see §7.

### 3.1 Timing

- ✅ Each stream carries `STMP`, the camera clock's time of its first row
  (µs), and `TSMP`, its rows so far. 📖 `TSMP` is the count since recording
  started (GoPro's documentation; it lists `STMP` for the Fusion only).
  ✅ A row's time runs forward from `STMP`: t(row n) = STMP + n / rate.
  Read backwards (`STMP − (TSMP − n) / rate`) every join of a clip is out of
  order; read forwards each chunk's rows end ~1 ms before the next `STMP`.
- ✅ The rate is the **median** of the chunks' ΔTSMP / ΔSTMP: the camera
  stops mid-sample at a clip's end (the last gyro chunk of `GS012267.360`
  has 128 rows against a full ~1.04 s span), which drags a first-to-last fit
  to 790.6 Hz where the median gives 801.6.
- ✅ The camera clock and the video clock are one clock. `STMP` against the
  same sample's packet time, a straight-line fit:

  | clip | slope | video 0 at | gyro residual | accel residual |
  |---|---|---|---|---|
  | GS012267 (64 s) | 1.000000 | +171.5 ms | max 0.61, rms 0.36 ms | max 2.58, rms 1.45 ms |
  | GS012268 (58 s) | 0.999999 | +171.7 ms | max 0.62, rms 0.36 ms | max 2.60, rms 1.45 ms |
  | GS022266 (482 s) | 1.000000 | +171.4 ms | max 0.63, rms 0.36 ms | max 2.58, rms 1.44 ms |
  | GS062269 (chapter 6) | 1.000000 | +2412.97 s | max 0.62, rms 0.36 ms | max 2.46, rms 1.44 ms |

  The reader therefore gives `ImuTrack` one `frameTimes` pair a sample (the
  packet's time, that sample's gyro `STMP`), exact by construction, and
  `captureScale` 1. `camera_test` checks slope and residuals on
  `GS012267.360`.

## 4. Gyro axes and picture lag

- ✅ **Axes**: body → the stitch's world is `-y,x,z`
  (`orbiscore::ImuTrack::axisMap`; sphere x right, y forward, z up ← body
  −y, −z, x). Not in the file. *Measured*: FFmpeg's equirect of frames k
  and k + 1 (oriented as ours, §2); frame k turned by the gyro's turn over
  the frame under each of the 24 proper signed permutations; each scored by
  how much of the photometric change to frame k + 1 it explains. One map
  wins on both clips tried: 30.5 % explained on `GS012267.360` (runner-up
  3.4 %), 32.5 % on `GS012268.360` (runner-up 9.7 %); the X5's `z,-x,-y`
  explains none. In the editor's stabilizer (`tools/residual_stab`,
  `GS012267.360` 0–15 s, forward view): Direction lock leaves 0.54°/frame
  against 1.09 unstabilized (`z,-x,-y`: 1.12, worse than nothing); Horizon
  lock's up stays within 1.6° roll / 0.7° pitch of gravity.
- ✅ **Lag**: the picture's motion lines up with the gyro **10 ms** after
  the frame times. *Measured* the same two ways: the frame-to-frame fit
  scores best at +10 ms (10.40 / 9.89 / 10.43 at 0 / +10 / +20 ms on
  `GS012267.360`), and Direction lock leaves 0.618 / 0.554 / **0.537** /
  0.571 / 0.627°/frame at 0 / +5 / +10 / +15 / +20 ms there, 0.587 → 0.562
  at 0 → +10 ms on `GS012268.360`. ⚠️ Most likely the sensor's middle row
  read that long after the frame's time (a rolling shutter).
- Both are in the reader's table for the MAX's 5.6K (`camera::GoproMode`).
  A model or mode without a row opens with them, marked untested.

## 5. Chapters

- ✅ A long take is split into chapters, `GS01nnnn.360`, `GS02nnnn.360`, …
  (the take's four digits the same). The container doesn't link them: no
  edit list, no chapter list, nothing naming the next file.
- ✅ The camera clock runs on across a take: on chapter 6 of a take,
  `STMP` at video 0 is +2412.97 s, and five 482.56 s chapters before it
  account for 2412.80 s, leaving +0.17 s, the same offset every first
  chapter has (§3.1). So a chapter's place in its take is in its own
  telemetry.

## 6. Global settings

The global settings are one GPMF block in `moov/udta/GPMF` ✅ (our parse;
the reader reads `PMOD` from it). On all six files, the same but for
nothing that matters here:

- ✅ `PMOD = [32, 1408, 1376]` (§2). ❓ Not in GoPro's documentation.
- ✅ `PRJT = "EACO"` in the `.360`, `"FSFB"` in the `.LRV`: the cube and
  the two fisheyes. 📖 GoPro's documentation: "Lens Projection", "GPRO" or
  "EACO" for 360 modes (it doesn't list `FSFB`).
- ✅ `EISE = "N"`, `EISA = "N/A"`. 📖 `EISE` is electronic stabilization,
  N off, Y on; `EISA` which kind was applied (GoPro's documentation).
- ✅ `SROT = 20.74`. 📖 The sensor's read-out time in ms (GoPro's
  documentation). ❓ Not used yet: the camera made the cube from the
  rolling-shutter picture, so there's no per-row correction left to apply.
- ✅ `ZFOV = 0`, `RATE = "2_1SEC"`. ❓ What they mean on the MAX.
- ✅ The lens calibration, the same on every file: `KLNS` (5 terms a
  lens), `CTRX` / `CTRY`, `MFOV = 100`, `SFTR`, the lens pose `SHFX =
  22.75`, `SHFY = −0.13`, `SHFZ = −36.43`, `ANGX = 0.05`, `ANGY = 180.19`,
  `ANGZ = −0.97`, `CALW` / `CALH = 4056 × 3040`. ⚠️ Millimetres and degrees,
  and the lenses before the camera made the cube (the `.LRV`'s picture);
  the `.360` needs none of it. ❓ None of these is in GoPro's documentation.

## 7. Stabilization in the camera

✅ **The camera doesn't stabilize the 360 picture on our clips.** `EISE` is
`N` on every file (§6), and `IORI` — the image's orientation relative to
the camera body — is the identity on every frame (0.000° at most), while
`CORI`, the camera's own, turns by up to 180°. The picture moves with the
camera, and the gyro explains its motion (§4). 📖 GoPro sells the MAX with
"Max HyperSmooth" and Horizon Leveling (its product pages): for its
single-lens HERO mode and the reframed export, as far as these files show.
❓ A clip filmed with HyperSmooth on in a 360 mode, if the camera offers it,
would say whether `EISE` / `IORI` change.

## Modes and sample clips

📖 360 video at 5.6K30 and 3K60 (GoPro's MAX comparison chart). ✅ Our
clips are 5.6K at 25 fps; ⚠️ the camera's 50 Hz setting, as GoPros offer.
**Enough** means examined here and what the reader keeps per mode (the
layout, the IMU axes, the lag) checked on it.

| Mode | Our clips | Status |
|---|---|---|
| 360 5.6K, 25 fps | `GS012267` (65 s), `GS012268` (58 s), `GS012266` (three chapters, 1317.7 s), `GS062269` (chapter 6 alone); fw `H19.03.02.02.00`, GPS off | enough |
| 360 5.6K, 30 fps | — | wanted: the same layout and lag? |
| 360 3K, 60 fps | — | **wanted**: its stream size, `PMOD`, the lag |
| MAX2, any | — | **wanted**: model name, stream size, `PMOD`, axes |

## Open questions

- ❓ `PMOD[1]` (§2), and whether `PMOD` changes with the mode (a 3K clip
  would say).
- ❓ `fdsc`, the per-frame record track: not decoded.
- ❓ `SROT`'s use, `ZFOV`, `RATE`, and what `CORI`'s convention is.
- ❓ The MAX2: nothing here has met one of its files.
- ❓ Whether the two video streams are guaranteed frame-locked.

## Sources

- Our MAX clips (*Modes and sample clips*): streams and frame counts by
  `ffprobe`; the maker note and boxes by their bytes; the telemetry and
  global settings parsed as GPMF; the layout fitted against FFmpeg's
  stitch, the axes and lag against the picture's own turn, as §2 and §4
  describe.
- GoPro, GPMF documentation (the framing, `TSMP`, `SCAL`, `SIUN`, `STNM`,
  `CORI`, `IORI`, `GRAV`, `DISP`, `EISE`, `EISA`, `PRJT`, `SROT`, `GPS5`):
  <https://github.com/gopro/gpmf-parser/blob/main/README.md>
- GoPro, MAX / HERO13 Black / MAX camera comparison (360 modes 5.6K30,
  3K60):
  <https://fotoforma.pl/userdata/public/assets/Kamil/GoPro/GoPro%20MAX%20360/MAX_H13Black_MAX_CameraComparison_en-US_REVA.pdf>
- GoPro, MAX product page (Max HyperSmooth, Horizon Leveling):
  <https://gopro.com/en/gp/shop/cameras/buy/max/CHDHZ-203-master.html>
- Retail spec listings saying the MAX has GPS:
  <https://www.currys.co.uk/products/gopro-max-360-5.6k-action-camera-10280301.html>,
  <https://m.cdw.com/product/gopro-max-action-camera/5861939>
- FFmpeg 9.1, `v360_vulkan` with `input=gopromax`, used as a black-box
  reference picture for §2: <https://ffmpeg.org/ffmpeg-filters.html#v360>
- trek-view's max2sphere, whose layout was tried first and doesn't match
  (§2): <https://github.com/trek-view/max2sphere>
