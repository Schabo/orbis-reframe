# Antigravity A1 file format

**Desk research (2026-10-15); measured on clips 2026-10-29**: three
contributed 8K flights. What's marked ✅ below is checked on them; the
modes we have no clips of are still desk research, from reviews and
Gyroflow's telemetry-parser. The app supports the A1 **experimentally**
from 2026-10-29: 8K clips open, stitch, stabilize (leveled by the drone's
own attitude) and export. The A1 is a 360 drone from Antigravity, a
brand incubated by Insta360, and writes the same `.insv` family; written as
differences from the X5 ([`insta360-x5.md`](insta360-x5.md), "X5 §n" below).
Markers as in [`README.md`](README.md).

## Streams

- 📖 Two 1/1.28" sensors. Reviews say **180°** lenses, but ✅ each lens
  images **~200°**, like the X5's: by the clip's own calibration the image
  circle's edge lies at ~100° off-axis all round (99-101° on lens 1;
  101-105° on lens 0, where the frame clips the circle), measured on a
  frame of `_011` and `_012` by where the picture goes dark. The stitch's
  200° default (X5 §5.4, used when the file has no lens profiles) fits.
  The A1's frame is cropped closer to the circle than the X5's: the circle
  nearly fills the 3840² stream.
- 📖 360 modes, H.264 or H.265, up to 170 Mb/s:

  | mode | frame | fps | per lens (⚠️) |
  |---|---|---|---|
  | 8K | 7680×3840 | 30 / 25 / 24 | 3840² |
  | **5.2K** (new) | 5248×2624 | 60 / 50 / 30 / 25 / 24 | **2624²** |
  | 4K | 3840×1920 | 100 / 30 / 25 / 24 | 1920² |

- ✅ **One `.insv` holds both lenses** as two square HEVC Main streams
  (8-bit, full range, BT.709 on our 8K clips), lens 0 (looking **down**)
  first, lens 1 (looking **up**) second. No audio track; a `mov_text` track
  holds only a sensor frame counter per frame (`F:13776`, ...).
