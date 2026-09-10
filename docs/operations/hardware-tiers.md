# Hardware tiers

The same build must run well from a 2-core laptop to a 64-core GPU server, so
resources are sized at runtime.

## Probe

`scripts/detect-hardware.sh` writes `scripts/.hw-profile` with CPU model,
cores/threads, RAM/swap, GPU vendor/model/driver, video acceleration and
Vulkan/CUDA/glslc availability, and derives a capability tier (`minimal`,
`low`, `balanced`, `high`).

## ThreadBudget

`packages/argus-common/src/shared/wrapper/thread-budget/` is the single
source of thread counts: `computeThreads`, `batchThreads`, `heavyThreads`,
`lightThreads`, `inferenceSlots`. AI services never hardcode thread counts;
LLM decode uses `lightThreads`, prefill uses `batchThreads`, and ncnn face
inference is bounded by `inferenceSlots`.

## Accelerators

- `ncnn` builds with `NCNN_VULKAN=ON`: Vulkan runs when a device exists and
  falls back to CPU otherwise. The camera object detector keeps a
  Vulkan-to-CPU path; the face detector can run CPU-only when the tier
  chooses it.
- `llama.cpp` prefers CUDA when the toolkit is present, then Vulkan when the
  loader plus `glslc` and SPIRV-Headers are available, then CPU. Missing GPU
  support degrades, it never fails the build.
- `[llm] gpu_layers` and `[vision] gpu_layers` override automatic offload
  (`-1` auto, `0` CPU, `N` layers).
- TTS diffusion steps are clamped by the detected tier; `tts.steps_cap` pins
  the ceiling for services that cannot probe Vulkan.
