# Camera file formats

What this project knows about each camera's recorded files: one document per
camera, or per family where the cameras barely differ. The X5 document is the
full reference, built from our own footage; the others are written as
differences from it, or, for another vendor, from scratch.

| Camera | Kind | Document | Status in the app |
|---|---|---|---|
| Insta360 X5 | 360, one `.insv` with both lenses | [`insta360-x5.md`](insta360-x5.md) | supported; every fact checked on our clips |
| Insta360 X4 | 360, one `.insv` with both lenses | [`insta360-x4.md`](insta360-x4.md) | not yet; desk research, no clips |
| Insta360 X6 | 360, one `.insv`, 10-bit modes | [`insta360-x6.md`](insta360-x6.md) | not yet; desk research, no clips |
| Insta360 ONE X2, X3 | 360, one `.insv` per lens | [`insta360-one-x2-x3.md`](insta360-one-x2-x3.md) | not yet; desk research, no clips |
| Antigravity A1 | 360 drone, `.insv` | [`antigravity-a1.md`](antigravity-a1.md) | not yet; desk research, no clips |
| Insta360 Luna (Pro / Ultra) | flat gimbal camera, `.mp4` | [`insta360-luna.md`](insta360-luna.md) | not yet; desk research, no clips |
| DJI Osmo 360, Osmo 360 II / Air, Avata 360 | 360, `.osv` | [`dji-360.md`](dji-360.md) | not yet; desk research, no clips |
| GoPro MAX, MAX2 | 360 stitched in the camera, `.360` (EAC) | [`gopro-max.md`](gopro-max.md) | not yet; desk research, no clips |

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
