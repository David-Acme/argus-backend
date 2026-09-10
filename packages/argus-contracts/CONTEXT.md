# CONTEXT.md — why this folder exists

## Origin

The Argus backend (C++20/Drogon monolith) is being migrated to microservices
(see `docs/migracion-microservicios.md`, phase 0, step 5). Once several
services exist, a contract owned by any single service stops working: the
mobile app and every service must agree on one wire format. This folder is that
single source of truth.

## What lives here

- `proto/argus/<domain>/v1/*.proto` — the wire contracts. Since F6-3 they are
  compiled: `cmake/argus-module.cmake` turns each domain into an
  `argus::sdk-<domain>` C++ module (protobuf + gRPC stubs plus a thin typed
  wrapper), and services link the SDK instead of speaking raw strings.
- `sdk/` — the thin per-domain C++ wrappers over the generated stubs
  (`sdk/voice/`, `sdk/identity/`), so consumers never see protobuf types
  directly.
- `manifests/package.schema.json` — typed per-capability package manifest
  (`model_path`, `accepted_models`, `defaults`) for interchangeable on-device
  models, without a database; consumers pin a version range (e.g. `llm: ^1.2`).
- `manifests/plugin.schema.json` — plugin `manifest.json` (declarative views,
  permissions, `requires`), ed25519-signed and verified before install.
- `sync/` — the frozen sync wire values (`SyncOperation` 0-7, `TableName`
  0-23, `SYNC_LIMIT = 200`) extracted verbatim from the backend, plus golden
  fixtures.

## Invariants the migration depends on

- The gateway routes by path prefix identical to the current HTTP paths, so
  paths, the `{status, info, errors}` envelope, its error codes and
  `SyncOperation` 0-7 cannot change without breaking every deployed client
  (the mobile app paints persisted local data first, then reacts to live sync).
- Proto evolution inside v1 is additive only: `reserved` for retired fields,
  never renumbering, never reassigning enum numbers 0-7 or table ids 0-23.

## Codegen substrate (F6-3)

`CMakeLists.txt` exposes `argus_contracts_substrate()` for every standalone
consumer to resolve Protobuf + gRPC. The package builds with its own
`conanfile.txt` and dev/prod presets: with a single abseil flavor the cq bridge entry
symbol would interpose the identically-named implementation inside
`libgrpc`, so the standalone configure declares empty bridge stand-ins and
lets the vendored gRPC resolve its callbacks natively. The
gRPC toolchain on the development host is vendored under
`~/.local/argus-thirdparty/grpc` (Arch grpc 1.83.1 shared libraries); the
CMake fallback appends that prefix and records the library directory so
`argus_runtime_rpath()` can add it to every gRPC-linked binary. The container
image instead uses Debian trixie's grpc 1.51 packages — the code avoids APIs
whose signatures differ across those versions (e.g. no `OnCancel` overrides;
`OnDone` + `IsCancelled()` instead). Generated stubs land in the build tree
and are never committed. `grpc.health.v1` is vendored verbatim from the
upstream protobuf well-known types so the health surface does not depend on
host-specific well-known-proto installs.

The image pins the whole conan graph to protobuf 3.21.12 (Dockerfile:
`[replace_requires] protobuf/*: protobuf/3.21.12`) so it matches Debian
trixie's protobuf, which the SDK binds to under `ARGUS_SYSTEM_PROTOBUF`.
onnxruntime pulls conan protobuf 6.33 statically into `argus-voice`; with
two different protobuf runtimes in one binary the same-mangled-name
symbols interpose across versions and heap corruption hits at static
descriptor registration (observed as a SIGSEGV crash loop). Same version
on both runtimes is the only safe mix; Debian grpc++ 1.51 headers cannot
pair with conan protobuf 6.33 headers, so pinning the graph down is the
one available direction. The dev host is unaffected: the vendored grpc
1.82 stack pairs with conan protobuf only, a single runtime.

## Camera sync contract (F6-5)

`proto/argus/camera/v1/sync.proto` types the frozen /sync semantics of the
camera domain: `SyncService.PullTable` carries one camera/camera_stream/zone
branch with the required-create/required-delete/find-last legs and the
(createdAt, id) cursor range, and answers typed rows (`CameraRow`,
`CameraStreamRow`, `ZoneRow`) plus `{id, deletedAt}` tombstones. The row
messages mirror the sync-table JSON field sets exactly (driver/record_mode/
zone_type stay strings like the CRUD contract; secrets never cross it). The
SDK wrapper `argus::sdk-camera` (`sdk/camera/camera-sync-client.cc`) is the
one wire-handling point: a blocking unary pull with the
x-argus-user / x-argus-role / x-argus-device metadata and a 5s deadline.
The contracts subdirectory guards are per-module now, so a build that adds
`argus-contracts` twice still defines whichever SDK targets are missing.
