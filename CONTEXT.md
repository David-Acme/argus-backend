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
- **spdlog is NOT added** — Drogon already provides logging. Do not add spdlog.
- **Smart pointers only** — no raw owning pointers. All service resources use
  `std::unique_ptr` with custom deleters. `LlamaModel`, `LlamaContext`,
  `SherpaOnnxOnlineRecognizer`, `mtmd_context` — all wrapped in smart pointers.
  Raw pointers only for non-owning access (`.get()`).
- **Services use hardcoded model paths** — like `TtsService::init()`, all
  services know their model paths internally (no parameters). Paths live at
  `models/{llm,stt,vision}/` under the project root.
- **Conan 2** for deps (`conanfile.txt` + `CMakePresets.json`). CMake presets:
  `dev` (Debug) and `prod` (Release), generator Ninja.
- **Git submodules** under `third_party/` for libs that change rarely and we want
  to control: `fastText`, `inspireface` (InspireFace). Built via `add_subdirectory`
  with `EXCLUDE_FROM_ALL`.
- Convention: Conventional Commits in English. Remote `git@github.com:David-Acme/argus-backend.git`,
  branch `main`.

## Dependency resolution notes (Conan conflicts, learned the hard way)

- `nlohmann_json` pinned to **3.12.0** — jwt-cpp/Drogon use this version.
- `opencv/4.13.0` built **headless**: `with_protobuf=False`, `with_eigen=False`,
  `with_ffmpeg=False`, `with_wayland=False`, `with_gtk=False`, `with_vulkan=False`.
  Reason: the ConanCenter prebuilt opencv pulls X11/Wayland system deps
  (`libxres`, etc.) that need sudo to install; headless avoids that and is leaner.
- `eigen/3.4.0` used for tracker/geometry (Kalman optional behind a flag).
- protobuf: unified via opencv `with_protobuf=False` — no protobuf in the graph.
- **conanfile.txt cannot resolve version conflicts** (no `override=True`/`force`).
  Conflict resolution is done by pinning versions + disabling the offending option
  in the consuming package. Keep this pattern.

## fastText CMake warnings (silenced)

- `third_party/CMakeLists.txt`: `CMAKE_POLICY_VERSION_MINIMUM` bumped to 3.10, and
  `CMAKE_CXX_STANDARD` restored to 20 after the fastText `add_subdirectory`.
- `scripts/setup-dev.sh` / `setup-prod.sh`: after submodule init, patch fastText's
  `set(CMAKE_CXX_STANDARD 17)` → `20` in place (submodule stays git-clean; re-applied
  each init). Do NOT edit the fastText submodule file directly.

## InspireFace (opt-in)

