# AGENTS.md — packages/contracts AI Agent Instructions

> This file is read by AI coding assistants before any code generation task.
> It defines the repo's purpose, conventions, and constraints.

## Project Identity

- **packages/contracts** — the source of truth for Argus wire contracts:
  protobuf v1 skeletons, capability/package manifests and frozen sync wire
  values.
- The contract group under `packages/` of the Argus backend, versioned via
  repo tags (`contracts-v*`), not an independent repo. Services (C++20 + Drogon)
  and the mobile app (`frontend/` React Native) coordinate against THIS folder,
  never against a service's local copy.
- English only: file contents, commit messages.

## Frozen contract policy

The following are **frozen forever**; changing them is a contract break and a
review blocker:

- HTTP paths and the response envelope `{status, info, errors}` (always present;
  `info`/`errors` empty when absent).
- Error codes `BAD_REQUEST`, `VALIDATION_ERROR`, `UNAUTHORIZED`, `FORBIDDEN`,
  `NOT_FOUND`, `METHOD_NOT_ALLOWED`, `CONFLICT`, `SERVICE_UNAVAILABLE`,
  `TOO_MANY_REQUESTS`, `INTERNAL_ERROR`, `CAMERA_UNREACHABLE`
  (`proto/argus/common/v1/base.proto`).
- `SyncOperation` 0-7 and `TableName` 0-23, both frozen in
  `proto/argus/sync/v1/contracts.proto`, and `SYNC_LIMIT = 200`, which is
  `SyncLimits::kMaxRows` in
  `packages/contracts/sync/src/sync/sync-limits.hxx` and cited by
  `docs/architecture/wire-sync-tables.md`. New sync operations may only use
  numbers >= 8.
- Versioning: packages `argus.<domain>.v1`; additive changes only inside v1.
  A retired field gets `reserved` + a new field. Breaking changes go to a new
  `/v2/...` route or package, which may run in parallel with v1 during the
  migration window.

## Layout

- `proto/argus/{domain}/v1/*.proto` — the shared proto root, one package per
  domain still served from it (`auth`, `camera`, `common`, `identity`,
  `notification`, `productivity`, `sync`, `voice` — eight — plus vendored
  `grpc/health/v1`). No orphan skeleton is left: the five domains whose gRPC
  boundary this work introduced (`response`, `stt`, `tts`, `vlm`, `llm`) each
  moved their schema out to their own package folder, and Phase 4 step 6c took
  the last one — `ai/v1/llm.proto`, unimported by any source — so the `ai`
  domain is gone.
- `response/`, `stt/`, `tts/`, `vlm/`, `llm/` — the five packages whose `.proto`
  sits at
  the folder root (`argus.response.v1`, `argus.stt.v1`, `argus.tts.v1`,
  `argus.vlm.v1`, `argus.llm.v1`, all self-contained). `stt`, `tts`, `vlm` and
  `llm` carry
  `src/<domain>/<domain>-errors.hxx`, the refusal catalog its server throws, and
  their own `argus_<domain>_rpc_contract()`, which calls
  `argus_response_rpc_contract()`, ensures `lib/grpc` and declares
  `argus::contracts::<domain>-wire`, so a client package or a service links the
  wire module and never generates a stub itself. `response` is where
  `argus_response_rpc_contract()` is **defined**, and the message and the two
  mapping functions of `src/response/response-rpc.{hxx,cc}` are its whole
  surface: no `argus_contracts` vocabulary and no refusal catalog of its own.
- `CMakeLists.txt` — `argus_contracts(NAME <domain> ...)` turns each domain's
  headers into the `argus::contracts::<domain>` vocabulary, an INTERFACE
  target. The generated stubs and the typed wrapper are a *client* package's
  job (`packages/clients/<domain>`, `argus::clients::<domain>`). Consumers link
  by module name only; generated headers stay in the build tree.
- `manifests/` — JSON Schemas for the typed capability package manifest
  (`package.schema.json`) and the ed25519-signed plugin manifest
  (`plugin.schema.json`).
- `sync/` — the frozen sync wire values (`SyncOperation` 0-7, `TableName` 0-23,
  `SYNC_LIMIT = 200`) as a C++ vocabulary under `sync/src/sync/`. The golden
  /sync fixtures live with the engine that replays them,
  `services/sync/tests/fixtures/sync/`.
- `routes/` — the LAN discovery spellings (`_argus-route._tcp` and the TXT keys
  `path`/`https`) as a C++ vocabulary under `routes/src/routes/`. Every
  app-facing service announces itself through them and the app discovers by
  them, so both sides read these constants instead of a literal.
- `buf.yaml` (lint STANDARD, breaking FILE) and `buf.gen.yaml` (C++ codegen).

## Conventions

- No comments in protos (root rule 20).
- Field numbers are never reused; removal means `reserved`, never renumbering.
- Enum numeric values mirror the C++ sources verbatim
  (`sync-operation.hxx`, `table-name.hxx`, `error-code.hxx`); when the C++
  changes, the proto change must quote the new source lines in its commit
  message.
- Generated code (`gen/`) is never committed.
- Validation before commit: `buf lint && buf breaking` when buf is available,
  else `protoc --descriptor_set_out=/dev/null -I proto $(git ls-files
  'proto/*.proto')`. Both forms span the `proto/` tree only — buf is absent
  from this machine and its module declares `path: proto`, so the five
  boundary contracts are validated with `for d in response stt tts vlm llm; do
  protoc --descriptor_set_out=/dev/null -I $d $d/$d.proto; done`.

## Code style for generated consumers (backend services)

- Headers `.hxx`, sources `.cc`, tests `*-test.cc` (e.g. `user-service-test.cc`);
  never `.h`/`.cpp`.
- No raw owning pointers; `std::unique_ptr` with custom deleters.
- No ORM: repositories build SQL by hand over an async `DbClient`.
- Dependency injection is manual, no framework: dependencies are private
  members with a `_` suffix (`userRepository_`, `service_`); controllers hold a
  non-static service member.
- No comments in code either (root rule 20).
> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, no comments in code, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
