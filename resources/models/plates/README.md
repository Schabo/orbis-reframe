# Number-plate detector (ncnn)

The detector behind the privacy blur's plate scan (`src/privacy/PlateScanner`,
"Scan clip" in the Privacy panel): YOLOv9-s trained on licence plates at
608 x 608, run by ncnn on Vulkan.

- Source: open-image-models
  (<https://github.com/ankandrew/open-image-models>),
  `yolo-v9-s-608-license-plates-end2end.onnx` from its `assets` release
  (sha256 `2b878b38d9aa07b6ddc3ea75c4ffcb39869bc5c218e0a14002f60ab2f7b0be9a`).
- Converted for ncnn and cut before the model's own box decoding, whose
  anchor grid is baked in for 608 x 608 -- so the network takes any input
  size (the scan runs all 12 tiles as one 2560 x 1920 mosaic):
  `onnx.utils.extract_model(src, dst, ['images'], ['/model/model.22/Concat_output_0',
  '/model/model.22/Concat_1_output_0', '/model/model.22/Concat_2_output_0'])`,
  then `pnnx dst.onnx "inputshape=[1,3,608,608]"` (nothing left in the graph
  depends on the size). Input blob `in0` (RGB, 0..1); outputs `out0`..`out2`
  = the strides 8 / 16 / 32 heads, 65 channels each (4 sides x 16 DFL bins,
  then the class logit); `PlateScanner` decodes the boxes (DFL expectation,
  anchor at the cell centre) and does the NMS. fp16 weights.
  - `yolo_v9_s_608_license_plates_heads.ncnn.param` sha256
    `e82e01c55731292241cee84df1407a6b6de96ff6a8c4d52cac6cb808794ffb98`
  - `yolo_v9_s_608_license_plates_heads.ncnn.bin` sha256
    `b2b57a0fc6e2581390f444535e11afcc99a0a4e9d0dff944d5326815b732b090`
- Licensed under MIT (`LICENSE-MIT`), like the rest of open-image-models.
- Why this model and not the t-640 one (measured 2026-10-01/02, 12 tiles a
  frame): no false hits on a motorcycle clip where t-640 made 6 tracks on the
  rider's sleeves and gloves, and it finds the bike's plate more often
  (69/75 and 40/42 frames vs 57/64 and 29/39). It's slower on the CPU (ONNX
  Runtime 400 vs 150 ms a frame) but close on the GPU (4.6 vs 3.9 ms a tile
  with ONNX Runtime CUDA, 4.75 vs 3.9 ms with ncnn Vulkan fp16).
