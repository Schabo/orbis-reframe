# Insta360 X4 and X4 Air file format

**Desk research (X4 2026-10-15, X4 Air 2026-11-24), and one X4 Air file
(2026-11-27): no X4 file examined.** From public specs, Insta360's manuals,
launch coverage and Gyroflow's telemetry-parser, set against what our code
assumes, plus the log of one X4 Air owner who opened a clip with
orbis-reframe 0.10.1, and one X4 Air 6K clip of our own (*The X4 Air file
we have*, below). Written as differences from the X5
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
- ✅ X4 Air: our 6K clip is H.265 Main, **8-bit**, full range, BT.709
  (ffprobe), as Insta360 says.
- ❓ Bit depth of the X4. Insta360 doesn't say; one third-party listing
  claims 10-bit. The app's GPU stitch takes 8-bit and 10-bit (see
  [`insta360-x6.md`](insta360-x6.md)).
- 📖 Audio 48 kHz 16-bit AAC on both. Modes: X4 Auto Wind Noise Reduction,
  Active Wind Reduction, Stereo, 360 Direction Focus; X4 Air Auto Wind
  Noise Reduction, Voice Focus, Stereo, 360 Audio.
- ⚠️ Per lens: 3840² at 8K, 2880² at 5.7K, 1920² at 4K (the X5's sizes),
  and on the X4 Air **3008² at 6K** (6016×3008, the X6's 6K; ✅ two
  3008² streams in our clip). The app names a
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
  Gyroflow's (X5 §5.3).
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
  reads neither field; the X5 and X6 stitch correctly that way. ❓ Whether
  the X4 and X4 Air state the same order as the X5. Our X4 Air clip
  stitches nearly right with track 0 as lens 0, but that doesn't settle
  it: the two lenses' calibrations are so alike that the other order
  would too, and exchanging the lenses' intrinsics changes the seam error
  (*The X4 Air file we have*, below).

## IMU axes

- 📖 Gyroflow maps the X4's IMU with `yzX`, the same as the X5 (X5 §6.3). So
  our measured X5 mapping `z,-x,-y` may hold; it must be measured again on
  an X4 clip, the same way.
- ✅ X4 Air: the X5's `z,-x,-y` holds on our clip (`residual_stab
  --diagnose`, 5-20.5 s). telemetry-parser's orientation table has no X4
  Air entry (that library's default `Xyz` is an omission, not a
  measurement). Gravity is on raw x at rest (−1.00 g, the X5's sphere Y ←
  −raw x). The image-against-gyro sweep picks the X5's permutation (two
  axes swapped: correlation 1.61 -> 1.20 of 2). Under Direction lock the
  forward view keeps 0.084 / 0.023 °/frame of yaw / pitch pan; with any
  one axis's sign flipped, 10-13 times as much (the vertical in yaw, the
  other two in pitch). Roll shows only through the stick's 2-7° tilt, so
  a clip with deliberate turns about each axis would settle it fully.
- ✅ X4 Air capture timing: the X5's (field 28 + the model's −0.5 ms, each
  row at mid-exposure, X5 §7.1) holds. The app's per-clip sync fine-tune
  (`residual_stab --auto-sync`) finds −0.2 to 0.0 ms on three 7 s windows
  and on the turn, the image-against-gyro sweep −2.0 to −0.1 ms, at a
  0.5 ms exposure. One exposure only, so the mid-exposure rule itself is
  untested on this camera.

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
  and stitches: the owner's `…_039` alone, and our own 6K clip (below).

## The X4 Air file we have

`VID_20261127_141150_00_040.insv` + `LRV_…_01_040.lrv` (2026-11-27):
6K 30, 20.7 s, firmware v1.2.7, bare lenses, H.265 8-bit in both lens
streams; the `.lrv` is a 1664×832 H.264 proxy. Hand-held on a stick next to
a building, still for 5 s, then turned about the vertical.

- ✅ It opens on the general row, marked untested, and stitches on the GPU
  (Vulkan decode, compute stitch) and the CPU; the export works.
- ⚠️ **The seam is about 1.4° off.** Near content at the seam is doubled
  (a window on a nearby wall: its edges and bars twice, also *across* the
  seam, which parallax can't do there). Measured per 5° block of the seam over
  8 frames, lens 1's picture is shifted against lens 0's all round the
  seam: along it `1.38°·cos(az)`, across it `1.80°·sin(az)` (az: the
  azimuth around lens 0's axis), 1.37° RMS after taking out what
  parallax can do. The camera's own stitched thumbnail (record 2, X5 §4)
  places lens 1 **1.51°** differently against lens 0 than our stitch, so
  the camera knows a geometry we don't read.
- What it isn't (each tried against the seam): the radial models
  exchanged as on the X5; `offset_v3` instead of the 56-token string; cx
  and cy transposed or mirrored; field 27. Reading the 13-term block as
  Gyroflow does (X5 §5.3) takes about a fifth off (1.37 -> 1.09°). The two
  lenses' intrinsics exchanged removes the shift along the seam but leaves
  a 1.4° tilt.
- What fits: the lens centres moved, lens 0 by (−8, +61) and lens 1 by
  (+3, −12) calibration-frame pixels (~24 and ~5 px in the 3008² picture):
  the seam then agrees to 0.07° and our stitch matches the thumbnail to
  0.14°. The seam can't tell a lens-0 centre error from a tilt of the
  lenses, so this is a description, not the cause; nothing in the file
  gives these numbers.
- The app's per-clip seam fit (the old stitcher) measures the offset as
  1.29° and takes it to 0.72°, ending at its bounds (lens 1 yaw −2.5°,
  centre −1 %): a fine-tune of lens 1 can't take out this much.
- ⚠️ Against the thumbnail our stitch also makes lens 0's picture 0.59 %
  too large and lens 1's 0.44 % too small: the X5's sign and size, where
  the strings carry each lens's radial model in the other's place (X5
  §5.3). Not yet applied to the X4 Air.
- ❓ The cause. The next file to look at is an **8K clip from the same
  camera** (3840², at the calibration's own scale): a clean seam there
  would put it in how the 6K window maps onto the calibration.

## Modes and sample clips

All the modes 📖 from Insta360's shooting specs, product pages and launch
coverage (Sources). We have no file from either camera. The status words
are the X5's (X5 §3.4).

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
| 360 Video 8K | 7680×3840, 30 / 25 / 24; Active HDR up to 8K 30 | `VID_…_00_` `.insv` + `.lrv` | none | **wanted first**: 3840² lenses, the calibration string, the IMU |
| 360 Video 6K | 6016×3008, 50 / 48 / 30 / 25 / 24 | as above | one at 30, bare (above) | **partial**: opens and stitches; the seam is ~1.4° off (above); at 50 wanted |
| 360 Video 4K | 3840×1920, 50 / 48 / 30 / 25 / 24 (one retailer says 60) | as above | one owner's take at 30, split in two files (log only) | **wanted**: a short one-file clip, and the split take's files |
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
| IMU axes | | | the 6K clip (turns about the vertical, a stick's shake) | **partial**: the X5's map and timing hold (above); a clip with deliberate turns about each axis, for roll |

The most useful first set, 10–30 s each, straight from the card with the
whole folder (`DCIM` and `MISC`), as in "Sample clips wanted"
([`README.md`](README.md#sample-clips-wanted)):

1. 8K 30, Standard, hand-held or on a stick, with slow deliberate turns
   left-right, up-down and a roll. On the X4 Air also the reference for
   the 6K seam (above): ideally the same scene, at 8K and at 6K.
2. One clip in each other 360 resolution (X4: 5.7K 60, 4K 100; X4 Air: 6K
   50, 4K 50).
3. A take just over 30 minutes, both parts (any resolution; 4K fills the
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
