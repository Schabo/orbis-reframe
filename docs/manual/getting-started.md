# Getting started

This page covers what your computer needs, which packages to install, and how
to start orbis-reframe and open your first clip.

orbis-reframe is a video editor for **Insta360 X5** 360° footage. It opens the
camera's `.insv` files directly. You don't need Insta360 Studio or any other
Insta360 software.

> **Early software.** orbis-reframe has been built and tested on one computer
> so far (Debian Testing, an NVIDIA RTX 5070, the Sway desktop; the Debian 13
> and Ubuntu 26.04 builds in containers of those systems on that computer). Other setups that meet
> the requirements below should work but haven't been tried.

## 1. Check your computer

| | Needed |
|---|---|
| **Operating system** | Linux, 64-bit (x86_64): **Debian 13** ("trixie"), **Debian Testing** or **Ubuntu 26.04 LTS**. There is a separate build for each, because each needs the library versions of its own release (Debian 13: Qt 6.8, FFmpeg 7.1; Testing: Qt 6.10, FFmpeg 9; Ubuntu 26.04: Qt 6.10, FFmpeg 8). Download the one for your system. None of them starts on older releases, such as Ubuntu 24.04, whose versions differ. To see which system you have: `grep -E '^(ID\|VERSION_ID)=' /etc/os-release` prints `ID=debian` and `VERSION_ID="13"` on Debian 13, `ID=debian` and no `VERSION_ID` on Testing, and `ID=ubuntu` and `VERSION_ID="26.04"` on Ubuntu 26.04. |
| **Desktop session** | **Wayland**: Sway, GNOME, KDE Plasma or another Wayland desktop. An X11 session doesn't work: orbis-reframe exits at startup instead of falling back to X11. |
| **Graphics card** | **NVIDIA**, with the proprietary driver. Playback uses Vulkan (including Vulkan video decoding) and export uses NVIDIA's hardware encoder (NVENC). Tested on an RTX 5070 with driver 595. Other NVIDIA cards from the GTX 16 / RTX 20 series onwards should work. AMD and Intel graphics are not supported: playback may fall back to the processor, but export won't work at all. |
| **Memory** | 16 GB RAM or more. 8K clips are large. |
| **Sound** | PipeWire (with `pipewire-pulse`) or PulseAudio. This is the default on current desktops. |

To check your session and graphics card, run these in a terminal:

```sh
echo $XDG_SESSION_TYPE          # should print: wayland
nvidia-smi                      # should list your NVIDIA card and driver version
vulkaninfo --summary | grep -i nvidia   # should show the card as a Vulkan device
```

`vulkaninfo` comes from the `vulkan-tools` package.

## 2. Install the NVIDIA driver

If `nvidia-smi` above already worked and `vulkaninfo` showed the card, go on
to step 3.

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

## 3. Install orbis-reframe

