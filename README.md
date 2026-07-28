# Argus Backend

**Argus** is a 100% local, modular AI platform for the home — intelligent security
guard + virtual assistant. Everything runs on-device: face recognition, speech-to-text,
text-to-speech, LLM chat, and vision understanding. No cloud processing, ever.

- **Language:** C++20
- **Framework:** [Drogon](https://github.com/drogonframework/drogon) HTTP/WebSocket
- **Database:** SQLite (async `DbClient`, no ORM)
- **Package manager:** Conan 2 + CMake presets

## Philosophy

- **Local first, private by default.** No video, audio, or conversations leave your hardware.
- **Rules before LLM.** Events hit the rule engine first; the LLM is only invoked when rules
  can't resolve the situation. This keeps latency low and resource usage minimal.
- **Modular.** Every capability lives behind a clean interface so models can be swapped
  without touching business logic.

## Features (implemented)

| Feature | Engine | Latency |
|---------|--------|---------|
| **Face login** (multipart) | ncnn RetinaFace + MobileFaceNet + HNSW | ~220ms |
| **JWT auth** + refresh rotation | HS256, dual secrets, device-bound | <1ms |
| **LLM chat** | llama.cpp (LFM2.5-1.2B-Instruct, 4-bit) | streaming |
| **Vision analysis** | llama.cpp (LFM2.5-VL-450M) | ~5s/image |
| **Speech-to-text** | sherpa-onnx (Whisper tiny, Spanish) | streaming |
| **Text-to-speech** | Supertonic 3 (ONNX, 10 voices) | streaming |
| **Validation DSL** | 31 built-in validation rules | compile-time |

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

## Validation DSL

```cpp
START_VALIDATION(MyDto, dto)
IS_NOT_EMPTY(name)
IS_EMAIL(email)
IS_IN(role, "admin", "user", "guest")
MIN_LENGTH(password, 8)
BETWEEN(age, 18, 120)
END_VALIDATION()
```

31 rules available: strings (email, uuid, url, hex, slug, base64, alpha, alnum),
numeric (positive, non-negative, min, max, between), arrays (not-empty, min/max elements),
cross-field (equals), timestamps, booleans, custom lambdas. See `src/shared/validation/`.

## Tech stack

### Conan packages
- `drogon/1.9.13` — HTTP/WebSocket + logging
- `jwt-cpp/0.7.2` + `nlohmann_json/3.11.3` — JWT auth
- `llama-cpp/b6565` — LLM + Vision inference
- `opencv/4.13.0` (headless) — face image decoding
- `onnxruntime/1.24.4` — STT inference (sherpa-onnx)
- `tomlplusplus/3.3.0` — config
- `eigen/5.0.1` — linear algebra

### Third-party (git submodules)
- **ncnn** — high-performance neural net inference (face detection + recognition), Vulkan GPU
- **hnswlib** — approximate nearest neighbor search (face embedding index)
- **sherpa-onnx** — streaming speech-to-text (Whisper tiny via ONNX Runtime)
- **fastText** — text classification / intent detection

## Project structure

```
src/
├── config/              Centralized responses, CORS, exception handler
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
│   │   ├── vision/      Vision model inference (llama.cpp)
│   │   ├── stt/         Speech-to-text (whisper, sherpa-onnx)
│   │   ├── tts/         Text-to-speech (Supertonic 3, ONNX)
│   │   ├── sqlite/      DbClient access
│   │   └── config-service/  TOML config reader
│   ├── validation/      Validation DSL (rules + macros + validator)
│   └── wrapper/api-response/  Standardized JSON response format
└── main.cc
```

## Build

```bash
# Development (Debug)
cmake --preset dev
cmake --build --preset dev -j 8

# Production (Release)
cmake --preset prod
cmake --build --preset prod -j 8

# Full setup (installs deps + builds)
./scripts/setup.sh dev
./scripts/setup.sh prod
```

Server listens on `0.0.0.0:7024`. Database at `database/argus.db`.
Models under `models/{face,llm,vision,stt,tts}/`.

## Conventions

- **C++20**, `.hxx` / `.cc` extensions
- **Enums** for all constrained DB columns (no raw strings)
- **DTOs** self-validate using DSL macros
- **Controllers** ultra-thin (4-8 lines per endpoint)
- **Dependency injection** — all services/repos as private `_` suffix members
- **No ORM** — raw SQL via `DbService::client()->execSqlCoro()`
- **No spdlog** — built-in Drogon logging
- **No static service methods** — instance classes with default constructors
- **Smart pointers only** — no raw owning pointers
