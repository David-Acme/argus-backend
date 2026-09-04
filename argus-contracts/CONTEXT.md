# CONTEXT.md — why this folder exists

## Origin

The Argus backend (C++20/Drogon monolith) is being migrated to microservices
(see `docs/migracion-microservicios.md`, phase 0, step 5). Once several
services exist, a contract owned by any single service stops working: the
mobile app and every service must agree on one wire format. This folder is that
single source of truth.

## What lives here

- `proto/argus/<domain>/v1/*.proto` — v1 draft skeletons derived from the
  current backend DTO/controller semantics. They are drafts for the later
  implementation phase, not compiled code yet; the C++ services keep serving
  HTTP/WS on the same paths during the migration.
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