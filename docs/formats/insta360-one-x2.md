# Insta360 ONE X2 file format

**Examined on two clips (2026-10-20).**
Written as differences from the X5 ([`insta360-x5.md`](insta360-x5.md),
"X5 §n" below). Markers as in [`README.md`](README.md). The ONE X2 facts
come from the clips below, read with `ffprobe`, `insv_trailer_dump` and a
throw-away trailer / protobuf walker; field names are telemetry-parser's
(X5 §13). The app reads the 5.7K two-file take since 2026-10-21 (the
calibration from `offset_v3`, the lens-1 file found beside the `_00_` one,
telemetry in ms); what's still open is in §11 and §12.

The older, **two-file** generation: one file per lens at 5.7K. The X3
works the same way and has its own document,
[`insta360-x3.md`](insta360-x3.md).

Reference clips (`sample_footage/insta-one-x2/`, gitignored), one camera,
firmware `v1.0.92_build1`, recorded 2025-08-16 with the camera on a stick:

| take | files | length | notes |
|---|---|---|---|
| `103807_…_001` | `VID_…_00_001.insv`, `VID_…_10_001.insv`, `LRV_…_11_001.insv` | 309.4 s, 7736 frames | |
| `105108_…_003` | `VID_…_00_003.insv`, `VID_…_10_003.insv`, `LRV_…_11_003.insv` | 188.2 s, 4704 frames | |

Twelve more takes from the same day and camera (added 2026-10-21, the same
three files each): `_002`, `_005`, `_006`, `_008`–`_012` walking around a
bike meet on the stick (16–215 s), `_004`, `_007`, `_013`, `_014` riding
(1323, 731, 419, 230 s). All are the same mode: 5.7K (2880² per lens) at
25 fps, H.264, the same `offset_v3` and firmware, and every `_10_` file is
one frame shorter than its `_00_` (§3.1).

And one take from 2025-08-04 on older firmware (`v1.0.62_build2`, same
calibration): `VID_20250804_152938_00_003` + `_10_` + LRV, **5.7K at 29.97
fps** (30000/1001), 1740.9 s (29 min), 52176 frames, 10.96 GB and 10.91 GB
-- the longest and only 30 fps clip here. Here the **two files have the
same frame count**.

Beside them are five flat videos exported from Insta360's software from
the same day (`1.MP4`, `2.MP4`, `3.MP4`, `CKDH4299.MP4`, `EXAM7155.MP4`):
3840×2160 30 fps HEVC Main 10, **HLG** (`arib-std-b67`, BT.2020) with a
Dolby Vision configuration record, tagged make `Insta360`, model
`ONER.FixFrame`, muxed by `Lavf60.3.100`. Edited (reframed, a tiny planet,
a title), so they show Insta360's look and HDR export, not a reference
frame for the stitch.

## 1. Files

- ✅ **One file per lens**: `VID_…_00_NNN.insv` and `VID_…_10_NNN.insv`, the
  first digit being the lens (X5 §1). 📖 The X6 manual: "X6, X5, X4 Air and
  X4 generate only one VID file containing data from both lenses, unlike the
  X3, which produced two".
- 📖 **Only 5.7K writes two files.** Insta360's file-format table: on the
  X3, ONE X2 and ONE X, 360 video at 5.7K gives two `VID` files and one
  `LRV`; below 5.7K (4K, 3K) **one** `VID` file and one `LRV`. ❓ How that
  one file holds the two lenses (two streams like the X5, or one). No LRV
  at all in Bullet Time, TimeShift, Timelapse, or video at 4K 60 / 50,
  3K 100 or above 100 fps.
- ✅ **The proxy is `LRV_…_11_NNN.insv`**: extension `.insv`, not `.lrv`,
  and lens digit `1` (the X5 writes `LRV_…_01_NNN.lrv`). It holds both
  lenses (§3.2).
- ✅ **Only the `_00_` file and the LRV carry the `inst` trailer.** The
  `_10_` file is a plain MP4 that ends in padding: no calibration, IMU or
  model name of its own. So the `_10_` file can't be recognised as an
  Insta360 lens file from its content; only its name and its `_00_` sibling
  say what it is.
