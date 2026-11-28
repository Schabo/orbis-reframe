# Insta360 X4 and X4 Air file format

**Desk research (X4 2026-10-15, X4 Air 2026-11-24), and three X4 Air files
(2026-11-27 and 28): no X4 file examined.** From public specs, Insta360's
manuals, launch coverage and Gyroflow's telemetry-parser, set against what
our code assumes, plus the log of one X4 Air owner who opened a clip with
orbis-reframe 0.10.1, and X4 Air clips of our own at 8K 30, 6K 30 and 4K 50
(*The X4 Air files we have*, below). Written as differences from the X5
([`insta360-x5.md`](insta360-x5.md), "X5 §n" below). Markers as in
[`README.md`](README.md).

The X4 Air (October 2025) is a lighter, cheaper body of the X4's family, not
an X4 with a new name: other sensors, other modes, replaceable lenses. Its
files are the same kind -- one `.insv` per take with both lenses -- so both
cameras are in this document; where they differ, it says which.

## Files and streams

- 📖 One `.insv` per take holding both lenses, with the same
  `VID_…_00_…` / `LRV_…_01_…` naming as the X5 (X5 §1). Insta360's file
  format pages name the X6, X5, X4 Air and X4 as one-file cameras; the X3 is
  the last that wrote a file per lens.
- 📖 X4: recordings split every 29 min 59 s, as on the X5 (X5 §1).
- ⚠️ X4 Air: splits too, presumably at the same length (Insta360's X4 Air
  pages don't say). The owner's take is two files, `VID_…_211023_00_038`
  and `…_00_039`, 2003.235 s in all -- about 1799 s and 204 s if it splits
  like the X5. Our reader joined them by the trailer's file group (X5 §5.1
  field 26: same serial, same group, a higher index), so the X4 Air writes
  that group as the X5 does.
