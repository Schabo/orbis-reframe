# Release process (manual)

How to make a release of orbis-reframe by hand, from a clean `main` to the
published files. This page puts the pieces in order. Parts that don't exist yet are marked **(not yet)**:
the GitHub repository. Until it
does, the matching steps are done by hand as written here.

A release has three targets (`scripts/targets.sh`), each its own build:
`debian-testing` (built on this machine), `debian-13` (built in a
`debian:trixie` podman container) and `ubuntu-26.04` (in an `ubuntu:26.04`
container). Steps 4 and 5 run once per target.

A release is:

| File | What |
|---|---|
| `orbis-reframe-<version>-debian-testing-x86_64.tar.xz` | Unpack-anywhere build for Debian Testing x86_64, with a top-level `orbis-reframe` launcher |
| `orbis-reframe_<version>_amd64.deb` | The same build as a Debian package (`/usr`) |
| `orbis-reframe-<version>-debian-13-x86_64.tar.xz` | The same for Debian 13 |
| `orbis-reframe_<version>~deb13_amd64.deb` | Debian 13's `.deb` (a backport-style version, below Testing's) |
| `orbis-reframe-<version>-ubuntu-26.04-x86_64.tar.xz` | The same for Ubuntu 26.04 |
| `orbis-reframe_<version>~ubuntu26.04_amd64.deb` | Ubuntu 26.04's `.deb` |
| `orbis-reframe-<version>-source.tar.xz` | `git archive` of the release tag (GPL-3.0: the source goes with every binary) |
| `*.sha256` | One per file above |
| Release notes | The version's section of `CHANGELOG.md` |

## 1. One-time setup

- Build dependencies from `docs/dev-environment.md`, **all of them**: a
  release has every feature, and `scripts/package.sh` refuses to configure
  if one is missing (`ORBIS_REQUIRE_ALL_FEATURES`). That includes
  `qt6-multimedia-dev`, `libopencv-dev`, `libopencv-contrib-dev`,
  `libonnxruntime-dev` and ncnn (`scripts/fetch_ncnn.sh`).
- `ninja-build` (the package build uses Ninja), `dpkg-dev` (for
  `dpkg-shlibdeps`), `desktop-file-utils` (`desktop-file-validate`), `xz-utils`.
- commitizen (`sudo apt install commitizen`) and the commit-msg hook:
  `git config core.hooksPath .githooks`.
- An NVIDIA machine with a Wayland desktop to test on (step 5). The
  packages are built and tested on the same machine.
