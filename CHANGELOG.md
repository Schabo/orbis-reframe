## v0.9.0 (2026-11-12)

### Feat

- **insv**: print the extended calibration string in insv_trailer_dump
- **ui**: preview a file from the Media tab
- **timeline**: your own highlight marks, and hiding the camera's
- **timeline**: show the camera's highlight marks
- **stitch**: new clips use the new stitcher
- **stab**: X5 timelapses stabilize from the orientation the camera stores

### Fix

- **ui**: name the Overlay tab in the shortcut help
- **build**: the AppImage needs glibc 2.39, not 2.41
- **calib**: put the chart's corners in the stitch's pixel frame
- **calib**: aim the synthetic chart at project() pixel centres
- **export**: the last frame of an export is shown again
- **render**: titles and gauges no longer depend on what was drawn before
- **render**: the viewport draws from the lenses again after a failed import
- **audio**: read only the audio stream's packets from a clip
- **playback**: a crossfade's other 360 clip reads playback's own decode
- **stitch**: a stitch source can be told to decode on the CPU
- **playback**: a frame that fails to read is tried again, not taken as the clip's end
- **render**: the viewport survives having no room for its picture
- **playback**: keep clips on the graphics card only while it has room
- **stitch**: a stitch source can give its graphics memory back while idle
- **stitch**: the new stitcher's lenses meet in a narrow fade, wide where they still differ
- **stitch**: the seam colour blend follows the lenses within a quarter second
- **render**: keep the seam flow texture in GENERAL layout
- **stitch**: keep the seam flow still on bright featureless sky
- **stab**: Horizon lock levels on a swinging camera
- **build**: tell a snapshot.debian.org outage from a missing package
- **stab**: residual_stab reads the exposure record on the camera's clock
- **stab**: take each frame half its exposure before the camera's frame time
- **stab**: take the correction at the frame's exact time, not the rounded ms
- **render**: the equirect reproject wraps its left-edge column properly

### Refactor

- **core**: remove unused code and share duplicated helpers

## v0.8.0 (2026-11-04)

### Feat

- **stab**: slow motion and timelapses follow the time they were filmed in
- **stitch**: the new stitcher takes a low sun's haze out at the seam
- **stitch**: seam_regress measures the new stitcher
- **stitch**: the seam flow on the GPU
- **stitch**: the new stitcher's seam flow
- **stitch**: the camera model's full lens correction, not only a turn
- **stitch**: the camera model's lens 1 turn in the new stitcher
- **stitch**: the new stitcher's seam colour blend
- **stitch**: choose the stitcher per clip, the new one experimental
- **stitch**: calibrate lenses without a project or clip
- **stitch**: calibrate a camera's lenses with a chart on the monitor
- **stitch**: the seam's state from the frames either side, analysed per clip
- **stitch**: match the lenses' colour per clip
- **stab**: hold one direction in Custom stabilization
- **ui**: animate a picture in picture coming on and going off
- **ui**: give the Overlay tab the Overlays library's icon
- **ui**: call the settings sidebar's Title tab Overlay
- **ui**: route gauge seen ahead, heading up
- **ui**: lean dial, G circle and route gauges, {lean} and {g} fields
- **ui**: read clips' GPS and motion off the GUI thread
- **insv**: line the X5's GPS up with the video by the IMU
- **stab**: hold Direction lock's heading by the Antigravity A1's attitude
- **insv**: line the Antigravity A1's GPS up with the video by its speed
- **insv**: show the Antigravity A1's own preview on its cards
- **stab**: level the Antigravity A1 by its flight controller's attitude
- **stab**: measure the Antigravity A1's IMU axis map
- **stitch**: turn each camera's lenses into the picture by its mount
- **insv**: give the Antigravity A1 its own model row

### Fix

