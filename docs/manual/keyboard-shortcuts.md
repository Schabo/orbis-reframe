# Keyboard shortcuts

All of orbis-reframe's keyboard shortcuts and mouse actions on one page.
In the program, **Help ▸ Keyboard shortcuts** (`F1`) shows the keyboard
shortcuts.

## Playback and moving the playhead

| Key | Does |
|---|---|
| `Space` | Play / pause. |
| `←` / `→` | One frame back / forward (pauses playback). |
| `Shift+←` / `Shift+→` | One second back / forward. |
| `↑` / `↓` | To the previous / next cut or clip edge. |
| `Shift+↑` / `Shift+↓` | To the previous / next reframe keyframe, on any clip. With an overlay clip selected: to its previous / next keyframe instead. |
| `Home` / `End` | To the start / end of the timeline. |

## Editing

| Key | Does |
|---|---|
| `[` / `]` | Trim: make the clip start / end at the playhead. Works on the selected audio or overlay clip, otherwise on the video clip. |
| `R` | Restore: give the clip back its full length. Works on the selected audio or overlay clip, otherwise on the video clip. |
| `S` | Split at the playhead: the selected audio or overlay clip, otherwise the video clip. |
| `Delete` or `Backspace` | Delete the selected clip. With no clip selected: delete the reframe keyframe at the playhead. |
| `Esc` | Clear the selection. While drawing a box for Track subject or Blur a plate: cancel it. |
| `Ctrl+Z` | Undo. |
| `Ctrl+Shift+Z` | Redo. |

## Reframing

| Key | Does |
|---|---|
| `A` | Arm or disarm reframe authoring: while it's on, every drag or zoom in the viewport sets a keyframe. |
| `K` | Set a reframe keyframe at the playhead, with the current view. With an overlay clip selected: set an overlay keyframe instead, with its current position, size and opacity. |
| `Shift+K` | Delete the reframe keyframe at the playhead. With an overlay clip selected: delete its keyframe at the playhead instead. |
| `Ctrl+Shift+K` | Delete all reframe keyframes of the clip under the playhead. With an overlay clip selected: delete all its keyframes instead. |
| `T` | Track subject: draw a box around the subject. Press again to cancel. While tracking runs, `T`, `Space` or `Esc` stops it. |

## Privacy

| Key | Does |
|---|---|
| `B` | Blur a plate: draw a box around it. Press again to cancel, or to stop following the plate. |

## Timeline view

| Key | Does |
|---|---|
| `Ctrl++` (or `Ctrl+=`) | Zoom in. |
| `Ctrl+-` | Zoom out. |
| `Ctrl+0` | Zoom to fit the whole timeline. |

## Settings panels

| Key | Does |
|---|---|
| `Alt+1` … `Alt+9` | Show a settings panel, in the order of the rail from top to bottom: Project, Stitching, Reframe, Stabilization, Audio, Colour, Privacy, Overlay, Media information. Press again to hide it. |

## Files

| Key | Does |
|---|---|
| `Ctrl+O` | Open a project. |
| `Ctrl+S` | Save the project. |
| `Ctrl+Shift+S` | Save the project under another name. |
| `Ctrl+Shift+A` | Add a clip to the timeline. |
| `Ctrl+Shift+M` | Add an audio file. |
| `Ctrl+Shift+V` | Add a video overlay. |
| `Ctrl+E` | Export the video. |
| `Ctrl+Shift+P` | Save a screenshot of the frame at the playhead. |
| `Ctrl+Q` | Quit. |
| `F1` | Show the keyboard shortcuts. |

When orbis-reframe asks whether to save unsaved changes, `S` saves, `D`
discards them and `Esc` cancels. In the window for choosing where to save a
file, `Alt+S` saves (`Enter` does too).

## Mouse: viewport

| Action | Does |
|---|---|
| Drag | Look around. With reframe authoring armed, sets a keyframe when you let go. |
| Mouse wheel | Zoom in / out (field of view). With reframe authoring armed, sets a keyframe. |
| Click an overlay | Select it. |
| Drag an overlay | Move it. |
| Right-click a privacy blur | Remove it (or, with **Show regions** on, restore a removed one). |

## Mouse: timeline

| Action | Does |
|---|---|
| Click or drag on the ruler | Move the playhead / scrub. |
| Click a clip | Select it. |
| Drag a clip | Move it: reorder on the video track; move in time or to another track on audio and overlay tracks. |
| Drag a clip's edge | Trim it. |
| Right-click a clip | Its menu: split, speed, insert, move, remove, link, put its sound on an audio track. |
| Right-click an empty spot | Add clips, audio or overlays there; add, rename, move or remove tracks; zoom. |
| Click **M** / **H** in a track header | Mute an audio track (or the camera sound, on the video track) / hide an overlay track. |
| Double-click a track name | Rename the track. |
| Drag a track header | Change the order of tracks. |
| `Ctrl` + mouse wheel | Zoom in / out around the pointer. |
| Mouse wheel | Scroll along the timeline, while zoomed in. |
| Drop files from the file manager | Add `.insv` clips at the nearest cut, or audio files and flat video overlays where you drop them (those need a clip on the video track first). |

## When typing in a field

While the cursor is in a number or text field of a settings panel, keys
like `Space`, the arrow keys, `Delete` and the letters go to the field, not
to the shortcuts above. Press `Enter` or `Esc`, or click somewhere else, to
leave the field. Shortcuts with `Ctrl` or `Alt` work in fields too.
