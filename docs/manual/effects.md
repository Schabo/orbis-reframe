# Effects

Effects change the look of a clip's picture after its colour. For now
there is one, the **Vignette**: the picture gets darker (or lighter) toward
its edges, drawing the eye to the middle. More are planned.

![A clip with a vignette, the Effects tab, the clip's Effects section, and the ✦ on clips with an effect](images/effects.jpg)

1. A clip with a vignette: the corners darker.
2. The media sidebar's **Effects** tab, with the effects to put on a clip.
3. The **Effects** section in the settings sidebar's **Video** tab: the
   clip's effects and their settings.
4. A ✦ before a clip's name on the timeline: it has an effect on.

## Putting an effect on a clip

Open the media sidebar's **Effects** tab
(<img src="images/icons/tab-effects.png" width="24" alt="">), then:

- drag **Vignette** onto a clip on the video track: the clip it would go
  on is outlined; or
- double-click it (or select it and press `Enter`, or right-click it and
  choose **Add to the clip at the playhead**) to put it on the selected
  clip, or else the clip under the playhead.

The clip is then selected and the settings sidebar shows its **Effects**
section, in the **Video** tab
(<img src="images/icons/tab-video.png" width="24" alt="">), under
**Colour**. A clip has each effect once: putting one on a clip that has it
already leaves it as it is.

Effects work on 360° clips and on flat videos on the video track, not on
picture-in-picture clips or titles.

## Changing and removing an effect

The **Effects** section shows the effects of the selected clip, or else of
the clip under the playhead. Each effect has a heading with its name, and
its settings under it:

- The tick box by the name turns the effect off without losing its
  settings, to compare the clip with and without it.
- **Remove** takes the effect off the clip.

**Edit ▸ Undo** (`Ctrl+Z`) undoes any of these, like any other edit.

### Vignette

| Setting | What it does | Range |
|---|---|---|
| **Amount** | How much darker the edges get: −100 % turns them black. Above 0 they get lighter instead, 100 % white. | −100 % to 100 % (new: −30 %) |
| **Size** | Where it starts: at 0 % right from the middle of the picture, at 100 % only in the corners. | 0 % to 100 % (new: 50 %) |
| **Softness** | How gradually it comes in: at 0 % a hard edge, at 100 % so gradual that even the corners aren't at the full Amount. | 0 % to 100 % (new: 50 %) |

The vignette follows the shape of the video, an oval that reaches the
corners. It belongs to the whole video picture:

- In a [split screen](reframing.md#split-screen), the corners of the
  video get darker, not the corners of each pane. The divider between the
  panes stays its own colour.
- A flat video with bars at its sides gets darker toward the corners of
  the video too; the bars stay black.

## Effects and other edits

- Effects come after the [colour](colour.md): the vignette darkens the
  graded picture.
- The halves of a split clip and a duplicated clip keep its effects.
- A transition blends the two clips with their effects.
- [Track subject](reframing.md#track-subject) and the number-plate scan
  ([Privacy blur](privacy.md)) look at the picture without effects.

A project with an effect on a clip can't be opened by orbis-reframe
versions before effects.
