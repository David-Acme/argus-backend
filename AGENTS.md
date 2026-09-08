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
- **`frontend/`** (sibling) — React Native app (Expo SDK 57 + Expo Router +
  Tailwind v4 via Uniwind). Its HTTP, auth, WatermelonDB (15 tables) and
  autonomous `/sync` layers are implemented, plus the full screen set
  (dashboard, agenda/projects/cameras/people/users, welcome onboarding,
  cross-device QR login, voice, avatar reactions). The app must paint
  persisted local data first and then react to live sync; backend contract
  changes must preserve that model.
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

Roles are checked centrally via `src/shared/access/role-access.hxx` (the
`kTableAccess` map: role → table → `RolePermission`). **`RoleFilter` (HTTP) and
the sync engine share that single source of truth** — to change a permission,
edit only that file. Never imperative if/else.

```
Owner    → full access. Receives the directory and invitation metadata.
Resident → CRUD on home-management resources; user read is limited to their own
           profile in both HTTP and sync projections.
Guard    → read-only security resources plus the local people directory. They
           never receive invitation data or portrait bytes in sync/list results.
Guest    → read-only permitted resources; user read is limited to their own
           profile.
```

Helpers: `hasAccess(role, table, perm)`, `readableTables(role)`,
`tableFromPath(path)`, `permissionForMethod(method)`, `hasHttpAccess(...)`.

### 7b. People, invitations and private portraits

- `user_invitation` is syncable metadata **only for Owner**. Never serialize,
  sync, log or return its opaque invitation token/hash. `UserInvitationSchema`
  must keep token-hash fields out of `toJson()`.
- User data has two scopes: Owner/Guard receive the directory; Resident/Guest
  receive only the row whose id is `JwtContext.sub`. Apply that same scope in
  HTTP list services and `SynchronizedService`; route permission alone is not
  enough.
- Invitations are created by Owner with a preselected non-owner role, expiry and
  capacity. The token is 256-bit opaque material; persist only SHA-256. Consume
  it atomically with enrollment and redemption recording. An invitation QR that
  the frontend closes/unmounts is revoked, so it cannot be reused from a prior
  preview.
- A role update is not a logout: persist first, call
  `SocketService::replaceRoleRooms`, then emit `AuthContextChanged` with
  `resync=true` to the user's room. Preserve the socket and user room. Only
  account deactivation invalidates refresh tokens and disconnects the device.
- Portrait objects are private server storage, never sync tables. Guard/Owner
  use `/portrait-preview/{userId}` to mint a requester-bound, short-lived,
  one-use capability and `/portrait-preview/{token}/content` to consume it.
  Consume atomically before storage retrieval; do not expose bucket paths,
  signed URLs, credentials, binary or token in logs. A successful view writes a
  `UserAction::Read` audit event with safe metadata only.
- Keep invitation creation/revoke/redemption, user creation/role/deactivation,
  and portrait viewing in the server audit history. Do not put audit-only
  private metadata in the sync stream.

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

**FaceDB** — vec0-backed embedding search (sqlite-vec, exact cosine KNN):
- `search(embedding)` → `optional<pair<int64_t, float>>` (personId, confidence)
- `insert(embedding, personId, faceEmbeddingId)` — rowid = face_embedding.id
- Threshold: `distance > 0.20` (80% minimum cosine confidence)

**FaceEmbedding repository** — canonical persisted embeddings (synced via the
sync engine). `face_vec` (vec0) is the search index only. No boot-time load:
vec0 persists in the DB.

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
single `llama_batch` per generation loop). `VisionService` runs LFM2.5-VL-450M
through llama.cpp + `libmtmd` behind the same mutex pattern (see section 15).
`VadService` is the exception that proves the rule: it is an **instance
class** (one per audio stream, LSTM state per instance) whose ONNX session is
shared file-static behind a mutex.

### 13e. Main LLM artifact

- The active conversation model is Liquid AI
  `LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf` under `models/llm/`. It is loaded by
  `LlmService` through `[llm].model_path`; the same path is the compiled
  fallback in `llm-service.cc`.
