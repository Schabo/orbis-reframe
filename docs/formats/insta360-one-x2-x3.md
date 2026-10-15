# Insta360 ONE X2 and X3 file format

**Desk research (2026-10-15), no ONE X2 or X3 file examined.** From public
specs, Insta360's manuals, a user report and Gyroflow's telemetry-parser, set
against what our code assumes. Written as differences from the X5
([`insta360-x5.md`](insta360-x5.md), "X5 §n" below). Markers as in
[`README.md`](README.md).

The older, **two-file** generation: one file per lens.

## Files

- 📖 **One file per lens**: `VID_…_00_NNN.insv` and `VID_…_10_NNN.insv`, the
  second digit pair being the lens-direction digit (X5 §1). The X6 manual:
  "X6, X5, X4 Air and X4 generate only one VID file containing data from
  both lenses, unlike the X3, which produced two".
- 📖 telemetry-parser describes "one lens per file" as a layout the camera
  *declares*, and a *sibling hint* naming the other file of the pair. It
  warns that a long recording split into two files is also a group of two,
  so a pair can't be matched by name alone.
- ⚠️ One X3 owner reports that the `_00_` file holds the front lens plus the
  GPS. ❓ Which file carries the IMU and calibration.
- The app's demux opens one file with both lens streams; a pair of files
  isn't supported. The clip would be the pair everywhere: opening, adding,
  the project file, missing media, the `.lrv`.

## Streams

- 📖 360 video up to 5.7K 30 (5760×2880); ONE X2 also 4K 50 and 3K 100.
  ⚠️ So 2880² per lens at 5.7K, the X5's 5.7K size.
- 📖 The X3 records **H.264 only**, up to 120 Mb/s; the ONE X2 H.264 or
  H.265, up to 100 Mb/s. The app's decode doesn't depend on the codec, and
  its Vulkan decoder checks for H.264 decode support itself.
- 📖 Colour profiles Standard, Vivid and **LOG**. ❓ Bit depth, presumably
  8-bit.

## Calibration

- ⚠️ telemetry-parser builds its lens profile only from `offset_v3`
  (X5 §5.3, field 54), and lists the ONE X2's IMU orientation only for clips
  *without* `offset_v3`. So the ONE X2 most likely carries only the older
  calibration strings (`offset`, maybe `offset_v2`; X5 §5.2, §5.3) -- not the
  56-token MEI string the app's stitch requires. It would need a stitch model
  from those strings, checked against Insta360 Studio's stitch of the same
  clip.
- ❓ What the X3 carries.
- 📖 telemetry-parser's rig code mentions a ONE X2 lens type 42, with a lens
  guard whose optical table differs from its mask table. ❓ Whether the
  lens-accessory profiles (X5 §5.4) exist.

## IMU

- ⚠️ Probably the 56-byte layout of six doubles already in g and rad/s
  (X5 §6.1, `is_raw_gyro` false); the parser reads it.
- 📖 Gyroflow maps the ONE X2's IMU with `xZy`, against `yzX` on the X4/X5:
  the app's axis mapping (X5 §6.3) must be measured per model.

## Sources

- Insta360 X6 manual, file formats (one file vs. two):
  <https://onlinemanual.insta360.com/x6/en-us/operating-tutorials/storage/fileformat>
- Insta360 X-series shooting specs, codecs and colour profiles per model:
  <https://onlinemanual.insta360.com/onex2/en-us/specs/shooting-specs>
- Mapillary forum, an X3 owner on the `_00_` / `_10_` files:
  <https://forum.mapillary.com/t/insta-360-one-x3/6587>
- Prevu3D, both `.insv` files needed per recording:
  <https://doc.prevu3d.com/en/realityplatform/dataset-preparation-and-upload/input-types/photogrammetry-data/360-video/insta360-capture-setup/>
- Clifton Cameras, ONE X2 spec sheet (modes)
- Gyroflow telemetry-parser, `src/insta360/mod.rs` (IMU orientation tables,
  lens profile from `offset_v3`) and `rig.rs` (one-lens-per-file layout,
  sibling hint, lens type 42): <https://github.com/AdrianEddy/telemetry-parser>
