# Antigravity A1 file format

**Desk research (2026-10-15); first clips examined 2026-10-29**: three
contributed 8K clips. The trailer framing below is checked on them; the rest
is still desk research, from reviews and Gyroflow's telemetry-parser, set
against what our code assumes. The A1 is a 360 drone from Antigravity, a
brand incubated by Insta360, and writes the same `.insv` family; written as
differences from the X5 ([`insta360-x5.md`](insta360-x5.md), "X5 §n" below).
Markers as in [`README.md`](README.md).

## Streams

- 📖 Two 1/1.28" sensors, **180°** lenses (the X5's are ~200°, so the two
  pictures overlap much less at the seam).
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
- ❓ How much of the drone itself (props, arms) is in the picture -- it would
  need a mask.

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
- ✅ Records in the clips: types 1 (Metadata, protobuf), 2, 3, 4, 7, 9, 11,
  14, 23, 32, 33, 37, 38 and 56; the proxy holds 1 and 2. What the ones the
  X5 doesn't have hold is ❓.
- ✅ Record 1 is the X5's protobuf (X5 §5.1); on our clips: model (field 2)
  `Antigravity A1`, firmware `10.0.397.3_5.5`; field 27 `{5248, 5248, 5248,
  5248}`, so the focal reference is 5248; only the **40-token `offset_v3`**
  MEI calibration (field 54, X5 §5.3; no 56-token string), in a 10496×5248
  side-by-side frame, `xi` 1.504, lens type 155; fields 24, 25 (readout
  ~21.25 ms), 28 (3.7 ms), 62 (raw IMU) and 65 (±32 g, ±2000 °/s) as on the
  X5, and the IMU (~1 kHz) and exposure records decode with the X5's
  layout, in µs. Field 7 is **`YYYYMMDDhhmm`**, without the X5's seconds, so
  the recording time comes from the file name, which has them.
- ⚠️ The calibration's extrinsics are yaw / pitch / roll ≈ (−177.5, −89.95,
  90.4) and (−179.4, −89.9, 89.4): pitch −90° puts yaw and roll about the
  same axis, so the order they compose in matters (it doesn't on the X5).
  Pitch, then yaw, then roll (X5 §5.3) leaves the lens axes opposite to
  0.2°, and the per-clip seam fit converges to 0.02° on all three clips
  with small corrections (lens 1 pitch +0.14 to +0.24°, roll +0.40 to
  +0.42°, centre −0.06 to −0.08 %, radius −0.17 to −0.20 %). Before the
  fit the lenses are 0.24-0.53° apart at the seam (X5: ~0.17°).
- ❓ The IMU axes, fields 69 and 91 (unit quaternions; 69 is (w, x, y, z) =
  (0.5, 0.5, −0.5, 0.5), a pure axis permutation), field 98 (absent: GPS
  not yet lined up with the video).

## Modes and sample clips

All the A1's modes 📖 (Antigravity's spec table as retailers and reviews
reprint it, and the spring 2026 update for timelapse; Sources). Our clips:
three 8K clips from one contributor (firmware `10.0.397.3_5.5`, 2026-10-29).
The status words are the X5's (X5 §3.4).

| Mode | Spec | Files | Our clips | Status |
|---|---|---|---|---|
| 360 Video 8K | 7680×3840, 30 / 25 / 24 | ✅ one `.insv` with both lenses (two 3840² HEVC streams), and a `PRX_….prx` proxy | 3, 29.97 fps, H.265, standard colour | trailer read; stitch, orientation and IMU axes not yet |
| 360 Video 5.2K | 5248×2624, 60 / 50 / 30 / 25 / 24 | as above | none | **wanted**: a lens size no Insta360 camera has (2624² ⚠️), and 60 fps |
| 360 Video 4K | 3840×1920, 100 / 30 / 25 / 24 | as above | none | **wanted**: 100 fps |
| 4K Slow Motion | 3840×1920, 30 / 25 / 24 | ❓ | none | **wanted**: whether the file is tagged at the playback rate or the capture rate (100 ⚠️) |
| H.264 encoding | up to 170 Mb/s, as H.265 | as above | none | **wanted**: one clip in each codec |
| Timelapse | added by the spring 2026 update; ❓ sizes and intervals | ❓ | none | wanted |
| 360 Photo: Standard, HDR, Burst, AEB | 55 MP 10496×5248, 14 MP 5248×2624 | `.insp`; `.dng` | none | wanted, low |
| Internal storage (20 GB) vs microSD | | ❓ the same files | none | low: say which a clip came from |
| Audio | not in the spec table | ✅ no audio track in our clips | 3 | done |
| Drone in the picture (props, arms, landing gear) | | | none | **wanted**: a clip with sky above and ground below, to see what a mask must cover |
| Replaceable lenses | | | none | wanted, low: whether the calibration in the file follows a lens swap |
| IMU axes | | | none | **a flight with deliberate yaw, pitch and roll**, e.g. in Sport mode |

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