- `scripts/setup.sh` downloads the artifact only when missing, writes to a
  temporary `.part` file, verifies SHA-256
  `bb741ebb106d543e9de114b843a3d3d73d51c74b5801e69da2abde821a0cb3e1`, and
  atomically moves it into place. A mismatched existing file is replaced.
- Model weights are runtime artifacts and remain ignored by Git. Do not add a
  GGUF to the repository or silently change the active filename without also
  updating `config.toml`, the service fallback, `scripts/setup.sh`, the model
  notice and the relevant project context.

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
- `onnxruntime/1.24.4` (STT/TTS via sherpa-onnx, VAD via Silero ONNX)
- **Kùzu — NOT a dependency (removed).** The Phase 0 gate rejected both Kùzu
  candidates (upstream `kuzudb/kuzu` @ `v0.11.3` and the Vela fork
  @ `v0.12.0-vela.87bf0be`) as the memory engine: write+read interleaving on
  one serialized connection fails on both (upstream: checkpoint starvation,
  inserts 12 ms → 4–18 s under read load + `DirectedCSRIndex` asserts +
  SIGTERM-proof hangs; fork: any second connection crashes). The submodules,
  the build integration and the `labs/kuzu-probe` reproducer are deleted;
  the gate result lives in `CONTEXT.md`. The memory redesign's
  `SemanticGraph` is backed by the existing SQLite tables.
- **llama.cpp as a submodule** (`third_party/llama.cpp`, tag `b10305`) — powers
  the LLM **and** the VLM through `libmtmd`. Targets: `${ARGUS_LLAMA_TARGETS}`
  (= `llama mtmd`). Built with `LLAMA_BUILD_MTMD=ON`, everything else OFF.
  Vendored instead of taken from Conan because `llama-cpp/b6565` is the newest
  recipe on Conan Center, its `lfm2` projector still requires
  `mm.input_norm.*` (dropped by LFM2.5-VL), it ships CPU-only, and it exposes
  no mtmd component. API notes for b10305: `llama_model_params` uses
  `load_mode` (`LLAMA_LOAD_MODE_MMAP`), NOT `use_mmap`/`use_mlock`;
  `llama_sampler_init_penalties()` takes `n_vocab` as its first argument;
  `mtmd_input_text` has a `text_len` field that MUST be set.
  `GGML_VULKAN` is enabled automatically when Vulkan + glslc + SPIRV-Headers
  are present, otherwise the build falls back to CPU.
- **Vision runs LiquidAI LFM2.5-VL-450M (GGUF Q8_0 + mmproj F16)** through
  llama.cpp + `libmtmd`, downloaded by `scripts/setup.sh` into
  `models/vision/lfm2vl-25/`. Unlike the previous SmolVLM2 ONNX pipeline it
  takes **arbitrary prompts**, so `VisionRequest::prompt` is real. Cost is
  driven by input resolution because the model tiles dynamically: `[vision]
  max_input_px` (default 384) is the knob, not `image_max_tokens` (which only
  trims per-tile detail and makes the model stop reading text below 256).
  The prompt is built as ChatML by hand: `llama_chat_apply_template()` is NOT
  a jinja parser and mangles LFM2.5's template.
- **sqlite-vec** (vendored in `third_party/sqlite-vec/`, MIT/Apache-2.0) — vec0
  vector search; compiled with `SQLITE_CORE`, registered via
  `sqlite3_auto_extension` in `DbService::installExtensions()` (must run AFTER
  Drogon's first connection — see CONTEXT.md ordering note). FTS5 (bm25,
  unicode61, trigram) is enabled via the conan option
  `sqlite3/*:enable_fts5=True` (Drogon rebuilt once).
