# Reframing

A 360° clip records everything around the camera. Reframing chooses what
your flat video shows: where the virtual camera looks at each moment, and
how wide its view is. You do this with **reframe keyframes**: you set the
view at a few moments, and orbis-reframe moves the camera smoothly from one
to the next. **Track subject** can also set the keyframes for you, by
following a person or vehicle.

![Setting a keyframe: reframe authoring armed, the Reframe section, and the keyframes on the clip](images/reframing.jpg)

1. The edit toolbar, with **Arm reframe authoring** on: the button
   outlined in orange. While it's on, every look-around in the viewport
   sets a keyframe.
2. The **Reframe** section: the view at the playhead as numbers, and
   **Track subject…**.
3. The clip's keyframes: the white diamonds along the bottom of the clip.

## The view

The view has four settings. You can see and type them in the **Reframe**
section (settings sidebar, **Video** tab):

| Setting | What it is | Range |
|---|---|---|
| **Yaw** | Left / right: the compass direction the camera looks in. 0° is straight ahead of the camera. | −180° to 180° |
| **Pitch** | Up / down. 0° is level, 90° straight up. | −89° to 89° |
| **Roll** | Tilts the picture, like tilting your head. Usually 0°. | −180° to 180° |
| **Field of view** | How much the view takes in, from top to bottom. Small values zoom in, large values give a wide view. | 20° to 110° |

In the viewport, dragging changes yaw and pitch, and the mouse wheel changes
the field of view. Roll can only be set in the section. The project's aspect
ratio (**File ▸ Project settings…**) decides how wide the picture is for a given field
of view. Changing the aspect ratio later keeps your keyframes; only the
edges of the picture change.

## Keyframes

