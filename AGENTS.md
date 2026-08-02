# AGENTS.md — Argus Backend AI Agent Instructions

> This file is read by AI coding assistants before any code generation task.
> It defines the project's architecture, conventions, and constraints.

## Project Identity

- **Argus** — 100% local AI home security + virtual assistant platform.
- **C++20**, Drogon HTTP/WebSocket, SQLite (async `DbClient`, no ORM).
- **Conan 2** + **CMake presets** (`dev` = Debug, `prod` = Release).

## Project layout (whole project)

- **`backend/`** (this repo) — C++20 + Drogon server: all AI on-device (face
  auth, LLM, vision, STT/TTS), JWT dual secrets, WebSocket sync on `/sync`.
  Listens on `0.0.0.0:7024`.
- **`frontend/`** (sibling) — React Native mobile app (Expo SDK 57 + Expo Router
  + Tailwind v4 via Uniwind). It is the client for this backend: face login
  (multipart → `POST /auth/login`), JWT access/refresh rotation, notifications,
  and the `/sync` WebSocket. **Frontend integration is not built yet** — the
  planned `core/services/http/http.service.ts`, auth and sync layers are pending.
- Frontend conventions live in `frontend/AGENTS.md` + `frontend/CONTEXT.md`
  (design system, colors, component structure, platform-split services, icon
  registry, Android dev environment).
- The backend and frontend are the two halves of one product; changes that affect
  API contracts (endpoints, WS operations, DTO shapes, `SyncOperation` values,
  role permissions in `role-access.hxx`) must be coordinated with the frontend.

## MUST-FOLLOW Rules

### 1. Enums, never raw strings for constrained columns

Every DB column with a CHECK constraint (`role`, `severity`, `record_mode`,
`zone_type`, `status`, `action`) MUST use its `enum class` from
`src/shared/enums.hxx`. Use `toString()`/`fromString()` at DB boundaries only.

```
UserRole, EventSeverity, CameraRecordMode, ZoneType,
ReminderDetailStatus, UserAction
```

### 2. Parameter structs for 3+ params

ANY function with 3+ parameters MUST define a struct. This applies to ALL
layers (repositories, services, controllers, filters), not only repositories.
Prefer passing a single object over many positional arguments — easier to
maintain and to send data.

```
{Entity}CreateInput, {Entity}UpdateInput
```

- If the function belongs to a repository, define the struct in the
  corresponding `*-query.hxx` file, after the query namespace.
- If there is no `*-query.hxx`, define the struct in the class header that
  declares the function.

Never write raw multi-parameter method signatures.

Construct parameter structs with C++20 **designated initializers** passed
inline where possible: `service_.register({.userId = ctx.sub, .token = t})`.
The compiler infers the type from the parameter; keep the fields in declared
order and always list every member (avoids `-Wmissing-field-initializers`).

### 3. Repository structure

```
src/shared/repositories/{entity}/
  {entity}-query.hxx      — SQL strings (namespace) + param structs (global)
  {entity}-repository.hxx — class declaration (uses structs from query file)
  {entity}-repository.cc  — implementations (uses `using namespace *_query`)
```

- `using namespace {entity}_query;` ALWAYS in `.cc` files.
- `create()` builds the schema from the input + `insertId()` (NO extra query).
- `update()` is PATCH semantics: `{Entity}UpdateInput` fields are
  `std::optional`, the SET clause is built dynamically from the provided
  fields (fragments `UPDATE_COL_*` in the query file), then re-fetched via
  `findById()`.
- Soft-delete: `UPDATE ... SET deleted_at = strftime('%s', 'now')`.
- Syncable repos implement `find/findDeleted/findLast/findLastDeleted`.

### 4. Dependency injection (manual, no framework)

Services and filters hold their dependencies as **private members with `_` suffix**.
Never instantiate repositories/services as local variables or temporaries.

```cpp
class AuthService {
private:
    JwtService jwtService_;
    PersonRepository personRepository_;
    UserRepository userRepository_;
    RefreshTokenRepository refreshTokenRepository_;
};
```

Naming conventions:
- Single repository: `repository_`
- Multiple repositories: `userRepository_`, `personRepository_`
- Single service: `service_`
- Multiple services: `jwtService_`, `faceService_`

