# Insta360 X4 and X4 Air file format

**Desk research (X4 2026-10-15, X4 Air 2026-11-24), no X4 or X4 Air file
examined.** From public specs, Insta360's manuals, launch coverage and
Gyroflow's telemetry-parser, set against what our code assumes, plus the
log of one X4 Air owner who opened a clip with orbis-reframe 0.10.1 (X4 Air
below). Written as differences from the X5
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
- ❓ Bit depth of the X4. Insta360 doesn't say; one third-party listing
  claims 10-bit. The app's GPU stitch takes 8-bit and 10-bit (see
  [`insta360-x6.md`](insta360-x6.md)).
- 📖 Audio 48 kHz 16-bit AAC on both. Modes: X4 Auto Wind Noise Reduction,
  Active Wind Reduction, Stereo, 360 Direction Focus; X4 Air Auto Wind
  Noise Reduction, Voice Focus, Stereo, 360 Audio.
- ⚠️ Per lens: 3840² at 8K, 2880² at 5.7K, 1920² at 4K (the X5's sizes),
  and on the X4 Air **3008² at 6K** (6016×3008, the X6's 6K). The app names a
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
- ❓ Which calibration string it carries. The X5 has the 56-token MEI
  string (`offset_v3`, X5 §5.3, field 111/112), which the native stitch
  needs; without it the clip won't open. telemetry-parser also knows
  `offset_v6` (a 13-term lens model, the X6's) and `offset_v8` (a lens that
  focuses, one calibration per AF code); it doesn't say which the X4 Air
  writes. The replaceable lenses make it worth checking.
- ❓ Lens profiles (X5 §5.4, field 145): the X4 has lens guards; the X4 Air
  has replaceable lenses instead, and whether its clips carry a profile per
  lens, the X5's table or none is open.
- ❓ Whether the IMU is raw counts (`is_raw_gyro`, field 62) with the X5's
  full-scale ranges (X5 §6.1).
- 📖 Track order: telemetry-parser lists the X4, X5, X4 Air, X6 and A1 as
  bodies whose lens-to-track order is stated by
  `pano_record_multi_track_order` (field 80: `TRACK0_STREAM10` reversed,
  `TRACK0_STREAM00` in order), and notes that an X4 writes no `stream_type`
  at all. Our reader takes track 0 as lens 0 on every Insta360 body and
  reads neither field; the X5 and X6 stitch correctly that way. ❓ Whether
  the X4 and X4 Air state the same order as the X5.

## IMU axes

- 📖 Gyroflow maps the X4's IMU with `yzX`, the same as the X5 (X5 §6.3). So
  our measured X5 mapping `z,-x,-y` may hold; it must be measured again on
  an X4 clip, the same way.
- ❓ X4 Air: telemetry-parser's orientation table has no X4 Air entry, so it
  gets that library's default (`Xyz`) -- an omission, not a measurement.
  The app uses the X5's `z,-x,-y` for it; it must be measured.

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
  the log shows only the part 2 refusal. Whether a single-file X4 Air clip
  (`…_039` alone) opens and stitches is the first thing to check.

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
| 360 Video 6K | 6016×3008, 50 / 48 / 30 / 25 / 24 | as above | none | **wanted**: 3008² lenses (the general row names them 6K) |
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
| IMU axes | | | none | **a moving clip with deliberate turns about each axis** |

The most useful first set, 10–30 s each, straight from the card with the
whole folder (`DCIM` and `MISC`), as in "Sample clips wanted"
([`README.md`](README.md#sample-clips-wanted)):

1. 8K 30, Standard, hand-held or on a stick, with slow deliberate turns
   left-right, up-down and a roll.
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
- Gyroflow telemetry-parser, `src/insta360/mod.rs` (trailer layouts, IMU
  orientation table) and `src/insta360/rig.rs` (track order, file groups):
  <https://github.com/AdrianEddy/telemetry-parser>
