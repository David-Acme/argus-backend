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
| **LLM chat** (sync + streaming) | llama.cpp via Conan (LFM2.5-1.2B-Instruct QAD Q4_0) |
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

## Local RustFS storage (optional Docker service)

Native backend development remains the default. Docker does **not** start or
build the backend unless its explicit `backend` profile is selected, so it does
not compete with emulators or local AI workloads. RustFS is the only default
service and is bound exclusively to `127.0.0.1:9000`; its console is disabled
and neither the LAN nor a future tunnel can reach it. The image is pinned to
RustFS `1.0.0-beta.12`, which includes the security patch line introduced after
beta.11; it is not floated to `latest`.

```bash
./scripts/bootstrap-local-stack.sh
docker compose up -d rustfs rustfs-init
docker compose ps
```

The bootstrap is idempotent. It generates one-time RustFS root/RPC secrets, a
private bucket name and a distinct least-privilege application access/secret
pair. The application pair is written to `config.local.toml` with mode `0600`,
so it is secret and must never be committed or shared. It is not a RustFS root
credential. The bootstrap never prints credential values. `docker compose up`
with no service selection starts only `rustfs` and the one-shot `rustfs-init`;
it never starts the backend.

`scripts/setup.sh` also creates strong per-installation JWT and device
fingerprint secrets in the same ignored overlay. The checked-in `config.toml`
intentionally contains empty JWT secrets; the backend refuses to start until
the local overlay supplies them.

`rustfs-init` is the only container that receives both root and application
credentials. It creates/verifies an `argus-backend` service account whose inline
policy is limited to `ListBucket` on Argus's generated bucket and
`GetObject`/`PutObject`/`DeleteObject` under that bucket only. RustFS itself
never receives application credentials, and the native backend reads them only
from its local `config.local.toml` overlay.

RustFS documents `_FILE` injection for root access and secret keys. Its RPC
secret has no documented `_FILE` counterpart in the pinned release, so the
container entrypoint reads the Docker secret file only at startup and exports
it only to the RustFS process; the compose file never places that value in an
environment literal.

RustFS data lives in Docker volume `argus-rustfs-data`; generated instance state
lives under ignored `docker/runtime/`. Back up both before moving a machine:

```bash
mkdir -p backups/rustfs-data
docker run --rm -v argus-rustfs-data:/source -v "$PWD/backups/rustfs-data:/backup" \
  alpine:3.20 tar -C /source -cf /backup/rustfs-data.tar .
tar -C . -cf backups/argus-rustfs-instance-state.tar docker/runtime config.local.toml
```

To restore on a machine where the stack is stopped, restore `docker/runtime/`
and `config.local.toml`, then extract `rustfs-data.tar` into an empty
`argus-rustfs-data` volume. Keep the secrets with the data: replacing either
one makes existing private objects inaccessible.

### Optional backend image

The future portable backend image is intentionally opt-in:

```bash
# Run the native setup first so these host-owned paths already exist:
# certs/, database/, models/, uploads/, third_party/go2rtc/ and config.local.toml.
./scripts/setup.sh prod
mkdir -p database uploads

ARGUS_UID="$(id -u)" ARGUS_GID="$(id -g)" docker compose --profile backend build backend
ARGUS_UID="$(id -u)" ARGUS_GID="$(id -g)" docker compose --profile backend up -d
```

Its portable bridge mode reaches RustFS at `http://rustfs:9000`; set that value
in `config.local.toml` before starting it. On Linux, host networking preserves
mDNS and local camera discovery:

```bash
docker compose -f docker-compose.yml -f docker-compose.host.yml --profile backend up -d
```

For that override, set the storage endpoint back to `http://127.0.0.1:9000`.
It requires Docker Compose v2.24.4+ and does not attach the backend to the
storage network. Resource caps are configurable with `RUSTFS_MEMORY_LIMIT`,
`RUSTFS_CPU_LIMIT`, `ARGUS_BACKEND_MEMORY_LIMIT` and `ARGUS_BACKEND_CPU_LIMIT`;
they never affect the native backend process. The optional container defaults
to UID/GID `1000:1000`; setting `ARGUS_UID` and `ARGUS_GID` to the current host
user avoids root-owned files in `certs/`, `database/` and `uploads/`. Those
three mounts are writable because the backend rotates certificates and writes
SQLite/uploads; the config overlay, models and go2rtc binary mounts are
read-only. All backend binds use `create_host_path: false`: if native setup has
not prepared a required path, Compose fails before Docker can create a
root-owned directory or file.

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

One-way server→client synchronization, protected by `DeviceFilter` and
`JwtFilter` on the same route. Messages are `{ type, payload }`; responses use the
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
- `llama-cpp/b6565` — LLM inference (LFM2.5-1.2B-Instruct QAD)
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
Models under `models/{face,llm,vision,stt,tts}/` are runtime artifacts. The
LFM2.5-1.2B-Instruct QAD GGUF, vision model and STT assets are downloaded
automatically by `scripts/setup.sh`; model checksums are verified before the
files become active.

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
