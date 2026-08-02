# CONTEXT.md — Argus Backend project memory

> This file preserves the project's intent, decisions and state so context is
> never lost between sessions. Update it whenever a significant decision is made.

## What Argus is

- **Argus** is a 100% local AI platform for the home. Security ("intelligent
  guard") is the entry point; a local virtual assistant is a secondary capability.
- Philosophy: process everything locally, minimize resources, preserve privacy.
  Cloud only transports packets (tunnel), never processes data.
- AI is invoked **only when it adds value**: events → rule engine → if resolvable,
  execute; else → LLM. This keeps latency/cost low.
- Central abstraction: **Entity** (live object with state), not raw detections.
  The whole backend works with `Entity`.

## Hard constraints / decisions

- **C++20**, Drogon HTTP/WebSocket server. SQLite via Drogon async `DbClient`
  (NO ORM, manual SQL). DB file `database/argus.db`, `number_of_connections: 4`
  (pragmas reapplied at every boot via `DbService::applyPragmas()`).
- **Config**: single `config.toml` file (TOML, parsed via `tomlplusplus`).
  Replaces `.env` + `config.json`. Drogon section converted to JSON at runtime
  and loaded via `loadConfigJson()`. `ConfigService` in `src/shared/services/config-service/`.
- **NO spdlog** — Drogon already provides logging. Do not add spdlog.
- **Smart pointers only** — no raw owning pointers. All service resources use
  `std::unique_ptr` with custom deleters.
- **Services use hardcoded model paths** — all services know their model paths
  internally (no parameters). Paths live at `models/{llm,stt,vision,tts,face}/`.
- **Conan 2** for deps (`conanfile.txt` + `CMakePresets.json`). CMake presets:
  `dev` (Debug) and `prod` (Release), generator Ninja.
- **Git submodules** under `third_party/` for libs that change rarely and we want
  to control: `fastText`, `hnswlib`, `ncnn`, `sherpa-onnx`. Built via
  `add_subdirectory` with `EXCLUDE_FROM_ALL`.
- Convention: Conventional Commits in English. Remote `git@github.com:David-Acme/argus-backend.git`,
  branch `main`.

## Dependency resolution notes (Conan conflicts, learned the hard way)

- `nlohmann_json` pinned to **3.11.3** — jwt-cpp/Drogon use this version.
- `opencv/4.13.0` built **headless**: `with_protobuf=False`, `with_eigen=False`,
  `with_ffmpeg=False`, `with_wayland=False`, `with_gtk=False`, `with_vulkan=False`.
- `eigen/5.0.1` used for tracker/geometry.
- **conanfile.txt cannot resolve version conflicts** (no `override=True`/`force`).
  Conflict resolution is done by pinning versions + disabling the offending option
  in the consuming package.

## Current build state

- Build is **green**: `cmake --preset dev` + `cmake --build --preset dev -j 8`
  succeeds; server starts on `0.0.0.0:7024`.
- **Build script**: `./scripts/setup.sh` handles Conan deps + cmake configure + build.

## Current services (all implemented)

| Service | Engine | Model | Path |
|---------|--------|-------|------|
| `FaceService` | ncnn (Vulkan) | RetinaFace + MobileFaceNet | `models/face/` |
| `FaceDB` | HNSWlib | In-memory 128-dim index | Loaded from SQLite at startup |
| `LlmService` | llama.cpp (Conan) | LFM2.5-1.2B-Instruct-Q4_K_M | `models/llm/` |
| `VisionService` | SmolVLM2 (ONNX int8) | SmolVLM2-500M-Video-Instruct | `models/vision/smolvlm/` |
| `SttService` | sherpa-onnx | Whisper tiny | `models/stt/` |
| `TtsService` | Supertonic 3 | ONNX models | `models/tts/` |
| `JwtService` | jwt-cpp | HS256, instance class | — |
| `ConfigService` | tomlplusplus | TOML config reader | `config.toml` |
| `DbService` | Drogon DbClient | SQLite async client | `database/argus.db` |

## Performance & portability batch (2026-08)

The same binary must perform well on ANY machine (2 cores → 64 cores, with or
without GPU), so every AI service auto-tunes its resources at runtime:

- **`ThreadBudget`** (`src/shared/wrapper/thread-budget/`) — single source of
  truth for adaptive thread counts: `computeThreads` (half hw, 2-16),
  `batchThreads` (half hw, 4-16), `heavyThreads` (3/4 hw, 2-12),
  `lightThreads` (quarter hw, 2-8), `inferenceSlots` (hw/8, 1-4). Applied to:
  ncnn face nets, llama.cpp LLM/Vision (decode vs prefill split), ORT
  intra-op (TTS), sherpa-onnx (STT). NO hardcoded thread counts anywhere.
- **LLM**: `n_threads=lightThreads` + `n_threads_batch=batchThreads` (measured
  +54% prefill on long prompts), warmup decode at init, `use_mmap=true`,
  generation batch reused (no alloc per token), static mutex around context.
  **All tunables live in `config.toml` `[llm]`** (`context_size`, `max_tokens`,
  `temperature`, `threads`/`batch_threads` with 0 = auto ThreadBudget,
  `n_batch`/`n_ubatch`) so a stronger host can be tuned without code changes.
  `context_size` defaulted 128000 → 32768 (Ollama parity): -300MB KV cache
  RAM, faster decode as context grows; fine for short security-analysis turns.
  `ChatRequest.maxTokens`/`temperature` use 0/-1 sentinels → configured
  defaults. System prompt is English (keeps precision) and asks for 2-3 line
  concise answers.
- **Vision**: REPLACED with **SmolVLM2-500M-Video-Instruct (ONNX int8, 0.5B)** —
  `HuggingFaceTB/SmolVLM2-500M-Video-Instruct`: 3 sessions (SigLIP vision
  encoder base patch-16/512 → 64 image tokens, merged Llama3 decoder with
  fp32 KV cache, token embeddings) + GPT-2 byte-level BPE tokenizer. ChatML
  prompt `<|im_start|>User:<image>Can you describe this image?
  <end_of_utterance>\nAssistant:` with the `<image>` block expanded to
  `<fake_token_around_image><global-img><image>x64<fake_token_around_image>`;
  the 64 image rows are filled at runtime with the encoder features
  (inputs_merger pattern). Greedy decode; prefill runs the whole prompt in
  one pass, then iterative decode with growing attention_mask/position_ids.
  Frame cache: the vision encoder output is cached per image hash (2 slots) —
  repeated camera frames skip the ~0.5s encoder pass. The 500M is a full VLM
  (VQA + captioning) and beats Florence-2 on quality while being faster and
  lighter on CPU. NOTE: `vision.image_size` is now fixed at 512 by the model;
  the encoder runs in ~0.5s and decode ~24ms/token on the reference machine.
  **Tunables in `config.toml` `[vision]`**: `max_tokens` (default 64, caption
  is 1-3 sentences) and `threads` (0 = auto). ORT sessions use
  `spin_duration_us=1000` + `spin_backoff_max=8` (ORT #28096 recommended).
- **STT/TTS**: adaptive intra-op threads, static mutexes (recognizer, engine,
  voice cache) — thread-safe for concurrent requests.
- **Face**: `identifyMutex_` (global serialization) replaced by a
  `std::counting_semaphore` with `inferenceSlots()` permits — concurrent
  logins scale with cores. Decode is adaptive: `stbi_info` probe → images
  >2048px decode scaled via OpenCV `IMREAD_REDUCED_COLOR_2/4` (libjpeg IDCT
  scaling), small images keep stb full decode. Login buffer moved (no copies).
- **Coroutines**: `BlockingTask` gained a `void` specialization; every AI
  service exposes `*Async` coroutine variants (`chatAsync`, `chatStreamAsync`,
  `describeAsync`, `transcribeAsync`, `synthesizeAsync`,
  `synthesizeStreamAsync`) that wrap the sync method off the event loop and
  marshal streaming callbacks into the loop via `queueInLoop`.
- **Startup**: services initialize IN PARALLEL (plain `std::thread` + `join`,
  no futures) — full stack loads in ~1s on the reference machine.
- **SQLite**: `applyPragmas()` runs at EVERY boot (WAL, synchronous=NORMAL,
  busy_timeout, mmap 256MB, foreign_keys, temp_store) — previously pragmas
  only applied on fresh DBs and regressed after each restart; connections 1→4.
- **HTTP**: `client_max_memory_body_size` 64K→16M (no multipart spooling to
  disk for images). Release build: `-march=native` + `-flto=auto` on the app
  target only; `-Wall -Wextra` always on, third-party includes SYSTEM.
- **llama.cpp via Conan**: `llama-cpp/b6565` in `conanfile.txt` (latest stable
  on Conan Center, 2025-09). Third-party submodule removed (was pinned to
  b10216 for the LLM; mtmd is no longer built — vision moved to ONNX).
  API notes for b6565: `llama_model_params` uses `use_mmap`/`use_mlock`
  booleans (the newer `load_mode`/`LLAMA_LOAD_MODE_MMAP` from b10216 does not
  exist yet). Target: `llama-cpp::llama-cpp`. Supports the LFM2 arch used by
  LFM2.5-1.2B.
- **ncnn Vulkan**: kept ON — the reference machine has an AMD iGPU (RADV
  RENOIR) that ncnn uses via Vulkan; machines without GPU fall back to CPU.
- **Voice test** (`voice-test/`): standalone `argus-voice-test` binary that
  chains STT → LLM → TTS for a spoken conversation (EN/ES) to measure quality
  and latency end-to-end. PortAudio for mic (16 kHz, software resample
  fallback for devices without 16 kHz) and speaker (plays TTS PCM at its
  native 44.1 kHz so pitch stays natural). Turn-taking uses **Silero VAD v5
  (ONNX, `models/vad/silero_vad.onnx`, ~2.3MB, ~0.1ms/chunk)**: continuous
  speech probability with hysteresis (0.5 start / 0.35 end), ~250ms min
  speech, ~500ms silence hangover, and a 300ms pre-roll so the leading
  phoneme is never clipped (this fixed "hola" being transcribed as garbage).
  The LLM replies fully first, then the TTS speaks with its native chunking
  (natural, not choppy). Ctrl+C exits cleanly (shared std::atomic<bool>).
  `SttService::setLanguage("es"/"en")` switches Whisper at runtime; the
  `[stt] language` config defaults it. Run: `build/prod/voice-test/
  argus-voice-test`.

## Uniform API response format

Every endpoint returns `{ status, info, errors }`. Errors always
`errors: { code, message }` with `info: null`; success has `errors: null`.
Codes: `BAD_REQUEST`, `UNAUTHORIZED`, `FORBIDDEN`, `NOT_FOUND`,
`METHOD_NOT_ALLOWED`, `VALIDATION_ERROR` (422 adds `fields`). All error
responses go through `AppConfig::get{4xx}Response()`; `errorCode` was added
to `ResponseException` so services emit the same codes as the filters.
A Drogon custom error handler wraps built-in 404/405 in the same envelope.

## Face recognition architecture (ncnn, not InspireFace)

- **Detector**: RetinaFace (ncnn, Vulkan when available) — detects faces + 5-point landmarks
- **Recognizer**: MobileFaceNet 128-dim (ncnn) — computes face embedding
- **Index**: FaceDB (HNSWlib inner-product, 128-dim) — sub-millisecond nearest-neighbor search
- **Threshold**: 80% minimum confidence (`dist < 0.20` in inner-product space)
- **Persist**: FaceEmbedding repository stores embeddings as hex-encoded BLOBs in SQLite
- **Cold start**: FaceDB loads embeddings from database at startup via `loadFromDb()`
- **Pipeline cache**: Vulkan SPIR-V shaders cached to `models/face/face.ncnn.vkcache`
- **Optimizations applied**:
  - ncnnoptimize: fused ~60 conv+BN layers, fp16 weight storage
  - Vulkan on detector when a GPU exists (AMD iGPU RENOIR works; CPU fallback otherwise)
  - `from_pixels_roi` for optimized face crop + `PIXEL_BGR2RGB` inline conversion
  - PipelineCache + VkBlobAllocator/VkStagingAllocator for GPU memory efficiency
  - Bounded concurrency (`counting_semaphore`, `inferenceSlots()` permits)
  - Adaptive scaled decode: images >2048px decoded 1/2-1/4 via OpenCV
    (`IMREAD_REDUCED_COLOR_2/4`), smaller keep stb full decode

## Auth system

- **Login**: multipart/form-data face image → `FaceService::identify()` → person lookup
  → user lookup → JWT issuance (access + refresh)
- **JWT**: HS256, dual secrets. Claims: `sub=userId`, `iss=argus`. No role in token.
- **Refresh tokens**: single-use rotation (`is_used=1` after first rotation), replay-protected
- **Device binding**: hash of User-Agent + IP stored per token, validated on each request
- **Filter chain**: `DeviceFilter → ValidJsonFilter → JwtFilter → RoleFilter`
- **Token extraction**: Authorization Bearer / query param `?token=` / cookie
- **Logout**: invalidates all refresh tokens for user (`is_valid=0`)
- **Roles**: Owner (full), Resident (CRUD resources), Guard (read only), Guest (cameras + auth)
- **Error handling**: centralized via `AppConfig::get400/401/403/404/405Response()`
  — uniform envelope `{status, info, errors:{code,message}}` for ALL responses
  (404/405 included via Drogon custom error handler). `ResponseException` carries
  an `errorCode` string; error codes live in `AppConfig::ERROR_CODE_*`.
  Validation exceptions caught globally by `AppConfig::handleException()` → 422
  with `code: VALIDATION_ERROR` + `fields`.

## Validation DSL

Located in `src/shared/validation/`:

| File | Purpose |
|------|---------|
| `rules.hxx` | 31 rule classes (string, numeric, arrays, timestamps, cross-field) |
| `validation_dsl.hxx` | 31 macros (IS_NOT_EMPTY, IS_EMAIL, IS_IN, BETWEEN, etc.) |
| `validator.hxx` | `Validator<T>` + `ValidationException` (422) |

DTOs self-validate via `START_VALIDATION` → macro chain → `END_VALIDATION()`.
Throws `ValidationException` which is caught globally by `AppConfig::handleException()`.
Controllers never use try/catch for validation.

## Dependency injection pattern (manual)

All services and filters hold dependencies as private members with `_` suffix:

```
AuthService: jwtService_, personRepository_, userRepository_, refreshTokenRepository_
JwtFilter:   jwtService_, userRepository_, refreshTokenRepository_
AuthController: service_
```

Never static methods for service classes. Never local/temporary repository construction.

## DTO conventions

- **Request DTOs**: `{Action}Dto` con factory `fromJson()` (camelCase) / `form_multipart()`
- **Response DTOs**: `Response{Action}Dto` con `toJson()` (camelCase)
- **Validation**: DTO factory validates inline via DSL macros, throws on failure
- **Examples**: `LoginDto`, `RefreshTokenDto`, `ResponseLoginDto`, `ResponseRefreshTokenDto`

## Database tables

- `user` — accounts (role, is_active, soft-delete via deleted_at)
- `person` — known people linked to users via `user_id`
- `face_embedding` — persisted face embeddings (hex-encoded 128-dim float BLOBs)
- `refresh_token` — JWT refresh token lifecycle (is_valid, is_used, device_hash, expires_at)
- `camera`, `zone`, `event`, `person_event`, `reminder`, `reminder_detail`, `context_note`
- `camera_stream`, `schema_version`
- `audit_log` — audit global por módulo (`changes` = JSON diff de `JsonDiff`, `priority`, `create_user_id`)
- `user_audit_log` — audit a nivel de usuario (= `audit_log` + `user_id`), sincronizable por usuario
- `notification` — notificaciones personales por usuario (type/title/body/data/is_read)
- `notification_token` — push tokens por sesión (`UNIQUE(user_id, device_hash)`)
- `user_action_log` — historial server-side de acciones (write-only, NO sync)

## Sync engine (WebSocket, one-way server→cliente)

- **Ruta WS**: `/sync` con **solo `JwtFilter`** (sin `DeviceFilter`; el binding de
  dispositivo queda en HTTP). Solo sincronización; notificaciones y
  token-register van por HTTP.
- **Operaciones** (`src/shared/contracts/sync-operation.hxx`): `InitialInfo=0` (connect, envía
  `{id, role, isActive}` — sin lista de módulos),
  `Synchronize=1` (datos creados/eliminados, global + nivel usuario: notification),
  `SynchronizeAuditLog=2` (diffs globales, tablas decididas por el backend según rol),
  `SynchronizeUserAuditLog=3` (diffs a nivel usuario, filtrado por `sub`). Eventos en vivo:
  `Add=4`, `Delete=5`, `Log=6` (emitidos por `AuditLogService`/`NotificationService`).
- **Respuestas WS**: `SocketEmitDto` `{operation, option(TableName), info}`; errores
  `{type:"<type>_error", status, error}`.
- **Entidades syncables**: `user`, `camera`, `camera_stream`, `zone`, `reminder`,
  `reminder_detail` (implementan `Syncable`) + `notification` (dedicado por usuario).
  Fuera del sync: `event`, `person`, `context_note`.
- **Snapshot por día**: `AuditLogService`/`UserAuditLogService` buscan la entrada del
  `record_id` en el día y fusionan el diff (`JsonDiff::compareChanges`).
- **RoomManager**: instancia, estado `thread_local` a nivel de archivo; rooms de módulo
  (`1 + TableName`) y room de usuario (`1000 + sub`); `emitUser` llega a las N sesiones activas.
  Ciclo de vida vía `RoomManagerServiceAdapter` (IService) en `ServiceRegistry`.
- **HTTP**: `PATCH /notification/read` y `POST /notification-token` (cadena completa de filtros).

## Coding conventions

### Enums for constrained strings

All DB columns with CHECK constraints use `enum class` from `enums.hxx`:
`UserRole`, `EventSeverity`, `CameraRecordMode`, `ZoneType`, `ReminderDetailStatus`, `UserAction`.
Each has `toString()` / `fromString()`.

### Parameter structs (3+ params)

ANY function with 3+ parameters uses a struct (`{Entity}CreateInput` /
`{Entity}UpdateInput`, or any descriptive name). Applies to ALL layers
(repositories, services, controllers, filters). Struct goes in the
corresponding `*-query.hxx` file, or in the class header when there is no
`*-query.hxx`. Never raw multi-parameter signatures.

Construct structs with C++20 **designated initializers** passed inline
(`{.field = value, ...}`), fields in declared order, always listing every
member to avoid `-Wmissing-field-initializers`.

### Repository pattern

```
src/shared/repositories/{entity}/
  {entity}-query.hxx      — SQL + param structs
  {entity}-repository.hxx — class
  {entity}-repository.cc  — impl (using namespace {entity}_query)
```

### Controller thinness (4-8 lines per endpoint)

1. Parse → DTO (validates inline)
2. Extract context from attributes
3. Call service
4. Return `ApiResponse::ok(result.toJson())`

### ncnn optimization conventions

- Always set `use_vulkan_compute` + `use_fp16_packed/storage/arithmetic` on detector nets
- Use PipelineCache + VkBlobAllocator for GPU memory efficiency
- Use `from_pixels_roi` instead of manual pixel loops for face crops
- Use `PIXEL_BGR2RGB` in ncnn `from_pixels` to avoid separate cvtColor
- Run `ncnnoptimize` on models offline for fusion and fp16 conversion
- Recognize that some model architectures fall back to CPU on Vulkan (ncnn limitation)
