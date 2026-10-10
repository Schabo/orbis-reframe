# VitTrack (ONNX)

The single-object tracker that scores subject tracking (`src/track/SubjectTracker`,
"Track subject" in the Reframe panel): OpenCV CSRT follows the subject and
VitTrack checks, frame by frame, that it is still the same subject. Run with
OpenCV's `cv::TrackerVit` (opencv_dnn), no ONNX Runtime needed.

- Source: OpenCV Zoo, `models/object_tracking_vittrack/object_tracking_vittrack_2023sep.onnx`
  (<https://github.com/opencv/opencv_zoo>, mirrored at
  <https://huggingface.co/opencv/object_tracking_vittrack>), renamed to
  `vittrack.onnx`, otherwise unchanged (sha256
  `2990f0b7cd44d92afa48cd97db6de7be113fc1d9594fddb74e2725c10478e91d`, 714 726 bytes).
- Licensed under Apache-2.0 (`LICENSE-APACHE`), like the rest of OpenCV Zoo's
  VitTrack model.
- `tools/track_subject --tracker nano` also takes NanoTrack v2
  (`nanotrack_backbone_sim.onnx`, `nanotrack_head_sim.onnx` from
  <https://github.com/HonglinChu/SiamTrackers>, Apache-2.0) from `--model-dir`;
  not shipped, the app doesn't use it.
