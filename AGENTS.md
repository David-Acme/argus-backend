# AGENTS.md — Argus Backend AI Agent Instructions

> This file is read by AI coding assistants before any code generation task.
> It defines the project's architecture, conventions, and constraints.

## Project Identity

- **Argus** — 100% local AI home security + virtual assistant platform.
- **C++20**, Drogon HTTP/WebSocket, SQLite (async `DbClient`, no ORM).
- **Conan 2** with one `conanfile.txt` at the repository root; **`dev`** =
  Debug, **`prod`** = Release.

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
`zone_type`, `status`, `action`) MUST use its `enum class` from the
contract that owns the domain: `UserRole`
(`packages/contracts/auth/src/auth/user-role.hxx`), `EventSeverity`,
`CameraRecordMode` and `ZoneType`
(`packages/contracts/camera/src/camera/`), `ReminderDetailStatus`
(`packages/contracts/productivity/src/productivity/`) and `UserAction`
(`packages/contracts/sync/src/sync/user-action.hxx`).
Each header carries its own `<enum>ToString`/`<enum>FromString` pair, in
lowerCamelCase (`userRoleToString`, `zoneTypeFromString`), derived from the
enum's own name; use them at DB boundaries only.

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
<home>/repositories/{entity}/
  {entity}-query.hxx      — SQL strings (namespace) + param structs (global)
  {entity}-repository.hxx — class declaration (uses structs from query file)
  {entity}-repository.cc  — implementations (uses `using namespace *_query`)
```

- Home: `<home>` is `<owner>/src/shared/` when 2+ features of the same owner
  read the repository, and `<owner>/src/feature/<feature>/` when only one
  does — rule 23's 2+ rule. The shape inside is the same either way.
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

### 6. Responses and refusals

Every refusal is **thrown, never built**. The vocabulary lives in
`packages/lib/errors/src/errors/` (`ErrorCode`, `ErrorDefinition`, `ResponseException`),
and each boundary catalogs its own codes in one header: `auth-errors.hxx`,
`camera-errors.hxx`, `identity/identity-errors.hxx`, and so on.

```cpp
throw ResponseException(CameraErrors::CameraNotFound);          // catalog entry
throw ResponseException(503, TtsErrors::TtsNotLoaded);          // status override
throw ResponseException(CameraErrors::CameraUnreachable
                            .withMessage(result->error));        // device's own words
throw ResponseException(422, std::vector<ResponseError>{...});   // wire list
```

`ApiResponse` (`packages/lib/http/src/http/api-response.hxx`) is the only place an
envelope is built: `ok`, `created`, `noContent`, `validationError` and the three
`error` overloads. Handlers return `ApiResponse::ok(...)` and never set a status
code or a body themselves. The one advice, `ErrorHandler::handleException`
(`packages/lib/http/src/http/error-handler.hxx`), is registered once per service and
turns a thrown refusal into the envelope, so a handler needs no try/catch.

Validation errors use `ApiResponse::validationError(fieldErrors)` → 422.

Attribute keys are centralized constants:
```
AuthContext::kJwtKey    = "jwt_ctx"     (packages/contracts/auth/src/auth/request-context.hxx)
AuthContext::kDeviceKey = "device_ctx"
```

### 7. Role-based access

Roles are checked centrally via
`packages/lib/auth/src/auth/role-access.hxx` (the `kTableAccess` map:
role → table → `RolePermission`; the enum itself is declared in
`packages/contracts/sync/src/sync/role-permission.hxx`, beside the table it
names, because the tier-3 llm client spells it in tool descriptors).
**`RoleFilter` (HTTP) and the sync engine share that single source of truth**
— to change a permission, edit only that file. Never imperative if/else.

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
<owner>/src/feature/{feature}/dtos/
  {action}-dto.hxx     — struct + fromJson() / form_multipart() factory
  {action}-dto.cc       — implementation + validation DSL
```

Naming: `LoginDto`, `RefreshTokenDto`, `CreateCustomerDto`

**Response DTOs** — header + cc file, with `toJson()`:
```
<owner>/src/feature/{feature}/dtos/
  response-{action}-dto.hxx
  response-{action}-dto.cc
```

Naming: `ResponseLoginDto`, `ResponseRefreshTokenDto`

**WS/sync DTOs** (header-only, e.g. `synchronized-dto.hxx`): parsed with
`static {T} fromJson(const Json::Value&)`; declared beside the sync engine
that reads them (`packages/sync/src/feature/socket/sync/dtos/` today; that
package becomes `services/sync` in Phase 3a).