- ✅ The `_00_` file's metadata (field 26 `file_group_info`) doesn't name
  the `_10_` file: `{type 6, index 0, identify ""}` (index 1 on the LRV).
  📖 telemetry-parser describes a *sibling hint* naming the other file of a
  pair; these files have none. It also warns that a long recording split into
  two files is a group too, so a pair can't be matched by name alone. ❓ How
  a split ONE X2 recording is named (no clip long enough).

## 2. MP4 container ✅

```
ftyp  (32 B)    major_brand "avc1", compatible "avc1isom"
mdat            the A/V samples of this one lens
moov            two tracks; udta holds only an Ambarella "AMBA" box (107 B)
free            padding up to the next 1 MiB boundary of the file
trailer         _00_ and LRV only: the inst records, raw, to EOF
```

- **No `inst` box header** and no `wide` box: the trailer starts right at the
  1 MiB boundary after `free` with its first record's payload. It's found
  from the footer at EOF (X5 §4.1), as on the X5. `ExtractRawTrailer()`
  ("everything after `moov`") hands the walker the padding as well, which
  is harmless because the walk starts at EOF.
- The `_10_` file ends in its `free` box; its size is an exact multiple of
  1 MiB (1 761 607 680 = 1680 MiB).
- Metadata field 9 (`file_size`) is the trailer's start offset (the size
  before the trailer, padding included), as on the X5.
- No thumbnail in `moov` (the X5 has a ThumbnailTIFF there). The `AMBA`
  box's content is unknown ❓; it holds the frame count (7736 / 7735).
- Track timescale 25000; handlers `Ambarella AVC` / `Ambarella AAC`, encoder
  tag `Ambarella AVC encoder` (X5: `INS.*`).

## 3. Streams

### 3.1 `VID_…_00_` / `_10_` (one lens each) ✅

| index | what | details |
|---|---|---|
| 0 | the lens's fisheye video | H.264 **Main**, `avc1`, level 5.2, **2880×2880**, **25 fps**, ~45 Mb/s, **no B-frames** (`has_b_frames=2` in the header, but only I and P frames), **GOP 16** (0.64 s) |
| 1 | audio | AAC-LC, 48 kHz stereo, 128 kb/s |

- **Pixel format**: `yuvj420p`, 8-bit 4:2:0 full range like the X5, but
  **tagged BT.601** (`smpte170m` primaries, transfer and matrix), where the
  X5 says BT.709. Read the matrix from the stream (it's tagged), don't assume
  the X5's.
- The fisheye circle doesn't fill the frame: the calibration radius is
  ~1481 px in the 3040² sensor frame, so ~1403 stream px in the 2880² frame,
  with a black margin around it (X5: the circle touches the frame edge).
- The `_10_` file is **one frame shorter** than `_00_` on every 25 fps take
  (7735 vs 7736, 4703 vs 4704, ...), the same length on the 30 fps one; its
  `creation_time` can be a second later. ✅ **Frame n of one
  file is frame n of the other**; `_10_` only lacks the last frame
  (measured 2026-10-21): the seam match (`EstimateSeamAlignment`'s strip
  score, no lens-1 adjustment) with `_10_` shifted by −2..+2 frames, at 10
  points along each take and 12 more over `_001`'s last minute, is best at
  shift 0 almost everywhere -- mean block NCC 0.63 at 0 against 0.56 / 0.56
  at ±1 on `_001`, 0.59 against 0.56 / 0.52 on `_003`; the few points that
  pick another shift have flat scores (little texture or motion at the
  seam) and disagree with each other. On the 30 fps take, 10 points over
  its 29 min: shift 0 at every one (0.38 against 0.29 / 0.30). Both files
  start at pts 0.
- **Every file has the same audio**: the AAC streams of `_00_`, `_10_` and the
  LRV decode to identical samples (first 20 s compared, md5). One of them is
  enough.
- 📖 Specs (Insta360): 360 video up to 5.7K 30 (5760×2880), also 4K 50 and
  3K 100; H.264 or H.265, up to 100 Mb/s. Only 5.7K 25 H.264 is in our
  clips.

### 3.2 `LRV_…_11_` (proxy) ✅

| index | what | details |
|---|---|---|
| 0 | video | H.264 Main, **736×368**, 25 fps, ~14 Mb/s, GOP 15, no B-frames |
| 1 | audio | AAC-LC 48 kHz stereo, the same samples as the masters |

- **Both lenses side by side**, 368² each, not stitched. The **left half is
  the `_00_` lens** (on the X5 lens 0 is the right half).
- One frame shorter than `_00_`, like `_10_`.
- **It carries the full trailer**: IMU, exposure and type 9 are
  byte-identical to the `_00_` file's; the metadata differs only in field 9
  and `file_group_info.index` (1); its thumbnails (§5) are a 736×368 frame.

### 3.3 Every mode, and our clips of it

All the ONE X2's modes 📖 (Insta360's shooting specs, Sources), with our
clips and whether they're enough to build and check the reader on. The
status words are the X5's (X5 §3.4): **enough**, **one clip**, **wanted**.