- For `scripts/release_test.sh` (step 5): `podman`, the NVIDIA Container
  Toolkit (NVIDIA's apt repository) and its device list,
  `sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml` -- again
  after every driver update. `ffmpeg` (for `ffprobe`) and the test clip in
  `sample_footage/` (named in `tests/release/smoke.orp`; `--footage DIR`
  for another folder).
- **(not yet)** The GitHub repository, `gh auth login`.

## 2. Get `main` ready

1. Everything for the release is committed on `main`, the tree is clean
   (`git status`), and the last commits follow Conventional Commits: only
   `feat` / `fix` / `perf` / `refactor` / `BREAKING CHANGE` commits appear
   in the changelog.
2. The issue list: no open **high**-severity issue that the release would
   ship with, unless you decide it can go out (then say so in the release
   notes).
3. The user manual (`docs/manual/`) matches the app: new menus, panels,
   shortcuts and dependencies are described.
4. `./scripts/build.sh` builds and `ctest` passes.
5. If the `apt` dependencies changed since the last release (a new library,
   a new major version of Qt / FFmpeg / OpenCV / ONNX Runtime), check the
   package list in `docs/manual/getting-started.md` (step 3, option B)
   against the `.deb`'s `Depends` after step 4, and fix the guide first.
6. If a third-party component was added, removed or updated (a library,
   a bundled one like ncnn, a model, icons), update `THIRD_PARTY.md` and
   its license text (`packaging/licenses/`, or the model's folder);
   `scripts/package.sh` checks that the listed license files are packaged.

## 3. Bump the version and tag

Preview the version and changelog section first. It changes nothing:

```sh
cz bump --dry-run
```

The version comes from the commits since the last tag. While the major
version is 0 (`major_version_zero` in `.cz.toml`), a `feat` raises the
minor version (0.1.0 → 0.2.0), a `fix` / `perf` the patch version
(0.1.0 → 0.1.1), and a `BREAKING CHANGE` the minor version too.
**The very first release** has to be forced to 0.1.0:
`cz bump --dry-run --increment MINOR`.

If the preview looks right, bump for real and keep the release notes:

```sh
mkdir -p tmp
cz bump --changelog-to-stdout > tmp/release-notes.md
# first release: cz bump --increment MINOR --changelog-to-stdout > tmp/release-notes.md
```

This sets the version in `.cz.toml` and in `project(orbis-reframe VERSION …)`
in `CMakeLists.txt`, writes `CHANGELOG.md`, makes a commit
`bump: version X → Y` and tags it `vY` (an annotated tag, `annotated_tag` in
`.cz.toml`, so `git push --follow-tags` takes it along). Don't edit any of these by hand.
Nothing is pushed yet. If something is wrong, it can still be undone (step 6).

## 4. Build the packages

```sh
./scripts/package.sh                     # dist/debian-testing/
./scripts/package.sh --target debian-13  # dist/debian-13/, in a debian:trixie container
./scripts/package.sh --target ubuntu-26.04  # dist/ubuntu-26.04/, in an ubuntu:26.04 container
```

Each takes a few minutes (the first container run longer: it downloads the
build dependencies into `tmp/package-apt-cache/`). Each builds from scratch
in `build-package/<target>/` (Release,
all features, no source-tree fallback, `-ffile-prefix-map`), runs `ctest`,
makes the packages with CPack, adds the source archive and `.sha256` files
in `dist/<target>/` (both folders are replaced each run), and then checks the
unpacked tarball: the launcher, `--version`, that every library is found
and ncnn comes from the package, that no source-tree or `$HOME` path is left,
the models, and the desktop entry. It ends with `== done`, or with `FAIL`
lines and a non-zero exit.

Then check by eye:

- The first line says `== orbis-reframe Y (<target>)`, **exactly the new version**. A
  `+N.gabc1234` suffix means HEAD isn't the tag, and `.dirty` means
  uncommitted changes (it refuses those unless `--allow-dirty` is given,
  which is for testing only).
- The printed `Depends:` line: nothing unexpected, in particular no
  `qt6-base-private-abi` (that comes from using Qt's private-ABI classes
  and would make the `.deb` uninstallable after a Qt update; see
  `docs/dev-environment.md`). Compare it with the guide's `apt` line for
  that target (step 2.5). The Debian 13 `Version:` ends in `~deb13`, Ubuntu
26.04's in `~ubuntu26.04`.
- `Maintainer:` in `dpkg-deb --field dist/*/*.deb` is right
  (`CPACK_PACKAGE_CONTACT` in `cmake/Packaging.cmake`).

## 5. Test the packages on the GPU machine

`package.sh` doesn't start the GUI or touch the GPU. `scripts/release_test.sh`
does, in clean containers of the target's release (`debian:testing`,
`debian:trixie`, `ubuntu:26.04`) that use this machine's NVIDIA GPU and driver (podman + the
NVIDIA Container Toolkit, `docs/dev-environment.md`):

```sh
scripts/release_test.sh                      # tests dist/debian-testing/; ~5 minutes
scripts/release_test.sh --target debian-13   # tests dist/debian-13/
scripts/release_test.sh --target ubuntu-26.04   # tests dist/ubuntu-26.04/
```