- 📖 Codecs H.264 and H.265, both cameras. Maximum bitrate: X4 200 Mb/s,
  X4 Air 180 Mb/s (8K 30 at "high" 180 Mb/s, "standard" 120 Mb/s; 6K 30 at
  140 / 100 Mb/s, the X4 Air manual's file-size FAQ).
- 📖 Colour profiles (in-camera filters) Standard, Vivid and Flat on both;
  **no I-Log**. Insta360's X-series colour page lists both cameras among
  those without a log mode ("applying a LUT will not produce correct
  results") and the X4 Air's video as 8-bit.
- ✅ X4 Air: our clips (8K, 6K, 4K 50) are H.265 Main, **8-bit**, full
  range, BT.709 (ffprobe), as Insta360 says.
- ❓ Bit depth of the X4. Insta360 doesn't say; one third-party listing
  claims 10-bit. The app's GPU stitch takes 8-bit and 10-bit (see
  [`insta360-x6.md`](insta360-x6.md)).
- 📖 Audio 48 kHz 16-bit AAC on both. Modes: X4 Auto Wind Noise Reduction,
  Active Wind Reduction, Stereo, 360 Direction Focus; X4 Air Auto Wind
  Noise Reduction, Voice Focus, Stereo, 360 Audio.
- ⚠️ Per lens: 3840² at 8K, 2880² at 5.7K, 1920² at 4K (the X5's sizes),
  and on the X4 Air **3008² at 6K** (6016×3008, the X6's 6K). ✅ X4 Air:
  two 3840² streams at 8K, 3008² at 6K, 1920² at 4K in our clips. The app
  names a
  clip's mode by its lens size, from the model's row in
  `orbiscore::InsvModel`; neither camera has a row, so both get the general
  one, which names 3008² 6K as the X6's row does.
- ✅ The X4 Air's trailer names the model `Insta360 X4 Air` (read by our
  code on the owner's machine: "no table entry for "Insta360 X4 Air";
  using the Insta360 X5's", as the app said then).
- Without a row, both cameras open on the general one
  (`orbiscore::FindInsvModel`), marked untested: only what the measured
  rows agree on (the X5's and X6's IMU axes, Insta360's mount, the X line's
  file names, the mode names), and none of the X5's own corrections -- its
  strings' radial models stand as written, there is no guard table but the
  file's, and timelapses and photos aren't levelled. Without field 27 the
  focal reference is half the calibration's frame.

### Hardware

| | X4 | X4 Air |
|---|---|---|
| Released | April 2024 | October 2025 |
| Sensors | 1/2" | 1/1.8" |
| Lens | f/1.9, 6.7 mm equiv. | f/1.95, 6.4 mm equiv. |
| Lenses | fixed; standard and premium lens guards | **replaceable** |
| ISO | 100–6400 | 100–3200 |
| IMU | six-axis | six-axis |
| Battery | 2290 mAh | 2010 mAh |
| Weight | 203 g | 165 g |

📖 Both are described as built on one "5 nm AI chip" (Insta360 says the
same chip powers the X4; the X5 adds two imaging chips).

## Stabilization in the camera

- 📖 X4 Air: Insta360's FlowState table says in-camera stabilization in
  360 mode is **not supported**; 360 video is stabilized afterwards
  ("all video modes" in post), as on the X5. So the `.insv` should hold the
  unstabilized picture and the raw IMU, which is what our stabilizer needs.
  In-camera stabilization exists only for single-lens Video and Loop
  Recording.
- 📖 Horizon Lock is on by default in 360 mode on both and shares the
  FlowState toggle; it is a post-production setting for a 360 clip.

## Trailer

- 📖 The same `inst` trailer as the X5 (X5 §4): telemetry-parser reads X4,
  X4 Air and X5 trailers with one code path -- an Offsets table, records
  back to back, each closed by its six-byte descriptor.
- ✅ X4 Air: both calibration strings, as on the X5 (X5 §5.3): `offset_v3`
  and the 56-token MEI string (telemetry-parser's `offset_v6`), lens type
  **131** (X5 113, X6 193), calibration frame **15360×7680** (7680² per
  lens), ξ 2.31, fx ~7093 / 7107. Field 27: src 7680², **dst 7424²**
  (fields 5 and 6 are 0, as on the X5), so the 6K picture scales the
  focal length by 3008/7424. The two strings disagree more than on the
  X5: lens 0's cy is 3831.2 in the 56-token string and 3845.4 in
  `offset_v3` (X5: within 4 px). Its 13-term block has the thin-prism
  terms the X5's has, and our reading of that block doesn't match
  Gyroflow's (X5 §5.3). The same strings and the same field 27 in our 8K,
  6K and 4K clips: every mode is described against one 7424² window.
- ❓ X4: which calibration string it carries. The X5 has the 56-token MEI
  string (`offset_v3`, X5 §5.3, field 111/112), which the native stitch
  needs; without it the clip won't open. telemetry-parser also knows
  `offset_v6` (a 13-term lens model, the X6's) and `offset_v8` (a lens that
  focuses, one calibration per AF code); it doesn't say which the X4 Air
  writes. The replaceable lenses make it worth checking.
- ✅ X4 Air lens profiles (X5 §5.4, field 145): our clip carries a table
  in the X5's layout -- `bare` 200°, `ProtectorA`, `ProtectorS`,
  `ProtectorAS` 195° (one polynomial for all three), `InvisibleDiveWater`
  and `InvisibleDiveAir` 190° -- and field 68 = 0 (bare). ❓ Whether a
  swapped lens changes the table or the calibration.
- ❓ X4 lens profiles: the X4 has lens guards; whether its clips carry the
  X5's table is open.
- ✅ X4 Air IMU: ~1007 Hz, read with the X5's layouts; at rest the
  accelerometer reads 1.00 g on raw x, so the scaling is right whichever
  layout field 62 picks. Exposure (one sample per frame) and GPS records
  are there too.
- ❓ X4: whether the IMU is raw counts (`is_raw_gyro`, field 62) with the
  X5's full-scale ranges (X5 §6.1).
- 📖 Track order: telemetry-parser lists the X4, X5, X4 Air, X6 and A1 as
  bodies whose lens-to-track order is stated by
  `pano_record_multi_track_order` (field 80: `TRACK0_STREAM10` reversed,
  `TRACK0_STREAM00` in order), and notes that an X4 writes no `stream_type`
  at all. Our reader takes track 0 as lens 0 on every Insta360 body and
  reads neither field; the X5 and X6 stitch correctly that way. ✅ The
  X4 Air states the X5's layout in all three of our clips: field 79
  (`pano_record_type`) 2, field 80 1 (`TRACK0_STREAM10`), field 131
  (`stream_type`) 4, as the 16 X5 clips we checked write them. So the
  order that holds on the X5 is the one stated here. ❓ The X4.

## IMU axes

- 📖 Gyroflow maps the X4's IMU with `yzX`, the same as the X5 (X5 §6.3). So
  our measured X5 mapping `z,-x,-y` may hold; it must be measured again on
  an X4 clip, the same way.
- ✅ X4 Air: the X5's `z,-x,-y` holds, about every axis
  (`residual_stab --diagnose`). telemetry-parser's orientation table has
  no X4 Air entry (that library's default `Xyz` is an omission, not a
  measurement). Gravity is on raw x at rest (−1.00 g, the X5's sphere Y ←
  −raw x). On the 6K clip (5-20.5 s) the image-against-gyro sweep picks
  the X5's permutation (two axes swapped: correlation 1.61 -> 1.20 of 2),
  and under Direction lock the forward view keeps 0.084 / 0.023 °/frame of
  yaw / pitch pan; with any one axis's sign flipped, 10-13 times as much
  (the vertical in yaw, the other two in pitch). That clip turns only
  about the vertical; the 8K and 4K 50 clips turn the camera over (the
  correction spans ±171° of roll and ±90° of pitch), and there `z,-x,-y`
  leaves 0.17 / 0.14 (8K) and 0.11 / 0.07 (4K 50) °/frame, any one sign
  flipped 3-8 times as much.
- ✅ X4 Air capture timing: the X5's (field 28 + the model's −0.5 ms, each
  row at mid-exposure, X5 §7.1) holds. The app's per-clip sync fine-tune
  (`residual_stab --auto-sync`) finds −0.2 to 0.0 ms on three 7 s windows
  and on the turn, the image-against-gyro sweep −2.0 to −0.1 ms, at a
  0.5 ms exposure; on the 8K 30 and 4K 50 clips −0.0 to +0.3 ms on three
  7 s windows each, at 0.6-0.7 ms. Exposures that short leave the
  mid-exposure rule itself untested on this camera.

## Split recordings (X4 Air)

- ⚠️ The owner's two-file take doesn't play (their log). Our reader joins the parts
  (above), but `LensDemuxer` refuses part 2: *"lens 0 isn't coded like the
  first part's"*. Part 1's decoders go on into part 2, so the reader asks
  for the same codec, size, pixel format, time base and, byte for byte, the
  same codec header (`extradata`, the H.264 / H.265 parameter sets). Size
  and frame rate matched (the reader checks those before joining), so the
  difference is in the pixel format, the time base or -- most likely -- the
  header. Our X5 split recording passes the same check (X5 §1). ❓ Which field differs: `ffprobe -show_streams` on both files will
  say. If it is the header only, part 2 needs a decoder of its own rather
  than a refusal.
- ❓ Nothing in the preview on the owner's machine even with the right GPU;
  the log shows only the part 2 refusal. ✅ A single-file X4 Air clip opens
  and stitches: the owner's `…_039` alone, and our own clips (below).

## The X4 Air files we have

One camera, firmware v1.2.7, bare lenses, H.265 8-bit in both lens streams,
hand-held on a stick next to a building, with its `.lrv` proxy:

| File | Mode | Length | Motion |
|---|---|---|---|
| `VID_20261127_141150_00_040` | 6K 30 (3008²) | 20.7 s | still for 5 s, then turned about the vertical |
| `VID_20261128_143347_00_042` | 8K 30 (3840²) | 22.1 s | turned over: upside down, on its side |
| `VID_20261128_143309_00_041` | 4K 50 (1920²) | 22.3 s | as the 8K clip |

The same scene in all three, so the 8K and 6K clips are the pair the seam
needed.

- ✅ Each opens on the general row, marked untested, and stitches on the
  GPU (Vulkan decode, compute stitch) and the CPU; the 6K clip's export
  works.
- ✅ The `.lrv` proxy is 1664×832 H.264, the two lenses side by side at
  832² each, at 30 fps for the 30 fps clips and **25 fps for the 50 fps
  one** (the X5's 60 and 120 fps clips have 30 fps proxies). Playback from
  the proxy lines up with the full file in all three modes
  (`stitch_source_smoke --proxy`: PSNR highest unshifted); at 50 fps each
  frame shows the proxy's frame at or before its time.
- ⚠️ **The seam is about 1.4° off, in every mode.** Near content at the
  seam is doubled (a window on a nearby wall: its edges and bars twice,
  also *across* the seam, which parallax can't do there). Measured per 5°
  block of the seam over 8 frames of the 6K clip, lens 1's picture is
  shifted against lens 0's all round the seam: along it `1.38°·cos(az)`,
  across it `1.80°·sin(az)` (az: the azimuth around lens 0's axis),
  1.37° RMS after taking out what parallax can do. The app's seam
  measurement reads 1.39° at 8K, 1.32° at 6K and 1.37° at 4K 50. Three
  pictures of the one 7424² window at three scales, with one error: it
  isn't in how a mode's picture maps onto the calibration.
- The camera's own stitched thumbnail (record 2, X5 §4) places lens 1
  1.50-1.57° differently against lens 0 than our stitch does, in all three
  clips, so the camera knows a geometry we don't read.
- What it isn't (each tried against the seam): the radial models
  exchanged as on the X5; `offset_v3` instead of the 56-token string; cx
  and cy transposed or mirrored; field 27; the mode (above); the track
  order (*Trailer*). Reading the 13-term block as Gyroflow does takes
  about a fifth off (1.37 -> 1.09°); at the seam that block's
  decentering and thin-prism terms move the picture by about 7
  calibration-frame pixels, where about 80 are missing. The two lenses'
  intrinsics exchanged removes the shift along the seam but leaves a 1.4°
  tilt. No rotation of lens 1 takes out the shift along the seam (a
  rotation moves the seam's points across it, or all along it alike),
  and the thumbnail's rotation applied to lens 1 leaves 1.0-1.1°.
- What fits: **lens 0's centre** moved by about (−13, +83) calibration-frame
  pixels (~1.1 % of the lens width): fitted on each clip on its own,
  (−13, +82) at 6K, (−17, +84) at 4K 50 and (−12, +84) at 8K take the seam
  to 0.10, 0.13 and 0.12°. Lens 1's centre alone gets no closer than
  1.01°, one shift common to both lenses no closer than 1.05°. But that
  shift overshoots the thumbnail: lens 1 is then 0.65-0.67° off the
  camera's stitch the other way.
- What fits both: the 13-term block **read as Gyroflow reads it** and the
  centres moved by lens 0 (−8, +61), lens 1 (+3, −12), fitted on the 6K
  clip: the seam 0.08° in all three modes, and lens 1 against lens 0
  0.15-0.21° from the camera's thumbnail (radial scales still about
  ∓0.4 %). Gyroflow's reading alone leaves 1.07-1.11° at the seam. On the
  X5 the same reading scores worse against a chart than ours (X5 §5.3), so
  this doesn't settle the reading; with the X4 Air's lens type it agrees
  with two independent measures where ours doesn't. The seam can't tell a
  lens-0 centre error from a tilt of the lenses, so the shifts describe
  the error, not its cause; nothing in the file gives these numbers.
- The app's per-clip seam fit (the old stitcher) can't take it out: it
  fine-tunes lens 1 only, within bounds, and ends at them on all three
  clips (lens 1 yaw −2.5°, centre −1 %), at 0.72-0.88°.
- ⚠️ Against the thumbnail our stitch also makes lens 0's picture about
  0.5 % too large and lens 1's 0.5 % too small, in all three clips: the
  X5's sign and size, where the strings carry each lens's radial model in
  the other's place (X5 §5.3). Not yet applied to the X4 Air.
- ❓ The cause. One clip from a second X4 Air would tell a fault of this
  camera's calibration from something every X4 Air does. The X4 Air's
  lenses are replaceable, so whether a lens of this camera was ever
  changed matters too: a calibration from before a swap would describe the
  old lens.

## Modes and sample clips

All the modes 📖 from Insta360's shooting specs, product pages and launch
coverage (Sources). We have no X4 file and three X4 Air files (*The X4 Air
files we have*). The status words are the X5's (X5 §3.4).

### X4

| Mode | Spec | Files | Our clips | Status |
|---|---|---|---|---|
| 360 Video 8K | 7680×3840, 30 / 25 / 24 | `VID_…_00_` `.insv` + `.lrv` | none | **wanted first**: 3840² lenses, the calibration string, the IMU |
| 360 Video 5.7K+ | 5760×2880, 30 / 25 / 24 | as above | none | wanted |
| 360 Video 5.7K | 5760×2880, 60 / 50 / 30 / 25 / 24 | as above | none | **wanted**: at 60 |
| 360 Video 4K | 3840×1920, 100 / 60 / 50 / 30 / 25 / 24 (third-party spec sheet) | as above | none | **wanted**: at 100 |
| Active HDR | ❓ which resolutions | as above | none | wanted |
| Filters Vivid / Flat | | as above | none | wanted, low: probably baked into the picture |
| H.264 encoding | | as above | none | **wanted**: one clip in each codec |
| Timelapse, TimeShift | ❓ | ❓ | none | wanted |
| Bullet Time | ❓ | ❓ | none | wanted |
| Loop Recording, Pre-recording, Dashcam | | ❓ | none | wanted, low |
| Split recording (over 29 min 59 s) | | two `.insv` + two `.lrv` | none | **wanted**: one take over the split length, both parts (the X4 Air's parts don't join, above) |
| Single-lens Video | 4K 3840×2160, 60 / 50 / 30 / 25 / 24 | `.mp4` | none | wanted, low |
| Single-lens FreeFrame | ❓ | `PRO_VID_….mp4` | none | wanted, low |
| Me Mode | 4K 3840×2160 30 / 25 / 24; 2.7K and 1080p at higher rates | `.insv` | none | wanted |
| 360 Photo | 72 MP 11904×5952, 18 MP 5888×2944 | `.insp`, `.dng` | none | wanted, low |
| Lens guards (standard, premium) | | | none | **one clip with each guard, and the same scene bare** |
| IMU axes | | | none | **a moving clip with deliberate turns about each axis** |

### X4 Air

| Mode | Spec | Files | Our clips | Status |
|---|---|---|---|---|
| 360 Video 8K | 7680×3840, 30 / 25 / 24; Active HDR up to 8K 30 | `VID_…_00_` `.insv` + `.lrv` | one at 30, bare, turned over (above) | **partial**: opens, stitches, plays from the proxy; the seam is ~1.4° off, as at 6K (above) |
| 360 Video 6K | 6016×3008, 50 / 48 / 30 / 25 / 24 | as above | one at 30, bare (above) | **partial**: as 8K; at 50 wanted |
| 360 Video 4K | 3840×1920, 50 / 48 / 30 / 25 / 24 (one retailer says 60) | as above | one at 50, bare, turned over (above); one owner's take at 30, split in two files (log only) | **partial**: as 8K, the proxy at 25 fps; the split take's files wanted |
| Filters Vivid / Flat | | as above | none | wanted, low |
| H.264 encoding | | as above | none | **wanted**: one clip in each codec |
| InstaFrame | 360 6K + flat 1080p, 30 / 25 / 24 | `.insv` (360) + `.mp4` (flat) | none | wanted |
| Timelapse, TimeShift | ❓ | ❓ | none | wanted |
| Bullet Time | ❓ | ❓ | none | wanted |
| Loop Recording, Dashcam | | ❓ | none | wanted, low |
| Split recording | | two `.insv` + two `.lrv` | the owner's take above | **wanted**: both parts, to find what the second part codes differently |
| Single-lens Video | 4K 3840×2160 16:9 60…24, 9:16 30 / 25 / 24; 2.7K 2720×1536 60…24; 1080p 120…24 | `.mp4` | none | wanted, low |
| Single-lens FreeFrame | 3840² 30 / 25 / 24, 2720² 60…24, 1920² 120…24 | `PRO_VID_….mp4` | none | wanted, low |
| Me Mode | 4K 3840×2160 30 / 25 / 24, 2.7K 2720×1536 60…24, 1080p 120 / 100 | `.insv` | none | wanted |
| 360 Photo (HDR, Interval, Starlapse, Burst; PureShot, RAW) | 29 MP 7680×3840 | `.insp`; `.jpg` with in-camera stitching; `.dng` with RAW | none | wanted, low |
| Replaceable lenses | | | none | **a clip before and after a lens swap**: does the calibration change, is there a profile per lens |
| IMU axes | | | all three clips; the 8K and 4K ones turn about every axis | **enough**: the X5's map and timing hold (above) |
| A second camera | | | none | **wanted**: any clip from another X4 Air, to tell the seam's cause (above) |

The most useful first set, 10–30 s each, straight from the card with the
whole folder (`DCIM` and `MISC`), as in "Sample clips wanted"
([`README.md`](README.md#sample-clips-wanted)):

1. X4: 8K 30, Standard, hand-held or on a stick, with slow deliberate
   turns left-right, up-down and a roll. (X4 Air: we have it.)
2. X4 Air: any clip from a second camera, with something within a metre
   or two near the seam (the seam, above).
3. One clip in each other 360 resolution (X4: 5.7K 60, 4K 100; X4 Air: 6K
   50).
4. A take just over 30 minutes, both parts (any resolution; 4K fills the
   card least).

## Sources

- Insta360 X4 manual, shooting specs:
  <https://onlinemanual.insta360.com/x4/en-us/specs/shooting-specs>
- Insta360 X4 Air manual, shooting specs (a table of every X model) and
  hardware:
  <https://onlinemanual.insta360.com/x4air/en-us/specs/shooting-specs>,
  <https://onlinemanual.insta360.com/x4air/en-us/specs/hardware>
- Insta360 X4 manual, hardware:
  <https://onlinemanual.insta360.com/x4/en-us/specs/hardware>
- Insta360 X4 Air manual, file format (one-file cameras, naming, photo
  formats):
  <https://onlinemanual.insta360.com/x4air/en-us/operating-tutorials/storage/file-format>
- Insta360 X4 Air manual, FlowState (in-camera and post stabilization per
  model):
  <https://onlinemanual.insta360.com/x4air/en-us/operating-tutorials/capture-preview/shooting-parameters/flowstate>
- Insta360 X-series colour (bit depth, which models have no log mode):
  <https://onlinemanual.insta360.com/x6/en-us/specs/color-chroma>
- Insta360 X6 manual, file formats (one-file cameras):
  <https://onlinemanual.insta360.com/x6/en-us/operating-tutorials/storage/fileformat>
- Insta360, X5 vs X4 (the 5 nm chip):
  <https://www.insta360.com/blog/insta360-x5-vs-x4.html>
- Newsshooter, X4 Air launch (every video mode with frame rates):
  <https://www.newsshooter.com/2025/10/28/insta360-x4-air/>
- DPReview, X4 Air announcement (Active HDR at 8K, replaceable lenses):
  <https://www.dpreview.com/news/7450240546/insta360-x4-air-announcement/>
- Australian Photography, X4 Air review (sensor, lens, single processor):
  <https://www.australianphotography.com/gear/review-insta360-x4-air>
- Gyroflow, `src/core/stabilization/distortion_models/insta360_v6.rs`
  (the 13-term lens model of the 56-token string):
  <https://github.com/gyroflow/gyroflow>
- Gyroflow telemetry-parser, `src/insta360/mod.rs` (trailer layouts, IMU
  orientation table) and `src/insta360/rig.rs` (track order, file groups):
  <https://github.com/AdrianEddy/telemetry-parser>