| Mode | Spec | Files | Our clips | Status |
|---|---|---|---|---|
| 360 Video 5.7K | 30 / 25 / 24 | `_00_` + `_10_` + `LRV_…_11_` | 14 takes at 25 (walking and riding), one 29 min take at 29.97, H.264, `gamma_mode` `linear` | enough at 25 and 30 H.264; **24 wanted** |
| 360 Video 4K | 50 / 30 | one `VID` file (+ LRV at 30 only) | none | **wanted**: the one-file layout |
| 360 Video 3K | 100 | one `VID` file, no LRV | none | **wanted** |
| HDR Video | 5.7K 25 / 24 | ❓ | none | **wanted** |
| H.265 encoding | | | none | **wanted** |
| Filters Vivid / LOG | | | none | **LOG wanted**: Insta360 publishes a LUT for it (`ONE-X2-LUT-Final-V1.0.0.cube`, 65³, the same file as the X3's); Vivid low |
| Timelapse | 5.7K 30 | no LRV | none | **wanted** |
| TimeShift | 5.7K 30 / 25 / 24 | no LRV | none | **wanted** |
| Bullet Time | 3K 100 | no LRV | none | **wanted** |
| Steady Cam (Ultra Wide Stabilization, 150°) | 1440p 50 / 30, 1080p 120 / 50 / 30; HDR, Timelapse, TimeShift at 1440p | `.mp4` | none | wanted: a flat clip, may open as one already |
| Split recording | | ❓ (§12 question 3) | none split: `_004` (1323 s, 7.5 GB per lens file) and `152938_…_003` (1741 s, 11 GB per lens file) are one file per lens each | **wanted** if longer ones split |
| 360 Photo (Burst, Interval, Night Shot, HDR) | 18 MP | `.insp`, `.dng` (RAW) | none | wanted, low |
| InstaPano (270°), 150° wide-angle photo | 4320×1440; 1440p / 1080p | ❓ | none | wanted, low |
| Audio: Wind Reduction, Stereo, 360 Direction Focus | | | ❓ which ours used | **one clip of each wanted** |
| Lens guards | | | ❓ whether ours had them | **one clip with, one without wanted** |
| IMU axes | | | the two takes (stick swing) | **a clip with deliberate turns about each axis wanted** |

## 4. The `inst` trailer — framing ✅

- **Footer** as on the X5 (X5 §4.1): the same magic, **version 3**, the
  trailer length.
- **No type-0 Offsets table.** The sequential walk from the footer (X5 §4.2)
  reaches every record; the first record in the file (at the trailer start)
  is type 10.
- Records, `_00_001` (309 s, 7736 frames), walked from EOF:

| hdr id | type | name | size | contents |
|---|---|---|---|---|
| `0x0101` | 1 | Metadata (protobuf) | 1 843 | §6 |
| `0x0200` | 2 | Thumbnail | 2 343 322 | **one H.264 keyframe, 2880×2880**: the `_00_` lens's first frame (Annex B, an 18-byte NAL of type 26 before the SPS). LRV: 736×368, both lenses |
| `0x0300` | 3 | Gyro (IMU) | 8 725 080 | 56-byte samples, 500 Hz (§8) |
| `0x0400` | 4 | Exposure | 124 144 | 16-byte samples, one per frame (§9) |
| `0x0500` | 5 | ThumbnailExt | 2 000 058 | the same, for the **`_10_` lens**: its first frame, 2880×2880 H.264. LRV: identical to its type 2 |
| `0x0900` | 9 | AAAData? | 94 608 | 48-byte records, ~6.4 Hz (§10) |
| `0x0a00` | 10 | Anchors | 35 | as on the X5: 7 types, all count 0 |

- Not present: 0 (Offsets), 7 (GPS; no GPS source on these clips), and the
  X5's 11, 22, 28, 29.
- The `_00_` file's trailer is the only place that has the `_10_` lens's
  thumbnail.

## 5. Thumbnails

Types 2 and 5 are each a single H.264 IDR frame that FFmpeg decodes as a raw
`h264` stream ✅: the `_00_` and `_10_` lens's first frame at full size (X5:
one stitched 1280×640 NV12 frame). The app's preview for a ONE X2 clip
(since 2026-10-21) decodes both, scaled to 720², and stitches them on the
CPU with the clip's calibration into a 1024×512 frame of the X5's form:
~0.27 s per clip on the sample takes, once (the media card caches it).

