# AGENTS.md — argus-contracts AI Agent Instructions

> This file is read by AI coding assistants before any code generation task.
> It defines the repo's purpose, conventions, and constraints.

## Project Identity

- **argus-contracts** — the source of truth for Argus wire contracts: protobuf
  v1 skeletons, capability/package manifests and frozen sync wire values.
- Top-level folder of the Argus monorepo (rooted at `backend/`), versioned via
  repo tags (`contracts-v*`), not an independent repo. Services (C++20 + Drogon)
  and the mobile app (`frontend/` React Native) coordinate against THIS folder,
  never against a service's local copy.
- English only: file contents, comments, commit messages.

## Frozen contract policy

The following are **frozen forever**; changing them is a contract break and a
review blocker:

- HTTP paths and the response envelope `{status, info, errors}` (always present;
  `info`/`errors` empty when absent).
- Error codes `BAD_REQUEST`, `VALIDATION_ERROR`, `UNAUTHORIZED`, `FORBIDDEN`,
  `NOT_FOUND`, `METHOD_NOT_ALLOWED`, `CONFLICT`, `SERVICE_UNAVAILABLE`,
  `TOO_MANY_REQUESTS`, `INTERNAL_ERROR`, `CAMERA_UNREACHABLE`
  (`proto/argus/common/v1/base.proto`).
- `SyncOperation` 0-7, `TableName` 0-23 and `SYNC_LIMIT = 200`
  (`proto/argus/sync/v1/contracts.proto`, `sync/README.md`). New sync
  operations may only use numbers >= 8.
- Versioning: packages `argus.<domain>.v1`; additive changes only inside v1.
  A retired field gets `reserved` + a new field. Breaking changes go to a new
  `/v2/...` route or package, which may run in parallel with v1 during the
  migration window.

## Layout

- `proto/argus/{domain}/v1/*.proto` — one package per domain
  (`common`, `camera`, `ai`, `productivity`, `notification`, `memory`, `sync`).
- `manifests/` — JSON Schemas for the typed capability package manifest
  (`package.schema.json`) and the ed25519-signed plugin manifest
  (`plugin.schema.json`).
- `sync/` — frozen sync values + golden-frame fixtures.
- `buf.yaml` (lint STANDARD, breaking FILE) and `buf.gen.yaml` (C++ codegen).

## Conventions

- Minimal comments: short English one-liners only, no prose blocks in protos.
- Field numbers are never reused; removal means `reserved`, never renumbering.
- Enum numeric values mirror the C++ sources verbatim
  (`sync-operation.hxx`, `enums.hxx`, `app-config.hxx`); when the C++ changes,
  the proto change must quote the new source lines in its commit message.
- Generated code (`gen/`) is never committed.
- Validation before commit: `buf lint && buf breaking` when buf is available,
  else `protoc --descriptor_set_out=/dev/null -I proto $(git ls-files 'proto/*.proto')`.

## Code style for generated consumers (backend services)

- Headers `.hxx`, sources `.cc`, tests `*-test.cc` (e.g. `user-service-test.cc`);
  never `.h`/`.cpp`.
- No raw owning pointers; `std::unique_ptr` with custom deleters.
- No ORM: repositories build SQL by hand over an async `DbClient`.
- Dependency injection is manual, no framework: dependencies are private
  members with a `_` suffix (`userRepository_`, `service_`); controllers hold a
  non-static service member.
- Minimal comments in code too: direct "what it does" one-liners only.
> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, comment discipline, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
