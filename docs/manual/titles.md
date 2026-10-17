# Titles

A title is text shown over your video: the name of the ride, a place, a
date. Titles sit on overlay tracks, like picture-in-picture videos
([Overlays](overlays.md)), and are placed, keyframed and faded the same way.

![A title in the viewport, the Titles tab, the Title tab with the title's settings, and the title on the timeline](images/titles.jpg)

1. The title in the viewport. Drag it to move it.
2. The media sidebar's **Titles** tab.
3. The settings sidebar's **Title** tab, with the selected title's
   text, font and colour, and below them its position, size and timing.
4. The title on its overlay track.

## Adding a title

Open the media sidebar's **Titles** tab
(<img src="images/icons/tab-titles.png" width="24" alt="">), then:

- drag **Text** onto the timeline, onto an overlay track or anywhere under
  the tracks; or
- double-click **Text** (or select it and press `Enter`, or right-click it
  and choose **Add to timeline**) to add it at the playhead.

A new title says *Title*, in white bold letters in the middle of the
picture, and lasts 5 seconds. It goes onto the overlay track you dropped it
on if that track is free there; otherwise onto the first overlay track that
is, or a new one. The title is selected and the settings sidebar shows its
**Title** tab (<img src="images/icons/tab-title.png" width="24" alt="">),
ready to type the text. There must be a clip on the video
track first.

## Text, font and colour

Select the title on the timeline, or click it in the viewport. The settings
sidebar's **Title** tab then shows its settings, in two sections. If the
**Overlay** tab is showing, it switches to the **Title** tab by itself (and
back again when you select an overlay video). The **Text** section:

| Setting | What it does |
|---|---|
| **Text** | The title's text. `Enter` starts a new line; the lines are centred under each other. The title changes as you type. |
| **Font** | The typeface. Every font installed on your computer is listed. |
| **Style** | **Bold** and **Italic**; click to switch them on or off. |
| **Colour** | Click to choose the text's colour. |

On the timeline, a title is labelled with its first line.

## Position, size, keyframes and fades

The Title tab's **Position and timing** section. These work as for an
overlay video ([Overlays](overlays.md)):

- **Position X** and **Position Y**: where the title's centre is, as a
  percentage of the picture's width and height. Or drag the title in the
  viewport.
- **Text size**: how tall the letters are, as a percentage of the picture's
  height. 8 % is a good size for a heading; the box around the text grows
  with the text, so typing more doesn't make the letters smaller.
- **Opacity**: 100 % is solid.
- **Keyframes**: with **Set key** (`K`) a title can move, grow, shrink or
  fade during its clip, exactly like an overlay video
  ([Moving an overlay during a clip](overlays.md#moving-an-overlay-during-a-clip)).
  Its text, font and colour stay the same for the whole clip.
- **Fade in** / **Fade out** and **Start**: as for an overlay.
- **Duration**: how long the title shows, in seconds. Its start stays put
  and its end moves.

A title has no corners or border settings, and no sound.

To make a title longer or shorter, set its **Duration**, or drag its ends
on the timeline. Trimming,
splitting, moving it to another overlay track, hiding its track and the
layer order all work as for an overlay clip ([The timeline](timeline.md#audio-and-overlay-tracks),
[Several overlays](overlays.md#several-overlays)).

Titles appear in playback, screenshots and the export exactly as in the
viewport. The text is drawn at the export's resolution, so it stays sharp
in a 4K export.