## 6. Type 1 — Metadata (protobuf) ✅

Values from `_00_001`; `_00_003` differs only where noted.

| # | name (telemetry-parser) | ONE X2 value | notes |
|---|---|---|---|
| 1 | `serial_number` | `IXSE…` (14 characters) | |
| 2 | `camera_type` | `Insta360 ONE X2` | |
| 3 | `fw_version` | `v1.0.92_build1` | |
| 5 / 17 | `offset` / `original_offset` | 16-token short calibration | §7 |
| 9 | `file_size` | trailer start offset | |
| 10 | `total_time` | 309 (188) | seconds |
| 14 | `gyro` | 56 bytes | **the IMU sample at the first frame**: its timestamp is field 24, then the 6 doubles of that type-3 sample |
| 18 | `trigger_source` | 1 = CameraButton | |
| 19 / 44 | `dimension` / `resolution_size` | {2880, 2880} | per-lens stream size |
| 20 | `frame_rate` | 25 | |
| 22 | `gamma_mode` | `linear` | X5: `standard` / `I_Log`. ❓ which colour profile writes `linear` |
| 24 | `first_frame_timestamp` | 4407 (4378) | **milliseconds** on the device clock (X5: µs) (§9) |
| 25 | `rolling_shutter_time` | **36.94** | readout, ms: nearly the whole 40 ms frame (X5: 11.8–21.2) |
| 26 | `file_group_info` | {type 6, index 0, identify ""} | type 6 = `VideoHdr` in telemetry-parser's enum ⚠️; index 1 on the LRV |
| 27 | `window_crop_info` | {src 3040×3040, dst 2880×2880} | sensor 3040² per lens; dst = the stream size, so the focal scale `dimension/dst` is 1 |
| 28 | `gyro_timestamp` | **−12.0** | ms; negative (X5: +1.6) |
| 29 | `is_has_gyro_timestamp` | true | |
| 31 | `gyro_calib` | 6 × f64 + u64 | (−0.0034, −0.0090, +0.0090, 0.034, 0.038, −0.0053), then Unix time 1700149542 (2023-11-16; the X5 has 0 there) |
| 36 | `first_gps_timestamp` | = field 24 | not a Unix time without GPS |
| 41 | `is_selfie` | 0 | |
| 43 | `is_dewarp` | 1 | |
| 45 | `battery_type` | 1 | |
| 46 | `cam_posture` | 0 | |
| 47 / 48 / 49 | `fov_type` / `distance` / `fov` | 3 = Ultrawide / 1.0 / 0 | default reframe view |
| 50 | `gyro_filter_type` | 2 | |
| 51 | `gyro_type` | 1 = `InsdevImuType40609` | the same IMU part as the X5 ⚠️ (enum value only) |
| 52 | `media_data_rotate_angel` | 1 | |
| 53 / 55 | `offset_v2` / `original_offset_v2` | 34 tokens | §7 |
| 54 / 56 | `offset_v3` / `original_offset_v3` | **40-token MEI** | §7 |
| 57 | `focus_sensor` | 3 = All | |
| 58 | `expect_output_type` | 0 | |
| 60 | `photo_rot` | 9 floats | an exact rotation matrix (orthonormal, det 1), different per clip (167° and 170° from identity). ❓ Probably the camera's orientation at the start |
| 61 | `audio_mode` | 2 = Stereo | |

- **Absent** compared with the X5: 7 (`creation_time`), 11 (GPS), 42, **62
  (`is_raw_gyro`, so false)**, **64 (`pts_type`)**, **65 (`gyro_cfg_info`)**,
  93, **98 (first / last frame times, frame count)**, **111 / 112 (the
  56-token MEI)**, 120 / 121, **145 (lens-accessory profiles)**.
- Without field 145 there are no guard profiles to pick from. ❓ How Studio
  handles a guarded ONE X2.

