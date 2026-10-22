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

### 1. Wide views and "tiny earth"

One per-keyframe **distortion** amount `d` from 0 to 1 -- the projection
centre moves from the sphere's centre (`d = 0`, today's rectilinear) to its
back (`d = 1`, stereographic). It blends between keyframes like the field of
view, so a move can go from a normal view into a wide fisheye look or a
tiny-earth planet. The field of view becomes the **diagonal** angle (corner
to corner), so landscape and portrait frame alike. By default `d` follows it
(0 up to about 90° diagonal, rising to 1 around 170°) with a manual override
per keyframe; the field-of-view limit then depends on `d`. Old projects load
with the automatic `d`, so their wide views lose the stretch. It is a change
to all the view implementations and the subject tracker's crop together
(`architecture.md`, *One view, several implementations*). Until then the
rectilinear field of view stays capped at 110°: wider rectilinear just
stretches.

### 2. Insta360 X6

Built on top of the common camera model below, from a contributor's work:
the X6's own facts are its row in the Insta360 reader's table
(`insv::InsvModel`) and fields its reader reads from the file. The X6 is
close to the X5: the same `.insv`
trailer, larger lenses (6K lens size 3008²), 8K 50, and 10-bit I-Log and
Dolby Vision modes. What it needs beyond the X5 (what's left needs real X6
clips):

- done: the focal reference (field 27) read from the file, the X6's row in
  the model table (6K named in Media information and the media pool, no
  lens-guard profiles), the calibration sanity ranges checked on X6 clips,
  10-bit clips decoded and stitched (P010) with the stream's own colour
  matrix and range on every decode path, HLG clips (the Dolby Vision mode)
  recognised from their stream with a shipped HLG -> Rec.709 LUT
  (`tools/make_hlg_lut`; colour conversion stays the clip's LUT, see
  `architecture.md`);
- the IMU axis map confirmed on a clip with deliberate rotations (the X5's
  is used until then);
- the Dolby Vision mode and the HLG LUT's highlight roll-off checked on
  real X6 clips (built on stand-ins);
- real-time decode at 8K 50 checked.

### 3. Common camera model -- *maintainer*

Before more cameras are added, the camera-specific code is pulled behind one
description of a clip, so each new camera is a reader, not a set of branches
through the pipeline.

- **Built, step 1** (a pure refactor on the X5; stitched frames, the
  stabilizer's output and an NVENC export byte-identical before and after on
  the X5 sample clips): `camera::ClipMetadata` behind one cached
  `camera::ReadClipMetadata(path)` in `src/camera/` -- model / firmware /
  serial, projection kind, the lenses with their calibration and which file
  and stream each comes from, colour and log profile, lens-guard profiles,
  capture-mode name, the proxy file, telemetry facts -- and on demand the IMU
  (`ReadClipImu`, an `insv::ImuTrack`: samples, axis map, video <-> IMU
  timing, readout) and the preview picture (`ReadClipThumbnail`). One reader
  per camera family, asked in turn (`Insta360Reader`); Insta360's per-model
  differences are a table (`insv::InsvModel`: IMU axis map, lens-guard
  profile names, capture-mode names). The stitch, sync, seam, stabilizer,
  export, plate scan, thumbnails, the Media information panel and the media
  pool's mode names read clips only through it; `GyroStabilizer` no longer
  knows the `.insv` timing.
- In progress: (2) the ONE X2 as the first real second camera -- two files
  per take, one per lens. Built: the picture sources open each lens from its
  own `LensSource`, and the Insta360 reader pairs the take's files, reads the
  40-token calibration and the ONE X2's ms telemetry; the take is one clip
  in the media pool and the project (either file names it, both are saved
  and checked for missing media); its media card shows a preview stitched
  from the lens pictures in the file; the IMU axis map and the sync are
  measured on its clips. Next: a check against Insta360 Studio's stitch.
  Then (3) DJI and
  GoPro readers only once real clips are available: an in-camera cubemap
  (`Projection`) behind its own `IStitchSource`, an orientation track for
  cameras that record one, readout time per mode.

