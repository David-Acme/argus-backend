# CONTEXT.md — why this folder exists

## Origin

The Argus backend (C++20/Drogon monolith) is being migrated to microservices
(see `docs/history/plans/microservices-migration-plan.md`, phase 0, step 5). Once several
services exist, a contract owned by any single service stops working: the
mobile app and every service must agree on one wire format. This folder is that
single source of truth.

## What lives here

- `proto/argus/<domain>/v1/*.proto` — the wire contracts. Since F6-3 they are
  compiled: a client package passes its domain's `.proto` to `argus_clients`,
  which turns it into `argus::clients::<domain>` (protobuf + gRPC stubs plus a
  thin typed wrapper), and services link the SDK instead of speaking raw
  strings.
- `<domain>/src/<domain>/` — the C++ vocabulary that crosses the wire, declared
  by `argus_contracts` as `argus::contracts::<domain>`. The include root is
  `src/`, so a consumer writes `<domain>/<header>.hxx` and links by module name.
  The thin per-domain wrappers over the generated stubs live with their client
  (`packages/clients/<domain>/`), not here, so consumers never see protobuf
  types directly.
- `manifests/package.schema.json` — typed per-capability package manifest
  (`model_path`, `accepted_models`, `defaults`) for interchangeable on-device
  models, without a database; consumers pin a version range (e.g. `llm: ^1.2`).
- `manifests/plugin.schema.json` — plugin `manifest.json` (declarative views,
  permissions, `requires`), ed25519-signed and verified before install.
- `sync/` — the frozen sync wire values (`SyncOperation` 0-7, `TableName`
  0-23, `SYNC_LIMIT = 200`) extracted verbatim from the backend, as a C++
  vocabulary under `sync/src/sync/`. The golden /sync fixtures live with the
  engine that replays them, `services/sync/tests/fixtures/sync/`.
- `routes/` — the LAN discovery contract: the service type `_argus-route._tcp`
  and the TXT keys `path` and `https`, one file under `routes/src/routes/`. A
  service announces one instance per logical route through `lib/http`'s
  `routeAnnouncements()`; the app resolves the type and reads the SRV port, so
  the spellings are the one thing both sides must agree on.

## Invariants the migration depends on

- The HTTP paths are the frozen surface every deployed client already speaks:
  paths, the `{status, info, errors}` envelope, its error codes and
  `SyncOperation` 0-7 cannot change without breaking them (the mobile app
  paints persisted local data first, then reacts to live sync).
- Proto evolution inside v1 is additive only: `reserved` for retired fields,
  never renumbering, never reassigning enum numbers 0-7 or table ids 0-23.

## Codegen substrate (F6-3)

`cmake/argus-module.cmake` exposes `argus_contracts_substrate()` for every
standalone consumer's contracts module to resolve Protobuf + gRPC through. The
tree has one `conanfile.txt`, at the
repository root, and one Conan graph: `scripts/build-all.sh` resolves it once
and every project configures against the toolchain that install produced. When
that graph carries a Conan abseil — it does, through onnxruntime's protobuf —
the substrate records `ARGUS_SECOND_ABSEIL_FLAVOR` and
`argus_grpc_absl_bridge()` builds the two-object cq bridge that joins the
SDK's abseil inline namespace to the one inside the vendored `libgrpc`: the
entry defines `grpc_call_run_cq_cb` in the SDK's flavor, the exit calls the
vendored one, and both reach the binary through the generated client module.
With a single abseil flavor the entry's definition would instead interpose the
identically-named implementation inside `libgrpc`, whose callbacks would land
back on the entry and recurse — measured on `socket`'s suite before the flavor
count was recorded — so the bridge exists only when the substrate has measured
two. The gRPC toolchain on the development host is vendored under
`~/.local/argus-thirdparty/grpc` (Arch grpc 1.82.1 shared libraries); the
CMake fallback appends that prefix and records the library directory so
`argus_runtime_rpath()` can add it to every gRPC-linked binary. The service
images instead use Debian trixie's grpc 1.51 packages — the code avoids APIs
whose signatures differ across those versions (e.g. no `OnCancel` overrides;
`OnDone` + `IsCancelled()` instead). Generated stubs land in the build tree
and are never committed. `grpc.health.v1` is vendored verbatim from the
upstream protobuf well-known types so the health surface does not depend on
host-specific well-known-proto installs.

The service images pin the whole conan graph to protobuf 3.21.12 (every
service Dockerfile: `[replace_requires] protobuf/*: protobuf/3.21.12`) so it
matches Debian
trixie's protobuf, which the SDK binds to under `ARGUS_SYSTEM_PROTOBUF`.
onnxruntime pulls conan protobuf 6.33 statically into `argus-voice`; with
two different protobuf runtimes in one binary the same-mangled-name
symbols interpose across versions and heap corruption hits at static
descriptor registration (observed as a SIGSEGV crash loop). Same version
on both runtimes is the only safe mix; Debian grpc++ 1.51 headers cannot
pair with conan protobuf 6.33 headers, so pinning the graph down is the
one available direction. The dev host is unaffected: the vendored grpc
1.82 stack pairs with conan protobuf only, a single runtime.

