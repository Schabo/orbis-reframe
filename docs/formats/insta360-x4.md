# Insta360 X4 file format

**Desk research (2026-10-15), no X4 file examined.** From public specs,
Insta360's manuals and Gyroflow's telemetry-parser, set against what our code
assumes. Written as differences from the X5 ([`insta360-x5.md`](insta360-x5.md),
"X5 §n" below). Markers as in [`README.md`](README.md).

## Files and streams

- 📖 One `.insv` per take holding both lenses, with the same
  `VID_…_00_…` / `LRV_…_01_…` naming as the X5 (X5 §1; the X6 manual names
  the X6, X5, X4 Air and X4 as one-file cameras). Recordings split every
  29 min 59 s, as on the X5.
- 📖 360 modes: 8K 30, 5.7K 60, 4K 100; H.264 or H.265, up to 200 Mb/s.
  Colour profiles Standard, Vivid and Flat.
- ⚠️ Per lens 3840² / 2880² / 1920², the X5's sizes. The capture-mode labels
  the app derives from the lens size would match.
- ❓ Bit depth. Insta360 doesn't say; one third-party listing claims 10-bit.
  The app's GPU stitch takes 8-bit only (see [`insta360-x6.md`](insta360-x6.md)).

## Trailer

- 📖 The same `inst` trailer as the X5 (X5 §4): telemetry-parser reads X4 and
  X5 trailers with one code path -- an Offsets table, records back to back,
  each closed by its six-byte descriptor.
- ❓ Whether it carries the 56-token MEI calibration string (X5 §5.3, field
  111/112). The native stitch needs it; without it the clip won't open.
- ❓ Whether the IMU is raw counts (`is_raw_gyro`, field 62) with the X5's
  full-scale ranges (X5 §6.1).

## IMU axes

- 📖 Gyroflow maps the X4's IMU with `yzX`, the same as the X5 (X5 §6.3). So
  our measured X5 mapping `z,-x,-y` may hold; it must be measured again on
  an X4 clip, the same way.

## Sources

- Insta360 X4 manual, shooting specs:
  <https://onlinemanual.insta360.com/x4/en-us/specs/shooting-specs>
- Insta360 X-series shooting specs, codecs and colour profiles per model:
  <https://onlinemanual.insta360.com/onex2/en-us/specs/shooting-specs>
- Insta360 X6 manual, file formats (one-file cameras):
  <https://onlinemanual.insta360.com/x6/en-us/operating-tutorials/storage/fileformat>
- Gyroflow telemetry-parser, `src/insta360/mod.rs` (trailer layouts, IMU
  orientation table): <https://github.com/AdrianEddy/telemetry-parser>