A distribution built on a target, such as Linux Mint on Ubuntu, has no
build of its own. `--image` runs a target's test in that distribution's
image instead, e.g. `--target ubuntu-26.04 --image
docker.io/linuxmintd/mint23-amd64` once Linux Mint 23 is out (its
development image's repository is gone). Results then go to
`tmp/release-test/<version>/<target>-on-<image>/`.

- **Clean install**, per package: the `.deb` with apt, the tarball with the
  target's `apt` line from the install guide (option B) -- every library found,
  `--version` right. This is also the check that the guide's package list
  is complete.
- **GUI test**, per package, on a headless Sway with PipeWire: opens 10 s of
  a clip from `sample_footage/` (`tests/release/smoke.orp`), plays it with
  "Voice only" (frames arrive, ONNX Runtime loads, sound reaches the
  output), Track subject, Scan clip, File ▸ Export flat video (HEVC,
  1920x1080, 10 s, with audio), saves, quits.
- **Dev test**: the source archive builds with Debian's `-dev` packages,
  `ctest` passes, the headless GPU tools run.

It prints one line per check and ends with "all checks passed", or exits
non-zero naming the failures. Logs, the saved project, the export and a
screenshot after each GUI step are in `tmp/release-test/<version>/<target>/` --
look at the screenshots when a GUI check fails (the clicks are at fixed
positions for the default layout; a moved button shows there).

Not covered: Debian 13's own NVIDIA driver (550; the containers use this
machine's), which doesn't support RTX 50-series cards anyway. Also not
covered, so check by hand on the real desktop, with the Debian Testing packages from
`dist/debian-testing/`, never with the dev build -- briefly, unless the release changed
something there:

- [ ] `.deb`: `sudo apt install ./dist/debian-testing/orbis-reframe_*_amd64.deb`; the menu
      entry shows the name and icon, and starting from the menu works;
      `sudo apt remove orbis-reframe` afterwards.
- [ ] **Help ▸ About**: the version, and the diagnostics list GPU video
      decode, NVENC and all models as found.
- [ ] Playback sounds right on the real speakers; dragging looks around.
- [ ] GNOME or KDE (the reference desktop is Sway), drag and drop of a clip
      from the file manager.

## 6. If something is wrong

**Before anything is pushed**, the bump can be undone and redone. These
two commands throw away the bump commit and its tag; make sure HEAD is
the bump commit (`git log -1` shows `bump: version …`) first:

```sh
git tag -d vY
git reset --hard HEAD~1
```

Then fix the problem (normal commits), and go back to step 3.

**After the tag is pushed**, never move or delete it: others may have
fetched it. Fix the problem and make a new patch release (Y+0.0.1) with
the steps above. If a published file is broken, take it down from the
release page and say why in the release notes.

## 7. Publish

**(not yet: the GitHub repository doesn't exist.)** Once it does:

```sh
git push origin main --follow-tags
gh release create vY --draft --title "orbis-reframe Y" \
    --notes-file tmp/release-notes.md dist/debian-testing/* \
    dist/debian-13/*~deb13* dist/debian-13/*-debian-13-* \
    dist/ubuntu-26.04/*~ubuntu26.04* dist/ubuntu-26.04/*-ubuntu-26.04-*
```

(Both targets' folders hold the same source archive; it's uploaded once.)
Check the draft on GitHub (all ten files, the notes), add anything the
notes are missing (known issues, changed dependencies), then publish it.
Later, a GitHub Actions workflow moves the build into a
release workflow that makes the draft. Then step 4 runs in CI and this
step only reviews and publishes; the GPU test (step 5) stays by hand.

Until then, keep `dist/` (or copy it somewhere safe) and push the tag
wherever the repository lives, so the source archive and the tag match.

## 8. Afterwards

- Close the issues and milestones the release finished, and note what was
  tested and anything that came up.
- README / project page (`site/`): if they mention the latest version or
  "not published yet", update them.
- From the next commit on, builds call themselves `Y+1.gabc1234`, so a
  dev build is never mistaken for the release.
