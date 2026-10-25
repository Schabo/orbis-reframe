# Titles

A title is text shown over your video: the name of the ride, a place, a
date. Titles sit on overlay tracks, like picture-in-picture videos
([Overlays](overlays.md)), and are placed, keyframed and faded the same way.

![A title with a drop shadow in the viewport, the Titles tab, the Title tab with the title's text and look, and the title on the timeline](images/titles.jpg)

1. The title in the viewport. Drag it to move it.
2. The media sidebar's **Titles** tab.
3. The settings sidebar's **Title** tab, with the selected title's
   text, font, colour and alignment, then how its text is drawn (outline,
   shadow, box, spacing), and below them its position, size and timing.
4. The title on its overlay track.

## Adding a title

Open the media sidebar's **Titles** tab
(<img src="images/icons/tab-titles.png" width="24" alt="">). Its tiles are
ready-made titles, each showing roughly how and where it lands in the
picture:

| Tile | What you get |
|---|---|
| **Text** | *Title* in white bold letters in the middle of the picture. |
| **Heading** | A big title in the middle, with a soft shadow. |
| **Heading, outlined** | A big title in spaced-out capitals with a black outline, readable over a bright sky or snow. |
| **Lower third** | Two lines in the lower left on a dark box: a name, and a line about it. |
| **Lower third, bar** | One line in the lower left on a solid coloured bar. |
| **Caption** | Small text at the bottom in the middle, on a rounded box. |
| **Date** | The date in the lower right. |
| **Date and place** | The date and, below it, *Place* to type over, in the lower left. |

To add one:

- drag its tile onto the timeline, onto an overlay track or anywhere under
  the tracks; or
- double-click the tile (or select it and press `Enter`, or right-click it
  and choose **Add to timeline**) to add it at the playhead.

The new title lasts 5 seconds. It goes onto the overlay track you dropped
it on if that track is free there; otherwise onto the first overlay track
that is, or a new one. The title is selected and the settings sidebar shows
its **Title** tab (<img src="images/icons/tab-title.png" width="24" alt="">),
ready to type the text. There must be a clip on the video track first.

The **Date** tiles write the day the clip under the title was recorded, as
your computer writes a date in full (in US English, *Thursday, August 28,
2025*). It comes
from the camera's clock, as the clip's file records it; for a clip whose
file doesn't say, the title says *Date* for you to type over. orbis-reframe
doesn't look up places: type the place yourself.

A tile is only where a title starts. Once added it is an ordinary title:
everything below can be changed, and nothing ties it to its tile.

## Text, font, colour and alignment

Select the title on the timeline, or click it in the viewport. The settings
sidebar's **Title** tab then shows its settings, in three sections. If the
**Overlay** tab is showing, it switches to the **Title** tab by itself (and
back again when you select an overlay video). The **Text** section:

| Setting | What it does |
|---|---|
| **Text** | The title's text. `Enter` starts a new line. The title changes as you type. |
| **Font** | The typeface, picked from a list of every font installed on your computer, each shown in its own face. The three fonts you picked last are at the top. Type a name's first letters to jump to it. A project made on another computer may use a font that isn't installed here: it is listed as *(not installed)*, kept, and drawn with a similar font until you pick another. |
| **Style** | **Bold** and **Italic**; click to switch them on or off. |
| **Colour** | Click to choose the text's colour. |
| **Align** | **Left**, **Centre** or **Right**: how the lines line up under each other, and which point of the title its position is. A left-aligned title's position is its left edge, so the text keeps its start where you put it as you type more, as a caption in a corner should; a right-aligned one keeps its right edge. Changing the alignment doesn't move the text. |

On the timeline, a title is labelled with its first line.

## Outline, shadow, box and spacing

The **Look** section draws more than the letters, to keep a title readable
over a bright sky or a busy road. Sizes are a percentage of the text size,
so the look stays the same when you make the title bigger or smaller.

| Setting | What it does |
|---|---|
| **Line spacing** | The space from one line to the next: 100 % is the font's own, more spreads the lines apart. |
| **Letter spacing** | Space added between the letters; below 0 brings them closer. |
| **Outline** | A line round each letter, in the colour on the button beside it (click it to choose). **Width**: how thick. |
| **Shadow** | A soft shadow below and to the right of the text (and of its outline), in its colour. **Opacity**; **Distance**: how far it falls from the text; **Blur**: how soft its edge is, 0 % for a sharp one. |
| **Box** | A filled box behind the text, in its colour. **Opacity**; **Padding**: the room between the text and the box's edge; **Corners**: how round its corners are, 50 % for round ends. |

Each can be switched on and off with its tick box; switched off, it keeps
its settings for when you switch it on again. With a box, a left- or
right-aligned title's position is the box's edge.

## Position, size, keyframes and fades

The Title tab's **Position and timing** section. These work as for an
overlay video ([Overlays](overlays.md)):

- **Position X** and **Position Y**: where the title is, as a percentage
  of the picture's width and height: across, its left edge, middle or right
  edge, as its **Align** says; down, its middle. Or drag the title in the
  viewport.
- **Text size**: how tall the letters are, as a percentage of the picture's
  height. 8 % is a good size for a heading; the box around the text grows
  with the text, so typing more doesn't make the letters smaller.
- **Opacity**: 100 % is solid.
- **Keyframes**: with **Set key** (`K`) a title can move, grow, shrink or
  fade during its clip, exactly like an overlay video
  ([Moving an overlay during a clip](overlays.md#moving-an-overlay-during-a-clip)).
  Its text and how it's drawn stay the same for the whole clip.
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
