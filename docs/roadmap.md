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

1. **Interval photos as a timelapse** (*maintainer*), the GPS left: an
   Interval series of `.insp` 360 photos (the X5's: a JPEG with both
   fisheyes, the lens calibration and a moment of IMU in the trailer, GPS
   in the Exif). Built: the series opens as one clip -- a clip in parts,
   one photo per part, the parts opened as playback reaches them, read
   ahead on several threads -- and plays, stabilizes and exports like the
   camera's own timelapse video. The clip's speed sets the pace. Each
   photo is levelled by the gravity the camera stores with it (its own
   estimate, the vehicle's acceleration already out), so Horizon lock and
   Lean follow work; the photos hold no heading (no gyro, and the GPS a
   single fix on the samples), so Direction lock doesn't, and says why.
   The photos carry no lens-guard table, so they take the one every X5
   video carries, and lens 1, which the camera places a little differently
   in its photos than in video, has its own measured correction (from one
   series so far; re-fitted when more arrive). Left: the photos' GPS as a
   track for the map and gauges, only where its fixes change. The first
   step of *360 photos* below.

Everything else planned is under *Planned, not scheduled* below.

## Other cameras -- ongoing

Not in the order above: each camera is taken up when there are sample clips
or file facts to build on, so this runs alongside the rest.

Each needs sample clips first ([Sample clips wanted](formats/README.md#sample-clips-wanted)),
and goes in as a reader behind the common camera model. What's known about
each is in its document in [`formats/`](formats/README.md).

- **Insta360 X6**: built, experimental, from a contributor's work on the
  common camera model: the X6's facts are its row in the Insta360 reader's
  table (`orbiscore::InsvModel`) and fields read from the file (the focal
  reference, field 27); 6K named, 10-bit clips decoded and stitched with
  the stream's own colour matrix and range, HLG (Dolby Vision mode) clips
  recognised with a shipped HLG -> Rec.709 LUT; a contributor's clip with
  deliberate turns confirmed the IMU axes as the X5's, and a Dolby Vision
  clip exported through the HLG LUT. Left: the same checks on clips of our
  own; the HLG LUT's highlight roll-off; real-time decode at 8K 50 (a
  contributor measured far less than the X5's 8K 30 reaches, not explained
  yet); and **lens guards** -- the X6 has Standard and Premium guards, but
  the clips we have carry no lens-guard profiles, so X6 clips are stitched
  as bare lenses until a clip filmed with each guard shows what the file
  records ([`formats/insta360-x6.md`](formats/insta360-x6.md)).
- **Insta360 X4**: probably close to working; confirm the calibration string,
  IMU flag and scaling, and axis map.
- **Insta360 X4 Air**: one owner's log and one 6K clip of our own. It
  opens, untested, on the general row of the Insta360 reader's table (what
  the measured rows agree on, none of the X5's own corrections) and
  stitches, but the lenses are ~1.4° apart at the seam, which nothing in
  the file explains yet; the X5's IMU axes and timing hold; a split
  recording's second part is refused. Needs its own row (6K lenses, like
  the X6), the seam's cause (an 8K clip is the next reference), and how a
  lens swap shows in the file
  ([`formats/insta360-x4.md`](formats/insta360-x4.md), with the X4).
- **Insta360 ONE X2**: built, experimental (step 2 of the common camera
  model). Left: the stitch checked against Insta360 Studio's on the same
  frame, and the IMU axis map confirmed on a clip with deliberate turns.
- **Insta360 X3**: two files per take like the ONE X2, so its own row in the
  Insta360 reader's table; see what calibration it carries.
- **Antigravity A1**: built, experimental, from a contributor's 8K clips:
  the trailer's slotted layout read, its own row in the Insta360 reader's
  table (names, proxy, focal reference, how the camera holds its lenses --
  down and up -- so the picture is level and faces the drone's nose), and
  the seam measured (the lens extrinsics' order, the 71 mm baseline, ~200°
  lenses, no drone parts in the stitched picture), the IMU axis map, and
  Horizon lock in flight leveled by the drone's own attitude track (the
  accelerometer also reads the drone's acceleration; step 3's orientation
  track, see *Common camera model*), its own flat preview
  picture on the cards, clips with no audio track, and its GPS lined up
  with the video by matching the GPS speed against the attitude record's
  (the file has no first-frame Unix time), and Direction lock's heading
  held by the attitude's, without drift. A clip in each 5.2K and 4K 100
  mode, in H.264, works as 8K does, with the same calibration and seam
  correction, and 4K 100 plays at its rate; a renamed clip finds its
  recording time and preview file by the name it holds
  ([`formats/antigravity-a1.md`](formats/antigravity-a1.md)).
- **DJI Avata 360**: built, experimental, from a contributor's clips
  (1920² per lens at 100 fps, 3000² and 3840² at 24-60 fps, Normal and
  D-Log M;
  [`formats/dji-360.md`](formats/dji-360.md)): the lens pose is data the
  camera's reader fills in (`orbiscore::LensPose`) instead of Insta360's fixed
  sensor clock and lens yaws (X5 output unchanged); DJI's lens model, a
  θ-polynomial with a tangential pair, beside MEI on every path that
  projects a lens; `camera::DjiReader` -- the schema picked by product
  name, the master lens on track 0, the mount (the nose measured from the
  sun) and usable field of view per product, the cover picture as the
  preview, GPS from the metadata at 10 Hz, D-Log M named for DJI's own LUT
  in the Colour panel; where the drone's body shows in each lens, a mask
  per lens (`orbiscore::LensMask`) the stitch leaves out of the blend and its
  measurements. Chart calibration stays MEI-only and says so.
  Stabilization from the camera's fused attitude (the file has no raw
  gyro): DJI's 4 kHz quaternions as an orientation-only track, read from
  the `.OSV` (the `.LRF` keeps a quarter of them), the IMU's axes and the
  timing measured against the picture's own rotation on six clips.
  The Osmo 360, 360 II and Air follow when their clips can be measured.