- ✅ File names: **`VID_<date>_<time>_<n>.insv`**, with no lens token
  (the X5's is `VID_…_00_…`), and the proxy beside it
  **`PRX_<date>_<time>_<n>.prx`**: the same name with `PRX` and `.prx`, both
  fisheyes side by side at 2560×1280 (the X5's `.lrv`, X5 §3.2), with a
  trailer of its own (records 1 and 2).
- ✅ **The drone in the picture**: on our clips, only a light bar with a
  dark edge at the very rim of lens 0 (the downward one), 94-97° off its
  axis, in the same place on every clip -- the drone's body or an arm
  seen past the horizon. It lies at the outer end of the seam's blend
  band, where lens 0 hardly counts (the edge fade), so it doesn't show in
  the stitched picture (checked at the spot, depth-aware seam off and on).
  Nothing of the drone near the seam elsewhere, nor in lens 1. ❓ Landing
  gear or a take-off on the ground.

## Trailer

- ✅ The same 78-byte footer as the X5 (X5 §4.1): magic
  `8db42d694ccc418790edff439fe026bf`, trailer version 3. The footer's
  header names the Offsets table (type 0, 570 bytes: 57 entries).
- ✅ **The Offsets table (X5 §4.2) is the only index.** Each record's payload
  starts a **256 KiB slot** (the entries' offsets are 0x40000 apart, a few
  records packed between them), and **no six-byte header follows a
  record**: the bytes after it are zeros or the next record's data. So the
  walker takes each record's type and size from its table entry, as
  telemetry-parser does; reading them from the header after the record
  finds nothing. Checked on all three clips and the `.prx` proxy.
- ✅ Records in the clips: types 1 (Metadata, protobuf), 2 (thumbnail), 3
  (IMU), 4 (exposure), 7 (GPS), 9, 11, 14, 23, 32, 33, 37 (attitude), 38
  and 56; the proxy holds 1 and 2. Of the ones the X5 doesn't have, 37 is
  read and 14 / 56 partly understood (below); 23, 32, 33 and 38 hold
  timestamped floats, meaning ❓ (a flight log?).
- ✅ Record 1 is the X5's protobuf (X5 §5.1); on our clips: model (field 2)
  `Antigravity A1`, firmware `10.0.397.3_5.5`; field 27 `{5248, 5248, 5248,
  5248}`, so the focal reference is 5248; only the **40-token `offset_v3`**
  MEI calibration (field 54, X5 §5.3; no 56-token string), in a 10496×5248
  side-by-side frame, `xi` 1.504, lens type 155; fields 24, 25 (readout
  ~21.25 ms), 28 (3.7 ms), 62 (raw IMU) and 65 (±32 g, ±2000 °/s) as on the
  X5, and the IMU (~1 kHz) and exposure records decode with the X5's
  layout, in µs. Field 7 is **`YYYYMMDDhhmm`**, without the X5's seconds, so
  the recording time comes from the file name, which has them. Fields 195
  / 196 are 30000 / 1001, the video's frame rate as a fraction; fields 51
  (1) and 64 (2) are set, meaning ❓. The serial is in field 1 as on the
  X5.
- ⚠️ The calibration's extrinsics are yaw / pitch / roll ≈ (−177.5, −89.95,
  90.4) and (−179.4, −89.9, 89.4): pitch −90° puts yaw and roll about the
  same axis, so the order they compose in matters (it doesn't on the X5).
  Pitch, then yaw, then roll (X5 §5.3) leaves the lens axes opposite to
  0.2°, and the per-clip seam fit converges to 0.02° on all three clips
  with small corrections (lens 1 pitch +0.14 to +0.24°, roll +0.40 to
  +0.42°, centre −0.06 to −0.08 %, radius −0.17 to −0.20 %). Before the
  fit the lenses are 0.24-0.53° apart at the seam (X5: ~0.17°). The app's
  per-clip measurement gives the same on all three clips, over each whole
  clip (0.47-0.52° -> 0.02°).
- ✅ The lenses are **71 mm apart** (lens 1 `tz` −0.0711; X5: 32 mm), lens 0
  in front along its own axis as on the X5: the depth-aware seam lines up
  houses, poles and wires along the horizon when the drone flies low
  (`_011` at 1 s, confidence 0.89), and with the sign flipped
  (`insv_stitch --baseline -0.0711`) they double again.
- ⚠️ **How the lenses sit in the picture** (`InsvModel::mountDeg`, the
  stitch's output turn): not in the file -- the calibration places the
  lenses relative to each other, as on the X5, where lens 0 faces forward.
  Stitched the X5's way, the A1's lens axes lie on the horizon. Turned so
  lens 0 looks straight down and the picture's forward is the drone's
  nose: yaw −90°, pitch 90°, roll 0 (the X5's: 180, 0, 0). The nose found
  from the horizon on 2026-10-29: a drone flying fast tilts nose down, so
  the horizon in the stitched picture is highest toward the nose. A fitted
  sinusoid on six frames of clip `_010` at a steady 15.7 m/s (62-72 s,
  tilt 16-19°) puts the high point at −86° ± 6° from lens 0's side; taken
  as −90°, the body square to the lens frame. It puts the trailer's
  thumbnail (type 2) of `_011` at the picture's centre too; `_012`'s
  thumbnail looks elsewhere, so the thumbnail is a view, not the nose.
- ✅ **IMU axes `-x,y,-z`** (`InsvModel::imuAxes`): sphere X ← −raw x,
  Y ← raw y, Z ← −raw z, in the sphere the A1's mount gives. Measured
  2026-10-29 as on the X5 (X5 §6.3) and the ONE X2: four unstabilized
  horizon views (yaw 0 / 90 / 180 / 270°, 90° diagonal field of view) of
  `_010`, the camera rotation per frame interval from tracked features
  (rotation-only Kabsch fit), against the gyro integrated over the same
  interval. On 40-60 s (turns at 100-150 m): 1.3° from the free fit, gain
  1.01 / 0.97 / 0.99, every view picks it (1.7-2.7°), and the rotation left
  per frame drops from 1.20° to 0.074°; on 10-40 s the same map, 2.7°. The
  runner-up is 90° away. Raw +y is also where the accelerometer puts "up"
  in a hover (0.92 g of it).
- ✅ **Thumbnail, record type 2**, in a layout of its own: a 64-byte
  header -- `u32 0`, `u32 64` (the header's size), `u32` the picture's
  size in bytes, then zeros but for width (`u32` at byte 24), height (28)
  and row stride (32) -- then a **flat view, not an equirect**, NV12:
  **256×192** in the clips, **768×768** in the `.prx` proxy, the stride
  equal to the width in both (the X5's: a 40-byte header and a 1280×640
  equirect, X5 §4.3). The view looks ahead and down at the ground, a wide
  lens look; on `_011` the proxy's picture shows the same view as the
  clip's, cut square. Checked 2026-10-29 on all three clips and their
  proxies. **Full range** like the video: 2-14% of the clips' pixels are
  above 235, a sky in the video's lens 1 likewise (10%), and the pictures
  look right decoded so. The matrix is ⚠️ BT.709, the video's, not
  measured: a view fitted from the stitched first frame matched no better
  under BT.601 or BT.709. The app shows the picture on the A1's cards
  (Media tab, project library), cut to the card's shape.
- ✅ **GPS, record type 7**, in the X5's layout (X5 §8): a fix every
  **1.6 s** on all three clips (the X5 with the GPS remote: once a second),
  one record per fix, from the first one the drone has after recording
  starts. Field 98 (X5 §5.1), which on the X5 gives the first frame's Unix
  time, is absent, and nothing else in the file ties the GPS clock to the
  video: field 36 is the first fix's own Unix time (to the ms on all
  three), the MP4's `creation_time` is in whole seconds, and no other
  record holds a Unix time.
- ✅ **The GPS lined up with the video by its speed** (`AlignGpsBySpeed`,
  `Gps.h`): the fixes' speed against record 37's horizontal speed, timed
  in video time by field 24, over every shift; the best to the ms. On
  `_010` / `_011` / `_012` the first fix is **1.129 / 0.911 / 0.526 s**
  into the video, matched to 0.20 / 0.06 / 0.08 m/s RMS, 0.8-2.0 m/s a
  second either way. A check that doesn't use the fit's own clip: the
  three put the drone's clock (field 24) at the same offset from Unix time
  to within 31 ms. A shift is taken only when it is clear: at least 5
  fixes over the attitude track, their speeds spread by 0.5 m/s or more,
  matched to 1 m/s RMS or better, and every shift a second or more away
  fitting at least twice as badly (+0.1 m/s). A hover, or a clip too
  short for 5 fixes (8 s), stays without GPS in titles and gauges, and the
  log says why.
- ✅ **Sync**: the X5's formula (field 24 + field 28, X5 §7.1) holds; the
  app's per-clip fine-tune finds +0.9 to +1.2 ms on all three clips, and
  the image fit above agrees within ~1 ms.
- ✅ **Attitude, record type 37** (the walker's id 0x2500): the flight
  controller's orientation, 50 Hz, 36 bytes each: `u64` µs on the IMU's
  clock (4-8 ms behind the gyro), a unit quaternion as x, y, z, w (4 ×
  f32), body to world -- body x the nose, y right, z down; world north,
  east, down -- then the velocity north, east, down in m/s (3 × f32; its
  horizontal speed is the GPS speed). In fast flight the body pitches
  −25° to −28°. Measured 2026-10-29 on all three clips by matching the
  attitude's rotation per 20 ms step against the gyro integrated over the
  same step: body ← IMU is **field 69**'s rotation, transposed -- (w, x,
  y, z) = (0.5, 0.5, −0.5, 0.5), an exact axis permutation -- then a
  **9.5-9.9° pitch up** about the body's right axis, the camera tilted in
  the airframe (26° body pitch − 9.7° = the 17° the camera's horizon
  tilts). 0.09-0.14° per step is left. The tilt isn't in the file; the
  app takes 9.7° (`InsvModel::attitudeTiltDeg`, one drone measured).
- ✅ **The accelerometer reads the drone's acceleration with gravity.** In
  steady flight it points up however the drone is tilted (thrust and drag
  balance gravity: `_010` 62-64 s at 15.6 m/s, 0.2-1.3° from the
  attitude's up), but the drone speeds up, brakes and turns most of the
  time, which moves it 4-35° off up (2 s means, `_010`). The attitude's
  up plus record 37's velocity, differentiated, predicts it to ~1° (0.3-4°
  per 2 s), which checks the attitude and the tilt above. Leveled by the
  accelerometer, Horizon lock's up was up to 15° (pitch) and 9° (roll)
  off per 10 s on `_010`, 5-8° on `_011`, under 1° on `_012`; leveled by
  the attitude, 0.2° or less on all three (`residual_stab --diagnose`,
  block [6]). The app levels by the attitude (Horizon lock, Direction
  lock, Lean follow's auto-level).
- ⚠️ **Other orientation records** (layouts from the bytes, meaning
  inferred, not used): type 14 has one 36-byte entry per video frame with a
  unit quaternion in it; type 56 holds the same orientation in another
  layout (its first quaternion is type 14's, sign flipped). ❓ Field 91 (a
  unit quaternion, none of the attitude, the IMU mount or their products).

## Modes and sample clips

All the A1's modes 📖 (Antigravity's spec table as retailers and reviews
reprint it, and the spring 2026 update for timelapse; Sources). Our clips:
three 8K clips from one contributor (firmware `10.0.397.3_5.5`, 2026-10-29).
The status words are the X5's (X5 §3.4).

| Mode | Spec | Files | Our clips | Status |
|---|---|---|---|---|
| 360 Video 8K | 7680×3840, 30 / 25 / 24 | ✅ one `.insv` with both lenses (two 3840² HEVC streams), and a `PRX_….prx` proxy | 3, 29.97 fps, H.265, standard colour | experimental: opens, stitches, stabilizes and exports; GPS lined up by its speed |
| 360 Video 5.2K | 5248×2624, 60 / 50 / 30 / 25 / 24 | as above | none | **wanted**: a lens size no Insta360 camera has (2624² ⚠️), and 60 fps |
| 360 Video 4K | 3840×1920, 100 / 30 / 25 / 24 | as above | none | **wanted**: 100 fps |
| 4K Slow Motion | 3840×1920, 30 / 25 / 24 | ❓ | none | **wanted**: whether the file is tagged at the playback rate or the capture rate (100 ⚠️) |
| H.264 encoding | up to 170 Mb/s, as H.265 | as above | none | **wanted**: one clip in each codec |
| Timelapse | added by the spring 2026 update; ❓ sizes and intervals | ❓ | none | wanted |
| 360 Photo: Standard, HDR, Burst, AEB | 55 MP 10496×5248, 14 MP 5248×2624 | `.insp`; `.dng` | none | wanted, low |
| Internal storage (20 GB) vs microSD | | ❓ the same files | none | low: say which a clip came from |
| Audio | not in the spec table | ✅ no audio track in our clips | 3 | done: opens and exports as picture only |
| Drone in the picture (props, arms, landing gear) | | | 3, in flight | done in flight: a part at lens 0's rim only, outside the stitched picture; wanted: a take-off or landing |
| Replaceable lenses | | | none | wanted, low: whether the calibration in the file follows a lens swap |
| IMU axes | | | `_010` (flight turns) | done (`-x,y,-z`) |

Cinematic, Normal and Sport are flight modes, not recording modes; they
change how the drone moves, which matters only for the stabilization check.

The most useful first set, 10–30 s each, straight from the card with the
whole folder, as in "Sample clips wanted"
([`README.md`](README.md#sample-clips-wanted)):

1. 8K 30, H.265, hovering for a few seconds, then slow deliberate turns:
   yaw left-right, pitch up-down, a roll.
2. 5.2K 60.
3. 4K 100, and 4K Slow Motion.
4. One clip in H.264.
5. A short timelapse.

With the drone's firmware version, and whether the clip came from the
internal storage or the microSD card.

## Sources

- Shotkit, Antigravity A1 review (specs, formats):
  <https://shotkit.com/antigravity-a1-review/>
- Space.com, Antigravity A1 review (sensors, weight):
  <https://space.com/technology/antigravity-a1-drone-review>
- Antigravity's spec table (video and photo modes, codecs, storage), as
  reprinted by apollomaniacs:
  <https://www.apollomaniacs.com/ipod/howto_ar_drone_basic_antigravity_a1.htm>
- No Film School, the spring 2026 update (adds timelapse):
  <https://nofilmschool.com/antigravity-a1-drone-spring-update>
- Gyroflow telemetry-parser, `src/insta360/mod.rs` (the slotted,
  descriptor-less layout): <https://github.com/AdrianEddy/telemetry-parser>