## 7. Calibration ✅

The ONE X2 carries the **40-token `offset_v3`** (X5 §5.3) but **not the
56-token string** (fields 111 / 112) the app's stitch requires. (The desk
research guessed it had no `offset_v3`; it has.) All strings are stated in a
**6080 × 3040** side-by-side frame (2 × 3040²), lens type **41**.

`offset_v3` (field 54), per lens `xi fx fy cx cy yaw pitch roll tx ty tz k1 k2
k3 p1 p2 w h lensType` (X5 §5.3), trailing flag 199680 (0x30C00):

| | xi | fx fy | cx cy | yaw pitch roll | tx ty tz | k1 k2 k3 | p1 p2 |
|---|---|---|---|---|---|---|---|
| lens 0 | 1.72859 | 2322.61 2322.41 | 1528.43 1521.45 | 0.799 −0.157 **178.977** | 0 0 0 | 0.2279 −0.1489 −0.9684 | −0.00185 0.00116 |
| lens 1 | 1.72859 | 2318.48 2318.72 | 4554.65 1542.81 | −0.848 −0.368 **1.421** | 0.00017 −0.00195 **−0.0211** | 0.2436 −0.2279 −0.8443 | 0.00129 −0.00042 |

- **Lens 1 sits ~21 mm behind lens 0** (X5: ~32 mm).
- **The roll is ~180° on lens 0 and ~0° on lens 1** (X5: ~90° on both). In
  the X5 this angle is also the lens ↔ IMU rotation (X5 §6.3), so the ONE
  X2's IMU sits differently. The lens pictures themselves lie on their side
  (the horizon runs top to bottom in both files).
- ⚠️ With the X5's MEI reading (radial terms in r², r⁴, r⁶), the short
  string's radius (1481 px) falls at θ ≈ 100°: about a 200° lens, like the
  X5.
- **What's missing for the app's MEI model**: no `k4` and no thin-prism
  `s1..s4` (the 56-token string has both); everything else maps one to one.
  Scaled to the stream, `cx/cy · 2880/3040` (lens 1: `cx − 3040` first) and
  `fx · 2880/2880`.
- `offset` (field 5), 16 tokens, per lens `radius cx cy yaw pitch roll`
  (X5 §5.2):
  `2 1480.960 1532.890 1516.290 0.712 -0.131 179.140 1480.280 4551.640
  1546.940 -0.762 -0.108 1.275 6080 3040 3113`.
- `offset_v2` (field 53), 34 tokens, per lens `radius cx cy yaw pitch roll tx
  ty tz` + 4 polynomial coefficients (`1.0 −0.152 0.271 −0.112`) + `6080 3040
  41`, flag 134144 (0x20C00).
- The `original_` copies (17, 55, 56) equal the working ones.
- ✅ **The stitch takes it as the X5's string** (checked 2026-10-21 on
  `_001`, `insv_stitch --lens1`): `cx / cy` scaled by 2880 / 3040 (half the
  6080 frame), `fx / fy` by 2880 / field 27's dst 2880, and the same roll
  handling as the X5 (the stored roll less 90°). The stitched frame is
  continuous, and the seam fine-tune (`--measure-seam 0 300000`, five frames)
  starts at a seam match of 0.625 and moves lens 1 by under a degree (yaw
  −0.10°, pitch −0.80°, roll −0.15°; match 0.752). (An X5 clip, for scale:
  0.262 → 0.284, under half a degree.) Not yet compared with Insta360
  Studio's stitch of the same frame.
- With no lens-profile table (field 145) the stitch takes the bare lens at
  its default 200° field of view.
- 📖 telemetry-parser's rig code mentions a ONE X2 lens type 42 (a lens guard
  whose optical table differs from its mask table). Our clips say 41,
  presumably the bare lens ⚠️.

## 8. Type 3 — IMU ✅

- **56-byte samples**: `u64` timestamp, then 6 × `f64` — accel x y z in **g**
  (mean magnitude 1.01 g over the clip), then gyro x y z, ⚠️ rad/s (the
  doubles layout, X5 §6.1; field 62 is absent). `ReadInsvTelemetry`'s
  `len % 56` guess picks the layout correctly.