- **stitch**: the GPU seam flow tears the picture where its strip wraps round
- **stitch**: a seek measures every frame before it for the seam colour blend
- **stitch**: the depth seam counts only unique matches within its range
- **stitch**: match the lenses' colour where they meet, and follow it
- **stitch**: take a low sun's haze out of one lens at the seam
- **stitch**: match the lenses' colour along the seam, stretch by stretch
- **ui**: one status line for the seam and the lens colour
- **stitch**: match the depth-aware seam on texture, not on glare
- **insv**: find a renamed clip's recording time and proxy
- **playback**: say when the graphics card can't decode a clip's format
- **render**: draw every overlay in the viewport, not five
- **render**: keep a frame's LUTs while it draws
- **ui**: show a route that stays in one place
- **stab**: keep Horizon lock's level out of fast bends and accelerations
- **ui**: keep PlateJobController complete where MainWindow deletes it
- **ui**: say when the selected clip has no sound
- **stitch**: compose the lens extrinsics pitch first, then yaw, then roll
- **insv**: take trailer record ids and sizes from the offsets table
- **test**: make zip LUT fixtures and title checks locale-safe

### Refactor

- **export**: move MainWindow export/screenshot wiring into its own TU
- **ui**: move MainWindow library/open-project wiring into its own TU
- **track**: move MainWindow subject-tracking wiring into its own TU
- **stitch**: move MainWindow stitch/stab wiring into its own TU
- **privacy**: move MainWindow privacy wiring into its own TU

## v0.7.0 (2026-10-28)

### Feat

- **ui**: gauges draw GPS values as a dial, a bar or a heading tape
- **ui**: titles show speed, altitude and heading from the GPS
- **insv**: read the GPS track and its values between fixes
- **ui**: name the tabs Picture in picture and Overlays
- **ui**: date tiles show the date as a field
- **ui**: a title's clock in 24 or 12 hours
- **ui**: titles show the recording time as a running clock
- **ui**: type titles on and off
- **ui**: animated title tiles play while hovered
- **ui**: slide and pop titles in and out
- **ui**: ready-made titles as tiles in the Titles tab
- **ui**: outline, shadow, box, spacing and alignment for titles

### Fix

- **ui**: send FFmpeg's log to the opt-in verbose log
- **ui**: no font size warnings from the title font list
- **export**: say when the video encoder fails, not just its error
- **render**: say when the viewport runs out of graphics memory
- **stitch**: say when a clip decodes or stitches on the CPU

## v0.6.1 (2026-10-25)

### Fix

- **playback**: compare the audio with where the video is now
- **audio**: hold the output clock still while the sound is stopped
- **playback**: seek the audio when the video seek lands, not before
- **insv**: read calibration and LUT numbers regardless of the locale
- **ui**: grey out the Reframe section when there is no clip
- **ui**: grey out the text of disabled widgets
- **ui**: grey out disabled checkboxes and radio buttons

## v0.6.0 (2026-10-24)

### Feat

- **audio**: play sound through miniaudio instead of Qt Multimedia
- **build**: file types for .orp and .insv
- **ui**: Load LUT takes the camera maker's zip
- **render**: Auto eases on to stereographic for tiny planets
- **render**: Auto distortion holds the corners' stretch at 1.5
- **ui**: pick a title's font from a list, the last three on top
- **stitch**: match the lenses' colour along the depth-aware seam
- **stitch**: cut the depth-aware seam where the lenses agree
- **stitch**: fit the lens centre and scale in the seam fine-tune
- **render**: diagonal field of view and per-keyframe distortion
- **ui**: split screen gets its own section in the Video tab
- **ui**: split screen in the viewport and the Reframe section
- **export**: draw a split clip's panes on every render path
- **core**: split screen in the clip model and the project file
- **stab**: Direction lock, a level view that keeps pointing one way

### Fix

- **playback**: a clip without keys starts at its forward view
- **ui**: draw the arrow on drop-down lists
- **timeline**: delete the keyframes a trim or split leaves outside the clip
- **stitch**: keep the depth-aware seam steady between near and far matches
- **ui**: fill every clip section when a project or clip opens
- **stitch**: offer only the lens guards a clip's file has profiles for

