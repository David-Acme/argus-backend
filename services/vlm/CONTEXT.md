# argus-vlm — CONTEXT

## Why the vlm service exists

F4-4 of the `migracion-microservicios` plan extracts the vision engine out
of the legacy monolith (Rulings BO-BR). `argus-vlm` is a sibling service
with its own binary, own CMake preset and no public exposure. The VLM is a
CAPACITY move: it is registered and loaded at legacy boot but called by
nothing in production, so extracting it removes a llama.cpp+mtmd model from
the legacy's RAM/VRAM with zero functional risk. It mirrors the argus-tts
(F4-2) and argus-stt (F4-3) scaffolds.

## What it owns

- **The vision engine** (`VisionService`, the vlm feature's own engine
  compiled into this binary through `argus::vlm`): LiquidAI LFM2.5-VL-450M
  (GGUF Q8_0 + mmproj F16) through llama.cpp + `libmtmd`, loaded at boot from
  the shared `models/vision/lfm2vl-25/` tree (`[vision] model_path` and
  `mmproj_path`; the build symlinks `../models` next to the binary). Boot
  aborts if the engine fails to load — the service is useless without its
  capacity. `VisionService` is owned BY VALUE by the controller (the adapter
  shape — no singleton), and takes `ai_init::llamaMutex()` exactly as the
  legacy vision-service.cc does; `argus-vlm`'s main owns its own
  `llama_backend_init/free` (Ruling BR): the process-global llama mutex
  contention disappears between processes by construction.
- **llama.cpp vendor constraint**: the mtmd projector must vendor the SAME
  llama.cpp commit the legacy links (`third_party/llama.cpp`, tag b10305 —
  Conan's llama-cpp recipe ships CPU-only with no mtmd and a projector that
  rejects LFM2.5-VL mmproj files). The standalone project links the exact
  vendored targets (`llama` and `mtmd`, named in the feature module's
  `DEPENDS`) with the pinned cache variables.
- **The internal wire (Ruling BP)**:
  - `POST /vlm/v1/describe` — JSON body `{image_b64, prompt?, camera_id?}`.
    `image_b64` is a base64 JPEG: the in-process API takes a `cv::Mat`, so
    the caller encodes before sending (the proto's `bytes image_jpeg` is
    the semantic reference). `prompt` empty resolves the service's
    configured default (`[vision] prompt`); `camera_id` is caller context,
    logged per request. No streaming (describeMat is synchronous), no auth
    (loopback bind is the trust boundary, never announced or published).
    Response is the frozen app-envelope with `info.caption`
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
  `[server]` (loopback listener, default 7031) + `[rpc]` (address and caller
  pairs, both empty by default when Phase 4 step 6b added the gRPC leg). No
  database, no NATS, no JWT/device keys — nothing here persists anything.

## The gRPC leg (Phase 4 step 6b)

The service answers `argus.vlm.v1` (`packages/contracts/vlm`: the `Vision`
service, a unary `Capabilities` and a unary `Describe`) beside its HTTP wire,
through `argus::vlm-rpc` (`src/app/rpc/`). It follows the tts and stt
precedents field by field and adds nothing the domain does not force: the
server holds the same `VisionService` the controller drives, reached through
two callbacks — a live `capabilities` read and a `describe` that takes the
decoded `cv::Mat` — so the caption a gRPC caller receives is the caption the
HTTP route would answer, caption cache included, and a shut-down engine
reports `loaded: false` truthfully instead of a boot snapshot. It is composed
in `main.cc` only when `rpc.address` and a non-empty `[rpc.callers]` pair are
set, and nothing in the tree sets either key, or `vlm.grpc_target` on the
caller side: the leg is live and dormant, and the cutover is a later decision.

