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
- **Audit caveat (pre-cutover)**: `markAsRead` publishes user audit diffs
  through `SyncAuditService::publishUsers`, which targets substrate this
  service does not own (Ruling AO moves the diffs to NATS emission + gateway
  persistence at the cutover). Out-of-contract direct hits before F3-2 fail
  there; the cutover task replaces the emission.
- **Identity client (F3-2)**: the JWT filter resolves the caller's user row
  (and the bound refresh-token session) in identity.db, so the boot installs
  the named identity client read-only (same install as argus-productivity,
  Ruling AM) from the `[identity] db` key; without it the fallback to the
  default notification.db client would 401 every authenticated request.
- **`GET /health`**: standard `ApiResponse` envelope
  `{status: 200 (int), info: {service: argus-notification, uptimeSeconds},
  errors: null}`; never depends on any downstream service.
- **What stays away**: no read-path controller (the gateway keeps serving
  notification reads until F3-2), no /sync socket, no AI symbols (verified
  with `nm -C`), no alarm-triggering code.

## Build wiring (decisions)

- The canonical notification-service builds are the ROOT presets
  (`cmake --build --preset notification` / `--preset notification-prod`):
  they reuse the root Conan cache. The standalone `argus-notification/`
  build directory goes stale on new `conanfile.txt` requires until
  `conan install` is re-run there.
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
