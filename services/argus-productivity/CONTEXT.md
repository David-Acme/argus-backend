# argus-productivity — CONTEXT

## Why the productivity service exists

Fase 3 of the `migracion-microservicios` plan splits the productivity domain
out of the monolith (blueprint :566-569). This task (F3-1) creates the
substrate WITHOUT cutover — the legacy keeps owning every productivity write
and the app keeps talking through the gateway unchanged. `argus-productivity`
mirrors the proven argus-camera shape (F2-1): own binary, own CMake preset,
own `productivity.db`.

## What it owns (F3-1)

- **productivity.db**: the 7 productivity tables (`reminder`, `project`,
  `project_task`, `calendar_event`, `project_member`,
  `calendar_event_share`, `reminder_detail`) plus their 6 indexes, DDL
  copied verbatim from `database/schema.sql:159-280`. The schema lands as
  `argus-productivity/database/schema.sql` and is applied at boot through
  `DbService::runScriptFile` — abort on failure. `argus.db` is never
  touched. `context_note` is NOT recreated (Ruling AK): it was an orphan
  table with no controller and no sync pull, and the frozen argus.db copy
  was dropped from `database/schema.sql` in F6-1.
- **Foreign keys stay off** (schema file pragma + re-applied after
  `applyPragmas`, which would otherwise turn them on per connection): every
  productivity table references `user(id)`, and the user rows live in
  identity.db, not here. The `REFERENCES user(id)` clauses are kept verbatim;
  enforcement is replaced by code — share/member targets are validated
  through the identity client (Ruling AM) and the JWT context provides the
  actor.
- **Write-side feature surface, registered in THIS binary only**: the
  calendar-event, calendar-event-share, project, project-member and
  project-task controllers + feature services + DTOs compile from the shared
  tree into the `argus-productivity` executable, and their repositories and
  schemas ride `argus_sync`. The controllers are Drogon `AutoCreation`
  controllers: their routes register during static init, exactly like the
  legacy binary registers them, and they cannot be registered explicitly
  (Drogon static-asserts against it), so the executable-target compilation is
  what guarantees the routes exist. The legacy keeps its own registration
  until F3-2 — this task changes NO legacy build input and NO legacy
  behavior. Until the F3-2 cutover, `productivity.db` is a migrate-tool copy,
  NOT the authoritative store: the gateway never routes here, so the
  registered routes are out-of-contract before the cutover (same reasoning as
  argus-camera F2-1, which shipped without routes because its brief
  constrained it — this brief instead directs the port to land now).
- **Change emission (F3-2 cutover, Rulings AQ/Y)**: the feature services no
  longer touch `SyncAuditService`/`SocketService` — every change goes
  through the `user_change` sink, whose argus-productivity binding produces
  the exact USER-SCOPED `user_audit_log` rows the legacy would have written
  (same `changes` JSON via `JsonDiff::createFlatDiff`, same per-user
  `userIds` expansion) and emits them over NATS (`argus.productivity.v1.change`,
  `argus-contracts/subjects.md`). The gateway persists them verbatim into
  identity.db; nothing audit-shaped is ever written to productivity.db.
- **Serving live traffic (F3-2, Ruling AP)**: the gateway relays
  `/calendar-event`, `/calendar-event-share`, `/project`,
  `/project-member`, `/project-task` (all methods + subpaths) to this
  service with identical paths; responses are byte-identical with the
  legacy (envelope, statuses, CORS headers — verified live). Role checks
  (Resident kFull / Guard read-only) and personal-table scoping stay
  gateway-side; JWT resolution uses the read-only identity client (Ruling
  AM). The databases open WAL with `busy_timeout`; no DDL runs at boot
  beyond the migrate tool's schema-current check.
- **Identity reads (Ruling AM, narrowed in f7-3)**: `[identity] db` opens
  mode=ro as the named identity client (`DbService::setIdentityClient` slot;
  SQLite URI filenames are enabled before the first `sqlite3_open` so the
  `mode=ro` URI parses); `UserRepository` reads then resolve to identity.db,
  so share targets created after the cutover are shareable. That is now the
  ONLY reason this service opens the file — the JWT filter stopped reading
  it in f7-3 and validates over `argus.identity.v1.ValidateToken` at
  `[identity] target` instead. Absent db key boots identity-free (share
  target validation degrades, authentication does not). The gateway creates
  identity.db at its own boot, which on a fresh install may land after ours,
  so the open waits bounded for the file to exist; the productivity tables
  reference user rows that live in identity.db, so foreign-key enforcement
  stays off on every connection.
- **CORS**: the legacy answered every preflight in pre-routing and the
  gateway forwards OPTIONS on proxied paths untouched, so this surface keeps
  answering OPTIONS itself (`AppConfig::handleOptions`).
- **Audit recipients**: `publishAudit` keeps the same recipient set the
  legacy `SyncAuditService::publishUsers` kept — non-positive ids out,
  duplicates collapsed.
- **`GET /health`**: standard `ApiResponse` envelope
  `{status: 200 (int), info: {service: argus-productivity, uptimeSeconds},
  errors: null}`; never depends on any downstream service.
- **What stays away**: no reminder/reminder_detail write path anywhere
  (sync-read-only, Ruling AL — the repositories/schemas exist in `argus_sync`
  and nothing more), no context_note table, no /sync socket (reads ride the
  gateway's sync pull until F3-2), no AI symbols (verified with `nm -C`), no
  alarm-triggering code.

## Build wiring (decisions)

- The canonical build is the service's standalone graph. From the repository
  root use `scripts/build-all.sh dev --only argus-productivity`; direct builds
  rerun Conan before the matching preset and CTest.
- The standalone build compiles ncnn only because `argus_identity` compiles
  the face services, whose headers need it; nothing references those objects,
  so they drop at link time (zero AI symbols).

## Migration tool

`tools/migrate-productivity` (`argus-migrate-productivity`) copies the 7
tables from `argus.db` into `productivity.db` in FK-safe order and then goes
quiet: a schema-current `productivity.db` makes reruns a verified no-op,
because after the F3-2 cutover `productivity.db` is live data and the frozen
`argus.db` copy must never be resurrected over it. Nothing is deleted from
`argus.db`. Its `foreign_key_check` ignores user references by design (the
user parent rows live in identity.db) and fails on any violation between the
productivity tables themselves.

## The folder owns its domain (f7-7b)

The five write-side feature trees (calendar-event, calendar-event-share,
project, project-member, project-task), the productivity schema and the
three unit suites moved out of the shared `src/` tree into this folder,
prefixes preserved, so no include line changed. The feature source list
is now a single `PRODUCTIVITY_FEATURE_SOURCES` variable that both the
executable and the controller suite consume, instead of the two
hand-kept copies (one here, one in the root test tree) that could drift.

The suites register in the service's standalone CTest graph.

What did NOT move: the productivity repositories and schemas, which
`argus_sync` still compiles because the gateway's `/sync` reads the same
rows, and the read-only identity.db the share/member validation needs
(Ruling AM, narrowed in f7-3).