- **MemoryService** (`src/shared/services/memory/`) — long-term memory over a
  SQLite semantic graph (NOT the legacy `memory_l1`): `memory_entity`/alias
  (person frames), `memory_fact` (upsert closes the previous open fact via
  `supersedes`), `memory_edge`, `memory_episode` (compaction/system-event
  summaries, now recallable), `memory_procedure`. Capture: deterministic
  `RuleParser`+`PhraseCatalog` (vocabulary is static per-language constants in
  `src/shared/vocabulary/`, never DB tables) → `TieredExtractor` (lexicon tier
  inline, NuExtract tier off-turn).
  Recall (`GraphRecall`): entity-anchored (recursive CTE) → FTS5 → vec0
  semantic tier with store-size-aware margin gates → episode tier; anaphora
  via `WorkingMemory.activeEntities`; block injected at the TAIL of the user
  turn. Background worker: embed (chunks+synonyms+vector dedup with
  priority-merge), compaction with the MAIN LlmService (re-enqueues when
  busy), L3 profile (deterministic persona/instruction selection + optional
  off-turn LLM polish). Facade `MemoryService`; DB via `SqliteGraph`/`VecDb`
  (mutex-serialized, prepared statements are RAII `SqliteStmt`). Embeddings:
  `multilingual-e5-small` int8 ONNX, loaded lazily. Full details in CONTEXT.md.
- **fastText** (submodule `third_party/fastText`, pinned 1f12150 = v0.9.2 +
  local C++20 patch) — supervised text classification for `IntentService`
  (`src/shared/services/intent/`): static intent detection for implicit tool
  activation (camera + memory_save), `loss softmax` + `none` class, trained
  from `labs/intent-data/` with `labs/intent-probe` (`--intent-train`,
  `--intent-quantize` → `models/intent/argus-intent.ftz`, `--intent-check`).
  Thread-safe read-only predict (~20 µs), degraded to literal keywords when
  the model is missing.
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

### 17b. Local deployment

- Argus remains self-hosted: the backend, database, certificates, models and
  private files run on the user's hardware. A tunnel may route remote traffic,
  but neither client contracts nor backend code should need to switch between
  LAN and tunnel endpoints.
- Native backend development is the default: run `scripts/setup.sh` to create
  the per-installation 0600 `config.toml` from `config.toml.example` and the
  ignored lab overlay from `labs/config.toml.example`, then run
  `build/dev/argus-gateway/argus-gateway`. Never commit, print, log or send
  instance secrets to the frontend.
- Production-style deployment is container-only: `argus-deploy/` builds one
  source-built image and `argus-deploy/docker-compose.yml` runs the gateway and
  the domain services from it. There is no separate local compose stack.
- Object storage is opt-in through `S3StorageService`
  (`storage.mode = "s3"` in `config.toml`); no object store ships with the
  repository. Use the service, never direct ad-hoc HTTP from feature code.
- Development uses a fresh schema when the developer explicitly resets the
  local DB. Do not silently delete, migrate or recreate a user's database as a
  side effect of a feature; ask/require an explicit development reset.

### 18. WebSocket sync engine

- WS route: `/sync` with **`DeviceFilter` + `JwtFilter`** so device binding is
  enforced on every authenticated transport. Messages are `{type, payload}` and responses use
  `SocketEmitDto` `{operation, option(TableName), info}`.
  Errors: `{type:"<type>_error", status, error}`.
- Operations (`src/shared/contracts/sync-operation.hxx`): `InitialInfo=0`,
  `Synchronize=1` (initial bootstrap plus creations/deletions; includes
  `notification` per user), `SynchronizeAuditLog=2` (global field diffs; the
  backend selects tables by role), `SynchronizeUserAuditLog=3` (recipient
  field diffs, filtered by `sub`). Live events: `Add=4`, `Delete=5`, `Log=6`.
- **Normal rows are creation-only after bootstrap**: `Synchronize` pages by
  `created_at`; do not switch it to `updated_at`/`syncAt` to represent an
  update. Every persisted update/revocation must instead publish a granular
  audit change. New records are `Add` with the complete current row; deletion
  events carry only `id` and `deletedAt`.
- Audit requests use monotonic SQLite ids, not timestamps. A client asks
  `{findLast:true}` for a `watermarkId`, then pages
  `afterId < id <= endId` in ascending order. `afterId=0` is valid to establish
  an empty baseline; `endId` is a positive bound. `SynchronizedService` returns
  `nextCursorId` only after the bounded query shape is accepted.