- **GoPro MAX / MAX2**: stitched in the camera into a cubemap over two
  tracks, so a cubemap picture source behind `IStitchSource` instead of the
  fisheye stitch; orientation from GoPro's GPMF metadata.
- **Insta360 Luna**: a flat camera, so the flat-video path; needs the
  10-bit / log handling there, through LUTs.
- Not planned: the ONE X / ONE R / ONE RS.

## Planned, not scheduled

- **Video effects** (*maintainer*), more of them: a library of effects in the left
  sidebar's **Effects** tab, dragged onto a clip and adjusted in the
  right sidebar. Built: the library, a clip's effects (`fx::ClipEffects`
  in `src/core/ClipEffects.h`: at most one of each, in the pipeline's
  fixed order, saved per clip) and the first effects, a colour look (a
  creative LUT after the grade: one of the built-in looks (Black & White,
  Deep sky, Teal & orange, Faded film; each a formula sampled into a LUT,
  `src/core/LookPresets.*`), or a `.cube` of the user's,
  copied into the project's `looks/`), sharpen (an unsharp mask on the
  luma, its own pass, `sharpen.comp` and `src/core/Sharpen.h`, as it needs
  the neighbouring pixels), a vignette and film grain, on every
  render path (`clip_effects.glsl` and its C++ twin, right after the
  grade). Lens filters are video
  effects too: a **computed ND** filter -- the motion blur a longer
  shutter would give, with a strength in stops and ND16 / ND32 / ND64
  presets that multiply the exposure time the camera recorded per frame
  (up to one frame interval), blurred along the stabilized view's motion
  (from the gyro and the keyframes) and the scene's (optical flow, with
  the view's motion alone as the fallback); a **graduated ND** that
  darkens the sky against the horizon on the sphere, so it stays on the
  sky however the view is reframed; diffusion / mist and a star filter
  (streaks from bright points). More looks (sepia, retro, bleach bypass,
  cross-process, day-for-night), an HDR look (local contrast and tone
  compression, its own pass; not HDR export), colour pop and split
  toning, a motion trail (neighbouring frames, as the computed ND), speed
  zoom, tilt-shift, lens flare held on the sun's place on the sphere and
  light leaks, chromatic aberration, letterbox bars, stylised effects
  (VHS, glitch, scanlines) and handheld shake on the view.
  The split screen layouts (side by side, stacked, grid) join the library
  as **Layouts** to drop on a clip: a drop sets the clip's split, as its
  **Split screen** section does. Split screen isn't an effect -- it is
  several views into the sphere, not a filter on the flat picture -- so
  it keeps its own section and its place in the clip. To settle when
  they're built: a few effects (graduated ND) run on the sphere before
  the reframe; room for the audio effects in the same tab; what preview
  costs (the viewport shows the same blur as the export).
