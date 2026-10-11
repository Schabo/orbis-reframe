# Project Instructions

## Docs

- `docs/INSV_FORMAT.md` — reference: everything known about the X5 `.insv`/`.lrv` files (container, streams, `inst` trailer records, IMU/gyro/accel, exposure, GPS, lens calibration, video↔IMU sync). Read before touching `src/insv/` parsing, calibration, or gyro code.
- `docs/manual/` — the user manual (for users, not developers), one Markdown file per chapter, indexed in `docs/manual/README.md`. Update the affected chapter when a user-visible feature, menu, shortcut or dependency changes.
- `docs/dev-environment.md` — toolchain, dependencies and pinned versions. Update it when the build's dependencies change.
- `tools/README.md` — the developer command-line tools: what each is for, when to use it, what it needs, examples. Update it when a tool is added, removed, or its purpose or options change.
- `docs/RELEASE.md` — the release process, step by step: version bump, the release workflow's draft, GPU test, publishing, undoing a bad bump. Follow it when making a release; update it when packaging, versioning or the release flow changes.

## Folders

- `tmp/` — Use as temporary storage and scratchpad instead of filling up the real /tmp with limited space.

## Git commits

- Use **Conventional Commits** (for commitizen; the rules for people are in `CONTRIBUTING.md`, keep both in step): `type(scope): subject`, e.g. `feat(privacy): …`, `fix(playback): …`, `docs: …`. Types: feat, fix, perf, refactor, docs, test, build, ci, chore. Scope is the area: privacy, playback, stitch, stab, audio, timeline, export, track, ui, insv, core, render, build. `feat` / `fix` drive the version bump and appear in `CHANGELOG.md`; use `BREAKING CHANGE:` in the body for an incompatible project-file change. The `.githooks/commit-msg` hook (`git config core.hooksPath .githooks`) runs `cz check`; never bypass it with `--no-verify`. Don't edit the version by hand -- `cz bump` owns it (`docs/dev-environment.md`, `docs/RELEASE.md`).
- Do NOT include the Claude session link in commit messages.
- DO end every commit message with the "Co-Authored-By: Claude …" trailer.