### Refactor

- **stitch**: one SeamCorrection for the per-clip seam fine-tune
- **render**: one view projection with a distortion, shared by every path

## v0.5.0 (2026-10-21)

### Feat

- **insv**: give a ONE X2 clip its preview picture
- **core**: keep a ONE X2 clip's second file with it in the project
- **insv**: read the Insta360 ONE X2's two-file 5.7K clips
- **timeline**: click a keyframe diamond to go to it
- **export**: 10-bit export as HEVC Main 10
- **ui**: recognise HLG clips and ship an HLG to Rec.709 LUT
- **insv**: read the X6's focal reference from the file and name its 6K mode
- **stitch**: 10-bit 360 clips (HEVC Main 10)
- **ui**: recognise log clips and ask for their LUT
- **render**: 3D LUTs per clip in the Colour panel

### Fix

- **stab**: turn a ONE X2 clip's gyro the right way
- **ui**: import a ONE X2 take once, from any of its files
- **stitch**: give a failed seam / gyro-sync measurement's reason in words
- **playback**: pick up a media file repaired in place
- **ui**: give each clip its own reason when its measurement fails
- **playback**: leave the path out of every demuxer error
- **playback**: name the file and its clip in the playback error box
- **render**: a transition layer jumps to the frame showing at its time
- **playback**: show the next clip's frame on a step onto a cut
- **timeline**: show a key on a clip's trim-out on its last frame
- **ui**: copy a pasted grade's LUT into the project
- **ui**: list missing files on hidden and muted tracks when a project opens
- **timeline**: no keyframe diamond on a clip's trim-out point
- **playback**: play a missing clip as black and go on past it
- **ui**: name a 360 clip's mode on media cards from its camera's reader
- **ui**: give Export an outgoing arrow instead of the download icon
- **ui**: report a stop at a missing file without a stat per clip switch
- **ui**: say which file and which clip a missing-media error belongs to
- **stitch**: build the ycbcr conversion for the format the decoder produces
- **stitch**: decode lens streams with their own colour matrix and range

### Refactor

- **insv**: decode a clip's lenses from one file or a file each
- **timeline**: one helper for a key's place on the timeline
- **insv**: common camera model -- clips read through one description

### Perf

- **export**: run the Vulkan encoder on the decoders' Vulkan device

## v0.4.0 (2026-10-18)

### Feat

- **export**: skip Vulkan encoders when the device is NVIDIA's
- **export**: try VAAPI before Vulkan video
- **export**: set VBR and quality hints on Vulkan and VA-API encoders
- **export**: ORBIS_ENCODER env var pins the encoder backend
- **export**: probe Vulkan and VA-API encoders when NVENC is absent
- **export**: refuse to export when a source file is missing
- **timeline**: crossfade and dip-to-black transitions at cuts
- **timeline**: keyframe moves start and stop gently
- **ui**: text titles on overlay tracks
- **timeline**: keyframes ease fast to slow or slow to fast
- **timeline**: duplicate a clip from its menu
- **stitch**: lens guards and depth-aware seam per clip
- **timeline**: flat videos on the main track, mixed with 360 clips

### Fix

- **export**: probe each encoder once, off the GUI thread, and say why none works
- **export**: fall back to the next encoder when open fails
- **render**: check the viewport's descriptor set layout, pools and sets
- **build**: link OpenCV 5's features and geometry modules
- **render**: size the stitch descriptor pool from the ycbcr sampler's count
- **render**: build on FFmpeg before 9, name the right queue flag
- **render**: adopt FFmpeg's Vulkan queue with its creation flags
- **insv**: include features2d.hpp for goodFeaturesToTrack
- **export**: missing overlay or sound files block the export too
- **playback**: a cut changes the view and grade on the same frame as the picture
- **ui**: tracking status shows under the Track subject button
- **ui**: a new project starts from a blank view and default settings
- **privacy**: saving removes plate scans no clip uses
- **privacy**: each clip has its own plate scan
- **privacy**: the plate blur shows only on the clip it's on
- **playback**: a flat main-track clip can't be dragged out of place
- **playback**: a flat clip's colour change shows while paused
- **export**: overlay videos decoded at their own size no longer overrun the frame
- **timeline**: draw linked clips' link mark as an icon, not an emoji

