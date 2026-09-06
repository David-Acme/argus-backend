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
  `database/productivity-schema.sql` and is applied at boot through
  `DbService::runScriptFile` — abort on failure. `argus.db` is never
  touched. `context_note` is NOT recreated: it is an orphan table with no
  controller and no sync pull (Ruling AK); its frozen argus.db copy stays
  and the drop decision is the user's.
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
- **Audit/socket caveat (pre-cutover)**: the feature services publish user
  audit diffs through `SyncAuditService::publishUsers` and emit live changes
  through `SocketService`. Both target substrate tables/channels this
  service does not own (Ruling AO moves the diffs to NATS emission + gateway
  persistence at the cutover). Out-of-contract direct hits before F3-2 fail
  there; the cutover task replaces the emission.
- **Identity reads (Ruling AM)**: `[identity] db` opens mode=ro as the named
  identity client (`DbService::setIdentityClient` slot); `UserRepository`
  reads then resolve to identity.db, so share targets created after the
  cutover are shareable. Absent key boots identity-free.
- **`GET /health`**: standard `ApiResponse` envelope
  `{status: 200 (int), info: {service: argus-productivity, uptimeSeconds},
  errors: null}`; never depends on any downstream service.
- **What stays away**: no reminder/reminder_detail write path anywhere
  (sync-read-only, Ruling AL — the repositories/schemas exist in `argus_sync`
  and nothing more), no context_note table, no /sync socket (reads ride the
  gateway's sync pull until F3-2), no AI symbols (verified with `nm -C`), no
  alarm-triggering code.

## Build wiring (decisions)

- The canonical productivity-service builds are the ROOT presets
  (`cmake --build --preset productivity` / `--preset productivity-prod`):
  they reuse the root Conan cache. The standalone `argus-productivity/` build
  directory goes stale on new `conanfile.txt` requires until `conan install`
  is re-run there.
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