- `audit_log` is module/global and `user_audit_log` is recipient-scoped. Their
  `changes` payload comes from `JsonDiff::createFlatDiff(before, after)`: only
  changed fields with their previous/current values, never a replacement record.
  Daily compaction merges a record's diff and inserts the compacted result as a
  new audit row so its id advances and reconnecting clients converge.
- `SyncAuditService` (`src/shared/services/sync-audit/`) is the feature-level
  publishing facade. Capture `before` before a repository mutation, capture
  `after` once it succeeds, then call `publishModule` or `publishUsers` with the
  correct audience. Do not hand-build `Log` payloads or emit a full `Add` for an
  update. Recipient lists must be deduplicated and must never contain a secret.
- `PATCH /notification/read` follows the same rule: select the unread rows
  before mutation, update only those rows, and publish their user audit diffs.
- Sync queries: use `sync_query::buildSyncQuery(filter, Q1, Q2, Q3)` (a
  SYNCHRONOUS function returning query+args by value) then `co_await
  client->execSqlCoro(query, argsRef)` directly. **NEVER** capture references
  in an inner `[&]() -> Task` coroutine lambda that suspends: the frame lands
  on a reused stack and causes a use-after-free (crash). Do not pass
  temporaries to coroutines that store references either — use named locals.
- `RoomManager` is an instance class with file-level `thread_local` state; its
  lifecycle goes through `RoomManagerServiceAdapter` (IService).

### 19. Modern C++20 everywhere

Write C++20 as if starting today; carry nothing old forward. No owning raw
pointers (rule 16), no C-style casts, no `typedef` (use `using`), no `NULL`
(use `nullptr`), no C arrays, no `std::bind`, no `printf` family. Reach for
`string_view`/`span` at hot boundaries, `constexpr`, designated initializers,
structured bindings, ranges/algorithms over manual index loops, and
`std::make_unique`/`make_shared` by default. Modernizing existing code is a
normal part of any task that touches it — do not preserve old idioms out of
consistency with their file.

### 20. Comment discipline

Comments exist ONLY at class, namespace or function scope, short and direct.
No comments attached to individual statements, no multi-line doc blocks, no
commented-out code. If a statement needs a comment to be understood, rewrite
the statement. The "why" of a design decision goes to CONTEXT.md, not to the
code.

### 21. Efficiency first, architecture intact

Every write/read path is written with cost in mind:

- One transaction or multi-row statement over N single statements in a loop
  (fan-out writes, audit batches, replica upserts, notification inserts).
- Prepared statements and bulk `IN (...)` clauses over repeated round-trips.
- No allocation churn in hot paths (string concatenation in loops, per-request
  vector copies, JSON re-serialization round-trips).
- No blocking IO inside event-loop callbacks (trantor/cnats).
- Efficiency never changes semantics: the sync fan-out stays row-scoped and
  per-recipient; batching applies only where the observable behavior is
  identical.

### 22. Databases optimized, not limited

Each service's DB is tuned for its own hot queries (covering indices on sync
cursors and join columns, WAL + PRAGMA tuning, FTS5 where already present)
WITHOUT tricks that box in future work: no manual partitioning, no aggressive
denormalization, no over-indexing every column. New features must fit the
existing schema shape or extend it additively.

### 23. Feature-based architecture + shared SDK

- Every service follows the feature-based layout of the monolith
  (`src/feature/...`, `src/filter/...`, `src/shared/...`, `src/server/...`).
  Do not invent a new layout per service.
- Cross-service calls go through the SHARED client/SDK layer, never through
  per-service hand-rolled clients. Wire handling (URL/connect/envelope parse/
  retry/auth token pass-through) lives in one place.
- Each service owns its subroute (`/camera`, `/reminder`, ...) and its own
  auth handling via the shared filter/auth SDK — no service reaches into
  another service's database for auth.
- Dead code, unused folders and duplicated copies are removed in the same
  change that introduces their replacement.