### 11. Validation DSL

All DTO validation uses the macro DSL in `packages/lib/validation/src/validation/`:

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
The global `ErrorHandler::handleException()` catches it and returns a 422 JSON response.
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
    const auto& dev = req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);
    const auto result = co_await service_.login(body, dev.deviceHash, dev.userAgent);
    if (!result) throw ResponseException(IdentityErrors::FaceNotRecognized);
    co_return ApiResponse::ok(result->toJson());
}
```

Never: `if (!json)`, `if (!attrs->find(AuthContext::kJwtKey))`, manual field extraction, try/catch.

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
`packages/lib/runtime/src/runtime/thread-budget.hxx`:
`computeThreads()`, `batchThreads()`, `heavyThreads()`, `lightThreads()`,
`inferenceSlots()`. This keeps the same build fast on 2-core laptops and
64-core servers. Example: LLM uses `lightThreads()` for token decode and
`batchThreads()` for prompt prefill.

### 13c. Coroutines for heavy AI calls

Every AI service exposes a sync method (`chat`, `describe`, `transcribe`,
`synthesize`) AND a coroutine variant (`chatAsync`, `describeAsync`,
`transcribeAsync`, `synthesizeAsync`) that wraps the sync one in
`BlockingTask` (see `packages/lib/runtime/src/runtime/blocking-task.hxx`,
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
  the gate result lives in `docs/history/project-log.md`. The memory redesign's
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
  Drogon's first connection — see docs/history/project-log.md ordering note). FTS5 (bm25,
  unicode61, trigram) is enabled via the conan option
  `sqlite3/*:enable_fts5=True` (Drogon rebuilt once).
- **MemoryService** (`packages/memory/src/shared/services/memory/`, a package compiled into its host argus-llm) — long-term memory over a
  SQLite semantic graph (NOT the legacy `memory_l1`): `memory_entity`/alias
  (person frames), `memory_fact` (upsert closes the previous open fact via
  `supersedes`), `memory_edge`, `memory_episode` (compaction/system-event
  summaries, now recallable), `memory_procedure`. Capture: deterministic
  `RuleParser`+`PhraseCatalog` (vocabulary is static per-language constants in
  `packages/lib/phrase/src/phrase/details/`, never DB tables) → `TieredExtractor` (lexicon tier
  inline, NuExtract tier off-turn).
  Recall (`GraphRecall`): entity-anchored (recursive CTE) → FTS5 → vec0
  semantic tier with store-size-aware margin gates → episode tier; anaphora
  via `WorkingMemory.activeEntities`; block injected at the TAIL of the user
  turn. Background worker: embed (chunks+synonyms+vector dedup with
  priority-merge), compaction with the MAIN LlmService (re-enqueues when
  busy), L3 profile (deterministic persona/instruction selection + optional
  off-turn LLM polish). Facade `MemoryService`; DB via `SqliteGraph`/`VecDb`
  (mutex-serialized, prepared statements are RAII `SqliteStmt`). Embeddings:
  `multilingual-e5-small` int8 ONNX, loaded lazily. Full details in docs/history/project-log.md.
- **fastText as a submodule** (`third_party/fastText`, `1142dc4`) — inference
  only, built as a static lib by `packages/intent`. It backs the fast
  tier of the intent router: rules (`argus::lib::phrase`) decide explicit triggers,
  fastText classifies the rest into six classes (`memory_save`,
  `memory_recall`, `reminder_set`, `memory_forget`, `camera`, `none`), and the
  LLM's own tool calling keeps every turn the router is not confident about.
  The classifier picks the tool; the model only writes its arguments and the
  prose. Operating point 0.90 / 0.10, precision-gated — a gated false `none`
  costs one LLM round trip, a false tool call writes a fact nobody stated.
  The model is a published artifact carried in-repo
  (`models/intent/intent.bin`, 12.6 MB) with a configure-time SHA256 pin in
  `packages/intent/models/`; training lives OUTSIDE this repo, in the
  sibling `intent-training/` project, and only the artifact, its card and the
  frozen eval fixtures cross over. **Degradation is a contract**: no model on
  disk, or a sub-threshold score, and the router abstains so tool calling runs
  byte for byte as it did before. The labelled tool-calling evaluation set
  stays at `services/llm/tests/fixtures/tools/`.
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
  the per-installation 0600 per-project `config.toml` files from each
  `config.toml.example`, and then run
  `services/gateway/build/dev/argus-gateway`. Never commit, print, log
  or send instance secrets to the frontend.
- Production-style deployment is container-only: every microservice owns a
  `Dockerfile` and `argus-deploy/docker-compose.yml` builds and runs one
  container per service. Packages are reusable libraries compiled into the
  service images — no package has an image of its own. There is no separate
  local compose stack.
- `scripts/provision-host.sh` prepares a deployment host (Docker + Compose,
  PKI, per-service deploy configs with unique shared secrets) and writes the
  gitignored `argus-deploy/.env`. State (`ARGUS_DATA_DIR`, certs, models,
  go2rtc) lives on the host and is bind-mounted, so updating is a rebuild plus
  `docker compose up -d`; never `down -v`.
- Object storage is the RustFS service in `argus-deploy/docker-compose.yml`
  (`rustfs` + one-shot `rustfs-init`), consumed through `S3StorageService`
  (`storage.mode = "s3"` plus the `[storage.s3]` keys in each service's
  config). `provision-host.sh` generates the 0600 root/RPC/application
  credentials under `${ARGUS_DATA_DIR}/rustfs/secrets`, creates the private
  bucket and writes the bucket-scoped application pair into the
  gateway/camera/guard configs. Objects live in
  `${ARGUS_DATA_DIR}/rustfs/objects`; the gateway (host networking) uses
  `http://127.0.0.1:9000` and the internal services `http://rustfs:9000`.
  Use the service, never direct ad-hoc HTTP from feature code.
- Development uses a fresh schema when the developer explicitly resets the
  local DB. Do not silently delete, migrate or recreate a user's database as a
  side effect of a feature; ask/require an explicit development reset.

### 18. WebSocket sync engine

- WS route: `/sync` with **`DeviceFilter` + `JwtFilter`** so device binding is
  enforced on every authenticated transport. Messages are `{type, payload}` and responses use
  `SocketEmitDto` `{operation, option(TableName), info}`.
  Errors: `{type:"<type>_error", status, error}`.
- Operations (`packages/contracts/sync/src/sync/sync-operation.hxx`): `InitialInfo=0`,
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
- `SyncAuditService` (`packages/audit/src/shared/services/sync-audit/`) is the feature-level
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

Rules 16 and 19 are measured, not reviewed: `.clang-tidy` declares the check
set, and a full `./scripts/build-all.sh dev` runs `scripts/check-tidy.sh` over
every first-party translation unit, comparing the per-check counts with
`scripts/lib/tidy-baseline.txt`. The gate fails when a count rises or when the
scan sees fewer translation units than the baseline records — a check that
cannot be run is not a check that passed. Bring a baseline count down in the
same change that fixes what stands behind it.

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

Every service follows the same layout inside its own folder:

```
<service>/
├── src/
│   ├── app/                    composition only: main.cc + rpc/
│   ├── config/                 this service's typed config (D20)
│   ├── feature/<feature>/      a vertical slice, never a layer
│   │   ├── controllers/   dtos/         repositories/
│   │   ├── schemas/       services/     infra/
│   └── shared/                 ONLY what 2+ features of THIS service use
├── database/schema.sql
├── scripts/                    migrations and provisioning
├── tools/                      dev utilities
└── tests/{unit,e2e}
```

- **`feature/<feature>/` owns everything the capability needs** — controllers,
  DTOs, repositories, schemas, services and its own `infra/` adapters. That
  folder is the home: there is no `feature/api/<resource>/` level (the `api/`
  segment is the pre-migration spelling) and no `feature/rpc/` — gRPC belongs
  to `app/rpc/`, infrastructure rather than a capability.
- **`shared/` is earned, not default — the 2+ rule.** A repository, schema or
  service moves to `shared/` only when a second feature of the same service
  reads it; until then it lives in its feature. `shared/repositories/`
  therefore holds exactly the repositories 2+ features read, which is what
  makes rule 24's "no directory without a consumer" checkable and sharpens
  rule 3.
- **`app/` is process composition only** — `main.cc` and `rpc/`. No domain
  logic, no controller, no repository, no config resolution: that is
  `src/config/`, a sibling of `feature/` (D20). `src/server/` and a `main.cc`
  at the service root are the pre-migration spellings.
- Cross-service calls go through the SHARED client/SDK layer, never through
  per-service hand-rolled clients. Wire handling (URL/connect/envelope parse/
  retry/auth token pass-through) lives in one place.
- Each service owns its subroute (`/camera`, `/reminder`, ...) and its own
  auth handling via the shared filter/auth SDK — no service reaches into
  another service's database for auth.
- Dead code, unused folders and duplicated copies are removed in the same
  change that introduces their replacement.

**Today, against that target** (Phase 4 of
`docs/history/plans/architecture-plan.md` lands it): `services/tts` is the
only service with `src/app/`; `notification`, `productivity` and `guard`
already have the `{controllers,services,dtos}` interior, one level deeper
under `feature/api/<resource>/` (`notification` also carries `feature/rpc/`,
which Phase 4 step 2 moves to `app/rpc/`). The other nine services keep a
single `main.cc` at their `src/` root; `tunnel`, exempt by design (D19),
instead has two entry points there, `main-client.cc` and `main-relay.cc`.

Five services have no `feature/` at all today — `gateway`, `llm`, `stt`,
`tunnel` and `vlm` — and keep their code at `src/` level instead:
`src/controllers/` in `llm`, `stt` and `vlm`, a `src/llm/` and a `src/vlm/`
beside it, and `gateway`'s four domain folders. Of the six services that do
have a `feature/`, four still keep code beside it: `camera`
(`src/controllers/` and `src/camera/`, `monitor/`, `objects/`, `operator/`),
`notification` (`src/notification/`), `productivity`
(`src/productivity/`) and `voice` (`src/test-support/`). `guard` and `tts`
keep everything inside `feature/` (plus `app/` in `tts`).

No service has `src/config/` yet: the per-service typed config that step 9
moves there lives today under that same domain folder
(`camera-config.{hxx,cc}`, `notification-config.{hxx,cc}`,
`productivity-config.{hxx,cc}`, `operator-config.{hxx,cc}`). No service has
`tests/e2e/` yet — the only first-party one in the tree is
`packages/sync/tests/e2e/`, the frozen-frame suite that moves with the service
in Phase 3a. `gateway` is
deleted in Phase 3d.

### 24. One shape for every unit, never speculative structure

The folder architecture, naming and ordering are settled and shared: a service
is `src/{app,config,feature,shared}` + `database/` + `tests/` + `scripts/` +
`tools/` (rule 23), a package is `src/<name>/`, its own name as the include
root (rule 25). They
repeat the shape the legacy monolith established (`src/feature`, `src/shared`,
`src/server`, `database/`, `tools/`) once per unit rather than once at the
repo root, which holds no source tree of its own since f7-8. The one rename
inside that shape is `src/server/` → `src/app/` (rule 23); do not invent a
layout per service, and do not carry a spelling forward because a file
happened to have it.

Do not implement structure for its own sake: no abstraction, layer or
directory that has no current consumer. A `shared/` folder with one reader, a
module declared but linked by nothing, a `tools/` directory with no tool —
each is structure ahead of its consumer, and rule 23 says where the code goes
instead.

### 25. Build ergonomics: the folder IS the module

Build import is by module name, never by listing files in consumers.
Every shared feature/repo/SDK piece lives in its OWN folder and is declared
ONCE, in its home, through the project helpers:

```cmake
# services/<name>/src/shared/services/<module>/CMakeLists.txt — a service-local
# module declares its own sources + deps and names no group
argus_module(NAME <module>
  SOURCES <module>-service.cc
  DEPENDS onnxruntime)

# A package declares itself through its group's helper, which owns the target
# name — packages/clients/identity/CMakeLists.txt
argus_clients(NAME identity
  PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/proto
  PROTO argus/identity/v1/identity.proto
  SOURCES src/identity/identity-client.cc
  INCLUDES src)
```

- Parents auto-discover modules (a loop over subdirectories) — adding a
  module = creating a folder; adding a file = dropping it in its module.
  NO ONE edits another module's CMakeLists and NO consumer lists `.cc`
  files.
- Consumers link by name: `target_link_libraries(argus-voice PRIVATE
  argus::<module> argus::clients::identity)`. Include paths travel with the
  target.
- Services bootstrap through a `argus_service()` helper (the executable, its
  module links, the `-Wall -Wextra` gate, the `$ORIGIN` rpath and the
  `ARGUS_PORTS` property) instead of copy-pasted CMake blocks.
- Explicit source lists stay ONLY inside the module's own CMakeLists.
  `file(GLOB)` for sources is forbidden (fragile); auto-discovery of
  module folders (GLOB over `*/CMakeLists.txt`) is the only allowed glob.

#### The three package groups, and the name a group gives a target

A package is one of three things, and its group is the first segment of its
path — a lib, a contract or a client. The group is part of the target name, so
a link line says what a package is and where it lives without consulting the
tree:

| Group | Holds | Target | Alias | Consumed as |
|---|---|---|---|---|
| `packages/lib/<name>/` | reusable infrastructure: no domain data, no wire | `argus_lib_<name>` | `argus::lib::<name>` | `argus::lib::validation` |
| `packages/contracts/<domain>/` | one domain's `.proto` + the C++ types that cross the wire | `argus_contracts_<domain>` | `argus::contracts::<domain>` | `argus::contracts::camera` |
| `packages/clients/<domain>/` | the SDK for one service: the only place a stub, a URL or a retry policy exists | `argus_clients_<domain>` | `argus::clients::<domain>` | `argus::clients::camera` |

The folder never repeats the group (`packages/clients/llm`, not the flat tree's
`packages/clients/llm-client`), and a `DEPENDS`/link line never spells a target
name by hand — the group helper owns the spelling, so a misplaced package fails
at configure time instead of linking the wrong thing. A service is not a
package: it is the executable `argus-<name>`.

The target-name half of that rule is structural: `argus_lib`,
`argus_contracts` and `argus_clients` build the group into the name, so a
package cannot declare itself into the wrong tier (rule 25's own names are in
`cmake/argus-module.cmake`). The dependency half is still prose — 20 grouped
packages' helper calls name a dependency's `argus::` alias by hand in their own
`DEPENDS` (the seven contracts that consume `lib/errors`, the `response` and
`tts` wire modules, five clients, six libs), and the four ungrouped packages
that do the same add four more (`audit`, `identity`, `intent`, `room`).
Nothing checks those spellings yet; the edge checker of Phase 2 step 5 is where
they become checked edges.

The three groups sit where they belong — `packages/lib/<name>`,
`packages/contracts/<domain>`, `packages/clients/<domain>`, each declared by
its group's helper: `argus_lib_<name>` / `argus::lib::<name>`,
`argus_contracts_<domain>` / `argus::contracts::<domain>`,
`argus_clients_<domain>` / `argus::clients::<domain>`. Seven of the ten clients
wrap a generated gRPC stub — six of them pass `PROTO` to `argus_clients`, and
`tts` reaches the same stub through `argus::contracts::tts` instead; the three
wire clients (`llm`, `stt`, `vlm`) speak HTTP and take the helper's
plain-module branch, which `tts` also takes because it carries an HTTP
transport beside the stub. Three wire modules are not a domain SDK and call
`argus_client_module` with the group they live in: `lib/grpc`'s health stubs
(`GROUP lib`) and the `response` and `tts` wire contracts, which live in
`packages/contracts/` and are aliased `argus::contracts::…`.

#### The dependency tiers

Dependencies run in tiers, and the permitted edges are explicit. A package may
consume contracts and clients; what it may NEVER do is query another owner's
data (D18):

| Tier | Packages | May depend on | May never depend on |
|---|---|---|---|
| 1 · foundation | `lib/`: audio, cert, config, errors, grpc, mdns, nats, phrase, runtime, sqlite, storage, text, validation | third-party, other tier-1 `lib` packages | contracts, clients, services |
| 2 · wire | `contracts/*`, `lib/http` | tier 1 (`lib/errors`, `lib/grpc`), third-party (Drogon), generated protobuf | clients, services |
| 3 · transport | `clients/*` | tier 1 + tier 2 | other clients, services |
| 4 · service-aware lib | `lib/auth` | tiers 1–3 | services |
| 5 · services | `services/*` | everything above | another service's `src/` |

- A tier never points back up and the graph has no cycles. Two units that need
  each other are one unit.
- A contract cannot call a service: if a domain's data is needed, it is needed
  through a client, and the client is the only place a stub, a URL or a retry
  policy exists.
- A service depends on packages and clients, never on another service's
  source.
- Interface dependencies (types in public headers) are `PUBLIC`;
  implementation-only dependencies are `PRIVATE`.
- Header-only where nothing is compiled: nine of the ten contracts go through
  `argus_contracts`, which is `INTERFACE` by construction (`auth`, `camera`,
  `gateway`, `identity`, `notification`, `productivity`, `sync`, `tts`,
  `voice`), and `validation` declares `HEADER_ONLY` to `argus_lib` for the same
  result. `response` is the exception under `packages/contracts/`: it carries
  no vocabulary but the response wire, declared `argus_client_module(NAME
  response-wire GROUP contracts …)` and compiling `response-rpc.cc`. `cert` is
  a `STATIC` `argus_lib` over one `.cc`; `text` and `phrase` compile real
  sources and are not candidates for it.
- **Enums live where they are used.** A service declares its domain enums
  inside the feature that uses them; the vocabulary that crosses the wire
  (`UserRole`, `SyncOperation`, `TableName`, priorities) is declared once in
  the contract that owns it. There is no shared enum package, and no service
  keeps a copy of a wire enum — a copy drifts and breaks the frozen wire.
- `lib/http` is tier 2, not tier 4: no tier-1 package may reach it, which is
  why the health controller and the listener config are not in `lib/config`.
- The tiers are checked mechanically, not by review: `scripts/check-deps.sh`
  reads every `CMakeLists.txt` under `packages/` and `services/` and fails on a
  forbidden edge. It reads the edges from **both** places one can be written —
  the `DEPENDS`, `SYSTEM_DEPENDS` and `MODULES` lists of the `argus_*` helpers,
  where a first-party dependency actually lives, and literal
  `target_link_libraries` calls — because a scan of `target_link_libraries`
  alone sees almost none of the graph. `./scripts/build-all.sh` runs it before
  it builds anything.

### 26. One schema per microservice: `database/schema.sql`

Every owner keeps exactly one schema file named `database/schema.sql` inside
its own project — `services/<name>/database/schema.sql`, or
`packages/<owner>/database/schema.sql` while the owner is still a package
(`identity` and `memory` are the two left, and they move in Phase 3c and
Phase 4 step 7). Never introduce `<domain>-schema.sql` aliases. The deploy
stack bind-mounts each owner's file at `database/schema.sql` in its container
and every config points at `database/schema.sql`; a unit applies only its own
schema, never the schema of another. The gateway is the one owner that is not
at that path: it mounts its own file at `/opt/argus/gateway/schema.sql` and
its config says `gateway/schema.sql`, which is also the only place a second
schema appears — the identity one it hosts, at `database/schema.sql`. Seven
units carry one today:
`camera`, `gateway`, `guard`, `notification` and `productivity` (the gateway's
own 32-line file is its `gateway.db` degraded-fallback record — it holds no
table of another domain and says so — and it additionally applies the identity
schema it hosts; both go with the gateway in Phase 3d), plus
`packages/identity` and `packages/memory`.

### 27. Database isolation between microservices

A service may only open its own database. Accessing another domain's data is
forbidden at the file/SQL level, even read-only. Cross-domain reads travel
ONLY through the typed gRPC contracts and their SDK clients
(`packages/clients/<domain>/src`, linked as `argus::clients::<domain>`, rule
25) and change feeds travel through NATS events. No compose mount may expose
one unit's database to another. If a domain needs data it does not own, add an
SDK method on the owner and call it through the client; never reach into its
DB file. The SDK/client is the whole point of the boundary.

## Build Commands

```bash
# All standalone projects (Debug + tests)
./scripts/build-all.sh dev

# All standalone projects (Release + tests)
./scripts/build-all.sh prod

# One project
./scripts/build-all.sh dev --only camera
```

Before any commit, verify the affected standalone project with
`./scripts/build-all.sh dev --only <project>` and **0 errors, 0 warnings**.
Run the full orchestrator when changing shared build infrastructure.

The orchestrator runs two gates of its own, beyond the eighteen projects:
`scripts/check-deps.sh` before anything is built (§2.4's tiers), and, at the
end of a full run only, `scripts/check-tidy.sh` (rules 16 and 19). `--only`,
`--no-tests` and `--install-only` skip the clang-tidy scan deliberately: it
needs every project's compile database, and a per-project run has to stay
quick.

## File Naming

- Headers: `.hxx`
- Sources: `.cc`
- Tests: `*-test.cc` (e.g. `user-service-test.cc`)
- No `.h` or `.cpp` extensions.

## Key Files Reference

The group prefixes (`lib/`, `contracts/`, `clients/`) and the dropped
`-contract`/`-client` suffixes have landed (rule 25), and the `lib/` rows below
already carry §2.3's interior (`src/<name>/`) — Phase 2 step 2 applied it to
every lib. The contract, client and service rows still show the spellings of
the pre-layout tree, which steps 2b-2d replace. A path below can therefore move
for two different reasons, and says which when it does.

**Any owner**

| File | Purpose |
|------|---------|
| `<owner>/src/shared/schemas/*/` | DB row → C++ struct mapping |
| `<owner>/src/shared/repositories/*/` | Data access layer — `shared/` when 2+ features of the owner read it, the feature's own `repositories/` otherwise (rule 23) |

**Tier 1 — `lib/`**

| File | Purpose |
|------|---------|
| `packages/lib/validation/src/validation/` | Validation DSL (rules, macros, validator) |
| `packages/lib/errors/src/errors/` | `ErrorCode`, `ErrorDefinition`, `ResponseException` — the refusals every boundary throws |
| `packages/lib/http/src/http/` | The `{status, info, errors}` envelope (`ApiResponse`), the one advice (`ErrorHandler`), CORS, health, listener |
| `packages/lib/audio/src/audio/` | `AudioResampler` (stateful sinc) + `EndpointDetector` — every block-processed audio path MUST use these, never a custom conversion |
| `packages/lib/phrase/src/phrase/details/` | Static per-language memory vocabulary (es/en): `PhraseSeed`/`LexiconSeed` constants — no DB tables |
| `packages/lib/phrase/src/phrase/` | `RuleParser` + `PhraseCatalog` — the vocabulary's parser and catalog, read by `packages/memory`, `packages/intent`, `services/llm` and `services/voice` |
| `packages/lib/sqlite/src/sqlite/` | DB client access (`DbService::client()`, extensions) + `VecDb` (vec0 connection) |
| `packages/lib/storage/src/storage/` | `S3StorageService` (RustFS S3, SigV4 in `details/s3-signing.hxx`) — private objects, read back through a one-use capability |
| `packages/lib/config/src/config/` | `ConfigService` read + runtime writes (`setBool/...` persist to `config.toml`, comments preserved) |
| `packages/lib/runtime/src/runtime/cancellation-token.hxx` | `CancellationToken` shared across streaming AI/audio paths |
| `packages/lib/runtime/src/runtime/blocking-task.hxx` | Coroutine awaiter for off-loop heavy work |
| `packages/lib/runtime/src/runtime/thread-budget.{cc,hxx}` | Adaptive thread sizing for AI services |
| `packages/lib/runtime/src/runtime/hardware-profile.{cc,hxx}` | CPU/RAM/ISA and video-accel probe (`HardwareProfile`), ncnn-free and ncnn variants |
| `packages/lib/text/src/text/json-diff.{cc,hxx}` | Diff JSON + snapshot (`JsonDiff`) |
| `packages/lib/text/src/text/json-util.hxx` | `json_util::toString` (compact, `{}` for null), `isValid` (empty is not valid) and `fromString` |

**Tier 2 — `contracts/`**

| File | Purpose |
|------|---------|
| `packages/contracts/{auth,camera,productivity,sync}/src/<domain>/` | Each domain's wire enums, each with its own lowerCamelCase `<enum>ToString`/`<enum>FromString` pair (`packages/contracts/camera/src/camera/zone-type.hxx`). The enums that mirror a `CHECK` constraint are not all here — `notification` and `packages/identity/src/shared/vocabulary/` each carry their own |
| `packages/contracts/sync/src/sync/` | `Syncable`, `SyncFilter` base classes + `sync-operation.hxx` |

**Tier 3 — `clients/`**

| File | Purpose |
|------|---------|
| `packages/clients/<domain>/src/<domain>/` (all ten now hold a single `src/<domain>/`; `camera-actions` shares `camera`'s domain folder) | The SDK for one service: the only place its stub, URL, envelope parse, retry and auth pass-through exist (`camera`, `identity`, `notification`, `productivity`, `voice`, `camera-actions` + the `llm`/`stt`/`tts`/`vlm` wire clients). Callers link `argus::clients::<domain>` (rule 25, rule 27) |

**Tier 4 — `lib/auth`**

| File | Purpose |
|------|---------|
| `packages/lib/auth/src/auth/role-access.hxx` | Centralized role → table → permission table (`role_access`), used by `RoleFilter` and sync |
| `packages/lib/auth/src/auth/device-filter.{cc,hxx}` | Device fingerprint extraction |
| `packages/lib/auth/src/auth/jwt-filter.{cc,hxx}` | JWT verification + refresh token validation |
| `packages/lib/auth/src/auth/role-filter.{cc,hxx}` | Role-based access control |
| `packages/lib/auth/src/auth/valid-json-filter.{cc,hxx}` | JSON body validation for POST/PATCH |
| `packages/lib/auth/src/auth/jwt-service.{cc,hxx}` | JWT sign/verify (HS256, instance class) |

**Tier 5 — services, and the packages that are on their way to one**

| File | Purpose |
|------|---------|
| `services/llm/src/shared/services/llm/` | LLM inference (llama.cpp) |
| `packages/memory/src/shared/services/embedding/` | `EmbeddingService` (multilingual-e5-small int8 ONNX) + `UnigramTokenizer`; becomes a feature of `services/llm` (Phase 4 step 7) |
| `packages/memory/src/shared/services/memory/` | `MemoryService`/`SemanticGraph`(`SqliteGraph`)/`GraphRecall`/`MemoryFormation`/`EntityResolver`/`ToolParser`/`MemoryChat` — semantic-graph long-term memory (async worker, episode recall, L3 profile); a package hosted by argus-llm, and a feature of it after Phase 4 step 7 |
| `packages/identity/src/shared/services/face/` | Face detection + recognition (ncnn) — FaceDB = vec0 index (sqlite-vec); becomes `services/identity` (Phase 3c) |
| `packages/identity/src/shared/services/storage/` | `PrivatePortraitService` — a user's private portrait bytes (`store`/`has`/`read` by `userId`), served onward by the `user` feature's portrait-preview capability; same move |
| `packages/identity/src/shared/repositories/{user-invitation,portrait-*,device-login-challenge}/` | People domain: invitations (hash-only), portrait capabilities, cross-device login challenges; same move |
| `services/vlm/src/shared/services/vision/` | VLM inference: LFM2.5-VL-450M via llama.cpp + libmtmd (arbitrary prompts, caption cache) |
| `services/stt/src/shared/services/stt/` | Speech-to-text via sherpa-onnx (default `nemo_transducer` FastConformer RNN-T, es/en; whisper/canary/nemo_ctc/omnilingual selectable) |
| `services/tts/src/feature/synthesis/` | `TtsService` (`domain/` → `services/` in Phase 4 step 1) + the Supertonic engine set (`infra/supertonic/`: `TtsEngine`, `Style`, `UnicodeProcessor`, onnx loading) — Supertonic 3 text-to-speech |
| `services/voice/src/shared/services/vad/` | `VadService` — Silero VAD v5 as an **instance** class (per-stream LSTM, shared ONNX session), with the turn-quality gate |
| `services/voice/src/shared/wrapper/audio/` | `SampleRing` — the fixed-capacity float ring the voice paths carry samples in across calls |
| `services/voice/src/shared/services/reaction/` | `ReactionEngine` — per-turn reactions by signal priority → `voice:event` (meaning, never expression names) |
| `services/camera/src/shared/services/stream/` | go2rtc manager, `StreamHub` (fMP4 over `/sync`, per-connection credit window, lock order `hubMutex_ → Upstream::mtx`), `Fmp4Reader` (encoding from headers, whole fragments) |
| `services/camera/src/shared/services/tapo/` | Tapo camera local protocols: control (`stok` + `securePassthrough`, legacy fallback) and the 8800 talk channel (Digest + MPEG-TS PCMA) |
| `services/notification/src/shared/services/notification-token/` | Push tokens per session |
| `packages/sync/src/feature/socket/sync/` | `SyncSocket` + `SyncService` + `SynchronizedService` + DTOs; becomes `services/sync` (Phase 3a) |
| `packages/sync/src/shared/services/notification/` | Per-user notifications: `Add` on create and granular user-audit on mark-as-read; same move |
| `packages/socket/src/shared/services/socket/` | `SocketService` (emitModule/emitUser) + `SocketEmitDto`; dies in Phase 3a — its vocabulary goes to `contracts/sync` and its transport to `services/sync` |
| `packages/room/src/shared/services/room/` | local `RoomManager` (rooms per module/user, `thread_local`); dies with `socket` in Phase 3a |
| `packages/audit/src/shared/services/audit-log/` | Global audit: per-field diffs, daily compaction and monotonic id for sync |
| `packages/audit/src/shared/services/user-audit-log/` | Per-recipient audit: per-field diffs, daily compaction and monotonic id for sync |
| `packages/audit/src/shared/services/sync-audit/` | Central facade to publish module/user diffs after feature mutations. All three become `services/sync`, which owns the three tables and is the only writer (Phase 3a) |

**Docs and templates**

| File | Purpose |
|------|---------|
| `docs/README.md` | Documentation index and reading order |
| `docs/history/project-log.md` | Full project history and decisions |
| `<project>/config.toml.example` | Per-project template; `setup.sh` generates the gitignored `config.toml` |