A keyframe stores the view (yaw, pitch, roll and field of view) at one
moment of a clip. Between two keyframes the camera turns and zooms from one
view to the other, the short way round, at the pace the first keyframe's
**Easing** sets (see [Easing](#easing)). Before a clip's first keyframe the
view stays as at that keyframe, and after the last keyframe it stays as at
the last one.

Each clip has its own keyframes, stored with the clip. They move with the
clip when you reorder or trim it, and a split clip's halves both keep the
keyframes that fall inside them.

> **A clip without keyframes is exported looking straight ahead** (yaw 0°,
> pitch 0°, field of view 75°), whatever you looked at in the viewport. Give
> every clip at least one keyframe. A clip without any shows a hollow
> diamond (◇) before its name on the timeline, the **Reframe** section warns
> while the playhead is in it, and the export dialog lists it.

### Setting keyframes

The simplest way, for a clip that should look in one direction all the
time:

1. Move the playhead into the clip.
2. Drag in the viewport until the view is right, and zoom with the mouse
   wheel.
3. Press `K`. This sets a keyframe with the current view at the playhead.

To make the camera move during a clip, set more keyframes:

1. Click **Arm reframe authoring** in the edit toolbar, or press `A`.
2. Move the playhead to where a move should start, and drag the view into
   place. When you let go of the mouse button, a keyframe is set at the
   playhead. Turning the mouse wheel sets one too.
3. Move the playhead to where the move should end, and drag the view into
   its new place. A second keyframe is set.
4. Play the clip to check the move. Add keyframes in between to shape it.
5. Click **Arm reframe authoring** again (or press `A`) to turn it off, so that looking
   around no longer changes your keyframes.

Pause before you set keyframes: they go in at the frame the playhead is on.
If there's already a keyframe at that frame, its view is replaced instead
of a second keyframe being added.

You can also type exact values into the **Reframe** section. With reframe
authoring armed, each value you enter sets a keyframe at the playhead, the
same as a drag. Without it, the values only change what the viewport shows.

### Looking around without changing anything

Without reframe authoring armed, you can drag in the viewport freely to
look around, for example to find your next subject. Your keyframes don't
change. Once the clip plays or you move the playhead, the view goes back to
following the keyframes.

### Changing and deleting keyframes

- **Go to a keyframe:** `Shift+↓` and `Shift+↑` move the playhead to the
  next and previous keyframe, across all clips.
- **Change a keyframe:** move the playhead onto it, and set the view again
  (with reframe authoring armed, or by pressing `K`). The keyframe's view is
  replaced.
- **Delete a keyframe:** move the playhead onto it, and press `Shift+K` or
  click **Delete keyframe** in the edit toolbar. The playhead doesn't have
  to be on the exact frame: a keyframe within 0.15 seconds is deleted.
  `Delete` does the same when no clip is selected.
- **Delete all keyframes of a clip:** move the playhead into the clip, and
  click **Delete all keyframes** in the edit toolbar (`Ctrl+Shift+K`).

**Edit ▸ Undo** (`Ctrl+Z`) undoes keyframe changes like any other edit.

### Easing

**Easing** in the **Reframe** section sets how the camera moves from one
keyframe to the next:

| Easing | The move |
|---|---|
| **Linear** | At an even pace from start to end. It starts and stops abruptly. |
| **Fast to slow** | Starts fast and slows down, coming to rest at the next keyframe. Good for settling on a subject. |
| **Slow to fast** | Starts slowly from rest and speeds up into the next keyframe. Good for leaving a view. |

Each keyframe has its own easing, for the move from it to the next keyframe.
Move the playhead onto a keyframe, or anywhere between it and the next one,
and pick the easing: it applies to that move. The field shows the easing of
the move the playhead is in, and is greyed out before a clip's first
keyframe and from its last keyframe on, where nothing moves. Choosing an
easing doesn't need reframe authoring armed.

New keyframes, and the keyframes **Track subject** sets, are linear. A
keyframe keeps its easing when you change its view.

## Track subject

Track subject follows a person, vehicle or other subject through the clip
and sets the keyframes for you, so the camera keeps it in view.

![Track subject following a car: the green box around it, and the progress in the Reframe section](images/reframing-track.jpg)

1. Move the playhead to where tracking should start. The subject has to be
   visible there.
2. Look around in the viewport until the subject is in view.
3. Click **Track subject…** in the **Reframe** section, or press `T`.
   Playback pauses.
4. Drag a box around the subject in the viewport. Fit it closely: a box
   with a lot of background around the subject is easier to lose. (`Esc`
   cancels.)
5. Tracking starts. The viewport plays the clip, with the camera following
   the subject and a box drawn around it: **green** while the tracker is
   sure it has the subject, **red** while it has lost it. The **Reframe**
   section shows how far it has come.
6. Tracking runs to the end of the clip. To stop earlier, click **Stop
   tracking**, or press `Space`, `Esc` or `T`.

The camera only turns to follow the subject. It keeps the field of view and
roll of the view you drew the box in, so choose the zoom before you start.

When tracking ends, the tracked path is turned into keyframes, from the
start of tracking to the last frame where the subject was seen. They replace
any keyframes the clip had in that stretch. Keyframes before and after it
stay. The playhead is left at the end of the tracked stretch, and the
edit toolbar says how many seconds and keyframes were tracked.

**If the subject is lost.** When the subject is hidden for a while (behind
a truck, a tree, or out of sight), the box turns red. Tracking goes on and
may find the subject again. If it doesn't, stop it: the keyframes run up to
where the subject was last seen, and the playhead waits there. Move the
playhead to where the subject is visible again and track again from there.

The tracked keyframes are ordinary keyframes. You can change or delete any
of them, add your own, or delete them all and try again.

**If Track subject is greyed out.** With a note under the button, the
tracking model is missing from the installation: install orbis-reframe
again. Without a note, the timeline is empty or an export is running.

Tracking works on the stabilized picture, so a shaking camera doesn't throw
it off. It works best with a subject that stands out from its background
and isn't too small in the view.

## Flat videos

A flat video on the video track ([The timeline](timeline.md#flat-videos-on-the-video-track))
isn't reframed: there's nothing around it to look at. With one selected, or
under the playhead, the **Reframe** section has one setting instead of the
view and **Track subject…**:

| Fit | What you see |
|---|---|
| **Whole picture, with bars** | The whole video, with black bars where its shape differs from your video's. The default. |
| **Fill the frame, edges cropped** | The video fills the frame; what sticks out at two sides is cut off. |