### 24. Respect the monolith structure, never speculative structure

Folder architecture, naming and ordering mirror the legacy monolith
(`src/feature`, `src/shared`, `src/filter`, `src/server`, `database/`,
`tools/`). Do not implement structure for its own sake: no abstraction, layer
or directory that has no current consumer.

### 25. Build ergonomics: the folder IS the module

Build import is by module name, never by listing files in consumers.
Every shared feature/repo/SDK piece lives in its OWN folder and is declared
ONCE, in its home, through the project helpers:

```cmake
# src/shared/vad/CMakeLists.txt — the module declares its sources + deps
argus_module(NAME vad
  SOURCES vad-service.cc
  DEPENDS onnxruntime)

# SDK modules wrap generated gRPC stubs — consumers never see protobuf
argus_sdk_module(NAME identity PROTO identity.proto)
```

- Parents auto-discover modules (a loop over subdirectories) — adding a
  module = creating a folder; adding a file = dropping it in its module.
  NO ONE edits another module's CMakeLists and NO consumer lists `.cc`
  files.
- Consumers link by name: `target_link_libraries(argus-voice PRIVATE
  argus::vad argus::sdk-identity)`. Include paths travel with the target.
- Services bootstrap through a `argus_service()` helper (presets,
  EXCLUDE_FROM_ALL, ports) instead of copy-pasted CMake blocks.
- Explicit source lists stay ONLY inside the module's own CMakeLists.
  `file(GLOB)` for sources is forbidden (fragile); auto-discovery of
  module folders (GLOB over `*/CMakeLists.txt`) is the only allowed glob.

## Build Commands

`ARGUS_BUILD_LABS` defaults OFF: the `labs/` probes and benches are a
developer opt-in (`cmake --preset dev -DARGUS_BUILD_LABS=ON`).

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
- Tests: `*-test.cc` (e.g. `user-service-test.cc`)
- No `.h` or `.cpp` extensions.

## Key Files Reference

