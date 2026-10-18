# Insta360 X6 file format

**Desk research (2026-10-15), no X6 file examined.** From public specs,
Insta360's manuals and Gyroflow's telemetry-parser, set against what our code
assumes. Written as differences from the X5 ([`insta360-x5.md`](insta360-x5.md),
"X5 §n" below). Markers as in [`README.md`](README.md).

## Files and streams

- 📖 One `.insv` per take holding both lenses, the X5's naming (X5 §1).
- 📖 Two 1/1.1" square sensors (a third more area than the X5's). H.265
  only, up to 180 Mb/s.
- 📖 360 modes:

  | mode | frame | fps | per lens (⚠️) |
  |---|---|---|---|
  | 8K | 7680×3840 | 50 / 48 / 30 / 25 / 24 | 3840² |
  | **6K** (new) | 6016×3008 | 60 / 50 / 48 / 30 / 25 / 24 | **3008²** |
  | 4K | 3840×1920 | 100 / 60 / 50 / 48 / 30 / 25 / 24 | 1920² |

- The app names a clip's mode by its lens size (8K / 5.7K / 4K); 3008² has
  no name yet.
- 8K 50 is ~1.5 Gpx/s across both lenses, ~1.7× the X5's 8K 30 (X5 §3.3).
  Real-time decode at that rate isn't measured.

## Colour: 10-bit, log and HDR

- 📖 Colour modes Standard, **Dolby Vision** and **I-Log 10-bit**.
- The app's GPU stitch samples the decoder's pictures through a BT.709
  full-range ycbcr conversion. Since 2026-10-18 it takes 10-bit HEVC (Main
  10) too: FFmpeg's Vulkan decoder gives P010 images
  (`VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16`), the stitcher's
  conversion is made for that format (by the stream's bit depth), and the
  viewport's shared lens planes are 16-bit (R16 / R16G16). Checked only on
  a stand-in -- the X5 I-Log clip re-encoded to Main 10 with its trailer
  kept: export (both GPU paths, and the CPU fallback) and playback match
  the 8-bit original at 46-48 dB PSNR, in real time. ❓ Whether real X6
  files are tagged full range like the X5's (the conversion assumes it),
  and their trailer, are unchecked: it needs a real X6 clip.
- I-Log is graded with Insta360's LUT (below); Dolby Vision presumably
  needs BT.2020 with a PQ or HLG transfer (❓ unconfirmed) and a tone map.
- The X5 has I-Log too, 8-bit (X5 §3.3).
- 📖 Insta360's 10-bit I-Log LUTs (`X6_I-Log_LUTs.zip`, version 2.0, made
  2026-05-20): 3D `.cube` files, each at 33³ and 65³, from I-Log to Rec.709
  / BT.1886 (`X6_I-Log_to_Rec709_BT1886_s65_v2.cube`), to Rec.2020 PQ and to
  Rec.2020 HLG (1000 nits, 203 nits diffuse white). The app reads all of
  them (Colour ▸ LUT); the Rec.709 one is the one for its SDR output. The
  10-bit pictures are graded in float before the 8-bit output, so they
  shouldn't band.

## Trailer

- ❓ telemetry-parser has no X6-specific code; an unknown model gets its
  default IMU orientation, `Xyz`. The trailer's framing, the calibration
  strings (X5 §5) and the IMU layout (X5 §6) are all unconfirmed.
- The larger sensor likely changes the calibration's reference frame
  (10752×5376 on the X5) and maybe the IMU part (field 51). Both are read
  from the file, but the sanity ranges in `Calibration.cpp` were set from
  X5 values.

## Sources

- Insta360 X6 manual, shooting specs:
  <https://onlinemanual.insta360.com/x6/en-us/specs/shooting-specs>
- Insta360 X6 manual, file formats:
  <https://onlinemanual.insta360.com/x6/en-us/operating-tutorials/storage/fileformat>
- Insta360 manual, I-Log mode (X5 and X6):
  <https://onlinemanual.insta360.com/x6/en-us/operating-tutorials/capture-preview/features/ilog>
- Insta360 X6 specs page: <https://www.insta360.com/specs/x6>
- Insta360 I-Log LUT downloads: <https://www.insta360.com/download/i-log>
- Gyroflow telemetry-parser, `src/insta360/mod.rs`:
  <https://github.com/AdrianEddy/telemetry-parser>
