# Antigravity A1 file format

**Desk research (2026-10-15), no A1 file examined.** From reviews and Gyroflow's
telemetry-parser, set against what our code assumes. The A1 is a 360 drone
from Antigravity, a brand incubated by Insta360, and writes the same `.insv`
family; written as differences from the X5 ([`insta360-x5.md`](insta360-x5.md),
"X5 §n" below). Markers as in [`README.md`](README.md).

## Streams

- 📖 Two 1/1.28" sensors, **180°** lenses (the X5's are ~200°, so the two
  pictures overlap much less at the seam).
- 📖 360 modes, H.264 or H.265, up to 170 Mb/s:

  | mode | frame | fps | per lens (⚠️) |
  |---|---|---|---|
  | 8K | 7680×3840 | 30 / 25 / 24 | 3840² |
  | **5.2K** (new) | 5248×2624 | 60 / 50 / 30 / 25 / 24 | **2624²** |
  | 4K | 3840×1920 | 100 / 30 / 25 / 24 | 1920² |

- 📖 Files are `.insv`, offloaded from the card. ❓ Whether one file holds
  both lenses.
- ❓ How much of the drone itself (props, arms) is in the picture -- it would
  need a mask.

## Trailer

- 📖 The same `inst` trailer, **laid out differently** (telemetry-parser): the
  Offsets table (X5 §4.2) is the only index; each record sits at the start
  of a **256 KiB slot**, and there are **no six-byte descriptors** after the
  records.
- The app's trailer walker takes each record's position from the table, but
  then reads its id and size from the descriptor after it -- on the A1, the
  next slot's bytes. Taking id and size from the table entry instead (as
  telemetry-parser does) fixes that and costs the X5 nothing.
- ❓ Calibration strings, IMU layout and axes.

## Sources

- Shotkit, Antigravity A1 review (specs, formats):
  <https://shotkit.com/antigravity-a1-review/>
- Space.com, Antigravity A1 review (sensors, weight):
  <https://space.com/technology/antigravity-a1-drone-review>
- Gyroflow telemetry-parser, `src/insta360/mod.rs` (the slotted,
  descriptor-less layout): <https://github.com/AdrianEddy/telemetry-parser>