- **Timestamps in milliseconds** (X5: µs), **exactly 2 ms apart: 500 Hz**
  (X5 ~1006 Hz, irregular). The app reads them at the model's tick
  (`InsvModel::telemetryTickUs`, 1000) and hands them on in µs, like the
  X5's; `insv_trailer_dump`'s summary does the same (its `--imu` /
  `--exposure` columns stay raw).
- Pre-roll: the IMU starts **1.60–1.63 s before the first frame** on the
  2025-08-16 takes, 1.32 s on the 30 fps take (older firmware). The X5's
  sync formula with the ms units gives a video ↔ IMU offset of +1619.7 ms
  on `_001` (`residual_stab`), within that range.
- 📖 Axes: telemetry-parser has `xZy` for the ONE X2, but only in its branch
  for files *without* `offset_v3`. These files have it, so it would use its
  default `Xyz` after rotating by lens 0's `yaw pitch roll`.
- ✅ **Ours: `"z,x,y"`** (the ONE X2 row of `insv::InsvModel::imuAxes`;
  measured 2026-10-21): sphere X ← raw z, Y ← raw x, Z ← raw y -- the X5's
  `z,-x,-y` turned 180° about sphere X. Method as X5 §6.3: four horizon
  views (yaw 0 / 90 / 180 / 270°, 90° FOV) rendered unstabilized by
  `reframe_export`, the camera rotation per frame interval from tracked
  features, put into the sphere frame and least-squares fitted against the
  gyro integrated over the same interval. Here the rotation is a
  rotation-only (Kabsch) fit of the tracked rays, not an essential matrix:
  the takes have slow and stationary stretches where the essential matrix
  degenerates. The chain was checked on the X5's `193105` first (essential
  matrix; it finds `z,-x,-y`, 5.6° from the free fit).
  - On 30 s of each take (`_001` 100–130 s, `_003` 40–70 s): `z,x,y` is
    0.4° (`_003`) and 3.6° (`_001`) from the free fit, the runner-up 90°
    away; every view on its own picks it (1–7°). The rotation left per
    frame: 0.11 / 0.14° against 0.74 / 0.55° raw.
  - The gyro is in **rad/s**: the free fit's scale is 0.95–1.00.
  - The accel shares the gyro's axes (as on the X5): carrying the
    low-passed accel along the integrated gyro, the identity ranks first on
    both takes (weak evidence: the rotations are mostly about gravity). And
    in the sphere frame it agrees with the picture: at 105–125 s of `_001`
    it puts the camera's forward 28–34° below level, the unstabilized
    forward view's vanishing point ~30°.
  - Riding (added takes): `_014` 60–90 s gives `z,x,y` 1.4° from the free
    fit, 0.13°/frame left of 0.82 raw. `_013` 120–150 s ranks it first too,
    but weakly (0.57 of 0.67°/frame): a ride with near parallax doesn't
    suit the rotation-only fit.

## 9. Type 4 — Exposure and sync ✅

- 16-byte samples: `u64` timestamp in **ms**, `f64` exposure time in s
  (0.62–1.31 ms on these sunny clips).
- Two samples at the IMU's start, a ~1.3 s gap (~1.07 s at 30 fps), then
  one every **exactly 40 ms** to the end (at 29.97 fps: 33 and 34 ms in
  the nominal 63:37 mix of 1001/30 ms rounded to ms): no drift at all at ms resolution, where the X5's
  measured 15–40 ppm would show (7–12 ms over 309 s). So these are probably
  nominal times, and the clock rate can't be fitted from them.
- **Field 24 is not an exposure timestamp** (X5: it equals one exactly). It
  lies **5 ms before** the nearest one on both clips (4407 vs 4412, 4378 vs
  4383).
- ✅ **The frame's time is field 24 + field 28** (−12 ms), as in the X5's
  formula (measured 2026-10-21): fitting the image rotation of the four
  views (§8) against the gyro over a lag sweep puts frame 0 at 4394 ms
  (`_001`) and 4364.5 ms (`_003`) on the IMU clock, 13 ms before field 24;
  the app's offset (+1619.7 / +1588.7 ms after the first IMU sample) puts
  it at 4392.7 / 4363.7 ms. The app's sync fine-tune
  (`residual_stab --auto-sync`, `stitch::MeasureClipSync`) finds +1.1 /
  +0.8 ms on the same ranges, and +2.6 ms on the 30 fps take (600–630 s,
  a calm stretch: 13 cells), so the formula holds on the older firmware
  and at 30 fps too. The readout time (field 25, 36.9 ms) is used
  as read; it wasn't measured on its own.
