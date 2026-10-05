# MiniFASNet anti-spoofing (face presentation-attack detection)

Two small CNNs from Minivision's Silent-Face-Anti-Spoofing, run by
`services/identity` (`AntiSpoofEngine`, onnxruntime, CPU) on every face login
and every registration. The files are runtime artifacts: they are downloaded by
`services/identity/scripts/provision.sh` (`.part` file, SHA-256 check, atomic
move) and are not in Git; only this card and the NOTICE are.

| File | Bytes | SHA-256 | Crop scale | Input |
|---|---|---|---|---|
| `MiniFASNetV2.onnx` | 1743581 | `b32929adc2d9c34b9486f8c4c7bc97c1b69bc0ea9befefc380e4faae4e463907` | 2.7 | 1x3x80x80 BGR float 0-255 |
| `MiniFASNetV1SE.onnx` | 1742335 | `ebab7f90c7833fbccd46d3a555410e78d969db5438e169b6524be444862b3676` | 4.0 | 1x3x80x80 BGR float 0-255 |

Output: three logits; softmax index 1 is "real", 0 and 2 are the two attack
classes. The service averages the real probability of both models (the upstream
fusion) and accepts at `[face] liveness_threshold` (default 0.80). The same
SHA-256 pins live in `services/identity/src/shared/services/face/anti-spoof.hxx`
and are verified again at load: a file that does not match is not loaded, and
while `[face] liveness_required` is true (the default) face login and
registration are refused (fail closed).

## Source and license

- Weights: <https://github.com/minivision-ai/Silent-Face-Anti-Spoofing>
  (Apache-2.0), `resources/anti_spoof_models/2.7_80x80_MiniFASNetV2.pth`
  (SHA-256 `a5eb02e1843f19b5386b953cc4c9f011c3f985d0ee2bb9819eea9a142099bec0`)
  and `4_0_0_80x80_MiniFASNetV1SE.pth`
  (`84ee1d37d96894d5e82de5a57df044ef80a58be2b218b5ed7cdfd875ec2f5990`).
- ONNX export: release `weights` of
  <https://github.com/yakhyo/face-anti-spoofing> (Apache-2.0), opset 17,
  exported with PyTorch 2.8.0 and dynamic batch.

## How the export was checked

No conversion was done here. The export was verified against the upstream
weights without PyTorch:

1. The republished `.pth` files of the ONNX release were compared tensor by
   tensor with the upstream legacy `.pth` files (both read with a plain pickle
   reader): 334/334 and 370/370 tensors, identical in order, shape and value
   (max abs difference 0.0); only the key names differ.
2. The ONNX graphs fold the batch norms into the convolutions, so a verbatim
   weight comparison does not apply; instead both ONNX files were run through
   the upstream pipeline (`CropImage`, the upstream RetinaFace Caffe detector,
   summed softmax) on the upstream sample images: `image_F1.jpg` fake
   (real 0.07), `image_F2.jpg` fake (0.18), `image_T1.jpg` real (0.994), the
   verdicts the upstream repository publishes.

## Measured here (onnxruntime 1.24, CPU)

| Image | Real probability |
|---|---|
| `tests/fixtures/face` NASA portraits (5 detected faces) | 0.883 - 0.9999 |
| the same portraits as a photo held up in a white frame on a plain background | 0.00001 - 0.024 |

The default 0.80 keeps every genuine fixture and refuses every framed print.
This is a small check, not an evaluation: the upstream model was trained on
Minivision's own data, and no public benchmark was reproduced here. Re-measure
on real phone selfies and recaptured attacks before changing the threshold.

## What it does and does not stop

It is a passive, single-frame check. It catches printed photos and photos or
videos shown on a screen when the frame, the moire or the reflections are in
the crop. It does not stop 3D masks, high-quality screen replays filling the
whole frame, or an injected camera stream (deepfake injection); the app's active
challenge is the planned second layer (services/identity/CONTEXT.md).

## Republishing

Change the files only together with the pins in `anti-spoof.hxx`,
`services/identity/scripts/provision.sh`, this card and the NOTICE.
