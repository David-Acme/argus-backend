# argus-vlm — CONTEXT

## Why the vlm service exists

F4-4 of the `migracion-microservicios` plan extracts the vision engine out
of the legacy monolith (Rulings BO-BR). `argus-vlm` is a sibling service
with its own binary, own CMake preset and zero HTTP exposure. The VLM is a
CAPACITY move: it is registered and loaded at legacy boot but called by
nothing in production, so extracting it removes a llama.cpp+mtmd model from
the legacy's RAM/VRAM with zero functional risk. It mirrors the argus-tts
(F4-2) and argus-stt (F4-3) scaffolds.

## What it owns

- **The vision engine** (shared-tree `VisionService`, compiled into this
  binary via the PORTED pattern): LiquidAI LFM2.5-VL-450M (GGUF Q8_0 +
  mmproj F16) through llama.cpp + `libmtmd`, loaded at boot from the shared
  `models/vision/lfm2vl-25/` tree (`[vision] model_path`/`mmproj_path`; the
  build symlinks `../models` next to the binary). Boot aborts if the engine
  fails to load — the service is useless without its capacity. `VisionService`
  is owned BY VALUE by the controller (the adapter shape — no singleton),
  and takes `ai_init::llamaMutex()` exactly as the legacy vision-service.cc
  does; `argus-vlm`'s main owns its own `llama_backend_init/free` (Ruling
  BR): the process-global llama mutex contention disappears between
  processes by construction.
- **llama.cpp vendor constraint**: the mtmd projector must vendor the SAME
  llama.cpp commit the legacy links (`third_party/llama.cpp`, tag b10305 —
  Conan's llama-cpp recipe ships CPU-only with no mtmd and a projector that
  rejects LFM2.5-VL mmproj files). Within the root build the service links
  the exact targets third_party integrates (`${ARGUS_LLAMA_TARGETS}` =
  llama + mtmd); the standalone preset re-adds the same submodule directory
  with the same cache variables.
- **The internal wire (Ruling BP)**:
  - `POST /vlm/v1/describe` — JSON body `{image_b64, prompt?, camera_id?}`.
    `image_b64` is a base64 JPEG: the in-process API takes a `cv::Mat`, so
    the caller encodes before sending (the proto's `bytes image_jpeg` is
    the semantic reference). `prompt` empty resolves the service's
    configured default (`[vision] prompt`); `camera_id` is caller context,
    logged per request. No streaming (describeMat is synchronous), no auth
    (loopback bind is the trust boundary, never routed through the
    gateway). Response is the frozen app-envelope with `info.caption`
    (`ApiResponse::ok` — every response in this codebase goes through
    ApiResponse). Errors: 400 `BAD_REQUEST` (non-JSON body), 422
    VALIDATION_ERROR (`image_b64` empty/not base64/not decodable, prompt or
    camera_id too long), 503 `VLM_NOT_LOADED`, frozen 404/405 envelopes.
    Latency logged per request (`bytes`, `camera_id`, `ms`).
  - `GET /vlm/v1/config` — `{loaded, maxInputPx, defaultMaxTokens}`
    (additive, internal-only).
- **Caption cache**: stays IN the service (per-process warm state keyed by
  FNV of the scaled pixels + prompt — an extraction WIN: the legacy lost it
  anyway on restart).
- **Config**: `[vision]` (engine knobs, mirroring the legacy block) +
  `[server]` (loopback listener, default 7031) only. No database, no NATS,
  no JWT/device keys — nothing here persists anything.

## Tier note (the F4-2 lesson, applied)

The service compiles `hardware-profile.cc` with `ARGUS_NO_NCNN_GPU=1`, so
its capability tier is derived WITHOUT the ncnn-gated Vulkan probe. The
tier stays >= Low on any reasonable host (the engine still loads), but
`HardwareProbe::vlmGpuLayers()` always resolves 0. When the legacy would
offload to GPU, pin the parity value with `[vision] gpu_layers` (999) in
this service's config — `VisionService::init` reads the same override the
legacy reads.

## What it did NOT change

- The app móvil never talks to this service; no gateway routing, no new
  app-facing contract.
- Model artifacts stay in the shared `models/vision/lfm2vl-25/` paths —
  never copied.
- The legacy `VisionService` stays linked in the legacy binary: only the
  boot registration is gated behind `vision.remote_url` (Ruling BQ), so the
  symbol proof for the legacy is unchanged by this task.
- The IKnownPersonMatcher wiring stays deferred (Ruling BA note) — the
  remote adapter exists so Fase 5's camera matcher can consume either the
  local or the remote adapter through the same registry name.

## Compose volume

The docker compose must mount the shared `models/` tree (at least
`models/vision/lfm2vl-25`) into this service's working directory — the
engine reads the GGUF pair relative to the `[vision]` config keys.