- Counting from the first exposure at or after field 24: 7750 samples for
  7736 frames (4714 for 4704): 14 / 10 after the last frame.
- With field 24 (ms), field 28 (−12 ms) and field 25 (36.9 ms) the X5's
  sync formula (X5 §7.1) carries over once the units are ms; per
  telemetry-parser, files without `is_raw_gyro` are already in seconds after
  the field-24 shift. Checked against the picture: see above.

## 10. Type 9 ❓

- 48-byte records, a `u32` ms timestamp first, about every **160 ms**
  (155–165; 1971 records on 309 s), 30 all-zero records among them.
- The X5 layout (X5 §4.4) doesn't fit the next fields (`ev_target`
  ~1e-26), except `luma_struct` (word 4): `>> 19` gives 64–65, so ISO 100
  by the X5 formula.

## 11. What the app reads, and what it still needs

Read (2026-10-21):
- **The take's two files** as one clip's lenses: the `_00_` file (it has
  the trailer) is the clip, and its lens 1 comes from the `_10_` file beside
  it, taken only when that is a one-stream video of the same size and rate,
  at most a frame shorter, without a trailer of its own. The clip's length is
  the shorter file's. The proxy is found as `LRV_…_11_….insv`. Any of the
  three files names the clip (`camera::ClipFileOf`: the name says which
  `_00_` file, which must claim it), so importing, dropping or adding the
  `_10_` file or the LRV, or the whole folder, gives the clip once. The
  project saves the `_10_` file with the clip's media item (`"files"`), and
  the clip is missing when either file is.
- **The stitch from the 40-token `offset_v3`** (field 54; §7).
- **Telemetry in ms**, turned into µs (§8).
- **The preview** for the media card, stitched from types 2 and 5 (§5).
- **The matrix from the stream** (BT.601 here), as for every camera.

- **The IMU axis map** `z,x,y` (§8) and the X5's sync formula with ms
  units (§9), both measured on these clips; the two files paired frame n
  with frame n (§3.1).

Still needed:
- The stitch checked against Insta360 Studio's, and the axis map confirmed
  on a clip with deliberate turns about each axis.

## 12. Open questions

Answered 2026-10-21: the two files are frame-locked (§3.1); the frame's
time is field 24 + field 28 (§9); the axis map is `z,x,y` and the gyro is
in rad/s (§8).

1. Which instant field 24 marks relative to the exposure record (5 ms
   apart) -- the sync doesn't need it (§9).
2. What `gamma_mode` `linear` and `file_group_info.type` 6 mean for the
   mode set on the camera; the other modes and the lens guards are
   untested (§3.3).
3. How a long recording is split into files: not by 4 GB or by 22 min
   (`_004`, §1), nor at 29 min / 11 GB
   (`152938_…_003`, §1).
4. Type 9's layout; the `AMBA` box; field 60.

## Sources

- Our clips (above), 2026-10-20: `ffprobe`, `insv_trailer_dump`, a trailer
  and protobuf walk following X5 §4–§5.
- Insta360 X6 manual, file formats (one file vs. two):
  <https://onlinemanual.insta360.com/x6/en-us/operating-tutorials/storage/fileformat>;
  the same table in the X5 manual has the X3 / ONE X2 / ONE X rows (two files
  at 5.7K only, when no LRV is written):
  <https://onlinemanual.insta360.com/x5/en-us/operating-tutorials/storage/fileformat>
- Insta360 X-series shooting specs, codecs and colour profiles per model:
  <https://onlinemanual.insta360.com/onex2/en-us/specs/shooting-specs>
- Prevu3D, both `.insv` files needed per recording:
  <https://doc.prevu3d.com/en/realityplatform/dataset-preparation-and-upload/input-types/photogrammetry-data/360-video/insta360-capture-setup/>
- Insta360 LUT downloads (the ONE X2's LOG LUT):
  <https://www.insta360.com/download/i-log>
- Clifton Cameras, ONE X2 spec sheet (modes)
- Gyroflow telemetry-parser, `src/insta360/mod.rs` (IMU orientation tables
  and when each applies, timestamp units, lens profile from `offset_v3`),
  `extra_info.rs` (field names and enums) and `rig.rs` (one-lens-per-file
  layout, sibling hint, lens type 42):
  <https://github.com/AdrianEddy/telemetry-parser>
