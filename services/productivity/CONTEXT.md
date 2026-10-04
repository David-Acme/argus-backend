# argus-productivity — CONTEXT

## Why the productivity service exists

Fase 3 of the `migracion-microservicios` plan splits the productivity domain
out of the monolith (blueprint :566-569). This task (F3-1) creates the
substrate WITHOUT cutover — the legacy kept owning every productivity write
and the app kept talking through the gateway unchanged. `argus-productivity`
mirrors the proven argus-camera shape (F2-1): own binary, own CMake preset,
own `productivity.db`.

## What it owns (F3-1)

- **productivity.db**: the 7 productivity tables (`reminder`, `project`,
  `project_task`, `calendar_event`, `project_member`,
  `calendar_event_share`, `reminder_detail`) plus their 13 indexes, DDL at
  `services/productivity/database/schema.sql:10-131`, where the schema lands
  and is applied at boot through `DbService::runScriptFile` — abort on
  failure. `argus.db` is never
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
- **Write-side feature surface, one module per feature**: the calendar-event,
  calendar-event-share, project, project-member and project-task trees are
  each a module (`argus::productivity-<feature>`) under
  `src/feature/<feature>/`; their repositories and schemas are
  `argus::productivity-repositories`' under `src/shared/`. Every controller is
  a Drogon `HttpController<T, false>` and `main.cc` registers it explicitly, so
  the executable links the five feature modules plainly — no static-init route
  registration, and therefore no whole-archive link. The legacy keeps its own
  registration until F3-2 — this task changes NO legacy build input and NO
  legacy behavior. Until the F3-2 cutover, `productivity.db` is a migrate-tool
  copy, NOT the authoritative store: the gateway did not route here yet, so the
  registered routes were out-of-contract before the cutover (same reasoning as
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
- **Serving live traffic (F3-2, Ruling AP)**: the gateway relayed
  `/calendar-event`, `/calendar-event-share`, `/project`,
  `/project-member`, `/project-task` (all methods + subpaths) to this
  service with identical paths; responses are byte-identical with the
  legacy (envelope, statuses, CORS headers — verified live). Since Phase 3d
  step 1c this service serves those routes itself, on its own TLS listener
  and announced as one `_argus-route._tcp` instance per logical route, and
  the role checks run in its own `RoleFilter` over the shared table map. The
  personal-table scoping moved to this service with the sync RPC (rule 27);
  JWT resolution uses the identity RPC (Ruling AM).
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
  unscoped exactly as the legacy sync pull did. `argus-sync` consumes it
  through `argus::clients::productivity`; no other service opens productivity.db.
  The identity metadata is plain text anyone on the network can send, so
  `PullTable` first requires argus-sync's caller credential
  (`[grpc] caller_sync`, paired with sync's `[productivity] credential` by
  `setup.sh` and `provision-host.sh`), as notification's pull already did.
  Without it any container could pull any user's projects, tasks and
  events by naming them in `x-argus-user`.
- **CORS**: the legacy answered every preflight in pre-routing, so this
  surface keeps answering OPTIONS itself (`Cors::handleOptions`).
- **Audit recipients**: `publishAudit` keeps the same recipient set the
  legacy `SyncAuditService::publishUsers` kept — non-positive ids out,
  duplicates collapsed.
- **`GET /health`**: standard `ApiResponse` envelope
  `{status: 200 (int), info: {service: argus-productivity, uptimeSeconds},
  errors: null}`; never depends on any downstream service.
- **What stays away**: no reminder/reminder_detail write path anywhere
  (sync-read-only, Ruling AL — the repositories and schemas are read here by
  the sync feature and their own modules alone), no context_note table, no /sync socket
  (reads ride `argus-sync`'s `/sync` pull over this service's gRPC leg), no
  identity.db, no AI symbols (verified with `nm -C`), no alarm-triggering code.
- **Every write answers with the record it names** (2026-10, for the app's
  optimistic layer): a create and an update return the full row in `info`, so
  `info.id` is the server id the app confirms its pending intent with, and a
  delete returns `{deleted: true, id}`. The `id` on the delete answer is
  additive; the app reconciles a pending create, update or delete by that id
  and drops it when `/sync` delivers the same record.
- **A create may carry an `Idempotency-Key`** (2026-10). `POST /project`,
  `/project-task` and `/calendar-event` read the optional header (at most 64
  letters, digits, `-`, `_`; anything else is a 422). Inside the create's own
  transaction the key is looked up per user in `idempotency_key`: a hit on
  the same route answers the row it created (no second row, no second emit),
  a hit on another route is `409 IdempotencyKeyReused`, and a miss creates
  and records the key in the same commit. Keys live 24 h and are purged on
  the next keyed create. Without the header a create behaves exactly as
  before. The app's Retry after a timeout sends the same key, so a request
  that did reach the server cannot duplicate the row.
- **PATCH: omitted means unchanged, null means clear** (2026-10). A task's
  `dueAt: null` clears its due date and a calendar event's `endsAt: null`
  clears its end (`due_at = NULL` / `ends_at = NULL`); `location: null` and
  `description: null` clear those text columns to `''`, the value their
  `NOT NULL DEFAULT ''` already means. The DTO records an explicit null as
  a `clears*` flag beside the `std::optional` that means "provided"; a
  field left out of the body stays untouched, as before.

## The productivity change feed (3a-2d)

- Every emit and every audit diff lands in `change_outbox` in productivity.db
  before it is published: the row mutation and the change row commit as one
  unit of work — the feature service opens a single `db_transaction` and
  commits it once — and a worker publishes from the table and marks a row
  `sent` only on the JetStream PubAck. A broker outage, a crash in between or a
  restart leaves the rows pending and they drain at the next boot; before this
  both legs were fire-and-forget core publishes that a broker outage dropped
  without a trace. An enqueue the database refuses fails the write it belongs
  to: the mutation rolls back with the change row, so a change is recorded
  exactly when the row it names is, and a caller whose change could not be
  recorded gets `ChangeNotRecorded` instead of a committed write nobody is
  told about.
- **One transaction, one owner.** Drogon's `Transaction` has no `commit()`: the
  commit is the single shared pointer's destructor, so the transaction lives in
  a local of the coroutine that opened it and is moved into
  `db_transaction::Commit` exactly once; parking it in a struct field, a lambda
  capture or a sink argument leaves the caller suspended forever. Everything
  inside the window — reads included — must name that client, because
  productivity's pool holds one connection and Drogon hands a second waiter an
  infinite timeout. The repositories therefore take a borrowed
  `drogon::orm::DbClient*` (null means "the pooled client") and never a
  `shared_ptr`: nothing but the opening coroutine may own it.
- **Both legs go through the same table.** The emit leg (`emitUsers`) writes the
  `SocketEmitDto` triple plus `users` — the row event
  the fan-out routes into the recipients' rooms — and the audit leg
  (`publishAudit`) writes the `kind: audit` diff; the fan-out reads a missing
  `kind` as a row event. An emit's recipients travel exactly as the feature
  service named them, because that leg *is* the client's own row: only the
  audit leg collapses duplicates and drops non-positive ids, which is the set
  the legacy `publishUsers` kept.
- **The event id names the transition, not the record.** It is the hash of the
  table, the record id and the payload's own canonical JSON, plus, for an emit,
  the enqueue time and an in-process sequence: a stored row keeps its id across
  every publish retry (JetStream dedups on it), while a record that moves
  again, or returns to a state it already held, is its own event. The audit
  leg gets that from the payload's own `eventTimestamp`; the emit leg needs the
  stamp because an unshare followed by a re-share sends the parent's Add with a
  byte-identical payload, and keyed on the payload alone the outbox took it for
  a replay and the re-shared user never received the event again (2026-10).
  An emit carries its record id under
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
- **The emit became `[[nodiscard]] drogon::Task<void>`.** Routing an emit
  through the outbox makes it awaitable and rule 21 keeps blocking IO off the
  event loop, so the contract's `emitUsers` returns a `Task` like
  `publishAudit` already did; the `emitMembership` helper of the
  project-member and calendar-event-share services became a coroutine with it.
- **Installed whenever NATS is configured**, not only when the first connect
  succeeds: the outbox is what makes a broker that is down survivable, so the
  sink is bound at boot and its drain reconciles once the schema is applied.
- **The drain is stopped before Drogon quits, not after.** The sink registers
  with `shutdown_signal` at boot, **before `drogon::app().run()`** — which is
  also before `reconcile()` starts its worker:
  SIGTERM/SIGINT only
  requests the stop, the loop keeps running while the worker leaves its pass,
  and `quit()` follows once the worker reports drained (a 10-second deadline
  bounds the wait). The worker is a thread of the sink's own, so Drogon does
  not stop it, and `quit()` destroys the database client manager the worker
  reaches through `DbService::productivityClient()`. The registration comes first because
  the hook's handlers are what `run()` installs Drogon's `sigaction` over, and
  because a drain registered after the stop was requested is only stopped at
  once, never waited for.
- **One pass drains a batch.** A create-with-share is one burst of changes, so
  the drain reads up to 64 pending rows per pass, publishes them oldest-first
  and waits 50 ms while it is progressing, the retry cadence otherwise. A
  refused publish still stops the pass at the oldest pending row, so nothing
  behind it is overtaken; a row the broker stored but the service could not
  mark `sent` also stays at the head rather than being republished on every
  tick.
- **The drain wakes at the commit.** `enqueue` runs inside the feature's
  transaction, so the row is invisible until the commit; the sink's
  `db_transaction::CommitObserver` wakes the drain when a commit lands, and
  its `WakeSignal` keeps a wake that arrives mid-pass. A wake before the
  commit used to find nothing and leave every live change waiting the 500 ms
  retry period.

## Build wiring (decisions)

- The canonical build is the service's standalone graph. From the repository
  root use `scripts/build-all.sh dev --only productivity`; direct builds
  rerun Conan before the matching preset and CTest.
- The standalone build compiles no AI code: the `third_party/ncnn` block and
  the unused `find_package(OpenCV)` belonged to the `argus_identity` package's
  face services. Phase 3c-1 made identity a service of its own, reached
  through `argus::clients::identity`, so this project adds neither (zero AI
  symbols in the binary, verified with `nm -C`).

## Migration tool

`tools/migrate-productivity` (`argus-migrate-productivity`) copies the 7
tables from `argus.db` into `productivity.db` in FK-safe order and then goes
quiet: a schema-current `productivity.db` makes reruns a verified no-op,
because after the F3-2 cutover `productivity.db` is live data and the frozen
`argus.db` copy must never be resurrected over it. Nothing is deleted from
`argus.db`. Its `foreign_key_check` ignores user references by design (the
user parent rows live in identity.db) and fails on any violation between the
productivity tables themselves.

The copy names the source's own columns and the verification checksums those
columns on both sides, refusing only a source column the target lacks
(2026-10). It used to copy with `SELECT *` and compare the column lists of
`src` and `main` read as `"src".pragma_table_info(...)`, which SQLite
resolves against the main schema: the check compared the target with itself,
and a legacy table that predates a later additive column failed the copy on
its column count. `tests/unit/productivity-migration-test.cc` pins both cases.

## The folder owns its domain (f7-7b)

The five write-side feature trees (calendar-event, calendar-event-share,
project, project-member, project-task), the productivity schema and the
three unit suites moved out of the shared `src/` tree into this folder,
prefixes preserved, so no include line changed.

The suites register in the service's standalone CTest graph.

What did NOT move: the productivity repositories and schemas, which
`productivity-core` compiled here since sub-step 3a-1b — `argus-sync`'s `/sync`
still reads the same rows, through the productivity sync RPC. Phase 4 step 4
later split that archive: the five families 2+ features read declare
`argus::productivity-repositories` today, and the reminder pair compiles inside
`argus::productivity-sync`, its only reader.

## The reference shape (Phase 4 step 4)

The pre-migration spellings are gone: `src/main.cc` is `src/app/main.cc`, the
`src/feature/api/<resource>/` level is flattened so each feature folder *is*
its module, and the hand-listed `productivity-core` / `PRODUCTIVITY_FEATURE_SOURCES`
sources are replaced by declarations that live beside the code they compile
(root rule 25):

| Module | Compiles |
|---|---|
| `argus::productivity-config` | `src/config/` — db path, schema path, listener |
| `argus::productivity-change-sink` | `src/shared/services/change-sink/` — the NATS change sink |
| `argus::productivity-repositories` | the five repositories + their schemas |
| `argus::productivity-change-outbox` | the durable outbox |
| `argus::productivity-<feature>` (×5) | one feature's controllers, DTOs and feature service |
| `argus::productivity-sync` | the sync RPC service, plus the reminder repositories and schemas only it reads |
| `argus::productivity-rpc-server` | `src/app/rpc/` — the gRPC listener |

The 2+ rule decided the repositories: calendar-event, calendar-event-share,
project, project-member and project-task rows are each read by 2+ features and
stay in `src/shared/repositories`; the reminder rows are read by the sync
feature alone and moved into `src/feature/sync`. The executable is built by
`argus_service`, which is the only helper that applies `-Wall -Wextra`, the
`$ORIGIN` rpath and the `ARGUS_PORTS` property (7027 HTTP, 7037 gRPC).

## Phase 4 step 9: config resolution into `src/config/` (D20)

`src/productivity/` is gone. Its typed config is `src/config/
productivity-config.{hxx,cc}` (`argus::productivity-config`: db path, schema
path and the TLS listener) and its NATS change sink is
`src/shared/services/change-sink/` (`argus::productivity-change-sink`).
`main.cc` keeps `config.toml` loading, `drogonConfig` and the `nats.url` gate
on the optional bus.

## Grants are one row; scoped pulls (2026-10)

`PullTable` accepts `TablePull.scope_ids` on `project` (by `id`),
`project_task` (by `project_id`) and `calendar_event` (by `id`), at most 50
positive ids, and refuses it anywhere else with `INVALID_ARGUMENT`. The pull
keeps the caller's owner-or-member scope and pages by `(created_at, id)` from
zero (`FIND_SCOPED_HEAD/AFTER/TAIL` + `sync_query::buildScopedQuery`). Adding
or removing a project member or an event share now emits only the grant row,
to the owner and the grantee; the grantee's app pulls the parent's rows
through that scope (`services/sync/CONTEXT.md`, "Grants").

The native `config.toml.example` gained `[identity] rpc_secret`: identity
refuses `GetUser` without the fleet secret once it has one, and without the
key `setup.sh` never shared it here, so every grant on a native install
answered "User not found" (the deploy example already carried it).