Controllers hold a private `AuthService service_;` member — never static methods.

### 5. Filter chain order

```
DeviceFilter → ValidJsonFilter → JwtFilter → RoleFilter
```

Registered via Drogon controller `ADD_METHOD_TO` macro using string names.
Never skip a filter in protected routes.

**Exception:** Multipart endpoints (`/auth/login`) skip `ValidJsonFilter` since
`req->getJsonObject()` returns `nullptr` for `multipart/form-data`.

### 6. Responses & attribute keys

ALL error responses go through `AppConfig` (in `src/config/`):

```cpp
AppConfig::get401Response();                         // default message
AppConfig::get401Response("Custom message");          // custom message
AppConfig::get403Response();
AppConfig::get400Response("Bad request");
AppConfig::get404Response("Path not found");
AppConfig::get409Response("Server already paired");
```

Never call `ApiResponse::error()` directly from filters/controllers.

Validation errors use `ApiResponse::validationError(fieldErrors)` → 422.

Attribute keys are centralized constants:
```
AppConfig::JWT_CTX_KEY    = "jwt_ctx"
AppConfig::DEVICE_CTX_KEY = "device_ctx"
```

### 7. Role-based access

Roles are checked centrally via `src/shared/access/role-access.hxx` (mapa
`kTableAccess`: rol → tabla → `RolePermission`). **`RoleFilter` (HTTP) y el
motor de sync comparten esa única fuente de verdad** — para modificar permisos,
edita solo ese archivo. No imperativo if/else.

```
Owner    → full access (bypasses all checks)
Resident → CRUD on /camera, /camera-stream, /zone, /reminder, /reminder-detail,
           /event, /person, /context-note; user = Read (NO registra usuarios);
           audit_log/user_audit_log/notification = Read (+Update propias);
           notification_token = Create (propias)
Guard    → GET only on /camera, /camera-stream, /event, /person, /zone, /auth
Guest    → GET only on /camera, /auth
```

Helpers: `hasAccess(role, table, perm)`, `readableTables(role)`,
`tableFromPath(path)`, `permissionForMethod(method)`, `hasHttpAccess(...)`.

### 8. JWT auth flow

1. `DeviceFilter` extracts UA+IP → `DeviceContext` (stored as `"device_ctx"` attribute)
2. `JwtFilter` extracts token (Header Bearer / query `?token=` / cookie), verifies HS256,
   validates `refresh_token` in DB (is_valid=1, is_used=0, device_hash match),
   injects `JwtContext` as `"jwt_ctx"` attribute
3. `RoleFilter` reads `JwtContext.role` and checks via
   `role_access::hasHttpAccess(role, path, method)`

### 9. JWT conventions

- **sub** = `userId` (stringified), **iss** = `"argus"`
- **NO role in JWT** — role is fetched from DB by JwtFilter on every request
- **Dual secrets:** `jwt.secret` for access tokens, `jwt.refresh_secret` for refresh tokens
- **Refresh token rotation:** single-use (`is_used=1` after first rotation), replay-protected
- JwtService is an **instance class** — secrets read once in constructor

### 10. DTO conventions

JSON factories/serializers use **camelCase**: `fromJson()` (parse) and
`toJson()` (serialize). Multipart parsing keeps `form_multipart()`.

**Request DTOs** — self-validating, header + cc file:
```
src/feature/api/{resource}/dtos/
  {action}-dto.hxx     — struct + fromJson() / form_multipart() factory
  {action}-dto.cc       — implementation + validation DSL
```

Naming: `LoginDto`, `RefreshTokenDto`, `CreateCustomerDto`

**Response DTOs** — header + cc file, with `toJson()`:
```
src/feature/api/{resource}/dtos/
  response-{action}-dto.hxx
  response-{action}-dto.cc
```

Naming: `ResponseLoginDto`, `ResponseRefreshTokenDto`

**WS/sync DTOs** (header-only, por ejemplo `synchronized-dto.hxx`): parsean con
`static {T} fromJson(const Json::Value&)`; se declaran en `src/feature/socket/sync/dtos/`.

### 11. Validation DSL

All DTO validation uses the macro DSL in `src/shared/validation/`:

