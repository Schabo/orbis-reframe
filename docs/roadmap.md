# Roadmap

What's planned for orbis-reframe and how each piece is meant to work. This is
the intended direction, not a promise of dates. It is here so that work
toward one of these items builds the planned design instead of a different
one next to it.

**Before you start on anything here, open an issue** (or comment on the
existing one) with a short plan, so we can agree on the approach and make
sure nobody else is on it. Items marked *maintainer* are being done by the
maintainer; help is welcome on everything else, after that issue. (An AI
agent writes the plan for its person to post, and posts nothing itself;
see [`AGENTS.md`](../AGENTS.md).)

The rules every change keeps are in [`architecture.md`](architecture.md).

## Next, in order

### 1. Titles -- *maintainer*

Titles on overlay tracks are built: text with a font picker that previews
the fonts, keyframed position and size, and a look -- an outline, a drop
shadow and a background box, line and letter spacing, and left / centre /
right alignment, which also says which point of the text the title's
position pins. All of it is part of the title's style (`title::Style`),
drawn into the one picture the viewport and the export both place
(`src/ui/TitleRender`).

Next, in this order:

1. **Ready-made styles**: more tiles in the left sidebar's Titles tab
   beside *Text* -- a lower third, a big heading, date / place, each a few
   looks. A tile is only where a title starts (a style and a position);
   once on the timeline it is an ordinary title, nothing ties it to the
   tile.
2. **Animations**: tiles for animated titles (type-on, slide in) and an
   *Animation* section in the Title tab to give any title one. A slide is
   the title's own position keys; type-on and keyframed colour change the
   picture over time, so the title's picture becomes one per moment
   (cached), still the same for the viewport and the export.

When non-text items arrive (the GPS gauges below), the Titles tab gets a
broader name.

### 2. GPS and stats overlays -- *maintainer*

Speed, altitude, distance, heading, the route, and lean / G-force gauges,
drawn on the picture from the clip's own telemetry and following the
clip's time (speed changes and trims included). The X5 records GPS in its
`.insv` trailer when a GPS source was connected, about once a second and
sometimes only for part of the clip ([`formats/insta360-x5.md`](formats/insta360-x5.md));
lean comes from the gyroscope. No map tiles: the route is a drawn line,
editing stays local-only. The overlays are saved in the project with their
style and position, and the viewport and the export draw the same. The
design (clips without GPS, gaps and the 1 Hz rate, units, gauge styles) is
still to be planned.

### 3. Optional

- **360 clip as an inset**: a second 360 clip's own view as picture in
  picture.
- **Seam quality**: a flow-guided blend inside the seam band, possibly
  export-only, where the depth-aware seam's cut still leaves something
  doubled (very close things the lenses see against different
  backgrounds).
- **`src/insv/` as its own repository**, once its API settles.

## Other cameras -- ongoing

Not in the order above: each camera is taken up when there are sample clips
or file facts to build on, so this runs alongside the rest.

