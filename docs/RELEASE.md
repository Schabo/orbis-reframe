# Release process

How to make a release of orbis-reframe, from a clean `main` to the published
files on GitHub (`github.com/Schabo/orbis-reframe`). You bump the version and
push the tag; the release workflow (`.github/workflows/release.yml`) builds
the packages on GitHub's runners and makes a **draft** release; you test the
draft's packages on the GPU machine and publish it.

A release has three targets (`scripts/targets.sh`), each its own build:
`debian-testing`, `debian-13` and `ubuntu-26.04`, each built in a clean
container of that release with `scripts/package.sh` -- the same script you
can run by hand (step 4).

A release is:

| File | What |
|---|---|
| `orbis-reframe-<version>-debian-testing-x86_64.tar.xz` | Unpack-anywhere build for Debian Testing x86_64, with a top-level `orbis-reframe` launcher |
| `orbis-reframe_<version>_amd64.deb` | The same build as a Debian package (`/usr`) |
| `orbis-reframe-<version>-debian-13-x86_64.tar.xz` | The same for Debian 13 |
| `orbis-reframe_<version>.deb13_amd64.deb` | Debian 13's `.deb` (its package version is a backport-style `<version>~deb13`, below Testing's; GitHub doesn't allow `~` in file names) |
| `orbis-reframe-<version>-ubuntu-26.04-x86_64.tar.xz` | The same for Ubuntu 26.04 |
| `orbis-reframe_<version>.ubuntu26.04_amd64.deb` | Ubuntu 26.04's `.deb` (version `<version>~ubuntu26.04`) |
| `orbis-reframe-<version>-source.tar.xz` | `git archive` of the release tag (GPL-3.0: the source goes with every binary) |
| `*.sha256` | One per file above |
| Release notes | The version's section of `CHANGELOG.md` |

## 1. One-time setup

- commitizen (`sudo apt install commitizen`) and the commit-msg hook:
  `git config core.hooksPath .githooks`.
- An NVIDIA machine with a Wayland desktop to test on (step 6).
- For `scripts/release_test.sh` (step 6): `podman`, the NVIDIA Container
  Toolkit (NVIDIA's apt repository) and its device list,
  `sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml` -- again
  after every driver update. `ffmpeg` (for `ffprobe`) and the test clip in
  `sample_footage/` (named in `tests/release/smoke.orp`; `--footage DIR`
  for another folder).
- Push access to `github.com/Schabo/orbis-reframe` and the GitHub CLI,
  `gh auth login` (the draft is only visible to maintainers).
- Only for building packages yourself (step 4, optional): the build
  dependencies from `docs/dev-environment.md`, **all of them** -- a release
  has every feature, and `scripts/package.sh` refuses to configure if one is
  missing (`ORBIS_REQUIRE_ALL_FEATURES`) -- plus `ninja-build`, `dpkg-dev`
  (`dpkg-shlibdeps`), `desktop-file-utils` (`desktop-file-validate`) and
  `xz-utils`.

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
4. The CI and GPU workflows are green on `main`'s last commit
   (`gh run list --branch main`).
5. If the `apt` dependencies changed since the last release (a new library,
   a new major version of Qt / FFmpeg / OpenCV / ONNX Runtime), check the
   package list in `docs/manual/getting-started.md` (step 3, option B)
   against the `.deb`'s `Depends` (step 4's dry run prints it), and fix the
   guide first.
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

If the preview looks right, bump for real:

```sh
cz bump
```

This sets the version in `.cz.toml` and in `project(orbis-reframe VERSION …)`
in `CMakeLists.txt`, writes `CHANGELOG.md`, makes a commit
`bump: version X → Y` and tags it `vY` (an annotated tag, `annotated_tag` in
`.cz.toml`, so `git push --follow-tags` takes it along). Don't edit any of
these by hand. Nothing is pushed yet, so it can still be undone (step 7).

## 4. Optional: try the packages before pushing the tag

Once the tag is pushed it stays (step 7), so when the release changes the
packaging or the dependencies, try the packages first, while the bump can
still be undone. Either of:

- **The workflow's dry run**, on `main` before the bump (no tag needed):
  `gh workflow run release.yml`, then `gh run watch`. It builds and checks
  all three targets' packages like a real run and keeps them as workflow
  artifacts (Actions ▸ the run ▸ Artifacts), but makes no release. The
  version is a dev one (`Y+N.gabc1234`).
- **By hand on this machine**, on the bump commit, with the GPU test:

  ```sh
  ./scripts/package.sh                         # dist/debian-testing/
  ./scripts/package.sh --target debian-13      # in a debian:trixie container
  ./scripts/package.sh --target ubuntu-26.04   # in an ubuntu:26.04 container
  scripts/release_test.sh --target debian-testing   # and the other two
  ```

  `package.sh` builds from scratch in `build-package/<target>/` (Release,
  all features, no source-tree fallback, `-ffile-prefix-map`), runs
  `ctest`, makes the packages with CPack, adds the source archive and
  `.sha256` files in `dist/<target>/` (replaced each run), and checks the
  unpacked tarball: the launcher, `--version`, that every library is found
  and ncnn comes from the package, that no source-tree or `$HOME` path is
  left, the models, the license files and the desktop entry. It ends with
  `== done`, or with `FAIL` lines and a non-zero exit. It refuses
  uncommitted changes unless given `--allow-dirty` (for testing only).