| File | Purpose |
|------|---------|
| `src/shared/enums.hxx` | All enum types + conversion helpers |
| `src/shared/schemas/*/` | DB row → C++ struct mapping |
| `src/shared/repositories/*/` | Data access layer |
| `src/shared/contracts/` | `Syncable`, `SyncFilter` base classes + `sync-operation.hxx` |
| `src/shared/access/` | Centralized `RoleAccess` (role → table → permissions), used by `RoleFilter` and sync |
| `src/shared/validation/` | Validation DSL (rules, macros, validator) |
| `src/filter/device/` | Device fingerprint extraction |
| `src/filter/jwt/` | JWT verification + refresh token validation |
| `src/filter/role/` | Role-based access control |
| `src/filter/valid-json/` | JSON body validation for POST/PATCH |
| `src/config/app-config.hxx` | Centralized responses + attribute keys |
| `src/shared/services/jwt/` | JWT sign/verify (HS256, instance class) |
| `src/shared/services/face/` | Face detection + recognition (ncnn) — FaceDB = vec0 index (sqlite-vec) |
| `src/shared/services/llm/` | LLM inference (llama.cpp) |
| `src/shared/services/embedding/` | `EmbeddingService` (multilingual-e5-small int8 ONNX) + `UnigramTokenizer` |
| `src/shared/services/intent/` | `IntentService` (fastText supervised: cámara/memory_save implícitos) + adapter IService |
| `src/shared/services/memory/` | `MemoryService`/`SemanticGraph`(`SqliteGraph`)/`GraphRecall`/`MemoryFormation`/`RuleParser`/`PhraseCatalog`/`EntityResolver`/`ToolParser` — semantic-graph long-term memory (async worker, episode recall, L3 profile) |
| `src/shared/vocabulary/` | Static per-language memory vocabulary (es/en): `PhraseSeed`/`LexiconSeed` constants — no DB tables |
| `src/shared/services/sqlite/` | DB client access (`DbService::client()`, extensions) + `VecDb` (vec0 connection) |
| `src/shared/services/vision/` | VLM inference: LFM2.5-VL-450M via llama.cpp + libmtmd (arbitrary prompts, caption cache) |
| `src/shared/services/vad/` | `VadService` — Silero VAD v5 as an **instance** class (per-stream LSTM, shared ONNX session), with the turn-quality gate |
| `src/shared/services/stream/` | go2rtc manager, `StreamHub` (fMP4 over `/sync`, per-connection credit window, lock order `hubMutex_ → Upstream::mtx`), `Fmp4Reader` (encoding from headers, whole fragments), `MediaRelay` (incl. `snapshotBytes`) |
| `src/shared/wrapper/audio/` | `AudioResampler` (stateful sinc) + `SampleRing` — every block-processed audio path MUST use these, never a custom conversion |
| `src/shared/services/stt/` | Speech-to-text via sherpa-onnx (default `nemo_transducer` FastConformer RNN-T, es/en; whisper/canary/nemo_ctc/omnilingual selectable) |
| `src/shared/services/tts/` | Text-to-speech (Supertonic 3) |
| `src/shared/services/tapo/` | Tapo camera local protocols: control (`stok` + `securePassthrough`, legacy fallback) and the 8800 talk channel (Digest + MPEG-TS PCMA) |
| `src/shared/services/storage/` | `S3StorageService` (RustFS S3, SigV4 en `s3-signing.hxx`) + `PrivatePortraitService` (objetos privados, lectura vía capability one-use) |
| `src/shared/services/reaction/` | `ReactionEngine` — reacciones de turno por prioridad de señales → `voice:event` (significado, nunca nombres de expresión) |
| `src/shared/repositories/{user-invitation,portrait-*,device-login-challenge}/` | Dominio people: invitaciones (hash-only), capabilities de retrato, retos de login cruzado |
| `src/shared/wrapper/cancellation/` | `CancellationToken` shared across streaming AI/audio paths |
| `labs/` | Standalone binaries for prototyping and validating new capabilities against real hardware before wiring them into the backend |
| `labs/tapo-probe/` | `argus-tapo-probe` — validates the camera protocols against real hardware |
| `labs/voice-test/` | `argus-voice-test` — STT → LLM → TTS conversation loop with Silero VAD (memory via `--memory-user <id>`) |
| `labs/memory-probe/` | `argus-memory-probe` — memory schema/capture/tool/embedding/recall checks + bench |
| `labs/reaction-probe/` | `argus-reaction-probe` — ladder completo de reacciones es/en (23 checks) |
| `src/shared/services/config-service/` | `ConfigService` read + runtime writes (`setBool/...` persisten a `config.toml`, comentarios preservados) |
| `src/shared/services/room/` | `RoomManager` local (rooms por módulo/usuario, `thread_local`) |
| `src/shared/services/socket/` | `SocketService` (emitModule/emitUser) + `SocketEmitDto` |
| `src/shared/services/audit-log/` | Audit global: diffs por campo, compactación diaria e id monotónico para sync |
| `src/shared/services/user-audit-log/` | Audit por destinatario: diffs por campo, compactación diaria e id monotónico para sync |
| `src/shared/services/sync-audit/` | Fachada central para publicar diffs module/user tras mutaciones de features |
| `src/shared/services/notification/` | Notificaciones por usuario: `Add` al crear y user-audit granular al marcar lectura |
| `src/shared/services/notification-token/` | Push tokens por sesión |
| `src/shared/utils/json-diff/` | Diff JSON + snapshot (`JsonDiff`) |
| `src/shared/utils/json-util/` | `jsonToString`/`jsonFromString` |
| `src/feature/socket/sync/` | `SyncSocket` + `SyncService` + `SynchronizedService` + DTOs |
| `src/shared/wrapper/api-response/` | Standardized API response builder |
| `src/shared/wrapper/blocking-task/` | Coroutine awaiter for off-loop heavy work |
| `src/shared/wrapper/thread-budget/` | Adaptive thread sizing for AI services |
| `config.toml` | System application + JWT config |
| `labs/config.toml` | Ignored lab-only overlay |
| `CONTEXT.md` | Full project history and decisions |
