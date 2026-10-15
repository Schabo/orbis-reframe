# Third-party components

orbis-reframe is licensed under the GNU General Public License v3.0 or later
(`LICENSE`); the `.insv` library in `src/insv/` under MIT OR Apache-2.0
(`src/insv/LICENSE-MIT`, `src/insv/LICENSE-APACHE`). This file lists what
else goes into the program and its packages, under which license, and where
the license text is.

Paths are given in the source tree. In the packages (`/usr` for the `.deb`,
the unpacked folder for the tarball) they are in `share/doc/orbis-reframe/`:
this file, `LICENSE`, `LICENSE.lucide`, and `licenses/` (ncnn, nlohmann/json,
and `src/insv/`'s two as `insv-LICENSE-MIT` / `insv-LICENSE-APACHE`). The
models' licenses are next to each model, in
`share/orbis-reframe/models/<model>/`.

## Shipped in the packages

| Component | Version | License | What it does here | License text |
|---|---|---|---|---|
| [ncnn](https://github.com/Tencent/ncnn) (Tencent) | 20260526, the `ubuntu-2404-shared` release | BSD-3-Clause (its third-party parts: zlib, BSD-2-Clause, BSD-3-Clause) | Runs the number-plate model on the GPU (plate scan). `libncnn.so.1` is bundled in `lib/orbis-reframe/` -- it isn't packaged in Debian | `packaging/licenses/ncnn-LICENSE.txt` |
| [nlohmann/json](https://github.com/nlohmann/json) (Niels Lohmann) | 3.11.3 (Debian's `nlohmann-json3-dev`) | MIT | Reads and writes project files. Header-only, compiled into the binary | `packaging/licenses/nlohmann-json-LICENSE.MIT` |
| [Lucide](https://lucide.dev/) icons (some derived from [Feather](https://feathericons.com/)) | -- | ISC (Feather parts: MIT) | Toolbar and panel icons, compiled into the binary | `resources/icons/LICENSE.lucide` |
| [DeepFilterNet 3](https://github.com/Rikorose/DeepFilterNet) model (Hendrik Schröter and contributors) | repository commit `84d57ec` | MIT OR Apache-2.0 | The "Voice only" / "Voice + ambience" noise modes | `resources/models/deepfilternet3/LICENSE-MIT`, `LICENSE-APACHE` |
| [VitTrack](https://github.com/opencv/opencv_zoo) model (OpenCV Zoo) | `object_tracking_vittrack_2023sep` | Apache-2.0 | Checks that subject tracking still follows the same subject | `resources/models/vittrack/LICENSE-APACHE` |
| YOLOv9-s licence-plate model from [open-image-models](https://github.com/ankandrew/open-image-models) (ankandrew) | `yolo-v9-s-608-license-plates-end2end`, converted for ncnn | MIT | Finds number plates for the privacy blur | `resources/models/plates/LICENSE-MIT` |

Each model folder also has a `README.md` with its source, checksums and any
changes made to it.

## Used from the system, not shipped

These come from the distribution's own packages (the `.deb` depends on them;
the tarball's install guide lists them). Their license texts are installed
with them, in `/usr/share/doc/<package>/copyright`.

| Component | Debian package (Debian Testing) | License | Linked how |
|---|---|---|---|
| Qt 6 (Core, Gui, Widgets, Multimedia; Wayland and SVG plugins) | `libqt6core6t64` etc., 6.10 | LGPL-3.0 (also GPL-2.0 / GPL-3.0) | Shared libraries; the Wayland and SVG plugins loaded by Qt |
| FFmpeg (libavcodec, libavformat, libavfilter, libavutil, libswscale, libswresample) | `libavcodec63` etc., 9.0 | LGPL-2.1-or-later; Debian builds it with `--enable-gpl`, which makes it GPL-2.0-or-later | Shared libraries |
| OpenCV (core, imgproc, video, calib3d; contrib: tracking) | `libopencv-*410`, 4.10 | Apache-2.0 | Shared libraries |
| ONNX Runtime | `libonnxruntime1.23` | MIT | Loaded with `dlopen()` when a voice mode is first used |
| Vulkan loader | `libvulkan1` | Apache-2.0 | Shared library |
| GNU OpenMP runtime (`libgomp`), C++ runtime (`libstdc++`, `libgcc_s`) | `libgomp1`, `libstdc++6`, `libgcc-s1` | GPL-3.0 with the GCC Runtime Library Exception | Shared libraries (`libgomp` through ncnn) |
| GNU C library | `libc6` | LGPL-2.1-or-later | Shared library |
| NVIDIA driver (Vulkan driver, video decode, NVENC) | none needed -- the user's driver | NVIDIA's proprietary license | Loaded at run time through the Vulkan loader and FFmpeg; `libnvidia-encode.so.1` is `dlopen()`ed to check that NVENC is there |

All of these licenses are compatible with distributing orbis-reframe under
GPL-3.0-or-later.

## Build tools only

Used to build, not part of the program: CMake, glslang (`glslangValidator`
compiles the shaders, whose source is this project's own), Qt's build tools,
commitizen. Analysis and spike tools under `tools/` aren't installed.

## Not code, but credited

The projects the `.insv` format knowledge came from (telemetry-parser /
Gyroflow, ExifTool, insv-stitch) and the published methods the code
implements (the MEI fisheye camera model, the Mahony complementary filter,
OpenCV's CSRT tracker) are credited in `README.md` ("Sources and thanks").
No code was copied from them: the `src/insv/` parser is written from the
documented layouts (`docs/INSV_FORMAT.md`).

Insta360, Insta360 Studio and X5 are trademarks of Arashi Vision Inc. This
project is not affiliated with or endorsed by Insta360.
