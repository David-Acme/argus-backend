# argus-tts — CONTEXT

## Why the tts service exists

F4-2 of the `migracion-microservicios` plan extracts the TTS engine out of
the legacy monolith (Rulings BG-BJ). `argus-tts` is a sibling service with
its own binary, own CMake preset and zero HTTP exposure: it exists so the
legacy's voice session and camera talk stop linking the engine in-process
and instead call it over the internal wire.

## What it owns

- **The onnxruntime TTS engine** (shared-tree `TtsService`, compiled into
  this binary via the F2-1 PORTED pattern), loaded at boot from the shared
  `models/tts` tree (`tts.models_dir`; the build symlinks `../models`
  next to the binary). Boot aborts if the engine fails to load — the
  service is useless without its capacity.
- **The internal wire (Ruling BH)**:
  - `POST /tts/v1/synthesize` → binary float32 PCM,
    `audio/x-argus-pcm-f32` + `X-Argus-Sample-Rate`.
  - `POST /tts/v1/synthesize-stream` → chunked float32 PCM with the same
    headers, one HTTP chunk per engine chunk; synthesis runs on a detached
    producer thread that pushes through the async stream.
  - `GET /tts/v1/config` → `{sampleRate, defaultSpeed, loaded}` so the
    legacy adapters can honor the engine's configured speed without a local
    engine (additive beyond Ruling BH's two endpoints — needed by the
    camera-control adapter that wraps defaultSpeed + sampleRate +
    synthesize).
  - Request: `{text, style_id?, lang?, speed?}` validated through the
    shared DSL; errors use the frozen `{status, info, errors}` envelope
    (422 validation, 503 `TTS_NOT_LOADED`).
  - Trust model: no auth, loopback bind by default — internal-network only,
    never routed through the gateway.
- **Config**: `[tts]` (engine knobs, mirroring the legacy block) +
  `[server]` (loopback listener, default 7029) only. No database, no NATS,
  no JWT/device keys.

## What it did NOT change

- The app móvil never talks to this service; `/camera/{id}/talk` keeps
  legacy ownership (Ruling BC) and the gateway routing is untouched.
- Model artifacts stay in the shared `models/tts` paths — never copied.
- The legacy `TtsService` stays linked in the legacy binary: only the boot
  init is gated behind `tts.remote_url` (Ruling BI), so the symbol proof
  for the legacy is unchanged by this task.

## Compose volume

The docker compose must mount the shared `models/` tree (at least
`models/tts`) into this service's working directory — the engine reads
`models/tts/onnx` and `models/tts/voice_styles` relative to `tts.models_dir`.