```cpp
LoginDto LoginDto::fromJson(const Json::Value& json) {
    LoginDto dto;
    dto.email = json.get("email", "").asString();
    START_VALIDATION(LoginDto, dto)
    IS_NOT_EMPTY(email)
    IS_EMAIL(email)
    END_VALIDATION()
    return dto;
}
```

Available macros: `IS_NOT_EMPTY`, `IS_NOT_EMPTY_OPTIONAL`, `IS_EMAIL`, `IS_UUID`,
`IS_URL`, `IS_HEX`, `IS_SLUG`, `IS_BASE64`, `IS_IN`, `IS_ALPHA`, `IS_ALNUM`,
`HAS_NO_SPACES`, `MATCHES_REGEX`, `MIN_LENGTH`, `MAX_LENGTH`, `MIN_LENGTH_OPTIONAL`,
`MAX_LENGTH_OPTIONAL`, `IS_POSITIVE`, `IS_NON_NEGATIVE`, `MIN_INT`, `MAX_INT`,
`BETWEEN`, `EQUALS_FIELD`, `ARRAY_NOT_EMPTY`, `MIN_ELEMENTS`, `MAX_ELEMENTS`,
`IS_VALID_TIMESTAMP`, `IS_POSITIVE_TIMESTAMP`, `IS_POSITIVE_TIMESTAMP_OPTIONAL`,
`IS_BOOLEAN`, `CUSTOM_LAMBDA`.

When validation fails, `END_VALIDATION()` throws `ValidationException(errors, 422)`.
The global `AppConfig::handleException()` catches it and returns a 422 JSON response.
**Controllers never need try/catch for validation.**

### 12. Controller thinness

Controllers act as pure gateways — **4-8 lines per endpoint**:
1. Parse request into DTO (factory throws on validation failure)
2. Extract context from attributes (guaranteed by filters)
3. Call service
4. Return `ApiResponse::ok(result.toJson())`

```cpp
Task<HttpResponsePtr> login(HttpRequestPtr req) {
    const auto body = LoginDto::form_multipart(parser);
    const auto& dev = req->getAttributes()->get<DeviceContext>(DEVICE_CTX_KEY);
    const auto result = co_await service_.login(body, dev.deviceHash, dev.userAgent);
    if (!result) co_return AppConfig::get401Response("Face not recognized");
    co_return ApiResponse::ok(result->toJson());
}
```

Never: `if (!json)`, `if (!attrs->find(JWT_CTX_KEY))`, manual field extraction, try/catch.

### 13. Face recognition architecture

**FaceService** — single entry point for face operations:
- `extract(rgbData, width, height)` → `FaceResult` (embedding + confidence)
- `identify(imageBytes)` → `optional<int64_t>` personId
- `identifyAsync(imageBytes)` → coroutine variant (runs off the event loop)

**FaceDB** — HNSW index for high-performance embedding search:
- `search(embedding)` → `optional<pair<int64_t, float>>` (personId, confidence)
- `insert(embedding, personId)` — add to index
- Threshold: `dist > 0.20` (80% minimum inner-product confidence)

**FaceEmbedding repository** — persists embeddings to SQLite (hex-encoded BLOB).
Loaded at startup via `FaceDB::loadFromDb()`.

Concurrency: `identify()` is bounded by a `std::counting_semaphore`
(`inferenceSlots()` slots, sized from the host CPU). Never replace it with a
global mutex. Image decoding is adaptive: `stbi_info` probes dimensions, large
images (>2048px) are decoded scaled via OpenCV (`IMREAD_REDUCED_COLOR_2/4`).

### 13b. Adaptive threading (ThreadBudget)

NEVER hardcode thread counts. All AI services size their thread pools from
`src/shared/wrapper/thread-budget/thread-budget.hxx`:
`computeThreads()`, `batchThreads()`, `heavyThreads()`, `lightThreads()`,
`inferenceSlots()`. This keeps the same build fast on 2-core laptops and
64-core servers. Example: LLM uses `lightThreads()` for token decode and
`batchThreads()` for prompt prefill.

### 13c. Coroutines for heavy AI calls