## Productivity and notification sync contracts (rule 27)

`proto/argus/productivity/v1/sync.proto` types the productivity domain's
frozen `/sync` semantics: `SyncService.PullTable` carries one of the 7 table
branches (reminder, reminder_detail, calendar_event, calendar_event_share,
project, project_member, project_task) with the required-create/required-delete/
find-last legs and the (createdAt, id) cursor range, and answers typed rows
plus `{id, deletedAt}` tombstones plus the optional last-row watermarks. The
SDK wrapper `argus::clients::productivity`
(`packages/clients/productivity/src/productivity/productivity-sync-client.cc`) is the one wire-handling
point: a blocking unary pull with the x-argus-user / x-argus-role /
x-argus-device metadata and a 5s deadline; the owner applies the
owner-or-membership scoping for the personal tables.

`proto/argus/notification/v1/notification.proto` types the notification
domain: `NotificationService.CreateNotifications` is the fan-out create
(one row per user id, the surface a remote caller uses; the service's own
camera-notifier creates in-process since Phase 3d step 1) and
`PullNotifications` is the user-scoped `/sync` page (created rows plus the
last-created watermark). The SDK wrapper `argus::clients::notification`
(`packages/clients/notification/src/notification/notification-client.cc`) carries the same metadata and
deadline shape. Both owners are the only openers of their databases;
`argus-sync` links the notification and productivity targets and neither
mounts those volumes (rule 27).

## Camera sync contract (F6-5)

`proto/argus/camera/v1/sync.proto` types the frozen /sync semantics of the
camera domain: `SyncService.PullTable` carries one camera/camera_stream/zone
branch with the required-create/required-delete/find-last legs and the
(createdAt, id) cursor range, and answers typed rows (`CameraRow`,
`CameraStreamRow`, `ZoneRow`) plus `{id, deletedAt}` tombstones. The row
messages mirror the sync-table JSON field sets exactly (driver/record_mode/
zone_type stay strings like the CRUD contract; secrets never cross it). The
SDK wrapper `argus::clients::camera` (`packages/clients/camera/src/camera/camera-sync-client.cc`) is the
one wire-handling point: a blocking unary pull with the
x-argus-user / x-argus-role / x-argus-device metadata and a 5s deadline.
`packages/lib/grpc/src/grpc/grpc-server-identity.hxx` is the server-side twin of that
metadata contract: every owner handler reads the caller through
`argus::client::callerUserId` (camera sync, productivity sync, notification
RPC) instead of re-parsing the metadata per service.
The contracts subdirectory guards are per-module now, so a build that adds
the contract group twice still defines whichever SDK targets are missing.

## Caller credentials and the camera action surface (camera guard)

Service-to-service authority no longer rides declared metadata.
`packages/lib/grpc/src/grpc/grpc-client-base.hxx` attaches
`x-argus-credential` (`addCallerCredential`);
`packages/lib/grpc/src/grpc/grpc-server-identity.hxx` matches
it against the receiver's configured `CallerCredential{service, secret}` set
(`authorizeCaller`, constant-time compare) and the matched secret is the
authority — a forged `x-argus-user`/`x-argus-role` pair without the secret
authenticates as nobody. Each RPC declares its own accepted caller set.

`proto/argus/camera/v1/actions.proto` (`CameraActionService`) is the single
audible/physical surface, served by argus-camera on 7036 and called only by
argus-guard: `Announce` (remote TTS + talk), `Alarm` (procedural tone),
`SetSiren` (arming as an expiring `lease_seconds` lease), `GetPersonCrop`
(bounded per-track snapshot ring) and `Listen` (endpointed capture + STT).
Outcomes are `CommandOutcome` (`SUCCEEDED`, `DUPLICATE_SUCCEEDED`,
`IN_FLIGHT`, `INDETERMINATE`, `REJECTED`, `RETRYABLE_FAILED`, `CONFLICT`);
the thin wrapper is `argus::clients::camera-actions`
(`packages/clients/camera-actions/src/camera/camera-action-client.cc`, 60 s call timeout). Commands are
idempotent by `command_id` (length-prefixed SHA-256 fingerprint, fenced
settle, expired claims reconciled to `indeterminate`), and the RPC requires
both the fleet secret and the `[actions].enabled` gate.

The notification `CreateNotifications` fan-out is idempotent the same way:
`command_id` plus the persisted payload fingerprint — a reused id with a
different payload answers `ALREADY_EXISTS`, a reused id with the same payload
replays the persisted counts. The SDK result type carries
`Success`/`Conflict`/`Rejected`/`Unavailable` instead of an optional.
