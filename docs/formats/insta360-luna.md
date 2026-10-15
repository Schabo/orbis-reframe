# Insta360 Luna (Pro / Ultra) file format

**Desk research (2026-10-15), no Luna file examined.** From Insta360's specs
and reviews. Markers as in [`README.md`](README.md).

The Luna is a pocket **gimbal** camera, not a 360 camera: it records flat
video, so nothing about the `.insv` format or the stitch applies. In the app
it's a flat video (an overlay today).

## Streams

- 📖 MP4, **H.265**, **10-bit**, up to 120 Mb/s.
- 📖 Colour modes Standard, **Dolby Vision** and **I-Log 10-bit**.
- 📖 Luna Ultra video modes:

  | mode | frame | fps |
  |---|---|---|
  | 8K (16:9) | 7680×4320 | 30 / 25 / 24 |
  | 8K (2.35:1) | 7680×3264 | 30 / 25 / 24 |
  | 4K (16:9) | 3840×2160 | up to 120 |
  | PureVideo 4K | 3840×2160 | up to 60 |
  | 1080p | 1920×1080 | up to 240 |

- 📖 The Ultra has a second, telephoto camera; ❓ whether it records to a
  separate file or stream. The Pro has one camera; ❓ its modes.

## What the app would need

- The flat-video reader decodes on the CPU (swscale). It reads 10-bit and
  BT.2020, but has no conversion for log or HDR transfer curves: I-Log and
  Dolby Vision would look flat or wrong. 8K 10-bit on the CPU is likely too
  slow for preview.
- ❓ Whether the files carry gyro data. Insta360's action cameras write the
  `inst` trailer into their MP4s; with a mechanical gimbal it matters little.

## Sources

- Insta360 Luna Ultra specs: <https://www.insta360.com/specs/luna-ultra>
- Luna Ultra manual, shooting specs:
  <https://onlinemanual.insta360.com/lunaultra/en-us/specs/shooting>
- Luna Pro manual, shooting specs:
  <https://onlinemanual.insta360.com/lunapro/en-us/specs/shooting>
- PetaPixel, Luna Ultra full details:
  <https://petapixel.com/2026/06/10/luna-ultra-insta360-finally-reveals-full-details-of-the-leica-branded-gimbal-camera/>
- Notebookcheck, Luna Ultra hands-on:
  <https://www.notebookcheck.net/Dual-cam-Insta360-Luna-Ultra-official-hands-on-preview-1-inch-sensor-10-bit-iLog-f-1-8-aperture-take-on-DJI.1278337.0.html>