## 5. Push: the workflow makes the draft

```sh
git push origin main --follow-tags
gh run watch        # pick the "Release" run for vY
```

The tag starts the release workflow. Per target, in a clean container of
that release: it checks that the tag is the version in `.cz.toml` and
`CMakeLists.txt`, runs `scripts/package.sh` (all of step 4's checks), then
installs the `.deb` in a fresh container (apt finds every dependency, every
library resolves, `--version` is right). Then it makes a draft release
`vY` with the fourteen files, their build provenance (`gh attestation
verify <file> --repo Schabo/orbis-reframe`) and the version's section of
`CHANGELOG.md` as notes. About 15 minutes.

If a job fails for a passing reason (a mirror or download hiccup), re-run
the failed jobs: `gh run rerun <run id> --failed`. If the draft was already
made, delete it first (`gh release delete vY`; the tag stays).

In the run's log, check the "Build the packages" step of each target: the
first line says `== orbis-reframe Y (<target>)`, **exactly the new
version**, and the printed `Depends:` has nothing unexpected -- in
particular no `qt6-base-private-abi` (from Qt's private-ABI classes; it
would make the `.deb` uninstallable after a Qt update, see
`docs/dev-environment.md`). The Debian 13 `Version:` ends in `~deb13`,
Ubuntu 26.04's in `~ubuntu26.04`.

## 6. Test the draft's packages on the GPU machine

GitHub's runners have no GPU and start nothing, so the GUI and the GPU are
tested here, on the files that will be published:

```sh
scripts/fetch_release.sh vY                  # the draft's files -> dist/<target>/
scripts/release_test.sh                      # tests dist/debian-testing/; ~5 minutes
scripts/release_test.sh --target debian-13   # tests dist/debian-13/
scripts/release_test.sh --target ubuntu-26.04   # tests dist/ubuntu-26.04/
```

`release_test.sh` runs in clean containers of the target's release
(`debian:testing`, `debian:trixie`, `ubuntu:26.04`) that use this machine's
NVIDIA GPU and driver (podman + the NVIDIA Container Toolkit,
`docs/dev-environment.md`).

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
- **GUI test**, per package: the smoke test (`tests/release/gpu_smoke.sh
  --sound`, the same one the GPU workflow runs on the dev build) on the
  installed app, on a headless Sway with PipeWire: opens 10 s of a clip from
  `sample_footage/` (`tests/release/smoke.orp`), plays it with "Voice only"
  (frames arrive and are drawn, the viewport shows a picture, ONNX Runtime
  loads, sound reaches the output), Track subject, Scan clip, File ▸ Export
  flat video (HEVC, 1920x1080, as long as the timeline, with audio, a
  picture in its middle frame), saves, quits. The app is driven by
  `tests/release/gpu_smoke.steps`, by key, action and button names -- no
  screen positions.
- **Dev test**: the source archive builds with Debian's `-dev` packages,
  `ctest` passes, the headless GPU tools run (`tests/release/gpu_tools.sh`).

It prints one line per check, with the measured values, and ends with "all
checks passed", or exits non-zero naming the failures. Logs, the saved
project, the export and the viewport's grabs are in
`tmp/release-test/<version>/<target>/` (per package: `app.log`,
`results.txt`); a step that failed left a `fail-<line>.png` of the window or
the dialog that was up, `<line>` being its line in `steps.txt` there.

Also check `Maintainer:` in `dpkg-deb --field dist/*/*.deb`
(`CPACK_PACKAGE_CONTACT` in `cmake/Packaging.cmake`).

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

## 7. If something is wrong

**Before the tag is pushed**, the bump can be undone and redone. These
two commands throw away the bump commit and its tag; make sure HEAD is
the bump commit (`git log -1` shows `bump: version …`) first:

```sh
git tag -d vY
git reset --hard HEAD~1
```

Then fix the problem (normal commits), and go back to step 3.

**After the tag is pushed**, never move or delete it: others may have
fetched it. Delete the draft (`gh release delete vY`), fix the problem and
make a new patch release (Y+0.0.1) with the steps above; vY stays an
unreleased tag. Releases are immutable (a repository setting): once
published, a release's files and tag can't be changed or deleted. If a
published file is broken, make the patch release and add a warning to the
old release's notes pointing to it.

## 8. Publish

Check the draft on GitHub (Releases ▸ vY): all fourteen files, the notes.
Add anything the notes are missing (known issues, changed dependencies),
then publish it there or with `gh release edit vY --draft=false`.
Publishing locks its files and tag for good.

## 9. Afterwards

- Close the issues and milestones the release finished, and note what was
  tested and anything that came up.
- README / project page (`site/`): if they mention the latest version or
  "not published yet", update them. A push to `main` that changes `site/`
  publishes the page to <https://schabo.github.io/orbis-reframe/>
  (`.github/workflows/pages.yml`).
- From the next commit on, builds call themselves `Y+1.gabc1234`, so a
  dev build is never mistaken for the release.
