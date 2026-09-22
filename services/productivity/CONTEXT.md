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
  `services/productivity/database/schema.sql` and is applied at boot through
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
  schemas into `productivity-core`. The controllers are Drogon `AutoCreation`
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
  `userIds` expansion) and hands them to the durable outbox that publishes them
  over NATS (`argus.productivity.v1.change`,
  `docs/architecture/wire-nats-subjects.md`; the change feed is below).
  `argus-sync` persists them verbatim into
  identity.db; nothing audit-shaped is ever written to productivity.db.
- **Serving live traffic (F3-2, Ruling AP)**: the gateway relays
  `/calendar-event`, `/calendar-event-share`, `/project`,
  `/project-member`, `/project-task` (all methods + subpaths) to this
  service with identical paths; responses are byte-identical with the
  legacy (envelope, statuses, CORS headers — verified live). Role checks
  stay in the gateway; the personal-table scoping moved to this service with
  the sync RPC (rule 27); JWT resolution uses the identity RPC (Ruling AM).
  The databases open WAL with `busy_timeout`; no DDL runs at boot
  beyond the migrate tool's schema-current check.
- **Identity reads (RPC-only since rule 27)**: this service opens no
  identity.db. The share/member target validation goes through
  `IdentityUserDirectory` (private member of both feature services), a
  `user-directory-identity.hxx` client of
  `argus.identity.v1.GetUser`; the JWT filter validates over
  `argus.identity.v1.ValidateToken` at `[identity] target`. A target that
  the RPC cannot resolve degrades to the same `UserNotFound` answer a
  missing row always produced.
- **Sync RPC owner (rule 27)**: `feature/sync/productivity-sync-rpc-service.cc`
  serves `argus.productivity.v1.SyncService` on `server.grpc_port` (7037).
  `PullTable` carries one of the 7 tables with the required-create/
  required-delete/find-last legs; the caller identity rides the
  x-argus-user/role/device metadata and scopes the personal tables
  (calendar_event, calendar_event_share, project, project_member,
  project_task) by owner-or-membership, while reminder/reminder_detail stay
  unscoped exactly as the monolith's gateway did. `argus-sync` consumes it
  through `argus::clients::productivity`; no other service opens productivity.db.
- **CORS**: the legacy answered every preflight in pre-routing and the
  gateway forwards OPTIONS on proxied paths untouched, so this surface keeps
  answering OPTIONS itself (`Cors::handleOptions`).
- **Audit recipients**: `publishAudit` keeps the same recipient set the
  legacy `SyncAuditService::publishUsers` kept — non-positive ids out,
  duplicates collapsed.
- **`GET /health`**: standard `ApiResponse` envelope
  `{status: 200 (int), info: {service: argus-productivity, uptimeSeconds},
  errors: null}`; never depends on any downstream service.
- **What stays away**: no reminder/reminder_detail write path anywhere
  (sync-read-only, Ruling AL — the repositories/schemas compile into
  `productivity-core` and nothing more), no context_note table, no /sync socket
  (reads ride `argus-sync`'s `/sync` pull over this service's gRPC leg), no
  identity.db, no AI symbols (verified with `nm -C`), no alarm-triggering code.

## The productivity change feed (3a-2d)

- Every emit and every audit diff lands in `change_outbox` in productivity.db
  before it is published: the row mutation commits first, the change row is
  written after it, and a worker publishes from the table and marks a row
  `sent` only on the JetStream PubAck. A broker outage, a crash in between or a
  restart leaves the rows pending and they drain at the next boot; before this
  both legs were fire-and-forget core publishes that a broker outage dropped
  without a trace. An enqueue the shared database connection refuses is retried
  before it is given up on: the mutation it records has already committed, and
  no later event repairs a change that was recorded nowhere.
- **Both legs go through the same table.** The emit leg (`emitUser`,
  `emitUsers`) writes the `SocketEmitDto` triple plus `users` — the row event
  the fan-out routes into the recipients' rooms — and the audit leg
  (`publishAudit`) writes the `kind: audit` diff; the fan-out reads a missing
  `kind` as a row event. An emit's recipients travel exactly as the feature
  service named them, because that leg *is* the client's own row: only the
  audit leg collapses duplicates and drops non-positive ids, which is the set
  the legacy `publishUsers` kept.
- **The event id names the transition, not the record.** It is the hash of the
  table, the record id and the payload's own canonical JSON, so a redelivery
  recomputes the same id while a record that moves again, or returns to a state
  it already held, is its own event. An emit carries its record id under
  `info.id`; one that carries none, or one that is not integral, has nothing to
  be keyed by and is logged and dropped rather than recorded under a wrong
  name.
- **The change leg owns its own stream.** `publishWithMsgId` is a JetStream
  publish with no core-NATS fallback, and nothing else in the tree captures
  `argus.productivity.v1.change` — no other service ensures a stream over it —
  so the subject is retained on `ARGUS_PRODUCTIVITY_CHANGE`, created
  self-healing by the drain: an ensure that fails is retried on the next tick
  instead of latching, and a publish the broker refuses clears the latch,
  because a stream that disappears under a running process looks exactly like
  that. `argus-sync` and the memory catalog replica read the subject over core
  NATS, so the stream exists for the PubAck. The name lives in the sink's own
  `Config`, not in `lib/nats`: nothing outside this service names it.
- **The emits became `[[nodiscard]] drogon::Task<void>`.** Routing an emit
  through the outbox makes it awaitable and rule 21 keeps blocking IO off the
  event loop, so the contract's two emits return a `Task` like `publishAudit`
  already did; the `emitMembership` helper of the project-member and
  calendar-event-share services became a coroutine with them.
- **Installed whenever NATS is configured**, not only when the first connect
  succeeds: the outbox is what makes a broker that is down survivable, so the
  sink is bound at boot and its drain reconciles once the schema is applied.
- **One pass drains a batch.** A create-with-share is one burst of changes, so
  the drain reads up to 64 pending rows per pass, publishes them oldest-first
  and waits 50 ms while it is progressing, the retry cadence otherwise. A
  refused publish still stops the pass at the oldest pending row, so nothing
  behind it is overtaken; a row the broker stored but the service could not
  mark `sent` also stays at the head rather than being republished on every
  tick.

## Build wiring (decisions)

- The canonical build is the service's standalone graph. From the repository
  root use `scripts/build-all.sh dev --only productivity`; direct builds
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
`productivity-core` compiles here since sub-step 3a-1b — `argus-sync`'s `/sync`
still reads the same rows, through the productivity sync RPC.
