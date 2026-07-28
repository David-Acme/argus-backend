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
  (NO ORM, manual SQL). DB file `database/argus.db`, `number_of_connections: 1`.
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
| `LlmService` | llama.cpp | LFM2.5-1.2B-Instruct-Q4_K_M | `models/llm/` |
| `VisionService` | llama.cpp | LFM2.5-VL-450M-Q4_K_M | `models/vision/` |
| `SttService` | sherpa-onnx | Whisper tiny | `models/stt/` |
| `TtsService` | Supertonic 3 | ONNX models | `models/tts/` |
| `JwtService` | jwt-cpp | HS256, instance class | — |
| `ConfigService` | tomlplusplus | TOML config reader | `config.toml` |
| `DbService` | Drogon DbClient | SQLite async client | `database/argus.db` |

## Face recognition architecture (ncnn, not InspireFace)

- **Detector**: RetinaFace (ncnn, Vulkan GPU) — ~50ms, detects faces + 5-point landmarks
- **Recognizer**: MobileFaceNet 128-dim (ncnn, CPU) — ~170ms, computes face embedding
- **Index**: FaceDB (HNSWlib inner-product, 128-dim) — sub-millisecond nearest-neighbor search
- **Threshold**: 80% minimum confidence (`dist < 0.20` in inner-product space)
- **Persist**: FaceEmbedding repository stores embeddings as hex-encoded BLOBs in SQLite
- **Cold start**: FaceDB loads embeddings from database at startup via `loadFromDb()`
- **Pipeline cache**: Vulkan SPIR-V shaders cached to `models/face/face.ncnn.vkcache`
- **Total latency**: ~220ms (decode 2ms + detect 50ms + recognize 170ms + search 0ms)
- **Optimizations applied**:
  - ncnnoptimize: fused ~60 conv+BN layers, fp16 weight storage
  - Vulkan on detector, fp16 packed arithmetic on both nets
  - `from_pixels_roi` for GPU-optimized face crop
  - PIXEL_BGR2RGB inline conversion (eliminates separate cvtColor)
  - PipelineCache + VkBlobAllocator/VkStagingAllocator for GPU memory efficiency
- **Recognizer CPU fallback**: MobileFaceNet uses models with some layers that ncnn
  doesn't accelerate on Vulkan (documented ncnn issue #6858). Detector runs fully on GPU.

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
- **Error handling**: centralized via `AppConfig::get401/403/400/404Response()`
  Validation exceptions caught globally by `AppConfig::handleException()` → 422 JSON

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

- **Request DTOs**: `{Action}Dto` with static factory (`form_json` / `form_multipart`)
- **Response DTOs**: `Response{Action}Dto` with `toJson()` method
- **Validation**: DTO factory validates inline via DSL macros, throws on failure
- **Examples**: `LoginDto`, `RefreshTokenDto`, `ResponseLoginDto`, `ResponseRefreshTokenDto`

## Database tables

- `user` — accounts (role, is_active, soft-delete via deleted_at)
- `person` — known people linked to users via `user_id`
- `face_embedding` — persisted face embeddings (hex-encoded 128-dim float BLOBs)
- `refresh_token` — JWT refresh token lifecycle (is_valid, is_used, device_hash, expires_at)
- `camera`, `zone`, `event`, `person_event`, `reminder`, `reminder_detail`, `context_note`
- `camera_stream`, `audit_log`, `user_action_log`, `schema_version`

## Coding conventions

### Enums for constrained strings

All DB columns with CHECK constraints use `enum class` from `enums.hxx`:
`UserRole`, `EventSeverity`, `CameraRecordMode`, `ZoneType`, `ReminderDetailStatus`, `UserAction`.
Each has `toString()` / `fromString()`.

### Parameter structs (3+ params)

Functions with 3+ parameters use `{Entity}CreateInput` / `{Entity}UpdateInput` structs
in the corresponding `*-query.hxx` file.

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