- Integrated behind `option(ARGUS_BUILD_INSPIREFACE OFF)` in `third_party/CMakeLists.txt`.
- OFF by default because InspireFace is heavy and pulls its own deps (MNN, InspireCV)
  that are **NOT git submodules** — they must be cloned manually into
  `third_party/inspireface/3rdparty/{MNN,InspireCV}` (see that repo's README), plus
  model packs downloaded. Also needs a license for commercial use (insightface.ai).
- `setup_submodules()` initialises nested submodules for InspireFace, but the MNN/
  InspireCV clones are still manual.

## Current build state

- Build is **green**: `cmake --preset dev` + `cmake --build --preset dev -j 8`
  succeeds; server starts on `0.0.0.0:7024`.
- fastText linked (static, `fasttext-static_pic`). InspireFace not linked (OFF).
- sherpa-onnx linked (static, `sherpa-onnx-c-api`) via git submodule. Uses
  Conan's onnxruntime (no duplicate FetchContent download).
- Services: TtsService, LlmService, SttService, VisionService — all follow the
  same static `init()`/`shutdown()` pattern with internal try/catch.
- **Filters**: `DeviceFilter` → `ValidJsonFilter` → `JwtFilter` → `RoleFilter`
  chain operational. JWT uses HS256, refresh tokens stored in `refresh_token`
  table with device binding (`device_hash`). Roles: owner/resident/guard/guest.
- **libsodium removed** — authentication will be face-based via InspireFace
  (no password hashing needed).

## Dev tooling: tree-sitter + MCP

- **For development only** — not part of the Argus runtime. Uses tree-sitter
  (C++ grammar) + MCP to give AI assistants structured, minimal-token context
  while developing the project.
- Python 3 venv at `.venv/` (gitignored) with `tree-sitter` + `tree-sitter-cpp`.
- All source files parsed once at startup, indexed in memory (~117 symbols across
  24 files). Queries return only the relevant AST nodes — signatures, classes,
  methods — never raw file dumps.

### MCP Server (`mcp-server.py`) — primary dev tool

- Configured in `opencode.json` (auto-enabled for this project):
  ```json
  "mcp": {
    "argus-context": {
      "type": "local",
      "command": [".venv/bin/python3", "mcp-server.py"]
    }
  }
  ```
- **Resources** (URI-addressed data):
  - `argus://overview` — project overview (1 query)
  - `argus://symbols` — full compact symbol index (1 query)
  - `argus://file/{path}` — compact context for one file
  - `argus://class/{name}` — full class/method signatures
- **Tools** (callable by AI):
  - `search_symbols(query, limit)` — find symbols by name
  - `get_class_info(name)` — class details (methods, members, access)
  - `get_function_info(name)` — function signatures across codebase
  - `get_dependencies(path)` — include graph for a file
  - `get_file_symbols(path)` — all symbols in a file
  - `get_project_stats()` — code statistics
  - `find_callers(name, limit)` — find call sites of a function
  - `find_symbol_occurrences(name, limit)` — text-grep for symbols
  - `read_lines(path, start_line, count)` — read specific file lines
  - `search_comments(pattern, limit)` — search C++ comments
  - `find_todo()` — list TODO/FIXME/HACK/TEMP markers
  - `get_includes_tree(path, max_depth)` — recursive include deps
- All responses return focused, minimal-token output. Token savings vs full
  source: **~80-95%** per query.

### Static context (fallback)

```
./scripts/dev-context.sh                     # stdout
./scripts/dev-context.sh CONTEXT_SYMBOLS.md  # to file
```

Generates a one-shot Markdown symbol index (~15 KB vs ~58 KB raw source, ~75%
smaller). Less flexible than MCP but useful for offline reference.

## Application features (in-app `ContextBuilder`)

- The app (**not** the dev tooling) will embed tree-sitter via C++ binding
  (`nsumner/cpp-tree-sitter`) inside a `ContextBuilder` service.
- This is part of the `vision-core` engine (Planned architecture below), not
  a migration of the Python MCP server. The MCP server stays as-is for dev.
- Same AST extraction concepts port to C++ for the app's own context needs.

## Planned architecture (not yet implemented)

`vision-core` engine (CameraManager, FramePreprocessor, Detector, Tracker,
FaceRecognizer, BehaviorAnalyzer, ZoneManager, EventManager, SnapshotManager,
ContextBuilder). Tracker: start with IoU+distance, **Kalman optional behind a flag**
(Eigen). Rule Engine / Context Builder / Behavior Analyzer are own IP. Communication:
WebSocket + TLS + own tunnel (LAN when local, tunnel when remote). Memory: SQLite +
RustFS for blobs. Profiles: light / balanced / advanced.

## Open questions / next steps

- Scaffold `vision-core` (separate lib or `src/vision-core/` — TBD when dev starts).
- Decide YOLO model export format behind `IVisionProvider` (ONNX / TorchScript / TFLite).
- RustFS integration for snapshots/video/audio blobs.

## Coding conventions & architecture (ALWAYS follow)

### Enums for constrained strings

All DB columns with CHECK constraints (e.g. `role`, `severity`, `record_mode`,
`zone_type`, `status`, `action`) MUST use a C++ `enum class` defined in
`src/shared/enums.hxx`. Each enum has `toString()` / `fromString()` helpers.
Never use raw `std::string` for these fields in schemas, repositories, or
filters.

**Available enums:**
- `UserRole` — owner, resident, guard, guest
- `EventSeverity` — info, warning, critical
- `CameraRecordMode` — events, continuous
- `ZoneType` — monitor, alert, exclude
- `ReminderDetailStatus` — pending, in_progress, done, blocked
- `UserAction` — create, update, delete

**Usage in schemas:** Use the enum type directly. `Row` constructor calls
`fromString()`. `toJson()` calls `toString()`.

### Parameter structs (functions with 3+ params)

ANY function with 3 or more parameters MUST use a struct defined in the
corresponding `*-query.hxx` file (at the bottom, after the query namespace).
This prevents breaking changes when adding new fields.

**Naming convention:** `{Entity}CreateInput`, `{Entity}UpdateInput`.

**Location:** Structs live in the query file (e.g. `camera-query.hxx`),
at file scope, after the closing `}` of the query namespace.

**Example:**
```cpp
// camera-query.hxx
namespace camera_query { /* SQL strings */ }

struct CameraCreateInput {
  std::string name;
  CameraRecordMode recordMode{CameraRecordMode::Events};
  std::optional<int64_t> retentionDays;
};

// camera-repository.hxx
drogon::Task<CameraSchema> create(const CameraCreateInput& input) const;
```

**JSON columns:** Use `Json::Value` for dynamic/metadata JSON. If the structure
is known, define a separate schema/struct (like lynk's `RolePermissionSchema`).

### Repository pattern

```
src/shared/repositories/{entity}/
  {entity}-query.hxx      — SQL strings + param structs
  {entity}-repository.hxx — class declaration
  {entity}-repository.cc  — implementations
```

- Always use `using namespace {entity}_query;` in `.cc` files.
- Use `co_await` with `DbService::client()` for DB operations.
- Re-fetch after INSERT/UPDATE via `findById()` to get DB-generated values.
- Soft-delete via `UPDATE ... SET deleted_at = strftime('%s', 'now')`.
- Syncable repos implement `find/findDeleted/findLast/findLastDeleted` with
  timestamp-range queries.

### Filter chain order

```
DeviceFilter → ValidJsonFilter → JwtFilter → RoleFilter
```

- **DeviceFilter**: Extracts device fingerprint (UA+IP) → injects `DeviceContext`
  into attributes keyed by `AppConfig::DEVICE_CTX_KEY`.
- **JwtFilter**: Extracts token (Header/Bearer, query param, cookie), verifies
  HS256 JWT via `JwtService::verifyAccess()`, validates `refresh_token` in DB
  (is_valid=1, is_used=0, device_hash match), injects `JwtContext` into
  attributes keyed by `AppConfig::JWT_CTX_KEY`.
- **RoleFilter**: Reads `JwtContext.role` (UserRole enum), checks against
  declarative `kRoleAccess` map in `role-filter.cc`. Owner = full access.
  No imperative if/else chains — just add entries to the map.

### Centralized response/attribute keys

All response helpers and attribute keys live in `src/config/app-config.{hxx,cc}`:

| Symbol | Purpose |
|--------|---------|
| `AppConfig::JWT_CTX_KEY` | `"jwt_ctx"` attribute key |
| `AppConfig::DEVICE_CTX_KEY` | `"device_ctx"` attribute key |
| `AppConfig::get401Response(msg?)` | Unauthorized response |
| `AppConfig::get403Response(msg?)` | Forbidden response |
| `AppConfig::get404Response(msg?)` | Not found response |

All three response methods accept an optional `message` parameter (defaults to
a generic message). Never call `ApiResponse::error()` directly from filters
or controllers — always use `AppConfig`.

## Database schema decisions

### Tables

- `user` — system access accounts (login, auth).
- `person` — people known to the camera (face recognition subjects), linked to
  FeatureHub via `feature_hub_id`.
- `event` — detected incidents/events from cameras.
- `person_event` — many-to-many: which persons appear in which events.

### User roles

- `owner` — full access: configure system, manage users, view all cameras,
  analyze events, access chat.
- `resident` — view live cameras and event history, no configuration.
- `guard` — view cameras, receive alerts, mark incidents as reviewed.
- `guest` — view live cameras only (no history, no chat).

### Key columns

- `user.is_active INTEGER NOT NULL DEFAULT 1` — 0 = account disabled, login
  rejected regardless of credentials.
- `person.feature_hub_id INTEGER UNIQUE` — the `allocId` returned by InspireFace
  FeatureHub, linking a known face to a person record.
- `person.first_seen_at / last_seen_at INTEGER` — Unix timestamps (UTC) for
  tracking presence history.
- `person.deleted_at INTEGER NULL` — soft delete (keep face data, mark as removed).

### Timestamps

All timestamps use `INTEGER` (Unix epoch, seconds, UTC):
```sql
created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
updated_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
deleted_at INTEGER NULL
```

### Face recognition

- InspireFace FeatureHub manages embeddings internally (512 floats, cosine
  similarity). The app does NOT store embedding vectors in its own tables.
- The `feature_hub_id` in `person` is the only link needed.
- InspireFace is pose-invariant: frontal, profile, tilted — all match the same
  person automatically via cosine similarity.
