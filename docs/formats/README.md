# Camera file formats

What this project knows about each camera's recorded files: one document per
camera, or per family where the cameras barely differ. The X5 document is the
full reference, built from our own footage; the others are written as
differences from it, or, for another vendor, from scratch.

| Camera | Kind | Document | Status in the app |
|---|---|---|---|
| Insta360 X5 | 360, one `.insv` with both lenses | [`insta360-x5.md`](insta360-x5.md) | supported; every fact checked on our clips |
| Insta360 X4 | 360, one `.insv` with both lenses | [`insta360-x4.md`](insta360-x4.md) | not yet; desk research, no clips |
| Insta360 X6 | 360, one `.insv`, 10-bit modes | [`insta360-x6.md`](insta360-x6.md) | experimental: opens, stitches and exports, 10-bit, I-Log and HLG through LUTs (a contributor's clips; not yet run by the maintainer); open: the IMU axis map (stabilization), the HLG LUT on real Dolby Vision clips, 8K 50 real time |
| Insta360 ONE X2 | 360, one `.insv` per lens at 5.7K | [`insta360-one-x2.md`](insta360-one-x2.md) | experimental: 5.7K clips open from any of the take's files, stitch, stabilize and export (the maintainer's clips); open: a check against Insta360 Studio's stitch, a turning clip to confirm the IMU axes, Horizon lock tilting on some clips, the 4K / 3K one-file modes |
| Insta360 X3 | 360, one `.insv` per lens at 5.7K | [`insta360-x3.md`](insta360-x3.md) | not yet; desk research, no clips |
| Antigravity A1 | 360 drone, one `.insv` with both lenses | [`antigravity-a1.md`](antigravity-a1.md) | not yet: 8K clips open with their calibration, proxy, IMU and GPS (a contributor's clips); open: Horizon lock in fast flight (levels to the drone's tilt), the preview picture |
| Insta360 Luna (Pro / Ultra) | flat gimbal camera, `.mp4` | [`insta360-luna.md`](insta360-luna.md) | not yet; desk research, no clips |
| DJI Osmo 360, Osmo 360 II / Air, Avata 360 | 360, `.osv` | [`dji-360.md`](dji-360.md) | not yet; desk research, no clips |
| GoPro MAX, MAX2 | 360 stitched in the camera, `.360` (EAC) | [`gopro-max.md`](gopro-max.md) | not yet; desk research, no clips |

## Sample clips wanted

A camera's document lists every mode the camera has and which of them we
have clips of: so far the
[X5](insta360-x5.md#34-every-mode-and-our-clips-of-it),
[X6](insta360-x6.md#modes-and-sample-clips),
[ONE X2](insta360-one-x2.md#33-every-mode-and-our-clips-of-it),
[X3](insta360-x3.md#modes-and-sample-clips) and
[Antigravity A1](antigravity-a1.md#modes-and-sample-clips). The others get
the list when their first clips arrive.
Most of the documents above are desk research: **we need short clips from
every camera above that isn't supported yet** (except the ONE X2: the
maintainer has one) to find out what the files really hold. If you own one, please
[offer some clips](https://github.com/Schabo/orbis-reframe/issues/new?template=sample_clips.yml).

- **Straight from the card**, untouched: not exported, trimmed or re-saved
  by Insta360 Studio, DJI Studio, GoPro Quik or any other app (they rewrite
  the metadata we need to read). Copy the whole set of files the camera
  wrote for each take: both `_00_` and `_10_` files on two-file cameras, and
  the `.lrv` / `.LRF` proxies.
- **Short**: 10–30 s per clip is plenty. Hold the camera still for the
  first few seconds, then move and turn it: the movement is what the
  stabilization data is checked against.
- **One clip per recording mode** if you can (resolution and frame rate),
  and one in each colour profile the camera has (standard, log, HDR / Dolby
  Vision). With and without a lens guard, if you have one.
- **Say** which camera, its firmware version, and the mode and settings of
  each clip.
- **Privacy**: clips can hold GPS positions, faces, licence plates and the
  camera's serial number. Film somewhere you're happy to share. The clips
  are only used to develop and test orbis-reframe: they're never put in the
  repository or published. An issue and the links in it are public; if the
  footage shouldn't be, open the issue without the link and say so.

Wanted most: the **Insta360 X4** and **X6**, the **DJI Osmo 360** family
and the **GoPro MAX / MAX2**, then the rest of the table.

## Confidence markers

Every document marks its facts the same way:
- ✅ **verified**: parsed by our code and cross-checked (ExifTool byte-for-byte,
  ffprobe, several clips agreeing, and/or an independent implementation)
- 📖 **documented externally**: stated by an independent source and
  consistent with our files (or, without files, not contradicted by
  anything), but not exercised by our code
- ⚠️ **inferred**: strong evidence, but not validated end to end
- ❓ **unknown**

A desk-research document has no ✅: nothing in it has met a real file yet.

## Where the knowledge may come from

- Files the cameras recorded, public documentation (manuals, spec sheets,
  ExifTool, Gyroflow's telemetry-parser), forums and patents.
- **Never** from disassembling or decompiling a vendor's software or SDK
  ourselves.
- Some open-source projects state facts they found by reverse-engineering a
  vendor's software. **File-format facts** from them -- schemas, field
  numbers, units, which track holds which lens, the lens model's formula,
  IMU axes and timing -- may be used: cite the source and check each against
  our own files before the code relies on it. **The vendor's own tuning and
  algorithm choices** -- blend and seam widths, which of several
  calibrations the vendor's app picks, constants read out of a running app --
  aren't needed to read a file: we measure those ourselves on our footage.
- Every fact says where it came from.
