# Instructions for coding agents

orbis-reframe is a Linux editor for 360 camera footage: open a dual-fisheye
clip, stitch it on the GPU, steer a virtual camera through the sphere with
keyframes, and export a normal flat video. One maintainer, C++20 / Qt 6
Widgets / Vulkan / FFmpeg. These instructions apply to any AI agent working
on this repository, and to people too.

The goal of a change here is the solution the project would keep, not the
smallest change that makes the symptom go away. A temporary fix that has to be
taken out again when the real feature comes costs more than waiting for it.
When the right solution is bigger than the task, tell the person you're
working for instead of building a shortcut.

**Don't post on GitHub unless the person you're working for asks you to**:
no issues, comments, reviews or pull requests on your own initiative. Where
these instructions say a plan or a question should reach the maintainer,
write it out for your person to post.

## Before you write code

1. **Read [`docs/architecture.md`](docs/architecture.md)**: the decisions
   that bound all work, how the pieces fit, and the rules every change has to
   keep.
2. **Find your change in [`docs/roadmap.md`](docs/roadmap.md).** Most areas
   have a planned design. If yours does, build toward it, or build the first
   step of it. Don't build a different mechanism next to it. If the planned
   design doesn't fit what you found, stop and tell your person why; don't
   quietly work around it.
3. **Read the docs for the area you touch**: the camera's document in
   [`docs/formats/`](docs/formats/README.md) before any `src/insv/`,
   calibration, gyro or file-format work; the user manual chapter
   ([`docs/manual/`](docs/manual/README.md)) for the feature you change.
4. **Agree on a plan with the maintainer first** for anything that is more
   than a bug fix. Write a short plan -- which files, which data goes where,
   what is saved in the project, how it is tested -- and stop for your person
   to review it and take it to the maintainer in an issue. A plan agreed on
   in five minutes saves a pull request that has to be redone. This is
   required, not a suggestion, for:
   - anything on the roadmap;
   - anything that changes the project file (`.orp`);
   - colour, stitching, projection, stabilization or export output;
   - support for a new camera or camera mode;
   - a new dependency, setting, or environment variable.

## Rules every change keeps

These come from reviews; a pull request that breaks one is sent back.

- **Every setting the user makes is saved in the project and can be undone.**
  A checkbox or value that lives only in the widget, or only in app-wide
  settings when it changes a project's output, is a bug. See *The project
  file* in `docs/architecture.md`.
- **The viewport and the export show the same picture.** The view math
  exists in several places (viewport shaders, GPU export, CPU export, the
  subject tracker's crop); change all of them together, and say in the PR
  how you checked they match.
- **Existing output doesn't change by accident.** For X5 clips, stitched
  frames, stabilizer output and NVENC exports stay identical unless the
  change is meant to alter them. If it does change them, say what changed and
  why, with a measurement (PSNR, before / after frames).
- **Camera differences are data, not branches.** Don't add
  `if (model == "X6")` paths through the pipeline; put what differs into the
  data a camera's reader fills in (see *Common camera model* in the roadmap).
- **Colour transforms go through LUTs**, never hard-wired into the stitch or
  the decode. The stitch delivers the camera's picture as recorded; log and
  HDR conversion is the clip's LUT in the Colour panel, which the user can
  change or turn off.
- **Nothing is hard-wired that the file can tell you.** Read the stream's
  bit depth, matrix, range and transfer from the stream, sizes from the
  file's metadata; fall back to a documented default only when the file
  doesn't say, and log that you did.
- **No silent quality loss.** Don't widen, narrow or convert pixel formats
  on paths that don't need it (e.g. 8-bit content into a 10-bit encode).
- **Everything new degrades, it doesn't break.** A GPU feature the device
  lacks falls back (CPU decode, the next encoder) with a clear message.
- **Local-only**: editing, playback and export never touch the network.
- **Format knowledge comes only from allowed sources**:
  [`docs/formats/README.md`](docs/formats/README.md) says which. Never
  disassemble or decompile a vendor's software, and don't use anything
  derived from that.

## How to work

- Build and test with `./scripts/build.sh` (configures, builds, runs
  `ctest`); `./scripts/build.sh -r` starts the app. Toolchain and versions:
  [`docs/dev-environment.md`](docs/dev-environment.md).
- Check on real clips with the tools in [`tools/README.md`](tools/README.md)
  (`reframe_export`, `insv_stitch`, `view_compare`, `insv_trailer_dump`, ...)
  and say in the PR which clips and which GPU you used.
- Use `tmp/` in the repository for scratch files, not `/tmp`.
- Match the code around the change: naming, formatting, comment density.
  Comments say *why* and stand on their own -- no references to issue
  numbers, chats or PRs.
- Keep docs in the same commit as the change: the manual chapter for
  anything users see, `tools/README.md`, `docs/dev-environment.md`, the
  camera's format document for new file facts (with their source).
- One concern per pull request. Don't stack unrelated changes, and don't
  stack a PR on another that is still under review unless it truly depends
  on it.
- If you find a problem you don't fix, write it in the PR description; don't
  leave a `TODO` that nobody will find.

## Commits

Conventional Commits, checked by commitizen (`.githooks/commit-msg`; turn it
on with `git config core.hooksPath .githooks`). `type(scope): subject`, e.g.
`feat(stitch): …`, `fix(playback): …`, `docs: …`. Types: feat, fix, perf,
refactor, docs, test, build, ci, chore. Scopes: privacy, playback, stitch,
stab, audio, timeline, export, track, ui, insv, core, render, build. A change
that breaks old project files needs `BREAKING CHANGE:` in the body. Never
`--no-verify`, and never edit the version by hand. Full rules:
[`CONTRIBUTING.md`](CONTRIBUTING.md).

## Docs map

- `docs/architecture.md` -- standing decisions, structure, rules.
- `docs/roadmap.md` -- what's planned, the intended design of each, what's
  ruled out.
- `docs/formats/` -- camera file formats, one document per camera;
  `insta360-x5.md` is the full reference.
- `docs/manual/` -- the user manual, one file per chapter.
- `docs/dev-environment.md` -- toolchain, dependencies, pinned versions.
- `tools/README.md` -- the developer command-line tools.
- `docs/RELEASE.md` -- the release process (maintainer only).
