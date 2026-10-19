<!--
Thanks for the pull request. Anything beyond a small fix should have an
issue with an agreed plan first (CONTRIBUTING.md). If you use an AI agent,
point it at AGENTS.md before it starts.
-->

## What and why

<!-- What the change does for the user, and why this way. -->

Issue: #
Roadmap item (`docs/roadmap.md`), if any:

## Tested on

<!-- Which clips (camera, mode, resolution, bit depth) and which GPU / driver. -->

## Checklist

Tick what applies; say why for anything that applies but isn't done.

- [ ] Builds with `./scripts/build.sh`, tests pass
- [ ] Follows the planned design in `docs/roadmap.md`, or the PR says why not
- [ ] New settings are saved in the project and undo / redo covers them
- [ ] Viewport and export match (every view implementation changed together)
- [ ] X5 output unchanged, or the change to it is intended and measured
- [ ] No camera-model branches; per-camera facts are data read from the file
- [ ] Project file: new fields have a default for old files, version handled,
      `tests/project_test.cpp` covers them
- [ ] Docs updated: manual chapter / `tools/README.md` /
      `docs/dev-environment.md` / the camera's `docs/formats/` document
- [ ] Commit messages follow Conventional Commits

## Known gaps

<!-- Anything left undone or untested, so it isn't lost. -->