Every AI service exposes a sync method (`chat`, `describe`, `transcribe`,
`synthesize`) AND a coroutine variant (`chatAsync`, `describeAsync`,
`transcribeAsync`, `synthesizeAsync`) that wraps the sync one in
`BlockingTask` (see `src/shared/wrapper/blocking-task/blocking-task.hxx`,
which has a `void` specialization). Controllers/services on the event loop
MUST `co_await` the Async variant — never call the sync method directly.
Streaming variants marshal callbacks into the loop via `queueInLoop`.

### 13d. Shared LLM/Vision context safety

`LlmService`, `SttService`, `TtsService` own one shared inference context
each: access is serialized with a static `std::mutex` (LLM also reuses a
single `llama_batch` per generation loop). `VisionService` runs SmolVLM2
(ONNX) behind the same mutex pattern. The vision encoder output is cached
per image hash (frame cache) so repeated camera frames skip the most
expensive pass; input resolution is fixed at 512 by the SigLIP encoder.

### 14. JSON columns

- Unknown/dynamic structure → `Json::Value`
- Known structure → define a separate schema/struct (like `RolePermissionSchema`)

### 15. Dependencies

- `jwt-cpp/0.7.2` via Conan (HS256)
- `nlohmann_json/3.11.3` (pinned for jwt-cpp)
- `tomlplusplus/3.3.0` for config
- `qr-code-generator/1.8.0` (Nayuki, QR codes for the pairing banner).
  Target: `qr-code-generator::qrcodegencpp`, header `<qrcodegen/qrcodegen.hpp>`.
- `opencv/4.13.0` (headless, for scaled face image decoding)
- `onnxruntime/1.24.4` (STT/TTS via sherpa-onnx, Vision via SmolVLM2)
- **llama-cpp/b6565 via Conan** (latest stable on Conan Center) — powers the
  LLM only. Target: `llama-cpp::llama-cpp`. API note: b6565 uses
  `use_mmap`/`use_mlock` (no `load_mode`). Not a submodule.
- **Vision runs SmolVLM2-500M-Video-Instruct (ONNX int8)** from
  `HuggingFaceTB/SmolVLM2-500M-Video-Instruct`: 3 sessions (SigLIP vision
  encoder 512px → 64 image tokens, merged decoder with fp32 KV cache, token
  embeddings) + `tokenizer.json`, downloaded by `scripts/setup.sh` into
  `models/vision/smolvlm/`. ChatML prompt with expanded `<image>` block,
  fixed captioning instruction (pre-tokenized ids).
- **NO spdlog** — use Drogon's built-in logging (`LOG_INFO`, `LOG_WARN`, `LOG_FATAL`)
- **NO libsodium** — auth is face-based
- **NO ORM** — raw SQL via `DbService::client()->execSqlCoro()`
- **NO std::future** — use plain `std::thread` + `join` (e.g. ServiceRegistry parallel init)

### 16. Smart pointers

No raw owning pointers. Services use `std::unique_ptr` with custom deleters.
Raw pointers only for non-owning access (`.get()`).

### 17. Startup & database hygiene

- Services are initialized IN PARALLEL by `ServiceRegistry` (threads + join).
- `DbService::applyPragmas()` runs at every boot (WAL, synchronous=NORMAL,
  busy_timeout, mmap, foreign_keys) — pragmas are per-connection and do NOT
  survive a restart otherwise.
- SQLite uses 4 connections (`config.toml`) — the JwtFilter issues 2 queries
  per authenticated request.
- Release builds are machine-tuned: `-march=native` + `-flto=auto` on the app
  target only. `-Wall -Wextra` are always on; third-party includes are SYSTEM.

### 18. WebSocket sync engine

- Ruta WS: `/sync` con **solo `JwtFilter`** (sin `DeviceFilter`; el binding de
  dispositivo queda en HTTP). Los mensajes son `{type, payload}` y las
  respuestas usan `SocketEmitDto` `{operation, option(TableName), info}`.
  Errores: `{type:"<type>_error", status, error}`.
- Operaciones (`src/shared/contracts/sync-operation.hxx`): `InitialInfo=0`,
  `Synchronize=1` (creados/eliminados, incluye `notification` por usuario),
  `SynchronizeAuditLog=2` (diffs globales, tablas decididas por el backend
  según rol), `SynchronizeUserAuditLog=3` (diffs por usuario, filtrado por
  `sub`). Eventos en vivo: `Add=4`, `Delete=5`, `Log=6`.