Download orbis-reframe from the [releases
page](https://github.com/Schabo/orbis-reframe/releases). There's a build for
Debian 13, one for Debian Testing and one for Ubuntu 26.04 (step 1), and each
comes in two forms.
Both forms contain the same program and use the system's own Qt, FFmpeg,
Vulkan and OpenCV libraries:

| | Debian 13 | Debian Testing | Ubuntu 26.04 |
|---|---|---|---|
| Debian package | `orbis-reframe_<version>.deb13_amd64.deb` | `orbis-reframe_<version>_amd64.deb` | `orbis-reframe_<version>.ubuntu26.04_amd64.deb` |
| Archive | `orbis-reframe-<version>-debian-13-x86_64.tar.xz` | `orbis-reframe-<version>-debian-testing-x86_64.tar.xz` | `orbis-reframe-<version>-ubuntu-26.04-x86_64.tar.xz` |

- The **`.deb`** is a Debian package. apt installs the libraries it needs,
  and it adds orbis-reframe to your desktop's application menu. Use this
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
It doesn't install the NVIDIA driver (step 2), because a driver installed
from NVIDIA's website would conflict with Debian's driver packages.

To remove it again: `sudo apt remove orbis-reframe`.

### Option B: the tarball

First install the libraries.

On Debian 13, use this command:

```sh
sudo apt install \
    libqt6core6t64 libqt6gui6 libqt6widgets6 qt6-wayland qt6-svg-plugins \
    libqt6multimedia6 \
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
    libqt6multimedia6 \
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
    libqt6multimedia6 \
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
| `libqt6multimedia6` | Sound during playback. |
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

## 4. Start orbis-reframe

- **`.deb`:** choose **orbis-reframe** in your desktop's application menu,
  or type `orbis-reframe` in a terminal.
- **Tarball:** run the `orbis-reframe` script at the top of the unpacked
  folder:

  ```sh
  ~/Applications/orbis-reframe-<version>-<target>-x86_64/orbis-reframe
  ```

It opens on the **project library**, the list of your projects (empty the
first time). You can also give it a clip or a project file to open straight
away:

```sh
orbis-reframe ~/Videos/VID_20260815_143012_00_012.insv
orbis-reframe ~/Videos/orbis-reframe/my-ride/my-ride.orp
```

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

- Each project's `cache/` folder: audio waveforms and privacy-blur scan
  results, so they don't have to be computed again. You can delete it at
  any time. The waveforms are rebuilt by themselves; clips with the privacy
  blur on have to be scanned again.
- `~/.cache/orbis-reframe/`: the same for untitled projects and for project
  files from before project folders.
- `~/.local/share/orbis-reframe/projects.json`: the project library's list
  and the projects folder. Deleting it empties the list (projects in the
  projects folder still show up); no project is deleted.
- `~/.config/orbis-reframe/orbis-reframe.conf`: which startup warnings
  you've chosen not to see again. Delete it to see them again.

## If it doesn't start

Start orbis-reframe from a terminal so you can see its messages.

| Message or symptom | Cause and fix |
|---|---|
| `error while loading shared libraries: libXYZ.so…` | A library is missing (tarball only: the `.deb` installs them). Use the `ldd` check from step 3, option B, to find which ones. |
| `Could not load the Qt platform plugin "wayland"` | Install `qt6-wayland`. If it's installed, you are probably in an X11 session: log out and choose a Wayland session on the login screen. |
| *"Vulkan couldn't be started"* or *"Vulkan found no graphics card"* | Vulkan isn't working. Check that `vulkaninfo --summary` lists the NVIDIA card. If it doesn't, install `nvidia-vulkan-icd` (step 2) and restart. |
| At startup: *"Your graphics card or driver can't decode video on the GPU"* | The card or driver has no Vulkan video decoding. orbis-reframe still works, but decodes on the processor, so 5.7K and 8K clips play slowly. Update the NVIDIA driver; cards older than the GTX 16 / RTX 20 series can't decode on the GPU. |
| At startup or on export: *"Exporting needs NVIDIA's hardware video encoder (NVENC)"* | Install `libnvidia-encode1` (step 2). Without an NVIDIA card, exporting isn't possible yet. |
| **Track subject** or **Scan clip** is greyed out, with *"… model, which is missing"* under it | A file from the download is missing. Install or unpack orbis-reframe again. |
| Choosing **Voice only** says *"The voice modes can't run on this computer"* | Install `libonnxruntime1.21` (Debian 13) or `libonnxruntime1.23` (Testing), as the message says (step 3, option B). If the message mentions the model, unpack or install orbis-reframe again. |
| The window stays black, or the clip never leaves *Buffering…* | Start with `ORBIS_VERBOSE=1 orbis-reframe` and look at the messages, and check **Help ▸ About** (below). |
| No sound | Check that `libqt6multimedia6` is installed and that the system's sound works in other programs. To send orbis-reframe's sound to another output, use your desktop's sound settings or `pavucontrol`. |

If none of these help, please [report the
problem](https://github.com/Schabo/orbis-reframe/issues) with the terminal
output and the diagnostics from **Help ▸ About orbis-reframe**. Click **Copy
diagnostics** and paste them into the report. They list the version, your
Linux distribution, graphics card and driver, and whether video decoding,
export and the AI features can run on your computer. If the window doesn't
open at all, include the output of `orbis-reframe --version`, your
distribution, and your graphics card and driver version (from `nvidia-smi`)
instead.