- **The GPU stitch only** (*maintainer*): the CPU stitch fallback goes. It
  can't play 5.7K / 8K in real time, differs visibly from the GPU export,
  and doubles the work on every stitch and view feature. A graphics card
  with Vulkan is required; decoding on the processor still feeds the GPU
  stitch where the card can't decode a clip's codec. CPU-decoded clips
  then export through the same GPU view pass as the rest, and the GPU tests
  run in CI on Mesa's software Vulkan (lavapipe). When the GPU stitch can't start
  on a clip (not enough graphics memory), it plays from the camera's
  preview file instead, if it has one, rather than showing no picture;
  an export stops with the clip named. Playing from the preview files is
  built (View ▸ Play from preview files, offered when a clip plays
  slowly); switching to them on its own comes with this.
- **`lib/orbis-core/` as its own repository**, once its API settles.
- **Highlight markers, follow-ups**: the camera's marks are built (X5,
  record type 10, [X5 format](formats/insta360-x5.md) §4.3): dots on the
  clip, read from the file and not saved in the project, `Ctrl+↑` / `↓`
  to jump; and so are your own (`M`, `Shift+M` or the dot's menu deletes
  one) and hiding a camera mark per clip, both saved in the project
  (format version 21) and undoable, and the same on a file previewed in
  the Media tab, copied into a clip made from it. Left: what the record's
  other six
  lists hold,
  whether marks made in the phone app reach the file, and other cameras'
  marks (the ONE X2 writes the same record; GoPro's HiLight tags) once
  clips with marks show them.
- **The new stitcher** (*maintainer*) -- built, the default for new clips,
  still marked experimental; follow-ups below. The old stitcher's
  depth-aware seam moves and bends the picture round the seam in playback:
  one stitch distance per stretch of the seam is too coarse, so it is
  ambiguous between frames, which needed the per-clip analysis to settle.
  The new one is built as layers on a template, each one worked out from the
  frame and the frames just before it (a seek measures those first), with no
  background analysis, chosen in the Stitching section's *Stitcher*:
  - **Template** (built): the lens model from the file or the clip's chart
    calibration, the guard profile, a fixed cross-fade; nothing measured from
    the video.
  - **Seam colour blend** (built): the two lenses' colour ratio per stretch
    of the seam, from what both see in the overlap near the seam over each
    frame and the quarter second before it; each lens moved half of it,
    fully near the seam and fading out by 30° from it, so neither lens's
    picture changes further away. With it, a low sun's haze in one lens
    (veiling light), measured per stretch as the old stitcher does, taken
    out of that lens round the seam before the colour is matched. A setting
    of its own.
  - **Seam flow** (built): per frame, optical flow both ways in the overlap,
    averaged over the frame and the four before it; each lens warped by a
    share of it growing to half at the seam, so near things crossing the
    seam join up while the rest keeps the template's geometry. Where the
    strips are bright and featureless (clipped sky, the sun's veil) both get
    the same fixed pattern first, so the flow there stays zero instead of
    arbitrary and doesn't drag the scene beside it round. A setting of its
    own. With it and the seam colour blend, the lenses meet in a narrow
    fade (±0.75°) on the seam instead of the template's cross-fade over
    the whole band, which mixes both lenses at their softest; per stretch
    of the seam the fade widens to the whole band where the lenses still
    differ near the seam after the flow and the colour match (the sun's
    haze and rays in one lens, a near thing the flow can't line up), from
    the frame and the four before it. The flow is Dense Inverse Search (DIS)
    in a compute shader, the same on every GPU (OpenCV's DIS on the CPU
    stitch fallback). NVIDIA's flow hardware (`VK_NV_optical_flow`) is left
    out for now: on strips this small the shader is fast enough, and the
    hardware would make an export depend on the GPU's vendor; worth trying
    only if it measures clearly better.
  - **Projects**: new clips get the new stitcher with its seam colour blend
    and seam flow on; a project that doesn't name a clip's stitcher is from
    before that and keeps the old one, and a clip added to a project takes
    its neighbour's. The old stitcher stays selectable until a decision to
    remove it, measured with `tools/stitch_bench` against Insta360 Studio's
    exports.
  - **Follow-ups**, each measured before anything changes: whether the seam
    flow should join near things (gloves, handlebars) more fully, against
    the sharpness it costs; the CPU stitch fallback's flow, which matches the
    GPU's less well on near things; lens 1's small turn against the
    file's lens data, which Insta360 Studio and the camera's own thumbnail
    both apply on the X5 (the X5's two radial lens models are read
    exchanged since they measured better against Studio); chart
    calibration of the A1 and ONE X2; and references for the cameras other
    than the X5, perhaps the stitched thumbnail each file carries.
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
    demand the IMU (`ReadClipImu`, an `orbiscore::ImuTrack`: samples, axis map,
    video <-> IMU timing, readout), the GPS track and the vehicle's lean and
    G-forces (`ReadClipTelemetry`: an `orbiscore::GpsTrack` and an
    `orbiscore::MotionTrack` in the clip's video time) and the preview picture
    (`ReadClipThumbnail`). One reader per camera family, asked in turn
    (`Insta360Reader`); Insta360's per-model differences are a table
    (`orbiscore::InsvModel`: IMU axis map, lens-guard profile names,
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
  - Step 3, once real clips are available: DJI (built for the Avata 360,
    `camera::DjiReader`, *Other cameras*) and GoPro readers -- an
    in-camera cubemap (`Projection`) behind its own `IStitchSource`, an
    orientation track for cameras that record one, readout time per mode.
    The orientation track is built, from the Antigravity A1:
    `orbiscore::ImuTrack::orientation`, world up and a fixed horizontal
    direction (the flight controller's north) over time in the IMU's axes.
    The stabilizer levels against its up instead of the accelerometer
    wherever the track has it (Horizon lock's anchor, the auto-level), and
    holds the heading to its north, so Direction lock doesn't drift. In
    video the 1 kHz gyro still carries all rotation; a 50 Hz orientation
    isn't smooth enough to stabilize with on its own, as the camera moves
    between its samples. A timelapse is different: each frame needs only
    the orientation across its own readout, and that is what the X5 stores
    there in place of the gyro. A track with an orientation and no gyro
    samples is stabilized from the orientation alone (built for the X5's
    timelapse, then the DJI Avata 360's 4 kHz fused attitude, and the X5's
    Interval photos, up only, one sample a photo; GoPro's
    `CORI` would plug in the same way), each frame placed by its own time
    (`orbiscore::ImuTrack::frameTimes`), since a timelapse's frames needn't be
    evenly spaced.

  New camera support keeps its differences in data -- the reader's
  per-model table, fields read from the file -- not in model checks spread
  through the code.

- **Audio effects**: audio effects in the same **Effects** library as the
  video effects (see *Video effects* above), after them: EQ, compressor, the existing
  noise filters as presets.
- **Keyframes**: hold keys, a smooth curve through all keys (no overshoot on
  the field of view), a default easing for new keys.
- **Move presets**: popular camera moves dropped on a clip as ordinary
  keyframes -- spin, tiny planet out and back, planet spin, tunnel dive,
  barrel roll, planet to globe, look back, drone pull-back, crash zoom,
  pan sweep, tilt reveal, slow push-in, fisheye pop. Full turns need keys
  that can turn more than half way round (intermediate keys first,
  unwrapped angles later).
- **Transitions**: more types -- dip to a colour, wipes (straight, clock,
  iris, barn door), a whip pan, push / slide, zoom blur, spin blur, luma
  fade, blur dissolve, flash and glitch; and 360 ones that move the view
  across the cut: sky swap, ground swap, planet swap, tunnel dive, globe
  roll, portal, spin through, and matching the direction of travel. A 360
  transition is an animated offset on top of each clip's keyframes, not
  keys of its own. Also: J and L cuts and an equal-power crossfade for
  the sound, dragging the edges to set the length, transitions on
  overlay tracks.
- **Track subject**: automatic zoom; finding a lost subject again.
- **Smaller project files**: privacy regions dominate the file; store fewer
  frames per region (those interpolation reproduces) and fewer numbers per
  frame, still JSON, with a reader for the old format.
- **Royalty-free music library**: an opt-in panel that searches a
  royalty-free catalogue and downloads tracks as ordinary local files -- the
  one allowed network feature.
- **Batch export** / render queue.
- **360 clip as an inset**: a second 360 clip's own view as picture in
  picture.
- **360 photos**: `.insp` photos as stills on the timeline, reframed with
  keyframes like a clip, and exported as a flat photo; later DNG and HDR
  brackets. An Interval series as a timelapse comes first (*Next*,
  built but for its GPS).
- **Speed ramps**: speed keyframed inside a clip instead of one speed per
  clip. Every conversion between timeline and source time follows the
  curve: playback, export, keyframes, overlays, privacy regions,
  stabilization.
- **Time freeze**: time stops on one frame while the view keeps moving
  through the frozen sphere along its keyframes; likely the same time
  mapping as speed ramps (speed 0), so designed with them.
- **Reverse / boomerang**: a clip or part played backwards, or forwards
  then back; needs backward decoding of long-GOP footage.
- **Direction lock, less drift**: Direction lock holds its heading from the
  gyroscope alone, which drifts 8-16° a minute on an X5. The gyro's bias
  can be measured where the camera stands still (it is stable per camera),
  and the heading corrected from the GPS course where a clip has one.
- **ProRes export** (maybe DNxHR) for handing off to other editors, beside
  H.264 / H.265.
- **Shooting modes**: the camera's special modes recognised and processed
  as such -- TimeShift / hyperlapse, bullet time, Starlapse. Timelapse is
  built on the X5 and the A1 (the X6's needs a clip to measure its IMU
  axes). Sample clips wanted.
- **HDR export**: keep HLG / PQ through to a 10-bit HDR export, beside the
  SDR route. The 10-bit SDR export (HEVC Main 10) is the base it builds
  on; it still needs HDR levels kept through the pipeline, HDR tagging and
  metadata, and an HDR-aware Colour panel; HDR on screen depends on the
  compositor.
- **Video noise removal**: denoising low-light and evening footage; not
  researched yet -- talk first.
- **GUI tests**: `objectName`s on the remaining actions and widgets, and the
  offscreen main-window test grown panel by panel.

## Stretch goals

Bigger, not researched far yet -- talk first.

- **Face blur** (plates are done): next a backward pass in the plate scan,
  then faces, then an automatic mask for the camera's own vehicle.
- **Object blur**: blur anything the user draws a box on, followed like
  "Blur a plate…"; things fixed to the vehicle (speedometer, phone) stay put
  on the sphere with no tracking.
- **AI quick edit** (a first cut made automatically, as an ordinary
  editable timeline).
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
- Dolly zoom: one viewpoint can't fake the camera moving forward.
- Thumbnails along the video track, backward tracking in Track subject --
  unless users ask.
- Split screen panes that pick their own angles: each pane's keyframes
  and Track subject in a pane already do it.

## Good places to help

- **Sample clips** from cameras other than the X5 (see
  [Sample clips wanted](formats/README.md#sample-clips-wanted)).
- **Testing on AMD and Intel GPUs**, with the diagnostics from Help ▸ About.
- **Tests**: offscreen tests for panels that have none, `objectName`s where
  they're missing.