The engine's inference slots are the face's backpressure: one acquisition from
`ThreadBudget::inferenceSlots()`, `try_acquire` rather than a blocking wait
(rule 13's counting semaphore, never a global mutex), an RAII guard for the
release, and 429 `Busy` for the caller that loses the race.

The deadline ceiling needed one measurement the precedents did not. gRPC
carries a deadline as a relative `grpc-timeout` header and rounds it, so a
server that refuses anything beyond the client's own two-minute ceiling
refuses the client's own maximum at the boundary — measured on the wire, not
inferred: the raw stub's 120 s call was refused while 5 s passed. The ceiling
is therefore `argus::vlm::kMaxTimeout` plus one second, named as the rounding
it is, and a caller that declares no deadline is served.

The service's own copies of the remote vision adapter and its HTTP client
(`feature/vlm/services/remote/`) went with this step: a service does not host
a client of itself, so the caller side is one package — `argus::clients::vlm`,
which now carries both transports behind the `VlmClient` façade the guard
assessment already holds.

## Tier note (the F4-2 lesson, applied)

The service compiles `hardware-profile.cc` with `ARGUS_NO_NCNN_GPU=1`, so
its capability tier is derived WITHOUT the ncnn-gated Vulkan probe. The
tier stays >= Low on any reasonable host (the engine still loads), but
`HardwareProbe::vlmGpuLayers()` always resolves 0. When the legacy would
offload to GPU, pin the parity value with `[vision] gpu_layers` (999) in
this service's config — `VisionService::init` reads the same override the
legacy reads.

## What it did NOT change

- The mobile app never talks to this service; no public routing, no new
  app-facing contract.
- Model artifacts stay in the shared `models/vision/lfm2vl-25/` paths —
  never copied.
- The legacy `VisionService` stays linked in the legacy binary: only the
  boot registration is gated behind `vision.remote_url` (Ruling BQ), so the
  symbol proof for the legacy is unchanged by this task.
- The IKnownPersonMatcher wiring stays deferred (Ruling BA note). The service
  no longer carries a remote adapter of its own — Fase 5's camera matcher
  reaches the wire through `argus::clients::vlm`'s façade, the same one
  `argus-guard` calls, rather than through a service-local registry name.

## Compose volume

The docker compose must mount the shared `models/` tree (at least
`models/vision/lfm2vl-25`) into this service's working directory — the
engine reads the GGUF pair relative to the `[vision]` config keys.

## Phase 4 step 9: config resolution into `src/config/` (D20)

The listener and the optional gRPC leg are resolved by
`src/config/vlm-config.{hxx,cc}` (`argus::vlm-config`):
`VlmConfig::resolveListener()` (`ListenerConfig::resolve(7031)`) and
`VlmConfig::resolveRpc()` (`rpc.address` plus the `rpc.callers` credential
pairs, empty ones dropped). `main.cc` keeps `config.toml` loading,
`drogonConfig` and the boot gate on the resolved address and credentials.

## Owner settings

`src/feature/settings/vlm-settings.cc` is the catalog an owner may change
through `argus.settings.v1.Settings`, registered on the same gRPC listener as
`argus.vlm.v1` (`argus::contracts::settings-wire`).

Live, because they are per-request inputs to the generation and
`main.cc` calls `VisionService::refreshDefaults()` when the registry reports
a change:

- `vision.max_input_px` (basic, 128..1024, fallback 384) — how much detail of
  what the cameras see reaches the model; the main speed/detail knob,
  read by `fitToBudget` on every description.
- `vision.max_tokens` (basic, 8..512, fallback 64) — how long a
  description may be, the default when a caller sends no `max_tokens`.
- `vision.prompt` (advanced, text, fallback "Can you describe this image?")
  — the default question when a caller sends none.
- `vision.caption_cache_slots` (advanced, 1..64, fallback 8).

`refreshDefaults()` takes the service's own `mutex_` (the one `run()` holds
for a whole generation), so a refresh waits for the description in flight
and the next one sees the new values. It also empties and resizes the
caption cache: the cache key is the scaled pixels plus the prompt, and a
caption cached under an older token budget must not answer after the budget
changed. `maxInputPx_` and `defaultMaxTokens_` are atomics because
`Capabilities` and `GET /vlm/v1/config` read them without the lock; both
report the refreshed value on the next call.

Restart, because they are read when the model, context and projector are
built: `vision.image_max_tokens` (0..4096, fallback 0 = the projector's
default), `vision.context_size` (2048..32768, fallback 8192),
`vision.threads` (0..64, fallback 0 = `ThreadBudget`) and
`vision.gpu_layers` (-1..999, fallback -1 = the hardware probe). Model and
projector paths, ports, addresses and callers are never in the catalog.

