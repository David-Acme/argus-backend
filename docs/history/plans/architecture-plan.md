# Argus Backend — Architecture Plan v4

> Date: 2026-09-19 · Status: DECIDED (design); migration under way — Phase 1 steps 0–4 done
>
> Supersedes `architecture-plan.md` v3 (2026-09-18). v3 kept the gateway, proposed
> `sync-tablets`, `packages/contract` (singular) and fused `guard` into `camera` and
> `voice` into `sync`; none of that is adopted here — the rationale survives in
> Appendix A. Migration **has started**: Phase 1 steps 0 and 1 landed in
> `build: land the package and service rename wave`, followed by steps 2–4 in
> `build: delete the packages/common transition shim`, `build: merge the json and
> hash packages into text` and `build: distribute the access vocabulary and delete
> the package`; §5 carries the order for the rest.
>
> **How to read it:** §0–§3 are the architecture, §4 is the code rules every change is written
> against, §5 is the migration order, §9 is the measured baseline the migration is checked
> against. The frozen wire is *defined* by `docs/architecture/wire-*.md` and
> `contracts-overview.md`; this plan fixes its shape and the packages that carry it, not the
> payloads themselves.
>
> The document is in English per the repository documentation rule; the implementer
> prompts derived from it are English as well.

---

## 0. Decisions taken (2026-09-19)

| # | Decision |
|---|---|
| D1 | Packages are reusable infrastructure only. A package that accesses data is **not** a package: all business logic and data access lives in microservices. |
| D2 | `packages/` is organised in three groups — `lib/`, `contracts/`, `clients/` — with one folder per unit and no name repeating its parent (`contracts/camera`, not `contracts/camera-contract`). |
| D3 | gRPC contracts live in `packages/contracts/<domain>/`; the easy, processed way for one microservice to talk to another is `packages/clients/<domain>/`. |
| D4 | A microservice's internal layout is the plain feature slice: `feature/<feature>/` with `controllers/`, `services/` and `dtos/`. gRPC is infrastructure (`app/rpc/`), **never** a feature. `src/shared/` holds only code consumed by 2+ features of the same service, and the service's own typed config lives in `src/config/` (D20). |
| D5 | The **gateway and the reverse proxy are eliminated**. There is a microservice for `auth` and a microservice for `identity`. Each microservice is autonomous: it serves its own route, announces itself over mDNS, terminates TLS with the same instance certificate (`argus.local`), and runs its own filter chain. |
| D6 | `user` is owned by `identity` (with `person`, `face_embedding`, invitations and portraits) so enrolment stays one atomic transaction. `auth` owns sessions and credentials and resolves the user context through `clients/identity`, with a short-TTL cache invalidated by the identity change event. |
| D7 | `audit_log` / `user_audit_log` are owned by `sync` (they exist only for synchronisation). Producers never write them: they publish field diffs durably. |
| D8 | Change events keep flowing over NATS; imperative realtime operations (`AuthContextChanged`, disconnect, role-room replacement, directed emit) go through `clients/sync` over gRPC. |
| D9 | `audio` stays a **package**: it is stateless-per-stream DSP (resampler, endpointer) called per audio block in two services. Voice selection is a `tts` feature; concurrent sessions are `voice`'s job. |
| D10 | Packages carry no `conanfile.txt`. One general `conanfile.txt` at the repository root resolves the dependency graph; each project imports only what it uses. |
| D11 | **One domain, one global certificate, routes as the addressing unit.** Every service answers as `https://argus.local/<route>` under a single instance certificate. No per-service certificates, no subdomains. The app is one application that talks to more than one process. |
| D12 | **No `packages/enums`.** Each service declares the enums it uses inside the feature that uses them. The vocabulary that crosses the wire (`UserRole`, `SyncOperation`, `TableName`, priorities) is declared once, in the contract that owns it, and is never copied per service. |
| D13 | **Full clean restructure.** Internal APIs, layouts, package boundaries and dead artifacts are rewritten freely — nothing internal is preserved for compatibility. The wire is the one exception (§1.8): it changes only when declared in the contract and shipped together with the single frontend. |
| D14 | `contracts/gateway` dies with the edge. Its error vocabulary is reassigned to whoever can still produce each error (`CAMERA_UNREACHABLE` → `contracts/camera`, session/credential errors → `contracts/auth`). |
| D15 | **Audit retention is a 90-day TTL.** `sync` prunes and compacts audit rows older than 90 days. A client offline longer than the window cannot converge by replay: it re-bootstraps with a full `Synchronize`, and the app must handle "replica too old". The semantics are declared in `contracts/sync`. |
| D16 | **A client keeps the transport its service exposes.** gRPC where a proto exists (camera, camera-actions, identity, notification, productivity, voice, tts); HTTP where the route is app-facing and frozen (llm, stt, vlm). The internal AI legs finish their gRPC wire following the `argus.tts.v1` precedent. A single-consumer framework shipped inside a client (the llm tool registry, executor and validator) is a feature of its service, not a client. |
| D17 | The error substrate is **not** HTTP-only (contracts, `storage`, and several services consume it): `packages/response` splits into `lib/errors` (the vocabulary and mechanism: `ErrorDefinition`, `ErrorCode`, typed exceptions) and `lib/http` (the HTTP substrate: envelope and its formatter, the shared advice, health controller, listener and route registration, where `packages/common` also lands). `http` is the counterpart of the existing `lib/grpc` — one name per transport, and neither suggests a frontend. |
| D18 | **The data rule, refined.** A package may not *query* or own data and may not reach into a service's database — but it **may consume other packages, contracts and clients**. So `lib/auth` validating a credential through `clients/auth` is legal and expected; what is illegal is SQL against another owner's domain, repositories of a domain, or a package that is a service in disguise. |
| D19 | **The tunnel is deferred.** `services/tunnel` stays exactly as it is — no layout work, no contract work, no phase touches it — until the remote-access work starts. Its own shape (`net`, `protocol`, `client`, `relay`) is accepted for now. |
| D20 | **Service config lives in `src/config/`; `app/` stays composition-only.** A service's typed configuration resolution (`<svc>-config.{hxx,cc}`: db path, schema path, ports, feature flags, read through `lib/config`) lives in `src/config/` as a sibling of `feature/` and `shared/`, so it is not buried inside the process entry point. `app/` holds `main.cc` and `rpc/` and nothing else, and every service registers the one shared exception advice with a single line. **There is no per-service `AppConfig` subclass** — verified: nobody subclasses `AppConfig` in the tree today. |

Each decision states the rule; the sections below are where it is implemented, so a decision is
never repeated in full twice. **Open decisions** are listed in §7 (none), and §9 is the verified
inventory the migration is measured against. If §4 (code rules) and `AGENTS.md` disagree during
the migration, §4 wins until Phase 1 step 12 rewrites `AGENTS.md`; after that `AGENTS.md` is the
binding file.

---

## 1. Design principles

1. **No super-packages.** There is no `common`, no shared drawer. Every package answers: *"would another service use this without changing it?"*
2. **A package is infrastructure.** No database of its own, no domain rules, no HTTP routes, no `main.cc`, no queries against another owner's data (D18). It may consume contracts and clients — a package that calls a service through its client is exactly how the boundary is respected.
3. **A microservice is business logic plus its data.** It owns its database, its domain, its feature slices and its HTTP/gRPC surface.
4. **Communication is contracts plus clients.** All service-to-service traffic goes through `packages/contracts/*` and `packages/clients/*`. No direct SQL into another service's database, no shared memory, no reaching into another service's `src/`.
5. **A feature is a vertical slice.** Controllers, DTOs, services and infrastructure of one capability live together, not in horizontal layers.
6. **RPC is infrastructure, not a feature.** gRPC servers live in `app/rpc/`.
7. **Two consumers or it is not shared.** Anything used by exactly one feature lives in that feature; anything used by 2+ features of the same service lives in that service's `shared/`; anything used by 2+ services is a candidate package and must additionally pass principle 2.
8. **Frozen wires stay frozen.** Public paths, the `{status, info, errors}` envelope, error codes, `SyncOperation` 0–7, `SYNC_LIMIT=200`, `TableName` values and the NATS subject contract do not change. New capabilities are additive; retired protobuf fields are `reserved`. Everything *behind* the wire is fair game: internal APIs, layouts and package boundaries are rewritten freely (D13). A wire change happens only when it is declared in the contract and the single frontend ships it in the same change.
9. **Nothing speculative.** No abstraction, layer or directory without a current consumer.
10. **One response, one formatter.** Every HTTP response — success, filter rejection and
    thrown exception — is built by the single envelope formatter in `lib/http`, from the error
    definitions declared once in `lib/errors`. No service assembles the envelope by hand and no
    service keeps its own copy of an error code.

---

## 2. Architecture A — Packages

### 2.1 The hard rule

A package exists only if **all** of these hold:

- [ ] Reusable: used by 2+ services without modification
- [ ] Non-business: infrastructure, not domain rules
- [ ] Data-free: it does not access data — no repositories, no schemas of a domain, no database of its own
- [ ] Surface-free: no HTTP controllers, no `main.cc`, no listeners
- [ ] Testable on its own

Infrastructure that *enables* storage or transport is data-free in this sense: `sqlite` opens the connection and `nats` carries messages — neither queries a domain. Code that owns domain rows, repositories or audit writes is never a package.

### 2.2 The tree

```
packages/
│
├── lib/                          reusable infrastructure (15)
│   ├── audio/                    AudioResampler (stateful sinc), EndpointDetector
│   ├── auth/                     DeviceFilter, JwtFilter, RoleFilter, ValidJsonFilter,
│   │                             JwtService, role gating
│   │                             (no DB: credential validation via clients/auth)
│   ├── cert/                     instance PKI: CA, issuance, rotation — one certificate
│   │                             for `argus.local`, presented by every service (D11)
│   ├── config/                   ConfigService + TOML reader
│   ├── errors/                   ErrorDefinition, ErrorCode, typed exceptions (D17)
│   ├── grpc/                     client-base, cq-bridge, server-identity
│   │                             + the standard grpc.health.v1 stubs
│   ├── mdns/                     service announcement and discovery
│   ├── nats/                     NatsBus, JetStream, subject constants
│   ├── phrase/                   es/en vocabulary, rule parser
│   ├── runtime/                  BlockingTask, CancellationToken, ai-init,
│   │                             ThreadBudget, HardwareProfile
│   ├── sqlite/                   DbService, VecDb, SqliteGraph, SqliteStmt
│   ├── storage/                  S3StorageService, PrivatePortraitService
│   ├── text/                     json-util, json-diff, sha256, base64, fnv, text-norm
│   ├── validation/               validation DSL (header-only)
│   └── http/                     api-response ({status, info, errors} formatter),
│                                 error-handler (the one advice and the framework
│                                 fallbacks), cors, health controller,
│                                 listener configuration, route registration
│
├── contracts/                    one folder per domain: its .proto + C++ contract types
│   ├── auth/                     roles, JWT claims, credential/context types (D12)
│   ├── camera/  identity/  notification/  productivity/  voice/
│   ├── sync/                     syncable, sync-filter, SyncOperation, TableName,
│   │                             priorities, errors, proto
│   ├── tts/  response/  health/  manifests/
│
└── clients/                      one folder per domain: the SDK for that service
    ├── auth/                     credential/context validation  (D6)
    ├── sync/                     realtime control: rooms, disconnect, resync  (D8)
    ├── camera/  camera-actions/  identity/  notification/  productivity/  voice/
    └── llm/  stt/  tts/  vlm/    (names unified; target prefix stays argus::client-)
```

### 2.3 Folder and file structure

Headers live next to their sources. Nothing is installed, nothing is published, so there is no
`include/` + `src/` split, no `install(EXPORT)`, no export headers and no PIMPL — those exist to
protect an ABI Argus does not ship. `details/` marks what is private by convention, and the
include root of a package is its `src/`, so consumers write `#include <errors/error-code.hxx>`.

Each contract keeps its own `proto/` root with the canonical protobuf path
(`argus/<domain>/v1/...`) and the helper collects one `-I` per contract, so imports resolve
without duplicating the tree.

Because the include root of a package is its `src/`, moving `packages/response` into `lib/http`
keeps today's include valid: `#include <http/api-response.hxx>` does not change in a single
controller. The health controller and the listener config **do** change theirs —
`<controllers/health-controller.hxx>` becomes `<http/health-controller.hxx>` in 18 files and
`<server/listener-config.hxx>` becomes `<http/listener-config.hxx>` in 14.

`lib/http` holds four files with four jobs, and none of them is configuration:

| File | Holds |
|---|---|
| `api-response.{hxx,cc}` | the formatter: `ok`, `created`, `noContent`, `error`, `validationError` — the only code that builds an `{status, info, errors}` |
| `error-handler.{hxx,cc}` | `handleException` (the one advice registered by each service) and the two callbacks Drogon cannot let throw: the unmatched-route 404 and 405 fallbacks |
| `cors.{hxx,cc}` | `applyCors` and the OPTIONS answer |
| `health-controller.{hxx,cc}` | the health route, registered per service |

All four live today inside `config/app-config.{hxx,cc}`. The `AppConfig` class and the
`getNNNResponse` family are leftovers from when that file was a config base: the
status-to-code pairing those builders hardcoded is what the error catalog now owns, so
the builders go with it (§4.7, Phase 1 step 10).

#### A lib package — `packages/lib/errors/`

```
packages/lib/errors/
├── AGENTS.md                          what this package is for, and why it exists
├── CMakeLists.txt                     argus_lib(NAME errors ...)
├── src/errors/                        the public surface: the include root
│   ├── error-code.hxx                 enum class + toString/fromString
│   ├── error-definition.hxx           code + status + message + details
│   ├── response-exception.hxx
│   ├── validation-exception.hxx
│   └── details/                       private by convention, never included outside
│       └── code-table.hxx
└── tests/unit/
    └── error-definition-test.cc
```

`packages/lib/sqlite/` is the same shape with real internals: `src/sqlite/{db-service,
sqlite-stmt, vec-db, sqlite-graph}.{hxx,cc}` plus `src/sqlite/details/pragma-apply.{hxx,cc}`.
A header-only package (`validation`, `text`) declares `HEADER_ONLY` and has no `.cc`.

#### A contract — `packages/contracts/camera/`

```
packages/contracts/camera/
├── AGENTS.md
├── CMakeLists.txt                     argus_contracts(NAME camera PROTO argus/camera/v1/...)
├── proto/argus/camera/v1/
│   ├── actions.proto                  the wire: messages + service
│   └── sync.proto
└── src/camera/
    ├── camera-errors.hxx              the typed errors this domain produces
    └── camera-types.hxx               C++ types callers use (no protobuf leaks out)
```

#### A client — `packages/clients/camera/`

```
packages/clients/camera/
├── AGENTS.md
├── CMakeLists.txt                     argus_clients(NAME camera PROTO_ROOT ... PROTO ...)
├── src/camera/
│   ├── camera-sync-client.hxx         the SDK method surface callers use
│   ├── camera-sync-client.cc
│   └── details/                       channel, credentials, retry, envelope parsing
└── tests/unit/
    └── camera-sync-client-test.cc
```

Consumers write `#include <camera/camera-sync-client.hxx>` and link `argus::clients::camera`.
No consumer ever sees a protobuf type, a stub, a URL or a retry policy.

#### A microservice — `services/camera/`

```
services/camera/
├── AGENTS.md
├── CONTEXT.md                         the "why" of design decisions (rule 20)
├── CMakeLists.txt                     argus_service(NAME camera PORT 7026 ...)
├── config.toml
├── Dockerfile
├── database/
│   └── schema.sql                     the only schema this service applies (rule 26)
├── scripts/                           migrations and provisioning
├── tools/                             dev utilities
├── src/
│   ├── app/                           composition only: no logic, no config resolution
│   │   ├── main.cc                    wiring, boot, listeners, one advice line
│   │   └── rpc/                       infrastructure, never a feature
│   │       ├── CMakeLists.txt
│   │       ├── camera-sync-rpc-server.{hxx,cc}      the surface other services call
│   │       └── camera-actions-rpc-server.{hxx,cc}
│   ├── config/                        this service's typed config, sibling of feature/ (D20)
│   │   ├── CMakeLists.txt
│   │   └── camera-config.{hxx,cc}      db path, schema path, ports, feature flags
│   ├── feature/
│   │   ├── camera-control/
│   │   │   ├── CMakeLists.txt
│   │   │   ├── controllers/camera-control-controller.{hxx,cc}
│   │   │   ├── dtos/create-camera-dto.{hxx,cc}  update-camera-dto.{hxx,cc}
│   │   │   │      response-camera-dto.{hxx,cc}
│   │   │   ├── repositories/camera-query.hxx  camera-repository.{hxx,cc}
│   │   │   ├── schemas/camera-schema.{hxx,cc}
│   │   │   └── services/camera-control-service.{hxx,cc}
│   │   ├── media/                     the same shape: controllers, dtos, services, infra
│   │   ├── monitor/                   detection loop with its own services and schemas
│   │   └── actions/                   gated camera actions (PTZ, talk, capture)
│   └── shared/                        ONLY what 2+ features of THIS service use
│       ├── repositories/              repos read by several features
│       ├── schemas/
│       └── services/{tapo,stream}/    adapters and hubs several features share
└── tests/
    ├── unit/
    └── e2e/
```

