# Getting started

This page covers what your computer needs, which packages to install, and how
to start orbis-reframe and open your first clip.

orbis-reframe is a video editor for 360° camera footage. It opens Insta360
`.insv` files directly, and the **Insta360 X5** is the camera it's built and
tested on. You don't need Insta360 Studio or any other Insta360 software.

The **Insta360 X6** is supported experimentally: its clips open, stitch and
export, including the 10-bit I-Log and Dolby Vision modes (with a LUT, see
[Colour](colour.md#luts)), but so far only a contributor has tried it on real
X6 files. [Stabilization](stabilization.md) may turn the picture the wrong
way on X6 clips: it reads the camera's motion the X5's way until an X6 clip
confirms it. If something looks wrong on your X6 clips, please
[open an issue](https://github.com/Schabo/orbis-reframe/issues).

The **Insta360 ONE X2** is supported experimentally, in its 5.7K mode. Each
clip is three files: one per lens (`VID_…_00_…` and `VID_…_10_…`) and the
camera's preview file (`LRV_…_11_…`). Add any of them, or the whole folder,
and the clip comes in once; it opens, stitches, [stabilizes](stabilization.md)
and exports. Keep a clip's files together in one folder. The stitch hasn't
been compared with Insta360 Studio's yet, and on some clips
[Horizon lock](stabilization.md#horizon-lock) leaves the horizon tilted by
a few degrees or more. The ONE X2's 4K and 3K modes (one file for both
lenses) aren't supported yet.

The **Antigravity A1**, a 360° drone, is supported experimentally in its 8K
mode, from one contributor's flights: its clips (`VID_….insv`, with the
`PRX_….prx` preview file beside them) open, stitch,
[stabilize](stabilization.md#on-the-antigravity-a1) and export, the picture
facing the drone's nose and level by its flight controller. It records no
sound. Titles and gauges show its GPS once the drone has moved: the clip
doesn't say when its GPS track starts, so orbis-reframe lines it up by
matching the GPS speed against the drone's own, which a hover or a clip
shorter than about 8 seconds doesn't allow. The 5.2K and 4K modes and H.264 clips haven't been
tried.

Other cameras (other Insta360 models, DJI, GoPro) aren't supported yet. If
you have one, short clips from it help add it:
[sample clips wanted](../formats/README.md#sample-clips-wanted).

> **Early software.** orbis-reframe has been built and tested on one computer
> so far (Debian Testing, an NVIDIA RTX 5070, the Sway desktop; the Debian 13
> and Ubuntu 26.04 builds in containers of those systems on that computer,
> the AppImage in containers of Debian 13, Ubuntu 26.04, Fedora 44, Arch and
> openSUSE Tumbleweed).
> Setups users have confirmed are listed under
> [Confirmed working](../../README.md#confirmed-working) in the README.
> Others that meet the requirements below should work but haven't been
> tried.

## 1. Check your computer

| | Needed |
|---|---|
| **Operating system** | Linux, 64-bit (x86_64). On **Debian 13** ("trixie"), **Debian Testing** and **Ubuntu 26.04 LTS** there's a package for each, built on the library versions of its own release (Debian 13: Qt 6.8, FFmpeg 7.1; Testing: Qt 6.10, FFmpeg 9; Ubuntu 26.04: Qt 6.10, FFmpeg 8); download the one for your system. None of them starts on older releases, such as Ubuntu 24.04, whose versions differ. To see which system you have: `grep -E '^(ID\|VERSION_ID)=' /etc/os-release` prints `ID=debian` and `VERSION_ID="13"` on Debian 13, `ID=debian` and no `VERSION_ID` on Testing, and `ID=ubuntu` and `VERSION_ID="26.04"` on Ubuntu 26.04. **Other distributions** (Fedora, Arch, openSUSE Tumbleweed, ...): the **AppImage**, which brings its own libraries and runs where the system's C library is version 2.41 or newer (`ldd --version` prints it on its first line). |
| **Desktop session** | **Wayland**: Sway, GNOME, KDE Plasma or another Wayland desktop. An X11 session doesn't work: orbis-reframe exits at startup instead of falling back to X11. |
| **Graphics card** | A card with **Vulkan**, including Vulkan video decoding. Export uses the card's hardware encoder: NVENC on NVIDIA, VAAPI or Vulkan video on AMD and Intel. **NVIDIA** with the proprietary driver is what orbis-reframe is tested on (an RTX 5070, driver 595); other NVIDIA cards from the GTX 16 / RTX 20 series onwards should work. **AMD** and **Intel** use the Mesa drivers that come with the system, and are supported best-effort: a contributor has tested playback and export on an AMD Radeon 8060S (Mesa 26.2); other AMD cards and Intel GPUs haven't been tried. If something doesn't work on yours, please [report it](#if-it-doesnt-start). |
| **Graphics memory** | **8 GB** or more on the graphics card for projects of 8K clips (Insta360 X5); 12 GB is better. Each 8K clip that's open needs about 1 GB, and orbis-reframe keeps up to four clips open while you play across a project, plus about 1 GB more while exporting. A project of a single 8K clip runs on 4 GB. The desktop and other programs, such as a web browser, also use graphics memory. With too little, the viewport may not show the picture, or clips decode on the processor and play slowly; orbis-reframe says which (see [If it doesn't start](#if-it-doesnt-start)). |
| **Memory** | 16 GB RAM or more. 8K clips are large. |
| **Sound** | PipeWire (with `pipewire-pulse`) or PulseAudio. This is the default on current desktops. |

To check your session and graphics card, run these in a terminal:

```sh
echo $XDG_SESSION_TYPE          # should print: wayland
vulkaninfo --summary | grep deviceName   # should name your graphics card
vulkaninfo | grep video_decode_h265      # should print VK_KHR_video_decode_h265
vulkaninfo | grep -B4 HEAP_DEVICE_LOCAL | grep size   # each device's memory, in the order above
```

`vulkaninfo` comes from the `vulkan-tools` package. If the second command
names only `llvmpipe`, Vulkan runs on the processor: the driver for the
card is missing (step 2).

## 2. Install the graphics driver

If `vulkaninfo` above showed your card, the driver is installed. With an
NVIDIA card, check that the export encoder is installed too
(`libnvidia-encode1`, below), then go on to step 3. With AMD or Intel, see
[AMD and Intel](#amd-and-intel).

The commands below are for Debian and Ubuntu. On other distributions,
install the driver as your distribution's documentation says; orbis-reframe
needs the same parts: the Vulkan driver, and for export NVIDIA's encoder
library (NVENC) or Mesa's VA-API driver.

### NVIDIA

On Debian, the driver is in the `non-free` section. Enable `contrib`,
`non-free` and `non-free-firmware` in your apt sources (see the Debian wiki's
[NvidiaGraphicsDrivers](https://wiki.debian.org/NvidiaGraphicsDrivers) page),
then:

```sh
sudo apt update
sudo apt install nvidia-driver nvidia-vulkan-icd libnvidia-encode1
```

- `nvidia-driver` is the driver itself.
- `nvidia-vulkan-icd` lets Vulkan programs use the card. Without it,
  orbis-reframe can't show video.
- `libnvidia-encode1` is the hardware encoder (NVENC) used by
  **File ▸ Export flat video…**. Without it, export fails.

Restart the computer after installing the driver.

If you installed the driver from NVIDIA's website instead, its installer
already includes Vulkan and NVENC.

**Debian 13 and RTX 50-series cards:** Debian 13's driver packages are
version 550 (also in `trixie-backports`), which doesn't support RTX 50-series
cards (RTX 5060, 5070, 5080, 5090). For those, install a newer driver from
NVIDIA instead, either their installer or their apt repository for Debian.
Older cards work with Debian's 550 driver.

**Ubuntu 26.04:** open **Software & Updates ▸ Additional Drivers** and choose
the newest *NVIDIA driver metapackage* marked *proprietary*, or run:

```sh
sudo ubuntu-drivers install
```

Ubuntu's `nvidia-driver-<version>` packages already include Vulkan and the
hardware encoder (NVENC). Restart the computer afterwards.

### AMD and Intel

AMD and Intel cards use the open-source Mesa drivers, which are part of the
system. Make sure the Vulkan and video (VA-API) drivers are installed:

```sh
sudo apt install mesa-vulkan-drivers mesa-va-drivers vainfo vulkan-tools
```

- `mesa-vulkan-drivers` lets Vulkan programs use the card (RADV for AMD,
  ANV for Intel). Without it, orbis-reframe can't show video.
- `mesa-va-drivers` is AMD's hardware encoder for **File ▸ Export flat
  video…** (VA-API). On Ubuntu 26.04 it's part of `mesa-libgallium`, which
  apt picks for you.
- On **Intel**, VA-API needs Intel's own driver instead:
  `sudo apt install intel-media-va-driver`.

To check that the card can encode, run `vainfo`: it should list
`VAEntrypointEncSlice` lines for `VAProfileHEVCMain` or `VAProfileH264…`.
If it doesn't, orbis-reframe tries Vulkan video encoding instead, which
newer Mesa versions have on some cards.

Vulkan video decoding is fairly new in Mesa, so the newer the system's Mesa,
the better: Debian 13 has Mesa 25.0, Ubuntu 26.04 Mesa 26.0, and Debian
Testing the newest. Without it orbis-reframe still runs, but decodes on the
processor (see [If it doesn't start](#if-it-doesnt-start)).

## 3. Install orbis-reframe

Download orbis-reframe from the [releases
page](https://github.com/Schabo/orbis-reframe/releases). There's a build for
Debian 13, one for Debian Testing and one for Ubuntu 26.04 (step 1), and each
comes in two forms.
Both forms contain the same program and use the system's own Qt, FFmpeg,
Vulkan and OpenCV libraries. For other distributions there's the AppImage
([option C](#option-c-the-appimage)), one file for all of them.

| | Debian 13 | Debian Testing | Ubuntu 26.04 |
|---|---|---|---|
| Debian package | `orbis-reframe_<version>.deb13_amd64.deb` | `orbis-reframe_<version>_amd64.deb` | `orbis-reframe_<version>.ubuntu26.04_amd64.deb` |
| Archive | `orbis-reframe-<version>-debian-13-x86_64.tar.xz` | `orbis-reframe-<version>-debian-testing-x86_64.tar.xz` | `orbis-reframe-<version>-ubuntu-26.04-x86_64.tar.xz` |

- The **`.deb`** is a Debian package. apt installs the libraries it needs,
  it adds orbis-reframe to your desktop's application menu, and your file
  manager then opens `.orp` and `.insv` files with it. Use this
  one unless you can't install packages.
- The **archive** (tarball) unpacks anywhere, without root. You install the
  libraries yourself.

In the commands below, `<target>` is `debian-13`, `debian-testing` or `ubuntu-26.04`, and
`<version>` the version you downloaded.

Each file has a `.sha256` file next to it. To check that the download is
complete, run `sha256sum -c <file>.sha256` in the folder with both files.
It should print `OK`.

For releases after 0.1.0, GitHub also signs each release, so you can check
that a file really comes from it. With the
[GitHub CLI](https://cli.github.com/) installed, run
`gh release verify-asset -R Schabo/orbis-reframe v<version> <file>`.
From 0.2.0 on, the packages and archives were built by GitHub from the
release's source code, and
`gh attestation verify <file> -R Schabo/orbis-reframe` shows which commit
and which build.

### Option A: the `.deb` package

```sh
sudo apt install ./orbis-reframe_<version>.deb13_amd64.deb    # Debian 13
sudo apt install ./orbis-reframe_<version>_amd64.deb          # Debian Testing
sudo apt install ./orbis-reframe_<version>.ubuntu26.04_amd64.deb   # Ubuntu 26.04
```

Keep the `./`: it tells apt to install the file instead of looking for a
package by that name. apt also installs every library orbis-reframe needs.
It doesn't install the graphics driver (step 2): an NVIDIA driver installed
from NVIDIA's website would conflict with Debian's driver packages.

To remove it again: `sudo apt remove orbis-reframe`.

### Option B: the tarball

First install the libraries.

On Debian 13, use this command:

```sh
sudo apt install \
    libqt6core6t64 libqt6gui6 libqt6widgets6 qt6-wayland qt6-svg-plugins \
    libpulse0 \
    libvulkan1 \
    libavcodec61 libavformat61 libavutil59 libavfilter10 libswscale8 libswresample5 \
    libopencv-core410 libopencv-imgproc410 libopencv-video410 \
    libopencv-calib3d410 libopencv-contrib410 \
    libonnxruntime1.21 libgomp1
```

On Debian Testing, use this command:

```sh
sudo apt install \
    libqt6core6t64 libqt6gui6 libqt6widgets6 qt6-wayland qt6-svg-plugins \
    libpulse0 \
    libvulkan1 \
    libavcodec63 libavformat63 libavutil61 libavfilter12 libswscale10 libswresample7 \
    libopencv-core410 libopencv-imgproc410 libopencv-video410 \
    libopencv-calib3d410 libopencv-contrib410 \
    libonnxruntime1.23 libgomp1
```

On Ubuntu 26.04, use this command:

```sh
sudo apt install \
    libqt6core6t64 libqt6gui6 libqt6widgets6 qt6-wayland qt6-svg-plugins \
    libpulse0 \
    libvulkan1 \
    libavcodec62 libavformat62 libavutil60 libavfilter11 libswscale9 libswresample6 \
    libopencv-core410 libopencv-imgproc410 libopencv-video410 \
    libopencv-calib3d410 libopencv-contrib410 \
    libonnxruntime1.23 libgomp1
```

What each group is for:

| Packages | Used for |
|---|---|
| `libqt6core6t64`, `libqt6gui6`, `libqt6widgets6` | The user interface (Qt 6). |
| `qt6-wayland` | Lets Qt open a window on a Wayland desktop. Without it orbis-reframe exits at startup with *"Could not load the Qt platform plugin "wayland""*. |
| `qt6-svg-plugins` | Draws the toolbar and sidebar icons. Without it the buttons show no icons. |
| `libpulse0` | Sound during playback, through PipeWire (`pipewire-pulse`) or PulseAudio. Usually installed already. Without it orbis-reframe plays without sound. |
| `libvulkan1` | Talks to the graphics card. |
| `libavcodec61` … `libswresample5` (Debian 13), `libavcodec63` … `libswresample7` (Testing), `libavcodec62` … `libswresample6` (Ubuntu 26.04) | FFmpeg: reading the video files, audio, and encoding the export. |
| `libopencv-*` | **Track subject** (Reframe section) and the privacy blur. |
| `libonnxruntime1.21` (Debian 13), `libonnxruntime1.23` (Testing, Ubuntu 26.04) | The **Voice only** and **Voice + ambience** noise reduction modes (Audio tab). The app starts without it, but those two modes say they can't load ONNX Runtime. |
| `libgomp1` | Used by the privacy blur's plate scan. Usually installed already. |

If apt can't find a package name, your distribution has a different version
of that library: check that you downloaded the build for your system
(step 1). On other distributions orbis-reframe won't run (see step 1).

Then unpack the archive into a folder of your choice, for example
`~/Applications`:

```sh
mkdir -p ~/Applications
tar -xf orbis-reframe-<version>-<target>-x86_64.tar.xz -C ~/Applications
```

This creates a folder `orbis-reframe-<version>-<target>-x86_64`. Keep
everything in it together. Besides the program (in `bin/`) it contains a
library it needs (in `lib/`) and the AI models that subject tracking, the
privacy blur and voice noise reduction use (in `share/`).

To check that no library is missing, run:

```sh
cd ~/Applications/orbis-reframe-<version>-<target>-x86_64
ldd bin/orbis-reframe | grep "not found"
```

No output means every library was found. Each line it does print is a
missing library. Find the package for it with
`apt-file search <library name>` (from the `apt-file` package) and install
it.

To remove the tarball version, delete its folder.

### Option C: the AppImage

Each release also has `orbis-reframe-<version>-x86_64.AppImage`: one file
that runs on any 64-bit distribution whose C library (glibc) is version 2.41
or newer, such as
Debian 13, Ubuntu 26.04, Fedora 42 and newer, Arch and openSUSE Tumbleweed.
It works on Debian and Ubuntu too, but there the `.deb` fits in better.

The AppImage carries the libraries that differ between distributions: Qt,
FFmpeg, OpenCV, ONNX Runtime and what they need. It takes from your system
only the C and C++ libraries, the graphics driver (step 2), the Vulkan
loader, the OpenGL / EGL libraries, fonts (fontconfig, freetype, harfbuzz),
`libX11`, `libwayland-client` and the sound server's client library (for
sound). Every desktop installation has these.

Make the file executable and start it:

```sh
chmod +x orbis-reframe-<version>-x86_64.AppImage
./orbis-reframe-<version>-x86_64.AppImage
```

(or set *Allow executing file as program* in the file's properties in your
file manager, then double-click it). Keep it wherever you like, for example
in `~/Applications`.

The AppImage mounts itself with FUSE, which needs the `fusermount3` program
from the `fuse3` package. Desktop installations usually have it. If it's
missing, the AppImage says *"No suitable fusermount binary found"*: install
`fuse3` (`sudo apt install fuse3`, `sudo dnf install fuse3`, `sudo pacman
-S fuse3`, `sudo zypper install fuse3`), or start it with
`--appimage-extract-and-run`, which unpacks it into a temporary folder
each time instead.

On its own the AppImage doesn't appear in the application menu and doesn't
register itself with the file manager. A tool such as
[Gear Lever](https://flathub.org/apps/it.mijorus.gearlever) (GNOME) or
[AppImageLauncher](https://github.com/TheAssassin/AppImageLauncher) adds it
to the menu, and can check for a new version and download it (the AppImage
tells them where its releases are; orbis-reframe itself never goes online to
check). Without one, download the new version from the
releases page as before.

To remove the AppImage, delete the file (and its menu entry, if a tool
added one).

The licences of everything the AppImage contains, and where the source code
of each part is, are inside it: `./orbis-reframe-<version>-x86_64.AppImage
--appimage-extract` unpacks it into `squashfs-root/`, and
`squashfs-root/usr/share/doc/orbis-reframe/` has them (`THIRD_PARTY.md`,
`licenses/`). To run it with a changed version of one of its LGPL
libraries, put that library in place of the one in `squashfs-root/usr/lib/`
and start `squashfs-root/AppRun`.

## 4. Start orbis-reframe

- **`.deb`:** choose **orbis-reframe** in your desktop's application menu,
  or type `orbis-reframe` in a terminal.
- **Tarball:** run the `orbis-reframe` script at the top of the unpacked
  folder:

  ```sh
  ~/Applications/orbis-reframe-<version>-<target>-x86_64/orbis-reframe
  ```

- **AppImage:** run the file, or choose it in the menu if a tool added it
  there (option C). In the commands below, use the AppImage's path in place
  of `orbis-reframe`.

It opens on the **project library**, the list of your projects (empty the
first time). You can also give it a clip or a project file to open straight
away:

```sh
orbis-reframe ~/Videos/VID_20260815_143012_00_012.insv
orbis-reframe ~/Videos/orbis-reframe/my-ride/my-ride.orp
```

With the `.deb` installed, you can also open a project (`.orp`) or a clip
(`.insv`) from your file manager: double-click it, or right-click it and
choose **Open With** > **orbis-reframe**. A clip opens in a new, unsaved
project, as from the command line. If double-clicking an `.insv` file starts a
video player instead (some file managers keep the default you chose for
all videos), choose orbis-reframe as the default for *Insta360 360° video*
in the file manager's *Open With* dialog. The tarball and the AppImage don't
register themselves with the file manager.

`orbis-reframe --version` prints the version without opening a window.

## 5. Open your first clip

1. Copy the `.insv` files from the camera's memory card to your computer.
   Keep the original file names. orbis-reframe only needs the `.insv` files.
   The `.lrv` files (low-resolution previews) aren't used.
2. In the project library, click **New project…** (`Ctrl+N`). Give the
   project a name and click **Create**. orbis-reframe makes a folder for it
   in `~/Videos/orbis-reframe`.
3. Choose **Media ▸ Add clip to timeline…** (`Ctrl+Shift+A`) and pick an
   `.insv` file. (Or import a whole folder of clips with **Media ▸ Import
   folder…** and drag them from the **Media** tab on the left onto the
   timeline.)
4. The player bar under the viewport shows *Buffering…* for a moment, and
   the viewport the first frame. Press **Space** (or the play button in the
   player bar) to play. Drag in the viewport to look around.
5. Save your work with **File ▸ Save project** (`Ctrl+S`). The project
   stores your edits and refers to the original clips, so don't move or
   rename the clips after adding them. Next time, open it from the project
   library.

**Help ▸ Keyboard shortcuts** (`F1`) lists every shortcut, and so does
[Keyboard shortcuts](keyboard-shortcuts.md), with the mouse actions too.
For a tour of the window, go on to [The main window](main-window.md).

## Files orbis-reframe writes

orbis-reframe never changes your original clips. Besides the projects and
exports you save yourself, it only writes:

- Each project's `cache/` folder: audio waveforms, privacy-blur scan
  results and the Media tab's thumbnails, so they don't have to be computed
  again. You can delete it at any time. The waveforms and thumbnails are
  rebuilt by themselves; clips with the privacy blur on have to be scanned
  again.
- `~/.cache/orbis-reframe/`: the same for untitled projects and for project
  files from before project folders.
- `<name>.orp.autosave` next to a project file, while it has unsaved
  changes: the autosave recovery file
  ([Autosave](projects-and-export.md#one-folder-per-project)). It's deleted
  when you save or discard the changes.
- `~/.local/share/orbis-reframe/projects.json`: the project library's list
  and the projects folder. Deleting it empties the list (projects in the
  projects folder still show up); no project is deleted.
- `~/.config/orbis-reframe/orbis-reframe.conf`: your settings, such as the
  theme, the window layout and which startup warnings you've chosen not to
  see again. Delete it to go back to the defaults.

## If it doesn't start

Start orbis-reframe from a terminal so you can see its messages.

| Message or symptom | Cause and fix |
|---|---|
| `error while loading shared libraries: libXYZ.so…` | A library is missing (tarball only: the `.deb` installs them). Use the `ldd` check from step 3, option B, to find which ones. With the AppImage, it's one of the few libraries it takes from the system (step 3, option C): install the package that has it, e.g. `libharfbuzz.so.0` is in `harfbuzz` or `libharfbuzz0b`. |
| AppImage: *"No suitable fusermount binary found"* or *"Cannot mount AppImage"* | FUSE is missing: install `fuse3`, or start it with `--appimage-extract-and-run` (step 3, option C). |
| AppImage: ``version `GLIBC_2.41' not found`` | The system is older than the AppImage supports (glibc 2.41, step 1). |
| `Could not load the Qt platform plugin "wayland"` | Install `qt6-wayland`. If it's installed, you are probably in an X11 session: log out and choose a Wayland session on the login screen. |
| *"Vulkan couldn't be started"* or *"Vulkan found no graphics card"* | Vulkan isn't working. Check that `vulkaninfo --summary` lists your card. If it doesn't, install the card's Vulkan driver (step 2: `nvidia-vulkan-icd` for NVIDIA, `mesa-vulkan-drivers` for AMD and Intel) and restart. |
| At startup: *"Your graphics card or driver can't decode video on the GPU"* | The card or driver has no Vulkan video decoding. orbis-reframe still works, but decodes on the processor, so 5.7K and 8K clips play slowly. On NVIDIA, update the driver; cards older than the GTX 16 / RTX 20 series can't decode on the GPU. On AMD and Intel, decoding needs a newer Mesa (step 2); `vulkaninfo \| grep video_decode` shows whether the driver has it. |
| At startup or on export: *"Exporting needs a hardware video encoder (NVIDIA NVENC, Vulkan video or VAAPI)"* | On NVIDIA, install `libnvidia-encode1` (step 2). On AMD and Intel, install the VA-API driver (step 2, [AMD and Intel](#amd-and-intel)) and check with `vainfo` that it lists encoding (`VAEntrypointEncSlice`). |
| **Track subject** or **Scan clip** is greyed out, with *"… model, which is missing"* under it | A file from the download is missing. Install or unpack orbis-reframe again. |
| Choosing **Voice only** says *"The voice modes can't run on this computer"* | Install `libonnxruntime1.21` (Debian 13) or `libonnxruntime1.23` (Testing, Ubuntu 26.04), as the message says (step 3, option B). If the message mentions the model, unpack or install orbis-reframe again. |
| *"Clip 2 plays slowly: the graphics card couldn't decode it (its memory may be full)"* in the edit toolbar, or after an export: *"The graphics card couldn't decode or stitch these clips…"* | The graphics card ran out of memory (or failed for another reason) when the clip was opened, so orbis-reframe decodes or stitches it on the processor instead. The picture is the same, but 5.7K and 8K clips play slowly and export takes much longer. Close other programs that use the graphics card (another orbis-reframe, games, a browser with many tabs), then close and reopen the project. *Graphics memory* in step 1 says how much a project needs. |
| *"The viewport can't show the picture: the graphics card's memory is probably full"* where the picture goes | The graphics card had no memory left for the viewport, when it started or when it changed size. Playback and export go on, but the viewport stays off until orbis-reframe restarts. Save the project, close other programs that use the graphics card, and start orbis-reframe again. |
| Export failed: *"The graphics card's video encoder couldn't start"* or *"… stopped at frame 1200 of 3600"* | The graphics card's encoder had no memory left (or no free encoding session). Close other programs that use the graphics card, including another orbis-reframe, and export again. **Show Details…** has the encoder's own error for a problem report. |
| The window stays black, or the clip never leaves *Buffering…* | Start with `ORBIS_VERBOSE=1 orbis-reframe` and look at the messages, and check **Help ▸ About** (below). |
| No sound, or *"No sound output found — playing without sound"* when you press play | Check that the system's sound works in other programs, that `libpulse0` is installed, and on PipeWire that `pipewire-pulse` is running. **Help ▸ About** shows the sound output orbis-reframe uses, or why it found none. To send orbis-reframe's sound to another output, use your desktop's sound settings or `pavucontrol`. |

If none of these help, please [report the
problem](https://github.com/Schabo/orbis-reframe/issues) with the terminal
output and the diagnostics from **Help ▸ About orbis-reframe**. Click **Copy
diagnostics** and paste them into the report. They list the version, your
Linux distribution, graphics card and driver, whether video decoding and
the AI features can run on your computer, which video encoder export uses
(with why the others can't be used), and the sound output. If the window
doesn't open at all, include the output of `orbis-reframe --version`, your
distribution, and your graphics card and driver (`vulkaninfo --summary`)
instead.