### Refactor

- **privacy**: the plate scanner's geometry and quad tracker apart
- **playback**: the decoder thread apart from the controller
- **timeline**: split the timeline widget into layout, paint and input
- **render**: the viewport's renderer apart from its window
- remove dead code found by a whole-build link check
- **render**: stitcher setup apart from its per-frame work
- **ui**: split MainWindow's constructor into its parts
- **timeline**: adding track files out of MainWindow
- **ui**: project saving and its dialogs out of MainWindow
- **ui**: media import and Locate file out of MainWindow
- **export**: render jobs and the export runner out of MainWindow
- **privacy**: the blur's data side out of MainWindow
- **ui**: keyboard shortcut help dialog out of MainWindow
- **ui**: stabilizer cache and clip measurements out of MainWindow

### Perf

- **export**: flat clips export many times faster

## v0.3.0 (2026-10-14)

### Feat

- **timeline**: split the clip at the playhead from the edit toolbar
- **ui**: pick a theme under View ▸ Theme: Ember, Light or Neutral dark
- **core**: autosave unsaved changes to a recovery file
- **ui**: rename and duplicate projects from the project library
- **ui**: notice media files going missing or coming back while the app runs
- **ui**: show each overlay track's layer and move it forward or backward
- **timeline**: fold a video track's sound tracks away under it
- **timeline**: keep unlinked audio in place and own-sound tracks to their source
- **timeline**: tracks in fixed groups, each overlay with its own sound track
- **audio**: an overlay's sound works like a video clip's
- **audio**: Audio tab lists every audio clip of the video clip
- **audio**: Audio tab in three sections
- **audio**: every .insv audio stream on its own track
- **timeline**: edit toolbar above the timeline, centred
- **timeline**: timeline across the full window width
- **ui**: Help ▸ User manual opens the manual on GitHub
- **ui**: player bar under the viewport
- **ui**: settings sidebar in tabs, with collapsible sections
- **ui**: locate a moved or renamed media file
- **ui**: the left sidebar can be resized
- **ui**: left sidebar with the Media tab, the project's media pool
- **ui**: the Ember theme, built from colour tokens
- **ui**: project library start screen and one folder per project
- **insv**: read the camera's stitched thumbnail from the trailer

### Fix

- **ui**: show "1 key" / "3 keys" instead of "key(s)" style plurals
- **ui**: keep the player bar's time moving while Track subject or Blur a plate runs
- **timeline**: keep a folded overlay track folded through undo and redo
- **core**: write a finished sync or seam measurement into the project file
- **ui**: keep a video overlay's frame rate in the media pool
- **timeline**: scroll the tracks while a drag is held at their top or bottom
- **timeline**: name an overlay's track "audio" and indent the mic tracks
- **audio**: move hand-placed mics onto their stream tracks on open
- **core**: opening a project no longer empties the project library
- **ui**: Enter presses a dialog's default button again

### Refactor

- **core**: drop the "micAir" track role alias
- **timeline**: drop the "overlaySound" role alias
- **timeline**: call an overlay's track its audio track in the code too

### Perf

- **ui**: keep the Media tab's thumbnails on disk

## v0.2.0 (2026-10-11)

### Feat

- **timeline**: drop .insv clips from the file manager
- **stab**: set stabilization per clip

### Fix

- **build**: .deb file names without '~', as GitHub serves them

## v0.1.0 (2026-10-10)

### Feat

- orbis-reframe, a 360 video editor for the Insta360 X5
