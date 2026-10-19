# Insta360 X6 file format

**Desk research (2026-10-15), then a contributor's test set (2026-10-18):
24 clips, 8K / 6K / 4K, Standard, I-Log and Dolby Vision, firmware v1.2.36,
read with our code on their machine.** The ✅ below are from that set; the
maintainer hasn't examined an X6 file yet. Public specs, Insta360's manuals
and Gyroflow's telemetry-parser for the rest. Written as differences from the
X5 ([`insta360-x5.md`](insta360-x5.md), "X5 §n" below). Markers as in
[`README.md`](README.md).

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

- ✅ Clips at all three lens sizes open and stitch. The app names a clip's
  mode by its lens size: 3008² is **6K** (the X6 row of `insv::InsvModel`).
- ✅ Vulkan decode + stitch on an AMD Radeon 8060S (Strix Halo iGPU, RADV),
  8K 30: ~84 fps decode, ~35 fps for the full 7680-wide stitch, ~60 fps for
  the direct view. 8K 50 is ~1.5 Gpx/s across both lenses, ~1.7× the X5's
  8K 30 (X5 §3.3); real-time decode at that rate isn't measured.

## Colour: 10-bit, log and HDR

- 📖 Colour modes Standard, **Dolby Vision** and **I-Log 10-bit**.
- ✅ Every clip of the set is H.265 Main 10 (`yuv420p10le`), in all three
  colour modes. Standard and I-Log clips are tagged `bt709`; **Dolby Vision
  clips are tagged `bt2020nc` + `arib-std-b67` (HLG), limited range**, with
  no Dolby Vision RPU side data. ❓ The range Standard and I-Log clips are
  tagged with (the X5's are full range, and the GPU conversion assumes it).
- ✅ The mode is only in the trailer, record 0x101 field 22 `gamma_mode`:
  `standard`, `I_Log` or `Dolby_Vision` (X5 §3.3 for the X5's values). The
  app reads it as the clip's colour profile; only `I_Log` is recognised as
  a log profile so far.
- The app's GPU stitch samples the decoder's pictures through a BT.709
  full-range ycbcr conversion. Since 2026-10-18 it takes 10-bit HEVC (Main
  10) too: FFmpeg's Vulkan decoder gives P010 images
  (`VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16`), the stitcher's
  conversion is made for that format (by the stream's bit depth), and the
  viewport's shared lens planes are 16-bit (R16 / R16G16). Checked on a
  stand-in -- the X5 I-Log clip re-encoded to Main 10 with its trailer
  kept: export (both GPU paths, and the CPU fallback) and playback match
  the 8-bit original at 46-48 dB PSNR, in real time -- and ✅ on the
  contributor's X6 clips. The conversion is still BT.709 full range
  whatever the stream says, which is wrong for Dolby Vision clips.
- An I-Log clip stitches correctly but looks flat (saturation ~0.16 vs
  ~0.31 for Standard) until Insta360's LUT is applied (below). Dolby
  Vision clips are HLG / BT.2020 as tagged and need an HLG -> Rec.709 LUT
  (Insta360 publishes none).
- The X5 has I-Log too, 8-bit (X5 §3.3).
- 📖 Insta360's 10-bit I-Log LUTs (`X6_I-Log_LUTs.zip`, version 2.0, made
  2026-05-20): 3D `.cube` files, each at 33³ and 65³, from I-Log to Rec.709
  / BT.1886 (`X6_I-Log_to_Rec709_BT1886_s65_v2.cube`), to Rec.2020 PQ and to
  Rec.2020 HLG (1000 nits, 203 nits diffuse white). The app reads all of
  them (Colour ▸ LUT); the Rec.709 one is the one for its SDR output. The
  10-bit pictures are graded in float before the 8-bit output, so they
  shouldn't band.

## Trailer

- ✅ The X5's framing (magic, footer, record directory). Record 0x101 has
  serial, model (`Insta360 X6`) and firmware; the IMU (0x300, ~1 kHz) and
  exposure (0x400) records parse with the X5's layouts.
- ✅ The extended MEI calibration string is there, the X5's 56-token
  layout: e.g. xi 2.455, fx / fy 7232, cx / cy ~3850 / 3880, roll ~90,
  combined frame **15488×7744**, type 193. The sanity ranges in
  `Calibration.cpp` hold.
- ✅ Field 27 `window_crop_info`: src 7744², **dst 7680²** (X5: 5376² ->
  5312², X5 §5.1). The dst width is the window fx / fy are given in, so the
  stitch reads it from the file (`InsvMakerNotes::crop_dst_w` ->
  `LensCalibExt::focalRefW`); with the X5's 5312 the lens circles come out
  too small and the sphere has gaps between them. A file without the field
  takes its model's value from `insv::InsvModel`, with a log line.
- ✅ **No field 145 lens-profile table**: there are no lens-guard or
  dive-case profiles, so the stitch takes every lens as bare at the default
  200° field of view. Stitches at all three sizes cover the sphere without
  gaps, so 200° looks right; guards can't be corrected without a profile.
- ❓ The IMU axis map. telemetry-parser has no X6-specific code (an unknown
  model gets its default orientation, `Xyz`); the app uses the X5's map,
  `z,-x,-y` (X5 §6.3), until a clip with deliberate rotations about each
  axis confirms or corrects it. The accel / gyro ranges parse sensibly at
  rest.

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
