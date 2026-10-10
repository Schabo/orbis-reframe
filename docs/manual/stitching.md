# Stitching

The X5 films with two fisheye lenses, back to back. Each one sees a little
more than half of the world. Stitching joins the two pictures into one
360° sphere. orbis-reframe does this itself, frame by frame, while it plays
and exports: there is no separate stitching step, and you don't need
Insta360 Studio.

The line where the two pictures meet is the **seam**. It runs all the way
round the camera, through both sides of it (left and right of the lenses),
over the top and under the bottom. Usually you won't see it. It can show
when something is very close to the camera, or when the settings below
don't match how the footage was filmed.

<img src="images/stitching-panel.jpg" width="350" alt="The Stitching panel">

The settings are in the **Stitching** panel. They're the same for the whole
project, and apply to playback, screenshots and the export alike.

## Lens guards

Set **Lens guards** to what was fitted over the lenses when you filmed:

- **None**: bare lenses.
- **Standard lens guards**
- **Premium lens guards**

Lens guards bend the light slightly, so each choice uses a different lens
model, recorded in the `.insv` file by the camera. The camera doesn't record
whether guards were fitted, so you have to tell orbis-reframe. With the
wrong choice the two pictures don't line up: things along the seam show up
twice, or have a piece missing.

New projects start with **Premium lens guards**. If you filmed without
guards, or with the standard ones, change this first.

## Depth-aware seam

The two lenses sit a few centimetres apart, so they see things close to the
camera from slightly different places, just like your two eyes do. Far away
things line up at the seam, but close ones (the handlebars, the rider, the
stick) don't, and show up twice or broken where the lenses meet.

With **Depth-aware seam** on, orbis-reframe works out for every frame how
far away the things along the seam are, and lines the two pictures up at
that distance.

![The handlebar at the seam, with depth-aware seam off and on](images/stitching-depth-seam.jpg)

*The handlebar crosses the seam. Off, the bar is broken and the brake cable
shows twice; on, they join up.*

It's on in new projects, and there's rarely a reason to turn it off.

## Seam alignment

The X5's two lenses are never mounted quite perfectly, and the lens model in
the file doesn't capture every small difference. orbis-reframe measures how
the two lenses line up from the video itself, once for each clip, and uses
that for a cleaner seam. Like the gyro sync (see
[Stabilization](stabilization.md)), this runs in the background while
playback is paused, and the result is saved in the project.

The **Stitching** panel shows the state for the clip under the playhead:

| Message | Meaning |
|---|---|
| *Seam: measuring this clip…* | The measurement is running. |
| *Seam: aligned for this clip (back lens yaw …, pitch …, roll …).* | Done. The numbers are how far the back lens is turned to line up with the front one, in degrees; a fraction of a degree is normal. |
| *Seam: couldn't align this clip (…).* | The measurement didn't succeed; the brackets say why. The clip is stitched with the camera's lens model only. |

Changing **Lens guards** measures every clip again, because the alignment
depends on the lens model.

## Preview size and Quality

- **Preview size** almost never matters. The viewport normally draws
  straight from the two lens pictures at full detail. Only when the graphics
  card or driver can't do that does orbis-reframe stitch a flat 360° preview
  first, and this sets its size. **Auto** uses the clip's full detail, as
  much as can play smoothly at its frame rate.
- **Quality** has only one choice for now, **Standard**. **Optical flow**,
  a smoother seam for close objects, isn't available yet.

## Filming tips

- **Keep important things off the seam.** Point one lens at what matters
  most (the road ahead, the person you're filming). The seam then runs
  through the camera's sides, where close objects are least likely to be.
- **Keep close things away from the camera's sides.** A hand, a handlebar
  or a wall near the side of the camera crosses the seam. Depth-aware seam
  helps, but the further away, the cleaner the join.
- **A selfie stick straight below the camera mostly disappears**: it falls
  in the narrow gap between what the two lenses see. Keep the stick in line
  with the camera body so it stays in that gap.