Each needs sample clips first ([Sample clips wanted](formats/README.md#sample-clips-wanted)),
and goes in as a reader behind the common camera model. What's known about
each is in its document in [`formats/`](formats/README.md).

- **Insta360 X6**: built, experimental, from a contributor's work on the
  common camera model: the X6's facts are its row in the Insta360 reader's
  table (`insv::InsvModel`) and fields read from the file (the focal
  reference, field 27); 6K named, 10-bit clips decoded and stitched with
  the stream's own colour matrix and range, HLG (Dolby Vision mode) clips
  recognised with a shipped HLG -> Rec.709 LUT. Left, all needing real X6
  clips: the IMU axis map confirmed on a clip with deliberate rotations
  (the X5's is used until then); the Dolby Vision mode and the HLG LUT's
  highlight roll-off; real-time decode at 8K 50; and **lens guards** -- the
  X6 has Standard and Premium guards, but the clips we have carry no
  lens-guard profiles, so X6 clips are stitched as bare lenses until a
  clip filmed with each guard shows what the file records
  ([`formats/insta360-x6.md`](formats/insta360-x6.md)).
- **Insta360 X4**: probably close to working; confirm the calibration string,
  IMU flag and scaling, and axis map.
- **Insta360 ONE X2**: built, experimental (step 2 of the common camera
  model). Left: the stitch checked against Insta360 Studio's on the same
  frame, and the IMU axis map confirmed on a clip with deliberate turns.
- **Insta360 X3**: two files per take like the ONE X2, so its own row in the
  Insta360 reader's table; see what calibration it carries.
- **Antigravity A1**: a different trailer layout, 180° lenses (thin seam
  overlap), the drone body in the picture.
- **DJI Osmo 360**: a DJI reader, an equidistant lens model beside the
  current one, D-Log M through a LUT, IMU timing fitted over packets.
- **GoPro MAX / MAX2**: stitched in the camera into a cubemap over two
  tracks, so a cubemap picture source behind `IStitchSource` instead of the
  fisheye stitch; orientation from GoPro's GPMF metadata.
- **Insta360 Luna**: a flat camera, so the flat-video path; needs the
  10-bit / log handling there, through LUTs.
- Not planned: the ONE X / ONE R / ONE RS.

## Planned, not scheduled

- **Common camera model** -- waiting for clips: the camera-specific code
  sits behind one description of a clip, so each new camera is a reader, not
  a set of branches through the pipeline. Its next step needs real clips
  from the cameras it is for.
  - Built, step 1 (a pure refactor on the X5; stitched frames, the
    stabilizer's output and an NVENC export byte-identical before and after
    on the X5 sample clips): `camera::ClipMetadata` behind one cached
    `camera::ReadClipMetadata(path)` in `src/camera/` -- model / firmware /
    serial, projection kind, the lenses with their calibration and which
    file and stream each comes from, colour and log profile, lens-guard
    profiles, capture-mode name, the proxy file, telemetry facts -- and on
    demand the IMU (`ReadClipImu`, an `insv::ImuTrack`: samples, axis map,
    video <-> IMU timing, readout) and the preview picture
    (`ReadClipThumbnail`). One reader per camera family, asked in turn
    (`Insta360Reader`); Insta360's per-model differences are a table
    (`insv::InsvModel`: IMU axis map, lens-guard profile names,
    capture-mode names). The stitch, sync, seam, stabilizer, export, plate
    scan, thumbnails, the Media information panel and the media pool's mode
    names read clips only through it; `GyroStabilizer` no longer knows the
    `.insv` timing.
  - Built, step 2: the ONE X2 as the first real second camera -- two files
    per take, one per lens (experimental, see *Other cameras*). The picture
    sources open each lens from its own `LensSource`, and the Insta360
    reader pairs the take's files, reads the 40-token calibration and the
    ONE X2's ms telemetry; the take is one clip in the media pool and the
    project (either file names it, both are saved and checked for missing
    media); its media card shows a preview stitched from the lens pictures
    in the file; the IMU axis map and the sync are measured on its clips.
  - Step 3, once real clips are available: DJI and GoPro readers -- an
    in-camera cubemap (`Projection`) behind its own `IStitchSource`, an
    orientation track for cameras that record one, readout time per mode.

  New camera support keeps its differences in data -- the reader's
  per-model table, fields read from the file -- not in model checks spread
  through the code.

- **More view shapes**, on top of the per-keyframe distortion (built): a
  globe (the sphere as a ball seen from outside: the distortion `d` past 1,
  up to orthographic), a
  natural view (Panini: the same distortion applied only across, so
  verticals stay straight), and, if the wide end of `d` isn't enough, a true
  fisheye and a mirror ball. Each is a continuous amount per keyframe that
  is the normal view at its rest value, so it blends between keys like the
  field of view -- not a list of projection modes in the project file. The
  math goes into the shared view-projection files, like `d`.

- **Effects library**: a library of video and audio effects in the left
  sidebar's **Effects** tab, dragged onto a clip and adjusted in the right
  sidebar. Starting with a few simple ones: video -- vignette, sharpen, film
  grain, colour presets (reusing the LUT code); audio -- EQ, compressor, the
  existing noise filters as presets. Lens filters are video effects too: a
  **computed ND** filter -- the motion blur a longer shutter would give,
  with a strength in stops and ND16 / ND32 / ND64 presets that multiply the
  exposure time the camera recorded per frame (up to one frame interval),
  blurred along the stabilized view's motion (from the gyro and the
  keyframes) and the scene's (optical flow, with the view's motion alone as
  the fallback); a **graduated ND** that darkens the sky against the
  horizon on the sphere, so it stays on the sky however the view is
  reframed; diffusion / mist and a star filter. The split screen layouts
  (side by side, stacked, grid) join the library as **Layouts** to drop
  on a clip: a drop sets the clip's split, as its **Split screen** section
  does. Split screen isn't an effect -- it is several views into the
  sphere, not a filter on the flat picture -- so it keeps its own section
  and its place in the clip. To settle before building: video effects run on the flat
  picture after the colour grade, a few (graduated ND) on the sphere before
  the reframe; how effects stack per clip and are stored in the project;
  what preview costs (the viewport shows the same blur as the export).
- **Keyframes**: hold keys, a smooth curve through all keys (no overshoot on
  the field of view), a default easing for new keys.
- **Transitions**: more types (dip to a colour, wipes, a move through the
  sphere), dragging the edges to set the length, an equal-power sound
  crossfade, transitions on overlay tracks.
- **Track subject**: automatic zoom; finding a lost subject again.
- **Smaller project files**: privacy regions dominate the file; store fewer
  frames per region (those interpolation reproduces) and fewer numbers per
  frame, still JSON, with a reader for the old format.
- **Royalty-free music library**: an opt-in panel that searches a
  royalty-free catalogue and downloads tracks as ordinary local files -- the
  one allowed network feature.
- **Batch export** / render queue.
- **360 photos**: `.insp` photos as stills on the timeline, reframed with
  keyframes like a clip, and exported as a flat photo; later DNG and HDR
  brackets.
- **Speed ramps**: speed keyframed inside a clip instead of one speed per
  clip. Every conversion between timeline and source time follows the
  curve: playback, export, keyframes, overlays, privacy regions,
  stabilization.
- **Direction lock, less drift**: Direction lock holds its heading from the
  gyroscope alone, which drifts 8-16° a minute on an X5. The gyro's bias
  can be measured where the camera stands still (it is stable per camera),
  and the heading corrected from the GPS course where a clip has one.
- **ProRes export** (maybe DNxHR) for handing off to other editors, beside
  H.264 / H.265.
- **Split screen, angles picked automatically**: split screen is built (a
  clip's picture in 2 or 4 panes, each with its own keyframes, placed by
  `pane::rects` in `src/core/PaneLayout.h` everywhere a view is drawn).
  Next, panes that choose their own direction -- e.g. one following a
  subject found in the clip -- written as ordinary pane keyframes, so they
  stay editable.
- **Shooting modes**: the camera's special modes recognised and processed
  as such -- timelapse, TimeShift / hyperlapse, bullet time. Sample clips
  wanted.
- **Arch Linux package**: a pacman package on the release, built from a
  PKGBUILD in the repository (no AUR).
- **GUI tests**: `objectName`s on the remaining actions and widgets, and the
  offscreen main-window test grown panel by panel.

## Stretch goals

Bigger, not researched far yet -- talk first.

- **Face blur** (plates are done): next a backward pass in the plate scan,
  then faces, then an automatic mask for the camera's own vehicle.
- **Object blur**: blur anything the user draws a box on, followed like
  "Blur a plate…"; things fixed to the vehicle (speedometer, phone) stay put
  on the sphere with no tracking.
- **HDR export**: keep HLG / PQ through to a 10-bit HDR export, beside the
  SDR route. The 10-bit SDR export (HEVC Main 10) is the base it builds
  on; it still needs HDR levels kept through the pipeline, HDR tagging and
  metadata, and an HDR-aware Colour panel; HDR on screen depends on the
  compositor.
- **Video noise removal**, **AI quick edit** (a first cut made
  automatically, as an ordinary editable timeline).
- **AMD and Intel**: best-effort today; testing on real cards is welcome.
- **Windows build**: cross-compiled with MinGW-w64, with the same bundled
  FFmpeg as the AppImage.


## Ruled out

Not wanted, unless something changes a lot:

- Full 360 (equirect) export for VR / YouTube 360.
- Colour conversion built into the stitch or decode (it's the clip's LUT).
- Shipping or downloading camera makers' LUTs: they carry no licence. Users
  download the maker's zip and load it as it is.
- X11 / XWayland.
- Flatpak.
- Network access outside the opt-in music library.
- Translational stabilization by depth and view synthesis.
- Thumbnails along the video track, backward tracking in Track subject --
  unless users ask.

## Good places to help

- **Sample clips** from cameras other than the X5 (see
  [Sample clips wanted](formats/README.md#sample-clips-wanted)).
- **Testing on AMD and Intel GPUs**, with the diagnostics from Help ▸ About.
- **Tests**: offscreen tests for panels that have none, `objectName`s where
  they're missing.
