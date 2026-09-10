# argus-notification — CONTEXT

## Why the notification service exists

Fase 3 of the `migracion-microservicios` plan splits the notification domain
out of the monolith. This task (F3-1) creates the substrate WITHOUT cutover —
the legacy keeps owning every notification write and the gateway keeps its
read side unchanged. `argus-notification` mirrors the proven argus-camera
shape (F2-1) and the argus-productivity shape built in the same round: own
binary, own CMake preset, own `notification.db`.

## What it owns (F3-1)

- **notification.db**: the `notification` and `notification_token` tables
  (Ruling AN — single-owner), DDL copied verbatim from
  `database/schema.sql:428-450`. The schema lands as
  `database/notification-schema.sql` and is applied at boot through
  `DbService::runScriptFile` — abort on failure. `argus.db` is never
  touched. No indexes exist on these tables in the legacy schema, so the
  schema file carries none.
- **Foreign keys stay off** (schema file pragma + re-applied after
  `applyPragmas`, which would otherwise turn them on per connection): the
  tables reference `user(id)`, and the user rows live in identity.db, not
  here. The `REFERENCES user(id)` clauses are kept verbatim; enforcement is
  replaced by code — rows are always addressed through the JWT actor.
- **Write-side feature surface, registered in THIS binary only**: the
  notification and notification-token controllers, their feature services
  (markAsRead, registerToken) and DTOs compile from the shared tree into the
  `argus-notification` executable; the notification schema/repository ride
  `argus_sync`, and the notification-token repository/service compile from
  the shared tree (they are not part of `argus_sync`). The controllers are
  Drogon `AutoCreation` controllers: their routes register during static
  init, exactly like the legacy binary registers them, and they cannot be
  registered explicitly (Drogon static-asserts against it), so the
  executable-target compilation is what guarantees the routes exist. The
  legacy keeps its own registration until F3-2 — this task changes NO legacy
  build input and NO legacy behavior. Until the F3-2 cutover,
  `notification.db` is a migrate-tool copy, NOT the authoritative store: the
  gateway never routes here, so the registered routes are out-of-contract
  before the cutover.
- **Audit emission (F3-2 cutover, Rulings AQ/Y/AR)**: `markAsRead` no longer
  publishes through `SyncAuditService` — each change goes through the
  `user_change` sink, whose argus-notification binding produces the exact
  per-change rows the legacy `markAsRead` published (`userIds={userId}`,
  `changes` JSON via `JsonDiff::createFlatDiff`, TableName::Notification)
  and emits them over NATS (`argus.notification.v1.change`,
  `docs/architecture/wire-nats-subjects.md`). The gateway persists them verbatim into
  identity.db `user_audit_log`; nothing audit-shaped is written to
  notification.db.
- **Serving live traffic (F3-2, Ruling AR)**: the gateway relays
  `/notification/read` (PATCH) and `/notification-token` (POST) to this
  service with identical paths; the envelope, statuses and validation
  (empty/unknown id handling) are byte-identical with the legacy — verified
  live. notification.db opens WAL with `busy_timeout`; the gateway opens it
  read-only for its `/sync` notification pulls and never runs DDL. The
  legacy keeps its own markAsRead/token routes registered but they are
  unreachable through the gateway (Ruling AS — quiet, not stripped).
- **Identity validation (f7-3)**: the JWT filter validates the caller over
  `argus.identity.v1.ValidateToken` at `[identity] target` — the user row,
  the bound refresh-token session and the device binding are resolved by the
  identity service, which owns them. This service opens NO identity.db: the
  F3-2 read-only client install is gone, and with it the boot-order wait on
  a file another service creates (`nm -C` on the binary shows zero
  UserRepository / RefreshTokenRepository / DeviceCredentialRepository
  symbols). An unreachable identity service means 401, never an open door.
- **CORS**: the legacy answered every preflight in pre-routing and the
  gateway forwards OPTIONS on proxied paths untouched, so this surface keeps
  answering OPTIONS itself (`AppConfig::handleOptions`).
- **Foreign keys (Ruling AN)**: the notification tables reference user rows
  that live in identity.db, so foreign-key enforcement stays off on every
  connection.
- **Audit recipients**: `publishAudit` keeps the same recipient set the
  legacy `SyncAuditService::publishUsers` kept — non-positive ids out,
  duplicates collapsed.
- **`GET /health`**: standard `ApiResponse` envelope
  `{status: 200 (int), info: {service: argus-notification, uptimeSeconds},
  errors: null}`; never depends on any downstream service.
- **What stays away**: no read-path controller (notification list/read
  snapshots keep flowing through the gateway's `/sync` pulls over
  notification.db), no /sync socket, no AI symbols (verified
  with `nm -C`), no alarm-triggering code.

## Build wiring (decisions)

- The canonical build is the service's standalone graph. From the repository
  root use `scripts/build-all.sh dev --only argus-notification`; direct builds
  rerun Conan before the matching preset and CTest.
- The standalone build compiles ncnn only because `argus_identity` compiles
  the face services, whose headers need it; nothing references those
  objects, so they drop at link time (zero AI symbols).

## Migration tool

`tools/migrate-notification` (`argus-migrate-notification`) copies
`notification` and `notification_token` from `argus.db` into
`notification.db` and then goes quiet: a schema-current `notification.db`
makes reruns a verified no-op, because after the F3-2 cutover
`notification.db` is live data and the frozen `argus.db` copy must never be
resurrected over it. Nothing is deleted from `argus.db`. Its
`foreign_key_check` ignores user references by design (the user parent rows
live in identity.db) and fails on any other violation.

## The folder owns its domain (f7-7c)

The notification feature tree, the notification-token repository, schema
and service, the notification schema file and the three unit suites moved
out of the shared `src/` tree into this folder, prefixes preserved. The
write-side source list is one `NOTIFICATION_FEATURE_SOURCES` variable
shared by the executable and the controller suite, replacing the two
hand-kept copies.

What did NOT move: the `notification` table's own repository and schema,
which `argus_sync` compiles because the gateway's `/sync` serves those
rows and its camera-notifier writes them. Only the notification-TOKEN
side is exclusively this service's.
