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

### 4. Filter chain order

```
DeviceFilter → ValidJsonFilter → JwtFilter → RoleFilter
```

Registered via Drogon controller `ADD_METHOD_TO` macro using string names.
Never skip a filter in protected routes.

### 5. Responses & attribute keys

ALL error responses go through `AppConfig` (in `src/config/`):

```cpp
AppConfig::get401Response();                         // default message
AppConfig::get401Response("Custom message");          // custom message
AppConfig::get403Response();
AppConfig::get404Response();
```

Never call `ApiResponse::error()` directly from filters/controllers.

Attribute keys are centralized constants:
```
AppConfig::JWT_CTX_KEY    = "jwt_ctx"
AppConfig::DEVICE_CTX_KEY = "device_ctx"
```

### 6. Role-based access

Roles are checked by `RoleFilter` against `kRoleAccess` map in
`role-filter.cc`. To add/modify role permissions, edit ONLY that map.
No imperative if/else chains.

```
Owner    → full access (bypasses all checks)
Resident → GET + POST + PATCH on /camera, /zone, /reminder, /event, /person, /context-note, /auth
Guard    → GET only on /camera, /event, /person, /zone, /auth
Guest    → GET only on /camera, /auth
```

### 7. JWT auth flow

1. `DeviceFilter` extracts UA+IP → `DeviceContext` (stored as `"device_ctx"` attribute)
2. `JwtFilter` extracts token (Header Bearer / query `?token=` / cookie), verifies HS256,
   validates `refresh_token` in DB (is_valid=1, is_used=0, device_hash match),
   injects `JwtContext` as `"jwt_ctx"` attribute
3. `RoleFilter` reads `JwtContext.role` and checks against `kRoleAccess`

### 8. JSON columns

- Unknown/dynamic structure → `Json::Value`
- Known structure → define a separate schema/struct (like lynk's `RolePermissionSchema`)

### 9. Dependencies

- `jwt-cpp/0.7.2` via Conan (HS256)
- `nlohmann_json/3.12.0` (pinned for jwt-cpp)
- `tomlplusplus/3.3.0` for config
- **NO spdlog** — use Drogon's built-in logging (`LOG_INFO`, `LOG_WARN`, `LOG_FATAL`)
- **NO libsodium** — removed; auth will be face-based (InspireFace)
- **NO ORM** — raw SQL via `DbService::client()->execSqlCoro()`

### 10. Smart pointers

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
| `src/filter/device/` | Device fingerprint extraction |
| `src/filter/jwt/` | JWT verification + refresh token validation |
| `src/filter/role/` | Role-based access control |
| `src/config/app-config.hxx` | Centralized responses + attribute keys |
| `src/shared/services/jwt/` | JWT sign/verify (HS256) |
| `src/shared/services/sqlite/` | DB client access (`DbService::client()`) |
| `database/schema.sql` | DDL applied at startup |
| `config.toml` | Application + JWT config |
| `CONTEXT.md` | Full project history and decisions |