Three rules make this shape hold:

1. **`feature/<feature>/` owns everything the capability needs** — controllers, DTOs,
   repositories, schemas, services and its own `infra/` adapters. A feature is a vertical
   slice, not a layer.
2. **`shared/` is earned, not default.** A repository, schema or service moves to `shared/`
   only when a second feature needs it; otherwise it lives in its feature. This is the
   sharpening of AGENTS.md rule 3: `shared/repositories/` stays for repos read by 2+ features,
   which is what the 2+ rule and rule 24 ("no directory without a consumer") require.
3. **`app/` is process composition only** — `main.cc` and `rpc/`. No domain logic, no
   controller, no repository, no config resolution: that lives in `src/config/` (D20), a
   sibling of `feature/`. `src/server/` disappears — today's `AppConfig` handling already
   lives in `main.cc` (verified in `tts`, `camera` and `guard`), and **nobody subclasses
   `AppConfig`** anywhere in the tree, so no per-service config class is preserved.

`services/tts` is the reference for the service-level split (`app/` + `feature/` + `shared/`;
it has no config resolution of its own yet, so `src/config/` is added by whoever needs it).
Its *feature* shape is what gets flattened to the above: `feature/synthesis/api/http/
{controller,dto}` → `feature/synthesis/{controllers,dtos}`, `domain/` → `services/`,
`infra/supertonic/` stays (a feature may hold its own adapters), and its empty
`src/shared/services/` disappears until a second feature exists.

### 2.4 Dependency rules

