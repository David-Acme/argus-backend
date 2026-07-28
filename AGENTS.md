# AGENTS.md — Argus Backend AI Agent Instructions

> This file is read by AI coding assistants before any code generation task.
> It defines the project's architecture, conventions, and constraints.

## Project Identity

- **Argus** — 100% local AI home security + virtual assistant platform.
- **C++20**, Drogon HTTP/WebSocket, SQLite (async `DbClient`, no ORM).
- **Conan 2** + **CMake presets** (`dev` = Debug, `prod` = Release).

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

ANY function with 3+ parameters MUST define a struct in the corresponding
`*-query.hxx` file, after the query namespace. Convention:

```
{Entity}CreateInput, {Entity}UpdateInput
```

Never write raw multi-parameter method signatures in repository headers.

### 3. Repository structure

```
src/shared/repositories/{entity}/
  {entity}-query.hxx      — SQL strings (namespace) + param structs (global)
  {entity}-repository.hxx — class declaration (uses structs from query file)
  {entity}-repository.cc  — implementations (uses `using namespace *_query`)
```

- `using namespace {entity}_query;` ALWAYS in `.cc` files.
- Re-fetch after INSERT/UPDATE via `findById()`.
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
```

Never call `ApiResponse::error()` directly from filters/controllers.

Validation errors use `ApiResponse::validationError(fieldErrors)` → 422.

Attribute keys are centralized constants:
```
AppConfig::JWT_CTX_KEY    = "jwt_ctx"
AppConfig::DEVICE_CTX_KEY = "device_ctx"
```

### 7. Role-based access

Roles are checked by `RoleFilter` against `kRoleAccess` map in
`role-filter.cc`. To add/modify role permissions, edit ONLY that map.
No imperative if/else chains.

```
Owner    → full access (bypasses all checks)
Resident → GET + POST + PATCH on /camera, /zone, /reminder, /event, /person, /context-note, /auth
Guard    → GET only on /camera, /event, /person, /zone, /auth
Guest    → GET only on /camera, /auth
```

### 8. JWT auth flow

1. `DeviceFilter` extracts UA+IP → `DeviceContext` (stored as `"device_ctx"` attribute)
2. `JwtFilter` extracts token (Header Bearer / query `?token=` / cookie), verifies HS256,
   validates `refresh_token` in DB (is_valid=1, is_used=0, device_hash match),
   injects `JwtContext` as `"jwt_ctx"` attribute
3. `RoleFilter` reads `JwtContext.role` and checks against `kRoleAccess`

### 9. JWT conventions

- **sub** = `userId` (stringified), **iss** = `"argus"`
- **NO role in JWT** — role is fetched from DB by JwtFilter on every request
- **Dual secrets:** `jwt.secret` for access tokens, `jwt.refresh_secret` for refresh tokens
- **Refresh token rotation:** single-use (`is_used=1` after first rotation), replay-protected
- JwtService is an **instance class** — secrets read once in constructor

### 10. DTO conventions

**Request DTOs** — self-validating, header + cc file:
```
src/feature/api/{resource}/dtos/
  {action}-dto.hxx     — struct + form_json() / form_multipart() factory
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

### 11. Validation DSL

All DTO validation uses the macro DSL in `src/shared/validation/`:

```cpp
LoginDto LoginDto::form_json(const Json::Value& json) {
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
`IS_VALID_TIMESTAMP`, `IS_POSITIVE_TIMESTAMP`, `IS_BOOLEAN`, `CUSTOM_LAMBDA`.

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

**FaceDB** — HNSW index for high-performance embedding search:
- `search(embedding)` → `optional<pair<int64_t, float>>` (personId, confidence)
- `insert(embedding, personId)` — add to index
- Threshold: `dist > 0.20` (80% minimum inner-product confidence)

**FaceEmbedding repository** — persists embeddings to SQLite (hex-encoded BLOB).
Loaded at startup via `FaceDB::loadFromDb()`.

### 14. JSON columns

- Unknown/dynamic structure → `Json::Value`
- Known structure → define a separate schema/struct (like `RolePermissionSchema`)

### 15. Dependencies

- `jwt-cpp/0.7.2` via Conan (HS256)
- `nlohmann_json/3.11.3` (pinned for jwt-cpp)
- `tomlplusplus/3.3.0` for config
- `opencv/4.13.0` (headless, for face image decoding)
- `llama-cpp/b6565` (LLM + Vision inference)
- `onnxruntime/1.24.4` (STT via sherpa-onnx)
- **NO spdlog** — use Drogon's built-in logging (`LOG_INFO`, `LOG_WARN`, `LOG_FATAL`)
- **NO libsodium** — auth is face-based
- **NO ORM** — raw SQL via `DbService::client()->execSqlCoro()`

### 16. Smart pointers

No raw owning pointers. Services use `std::unique_ptr` with custom deleters.
Raw pointers only for non-owning access (`.get()`).

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
| `src/shared/contracts/` | `Syncable`, `SyncFilter` base classes |
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
| `src/shared/wrapper/api-response/` | Standardized API response builder |
| `database/schema.sql` | DDL applied at startup |
| `config.toml` | Application + JWT config |
| `CONTEXT.md` | Full project history and decisions |
