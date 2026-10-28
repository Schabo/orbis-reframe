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

## Modes and sample clips

All the A1's modes 📖 (Antigravity's spec table as retailers and reviews
reprint it, and the spring 2026 update for timelapse; Sources). We have no
A1 clip yet. The status words are the X5's (X5 §3.4).

| Mode | Spec | Files | Our clips | Status |
|---|---|---|---|---|
| 360 Video 8K | 7680×3840, 30 / 25 / 24 | `.insv` ❓ one file or one per lens, ❓ LRV | none | **wanted first**: the trailer layout, the 180° lens calibration |
| 360 Video 5.2K | 5248×2624, 60 / 50 / 30 / 25 / 24 | as above | none | **wanted**: a lens size no Insta360 camera has (2624² ⚠️), and 60 fps |
| 360 Video 4K | 3840×1920, 100 / 30 / 25 / 24 | as above | none | **wanted**: 100 fps |
| 4K Slow Motion | 3840×1920, 30 / 25 / 24 | ❓ | none | **wanted**: whether the file is tagged at the playback rate or the capture rate (100 ⚠️) |
| H.264 encoding | up to 170 Mb/s, as H.265 | as above | none | **wanted**: one clip in each codec |
| Timelapse | added by the spring 2026 update; ❓ sizes and intervals | ❓ | none | wanted |
| 360 Photo: Standard, HDR, Burst, AEB | 55 MP 10496×5248, 14 MP 5248×2624 | `.insp`; `.dng` | none | wanted, low |
| Internal storage (20 GB) vs microSD | | ❓ the same files | none | low: say which a clip came from |
| Audio | not in the spec table | ❓ whether a track is recorded | none | checked on the first clip |
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
