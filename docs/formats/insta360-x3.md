# Insta360 X3 file format

**Desk research (2026-10-20), no X3 file examined.** From Insta360's
shooting specs and file-format tables, Gyroflow's telemetry-parser and an
owner's forum report. The X3 is the ONE X2's successor and records the same
way, two files per take at 5.7K, so this is written as differences from the
ONE X2 ([`insta360-one-x2.md`](insta360-one-x2.md), "ONE X2 §n" below) and
the X5 ([`insta360-x5.md`](insta360-x5.md), "X5 §n"). Markers as in
[`README.md`](README.md).

## Files and streams

- 📖 **Two files per take at 5.7K, one below.** Insta360's file-format
  table: 360 video at 5.7K gives two `VID` files and one `LRV`; at 4K and
  3K one `VID` file and one `LRV`. No LRV in Bullet Time, TimeShift,
  Timelapse, or video at 4K 60 / 50, 3K 100 or above 100 fps. The ONE X2
  works the same (ONE X2 §1); ❓ whether the X3's files are laid out like
  the ONE X2's (`_00_` / `_10_` per lens, the trailer on `_00_` and the LRV
  only, `LRV_…_11_….insv`).
- 📖 ⚠️ An owner reports that the `_00_` file holds the front lens plus the
  GPS, which matches the ONE X2.
- 📖 **H.264 only**, up to 120 Mb/s. Filters Standard, Vivid and **LOG**.
  ❓ Bit depth (presumably 8-bit, like the ONE X2), the matrix and range
  the streams are tagged with (the ONE X2's say BT.601, ONE X2 §3.1).
- 📖 Audio 48 kHz 16-bit AAC.
- ❓ Which calibration strings the trailer carries (the ONE X2 has the
  40-token `offset_v3` but not the X5's 56-token one), the IMU layout, rate
  and units (the ONE X2's are doubles at 500 Hz, timestamps in ms), lens
  profiles (field 145), and the IMU axis map: telemetry-parser has no X3
  entry, so it gets that library's default orientation.

## Modes and sample clips

All the X3's modes 📖 (Insta360's shooting specs, Sources). We have no X3
clip yet. The status words are the X5's (X5 §3.4).

| Mode | Spec | Files | Our clips | Status |
|---|---|---|---|---|
| 360 Video 5.7K | 5760×2880, 30 / 25 / 24 | `_00_` + `_10_` + LRV | none | **wanted first** |
| 360 Video 4K | 3840×1920, 60 / 50 / 30 / 25 / 24 | one `VID` (+ LRV at 30 and below) | none | **wanted**: the one-file layout |
| 360 Video 3K | 100 | one `VID`, no LRV | none | wanted |
| Active HDR | ❓ | | none | **wanted** |
| LOG filter | | | none | **wanted**, with Insta360's LUT if one is published ❓ |
| Vivid filter | | | none | wanted, low: probably baked into the picture |
| Timelapse, TimeShift | ❓ | no LRV | none | wanted |
| Bullet Time | ❓ | no LRV | none | wanted |
| Loop Recording | | | none | wanted, low |
| Split recording | | ❓ | none | wanted, low: one take over the split length |
| Single-lens Video | 4K 3840×2160 30 / 25 / 24; 3.6K 3584×2016, 2.7K 2720×1530, 1080p 1920×1088 at 60…24 | `.mp4` | none | wanted, low |
| Me Mode | listed among the single-lens modes, but its own spec row says not supported ❓ | | none | low |
| 360 Photo (Interval, Starlapse, Burst, HDR) | 72 MP 11968×5984, 18 MP 5952×2976 | `.insp`; `.dng` with PureShot RAW | none | wanted, low |
| Single-lens Photo | 36 MP 5984², 9 MP 2976² | `.insp` | none | low |
| Audio: Wind Reduction, Stereo, 360 Direction Focus | | | none | one clip of each, low |
| Lens guards | | | none | **one clip with, one without** |
| IMU axes | | | none | **a moving clip with deliberate turns about each axis** |

The most useful first set, 10–30 s each, straight from the card with the
whole folder (`DCIM` and `MISC`), as in "Sample clips wanted"
([`README.md`](README.md#sample-clips-wanted)):

1. 5.7K 30, Standard, hand-held or on a stick, with slow deliberate turns
   left-right, up-down and a roll.
2. 4K 30, to see the one-file layout.
3. 5.7K in LOG.
4. Active HDR.
5. A few seconds each of Timelapse, TimeShift and Bullet Time.

With the camera's firmware version, and whether lens guards were on.

## Sources

- Insta360 X-series shooting specs (X3 column: modes, resolutions, frame
  rates, codec, bitrate, filters, audio, photo sizes and formats):
  <https://onlinemanual.insta360.com/x5/en-us/specs/shooting-specs>
- Insta360 file formats (two files at 5.7K, when no LRV is written,
  photo formats):
  <https://onlinemanual.insta360.com/x5/en-us/operating-tutorials/storage/fileformat>
- Mapillary forum, an X3 owner on the `_00_` / `_10_` files:
  <https://forum.mapillary.com/t/insta-360-one-x3/6587>
- Gyroflow telemetry-parser, `src/insta360/mod.rs` (IMU orientation
  tables, no X3 entry): <https://github.com/AdrianEddy/telemetry-parser>
