# Argus Backend

**Argus** is a 100% local, modular AI platform for the home — intelligent security
guard + virtual assistant. Everything runs on-device: face recognition, speech-to-text,
text-to-speech, LLM chat, vision understanding, and a **WebSocket sync engine** that keeps
your devices in sync with the server. No cloud processing, ever.

- **Language:** C++20 (coroutines for async flow)
- **Framework:** [Drogon](https://github.com/drogonframework/drogon) HTTP/WebSocket
- **Database:** SQLite (async `DbClient`, no ORM)
- **Package manager:** Conan 2 + CMake presets

## Philosophy

- **Local first, private by default.** No video, audio, or conversations leave your hardware.
- **Rules before LLM.** Events hit the rule engine first; the LLM is only invoked when rules
  can't resolve the situation. This keeps latency low and resource usage minimal.
- **Modular.** Every capability lives behind a clean interface so models can be swapped
  without touching business logic.

## Features

| Feature | Engine |
|---------|--------|
| **Face login** (multipart) | ncnn RetinaFace + MobileFaceNet + HNSW |
| **JWT auth** + refresh rotation | HS256, dual secrets, device-bound |
| **LLM chat** (sync + streaming) | llama.cpp via Conan (LFM2.5-1.2B-Instruct, 4-bit) |
| **Vision analysis** | SmolVLM2-500M-Video-Instruct (ONNX int8, 0.5B) |
| **Speech-to-text** | sherpa-onnx (FastConformer RNN-T, Spanish) |
| **Text-to-speech** | Supertonic 3 (ONNX) |
| **WebSocket sync** (one-way server→client) | created/deleted + atomic audit diffs + live events |
| **Notifications** | per-user notifications + push tokens per session |
| **Validation DSL** | 31+ built-in validation rules |

All AI services run fully off the event loop (C++20 coroutines) and auto-tune
their thread usage to the host CPU, so the same build performs well on anything
from a 2-core laptop to a 64-core server. Vulkan is used automatically when a
compatible GPU is available.

## Quick start

```bash
./scripts/setup.sh prod     # install deps + build release
./build/prod/argus-backend  # start server on :7024
```

```bash
# Login with your face (multipart/form-data)
curl -X POST http://localhost:7024/auth/login \
  -F "image=@your-face.jpg"

# Use the token
curl http://localhost:7024/auth/status \
  -H "Authorization: Bearer <access_token>"
```

## API response format

Every endpoint returns the same envelope:

```json
{
  "status": 200,
  "info": { },
  "errors": null
}
```

Errors always use `errors: { "code": "...", "message": "..." }` with the matching
HTTP status (`BAD_REQUEST`, `UNAUTHORIZED`, `FORBIDDEN`, `NOT_FOUND`,
`METHOD_NOT_ALLOWED`, `VALIDATION_ERROR`).

## Auth flow

```
POST /auth/login (multipart image)
  → FaceService.identify() → FaceDB.search() → person → user
  → JWT (sub=userId, iss=argus, no role) + refresh token rotation
  → 200 { accessToken, refreshToken, userId, name, role, personId }
```

- **Tokens**: HS256, dual secrets. Access = 15min, Refresh = 30 days.
- **Replay protection**: Refresh tokens are single-use (`is_used=1` after rotation).
- **Device binding**: Tokens bound to User-Agent + IP hash.
- **Logout**: Invalidates all refresh tokens for the user.
- **Filters**: `DeviceFilter → ValidJsonFilter → JwtFilter → RoleFilter`

## WebSocket sync (`/sync`)

One-way server→client synchronization, protected by `JwtFilter` only (device
binding stays on HTTP). Messages are `{ type, payload }`; responses use the
`SocketEmitDto` envelope `{ operation, option(TableName), info }`. Errors are
`{ type: "<type>_error", status, error }`.

| operation | value | purpose |
|-----------|-------|---------|
| `InitialInfo` | 0 | user info (`{id, role, isActive}`) sent on connect |
| `Synchronize` | 1 | created/deleted data per table (global + `notification` per user) |
| `SynchronizeAuditLog` | 2 | atomic update diffs for global data (tables decided by the backend per role) |
| `SynchronizeUserAuditLog` | 3 | atomic update diffs for user-scoped data (filtered by `sub`) |
| `Add` | 4 | live event: entity/notification created |
| `Delete` | 5 | live event: entity deleted |
| `Log` | 6 | live event: audit log |

**Request** (`sync`):
```json
{ "type": "sync", "payload": {
  "camera": { "created": {"startTime": 100, "endTime": 200},
              "deleted": {"startTime": 100, "endTime": 200},
              "findLastCreated": true, "findLastDeleted": true,
              "requiredCreate": true, "requiredDeleted": true }
}}
```

**Response** (`Synchronize`):
```json
{ "operation": 1, "option": "user",
  "info": { "camera": { "created": [ ... ],
                        "deleted": [ ... ],
                        "lastSyncDate": { "createdId": 5, "created": 200 } } } }
```

Syncable entities: `user`, `camera`, `camera_stream`, `zone`, `reminder`,
`reminder_detail`, `notification`. `sync_audit_log` and `sync_user_audit_log`
don't ask for tables — the backend picks them from the role. Diffs use
`JsonDiff` (snapshot merge per record/day), so only the changed part is sent.

## Notifications (HTTP)

- `PATCH /notification/read` — mark own notifications as read
  (`{ "ids": [1, 2] }`).
- `POST /notification-token` — register a push token for the current session
  (`{ "token", "platform", "lang" }`; bound to `(user_id, device_hash)`).

Notifications are created server-side by `NotificationService` (writes to
`notification` + emits live `Add` to the user's socket sessions). Per-user
state changes are recorded in `user_audit_log` (syncable); server-side history
of user actions lives in `user_action_log` (write-only, not synced).

## Tech stack

### Conan packages
- `drogon` — HTTP/WebSocket + logging
- `jwt-cpp/0.7.2` + `nlohmann_json/3.11.3` — JWT auth
- `opencv/4.13.0` (headless) — scaled face image decoding
- `onnxruntime/1.24.4` — STT/TTS/vision inference
- `llama-cpp/b6565` — LLM inference (LFM2.5-1.2B-Instruct)
- `tomlplusplus/3.3.0` — config
- `eigen/5.0.1` — linear algebra

### Third-party (git submodules)
- **ncnn** — neural net inference (face detection + recognition), Vulkan when available
- **hnswlib** — approximate nearest neighbor search (face embedding index)
- **sherpa-onnx** — speech-to-text (FastConformer RNN-T / Whisper via ONNX Runtime)
- **fastText** — text classification / intent detection
- **inspireface** — optional face recognition backend

### Vision model (ONNX)
- **SmolVLM2-500M-Video-Instruct** (ONNX int8, 0.5B) from
  `HuggingFaceTB/SmolVLM2-500M-Video-Instruct` — 3 sessions (SigLIP vision
  encoder at 512px producing 64 image tokens, merged Llama3 decoder with fp32 KV
  cache, token embeddings) + GPT-2 byte-level BPE tokenizer, downloaded by
  `scripts/setup.sh` into `models/vision/smolvlm/`. ChatML prompt with an
  expanded `<image>` block and a fixed captioning instruction. A frame cache
  reuses the encoder output for repeated camera frames, so repeated frames skip
  the ~0.5s encoder pass. `vision.max_tokens` (default 64) controls caption length.

### Speech-to-text (sherpa-onnx)
Default engine is **FastConformer RNN-T** (`nemo_transducer`, en+de+es+fr,
RTF ~0.02, `models/stt/`), language `es`. `SttService::setLanguage()` switches
`es`/`en` at runtime. Other engines selectable via `config.toml [stt]`:
`canary` (NeMo Canary 180m flash), `whisper` (tiny/base/small, auto language),
`nemo_ctc` (fastest, weaker English), `omnilingual` (1600 languages).

## Project structure

```
src/
├── config/              Kernel: responses, CORS, exception handler, service registry
├── feature/
│   ├── api/
│   │   ├── auth/        Auth controllers, services, DTOs
│   │   └── notification/  Notification read + push token endpoints
│   └── socket/
│       └── sync/        SyncSocket (/sync), SyncService, SynchronizedService, DTOs
├── filter/
│   ├── device/          Device fingerprint (UA + IP hash)
│   ├── jwt/             JWT verification + DB validation
│   ├── role/            Role-based access control (via shared RoleAccess)
│   └── valid-json/      JSON body validation middleware
├── shared/
│   ├── enums.hxx        All enum types (incl. TableName, AuditLogPriority)
│   ├── contracts/       Syncable, SyncFilter (+ sync_query), sync-operation.hxx
│   ├── access/          RoleAccess centralizado (rol → tabla → permisos)
│   ├── dtos/            SocketEmitDto
│   ├── exceptions/      ResponseException
│   ├── repositories/    Data access layer (raw SQL, no ORM)
│   ├── schemas/         DB row → C++ struct mapping
│   ├── services/
│   │   ├── face/        Face detection (ncnn) + FaceDB (HNSW index)
│   │   ├── jwt/         JWT sign/verify (HS256, instance class)
│   │   ├── llm/         LLM inference (llama.cpp)
│   │   ├── vision/      Vision captioning (SmolVLM2, ONNX)
│   │   ├── stt/         Speech-to-text (sherpa-onnx)
│   │   ├── tts/         Text-to-speech (Supertonic 3, ONNX)
│   │   ├── sqlite/      DbClient access
│   │   ├── room/        RoomManager (thread_local, no Redis)
│   │   ├── socket/      SocketService (emitModule/emitUser)
│   │   ├── audit-log/   Global audit with daily snapshot + emit
│   │   ├── user-audit-log/  Per-user audit (syncable)
│   │   ├── notification/    Per-user notifications (createAndEmit + Add)
│   │   ├── notification-token/  Push tokens per session
│   │   ├── user-action-log/  Write-only server-side history
│   │   └── config-service/  TOML config reader
│   ├── utils/           json-diff (JsonDiff), json-util
│   ├── validation/      Validation DSL (rules + macros + validator)
│   └── wrapper/         api-response, blocking-task, thread-budget
└── main.cc
```

## Build

```bash
# Development (Debug)
cmake --preset dev
cmake --build --preset dev -j 8

# Production (Release) — auto-tuned to the build machine (-march=native, LTO)
cmake --preset prod
cmake --build --preset prod -j 8

# Full setup (installs deps + builds)
./scripts/setup.sh dev
./scripts/setup.sh prod
```

Server listens on `0.0.0.0:7024`. Database at `database/argus.db`.
Models under `models/{face,llm,vision,stt,tts}/` — vision (SmolVLM2 ONNX int8)
and STT are downloaded automatically by `scripts/setup.sh`.

## Benchmarks

```bash
./build/prod/argus-backend --bench   # LLM / VL / STT / TTS: init, latency, RAM
```

## Conventions

- **C++20**, `.hxx` / `.cc` extensions
- **Coroutines** for any flow that touches the event loop (`BlockingTask` awaiter)
- **Enums** for all constrained DB columns (no raw strings)
- **DTOs** self-validate using DSL macros; JSON factories use camelCase
  (`fromJson` / `toJson`)
- **Controllers** ultra-thin (4-8 lines per endpoint)
- **Dependency injection** — all services/repos as private `_` suffix members
- **Structs** for any function with 3+ params (in `*-query.hxx`, or the class
  header), built with designated initializers
- **No ORM** — raw SQL via `DbService::client()->execSqlCoro()`
- **No spdlog** — built-in Drogon logging
- **Smart pointers only** — no raw owning pointers
- **Sync queries** — build SQL synchronously via `sync_query::buildSyncQuery`
  and `co_await execSqlCoro(query, args)` directly; never capture references in
  inner coroutine lambdas that suspend