- Queries de sync: usar `sync_query::buildSyncQuery(filter, Q1, Q2, Q3)`
  (función SÍNCRONA que devuelve query+args por valor) + `co_await
  client->execSqlCoro(query, argsRef)` directo. **NUNCA** capturar referencias
  en una coroutine lambda interna `[&]() -> Task` que suspende: el frame queda
  en una pila reutilizada y provoca use-after-free (crash). Tampoco pasar
  temporales a coroutines que guardan referencias: usa locals con nombre.
- `RoomManager` es clase de instancia con estado `thread_local` a nivel de
  archivo; ciclo de vida vía `RoomManagerServiceAdapter` (IService).

## Build Commands

```bash
# Dev (Debug)
cmake --preset dev
cmake --build --preset dev -j 8

# Prod (Release)
cmake --preset prod
cmake --build --preset prod -j 8
```

Before any commit, verify: `cmake --build --preset dev -j 8` passes with
**0 errors, 0 warnings**.

## File Naming

- Headers: `.hxx`
- Sources: `.cc`
- Tests: `*_test.cc`
- No `.h` or `.cpp` extensions.

## Key Files Reference

| File | Purpose |
|------|---------|
| `src/shared/enums.hxx` | All enum types + conversion helpers |
| `src/shared/schemas/*/` | DB row → C++ struct mapping |
| `src/shared/repositories/*/` | Data access layer |
| `src/shared/contracts/` | `Syncable`, `SyncFilter` base classes + `sync-operation.hxx` |
| `src/shared/access/` | `RoleAccess` centralizado (rol → tabla → permisos) usado por `RoleFilter` y sync |
| `src/shared/validation/` | Validation DSL (rules, macros, validator) |
| `src/filter/device/` | Device fingerprint extraction |
| `src/filter/jwt/` | JWT verification + refresh token validation |
| `src/filter/role/` | Role-based access control |
| `src/filter/valid-json/` | JSON body validation for POST/PATCH |
| `src/config/app-config.hxx` | Centralized responses + attribute keys |
| `src/shared/services/jwt/` | JWT sign/verify (HS256, instance class) |
| `src/shared/services/face/` | Face detection + recognition (ncnn) |
| `src/shared/services/llm/` | LLM inference (llama.cpp) |
| `src/shared/services/vision/` | Vision model inference |
| `src/shared/services/stt/` | Speech-to-text (whisper via sherpa-onnx) |
| `src/shared/services/tts/` | Text-to-speech (Supertonic 3) |
| `src/shared/services/sqlite/` | DB client access (`DbService::client()`) |
| `src/shared/services/config-service/` | `ConfigService` read + runtime writes (`setBool/...` persisten a `config.toml`, comentarios preservados) |
| `src/shared/services/room/` | `RoomManager` local (rooms por módulo/usuario, `thread_local`) |
| `src/shared/services/socket/` | `SocketService` (emitModule/emitUser) + `SocketEmitDto` |
| `src/shared/services/audit-log/` | Audit global con snapshot por día + emit |
| `src/shared/services/user-audit-log/` | Audit a nivel de usuario (syncable) |
| `src/shared/services/notification/` | Notificaciones por usuario (`createAndEmit` + `Add`) |
| `src/shared/services/notification-token/` | Push tokens por sesión |
| `src/shared/utils/json-diff/` | Diff JSON + snapshot (`JsonDiff`) |
| `src/shared/utils/json-util/` | `jsonToString`/`jsonFromString` |
| `src/feature/socket/sync/` | `SyncSocket` + `SyncService` + `SynchronizedService` + DTOs |
| `src/shared/wrapper/api-response/` | Standardized API response builder |
| `src/shared/wrapper/blocking-task/` | Coroutine awaiter for off-loop heavy work |
| `src/shared/wrapper/thread-budget/` | Adaptive thread sizing for AI services |
| `database/schema.sql` | DDL applied at startup |
| `config.toml` | Application + JWT config |
| `CONTEXT.md` | Full project history and decisions |