Dependencies are organised in tiers, and the permitted edges are explicit (D18: a package may
consume contracts and clients — what it may never do is query another owner's data):

| Tier | Packages | May depend on | May never depend on |
|---|---|---|---|
| 1 · foundation | `lib/`: audio, cert, config, errors, grpc, mdns, nats, phrase, runtime, sqlite, storage, text, validation | third-party, other tier-1 `lib` packages | contracts, clients, services |
| 2 · wire | `contracts/*`, `lib/http` | tier 1 (`lib/errors`, `lib/grpc`), third-party (Drogon), generated protobuf | clients, services |
| 3 · transport | `clients/*` | tier 1 + tier 2 | other clients, services |
| 4 · service-aware lib | `lib/auth` | tiers 1–3 | services |
| 5 · services | `services/*` | everything above | another service's `src/` |

Rules that come with the table:

1. A tier may never point back up, and the graph has no cycles. Two units that need each
   other are one unit.
2. A contract cannot call a service: if a domain's data is needed, it is needed through a
   client, and the client is the only place a stub, a URL or a retry policy exists.
3. A service depends on packages and clients, never on another service's source.
4. Interface dependencies (types in public headers) are `PUBLIC`; implementation-only
   dependencies are `PRIVATE`. The helper enforces it (today everything is `PUBLIC`, §2.6).
5. Header-only (`INTERFACE`) when there is no state: `validation`, `text`, `phrase`.
6. **Enums live where they are used** (D12). A service declares its domain enums inside the
   feature that uses them; the vocabulary that crosses the wire (`UserRole`, `SyncOperation`,
   `TableName`, priorities) is declared once in the contract that owns it. There is no shared
   enum package, and no service keeps a copy of a wire enum — a copy drifts and breaks the
   frozen wire.
7. The tier table is checked mechanically: a script reads `target_link_libraries` in every
   `CMakeLists.txt` and fails on a forbidden edge, so the architecture is enforced by the build
   rather than by review.
8. **`lib/http` is tier 2, not tier 4, and that decides where three things live.** Verified:
   `packages/response` links nothing but `Drogon::Drogon`, so after the split `lib/http` needs
   only tier 1 and Drogon. The consequence is that no tier-1 package may reach it — which is
   exactly what `lib/config` does today: it holds the TOML reader **plus** the health controller
   **plus** the listener config. Only the reader stays (97 files consume it, tier 1);
   the **health controller and the listener config move to `lib/http`**, because they build the
   envelope and register routes. 18 files include `<controllers/health-controller.hxx>` and 14
   include `<server/listener-config.hxx>` — every service's `main.cc` and its wire test.

### 2.5 Naming

**One rule: the namespace mirrors the folder.** The group is part of the name, so a target
says where it lives and what it is without consulting the tree.

| Thing | Real target | Aliased as | Consumed as |
|---|---|---|---|
| lib package | `argus_lib_<name>` | `argus::lib::<name>` | `argus::lib::validation` |
| contract | `argus_contracts_<domain>` | `argus::contracts::<domain>` | `argus::contracts::camera` |
| client | `argus_clients_<domain>` | `argus::clients::<domain>` | `argus::clients::camera` |
| service | `argus-<name>` (executable) | — | the binary name |

```cmake
target_link_libraries(argus-voice PRIVATE
    argus::lib::audio
    argus::contracts::camera
    argus::clients::tts)
```

Notes that matter for the implementation:

- The `::` separator is reserved for ALIAS/IMPORTED targets, so the real target carries
  underscores and the alias carries the namespace. **Nested `::` aliases work** — verified
  by configuring a probe project with this exact toolchain (`argus::lib::validation` linked
  into `argus::contracts::camera` linked into `argus::clients::camera`: configure and
  generate clean).
- The folder never repeats the group (`packages/clients/llm`, not `clients/llm-client`).
- The drift that exists today (`argus::llm-client`, `argus::stt-client`, `argus::vlm-client`,
  `argus::tts-client` versus `argus::client-camera`) disappears: every client is
  `argus::clients::<domain>`.
- A target name is never spelled by hand in a `DEPENDS` list — the group comes from the
  helper, so a misplaced package fails at configure time instead of linking the wrong thing.

### 2.6 Build

One general `conanfile.txt` at the repository root is the single dependency manifest.
Packages declare no Conan graph of their own; each project's `CMakeLists.txt` imports only
the targets it links. `build-all.sh` resolves dependencies once. Third-party submodules
under `third_party/` stay compiled by the owner that needs them.

The helpers in `cmake/argus-module.cmake` are the enforcement point, and three of them need
work before Phase 2 can land:

| Helper | Change | Why |
|---|---|---|
| `argus_lib(NAME … GROUP …)` | new wrapper over `argus_module`, derives `argus::lib::<name>` | §2.5 naming |
| `argus_contracts(NAME … PROTO …)` | replaces the contract half of `argus_contracts_substrate` | §2.5 naming |
| `argus_clients(NAME … PROTO_ROOT … PROTO …)` | rename of `argus_client_module`, derives `argus::clients::<domain>` | §2.5 naming |
| `argus_module` | `HEADER_ONLY` option → `INTERFACE` library | today it hard-codes `STATIC` (`cmake/argus-module.cmake:31`), so `validation`, `text` and `phrase` cannot be header-only |
| `argus_module` | `PRIVATE_DEPENDS` | today `DEPENDS` and `SYSTEM_DEPENDS` are both linked `PUBLIC` (:35-36), so every implementation dependency leaks into consumer include paths and violates §2.4 rule 5 |
| `argus_service` | unchanged except the alias it links | already exists (:330) |

---

## 3. Architecture B — Microservices

### 3.1 Internal layout

```
services/<name>/src/
├── app/
│   ├── main.cc                 wiring, boot, listeners, the one advice line (D20)
│   └── rpc/                    gRPC listeners (infrastructure, never a feature)
├── config/                     this service's typed config, sibling of feature/ (D20)
├── feature/<feature>/
│   ├── controllers/            endpoints
│   ├── services/               the feature's logic
│   └── dtos/                   input/output
├── shared/                     only code consumed by 2+ features of this service
└── database/schema.sql         only if the service owns data
```

There is no intermediate `api/` layer, and no `feature/rpc/`. Feature slices may hold
non-HTTP capabilities with their own internal shape (`domain/`, `infra/`) as long as the
slice stays whole.

`config/` is **one folder per service, not per feature**: a feature that needs a value asks the
service config for it. It resolves the service's own section of `config.toml` through
`lib/config` — db path, schema path, ports, feature flags — and it is the only place a default
is written down.

### 3.2 Target services

| Service | Owns | Surface |
|---|---|---|
| `services/auth` | `refresh_token`, `device_login_challenge`, device credential | identity/credential validation contract, LAN-only pairing and registration, rate limiting |
| `services/identity` | `user`, `person`, `face_embedding` + `face_vec`, `user_invitation`, `invitation_redemption`, `user_portrait` | user context for auth, person directory, face identification, portraits |
| `services/sync` | rooms, connections, `audit_log`, `user_audit_log`, `user_action_log`, compaction state | `/sync` WebSocket, fan-out, audit persistence, action journal, realtime control RPC |
| `services/camera` | `camera.db` | camera/zone CRUD, media, object detection, gated actions |
| `services/guard` | `guard.db` | danger policy, incidents, gated actions (unchanged) |
| `services/llm` | `memory.db` | chat, tools, hosted memory |
| `services/stt` `services/tts` `services/vlm` | — (tts: voice preference) | AI capabilities |
| `services/voice` | — | voice sessions, VAD, reactions |
| `services/notification` | `notification.db` | notification policy and push tokens |
| `services/productivity` | `productivity.db` | calendar, project, reminder |
| `services/tunnel` | — | **deferred** (D19): byte-transparent remote transport, untouched until its own work starts |

**Eliminated:** `services/gateway` (its pieces are redistributed below) and the reverse
proxy concept entirely.

### 3.3 One domain, one certificate, no gateway

- **One domain, one certificate** (D11). There are no per-service hostnames and no
  per-service certificates: everything answers as `https://argus.local`, and `cert` issues
  a **single instance certificate** for that name (SAN `argus.local`, plus the tunnel
  hostname) that every service presents. The app is one application; it just talks to more
  than one process.
- **Routes are the unit of addressing.** The logical route (`/sync`, `/camera`, `/identity`,
  `/guard`, …) is part of the contract, and the app composes its API surface as one domain
  with path routes. Each service announces itself over mDNS with an SRV record carrying
  host and port, and the app resolves route → endpoint from discovery.
- **Every service terminates its own TLS** with that certificate and runs its own filter
  chain; the certificate is mounted read-only, the private key is never baked into an image.
- **One consequence, stated plainly.** DNS and mDNS resolve names to host and port, never
  to paths. With no dispatcher process, the port is where the process actually listens
  (`https://argus.local:7026/camera`) while the app's surface stays one domain with path
  routes. Literal path routing on a single 443 without a port requires a process that
  dispatches by path — that is a reverse proxy, which D5 eliminates. §7 keeps the one
  honest variant open.
- **Every service runs the filter chain** `DeviceFilter → ValidJsonFilter → JwtFilter →
  RoleFilter` from `lib/auth`. `JwtFilter` no longer queries a database: it validates
  through `clients/auth` (D6).
- **Auth owns the edge concerns**: LAN-only enforcement for pairing and registration,
  rate limiting before credential validation, one-time device credentials.
- **Relays disappear.** The gateway's bidi voice relay and the `/camera-stream` socket
  are gone: the client talks to `services/voice` and `services/camera` directly.
- **Frontend coordination is required**: the app moves from one endpoint to N discovered
  endpoints. This is a client-side change and must be declared in `packages/contracts`.

### 3.4 Data ownership

Each database has exactly one owner and is never mounted anywhere else. `identity.db`
splits: sessions and credentials to `auth`; people, faces, invitations and portraits to
`identity`; the audit tables to `sync` (`sync.db`, together with rooms and connections).
Cross-domain reads travel through `clients/*`; change feeds travel through NATS.

Enrolment stays atomic because `user`, `person` and `face_embedding` remain in one
database (D6). `auth` resolving the user context per request is the one added hop; it is
covered by a short-TTL cache in `auth` invalidated by the identity change event, so the
steady-state cost is one hop.

### 3.5 Realtime and audit

- **Changes (data)** travel over NATS as today: producers publish to the frozen subjects
  (`argus.<domain>.v1.change`), `sync` consumes, persists the audit diff and fans out.
  NATS keeps the write path independent of `sync` being alive.
- **Control (imperative)** travels over `clients/sync` gRPC: `AuthContextChanged`,
  disconnect, role-room replacement, directed emit to a user room.
- **Audit durability is a requirement, not a detail.** Since producers can no longer
  write the audit rows, a lost event is a change the clients never see. Every producer
  publishes through a **durable outbox** with a JetStream PubAck leg and a deterministic
  fingerprint per id: same id with the same fingerprint is a replay, same id with a
  different fingerprint is a conflict that is never dispatched. This is the pattern the
  repository already uses (`object_event_outbox`, `guard_action_outbox`,
  `notification_command` + inbox receipts).
- `before`/`after` capture and `JsonDiff::createFlatDiff` stay in the producer; `sync`
  persists and distributes. Daily compaction lives with the reader, in `sync`.
- **Retention is a 90-day TTL** (D15). Rows older than 90 days are pruned and compacted into
  a per-record summary. A client offline longer than the window re-bootstraps with a full
  `Synchronize`; the app shows "replica too old" instead of silently diverging. `sync` reads
  the window from config, and `contracts/sync` declares the resync semantics so a client
  cannot assume replay always works.
- **`user_action_log` belongs to `sync` with the rest of the audit trail.** It is alive today
  (`UserActionLogService::record`, six call sites across four `identity` features: user
  create/role/deactivate, invitation create/revoke/redeem, portrait reads) and AGENTS.md §7
  requires it — "keep invitation creation/revoke/redemption, user creation/role/deactivation,
  and portrait viewing in the server audit history". It is a server-side action journal with a
  retention of its own, not a side effect of the sync log: actor, record, `UserAction`
  (create/read/update/delete), full before/after, IP. So `sync` owns it next to `audit_log` and
  `user_audit_log`, the producer stops writing it directly, and `UserAction` and
  `TableName::UserActionLog = 20` — both frozen wire — are declared in `contracts/sync`.
  - **Write path:** a producer publishes an **additive** subject
    (`argus.<domain>.v1.user-action`) through its own durable outbox, exactly like the change
    events; `sync` persists. Deriving the journal from the change feed is not enough — a
    portrait read changes no row and a login writes a session, not a record — so the journal
    needs its own event, and it is append-only, which is why it fits the frozen
    creation-only paging rule.
  - **Read path:** no new HTTP route. Rows are insert-only, so the client gets them the way it
    gets every other table: `sync` serves one more table in the `Synchronize` page — a leg, not
    a route — and the table number is already frozen. Additive subject, so the rest of the wire
    is untouched (§1.8); the new row is declared in `wire-nats-subjects.md`, the authority,
    when Phase 3a creates it.

### 3.6 The change-event contract

Change events outlive the gateway, and their vocabulary must not die with `packages/socket`.

- **The authority is already written.** `docs/architecture/wire-nats-subjects.md` fixes the
  subjects (`argus.<domain>.v1.<event>`), their publishers and their payloads;
  `wire-sync-golden-frames.md` and `wire-sync-tables.md` fix the `/sync` frames and the audited
  tables. Subjects and payload shapes do not change (§1.8) — this plan decides which package
  carries them and who publishes, not what they contain.
- **`packages/socket` is three jobs in 8 files:** the **payload vocabulary**
  (`sync-change.hxx`: the `users` / `action` / `emit` / `disconnect` / `replace_role_rooms` field
  names and the payload builders, plus `SocketEmitDto`), the **transport**
  (`socket-service.{hxx,cc}`), and the **fan-out with rooms** (`packages/room`). A fourth thing is
  the misrouted `nats-identity-change-sink` (Phase 1 step 0).
- **Where each job goes:** payload vocabulary and field names → `contracts/sync`, declared once
  next to its proto (D12); transport, rooms, fan-out, disconnect and directed emit →
  `services/sync`; the publish path → `lib/nats` (subjects and bus, unchanged) plus the
  **durable outbox of each producer**, which is a feature of that service and not a package.
- **Who publishes:** `camera`, `guard`, `notification`, `productivity` and `identity` (its RPC
  builds `SocketEmitDto` today) all link `packages/socket` now. After the split each one builds
  the payload from `contracts/sync`, writes it to its own outbox in its own database, and
  publishes to the frozen subject through `lib/nats`. None of that is shared code, which is
  exactly why `socket`, `room` and `sync` die instead of becoming packages (D1, D18).
- **One leg is imperative, not a change:** `AuthContextChanged` and the role-room replacement
  travel over `clients/sync` gRPC (D8), so `auth` does not publish a change event to trigger them.

---

## 4. Code rules

These rules govern every change in this refactor. They are AGENTS.md rules 1–27, restated for
the new layout and extended where the layout needs it. Where a rule already existed its number
is kept (`rule 3`), so the migration can be traced against the current file. **AGENTS.md rule 19
applies to this document too: modernising code that a change touches is part of the change,
never a separate task.**

### 4.1 Files and names

| Thing | Rule |
|---|---|
| Header / source | `.hxx` / `.cc`. Never `.h` or `.cpp` |
| File name | hyphenated, lower-case: `camera-control-service.hxx` |
| Test | `*-test.cc`, in the owner's `tests/unit/` |
| Repository | `{entity}-query.hxx` (SQL + param structs) + `{entity}-repository.{hxx,cc}` (rule 3) |
| Service / controller | `{capability}-service.{hxx,cc}`, `{capability}-controller.{hxx,cc}` |
| Schema | `{entity}-schema.{hxx,cc}` — row → struct mapping |
| DTO | `{action}-dto.{hxx,cc}` request, `response-{action}-dto.{hxx,cc}` response (rule 10) |
| Config | `{service}-config.{hxx,cc}` in the service's `src/config/` (D20) |
| Module | one folder = one module = one `CMakeLists.txt` (rule 25) |
| Private-by-convention | anything under `details/` — never included from outside its unit |

One primary type per file, and the file is named after it. Two unrelated types in one header
are two files.

### 4.2 Declaration rules (headers)

The order inside a header is fixed, and it is what a reviewer reads:

```
1  #pragma once
2  system includes       <cstdint> <optional> <string> <vector>
3  third-party includes  <json/value.h> <drogon/...>
4  argus includes        <errors/error-code.hxx> <camera/camera-sync-client.hxx>
5  namespace             argus::<unit> — one per unit, no `using namespace` here
6  constants             inline constexpr: `kMaxFrameBytes`
7  enums                 enum class + toString/fromString at boundaries only (rule 1)
8  parameter structs     {Entity}CreateInput / {Entity}UpdateInput (rule 2)
9  the primary type      class / struct / interface
10 free functions        only when they are the point of the file
```

- **`#pragma once`**, never include guards.
- Include exactly what the header's own declarations use. Forward-declare a type the header
  only mentions by reference or pointer; include it as soon as a member, a return type or a
  by-value parameter needs the complete type.
- **No `using namespace` in a header, ever.** In a `.cc`, `using namespace {entity}_query;` is
  mandatory when the file implements a repository (rule 3).
- Members are declared in initialisation order and initialised **in the declaration**
  (`int retries_{3};`), except when a constructor parameter decides the value.
- Public surface first, then protected, then private — the contract before the mechanics.
- A class with dependencies declares them as private members (rule 4) above the methods that
  use them.
- Declarations are `const`-correct, `[[nodiscard]]` where dropping the result is a bug, and
  `noexcept` on getters that cannot throw.
- No virtual destructor on a type never deleted polymorphically, and no PIMPL: nothing here is
  published (§2.3).
- A parameter struct is declared in the header that declares the function taking it; for a
  repository method it lives in `{entity}-query.hxx` (rule 2).

### 4.3 Definitions (`.cc` files)

- Anonymous namespace for everything internal to the file: helpers, lookup tables, pure
  functions. `static` at file scope is not used.
- Definitions appear in the same order as their declarations.
- The constructor initialiser list follows member declaration order.
- Another unit's `details/` is never included.
- Blocking work is never called from an event-loop callback (rules 13c, 21): wrap it in
  `BlockingTask` and `co_await` the async variant.
- The request path is coroutines end to end; a coroutine never blocks the loop.

### 4.4 Types, ownership and modern C++20 (rules 16, 19)

- No owning raw pointers: `unique_ptr` by default, `shared_ptr` only with a real shared
  lifetime, references and `span` for non-owning.
- No C-style casts, no `typedef` (use `using`), no `NULL`, no C arrays, no `std::bind`, no
  `printf` family.
- `string_view` and `span` at hot or read-only boundaries; own a `std::string` only when the
  value is stored.
- `constexpr` constants, `enum class` vocabularies, designated initializers for parameter
  structs (rule 2), structured bindings over `.first`/`.second`, ranges and algorithms over
  index loops.
- `std::optional<T>` for "may be absent", a typed result for "may fail" (§4.7). Failure is
  never a magic value.
- `explicit` on every single-argument constructor.
- Rule of zero by default; a type that needs one special member declares all of them.

### 4.5 Functions and signatures

- **3+ parameters ⇒ a parameter struct** (rule 2), in every layer, built inline with
  designated initializers: `service_.login({.userId = ctx.sub, .token = token})`, listing every
  field in declaration order.
- One function, one job. A controller endpoint is 4–8 lines (rule 12): parse the DTO, read the
  attributes the filters guarantee, call the service, return `ApiResponse::ok(...)`. No
  `if (!json)`, no manual field extraction, no try/catch.
- Names say what, not how: `findActiveByUser`, not `doQuery`. Booleans read as assertions:
  `isEnabled`, `hasPendingTurn`.
- No output parameters where a return value will do.

### 4.6 Async, threading and cost (rules 13b, 13c, 21)

- Never hardcode a thread count: `ThreadBudget` (`computeThreads`, `batchThreads`,
  `heavyThreads`, `lightThreads`, `inferenceSlots`).
- Every heavy operation exposes a blocking method **and** an async variant; the event loop only
  ever calls the async one.
- One transaction or multi-row statement over N statements in a loop; prepared statements and
  bulk `IN (...)` over repeated round-trips.
- No allocation churn in hot paths, no JSON re-serialisation round-trips, no per-request vector
  copies.
- Fan-out stays row-scoped and per-recipient: batching never changes observable behaviour.
- Every block-processed audio path uses `argus::lib::audio`, never a hand-rolled conversion.

### 4.7 Errors

- A domain error is a typed definition from `lib/errors`, declared in the owning contract
  (`camera-errors.hxx`): never a bare string, never a magic status code.
- Validation failures come from the DSL and throw `ValidationException(errors, 422)`; the one
  shared advice catches it — registered by a single line in the service's `main.cc` — and no
  controller wraps anything in try/catch (rule 11).
- The envelope is built in exactly one place: `ApiResponse::ok` / `created` / `noContent` for
  success, `validationError` / `error` for failure. A handler never assembles `{status, info,
  errors}` itself, and a filter rejection goes through the same formatter as a controller —
  that is what makes the format impossible to diverge route by route.
- An error code is declared once, in `lib/errors`, and consumed as a type. A string literal for
  a code is the duplication that breaks the wire the first time the two copies disagree.
- **One way to reject: throw.** A controller, a filter or a service that refuses throws a typed
  exception; it never builds an error response. `ValidationException` for validation,
  `ResponseException` for a declared failure, and the code (with its status and message) comes
  from the `ErrorDefinition`, so `throw ResponseException(IdentityErrors::UserNotFound)` cannot
  disagree with itself. The `get400Response`…`get503Response` family disappears: the
  status-to-code pairing it hardcoded is what the catalog now owns, and keeping both is what
  lets two sibling routes answer `409` and `500` for the same failure. What remains
  hand-built are the two framework callbacks that cannot throw (unmatched-route 404/405) and
  the OPTIONS answer — all three in `lib/http`, all three through `ApiResponse`.
- Exceptions are for validation and the exceptional; a predictable failure returns `optional`
  or a result type.
- A `catch` block never swallows: it logs with context and maps to a typed error or rethrows.

### 4.8 Dependency injection (rule 4)

- Manual. No framework, no service locator, no singleton.
- Dependencies are private members with a `_` suffix: `repository_`, `userRepository_`,
  `service_`, `jwtService_`.
- A repository or service is never constructed as a local variable or temporary: it is wired in
  `app/main.cc` and passed down.
- Controllers hold one `service_` member; never static methods.
- Config is resolved once in `src/config/` and passed down; `main.cc` reads it and wires it.
  A feature never reads the environment or a TOML file on its own (D20).

### 4.9 Data access (rules 3, 21, 22, 26, 27)

- SQL lives only in `{entity}-query.hxx`; repositories live in `repositories/`; a feature owns
  its repositories, and only a 2+ consumer moves them to `src/shared/repositories/` (§2.3).
- `create()` builds the schema from the input and uses `insertId()` — no extra query.
  `update()` is PATCH semantics: optional fields, dynamically built SET clause, then re-fetch
  through `findById()`. Soft delete is `deleted_at`. Syncable repos implement
  `find/findDeleted/findLast/findLastDeleted` (rule 3).
- A service opens only its own database. Cross-domain reads go through
  `argus::clients::<domain>` over the typed contract; a package does exactly the same and never
  touches a domain's rows (D18).
- No `file(GLOB)` for sources; module folders are the only glob (rule 25).
- One `database/schema.sql` per owner, applied only by its owner (rule 26).
- Features extend the schema additively (rule 22): no partitioning tricks, no aggressive
  denormalisation, no over-indexing. Indices exist for real hot queries (sync cursors, join
  columns).

### 4.10 Comments (rule 20)

- Comments only at class, namespace or function scope, short and direct.
- No comment on an individual statement: if a statement needs one, rewrite the statement.
- No multi-line doc blocks, no commented-out code, no banner art.
- The **why** of a design decision goes to the unit's `CONTEXT.md`, never into the code.

### 4.11 Tests

- doctest, `TEST_CASE("...")` in `tests/unit/*-test.cc`, one file per unit under test.
- A test names behaviour, not the method: `TEST_CASE("a replay with the same fingerprint is
  dropped")`.
- Tests pass in isolation (`--test-case`, `--order-by`) and never depend on execution order or
  on a shared database file.
- Every step in §5 lands with its tests green in the same change, and anything the frozen wire
  depends on is covered by a golden test.

### 4.12 Forbidden, without exception

```
raw owning pointers         typedef / C-style casts / NULL / printf
file(GLOB) for sources      a second type in a file named after the first
3+ parameter signatures     static controllers / service locator / singleton
local repository instances  SQL outside {entity}-query.hxx
cross-service DB access     a package that queries a domain
comments on statements      commented-out code
speculative folders         an empty shared/ waiting for a future consumer
a copied wire enum          a hand-rolled gRPC client outside packages/clients
```

### 4.13 Enforcement

| Tool | State | Role |
|---|---|---|
| `.clang-format` | **exists**: LLVM base, Allman braces, 2-space indent, 80 columns, 4-space continuation | formatting is mechanical, never discussed in review |
| `.clang-tidy` | **to add** (Phase 2): `cppcoreguidelines-owning-memory`, `modernize-*`, `performance-*`, `bugprone-*` | catches rules 16 and 19 mechanically |
| `scripts/check-deps.sh` | **to add** (Phase 2) | fails the build on a forbidden edge of §2.4 |
| `build-all.sh dev` | exists | 0 errors, 0 warnings (rules 19, 21) |

---

## 5. Migration plan

Ordering rule: finish what is mechanical before starting what is structural. Phases 1–2
do not move a single domain; phases 3–5 do, and each one is independently revertible.

### Phase 0 — already executed (context, not work)

The strangler migration is done: 11 services, per-domain databases, NATS with durable
JetStream legs, typed gRPC contracts, per-project Conan graphs and presets, one image per
service. The build helpers `argus_module`, `argus_client_module` and `argus_service` exist
in `cmake/argus-module.cmake`. `packages/common` is already gutted to transition shims
and `packages/argus-contracts` is already split into `contracts/`, `clients/` and `grpc/`.
A rename wave (`packages/argus-*` → `packages/*`) was staged but **uncommitted**, and it was
larger and messier than a rename: **1341 renames, 119 of them carrying content edits on top of
the move**, plus 22 files added and 10 deleted. About 110 of the moves are the intended
decomposition (`common` → 17 destinations, `contracts` → `clients`/`grpc`, the four `*-client`
packages → `clients/`). Twenty entries are `.gitignore` rotations, one pair is a real break,
and one group of files was dropped in passing:

- **Misrouted, breaks the build:** `nats-identity-change-sink.{cc,hxx}` was moved from
  `packages/argus-socket/` into `packages/gateway/` — a folder with no `CMakeLists.txt` —
  while `services/gateway/CMakeLists.txt:130` and `services/gateway/src/main.cc:23` need it
  at `services/gateway/src/shared/services/socket/`. A full `CMakeLists`-versus-disk scan
  finds **exactly one missing source in the whole tree**, and it is this one: `argus-gateway`
  cannot build from the working tree. **Done** (Phase 1 step 0): moved, and the emptied
  `packages/gateway/` deleted.
- **The `.gitignore` rotation is content-safe, and was left alone.** 19 renames plus the
  deletion of `services/argus-voice/.gitignore`, each target receiving the next source's file.
  Verified by md5 on both sides of every pair: **every source is byte-identical**
  (`9c514cb6cde9ec1aded4407cd86e7f56`) except `argus-camera → services/camera`, which carries
  the richer file (`aee603aee5e2de0e87f53a66af3994d2`, the one with `go2rtc.yaml` and
  `bench-results/`) and **is correctly paired**. No folder lost a rule it had and none gained a
  foreign one, so no content needs restoring and the rotation is cosmetic: since the sources are
  byte-identical, git's pairing is arbitrary and re-pairing would only churn the record.
- **Eleven protos were dropped, and put back.** No `CMakeLists.txt` lists them as a codegen input
  (`ai/v1/{llm,stt,vlm,tts}`, `camera/v1/{camera,zone,stream}`, `memory/v1/memory`,
  `productivity/v1/{calendar,project}`), and the wave pruned them along with the two that
  `docs/architecture/wire-sync-tables.md` and `wire-device-identity.md` cite as the frozen
  record: `sync/v1/contracts.proto` (`SyncOperation` 0–7, `TableName` 0–23, `SYNC_LIMIT`) and
  `common/v1/base.proto` (the `{status, info, errors}` envelope and the `ERROR_CODE_*` values).
  All 19 are back at their canonical protobuf paths under `packages/contracts/`. `buf breaking`
  is a declared gate of that package (§1.8) and the policy retires fields with `reserved`, never
  by deleting the file, so pruning the protobuf surface is a wire change to declare — not a
  rename's side effect. No value changed.

### Phase 1 — mechanical cleanup (no domain moves)

**Every phase in this plan closes buildable and testable.** The base is the tree as it stands,
and that tree did not build until step 0 repaired the misrouted file below, so step 0 came
before anything else. No phase is done until `./scripts/build-all.sh dev` is clean and that
phase's tests run — a phase that cannot be built cannot be verified, and the new architecture is
only usable once its own tests run against it.

| Step | Action |
|---|---|
| 0 | **Repair the rename wave before committing it — done.** `nats-identity-change-sink.{cc,hxx}` moved to `services/gateway/src/shared/services/socket/` (where its includers expect it) and the emptied `packages/gateway/` deleted, so the tree builds again. Verified with a repo-wide `CMakeLists`-versus-disk scan (exactly one missing source before, none after) and by building `services/gateway`. The `.gitignore` rotation is left as staged: content-safe, verified by md5 on both sides of every pair (preamble above) |
| 1 | **Done** — `build: land the package and service rename wave`. What "finish it where incomplete" turned out to mean: 17 leaf packages got the `CMakeLists` the split never gave them (`access`, `config`, `grpc`, `hash`, `json`, `nats`, `storage`, `text`, `threading`, `validation`, plus the clients `camera`, `camera-actions`, `health`, `identity`, `notification`, `productivity`, `voice`); the stray singular `package/` tree was deleted, its copies being the staler ones (the tts client still said `argus::sdk::`); `packages/argus-contracts` stopped being a project of its own; and the wiring the split left broken was repaired — camera's guard feature needed `argus::client-notification`, tts's synthesis needed `argus::hardware-profile` (it used to arrive through `argus-common`), memory-core needed `contract::sync` and `argus::audit` with the guards at top level so they hold for consumers as well as the standalone build, and `argus_client_module` had to re-resolve gRPC and `Threads` in the caller's directory scope, because imported targets are directory-scoped while the substrate early-returns once configured. `argus::sdk::*` became `argus::client::*` in the one source still naming it, and every script, Dockerfile, compose file and doc that named a path or an `--only` project was repointed. Verified by building and testing all 19 standalone projects on the dev profile — 7, 12, 8, 8, 12, 14, 14, 9, 20, 29, 18, 23, 32, 9, 8, 9, 21, 12, 14 tests, all green, no first-party warnings. Two tests flaked once under four parallel builds (`cert-san-test` saw an unchanged fingerprint after a hot reload, `memory-backpressure-test` reported `BAD_COMMAND` while another build was relinking it) and both pass isolated. **Gotcha for later phases:** the `CMakeCache.txt` files under `packages/*/build` and `services/*/build` keep pre-rename paths and make a project fail in ways that look like source defects; rewrite them or wipe the build dirs before believing a failure |
| 2 | **Done** — `build: delete the packages/common transition shim`. The package had been reduced to two include redirects (`<response-exception.hxx>`, `<http/api-response.hxx>`), a pass-through object and the envelope's suite, while its `CMakeLists` still carried the whole pre-split closure (`access validation json hash text threading config storage nats`, plus `pkg::response-http`, Drogon and cnats PUBLIC) and its own `CMakePresets.json`/`conanfile.txt`; it said of itself "Deleting this package is the last step once no consumer links it", and that was the point — deleting it correctly meant repointing the consumers, not removing a directory. 41 sources now include the real paths; 33 `CMakeLists` declare the leaves each target actually uses; every `add_subdirectory` the removal introduced is `if(NOT TARGET <alias>)`-guarded, so the provider adds its subtree unguarded and consumers guard, which is what makes a standalone configure and a consumed one agree; the shim-era `packages/response/src/http/CMakeLists.txt` (`pkg::response-http`) went with it, `scripts/build-all.sh` is down to 18 projects, and the root `AGENTS.md` plus five other docs follow the files to their real owners. The envelope suite moved to `packages/response/tests/unit/api-response-test.cc`, same ten cases, with the definition-backed assertion rebased onto the response package's own `ErrorDefinition` — `services/tts` keeps asserting `TtsErrors::TtsNotLoaded` where that contract lives. **The deletion also exposed a defect worth more than the deletion**: `enable_testing()` was called mid-file in every project, so any package added before it got no `CTestTestfile` and its tests were compiled but never run — ctest's `subdirs()` chain dead-ends silently at the first directory that lacks one (proved with a minimal repro, and located in the tree: `packages/cert/build/dev/CTestTestfile.cmake` says `subdirs("config")` while `config/CTestTestfile.cmake` did not exist). Hoisting `enable_testing()` to just after `project()` in all 18 project files takes cert from 7 to 11 reachable tests, socket 4 → 5, sqlite 1 → 2, and `api-response-test` now runs in 17 of the 18 projects — the 11 that name `packages/response` and the 6 that reach it through another package's guarded add, with only `packages/intent` left out — instead of only in the shim's own build. The suites that no longer appear in a given project are the old closure going away, not lost coverage: `sha256-test`, `nats-wrapper-test`, `thread-budget-test` and `validation-dsl-test` still run where their packages belong (tts drops 9 → 7 because it no longer pulls `hash` and `nats`, which it never used). Verified: 282 `add_subdirectory` paths resolve on disk, no `CMakeLists` names `argus_common` or the dead `pkg::response-http` alias and no target links either, a header-owner-versus-link-closure audit over every target that lost the link (one real gap — `tts-synthesis-http` compiles the validation DSL and did not declare `argus::validation`, which the compiler confirmed; declaring it then required tts's standalone block to own the guarded `packages/validation` subtree too, which only the configure caught), plus a second gap the audit's `argus_module`-only scope could not see and the compiler found after every other project was green: `argus-tunnel-relay` is a plain `add_executable` whose `main` builds a `NatsBus` and subscribes to the push-intent subject, but it linked only `tunnel-core` and had been getting the nats include root from the shim's PUBLIC closure — it now links `argus::nats`, and tunnel is green at 9 tests. Verified too: `./scripts/build-all.sh dev` is green on all 18 projects with no first-party warnings. Deferred to step 11: the 12 comments and the two `docs/architecture` tables that still name `argus-common`; and the three different sentinels the standalone blocks use (`PROJECT_IS_TOP_LEVEL`, `NOT TARGET Drogon::Drogon`, the original third-party probes) are all safe but not the same question — unifying them is hygiene for a later step |
| 3 | **Done** — `build: merge the json and hash packages into text`. Three leaf tier-1 packages (`text` = text-norm alone; `json` = json-util + json-diff over jsoncpp; `hash` = base64, fnv-hash, sha256) all exposed the same include root `src` with their utilities already under `src/shared/utils/<utility>/`, so the merge is a target-level operation and nothing else: **0 `.cc`/`.hxx` changed**, all eight `git mv` renames at 100% similarity, and every `#include <shared/utils/...>` in the tree resolves before and after. `packages/text/CMakeLists.txt` now declares one module with all nine sources (`DEPENDS Drogon::Drogon` for jsoncpp) and wires `sha256-test` — which moved to `packages/text/tests/unit/` — against `argus_text`; `packages/json` and `packages/hash` are gone. 16 `CMakeLists` repointed (every guard and every link list; where a target listed both aliases the pair collapsed into one link), the 20 `json`/`hash`/`text` guard blocks became 15 (`sync`, `camera`, `gateway`, `guard` and `llm` guarded both packages adjacently), the four migration tools that reach the checksum header by include path now point at `packages/text/src`, the two `AGENTS.md` key-file rows follow, and the orphaned `build/dev/json` and `build/dev/hash` directories inside eleven project build trees went with the packages. The rewrite was scripted and then reviewed line by line, which is how the one defect surfaced: in `packages/memory/CMakeLists.txt` the `json` and `text` guards were not adjacent — the `nats` guard sat between them — so the collapse left two `if(NOT TARGET argus::text)` blocks; benign (the first creates the target, the second's condition is then false, and the project built green with it) but noise, and removed. Two shapes worth recording: `services/gateway` guards `packages/text` without linking it in any target — it repointed a guard whose purpose is the standalone configure of sources that include the JSON headers through the packages it does link — and `notification`/`productivity` used to link `argus::hash` while owning only the `json` subtree, getting the target from `packages/sync`'s guard; they now own `packages/text` themselves, so that accidental dependency is gone as a side effect. Verified: no `CMakeLists` anywhere names the two old aliases or paths, each of the 17 files that links `argus::text` owns exactly one guarded subtree (the two feature modules under `services/guard/src/feature/` excepted — their project owns it), `sha256-test` now runs in 13 of the 18 projects and six gained it with no suite lost (cert 11 → 12, socket 5 → 6, identity 11 → 12, intent 4 → 5, memory 13 → 14, tunnel 9 → 10), and `./scripts/build-all.sh dev` is green on 18/18 projects with no first-party warnings. Deferred to Phase 4's header normalization: the namespaces inside the merged package still read `argus::hash` (sha256), `Fnv1a` at global scope and `json_util`, while §4.2 rule 5 wants `argus::<unit>` — renaming them reaches into five consumer sources and their tests, which is not this step |
| 4 | **Done** — `build: distribute the access vocabulary and delete the package`. `packages/access` was one header of 32 `enum class` declarations with 64 inline `toString`/`fromString` helpers, plus `role-access.hxx`, the single `UserRole → TableName → permission` table that both `RoleFilter` and the sync engine read; the enums went to the unit that owns each (D12), the role table and its suite into `packages/auth` (the row's `lib/auth`, which Phase 2 renames), and the package is gone. **One deliberate deviation from the row's letter**: the row sends `CameraRecordMode` and `ZoneType` to `services/camera`, `ReminderDetailStatus` to `services/productivity` and `EventSeverity` to "its consumers", but `packages/sync`'s cross-domain repositories and schemas name all three and rule 3 forbids a package reaching into a service's source, so they went to the contract of the domain that owns them — D12's "the wire vocabulary is declared once in the contract that owns it", which is also what lets a client speak it — and `StoredFileCategory` went to storage's own vocabulary (the row's `lib/storage` is Phase 2's rename). Four contract packages did not exist (`contracts/{auth,voice,productivity,notification}`) and are created on `camera-contract`'s model: an `INTERFACE` target, a `contract::<domain>` alias, the package directory as include root, and a vocabulary round-trip suite — `camera-contract` itself had none at `HEAD`, so its suite is new too, as are `sync-contract`'s and the ones in `storage`, `phrase`, `identity` and `packages/auth` (the moved `role-access-test`). 30 of the 32 enums are byte-identical to their old bodies, enumerators and all 64 helpers compared token by token, and the two that are not in the tree — `MemoryScope`, `MemorySource` — are exactly the two with no consumer in it. `FeedbackLabel` was deleted with them and restored by the build gate: the only spelling of it anywhere is its helper `feedbackLabelFromString`, so the grep that cleared the dead pair looked for a name nothing writes, and `services/camera`'s build of `guard-regression` stopped on `guard-repository.cc:974`. The include rewrite is 80 files — 73 one-line swaps, 7 that only dropped a line they never needed, and 13 that named the vocabulary and included nothing, because the old header arrived transitively through service headers; 12 of those 13 were found by auditing every file's first-party include closure rather than by compiling one error at a time. The CMake rewire was scripted from the vocabulary each target's sources name, helpers included, because a consumer can name only `tableNameFromString`: `argus::access` is gone from every link list and the 31 `if(NOT TARGET <alias>)` guards the step added follow the house form. Making `packages/room` and `packages/clients/llm-client` reach `role_access` pulled `find_package(jwt-cpp)` into four projects that did not declare it — `socket`, `memory`, `llm`, `voice` — whose `conanfile.txt` now requires `jwt-cpp/0.7.2`, the version the other nine already carry. Verified: `./scripts/build-all.sh dev` green on 18/18 projects with no first-party warnings, the reached-test ledger 241 → 288 with `enums-test` the only suite lost (`intent` alone goes down, 5 → 4, because it no longer needs `packages/auth` to name a phrase enum), no `CMakeLists` naming the old alias and no source naming the old paths. **Flagged for step 9**: `packages/clients/llm-client` is tier 3 and now links `argus::auth`, tier 4 — §2.4 rule 1 says a tier may never point back up, and the tool framework this row moves into `services/llm` is the only thing that reads `role_access`, so that move removes the edge by construction rather than by patching it here. Deferred to the steps that own them: `AGENTS.md`'s four mentions of the package (11 and 12) and `wire-sync-tables.md`'s citation of the old header (13; the values it freezes do not move) |
| 5 | **Done** — `build: delete the dead artifacts left by the split`. The row's three items were measured before anything was deleted, and two of them were drafts that did not survive measurement. **`user_action_log` is live**, exactly as the row says: its repository is `packages/audit/src/shared/repositories/user-action-log/user-action-log-repository.cc` (the statement beside it is `INSERT INTO user_action_log`), the table is part of the frozen sync vocabulary (`sync-contract/src/shared/contracts/table-name.hxx:82,116`) and its schema is `packages/identity/database/schema.sql:198` — untouched, waiting for §3.5's move to `sync` in Phase 3a. **The gateway's mounts and its config in `argus-deploy` are all live**: each of the block's seven mounts has a reader (`[cert] dir`/`ca_cert`/`server_cert` plus `listener-config.cc`'s `certs/server.{pem,key}` default; `[identity] db` and `[gateway] db`; both `schema` keys; `[face] enabled` → `FaceService::init("models/face")`), all **85 keys** of `config.gateway.toml.example` are read (60 by their literal `"section.key"` in first-party code, 22 consumed by Drogon's own loader rather than by first-party code, and the 3 under `[mdns.txt]` as a *table* — `ConfigService::getStringPairs("mdns.txt")`, `mdns-service.cc:115` — which is exactly what a per-key grep reports as unread), and the compose declares two named volumes and uses both, so no volume is unmounted. **The "unused presets" reduce to one block**: all 18 projects carry a tracked `CMakePresets.json`, 17 define only the `dev`/`prod` configure and build presets that `build-all.sh:101-104` calls, and the exception was `services/gateway`'s `testPresets` — the tree's only one, with no `ctest --preset` caller anywhere (the gate runs a bare `ctest --output-on-failure` inside `build/dev`) — now deleted, so the file has the shape its 17 siblings have; Conan's generated `CMakeUserPresets.json` files are not artifacts (untracked, gitignored, ignored by every `Dockerfile.dockerignore`). What was actually dead, beside them: the gateway image's `mkdir` created `third_party/go2rtc camera stream productivity notification memory`, none of which its own compose block mounts — `CONTEXT.md:446` states the gateway links no go2rtc code and "neither mounts nor spawns go2rtc" (go2rtc is the camera's, `Go2rtcManager` and `[camera] go2rtc_bin`/`go2rtc_config`), and the other five are the camera, productivity, notification and memory images' own data directories — so the list is now `certs database gateway models`, exactly the directories that block mounts into, file mounts aside, a rule that also reproduces guard's existing `database guard`; `argus-deploy/config.memory.toml`, named by the ten `Dockerfile.dockerignore` files, has not existed since the f8 wave and the rule becomes `config.guard.toml`, which does exist (mode 600, gitignored) and was missing from every list while every Dockerfile does `COPY . .` from the repo root — so it was copied into every image's build stage; `docker/runtime` named a path retired in `b8233d0` and tracks nothing; `argus-deploy/.env` (host-local `ARGUS_*` paths, not secrets) and `argus-deploy/data` (15 MB of live deploy state — the databases the services open and the rustfs object tree — whose exclusion the root `.dockerignore:5` already declares, except a `Dockerfile.dockerignore` replaces that fallback, so the eleven services were the ones not getting it) joined the eleven lists; `services/guard` was the only one of the eleven services without a list of its own, so its `COPY . .` fell back to a root `.dockerignore` that ignores neither `certs/` nor `third_party/go2rtc` nor the deploy configs and its build stage took the whole repository — it now has one, byte-identical to the corrected ten (md5 `4608df190d07e800136a50173a6a5282`); and `scripts/build-all-test.sh` asked for `--only common`, the package step 2 deleted, so the CI workflow's "Test build orchestrator" step — which runs before "Build and test standalone projects" — had been failing with `[error] unknown project for --only: common` and exit 1 since then; it now uses `cert`, and the choice is forced by what the two cases measure (they count bare `cmake` calls, so the project must have no extra targets, which leaves `cert`, `socket`, `sqlite`, `sync`, `memory` and `intent`; `packages/text` is a subtree its consumers build, not one of the 18, so the obvious successor could not be used). `services/productivity/config.toml.example`'s `[identity] db = "database/identity.db"` has no reader — the service reads `productivity.db`/`productivity.schema` and its identity access filter reads `identity.target`/`identity.rpc_secret` — a leftover of pre-split direct-DB access, and was deleted. Verified: `./scripts/build-all-test.sh` exits 0 where it exited 1, the edited preset file parses and `cmake --list-presets` lists `dev` and `prod` (and the gateway configured and built from it inside the gate), the eleven ignore lists are one file, and `./scripts/build-all.sh dev` is green — exit 0 — on 18/18 projects, the reached-test ledger unchanged at 288 because this step touches no source, every configure warning in the run third-party (ccache's absence and the `CMAKE_CXX_STANDARD` notices from ncnn, glslang, llama.cpp and openfst) and none first-party. Nothing in the tree builds an image — CI runs the two shell scripts only — so every Dockerfile and compose claim above is static: a mount-versus-`mkdir` audit and a config-reading audit, not a container run. Flagged with the reasoning recorded, not fixed: nine Dockerfiles still create the whole monolith `mkdir` list, deferred because the keep-set needs the package-level defaults folded in first (`certs/` from `listener-config.cc:37-38`, `models/` from `FaceService` and the model loaders, `database/` from the DB-owning packages) and no gate would exercise the change; `services/gateway/tools/probe-captures/` (196 files, 816 KB of recorded probe transcripts) is named by no build, source or document and is a record rather than an artifact, so deleting it is its owner's call; and `argus-deploy/data/` holds untracked residue — a 0-byte root-owned `schema.sql`, a 0-byte `camera.db`, a stranded 864 KB `identity.db` that `migrate_identity_dir()` only folds in when the target directory is empty — left because it may hold rows the live database does not. Also for step 11: `argus-deploy/CONTEXT.md:180-186,408-409` still describes per-database named volumes and gateway `[productivity] db`/`[notifications] db` keys that no longer exist. Full report: `docs/history/reports/f1-5-dead-artifacts.md` |
| 6 | **Done** — nothing deleted, and the row's reason did not survive measurement. It says `socket` and `room`'s "only consumers are the gateway and each other"; counting the code that names their headers gives **eight** parties, not two. `packages/room`'s `room-manager.hxx` is included by `packages/socket` (`socket-service.hxx:7`, `sync-change.hxx:6`), `packages/sync` (`feature/socket/sync/services/sync-service.hxx:15`), `services/gateway` (`src/main.cc:19`, `sync/sync-fan-out.{hxx,cc}`, two tests) and `services/camera` (`src/main.cc:34`); and all four of `packages/socket`'s exports are spoken outside the gateway — the sink contracts by `notification` and `productivity` (and `camera` for its own), the emit DTO by `identity` and `packages/sync`, and `socket-service.hxx` itself by `identity`'s user/invitation/auth services and `audit`'s two log services. The deletion is deferred anyway, for the reason §9.1's `socket` row already gives: that package's three jobs (payload vocabulary → `contracts/sync`, transport and fan-out → `services/sync`, `lib/nats` + each producer's own outbox, §3.6) need `services/sync` to exist first, which is Phase 3a's, itself gated on the gateway dying (D5) — deleting now would break six projects that still need the vocabulary. What the correction does change is **Phase 3a's scope**: it inherits a repointing set of seven consumers that name the sink contracts, the emit DTO or `socket-service.hxx` directly (`identity`, `audit`, `camera`, `notification`, `productivity`, `packages/sync` and the gateway), where the row implies one; `room` is not free either, since `sync`'s `sync-service.hxx` and `camera`'s `main.cc` include it today. §9.1's `room` row is corrected to match — its 2-file count was right, its consumer claim was not — and the evidence is in `docs/history/reports/f1-6-socket-room-consumers.md`. Flagged there, not fixed: `services/guard` and `packages/cert` guard both subtrees without linking them or naming either package in any source, dead guards left by step 2's scripted insertion, kept for the step that unifies the standalone-block sentinels |
| 7 | **Done** — `build: merge threading and hardware into runtime`. The phase's first real code move: `threading` (BlockingTask, CancellationToken, ThreadBudget, ai-init — five headers, one source, one suite) and `hardware` (one wrapper folder declaring two targets over one source pair, the ncnn-free CPU profile and the Vulkan GPU probe) are one package, and **0 `.cc`/`.hxx` changed** — `git diff -M --raw` pairs all nine moved files at `R100`. Nothing could need to change, and the proof is the include root rather than the absence of errors: `argus_module` resolves `SOURCES` and `INCLUDES` against the declaring CMakeLists's own directory (`cmake/argus-module.cmake:22-33`), so the old `INCLUDES ../../..` under `packages/hardware/src/shared/wrapper/hardware-profile/` and the new `INCLUDES src` under `packages/runtime/` name the same `src/` root, and all 19 includers of the profile spell it package-relative. **The row's open decision is where the merged package sits**, and it goes flat — `packages/runtime`, `argus_runtime`, `argus::runtime`: rows 3 and 4 already fixed that boundary ("`lib/X` is Phase 2's rename"), §2.6 assigns the `argus_lib` helper to Phase 2, and `argus_lib` does not exist in `cmake/argus-module.cmake` today; the asymmetric risk is the tiebreak, since flat costs Phase 2 one mechanical sweep of the same ten consumers this step already swept, while `packages/lib/runtime` now would cost a new helper, a new alias scheme and a tree with exactly one nested package, all in the phase whose point is that it does not change meaning. The profile keeps two targets (the CPU variant is what keeps consumers ncnn-free) but is declared where `threading` declared its sources, which **ends the only CMakeLists nested inside a package's `src/` tree** — the profile's was depth 5, the deepest left under `packages/` is `packages/identity/tools/migrate-identity/` at depth 3, a `tools/` executable of the kind the folder architecture names beside `src/`; a strict reading of rule 25 would give the profile a module folder again, and Phase 2 step 2's layout pass is where that gets decided. 13 `CMakeLists` in 10 projects repointed: nine guarded `packages/threading` and six of those *also* guarded the profile folder, so the threading guard became the runtime guard and the profile guard was deleted rather than repointed — one folder now creates all three aliases, so `if(NOT TARGET argus::hardware-profile)` cannot be false while `argus::runtime` exists; `services/guard` links `argus::runtime` in three files with no guard of its own, exactly as it reached `argus::threading` before. The ncnn edge was checked, not assumed: `argus_hardware-profile-gpu` links ncnn only when `ARGUS_NCNN_TARGET` is set, and the subtree is added at the same position in both consumers of the GPU variant (`services/camera` after ncnn, `packages/identity` before it, as before). `AGENTS.md` rules 13b/13c, five Key Files rows and `docs/operations/hardware-tiers.md` follow the files, and `packages/hardware/` is gone from disk. Verified: `./scripts/build-all.sh dev` green on 18/18 projects, the reached-test ledger unchanged at 288 project by project — every hardware-profile guard sat beside a threading guard, so no project gained or lost a suite — the run's 23 warnings all third-party (ncnn, glslang, llama.cpp, openfst) and none first-party, no `CMakeLists` naming either old alias and no source naming either old path, and all 291 `add_subdirectory` calls resolving on disk. Also cleaned: the 51 orphaned build directories steps 2 and 3 left in 16–18 project trees (`build/*/argus-common`, `build/*/common`, 8.3 GB), removed after the gate ran so the green run still describes the committed tree. Flagged, not fixed: `packages/lib/errors/` is an untracked step-10 draft with no `src/` that calls a non-existent `argus_lib` and includes the build helper one level short, and the recommendation recorded is flat `packages/errors` for Phase 1. Full report: `docs/history/reports/f1-7-merge-runtime.md` |
| 8 | **Done** — `build: move the health stubs into the gRPC package`. The row deletes `packages/clients/health`, and the package held **one file**: a `CMakeLists.txt` with no `src/` at all, declaring a single generated module over the vendored `grpc/health/v1/health.proto` (upstream verbatim). Those stubs are **served, not called** — `services/voice` and `services/camera` implement `grpc::health::v1::Health::CallbackService` over them and nothing else in the tree names them — so §2.2's `lib/grpc` contents and §9.1's row (`grpc | 6 | lib/grpc \| + standard health stubs`, the `6` being exactly the files under `packages/grpc/src`) had already decided the destination, and the destination decided the **name**: `argus_client_module` grew an optional `GROUP` (default `client`), `packages/grpc` declares the stubs as `GROUP grpc` → `argus_grpc_health` / `argus::grpc-health`, and the alternative — `packages/grpc` declaring `argus::client-health` — was rejected because §2.5's "the namespace mirrors the folder" would then be false twice over, about the folder and about the direction of the call. The **default is what makes it safe**: all ten existing callers pass no `GROUP`, so every one of them still declares `argus_client_<domain>` / `argus::client-<domain>` and the helper's blast radius is nil; nothing else in the function moves (same codegen recipe, same ABI-bridge block, same client-base link), and only the build-tree directory name follows the group (`argus-grpc-health/generated`). **0 `.cc`/`.hxx` changed** for the second step running — the diff is three `CMakeLists.txt`, one deletion and the helper — and no include line could need one: `${gen_root}` is the module's `SYSTEM PUBLIC` include root and the header's path inside it comes from the proto's path, so `#include <grpc/health/v1/health.grpc.pb.h>` resolves wherever the guarded package happens to be added first (in `services/voice` that is `tts-client/grpc`, not `clients/voice/grpc`). `EXCLUDE_FROM_ALL` moved onto the target, because the package is now reached from the eight other places that want the runtime and only two of them serve health. The two consumers repoint a guard and a link each (voice `:65-70` and `:99`, camera `:155-160` and `:178`); **the guard never fires in the standard configure**, since the generated client each service already links adds `packages/grpc` first — it stays because it is not the dead-guard class f1-6 flagged (those named packages no source of the project reached), it names the stubs the service's own `health-rpc-service.{hxx,cc}` includes. `packages/clients/` is now the 10 live directories §9.1 records. Verified: `./scripts/build-all.sh dev` green on 18/18 projects with the reached-test ledger unchanged at 288, no first-party warning, no file anywhere naming `argus::client-health` or `clients/health` outside the comment that records what the stubs were, and every `add_subdirectory` path in the tree resolving on disk. Also cleaned after the gate: the two stale `build/dev/clients/health` binary directories (voice and camera) the deleted package's `add_subdirectory` had created. Flagged, not fixed: the stubs still link `argus_client_grpc_base` (caller-side channel/deadline/metadata machinery neither health service includes), and `packages/grpc` still declares no target for the folder itself — both are Phase 2 decisions, the first inside the `GROUP` this step introduces. Full report: `docs/history/reports/f1-8-health-into-grpc.md` |
| 9 | **Done** — `build: move the llm tool runtime into the service that runs it`. The row's four files are two things, and the count in its reason ("it has one consumer") is true of one of them. The machinery — `tool-registry`, `tool-validator`, `tool-executor`, the three D16 names — has exactly one production consumer (`services/llm`: `src/main.cc:17`, `src/controllers/llm-controller.cc:5`, `src/shared/services/llm/lfm-adapter.hxx:7`, plus the bench and the suites) and moved to `services/llm/src/shared/services/tools/`. The vocabulary, `tool-contracts.hxx`, has **two** consumers across a tier boundary — `packages/memory` declares its descriptors in `tools::ToolDescriptor` (`memory-tool-descriptors.hxx:3`) and takes a `tools::ToolCall` (`MemoryFormation::form`) — and §1.4's "no reaching into another service's `src/`" with §2.4 rule 1 forbid a package including a service's header, so it stays in the tier-3 client both sides link and follows at **Phase 4 step 7**, when memory leaves `packages/`. **That is the row's one deliberate deviation**, recorded in §9.3, in the header and in the package's `CMakeLists` and `AGENTS.md`. The move cost **zero include lines**: both trees root at `src` (`argus_module`'s `INCLUDES src`, `llm-core`'s PUBLIC `${LLM_SRC_ROOT}`), so `<shared/services/tools/tool-registry.hxx>` resolves to the moved file everywhere — 6 files paired at `R100` by `git diff -M --raw`, no consumer source touched. Two things did change: `llm-wire-test` gained the three `.cc` beside the loop it already recompiles (it builds its own copies of the service's sources and links neither `llm-core` nor the runtime); and **memory's seam was re-cut**, because the registry the service used to fill was named in its public header — `void registerTools(ToolRegistry&)` became `std::vector<tools::ToolDescriptor> toolDescriptors()`, `main.cc:172-175` registering what the stack hands over, which is what makes D16's "one consumer" exactly true instead of nearly true. The step also corrected a claim this client's `CMakeLists` and f8-b2's message both carried, that the descriptors travel with the chat contract: they do not — `ChatRequest` carries `toolsEnabled` and nothing tool-shaped (`llm-remote.cc:306-307` writes `body["tools"] = false`), the declarations the model reads are built server-side from the registry (`LfmAdapter::buildToolDeclarations`), and no `tools::` type appears on the client's wire surface. **The pipeline's only cover moved with the runtime**: `ToolExecutor` was constructed in exactly one test in the tree at `HEAD` (memory's reminder test, `:87-89`) and a package's test may not link a service's target, so resolve → validate → `hasAccess` → handler is now covered by `services/llm/tests/unit/llm-tool-runtime-test.cc` — seven cases over the descriptors memory really ships, including the **deny** path and the validator's rejections, neither of which had any coverage before, since memory's suite ran every call as `Resident`, whose `Memory` row is `kFull` (`role-access.hxx:53`). Memory's reminder test keeps every domain assertion and drives the handlers directly. Verified: `--only llm` 24/24 (23 + the new suite), `--only memory` 16/16, then the gate — `./scripts/build-all.sh dev` exit 0 on 18/18 projects, the reached-test ledger **289** (llm 23 → 24, every other project unchanged), no first-party warning in the run. The stale `tool-*.cc.o` that the deleted sources left in the consumers' build trees were removed after the gate ran, so the green run still describes the committed tree. Flagged, not fixed: row 4's forecast that this move "removes the edge by construction" is only half right — the machinery left, but `tool-contracts.hxx:5` still includes `role-access.hxx` for `RolePermission` and `llm-client/CMakeLists.txt:45` still links `argus::auth`, so the tier-3 → tier-4 edge survives, narrowed to one header and two `ToolDescriptor` fields, with the candidate fix (the gate vocabulary declared in `contracts/auth` beside the `TableName` `sync-contract` freezes, D12) belonging to Phase 2 step 4; the bench's mirrored `memory.remember` omits the `confidence` argument the real descriptor declares (`tool-calling-bench.cc:129-143` vs `memory-tool-descriptors.cc:38-42`), left alone because changing the schema would invalidate f8-b1's recorded latency baseline; `services/llm/CONTEXT.md:91-92` was already stale (f8-b4 landed the loop) and is step 11's to gather; and `docs/history/plans/tool-calling-reduction-plan.md:172-173` still names `registerTools` and `labs/*` — a dated plan whose intent, that the machinery be preserved rather than deleted, this step honours. Full report: `docs/history/reports/f1-9-tool-framework-into-llm.md` |
| 10 | **Done** — `build: split response into errors and http, and extract mdns`. The row's three destinations measured as four. `packages/errors` is the vocabulary the substrate answers with — `ErrorCode` (grown by the four codes that existed only as string literals where the refusal was raised), `ErrorDefinition`, `ResponseException` (five constructors, four delegating to the one that takes a wire record), `ValidationException` — and it is a **leaf with no `DEPENDS` at all**, which is D17: contracts and services consume it without dragging Drogon in. `packages/http` (`argus::errors argus::config Drogon::Drogon`) is the transport surface: `ApiResponse` (the only place an `{status, info, errors}` is built), `ErrorHandler` (the one advice, D20), `Cors`, `HealthController`, `ListenerConfig` and `http-errors.hxx`. `packages/mdns` is the gateway's responder, and the whole move is **two include lines** (`<drogon/drogon.h>` → `<trantor/utils/Logger.h>`; the header is byte-identical), with `mdns::mdns` `PRIVATE` so no consumer inherits the vendored `mdns.h` or its SYSTEM-include workaround; `packages/config` keeps the TOML reader and its 97-consumer include path and loses the two HTTP-facing files. §2.3 is corrected twice against the tree: `api-response` was never inside `app-config.{hxx,cc}`, and the health controller was a file in `config`'s tree rather than a member of the class — the listener config moves with it — and its include counts are low by one each (19 and 15 code files at `HEAD`, 21 and 16 after, the additions being files that had been resolving a type transitively). `AppConfig` is deleted and all nine of its members are accounted for one by one; the eleven `getNNNResponse` builders are **not** re-homed but collapsed into catalog rows that a handler throws (§4.7), the sweep having moved 188 `AppConfig::` mentions across 50 files and 92 builder call sites across 38, and only two answers are still built by hand — both in `packages/http`, because the framework asks for them with no exception to carry them: the unmatched route's 404/405, which goes through `ApiResponse::error` like everything else (`ErrorHandler::unmatchedRoute`, branching exactly as the 15 deleted lambdas did, no call site ever passing an argument), and OPTIONS, which is not an envelope at all and never went through `ApiResponse` at `HEAD` either. Row 13 is absorbed here and disclosed: `ERROR_CODE_*` cannot outlive the class, and `SYNC_LIMIT` had to reach `contracts/sync-contract` as `SyncLimits::kMaxRows` because the sweep had already rewritten its 11 call sites (a C string, not a `std::string_view`, because `operator+(std::string, std::string_view)` does not exist in C++20), with the frozen-wire citations moved to match and no number, code or table id changed. Two refusals whose catalog wording had drifted from the builder call they replace are restored to `HEAD`'s text (`InvalidMultipartForm`, `ServerAlreadyPaired`), so the frozen probe recordings still describe the surface. The step repairs what its own recovery broke — 19 duplicate includes, six files that lost includes (three restored, three proven dead at `HEAD`), four lost blank lines, and two camera suites plus identity's that aborted under SIGABRT because §4.7 turned their expectations into throws a `drogon::sync_wait`-driven test lets escape, the framework's advice running only when the framework drives the handler — and the two edges it left undeclared (`argus::http` in voice's `argus_voice-core`, tts's `tts-synthesis-http` and `tunnel-core`; `Drogon::Drogon` on `argus::mdns`; `argus::errors` on `memory-core`), the first of which was the accident D17 exists to remove: at `HEAD` those nine translation units reached the include root only through `config`'s `pkg::response` edge, deleted here. For later steps: `docs/architecture/camera-guardian-deep-analysis.md:942` still names `AppConfig`; the root `AGENTS.md` still points at the `packages/access` enums deleted in step 4 (`:37`, `:677`); three services declare `mdns/1.4.3` and never use it; `MdnsService::health()`/`isAdvertising()` ship with no caller; the four service-local `*-errors.hxx` in guard/llm/stt/vlm await a contract home; and `SyncLimits::kMaxRows` being a C string is load-bearing for eleven repositories. Full report: `docs/history/reports/f1-10-errors-http-mdns.md` |
| 11 | **Done** — `docs: refresh the stale docs that describe the old tree`. The row's three items measured as two that needed nothing and one that was wrong in both halves: `packages/common` was deleted in step 2 and left no live reference (`git grep packages/common` outside `docs/history` is empty), `packages/client` singular occurs in neither `README.md` nor `AGENTS.md`, and the key-files table was wrong about both halves of the audio row — `SampleRing` is `services/voice/src/shared/wrapper/audio/` while `packages/audio/src/shared/wrapper/audio/` holds `AudioResampler` + `EndpointDetector`, and `TtsService` is `services/tts/src/feature/synthesis/domain/` with the engine set under `infra/supertonic/`. Its real subject was four classes of stale reference in 41 files: **22 commands that cannot run** (`--only argus-<name>`, where `scripts/build-all.sh:69` matches `basename "$dir"` and the prefixed form exits 1 — proved by running it; now zero), **64 retired path spellings in 23 files — now zero**, the first sweep of this phase to close a class outright rather than to a deferred line, **four wrong counts** (`Twenty`/`20 projects` in `build-model.md:5`, `system-overview.md:44`, `build-and-test.md:6,:53`, where the build has **18** — counted two ways, `build-all.sh:9-27`'s PROJECTS and the directories carrying `CMakeLists.txt` + `conanfile.txt` + `CMakePresets.json`, which are the same 18), and **four claims whose subject does not exist** (`argus::tts-grpc-client`, a target and a directory that exist nowhere, while `services/tts/src/app/rpc/CMakeLists.txt:2` links `argus::tts-client`; `AppConfig` as what controllers answer with, where the boundary answers with `ResponseException`; `ToolValidator`, which is not a class — `tool-validator.hxx` declares `validateArguments`; and the rule that each contract header carries `<enum>ToString`/`<enum>FromString`, where every helper is lowerCamelCase (`userRoleToString`, `zoneTypeFromString`), so the PascalCase substitution the rule's own enum list invites does not compile). It discharged rows 5 and 9's handoffs to this step: `argus-deploy/CONTEXT.md`'s per-database volumes and gateway `db` keys (the compose declares two named volumes at `:894-898`, the databases are bind-mounted directories at `:199`, `:252`, `:484`, and the `db` keys belong to the owning services' configs, not the gateway's), the same bullets' f8-b4 tense (the llm block mounts `${ARGUS_DATA_DIR:-./data}/memory` at `:484` and `services/llm/src/main.cc:175-243` runs the durable `argus-llm-encounters` consumer), and `services/llm/CONTEXT.md`'s no-tool-loop bullet, stale in two directions at once. A read-only adversarial review of the changed lines returned twelve findings: **six fixed** (a deleted package's row in `services-and-packages.md`, the four counts, the `<enum>ToString` rule, `ToolValidator`, `supertonic/` placed beside `domain/` rather than under `infra/`, and the init rows still calling the data directories volumes), **four discarded with evidence** (bare `argus-<name>` is the house convention, not a stale spelling — 18 CMake projects are literally `project(argus-<name>)` and the units without one title their own docs `# argus-<name>`; `src/config/application.cc` is the legacy monolith's file, and both sentences saying so are intact), **one deferred by plan** (the twelve `## Layout` tree roots still rooted at `argus-<name>/`, which Phase 4 step 8 owns per line 872 — now enumerated in the report) and **one recorded for its owner** (the fastText classifier's liveness: `IntentGate` is constructed and loads `models/intent/intent.bin`, but no deploy mounts `models/intent`, so `tool-calling-eval-set.md` and `MODEL-CARD.md` disagree and only the intent feature's owner can say which means what). **The step narrowed its own first-pass deferral on purpose**: `build-model.md`'s project tree and `data-storage.md`'s schema paths were written off as Phase 3d step 4's, and the review's evidence disproved the reason — a deleted package and a dead schema path are today's facts, not post-gateway architecture — so both are fixed, while what the plan's carve-out actually protects (line 1061: the `Consumer` cells and the gateway-as-subscriber layout prose) is left untouched. Verified: `./scripts/build-all.sh dev` exit 0 on 18/18 projects, every suite `100% tests passed`, the reached-test ledger **302 — unchanged**, no first-party warning in the run; the sweep grep for the retired spellings answers nothing over the live tree, and all 80 path tokens this step introduced resolve (73 at the repo root, 7 against their unit). Full report: `docs/history/reports/f1-11-stale-docs.md` |
| 12 | **Done** — `docs: rewrite the rules file to the target architecture`. The row's four items measured as three that landed as written and one that could not take the shape the row implies: **§2.4's tier table did not become rule 28**, because rules 23, 25 and 27 are cited by number outside this file (10, 13 and 13 files — `cmake/argus-module.cmake:1`, `argus-deploy/AGENTS.md`, ten `packages/*/AGENTS.md`, six client headers, `packages/contracts/CONTEXT.md`), so a new number would have repointed every one of those citations at a different rule; the five tiers went inside rule 25, whose subject they are — which group may link which — as a five-row table plus the eight dependency rules (no back-edges, a contract never calls a service, `PUBLIC` for header types and `PRIVATE` for implementation-only deps, enums live where they are used (D12), `lib/http` is tier 2 with the reason three files moved for it, `scripts/check-deps.sh` in Phase 2 step 5). **The row's closing sentence decides the hard part** — the rules file is what a future change is written against, which is why it lands with the layout rather than after it — so a rule may state the target while the tree is mid-migration, but only if no reader can mistake a target for a fact: every rewritten rule ends with a bolded, measured **"Today, against that target"** paragraph naming the phase that closes the gap. Rule 23 gives the target tree (`app/` = composition only, `src/config/` per D20, `feature/<feature>/{controllers,dtos,repositories,schemas,services,infra}`, `shared/` under the 2+ rule) and then the tree as it is: `services/tts` the only one with `src/app/`, `notification`/`productivity`/`guard` already carrying the `{controllers,services,dtos}` interior one level deeper under `feature/api/<resource>/` (`notification` also `feature/rpc/`, which Phase 4 step 2 moves to `app/rpc/`), the other nine services with a single `main.cc` at their `src/` root and `tunnel`'s two entry points beside them, five services with no `feature/` at all (`gateway`, `llm`, `stt`, `tunnel`, `vlm`), four of the six that have one still keeping code beside it, no service yet with `src/config/` (the typed configs the phase moves live under `camera`/`notification`/`productivity`'s domain folders, four files), no service with `tests/e2e/` (the only first-party one is `packages/sync/tests/e2e/`), `tunnel` exempt by D19 and `gateway` gone in Phase 3d. Rule 25 gained the three package groups with their target names — and the pre-migration spelling of each, including the four clients whose alias is `argus::<name>-client` because they are declared with `argus_module` — plus the tier table; rule 24 states the settled shape, the package interior (`src/<name>/`) and the one rename (`src/server/` → `src/app/`); rule 26 corrects its own subject; rule 27 spells the client both ways. **Two rules outside the row's 23–27 moved with them**, because a self-contradicting rules file is worse than a small scope extension: rule 3's "a repository lives in `src/shared/repositories/`" became the home rule (its feature, unless 2+ features of that service read it) with the shape block otherwise unchanged, and rule 10's DTO path was given twice as the retired `feature/api/{resource}/dtos/`. The **Key Files table** was regrouped under the taxonomy — 48 rows → 51, in seven blocks — with the repositories row carrying the 2+ rule, a new row for the ten SDK clients, and every row pointing at a unit scheduled to die or leave `packages/` naming its destination in its own cell (`memory` → a feature of `services/llm` in Phase 4 step 7, `identity` → `services/identity` in 3c, `sync` → Phase 3a, `socket`/`room` dying in 3a, the three `packages/audit` rows owned by `services/sync`). **Step 11's handoff is discharged**: `services-and-packages.md` listed `argus-contracts` as a package that "owns a Conan/CMake graph and builds on its own", where `packages/contracts/` has no `CMakeLists.txt` at all — the standalone table is now the seven folders that really carry the graph (`cert`, `identity`, `intent`, `memory`, `socket`, `sqlite`, `sync`, measured as the folders holding `CMakeLists.txt` + `conanfile.txt` + `CMakePresets.json` and declaring `project(argus-<name>)`) and the direct-import list names all 35 (15 `packages/*`, ten contracts, ten clients). **Measurement corrected four plan statements and one rule of its own**: §2.6's header-only claim holds for one of `validation`/`text`/`phrase` (`text` and `phrase` compile 3 `.cc` each; only `validation` has none, declared `STATIC` over three headers with the `LINKER_LANGUAGE CXX` workaround at `packages/validation/CMakeLists.txt:20`); "the ten contracts are `INTERFACE`" is true of nine — the tenth, `response-contract`, defines `argus_client_response-wire` and compiles `response-rpc.cc` — and `cert` is a `STATIC` `argus_module` with one `.cc`, not header-only; Phase 4 never names `services/voice` although §9.2 gives it `app/` and gRPC, so as written the phase leaves it outside the shape the rules state; and §9.2/§2.3's "empty `src/shared/`" in `tts` does not exist (`services/tts/src/` holds `app/` and `feature/` only), making Phase 4 step 1's "empty `src/shared/services/` goes" a no-op. Rule 26's first draft also claimed the gateway's `database/schema.sql` *was* the identity schema; the md5s (`401aceb2…` vs `c21e7d77…`) and the file's own header ("Gateway schema (gateway.db) … must never gain their tables") disproved it, and the gateway mounts both. A self-review re-measured every "today" claim while the gate ran and caught four, all the same way — a folder listing generalised into a rule sentence (tunnel's two entry points; `llm`/`stt`/`vlm` having no `feature/` at all rather than code "also outside" one; the INTERFACE count; the client-folder suffix). A read-only adversarial review then returned **fourteen findings, all fourteen real and all fourteen fixed** — five were symbols attributed to the wrong owner (`PrivatePortraitService` is `packages/identity`'s, `json_util::toString` is not `jsonToString`, `RuleParser`/`PhraseCatalog` are `packages/phrase`'s, the CHECK-mirroring enums are not all in four contracts, the tier-3 path template holds for six of ten clients) and the rest were present-tense claims that only hold after Phase 2 (`nothing else about these rows moves`, which Phase 2 step 2 contradicts; the helper "derives" a `DEPENDS` name, where `cmake/argus-module.cmake:35-36` passes it verbatim and three `CMakeLists.txt` spell `argus::` by hand; rule 26's "every config points at `database/schema.sql`", false for the gateway, which mounts `/opt/argus/gateway/schema.sql` and says `schema = "gateway/schema.sql"`); rule 24's `details/` and `infra/` examples contradicted rule 23 and §2.3 and were replaced, and one review side note was declined with the plan's own open flag as evidence (`argus-grpc` in the direct-import list — plan row 8: "`packages/grpc` still declares no target for the folder itself — both are Phase 2 decisions"). `Verified:` `./scripts/build-all.sh dev` exit 0 on 18/18 projects, every suite `100% tests passed`, the reached-test ledger **302 — unchanged** (a doc step that moved it would have touched source), 0 first-party warnings in the run (all 23 warning lines are third-party `CMAKE_CXX_STANDARD` notices and a missing ccache); 51 Key Files rows under 7 subheads with every path token resolving to the tree or explicitly marked as a target, 0 table blocks with mismatched pipe counts, every `rule <n>` reference resolving to a heading, no added prose line over 78 columns, and the 10/13/13 external rule citations re-counted unchanged. Report: `docs/history/reports/f1-12-agents-rules.md` |
| 13 | **Done** — no change needed; absorbed by step 10. The row's two subjects were both in `packages/response/src/config/app-config.{hxx,cc}`, the file `31fd41c` (step 10) deletes whole: at its parent the header declares `SYNC_LIMIT{"200"}` and **eleven** `ERROR_CODE_*` string constants beside the codes the boundary actually threw. Measured after: `git grep ERROR_CODE_` over the live tree returns **one** file — `packages/contracts/proto/argus/common/v1/base.proto`, the frozen gRPC wire enum, a vocabulary with its own home in `contracts/` whose comment says it mirrors `packages/errors` — while the string literals that remain outside `packages/errors` are test assertions pinning the wire value (`services/tts/tests/unit/tts-rpc-test.cc:198`, `services/camera/tests/unit/camera-talk-cutover-test.cc:208`, `services/gateway/tests/gateway-test.cc:990`), which is the contract under test and not a declaration that can drift; `ErrorCode` gained the four codes that had lived only as literals where the refusal was raised. `SYNC_LIMIT` moved with the same commit: it is `SyncLimits::kMaxRows{"200"}` in `packages/contracts/sync-contract/src/shared/contracts/sync-limits.hxx` (created by `31fd41c`, +12 lines), whose own comment gives the reason — a wire invariant "declared once, here, so the SQL constants in the sync and audit repositories concatenate the same digits"; the row spells that unit `contracts/sync`, which is what Phase 2's rename makes of it, so the placement is done and the rename is Phase 2's. The citation clause holds: `wire-sync-tables.md` cites `sync-limits.hxx` (:29-31) and `table-name.hxx` (:19-20) by their current paths, and the two paths the row names — `backend/src/config/app-config.hxx` and `backend/src/shared/enums.hxx` — occur nowhere in the live tree (empty `git grep` outside `docs/history`); the values the file freezes are untouched (`TableName` 0-23, `SYNC_LIMIT = 200`). The row is closed as already-done rather than merged into step 10's, because the two describe the same commit from different directions — step 10 from the HTTP config it deleted, step 13 from the codes it deduplicated — and a merge would lose whichever half a reader arrives from. `Verified:` `./scripts/build-all.sh dev` exit 0 on 18/18 projects, every suite `100% tests passed`, the reached-test ledger 302 — unchanged from steps 11 and 12 — and 0 first-party warnings; the four greps above re-run over the live tree. Report: `docs/history/reports/f1-13-error-vocabulary.md` |

### Phase 2 — package tree and build

| Step | Action |
|---|---|
| 1 | **Done** — `build: move the packages into lib, contracts and clients`. The row measured as three move kinds and the naming rule they exist for: **15** packages left the flat tree for `packages/lib/` (`packages/errors` → `packages/lib/errors`), **10** contract folders dropped the `-contract` suffix that only existed because the folder was flat (`packages/contracts/sync-contract` → `packages/contracts/sync`), and **4** clients did the same for `-client` (`packages/clients/llm-client` → `packages/clients/llm`; `stt`, `tts`, `vlm` likewise) — 29 folders and 209 files in one sweep, D2's "the folder never repeats the group" now true by construction, since `packages/lib/`, `packages/contracts/` and `packages/clients/` hold no `lib`, `contracts` or `clients` subdirectory. The six clients that already read correctly (`camera`, `camera-actions`, `identity`, `notification`, `productivity`, `voice`) are not in the move map at all; registering them as identity moves is what crashed the sweep's first run (`fatal: can not move directory into itself, source=packages/clients/camera`). Two rewrites followed the move, and conflating them is what damaged vendored code: **489** `${CMAKE_CURRENT_SOURCE_DIR}/…`, `${CMAKE_CURRENT_LIST_DIR}/…` and `${CMAKE_CURRENT_FUNCTION_LIST_DIR}/…` references in the tree's **67** CMake files were resolved to repo paths, mapped through the move and recomputed *relative to the file that names them* (**70** files repointed — a folder that changed depth breaks its own escape paths even when its target did not move), and textual `packages/<old>` tokens were rewritten with lookarounds so `backend/packages/errors` (a real Dockerfile spelling) is repointed while a longer folder name that merely starts the same way is refused; the second rewrite reached into `third_party/` and touched **six vendored files across three submodules** before it was caught — the `"${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ncnn_generate_…"` script paths ncnn hands to `${CMAKE_COMMAND} -P` became paths relative to the file being rewritten, which is right at the definition site and wrong at every call site — and all six were restored with a scoped `git -C <submodule> restore -- <paths>`, all four submodules verifying clean. A third stale class the move could not have caught, because it carries no `packages/` prefix: the *second argument* of `add_subdirectory`, where **49** sites in **20** `CMakeLists.txt` spelled a retired label (`${CMAKE_BINARY_DIR}/tts-client` in `services/voice`, `${CMAKE_CURRENT_BINARY_DIR}/auth-contract` in `packages/identity`, …); all 49 were normalized to one canonical spelling per domain (`clients/<domain>`, `contracts/<domain>`), which makes a collision impossible by construction — verified: 0 stale labels remain, and the tree's 47 explicit labels resolve to **0** pairs of distinct source directories. The docs pass ran as four classes (folder paths that moved; retired helper names such as `argus_sdk_module`; alias claims such as `argus::threading` for what is now `argus::lib::runtime`; rules describing the old shape, including `AGENTS.md` rule 25's example naming a real call with real paths), with `build-model.md`'s 11-line project grid rebuilt programmatically because the longer paths broke its column widths — and the claim-by-claim review found **25** more sites in the same four classes (six `lib/*/AGENTS.md` and three client `AGENTS.md` still teaching `argus_module` for packages that now call `argus_lib`/`argus_clients` — the helper *was* `argus_module` at HEAD, so this step is what made those lines wrong — the four `-client` titles, the `README.md` layout block, three sentences in `services-and-packages.md`, the tree block in `contracts-overview.md`, and four `sdk/…` citations), all fixed. The review of the helper's keyword contract left one latent trap fixed in place: `cmake_parse_arguments` does not report an argument the helper does not declare, and in the realistic shape it fails *silently* (`NAME n DEPENDS d SYSTEM_DEPEND Foo` parses to `ARG_DEPENDS='d;SYSTEM_DEPEND;Foo'` with an empty `ARG_UNPARSED_ARGUMENTS` on CMake 3.31, so the misspelling travels into `target_link_libraries` and surfaces at link time), so `argus_reject_unknown_args` now runs in all seven keyword helpers and rejects both a non-empty unparsed list and any keyword-shaped token inside a value list — probe-verified on four typo shapes, with its limit stated in the helper rather than hidden (a lowercase misspelling is indistinguishable from a library name). `Verified:` `./scripts/build-all.sh dev` exit 0 on 18/18 projects, every suite `100% tests passed, 0 tests failed`, the reached-test ledger 302 — unchanged from steps 11, 12 and 13 — and the log's 24 warning lines are 23 third-party plus one `CMAKE_TOOLCHAIN_FILE` reconfigure artifact on `packages/lib/sqlite`, proved not to be a tree defect (emptied and rebuilt: exit 0, 0 warnings, 1/1 test; and the conan toolchain's own `Using Conan toolchain:` line appears once per project, 18/18, including the nine whose cache holds that entry as `:UNINITIALIZED` rather than `:FILEPATH`); the sweeps re-run over the live tree — no retired spelling in any of the 1474 live files, 118 `packages/…` tokens and all 489 CMake paths resolved, 0 label collisions, and 0 of 499 target commands naming an `argus::` alias as the target being modified — and `./scripts/build-all-test.sh` passes. Flagged rather than fixed, all recorded in the report: `packages/lib/grpc` declares no folder target (its content is two helper-defined clients plus the health stubs); `packages/sync` and `packages/memory` are not helper-declared at all (hand-written `argus_sync`, `memory-core`, `memory-catalog`, named raw across 33 consumer lines — converting them renames targets with consumers, a naming decision rather than a move); `packages/lib/runtime` declares the `hardware-profile` pair through the ungrouped helper; `argus_clients` merges `SYSTEM_DEPENDS` into `DEPENDS`; `ci.yml:40`'s `packages/*/config.toml.example` glob is written against the flat tree; five gitignored local `config.toml` files (untracked, so no commit can fix them) name schema paths from folders that have never existed; §2.3/§2.6's header-only claim holds for `validation` only; and the review's pre-existing docs rot — stale counts and lists, paths from an older tree, layout blocks naming `src/server/` in seven services and `src/controllers/` in four, claims a later phase falsified, citations to files that do not exist — is listed with file:line for a docs row to sweep in one pass. `.superpowers/sdd/` holds five 357-byte tombstones dated 2026-09-21 where working notes used to be, lost to a scripted `git show HEAD:<path> > <path>` restore over a path `.superpowers/sdd/.gitignore` excludes; no code, commit or plan step depends on them, and the surviving records are named in the report. Report: `docs/history/reports/f2-1-folder-moves.md` |
| 2 | **2a–2c done, 2d open** — `build: apply the section 2.3 layout to the ten client packages`. The row covers forty-six packages, so it runs as four sub-steps — 2a the fifteen `packages/lib/`, 2b the ten contracts, 2c the ten clients, 2d the eleven services — and this closes the third (2a's own record is `docs/history/reports/f2-2-layout-libs.md`, 2b's is `docs/history/reports/f2-2-layout-contracts.md`). Six of the ten clients were already in the shape §2.3 draws (`camera`, `camera-actions`, `identity`, `notification`, `productivity`, `voice`) and four were flattened: **thirteen files moved with `git mv`** — `llm`'s four (`llm-service.hxx`, `tool-contracts.hxx` and `remote/llm-remote.{hxx,cc}`), `stt`'s two, `tts`'s five (including the two at the package root) and `vlm`'s two — every basename preserved, so what changed for a consumer is the include spelling and nothing else; `clients/tts` also stops exporting the package root, whose `INCLUDES .` is what made `<tts-client.hxx>` legal, and only `clients/llm` ends up with a `details/` folder, because it is the only one of the four whose transport is a separate translation unit. The sweep that follows is the smallest of the four sub-steps and the one that had to be surgical rather than a substitution: **46 include lines in 34 code files**, eight spellings -- 4 lines in 4 files inside the four packages and 42 lines in 30 files outside, split by tree as 9 files in `services/llm`, 8 in `packages/memory`, 4 each in `services/guard` and `services/tts`, 2 each in `services/voice` and `services/camera`, 1 in `services/stt`, and every replacement keyed on a basename that exists in exactly one package because `services/llm` keeps its own `src/shared/services/llm/` and `services/vlm` its own `src/shared/services/vision/` — the two trees were merged under one spelling, so `<shared/services/llm/llm-service.hxx>` resolved into the client while the `lfm-adapter.hxx` beside it resolved into the service. The docs were a measured pass of their own: nine `AGENTS.md` lines at `HEAD` carried the eight spellings (4 in `clients/llm`, 3 in `clients/tts`, 1 in `clients/stt`, 1 in `services/tts`) and none of the nine survives in the tree. `camera` was the only client with a suite of its own, and **ten were added** — the eleven client targets now carry **32 cases in 1782 lines**, the ten registered in `camera`'s own shape including the `EXCLUDE_FROM_ALL FALSE` opt-back-in, two of them adding `tests/support` to the include path for the fake server they drive. How many test instances ten suites add is the finding of the sub-step: a client is not a gate project, so its suite is collected by the ctest of every project whose configure reaches it, and the static model of literal `add_subdirectory(` paths predicted **28** where `ctest -N` measures **39**. The two mechanisms no literal path can express are in the tree: `packages/lib/auth/CMakeLists.txt:42-45` pulls `clients/identity` into every tree that configures `lib/auth` whenever the target does not already exist, which is why **13** of the 18 trees carry `build/dev/clients/identity` and `sqlite`, `intent`, `stt`, `vlm` and `tunnel` do not, and `services/camera/CMakeLists.txt:369-380` adds three clients through `foreach(CLIENT IN ITEMS llm vlm notification)` into bare binary directories, so no `clients/llm` string exists for a grep to find. `identity`'s suite is `identity-grpc-client-test` because `packages/lib/sqlite` already registers an `identity-client-test`. The ten clients gained the `AGENTS.md` §2.3 asks for — **six gain the file and four are rewritten** (`llm` +80/−32, `stt` +73/−14, `tts` +116/−18, `vlm` +99/−22) — all ten carrying the H1 and the four H2s 2b fixed, and the **seventeen** consumer counts they state were re-derived one by one and all seventeen hold. One document was found false rather than left standing: `packages/clients/vlm/CONTEXT.md` asserted that nullopt covers transport failures, and a transport failure is not one of them — it raises `drogon::HttpException` out of `sync_wait`, which the suite pins on both sides. §2.3 still does not hold for four spellings of its own rule, each recorded in the package's `AGENTS.md`: a protoc type crosses three of the ten surfaces (`identity`'s answer types, `camera`'s `std::optional<argus::camera::v1::PullTableResponse>`, `camera-actions`' `using CameraCommandOutcome = argus::camera::v1::CommandOutcome;`), nine of the ten have no `details/` folder at all, `camera-actions` compiles `src/camera/` because it speaks the camera domain beside `clients/camera`, and `tts-wire.hxx` stays in the client although §2.3's own logic would move it to `packages/contracts/tts/`. The gate's first run after the ten suites were registered stopped in its second project: `socket`'s `identity-grpc-client-test` **segfaulted five runs of five**, in unbounded mutual recursion between `grpc_call_run_cq_cb` (`packages/lib/grpc/src/grpc/grpc-cq-bridge-entry.cc:7-13`) and `argus::bridgeCq` (`grpc-cq-bridge-exit.cc:9-12`). The entry defines `grpc_call_run_cq_cb` over an abseil `AnyInvocable` and the vendored `libgrpc++.so` carries that symbol undefined, so in a tree with **one** abseil flavor the entry's definition interposes the real implementation and the exit's call back into it lands on the entry; `packages/contracts/CONTEXT.md:45-50` had already written the hazard down and the gate trees did the opposite. The fix is two sites in `cmake/argus-module.cmake` and nothing else — `:347` records `ARGUS_SECOND_ABSEIL_FLAVOR` exactly where the Conan abseil export's includes come into play, `:172-175` returns before creating anything when that property is absent, and no caller needs editing because the existing guards skip the append by themselves. Measured after: the socket binary binds `U` like the passing `camera` one, five runs of five with 21 assertions each, the `llm` tree re-measured as the regression control is byte-for-byte what it was, and **6 of the 39** `libargus_clients_*.a` archives in the dev trees now carry the two bridge members — all six inside the four trees that carry the export (`llm`, `voice`, `memory`, `tts`), where before, the seven that had them were an accident of configure order. It is flagged as a scope question rather than presented as settled: the change is in shared build machinery and was made inside this unit. Three findings came out of the suites' first real run and all three are fixed here. `voice`'s client **dropped every frame the caller had already written** — `finish()` latched `writesDone_` and called `StartWritesDone()` in the same breath while the drain path refuses to start a write once that latch is set, so three of the four frames never left and the half-close went out behind them; the latch was in the wrong place, because queued frames are part of what the caller asked to send and only new ones are what `finish()` stops, and the close now runs from a `maybeCloseLocked()` when the queue empties with nothing in flight. `camera`'s **deadline bound was wrong and the deadline was not** — `<= 5000` against a measured 5009, because a gRPC deadline travels as a relative timeout and the server rebuilds the absolute one from its own clock; the bound is 6000 with the measurement in the comment beside it and `CHECK_MESSAGE` on both checks. And `camera-actions`' check **pinned an ack the wire never carries**: `accepted = true` beside `detail = "in_flight"`, which `finishAck` (`services/camera/src/feature/actions/camera-action-rpc-service.cc:71-80`) derives from an outcome every verdict sets explicitly and so can never produce — the client was right and the check was wrong, the same species as the false constraint 2b removed from the seven catalogs, and the second time in this phase that reading the producer settled which side was wrong. Six things are left open for a decision and none has an answer yet: §2.3's per-contract `proto/` root, whether `tts-wire.hxx` moves to `packages/contracts/tts/`, whether `llm`'s `details/` access should be closed with a surface factory, whether `packages/clients/voice`'s provably-unused `argus::contracts::auth` dependency and `<auth/user-role.hxx>` include should be deleted, the protoc types crossing the three surfaces, and the bridge fix's own scope. Verified: `./scripts/build-all.sh dev` exit 0 on 18/18 projects, every suite `100% tests passed, 0 tests failed`, no `Not Run`, no `***Failed` and no `warning:` line in the run, the ledger **428** — 389 plus exactly the **39** instances the ten new suites add, with the **386** instances that are not client suites unchanged — the three repaired suites and the bridge-broken one each run five times green, and every figure in this row re-derived from that run's log or from `git` on the same tree. Report: `docs/history/reports/f2-2-layout-clients.md` |
| 3 | One root `conanfile.txt`; delete per-package `conanfile.txt` and presets |
| 4 | Verify the DAG of §2.4 and make the violated edges compile the other way |
| 5 | Add `.clang-tidy` (rules 16/19) and `scripts/check-deps.sh` (forbidden edges); wire both into `build-all.sh` (§4.13) |
| 6 | `./scripts/build-all.sh dev` → 0 errors, 0 warnings; package tests run standalone |

### Phase 3 — auth, identity and sync as services

The order is not arbitrary: `sync` is what every other service already depends on for transport,
so it lands first, and the gateway loses its last responsibility only once its replacement is
proven. Each sub-phase is independently revertible and ships with its own tests.

#### 3a — the transport: `sync`

| Step | Action |
|---|---|
| 1 | Extract `services/sync` from the gateway: `/sync`, rooms, fan-out, audit persistence, the action journal (`user_action_log`, §3.5), control RPC. Create `contracts/sync` — including the payload vocabulary that leaves `packages/socket` (§3.6) and the audit/action vocabulary — and `clients/sync`; delete `packages/sync`, `packages/socket`, `packages/room`, `packages/audit` |
| 2 | Producers (`camera`, `guard`, `notification`, `productivity`, `identity`) move to `contracts/sync` + `lib/nats` + their own durable outbox with fingerprints; `sync` becomes the single audit writer. `identity` also moves its six `user_action_log` writes onto the additive action subject (§3.5), declared as a **new row** in `wire-nats-subjects.md` — existing rows and payloads untouched |
| 3 | Apply the 90-day TTL and its compaction (D15); `contracts/sync` declares the resync semantics |

#### 3b — `auth`

| Step | Action |
|---|---|
| 1 | Extract `services/auth`: sessions, credentials, device credential, LAN-only pairing/registration, rate limiting, credential-validation RPC; create `clients/auth` |
| 2 | Purge `lib/auth` of database access; `JwtFilter` validates through `clients/auth`; add the short-TTL context cache invalidated by the identity change event |

#### 3c — `identity`

| Step | Action |
|---|---|
| 1 | Extract `services/identity`: `user`, `person`, `face_embedding`, invitations, portraits, identification RPC. `packages/identity` is consumed here and dies |
| 2 | Split `identity.db`: sessions and credentials → `auth`, people and faces and invitations and portraits → `identity`, the audit tables (`audit_log`, `user_audit_log`, `user_action_log`) → `sync.db`, with row-count and checksum verification and a documented rollback |

#### 3d — the edge comes down

| Step | Action |
|---|---|
| 1 | Delete `services/gateway` and the proxy; every service terminates TLS with the single instance certificate (D11), announces host+port over mDNS and registers the filter chain |
| 2 | Retire the gateway relays: the client talks directly to `voice` and `camera` |
| 3 | Reassign the edge error vocabulary (`contracts/gateway` dies, D14) and remove the orphan contract |
| 4 | Rewrite the subscriber and publisher columns in `docs/architecture/wire-nats-subjects.md` and the layout prose of `system-overview.md`, `services-and-packages.md`, `events-and-contracts.md`, `build-model.md`, `data-storage.md`, `sync-engine.md`, `wire-camera-media.md`, `wire-device-identity.md` — payloads untouched, the gateway is simply no longer the consumer (§9.3) |
| 5 | Coordination: the frontend moves to N discovered endpoints under the one domain, with the route table from `contracts` |

### Phase 4 — service layouts

| Step | Action |
|---|---|
| 1 | Flatten `services/tts`'s feature shape to the reference: `feature/synthesis/api/http/{controller,dto}` → `{controllers,dtos}`, `domain/` → `services/`, `infra/supertonic/` stays, empty `src/shared/services/` goes |
| 2 | Migrate `notification` (`feature/rpc/` → `app/rpc/`, `feature/api/<resource>/` → `feature/<resource>/`) |
| 3 | Migrate `camera` (domain code in `src/operator`, `src/objects`, `src/monitor` → features; residual `src/controllers/` → features; gRPC out of `main.cc` into `app/rpc/`) |
| 4 | Migrate `productivity` and `guard` to the same shape |
| 5 | Migrate the legacy horizontal services `stt`, `vlm`, `llm` (`src/controllers/` + `src/<domain>/` → features, gRPC into `app/rpc/`) |
| 6 | Finish the internal gRPC legs for `stt`, `vlm` and `llm` following the `argus.tts.v1` precedent (D16), leaving their app-facing HTTP routes untouched |
| 7 | `memory` and `intent` leave `packages/` and become features of `services/llm`; `packages/identity` left earlier, in Phase 3c |
| 8 | Update every `AGENTS.md`/`CONTEXT.md` layout block (several are stale today) |
| 9 | Move every service's config resolution into `src/config/`, leaving `app/` with `main.cc` and `rpc/` only, and register the shared advice with one line in `main.cc` (D20) |

### Phase 5 — verification

| Step | Action |
|---|---|
| 1 | Per service: 0 errors, 0 warnings, its unit tests green, `--test-case`/`--order-by` isolation checked |
| 2 | Frozen `/sync` golden frames replay byte-identical |
| 3 | Golden HTTP contracts per route: exact envelope, 404 vs 502 `CAMERA_UNREACHABLE`, multipart intact, filter matrix identical route by route |
| 4 | Durability drills: kill `sync` during a write and confirm the audit diff arrives after restart (outbox + PubAck) |
| 5 | Isolation drill: killing an AI service does not take down `auth`, `sync` or `camera` |
| 6 | Real client against the mDNS-discovered endpoints: login, bootstrap, media, voice |


---

## 6. Risks

| Risk | Mitigation |
|---|---|
| Frontend must move from one endpoint to N discovered endpoints | Declare it in `contracts`; the app keeps painting persisted data first; ship the client change with the service split |
| mDNS discovery is a runtime dependency for the client, and it cannot carry paths | Discovery supplies host+port per route; a manual server entry stays available as fallback; the route table is a contract, not a guess |
| `auth → identity` adds a hop to every authenticated request | Short-TTL context cache in `auth`, invalidated by the identity change event |
| Splitting `identity.db` risks inconsistent state during cutover | Enrolment stays atomic in `identity`; cutover with row-count and checksum verification and a documented rollback (Phase 3c step 2) |
| Rate limiting and LAN-only enforcement were centralised in the gateway | Both move to `auth`, before credential validation (Phase 3b) |
| `lib/auth` in every service widens the blast radius of an auth bug | One implementation, one test suite, one contract; no service writes credentials |
| Service layouts are migrated in pieces, so the tree is half-old for a while | `tts` is the reference; each service migrates in one change with its own docs, and the layout rule is enforced by review against §2.3 until `check-deps.sh` and `.clang-tidy` exist |
| A single instance certificate is one key to protect | The same trust as today's gateway certificate, but rotation becomes one operation instead of N; the host is the trust boundary, as it already is |
| The gateway is deleted before its replacements are proven | Phase 3a–3c land first and each is revertible; the gateway only dies in 3d, and the mDNS path is verified against a real client before that |
| A producer that loses its outbox write loses a change forever | Nothing in Phase 3a step 2 is optional: the outbox row and the NATS publish travel in one transaction, and Phase 5 step 4 drills it |

---

## 7. Open decisions

**None.** The audit-retention question is decided (D15, 90-day TTL) and the single-443
dispatcher question is closed by D19: the tunnel is deferred, so no dispatcher is planned and
path routing lives in the client with ports from discovery (§3.3). When the remote-access work
starts, that decision reopens with the tunnel as its natural home.

---

## 8. Glossary

| Term | Meaning |
|---|---|
| **Package** | `packages/<group>/<name>/` — reusable infrastructure: no data, no domain, no surface |
| **Microservice** | `services/<name>/` — executable owning domain logic and its database |
| **Contract** | `packages/contracts/<domain>/` — `.proto` plus C++ contract types and errors |
| **Client** | `packages/clients/<domain>/` — the thin SDK used to call that service |
| **Feature** | `services/<name>/src/feature/<feature>/` — a complete vertical slice |
| **RPC** | `services/<name>/src/app/rpc/` — gRPC listeners, infrastructure |
| **Shared** | `services/<name>/src/shared/` — code used by 2+ features of that service |
| **Config** | `services/<name>/src/config/` — the service's typed configuration, resolved once (D20) |
| **Envelope** | The single `{status, info, errors}` response shape every HTTP route returns |
| **Advice** | The one registered exception handler (`ErrorHandler::handleException` in `lib/http`) that turns a thrown exception into the envelope (§4.7) |
| **Formatter** | `ApiResponse` in `lib/http` — the only code that builds an envelope |
| **Tier** | A layer of §2.4; its permitted edges are enforced, and a forbidden one fails the build |
| **Wire** | Everything a client or another service observes: paths, envelopes, error codes, subjects, protos (§1.8) |
| **Fat slice** | A feature holding its own controllers, services and infrastructure |
| **Sync** | `services/sync` — the realtime and audit service: `/sync`, rooms, fan-out, audit tables |

---

## 9. Verified inventory

Measured on the working tree on 2026-09-19 (`.cc`/`.hxx` counts). This is the baseline the
migration is checked against: every row is either mapped to a destination or listed as a
death.

### 9.1 Packages → destination

| Today | Files | Destination | Note |
|---|---|---|---|
| `audio` | 5 | `lib/audio` | stays a package (D9) |
| `auth` | 16 | `lib/auth` | loses direct DB access (D6) |
| `cert` | 11 | `lib/cert` | one instance certificate (D11) |
| `config` | 7 | `lib/config` + `lib/http` | the TOML reader stays (97 consumers, tier 1); the health controller and the listener config are HTTP-facing and move to `lib/http` (tier 2). A service's own typed config is `services/<name>/src/config/` (D20) |
| `nats` | 6 | `lib/nats` | subjects frozen |
| `phrase` | 10 | `lib/phrase` | |
| `sqlite` | 12 | `lib/sqlite` | stays a package (it enables the connection, it does not query a domain) |
| `storage` | 4 | `lib/storage` | + `StoredFileCategory` |
| `validation` | 4 | `lib/validation` | header-only |
| `text` | 2 | `lib/text` | absorbs `json` + `hash` |
| `json` | 3 | → `lib/text` | |
| `hash` | 5 | → `lib/text` | sha256, base64, fnv |
| `threading` | 6 | `lib/runtime` | BlockingTask, CancellationToken, ThreadBudget, ai-init |
| `hardware` | 2 | → `lib/runtime` | HardwareProfile |
| `response` | 9 | `lib/errors` + `lib/http` | D17. 5 files → `errors` (`error-code`, `error-definition`, `response-exception.{hxx,cc}`, `validation-exception`); 4 → `http` (`api-response.{hxx,cc}`, and `config/app-config.{hxx,cc}` split into `error-handler`, `cors` and the health controller — the `AppConfig` class and the `getNNNResponse` family are leftovers, and Phase 1 step 10 removes them, §2.3, §4.7) |
| `common` | 4 | — | dies: 3 forwarding shims + 1 test. `wrapper/api-response/api-response.hxx` already reads `#include <http/api-response.hxx>` and `exceptions/response-exception.hxx` reads `#include <response-exception.hxx>`, so the work is repointing its 10 consumers (all in `identity` and `sync`) and deleting the folder |
| `grpc` | 6 | `lib/grpc` | + standard health stubs |
| — | — | `lib/mdns` | extracted from `services/gateway/src/shared/services/mdns/mdns-service.{hxx,cc}`, the only implementation today |
| `clients/*` | 36 | `clients/*` | 10 live dirs; `health` dies; `auth` + `sync` are new |
| `contracts/*` | 10 | `contracts/*` | dirs lose the `-contract` suffix; `gateway` dies (D14) |
| `access` | 4 | — | dies: vocabulary distributed (D12) |
| `audit` | 25 | — | moves: `services/sync` owns its three tables (`audit_log`, `user_audit_log`, `user_action_log`); compaction and the audit vocabulary (`AuditLogPriority`, `UserAction`) go to `sync` and `contracts/sync` |
| `socket` | 8 | — | dies: three jobs split between `contracts/sync` (payload vocabulary), `services/sync` (transport, fan-out) and `lib/nats` + each producer's own outbox (§3.6) |
| `room` | 2 | — | dies with `socket` in Phase 3a. This row's "its only consumer is `socket`" was measured in Phase 1 step 6 and is wrong: `room-manager.hxx` is also included by `packages/sync` (`src/feature/socket/sync/services/sync-service.hxx:15`), `services/gateway` (`src/main.cc:19`, `src/sync/sync-fan-out.{hxx,cc}`, `tests/*` ×2) and `services/camera` (`src/main.cc:34`), so the phase owes those four the same repointing it owes `socket`'s consumers |
| `sync` | 89 | — | dies: `services/sync` (79 files, 12 cross-domain repositories) |
| `identity` | 131 | — | leaves packages: `services/identity` + `services/auth` |
| `memory` | 45 | — | leaves packages: a feature of `services/llm` |
| `intent` | 7 | — | leaves packages: a feature of `services/llm` |
| `gateway` | 2 | — | not a package: misrouted rename target |

### 9.2 Services → destination

| Service | Files | Today's `src/` | Layout work |
|---|---|---|---|
| `tts` | 39 | `app` + `feature` + `shared` (empty) | **reference for `app/` + `feature/`**, but its feature shape is still flattened in Phase 4 step 1 |
| `guard` | 68 | `feature` + `main.cc` | add `app/`, move gRPC |
| `voice` | 51 | `feature` + `shared` + `test-support` | `app/`, gRPC |
| `notification` | 58 | `feature` + `notification` + `shared` | `feature/api/<r>/` → `feature/<r>/` |
| `productivity` | 64 | `feature` + `productivity` | same, plus `app/` |
| `camera` | 158 | `camera` + `controllers` + `feature` + `monitor` + `objects` + `operator` | the largest: domain dirs → features |
| `llm` | 21 | `controllers` + `llm` + `shared` | legacy horizontal → features |
| `stt` | 632 | `controllers` + `shared` | legacy horizontal (mostly vendored model code) |
| `vlm` | 15 | `controllers` + `vlm` + `shared` | legacy horizontal |
| `tunnel` | 30 | `client` + `core` + `net` + `protocol` + `relay` + two mains | **deferred** (D19): its own shape is accepted, no phase touches it |
| `gateway` | 62 | `identity` + `proxy` + `server` + `sync` | **dies** (D5) |
| — | — | — | `auth`, `identity`, `sync` are new |

### 9.3 Other verified facts the plan depends on

- **Protos that exist:** `argus/camera/v1/{actions,sync}`, `argus/identity/v1/identity`,
  `argus/notification/v1/notification`, `argus/productivity/v1/sync`, `argus/voice/v1/voice`,
  `grpc/health/v1/health`, plus `tts.proto` and `response.proto` inside their packages. There
  is no proto for `llm`, `stt` or `vlm` — those clients are HTTP (`SttHttpClient`) (D16).
- **`clients/health` has zero sources** — it is proto-only and consumed server-side
  (`services/voice` and `services/camera` implement `grpc::health::v1::Health::CallbackService`),
  which is why it is not a client.
- **`packages/clients/llm-client` is two things:** the remote client and the tool framework
  (`tool-registry`, `tool-executor`, `tool-validator`, `tool-contracts`). The framework has one
  consumer and becomes an `llm` feature (D16). **This bullet's four files were measured in Phase
  1 step 9 and are two different things.** The machinery — `tool-registry`, `tool-executor`,
  `tool-validator` — has exactly one consumer (`services/llm`: `src/main.cc`, the controller, the
  adapter and its tests) and moved there in that step. The vocabulary, `tool-contracts.hxx`, has
  **two**, and one is a package: `packages/memory` declares its descriptors in
  `tools::ToolDescriptor` (`memory-tool-descriptors.hxx:3`, `memory-formation.hxx:5`) and takes a
  `tools::ToolCall` (`MemoryFormation::form`), so §2.4 rule 1 — a tier may never point back up —
  and §1.4's "no reaching into another service's `src/`" forbid it entering `services/llm` while
  memory is a package. It waits in the tier-3 client both sides link, and follows at **Phase 4
  step 7**, when memory leaves `packages/`. The step also corrected a claim this package's
  `CMakeLists` and f8-b2's message both made, that the descriptors travel with the chat contract:
  they do not — `ChatRequest` carries one tool-shaped field, `toolsEnabled`
  (`llm-service.hxx:29-30`), and the declarations the model reads are built server-side from the
  registry (`LfmAdapter::buildToolDeclarations`), so nothing on the client's wire surface names a
  `tools::` type. The tier-3 → tier-4 edge row 4 flagged is **not** gone with the machinery: the
  client still includes `role-access.hxx` (`tool-contracts.hxx:5`, for `RolePermission` on
  `ToolDescriptor`) and still links `argus::auth` (`CMakeLists.txt:45`), so it survives until
  Phase 4 step 7, narrowed to one header and two fields, and Phase 2 step 4's DAG check should
  read it that way.
- **`packages/config` is three packages in one folder:** the TOML reader
  (`config-service`, 97 consumers, tier 1), the health controller (18 including files) and the
  listener config (14). The last two build the envelope and register routes, so they are HTTP
  and belong in `lib/http`; leaving them in `lib/config` would make a tier-1 package depend on
  tier 2 (§2.4 rule 8).
- **`packages/response` links nothing but Drogon** (`CMakeLists.txt:14`), and
  `packages/validation` already depends on it only for `ValidationException`
  (`packages/validation/CMakeLists.txt:17`) — after the split the validation DSL depends on
  `lib/errors` alone and stops dragging the HTTP framework into a tier-1 package.
- **Error substrate reach:** `error-definition` / `error-code` / `response-exception` are
  consumed by `contracts/*`, `packages/storage`, `packages/identity`, `packages/sync` and four
  services — not HTTP-only, hence `lib/errors` (D17).
- **The HTTP config mixes layers, and there is no advice subclass to preserve:** `AppConfig`'s
  `ERROR_CODE_*` string constants duplicate the `enum class ErrorCode` already declared in
  `error-code.hxx` (`packages/response/src/config/app-config.cc:29-83`), and `SYNC_LIMIT{"200"}`
  — a sync wire invariant — lives there too. **Nobody subclasses `AppConfig`** anywhere in the
  tree: every service only calls `drogon::app().setExceptionHandler(AppConfig::handleException)`
  once (12 call sites), so `src/config/` + the one advice function replace it without losing
  anything (D20) — and since no subclass exists, nothing holds the class name in place: step 10
  splits the file into `error-handler`, `cors` and the health controller, and the 12 call sites
  read `ErrorHandler::handleException` (they are one line each either way). The
  `<svc>-config` classes that do exist (`CameraConfig`, `NotificationConfig`,
  `ProductivityConfig`) are config resolvers returning `dbPath`/`schemaPath`, not subclasses.
- **`user_action_log` is written, and it is `sync`'s.** Schema, query, repository and service
  live in `packages/audit`, and the writers are six call sites across four `identity` features
  (`user-feature-service`, `auth-service` ×3, `invitation-feature-service`,
  `portrait-preview-service`) — `UserAction::Read` on a portrait view, the session create on
  login, and user/invitation create-update-delete. AGENTS.md §7 makes it a requirement, so it
  is not dead code: it moves to `sync` in Phase 3a (§3.5).
- **Build output:** 52 GB of untracked `build/` trees across packages and services; none is
  tracked by git (`git ls-files | grep -c '/build/'` → 0). `packages/common/build` alone is
  374 MB and dies with its package.
- **The wire documentation still names the gateway as the consumer.** `wire-nats-subjects.md`
  lists it as the subscriber of every change subject (26 mentions), and the layout is described
  in `camera-guardian-deep-analysis.md` (13), `wire-camera-media.md` and `system-overview.md`
  (8 each), `events-and-contracts.md` (5), `services-and-packages.md` and `build-model.md`
  (4 each), `wire-device-identity.md` and `data-storage.md` (3 each), `sync-engine.md` (2).
  Payloads and subjects stay frozen; the subscriber columns and the layout prose are rewritten
  in Phase 3d step 4. `wire-sync-tables.md`, `wire-sync-golden-frames.md` and
  `contracts-overview.md` are already clean. **These files, not this plan, are the authority for
  what the wire contains** (§3.6).
- **One missing source in the whole tree** (scan of every `CMakeLists.txt` against disk):
  `services/gateway/src/shared/services/socket/nats-identity-change-sink.cc`.

---

## Appendix A — Alternatives considered and rejected

Proposals that were on the table and are **not** adopted, kept with their reason so they are not
re-proposed without new information.

- **`packages/sync-tablets`** — dropped. Cross-domain repositories in a shared package violate the hard rule (D1); each owner serves its own sync leg through its contract.
- **`guard` fused into `camera`** — not adopted. `guard` owns `guard.db`, its own policy and the belief gate; it stays a service.
- **`voice` fused into `sync`** — not adopted. `voice` is a pure-gRPC session service with no database; `sync` owns transport and fan-out.
- **`packages/contract` (singular) and per-domain contract folders** — replaced by `contracts/<domain>/` (D3).
- **`packages/enums` as a shared vocabulary package** — dropped (D12). The vocabulary is distributed to its owner service or contract; only what crosses the wire survives, declared once inside the contract that owns it.
- **`audio` as a microservice** — rejected (D9). It is DSP called per audio block inside two services; a service would add a per-block network hop to the conversation path. Voice selection belongs to `tts`; concurrent sessions belong to `voice`.