New camera support keeps its differences in data -- the reader's per-model
table, fields read from the file -- not in model checks spread through the
code.

### 4. Optional

- **360 clip as an inset**: a second 360 clip's own view as picture in
  picture.
- **Seam quality**: a flow-guided blend inside the seam band, possibly
  export-only, and a finer lens calibration for the small offset left on
  distant content.
- **`src/insv/` as its own repository**, once its API settles.

## Other cameras -- ongoing

Not in the order above: each camera is taken up when there are sample clips
or file facts to build on, so this runs alongside the rest.

Each needs sample clips first ([Sample clips wanted](formats/README.md#sample-clips-wanted)),
and goes in as a reader behind the common camera model. What's known about
each is in its document in [`formats/`](formats/README.md).

- **Insta360 X4**: probably close to working; confirm the calibration string,
  IMU flag and scaling, and axis map.
- **Insta360 ONE X2 / X3**: two files per take, one per lens -- the clip is
  the pair everywhere (adding, the project file, missing media, the `.lrv`).
  Calibration from the 40-token `offset_v3` string (the ONE X2 has no
  56-token one); telemetry in ms; the IMU axis map per model.
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

- **Effects library**: a library of video and audio effects in the left
  sidebar's **Effects** tab, dragged onto a clip and adjusted in the right
  sidebar. Starting with a few simple ones: video -- vignette, sharpen, film
  grain, colour presets (reusing the LUT code); audio -- EQ, compressor, the
  existing noise filters as presets. The split screen layouts (side by
  side, stacked, grid) join the library as **Layouts** to drop on a clip:
  a drop sets the clip's split, as its **Split screen** section does. Split
  screen isn't an effect -- it is several views into the sphere, not a
  filter on the flat picture -- so it keeps its own section and its place in
  the clip. To settle before building: video effects run on the flat
  picture after the colour grade; how effects stack per clip and are stored
  in the project; what preview costs.
- **LUT download**: a "Get the LUT" link next to the Colour panel's log
  prompt that opens the camera maker's download page, and Load LUT taking the
  downloaded zip directly. The makers' LUTs carry no licence, so they aren't
  bundled.
- **Keyframes**: hold keys, a smooth curve through all keys (no overshoot on
  the field of view), a default easing for new keys.
- **Transitions**: more types (dip to a colour, wipes, a move through the
  sphere), dragging the edges to set the length, an equal-power sound
  crossfade, transitions on overlay tracks.
- **Titles**: outline, shadow, background box, alignment, ready-made styles,
  simple text animations.
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
- **Lens colour matching**: evening out the two lenses' exposure and white
  balance across the seam. A correction between the lenses in the stitch,
  not a grade; it changes stitched output, so it comes with a measurement.
- **Direction lock, less drift**: Direction lock holds its heading from the
  gyroscope alone, which drifts 8-16° a minute on an X5. The gyro's bias
  can be measured where the camera stands still (it is stable per camera),
  and the heading corrected from the GPS course where a clip has one.
- **ProRes export** (maybe DNxHR) for handing off to other editors, beside
  H.264 / H.265.
- **GPS and stats overlays**: speed, altitude, distance, heading, the route,
  lean / G-force gauges, drawn from the clip's own telemetry; no map tiles
  (local-only).
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
- **AppImage**: one portable build for every distribution, the libraries
  that differ between them bundled, the NVIDIA driver from the system. To
  research first: GPU decode and NVENC from the bundle on several
  distributions, size. The bundled FFmpeg would be our own build: LGPL only (no
  `--enable-gpl`: the export uses hardware encoders, not x264 / x265), never
  `--enable-nonfree` (NVENC doesn't need it), shared libraries, and its
  exact source and configure line published with each release. The
  per-distribution packages stay, on the system's FFmpeg and its security
  updates.

## Ruled out

Not wanted, unless something changes a lot:

- Full 360 (equirect) export for VR / YouTube 360.
- Colour conversion built into the stitch or decode (it's the clip's LUT).
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