`resolveVisionDefaults()` and `resolveVisionEngineSettings()` are the one
reading of `[vision]`, so the test compares every fallback with what the
service runs on an empty config. Two code defaults disagreed with the
template and now match it: an absent `max_tokens` used to clamp to 8 (it is
64), and an absent `gpu_layers` used to mean 0 (it is -1, the probe, which
resolves 0 in this build — see the tier note — so behaviour is unchanged).

The `settings` entry of `[rpc.callers]` is the only credential the settings
service accepts, and `VlmConfig::resolveRpc()` removes it from the vision
callers: the settings caller cannot describe and the guard cannot change
settings. `vlm-settings-test` checks both directions on a live listener and
watches `Capabilities.max_input_px` follow a registry update.

## Real-time behaviour (measured 2026-10-03)

Host: Ryzen 7 5825U (8 cores, 16 threads) with its Radeon Vega 8 iGPU
(RADV RENOIR, Vulkan 1.4), prod build, `ThreadBudget::computeThreads()` = 8.
Other agents were building at load average 11–17, so the absolute numbers
are pessimistic. They are medians of three uncached describes over the HTTP
leg, including the base64 upload and the JPEG decode.

| `max_input_px` | backend | "describe in one sentence" | "is there a person?" ("No.") |
|---|---|---|---|
| 256 | CPU | 1.05–1.27 s | 0.79–0.88 s |
| 384 (default) | CPU | 1.35–1.70 s | 1.01–1.61 s |
| 512 | CPU | 1.38–1.98 s | 1.23–2.17 s |
| 384 | Vulkan iGPU (`gpu_layers = 999`) | 0.80–0.98 s | 0.55–0.69 s |

- The image encoder dominates: a one-word answer costs 60–75 % of a full
  sentence. `max_input_px` is the speed knob, as `AGENTS.md` says. 256 px kept
  the same captions on these scenes; 384 stays the default because camera
  frames carry smaller subjects than these photos.
- A cached caption answers in 8–130 ms. That time is the JPEG decode and the
  hash of the scaled pixels, growing with the upload: 0.1 MB → 13 ms,
  1 MB → 115 ms.
- Offloading to the iGPU through Vulkan was 1.6–1.9× faster under that CPU
  load. It also leaves the CPU cores to STT, the LLM and TTS during a call.
  The build already compiles `GGML_VULKAN`. `HardwareProbe::vlmGpuLayers()`
  resolves 0 because this service builds the probe without the GPU check, so
  the offload is a deploy decision: `[vision] gpu_layers = 999`, plus the
  container needs `/dev/dri`. It is not made here.
- The HTTP leg decodes the JPEG on the Drogon loop thread before the
  blocking leg: 5 ms for 720p and 25 ms for 4.5 MP (OpenCV, measured).
  `IMREAD_REDUCED_COLOR_*` would save at most 9 ms of that, so the decode
  stays as it is. The gRPC leg decodes on its own thread.

Fixes in the same pass:
- The caption cache key now includes the token budget. A caption generated
  under `max_tokens = 2` answered a later request for 32 tokens with the same
  two words.
- The decode loop no longer accepts each token twice:
  `llama_sampler_sample` already accepts it. That is harmless for the greedy
  chain, but wrong the day a stateful sampler joins it.
- `visionHashBytes` reads 8-byte words with `memcpy` instead of an
  unaligned `uint64_t*` dereference, which is undefined behaviour.
- `loaded_` is atomic, because `run()` and the capabilities callback read it
  without the engine lock.

### Re-measured after the shared helpers' busy-wait was fixed

Taken at 20:29 at load 6–7 with the same images and prompts, over three
uncached runs. The prod binary predates this pass's fixes, none of which is
on the hot path.

| `max_input_px` | backend | one sentence, median (range) | yes/no, median (range) |
|---|---|---|---|
| 384 | CPU | 1091 ms (980–1659) | 791 ms (685–1339) |
| 256 | CPU | 905 ms (654–970) | 693 ms (613–831) |
| 384 | Vulkan iGPU, `gpu_layers = 999` | 613 ms (500–661) | 379 ms (339–458) |

The earlier table above was taken while four helper processes spun a full
core each, so its CPU rows are 20–40 % high. The iGPU stays 1.8–2.1× faster
than the CPU at 384 px.
