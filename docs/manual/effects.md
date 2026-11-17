# Effects

Effects change the look of a clip's picture after its colour. For now
there are two, the **Vignette**, which makes the picture darker (or lighter)
toward its edges and draws the eye to the middle, and **Grain**, the
look of film, and a **colour look**: **Black & White**, or a LUT of your
own. More are planned.

![A clip with a vignette, the Effects tab, the clip's Effects section, and the ✦ on clips with an effect](images/effects.jpg)

1. A clip with a vignette: the corners darker.
2. The media sidebar's **Effects** tab, with the effects to put on a clip,
   and under them the **Colour looks**.
3. The **Effects** section in the settings sidebar's **Video** tab: the
   clip's effects and their settings.
4. A ✦ before a clip's name on the timeline: it has an effect on.

## Putting an effect on a clip

Open the media sidebar's **Effects** tab
(<img src="images/icons/tab-effects.png" width="24" alt="">), then:

- drag an effect (**Vignette**, **Grain**, a colour look) onto a clip on
  the video track: the clip it would go on is outlined; or
- double-click it (or select it and press `Enter`, or right-click it and
  choose **Add to the clip at the playhead**) to put it on the selected
  clip, or else the clip under the playhead.

The clip is then selected and the settings sidebar shows its **Effects**
section, in the **Video** tab
(<img src="images/icons/tab-video.png" width="24" alt="">), under
**Colour**. A clip has each effect once: putting one on a clip that has it
already leaves it as it is. A clip has one colour look too, but a new one
replaces the one it has (see [Colour look](#colour-look)).

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

### Colour look

A colour look gives the clip a style: it changes every colour into
another one, after the clip's [colour](colour.md) settings. It is a LUT,
like the one in the **Colour** section, but for a different job: the LUT
in **Colour** turns the camera's picture into normal colours (a log or
HDR clip needs it), the colour look then styles those normal colours. A
clip can have both. LUTs made as looks expect normal (Rec.709) colours,
so a clip filmed in log needs its camera LUT in **Colour** first.

The **Colour looks** in the **Effects** tab:

- **Black & White** turns the clip black and white, the way film does
  through a yellow filter: blue (a sky) comes out a little darker than
  in a plain grey version, so clouds and the horizon stand out, and the
  picture gets a little more contrast without losing the brightest and
  darkest parts. It's more than **Saturation** at −100 % in **Colour**,
  which treats every colour the same.
- **Custom LUT** asks for a `.cube` file (or the `.zip` a LUT pack comes
  in, as **Load LUT…** in **Colour** does) and makes it the clip's look.

| Setting | What it does | Range |
|---|---|---|
| **Look** | Which look: **Black & White**, a LUT file the project has, or **Load LUT…** to pick another file. | |
| **Strength** | How much of the look: 100 % is the look itself, 0 % the clip without it. 50 % of **Black & White** gives faded colours. | 0 % to 100 % (new: 100 %) |

A clip has one colour look. Dropping another one on it replaces it; to
combine two LUTs, make one `.cube` of them in a program that can. A LUT
file you pick is copied into the project's `looks/` folder (see
[Projects and export](projects-and-export.md)), apart from the **Colour**
section's LUTs in `luts/`, so each list offers only its own kind. As in
**Colour**, a project that hasn't been saved yet uses the file where it
is until you save it. If a clip's look file is missing, the clip is shown
without it, and the **Effects** section says why under **Look**.

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

### Grain

| Setting | What it does | Range |
|---|---|---|
| **Amount** | How strong the grain is. It is strongest in the midtones: black and white stay clean. | 0 % to 100 % (new: 25 %) |
| **Size** | How big a grain is: at 100 % one pixel of a 1080p video. | 50 % to 300 % (new: 100 %) |
| **Colour** | At 0 % the grain is grey, the same in red, green and blue. Toward 100 % each colour gets its own grain. | 0 % to 100 % (new: 0 %) |

The grain is new on every frame, so it moves when the video plays. A
paused frame keeps its grain. Its size goes with the video's size, not
its pixels: a 4K export has the same grain as a 1080p one, just in more
detail. When a clip runs slower than its own frame rate (slow motion, or a
25 fps clip in a 50 fps project), a frame shown twice keeps its grain.

The viewport draws the grain at its own size, one grain value per screen
pixel at most. When the viewport is smaller than the export, the grain
there looks coarser than in the export, at the export's strength.

Colour grain is softer in an exported video than in the viewport: the
video stores colour at half the resolution of brightness. Grey grain keeps
its full detail.

## Effects and other edits

- Effects come after the [colour](colour.md), in this order: the colour
  look, then the vignette darkens the picture, then the grain goes on top.
- The halves of a split clip and a duplicated clip keep its effects.
- A transition blends the two clips with their effects.
- [Track subject](reframing.md#track-subject) and the number-plate scan
  ([Privacy blur](privacy.md)) look at the picture without effects.

A project with an effect on a clip can't be opened by orbis-reframe
versions before effects. Versions with effects but without colour looks
(0.10) open it and leave the colour looks out.
