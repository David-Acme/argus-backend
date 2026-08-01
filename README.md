# Argus Backend

**Argus** is a 100% local, modular AI platform for the home — intelligent security
guard + virtual assistant. Everything runs on-device: face recognition, speech-to-text,
text-to-speech, LLM chat, and vision understanding. No cloud processing, ever.

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
| **LLM chat** (sync + streaming) | llama.cpp (LFM2.5-1.2B-Instruct, 4-bit) |
| **Vision analysis** | Florence-2-base-ft (ONNX int8, 0.23B) |
| **Speech-to-text** | sherpa-onnx (Whisper tiny, Spanish) |
| **Text-to-speech** | Supertonic 3 (ONNX, 10 voices) |
| **Validation DSL** | 31 built-in validation rules |

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

## Tech stack

### Conan packages
- `drogon/1.9.13` — HTTP/WebSocket + logging
- `jwt-cpp/0.7.2` + `nlohmann_json/3.11.3` — JWT auth
- `opencv/4.13.0` (headless) — scaled face image decoding
- `onnxruntime/1.24.4` — STT/TTS inference (sherpa-onnx)
- `tomlplusplus/3.3.0` — config
- `eigen/5.0.1` — linear algebra

### Third-party (git submodules)
- **llama.cpp** (b10216) — LLM inference
- **ncnn** — neural net inference (face detection + recognition), Vulkan when available
- **hnswlib** — approximate nearest neighbor search (face embedding index)
- **sherpa-onnx** — speech-to-text (Whisper tiny via ONNX Runtime)
- **fastText** — text classification / intent detection

### Vision model (ONNX)
- **Florence-2-base-ft** (0.23B, int8, MIT) from `onnx-community/Florence-2-base-ft`
  — 4 ONNX sessions (vision encoder, encoder, merged decoder, token
  embeddings) + byte-level BPE tokenizer, downloaded by `scripts/setup.sh`
  into `models/vision/florence/`. Produces detailed English captions
  (task `<MORE_DETAILED_CAPTION>`) for the main LLM. A frame cache reuses the
  encoder output for repeated camera frames; `vision.image_size` (default
  768) trades detail for speed on weak CPUs.

## Project structure

```
src/
├── config/              Kernel: responses, CORS, exception handler, service registry
├── feature/api/auth/    Auth controllers, services, DTOs
├── filter/
│   ├── device/          Device fingerprint (UA + IP hash)
│   ├── jwt/             JWT verification + DB validation
│   ├── role/            Role-based access control (declarative map)
│   └── valid-json/      JSON body validation middleware
├── shared/
│   ├── enums.hxx        All enum types + toString/fromString
│   ├── contracts/       Syncable, SyncFilter base classes
│   ├── exceptions/      ResponseException
│   ├── repositories/    Data access layer (raw SQL, no ORM)
│   ├── schemas/         DB row → C++ struct mapping
│   ├── services/
│   │   ├── face/        Face detection (ncnn) + FaceDB (HNSW index)
│   │   ├── jwt/         JWT sign/verify (HS256, instance class)
│   │   ├── llm/         LLM inference (llama.cpp)
│   │   ├── vision/      Vision captioning (Florence-2, ONNX)
│   │   ├── stt/         Speech-to-text (whisper, sherpa-onnx)
│   │   ├── tts/         Text-to-speech (Supertonic 3, ONNX)
│   │   ├── sqlite/      DbClient access
│   │   └── config-service/  TOML config reader
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
Models under `models/{face,llm,vision,stt,tts}/` — the vision model
(Florence-2 ONNX int8) is downloaded automatically by `scripts/setup.sh`.

## Benchmarks

```bash
./build/prod/argus-backend --bench   # LLM / VL / STT / TTS: init, latency, RAM
```

## Conventions

- **C++20**, `.hxx` / `.cc` extensions
- **Coroutines** for any flow that touches the event loop (`BlockingTask` awaiter)
- **Enums** for all constrained DB columns (no raw strings)
- **DTOs** self-validate using DSL macros
- **Controllers** ultra-thin (4-8 lines per endpoint)
- **Dependency injection** — all services/repos as private `_` suffix members
- **No ORM** — raw SQL via `DbService::client()->execSqlCoro()`
- **No spdlog** — built-in Drogon logging
- **Smart pointers only** — no raw owning pointers
