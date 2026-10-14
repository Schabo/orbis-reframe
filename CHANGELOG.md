## v0.3.0 (2026-10-14)

### Feat

- **timeline**: split the clip at the playhead from the edit toolbar
- **ui**: pick a theme under View ▸ Theme: Ember, Light or Neutral dark
- **core**: autosave unsaved changes to a recovery file
- **ui**: rename and duplicate projects from the project library
- **ui**: notice media files going missing or coming back while the app runs
- **ui**: show each overlay track's layer and move it forward or backward
- **timeline**: fold a video track's sound tracks away under it
- **timeline**: keep unlinked audio in place and own-sound tracks to their source
- **timeline**: tracks in fixed groups, each overlay with its own sound track
- **audio**: an overlay's sound works like a video clip's
- **audio**: Audio tab lists every audio clip of the video clip
- **audio**: Audio tab in three sections
- **audio**: every .insv audio stream on its own track
- **timeline**: edit toolbar above the timeline, centred
- **timeline**: timeline across the full window width
- **ui**: Help ▸ User manual opens the manual on GitHub
- **ui**: player bar under the viewport
- **ui**: settings sidebar in tabs, with collapsible sections
- **ui**: locate a moved or renamed media file
- **ui**: the left sidebar can be resized
- **ui**: left sidebar with the Media tab, the project's media pool
- **ui**: the Ember theme, built from colour tokens
- **ui**: project library start screen and one folder per project
- **insv**: read the camera's stitched thumbnail from the trailer

### Fix

- **ui**: show "1 key" / "3 keys" instead of "key(s)" style plurals
- **ui**: keep the player bar's time moving while Track subject or Blur a plate runs
- **timeline**: keep a folded overlay track folded through undo and redo
- **core**: write a finished sync or seam measurement into the project file
- **ui**: keep a video overlay's frame rate in the media pool
- **timeline**: scroll the tracks while a drag is held at their top or bottom
- **timeline**: name an overlay's track "audio" and indent the mic tracks
- **audio**: move hand-placed mics onto their stream tracks on open
- **core**: opening a project no longer empties the project library
- **ui**: Enter presses a dialog's default button again

### Refactor

- **core**: drop the "micAir" track role alias
- **timeline**: drop the "overlaySound" role alias
- **timeline**: call an overlay's track its audio track in the code too

### Perf

- **ui**: keep the Media tab's thumbnails on disk

## v0.2.0 (2026-10-11)

### Feat

- **timeline**: drop .insv clips from the file manager
- **stab**: set stabilization per clip

### Fix

- **build**: .deb file names without '~', as GitHub serves them

## v0.1.0 (2026-10-10)

### Feat

- orbis-reframe, a 360 video editor for the Insta360 X5
