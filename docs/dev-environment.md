# Dev Environment

Pinned versions for the reference dev machine (Debian Testing, NVIDIA, Sway).
Update this file if the toolchain moves.

- **OS**: Debian Testing, x86_64
- **Compositor**: Sway (wlroots), Wayland-only session. GNOME/Mutter available as a secondary test target. No X11/XWayland.
- **NVIDIA driver**: 595.99.02 (was 595.84 when this was first written)
- **GPU**: NVIDIA GeForce RTX 5070
- **CMake**: 4.3.4
- **Compiler**: g++ 15.3.0 (Debian 15.3.0-2)
- **Qt**: 6.11.2 (6.10.2 until 2026-10-11; apt: `qt6-base-dev`, `qt6-base-dev-tools`, `qt6-wayland`; `qt6-svg-plugins` draws the icons at run time, and `theme_test` needs it)
- **Vulkan**: loader/instance 1.4.341 (apt: `libvulkan-dev`, `vulkan-tools`)
- **Shader compiler**: `glslangValidator` (apt: `glslang-tools`)
- **Other libs**: `nlohmann-json3-dev`, `zlib1g-dev` (unpacking the zips LUTs come in)
- **FFmpeg dev** (demux/decode/encode for the stitch layer, export and tools): `libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libavfilter-dev` (libavfilter since 2026-09-27: `atempo` audio time-stretch for clips at speed ≠ 1)
- **OpenCV** 4.10 (*optional* — the per-clip gyro sync fine-tune, the plate scan, `tools/residual_stab`; subject tracking also needs the contrib `tracking` + `dnn` modules, `libopencv-contrib-dev`): `libopencv-dev`. CMake skips what needs it with a `STATUS` message when absent; the app still builds. OpenCV 5 builds too (Arch's 5.0.0, checked 2026-10-18): it moved `goodFeaturesToTrack` into the `features` module and `findHomography` / `estimateAffinePartial2D` from `calib3d` into `geometry`, and `CMakeLists.txt` links those by version.
- **ncnn** 20260526 (*optional* — the privacy blur's plate scan, "Scan clip" in the Privacy tab, and `tools/plate_scan`): not in Debian. `scripts/fetch_ncnn.sh` downloads the pinned release `ncnn-20260526-ubuntu-2404-shared.zip` (<https://github.com/Tencent/ncnn/releases>; Vulkan built in, "simplevk", no `libvulkan` link; needs only `libgomp1` besides libc/libstdc++), checks its SHA-256 and unpacks it into `third_party/ncnn/` (gitignored), where CMake finds it on its own; `-DNCNN_ROOT=<folder>` (`NCNN_ROOT=<folder> scripts/build.sh`) points at another copy. `cmake --install` ships `libncnn.so.1` in `lib/orbis-reframe/` (the binary's `INSTALL_RPATH` is `$ORIGIN/../lib/orbis-reframe`). Without it CMake prints a `STATUS` line; the app can't scan then, but still applies scans it has. After switching copies, clear the cached one: `cmake -U ncnn_DIR -U NCNN_ROOT build`.
- **ONNX Runtime** 1.23 (*optional* — the "Voice only" / "Voice + ambience" noise modes, DeepFilterNet 3): `libonnxruntime-dev`. Only its headers are needed to build; `insv::VoiceIsolator` `dlopen()`s `libonnxruntime.so.1.<ORT_API_VERSION>` (the headers' API version is the 1.x minor version: `.so.1.23` here, `.so.1.21` on Debian 13) on first use; CMake turns the same number into the `.deb`'s `libonnxruntime1.23` and the "install it" hint. CMake hides the voice modes with a `STATUS` message when the headers are absent. Without root, `apt-get download libonnxruntime-dev libonnxruntime1.23` + `dpkg -x` into a folder and configure with `-DONNXRUNTIME_ROOT=<folder>/usr` (how it was developed, 2026-09-30).
- **miniaudio** 0.11.25 (sound output, `src/ui/AudioOutput`): in the repository, `external/miniaudio/miniaudio.h` + its `LICENSE` (Unlicense OR MIT-0), copied unchanged from the release tarball `miniaudio-0.11.25.tar.gz` (<https://github.com/mackron/miniaudio/releases/tag/0.11.25>, SHA-256 `b900edcffe979816e2560a0580b9b1216d674b4f17fbadeca8f777a7f8ab0274`; `miniaudio.h` itself `ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89`). In the tree rather than fetched so package and offline builds need nothing extra. No dev package: miniaudio `dlopen()`s libpulse (PipeWire's `pipewire-pulse` or PulseAudio), ALSA or JACK at run time. `AudioOutput` reads PulseAudio's timing info from miniaudio's stream (`ma_device::pulse.pStreamPlayback`), an internal member: a new miniaudio version means checking that still holds and measuring A/V sync again (`audio_output_test`, and by ear).

The full list of build packages is written out again in
`.github/workflows/ci.yml`, `.github/workflows/release.yml`,
`scripts/package.sh` and `tests/release/dev_test.sh`: a new build dependency
goes into all of them, or CI and the release builds fail.

## Dependency management decision

Use **Debian Testing's own apt packages**,
not vcpkg or Conan. The project already targets Debian Testing as the pinned
platform, so there's no cross-platform reproducibility need vcpkg/Conan would
buy — and building Qt6 from source via vcpkg would cost hours for no benefit.
Revisit only if a dependency isn't reasonably packaged in Debian.

## First-time setup

Everything, including the optional features (what a release build needs;
`scripts/release_test.sh`'s dev test installs exactly this list in a bare
Debian Testing container and builds there):

```sh
sudo apt install g++ cmake ninja-build pkgconf \
    qt6-base-dev qt6-base-dev-tools qt6-wayland qt6-svg-plugins \
    libvulkan-dev glslang-tools nlohmann-json3-dev zlib1g-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libavfilter-dev \
    libopencv-dev libopencv-contrib-dev libonnxruntime-dev \
    curl unzip
scripts/fetch_ncnn.sh
```

(`curl unzip` are for `fetch_ncnn.sh`. `vulkan-tools` gives `vulkaninfo`,
handy but not needed to build.)

## Commits, versions and releases

Commit messages follow Conventional Commits (`CONTRIBUTING.md`, "Commit
messages").
Turn on the check once per clone -- it runs `cz check` (commitizen, `sudo apt
install commitizen`) on every commit message:

```sh
git config core.hooksPath .githooks
```

The version lives in `.cz.toml` and in `project(orbis-reframe VERSION ...)`;
don't edit either by hand. A build shows `0.1.0` on a release tag and e.g.
`0.1.0+12.gabc1234[.dirty]` after it (`orbis-reframe --version`, Help ▸ About;
`cmake/GitVersion.cmake`, regenerated on every build). To release:

```sh
cz bump                 # next version from the feat/fix commits since the last tag:
                        # updates .cz.toml, CMakeLists.txt, CHANGELOG.md, commits, tags vX.Y.Z
git push --follow-tags  # (once the GitHub repository exists)
```

The first release is `cz bump --increment MINOR` (0.0.0 -> 0.1.0); while the
major version is 0, a breaking change bumps the minor (`major_version_zero`).
Tags are annotated (`annotated_tag`), so `--follow-tags` pushes them. The
whole release, from bump to publishing, is in `docs/RELEASE.md`.

## Installed layout

`cmake --install build --prefix <dir>` gives the
layout packages use: `bin/orbis-reframe`, `lib/orbis-reframe/libncnn.so.1`,
`share/orbis-reframe/models/`, `share/doc/orbis-reframe/` (licenses, the
manual), `share/applications/orbis-reframe.desktop`,
`share/mime/packages/orbis-reframe.xml` (the `.orp` and `.insv` file types,
from `packaging/orbis-reframe-mime.xml`) and
`share/icons/hicolor/scalable/apps/orbis-reframe.svg` (`packaging/`). Shaders are compiled into the binary
(`render_shaders`); models are found from the binary's own location
(`src/core/DataPaths`), falling back to the source tree in dev builds
(`-DORBIS_SOURCE_TREE_FALLBACK=OFF` for packaged builds). The `ORBIS_DFN_MODEL_DIR`,
`ORBIS_TRACK_MODEL_DIR` and `ORBIS_PLATE_MODEL` environment variables
override.

## Release packages

`scripts/package.sh` builds what a release ships for one target
(`scripts/targets.sh`: `debian-testing`, `debian-13`, `ubuntu-26.04`), into `dist/<target>/`:

```sh
./scripts/package.sh                       # this machine's target; needs a clean tree (--allow-dirty to test)
./scripts/package.sh --target debian-13    # Debian 13, built in a debian:trixie podman container
./scripts/package.sh --target ubuntu-26.04 # Ubuntu 26.04 LTS, in an ubuntu:26.04 container
```

A target other than this machine's is built in a podman container of that
release (`target_image` in `scripts/targets.sh`), which installs the build
dependencies below plus `git dpkg-dev file desktop-file-utils shared-mime-info` and runs
`package.sh` again inside, with the source tree mounted at the same path
(downloaded `.deb`s are kept in `tmp/package-apt-cache/<target>/`). The same
source builds on all three: Debian 13 has Qt 6.8, FFmpeg 7.1, ONNX Runtime 1.21
and the same OpenCV 4.10; Ubuntu 26.04 has Qt 6.10.2, FFmpeg 8.0, ONNX
Runtime 1.23 and OpenCV 4.10, so it's Testing with an older FFmpeg. Two
version guards: `AVVulkanDeviceContext::queue_flags` is read only from
libavutil 60.32.100 (FFmpeg 9; older FFmpeg creates its queues without
flags), and OpenCV 5's module names (above). ncnn's
Ubuntu 24.04 build (glibc 2.39) runs on all three. Ubuntu 24.04 is too old
to build (Qt 6.4, FFmpeg 6.1 with the old `AVVulkanDeviceContext` queue
fields, OpenCV 4.6 without `TrackerVit`, Vulkan headers without AV1 decode,
no ONNX Runtime package). Two Qt 6.8 quirks the app works around, both found by the
release test's GUI run (the dev test passes without them):

- Qt 6.8's Wayland plugin loads the Vulkan loader as `libvulkan.so`, which
  only `libvulkan-dev` installs, so an end-user system "has no Vulkan";
  `main.cpp` sets `QT_VULKAN_LIB=libvulkan.so.1` unless it's set already.
- Closing a window hides it before `QVulkanWindow` releases its swapchain,
  and the NVIDIA driver crashes in `vkDestroySwapchainKHR` on quit;
  `MainWindow::closeEvent` sends the viewport `SurfaceAboutToBeDestroyed`
  first, while it's still shown.

- a clean Release build in `build-package/<target>/` with every feature
  (`-DORBIS_REQUIRE_ALL_FEATURES=ON` fails the configure if a dependency is
  missing -- run `scripts/fetch_ncnn.sh` first),
  `-DORBIS_SOURCE_TREE_FALLBACK=OFF` and `-ffile-prefix-map`, then `ctest`;
- CPack (`cmake/Packaging.cmake`): `orbis-reframe-<version>-<target>-x86_64.tar.xz`
  (with a top-level `orbis-reframe` launcher, `packaging/tarball/`) and
  `orbis-reframe_<version>_amd64.deb` -- for Debian 13 the package version is
  `<version>~deb13`, a backport-style version that sorts below Testing's (and
  `~ubuntu26.04` for Ubuntu); `scripts/package.sh` names the file
  `orbis-reframe_<version>.deb13_amd64.deb`, since GitHub turns `~` into `.`
  (`Depends` from `dpkg-shlibdeps` plus `qt6-wayland`, `qt6-svg-plugins` and
  the ONNX Runtime built against, `libonnxruntime1.23` / `1.21`; `Suggests`: Mesa's
  Vulkan and VA-API drivers for AMD / Intel, and Debian's NVIDIA driver packages
  except on Ubuntu, whose names carry the driver version);
- `orbis-reframe-<version>-source.tar.xz` (`git archive` of HEAD) and a
  `.sha256` for each file;
- checks on the unpacked tarball: launcher, `--version`, `ldd`, bundled
  ncnn, no source-tree / `$HOME` strings, models, `desktop-file-validate`.

`scripts/release_test.sh [--target T]` then tests `dist/<target>/` in podman
containers of that release on this machine's GPU (always with this
machine's NVIDIA driver, not the release's own packages -- Debian 13's 550
driver isn't tested and doesn't support RTX 50-series cards): clean installs, a GUI test per package,
a build of the source archive with ctest and the GPU tools
(`docs/RELEASE.md` step 6). Setup, once: `sudo apt install podman`, the
NVIDIA Container Toolkit from NVIDIA's apt repository
(<https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html>),
then `sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml` (again
after each driver update). A container sees the GPU with
`podman run --device nvidia.com/gpu=all -e NVIDIA_DRIVER_CAPABILITIES=graphics,video,compute,utility`;
a bare image also needs `libegl1` and `libxext6`, which NVIDIA's Vulkan
driver loads (the app's packages pull them in through Qt). The GUI test's
image is `tests/release/Containerfile`; it runs the smoke test below on the
installed package.

The GPU workflow (`.github/workflows/gpu.yml`) and the release test's GUI
test run a smoke test of the whole app: `tests/release/gpu_smoke.sh build
OUT_DIR` starts the dev build on a private headless Sway (needs `sway` and
`ffmpeg`), drives it through `tests/release/gpu_smoke.steps` -- play, Track
subject, Scan clip, export -- and checks the viewport's picture, the frames,
the tracked path, the scan and the exported file; about a minute, also
locally. `--sound` adds PipeWire with a null sink and checks the sound
and the voice model (the release test uses it). The GPU runner's jobs can't
open the GPU's render node, so there the viewport doesn't draw and its
checks are left out.

For a release, the release workflow (`.github/workflows/release.yml`) runs
`package.sh` per target on GitHub's runners when the tag is pushed and makes
a draft release; `scripts/fetch_release.sh vX.Y.Z` downloads the draft's
files into `dist/<target>/` for `release_test.sh`.

`<version>` is the binary's (`cmake/GitVersion.cmake`). The `.deb`'s
Maintainer is `CPACK_PACKAGE_CONTACT` in `cmake/Packaging.cmake`
(`ORBIS_MAINTAINER="Name <email>"` overrides it for one run).
Don't use Qt's private-ABI classes (`QVulkanDeviceFunctions` and friends,
`objdump -T ... | grep Qt_6_PRIVATE_API`): the `.deb` then depends on
`qt6-base-private-abi (= <exact Qt version>)`.

### Our own FFmpeg

The `.deb`s and tarballs link the system's FFmpeg. The planned AppImage
bundles our own build instead, the same everywhere:
`scripts/build_ffmpeg.sh` builds the pinned release (9.0.2) into
`third_party/ffmpeg/<version>-<os>/usr/` (gitignored; `<os>` is
`/etc/os-release`'s `ID-VERSION_CODENAME`, e.g. `debian-trixie`) and reuses
a finished build. `--print-dir` prints that folder. Point CMake at it with
`PKG_CONFIG_PATH=<folder>/usr/lib/pkgconfig`; the libraries have no RUNPATH,
so running against them needs `LD_LIBRARY_PATH=<folder>/usr/lib`.

- **Checked**: the tarball against its pinned SHA-256 and the signature
  against FFmpeg's release key, kept in `scripts/ffmpeg-release-key.asc`
  (fingerprint `FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8`, as on
  <https://ffmpeg.org/download.html>). The verified tarball stays in
  `third_party/ffmpeg/`: it's the source hosted next to a release that
  bundles the build.
- **Configured** LGPL only (no `--enable-gpl`, never `--enable-nonfree`;
  the script stops unless configure reports "LGPL version 2.1 or later"),
  shared, `--disable-autodetect` with only Vulkan, VAAPI + libdrm, NVENC,
  zlib, iconv and pthreads enabled; no network protocols
  (`--disable-network`: the app only touches local files) and no
  libavdevice. `--prefix=/usr` keeps build-machine
  paths out of the configure line the libraries embed; the `.pc` files are
  rewritten to the build folder. The licence texts and the configure line
  are in `usr/share/doc/ffmpeg/`.
- **Needs**: `gcc make nasm pkgconf curl gpg xz-utils libvulkan-dev
  libva-dev libdrm-dev libffmpeg-nvenc-dev zlib1g-dev`. The AppImage's
  build runs it in `debian:trixie`, so the libraries need no newer glibc
  than Debian 13's. A new FFmpeg release means a new version and SHA-256
  at the top of the script, checked against the signature.

### The AppImage runtime

The runtime at the front of the AppImage (AppImage/type2-runtime, the part
that mounts the image) links libfuse (LGPL-2.1-only) statically, so its
source is hosted next to the AppImage. `scripts/build_appimage_runtime.sh`
builds it from a pinned commit (`RUNTIME_COMMIT`, now `fedfad5`) with
upstream's own Alpine container, in podman, and fails unless the result is
byte-identical to upstream's release binary for that commit
(`RUNTIME_SHA256`); the source archive is then exactly what's in the file.
Output in `third_party/appimage-runtime/<commit>/`: `runtime-x86_64` and
`type2-runtime-<commit>-source.tar.xz` (the runtime's tree, the libfuse and
squashfuse tarballs its build downloads, the container's Alpine package
list, and how to rebuild). Reused when done; about 30 s otherwise.

- **Needs**: podman, network access and `/dev/fuse` on the host (libfuse's
  install step would `mknod` it, which a rootless build can't). It runs on
  the host, not inside the AppImage's `debian:trixie` build container.
- **When it fails** on an unchanged commit, Alpine 3.21 has updated a
  package the runtime links (zstd, zlib, mimalloc, musl; the error lists the
  build's versions). Upstream's next release is built with them: move to
  it as below.
- **A new runtime**: set `RUNTIME_COMMIT` and `RUNTIME_SHA256` (that
  release's `runtime-x86_64`), and review its row in
  `packaging/appimage/licenses.tsv`.

## Build & run

```sh
./scripts/build.sh          # configure + build
./scripts/build.sh -r       # build and run
./scripts/build.sh -c -r    # clean build dir, rebuild, run
```

Running forces the Wayland QPA plugin (`QT_QPA_PLATFORM=wayland` set in
`main.cpp`) — no X11/XWayland fallback. Should show a window with a
Vulkan-rendered triangle. This is the toolchain-scaffold check, not the
real renderer.

`scripts/build.sh` wraps the plain `cmake -B build && cmake --build build`
invocation — keep it in sync whenever the build gains new flags, targets, or
steps (e.g. running tests, packaging).

## Environment switches

The app reads a few environment variables, for diagnosing problems and for
comparing code paths. None is needed for normal use. A switch is on when
set to anything except empty or `0`, unless the table says otherwise.

| Variable | Effect |
|---|---|
| `ORBIS_VERBOSE=1` | Progress messages on stderr: which decode / stitch path a clip got, sync and seam measurements and their results. Warnings and fallbacks print without it. The first thing to ask for in a bug report. |
| `ORBIS_FRAME_STATS=1` | About once a second, the viewport and the playback presenter each print a one-line timing summary to stderr. For checking smooth playback. |
| `INSV_NO_GPU_DECODE` | Decode video on the CPU (FFmpeg) instead of with Vulkan video. Set to any value, even empty. |
| `INSV_NO_GPU_STITCH` | Stitch on the CPU too (implies CPU decode). Slow; the reference path. Set to any value. |
| `INSV_PLAYBACK_EQUIRECT` | Playback renders the view from a stitched equirect instead of straight from the lenses. Set to any value. |
| `INSV_FLAT_VIA_EQUIRECT` | Export reprojects through an equirect instead of the direct fisheye -> view pass. For comparisons. Set to any value. |
| `INSV_VK_VALIDATE` | Turn on the Vulkan validation layer (`VK_LAYER_KHRONOS_validation`, package `vulkan-validationlayers`) for the stitcher. Set to any value. |
| `ORBIS_DFN_MODEL_DIR=<dir>` | Load the DeepFilterNet voice model from `<dir>` instead of the installed one. |
| `ORBIS_PLATE_MODEL=<file.param>` | Use another ncnn plate model (`.param`, with its `.bin` next to it). |
| `ORBIS_TRACK_MODEL_DIR=<dir>` | Load the subject-tracking model (`vittrack.onnx`) from `<dir>`. |
| `ORBIS_SMOKE_TEST=<steps file>` | Run the smoke test's steps against the app and print what it measured as `[smoke]` lines on stdout (`src/ui/SmokeTest.h`; `tests/release/gpu_smoke.sh` sets it). Not for normal use: the app drives itself and quits. |

`plate_scan` has a few switches of its own (`FOLLOW_*`, `BACK_CHUNK`); its
`--help` lists them.
