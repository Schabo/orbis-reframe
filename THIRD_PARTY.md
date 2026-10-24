# Third-party components

orbis-reframe is licensed under the GNU General Public License v3.0 or later
(`LICENSE`); the `.insv` library in `src/insv/` under MIT OR Apache-2.0
(`src/insv/LICENSE-MIT`, `src/insv/LICENSE-APACHE`). This file lists what
else goes into the program and its packages, under which license, and where
the license text is.

Paths are given in the source tree. In the packages (`/usr` for the `.deb`,
the unpacked folder for the tarball, `usr/` inside the AppImage) they are in
`share/doc/orbis-reframe/`:
this file, `LICENSE`, `LICENSE.lucide`, and `licenses/` (ncnn, nlohmann/json,
miniaudio, and `src/insv/`'s two as `insv-LICENSE-MIT` /
`insv-LICENSE-APACHE`). The
models' licenses are next to each model, in
`share/orbis-reframe/models/<model>/`.

## Shipped in the packages

| Component | Version | License | What it does here | License text |
|---|---|---|---|---|
| [ncnn](https://github.com/Tencent/ncnn) (Tencent) | 20260526, the `ubuntu-2404-shared` release | BSD-3-Clause (its third-party parts: zlib, BSD-2-Clause, BSD-3-Clause) | Runs the number-plate model on the GPU (plate scan). `libncnn.so.1` is bundled in `lib/orbis-reframe/` -- it isn't packaged in Debian | `packaging/licenses/ncnn-LICENSE.txt` |
| [nlohmann/json](https://github.com/nlohmann/json) (Niels Lohmann) | 3.11.3 (Debian's `nlohmann-json3-dev`) | MIT | Reads and writes project files. Header-only, compiled into the binary | `packaging/licenses/nlohmann-json-LICENSE.MIT` |
| [miniaudio](https://miniaud.io/) (David Reid) | 0.11.25, in `external/miniaudio/` | Unlicense OR MIT-0 | Plays the sound: compiled into the binary, it loads the system's libpulse / ALSA / JACK at run time | `external/miniaudio/LICENSE` |
| [Lucide](https://lucide.dev/) icons (some derived from [Feather](https://feathericons.com/)) | -- | ISC (Feather parts: MIT) | Toolbar and panel icons, compiled into the binary | `resources/icons/LICENSE.lucide` |
| [DeepFilterNet 3](https://github.com/Rikorose/DeepFilterNet) model (Hendrik Schröter and contributors) | repository commit `84d57ec` | MIT OR Apache-2.0 | The "Voice only" / "Voice + ambience" noise modes | `resources/models/deepfilternet3/LICENSE-MIT`, `LICENSE-APACHE` |
| [VitTrack](https://github.com/opencv/opencv_zoo) model (OpenCV Zoo) | `object_tracking_vittrack_2023sep` | Apache-2.0 | Checks that subject tracking still follows the same subject | `resources/models/vittrack/LICENSE-APACHE` |
| YOLOv9-s licence-plate model from [open-image-models](https://github.com/ankandrew/open-image-models) (ankandrew) | `yolo-v9-s-608-license-plates-end2end`, converted for ncnn | MIT | Finds number plates for the privacy blur | `resources/models/plates/LICENSE-MIT` |

Each model folder also has a `README.md` with its source, checksums and any
changes made to it.

## Bundled in the AppImage

The AppImage (`orbis-reframe-<version>-x86_64.AppImage`) contains what the
packages above ship and also the libraries the `.deb`s and the tarball take
from the system: Qt, FFmpeg, OpenCV, ONNX Runtime and what they need, from
Debian 13's packages or our own builds. Only glibc, `libstdc++`, the Vulkan
loader, the graphics and VA-API drivers, GL / EGL, fontconfig, freetype,
X11 / xcb, `libwayland-client` and the sound server's client library come
from the system it runs on.

Inside the AppImage, in `usr/share/doc/orbis-reframe/`:

- `bundled.txt` lists every bundled file with the package and the exact
  version it comes from;
- `licenses/<component>/` holds each component's license texts (for
  Debian's packages, the package's `copyright` file);
- `licenses/appimage-sources.txt` says where the source of each copyleft
  component is. Each release has the same list as
  `orbis-reframe-<version>-appimage-sources.txt`: Debian's packages are
  linked by their exact version on snapshot.debian.org, and the source of
  our FFmpeg build and of the runtime is on the release.

Every component's license was reviewed in `packaging/appimage/licenses.tsv`,
which says why each one can be shipped inside a GPL-3.0-or-later program;
the build fails on a bundled file that review doesn't cover. Unlike
Debian's, the AppImage's FFmpeg is LGPL-2.1-or-later (built without
`--enable-gpl`). To run the program with a changed version of an LGPL
library, unpack the AppImage (`--appimage-extract`), replace the library in
`squashfs-root/usr/lib/` and start `squashfs-root/AppRun`.

<!-- Generated from packaging/appimage/licenses.tsv by scripts/appimage_third_party.sh --update; the AppImage build checks it. -->

| Component | From | Version | License | Source |
|---|---|---|---|---|
| abseil | Debian 13 | 20240722.0 | Apache-2.0 | -- |
| brotli | Debian 13 | 1.1.0 | MIT | -- |
| bzip2 | Debian 13 | 1.0.8 | bzip2-1.0.6 | -- |
| cpuinfo | Debian 13 | 0.0~git20250327.39ea79a | BSD-2-Clause | -- |
| dbus | Debian 13 | 1.16.2 | GPL-2.0-or-later OR AFL-2.1 | snapshot.debian.org |
| double-conversion | Debian 13 | 3.3.1 | BSD-3-Clause | -- |
| gcc-14 | Debian 13 | 14.2.0 | GPL-3.0-or-later WITH GCC-exception-3.1 | snapshot.debian.org |
| glib2.0 | Debian 13 | 2.84.4 | LGPL-2.1-or-later | snapshot.debian.org |
| graphite2 | Debian 13 | 1.3.14 | LGPL-2.1-or-later OR MPL-1.1 OR GPL-2.0-or-later | snapshot.debian.org |
| icu | Debian 13 | 76.1 | Unicode-3.0 | -- |
| lapack | Debian 13 | 3.12.1 | BSD-3-Clause | -- |
| libb2 | Debian 13 | 0.98.1 | CC0-1.0 | -- |
| libcap2 | Debian 13 | 2.75 | BSD-3-Clause OR GPL-2.0-only | -- |
| libffi | Debian 13 | 3.4.8 | MIT | -- |
| libglvnd | Debian 13 | 1.7.0 | MIT | -- |
| libpng1.6 | Debian 13 | 1.6.48 | libpng-2.0 | -- |
| libva | Debian 13 | 2.22.0 | MIT | -- |
| libxau | Debian 13 | 1.0.11 | MIT-open-group | -- |
| libxdmcp | Debian 13 | 1.1.5 | MIT-open-group | -- |
| libxkbcommon | Debian 13 | 1.7.0 | MIT AND HPND | -- |
| libzstd | Debian 13 | 1.5.7+dfsg | BSD-3-Clause OR GPL-2.0-only | -- |
| md4c | Debian 13 | 0.5.2 | MIT | -- |
| onetbb | Debian 13 | 2022.1.0 | Apache-2.0 | -- |
| onnx | Debian 13 | 1.17.0 | MIT AND Apache-2.0 | -- |
| onnxruntime | Debian 13 | 1.21.0+dfsg | MIT AND Apache-2.0 AND MPL-2.0 | -- |
| opencv | Debian 13 | 4.10.0+dfsg | Apache-2.0 AND BSD-3-Clause AND BSD-2-Clause | -- |
| pcre2 | Debian 13 | 10.46 | BSD-3-Clause WITH PCRE2-exception | -- |
| protobuf | Debian 13 | 3.21.12 | BSD-3-Clause | -- |
| pthreadpool | Debian 13 | 0.0~git20240616.560c60d | BSD-2-Clause | -- |
| qt6-base | Debian 13 | 6.8.2+dfsg | LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only | snapshot.debian.org |
| qt6-svg | Debian 13 | 6.8.2 | LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only | snapshot.debian.org |
| qt6-wayland | Debian 13 | 6.8.2 | LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only | snapshot.debian.org |
| re2 | Debian 13 | 20240702 | BSD-3-Clause | -- |
| systemd | Debian 13 | 257.13 | LGPL-2.1-or-later | snapshot.debian.org |
| wayland | Debian 13 | 1.23.1 | MIT | -- |
| xnnpack | Debian 13 | 0.0~git20241108.4ea82e5 | BSD-3-Clause | -- |
| ffmpeg | our build (`scripts/build_ffmpeg.sh`), configure line in `licenses/ffmpeg/configure.txt` | 9.0.2 | LGPL-2.1-or-later | on the release |
| ncnn | Tencent's release | 20260526 | BSD-3-Clause AND Zlib AND BSD-2-Clause | -- |
| type2-runtime | our build (`scripts/build_appimage_runtime.sh`), the AppImage's runtime | fedfad5 | MIT AND LGPL-2.1-only AND BSD-2-Clause AND Zlib AND BSD-3-Clause | on the release |

<!-- End of the generated table. -->

## Used from the system, not shipped

These come from the distribution's own packages (the `.deb` depends on them;
the tarball's install guide lists them). Their license texts are installed
with them, in `/usr/share/doc/<package>/copyright`.

| Component | Debian package (Debian Testing) | License | Linked how |
|---|---|---|---|
| Qt 6 (Core, Gui, Widgets; Wayland and SVG plugins) | `libqt6core6t64` etc., 6.10 | LGPL-3.0 (also GPL-2.0 / GPL-3.0) | Shared libraries; the Wayland and SVG plugins loaded by Qt |
| FFmpeg (libavcodec, libavformat, libavfilter, libavutil, libswscale, libswresample) | `libavcodec63` etc., 9.0 | LGPL-2.1-or-later; Debian builds it with `--enable-gpl`, which makes it GPL-2.0-or-later | Shared libraries |
| OpenCV (core, imgproc, video, calib3d; contrib: tracking) | `libopencv-*410`, 4.10 | Apache-2.0 | Shared libraries |
| ONNX Runtime | `libonnxruntime1.23` | MIT | Loaded with `dlopen()` when a voice mode is first used |
| Vulkan loader | `libvulkan1` | Apache-2.0 | Shared library |
| PulseAudio client library | `libpulse0` (recommended, not required) | LGPL-2.1-or-later | Loaded with `dlopen()` by miniaudio to play sound (PipeWire's `pipewire-pulse` or PulseAudio); ALSA's `libasound2` the same way when there's no sound server |
| zlib | `zlib1g` | Zlib | Shared library (unpacking zipped LUTs) |
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
documented layouts (`docs/formats/insta360-x5.md`).

Insta360, Insta360 Studio and X5 are trademarks of Arashi Vision Inc. This
project is not affiliated with or endorsed by Insta360.
