# Sub-step 3a-2d — the productivity durable change outbox

Scope: `services/productivity` stops publishing its change feed as a
fire-and-forget core NATS publish and starts writing every emit and audit diff
into a productivity-owned `change_outbox` first, one worker publishing from the
table and marking a row `sent` only on the JetStream PubAck. The third of the
four producers of §3.6; the shape is copied from `services/notification`'s
(3a-2c), which was copied from `services/camera`'s (3a-2b).

This unit also changes a shared contract: `UserChangeSink`'s two emits stop
being `void` and become `[[nodiscard]] drogon::Task<void>`.

## Pre-state, measured

| Site | At `HEAD` |
|---|---|
| `services/productivity/database/schema.sql` | 7 productivity tables and 13 indexes; no `change_outbox` |
| `NatsProductivityChangeSink` (`src/productivity/nats-productivity-change-sink.{hxx,cc}`) | one class with `void emitUser`/`void emitUsers`/`drogon::Task<void> publishAudit`, each ending in `bus_->publish(nats_subject::kProductivityChange, …)` — core NATS, the return value feeding a `LOG_WARN` at most |
| `user_change::setProductivitySink` (`src/main.cc:130`) | installed **only** when `natsBus->connect()` returned true; a broker that is down at boot disables the feed for the process's life, and a missing sink drops every change with a `LOG_WARN` |
| `NatsBus` stream over the change subject | none — `ensureStream` was called for `ARGUS_GUARD`, `ARGUS_NOTIFICATION` and `ARGUS_NOTIFICATION_CHANGE` and `ARGUS_CAMERA` only, and the change subject has no deployment-side stream either (`argus-deploy/docker-compose.yml` mounts no stream config) |
| `packages/contracts/sync/src/sync/user-change-sink.hxx:36-39` | `virtual void emitUser(…) const = 0;` / `virtual void emitUsers(…) const = 0;` |
| Call sites of the funnel in this service | 14: 7 `emitUsers`/`emitUser` (calendar-event, calendar-event-share ×2, project, project-member ×2, project-task) and 7 `publishAudit` (calendar-event, calendar-event-share ×2, project, project-member ×2, project-task) |
| Implementers of `UserChangeSink` in the tree | exactly two — this service's sink and `RecordingSink` in `tests/unit/productivity-controller-test.cc:221` (notification's two doubles implement `AuditSink` only) |
| Consumers of `argus.productivity.v1.change` | `services/sync`'s fan-out (`sync-fan-out.cc:131`, wildcard `argus.*.v1.change`) and `packages/memory`'s catalog replica (`catalog-replica.cc:105`, same wildcard, no-ops on non-`camera_stream` rows) — both over **core** NATS, so the stream this unit creates exists for the PubAck, not for a consumer |

## The design

**The module is copied, not packaged** — `services/productivity/src/shared/
repositories/change-outbox/` holds its own `argus_module(NAME
productivity-change-outbox …)`, as §3.6 requires and as camera and notification
already do. The five files are notification's, and a byte comparison of the two
copies reports exactly five differences: the key prefix
(`"productivity-change:"` in `change-outbox-key.hxx:31`), the repository's
class comment, the two log prefixes, and the module name in `CMakeLists.txt`.
Everything else — the query namespace, the dispositions, the status-guarded
CAS, `fingerprintJson` — is identical on purpose: an operator should find one
table with one behaviour under one name in every producer's database.

**Both legs go through the table.** `emitUser`/`emitUsers` and `publishAudit`
each build their payload and enqueue it; the difference between them is what
they put in it. The emit leg writes `sync_change::userEmitPayload(body, users)`
— the `SocketEmitDto` triple (`operation`, `option`, `info`) plus `users` and
no `kind` — and the audit leg writes `UserAuditEvent::toJson()`, which carries
`kind: "audit"`. The fan-out reads a missing `kind` as a row event
(`sync-fan-out.cc:34-42`, `kindOf` returns `""`), so both shapes are already
consumed correctly and the payload is published byte for byte as built. The
emit's recipients are **not** deduplicated: that leg is the client's own row,
the fan-out dedups recipients itself, and adding a filter here would be a
behaviour change smuggled into a durability change. The audit leg keeps the
dedup and the non-positive-id drop it already had, which is the set the legacy
`publishUsers` kept (`CONTEXT.md:82-84`).

**The event id names the transition, not the record.** For an emit the record
id comes from `body.obj["id"]` — an emit that carries no id, or one that is not
integral, has nothing to be keyed by and is logged and dropped rather than
recorded under a wrong name. That is a real case only for a hand-built body:
all 7 emit sites in this service set `info` from a repository row's `toJson()`
or from an `id`-bearing tombstone.

**The change leg owns its own stream.** `publishWithMsgId` is JetStream-only
and never degrades to core NATS (`nats-bus.hxx:54-57`), and nothing else in the
tree ensures a stream over this subject, so the sink ensures
`ARGUS_PRODUCTIVITY_CHANGE` itself (7 days, file storage, 2-minute duplicate
window), self-healing: a failed ensure is retried on the next tick rather than
latching, and a publish the broker refuses clears the latch. The name lives in
the sink's own `Config`, not in `lib/nats` — nothing outside this service names
it.

**The drain is the improved shape 3a-2c landed**: `pendingBatch(kDrainBatch=64)`
in one statement, `flush(row)` per row, wait 50 ms while progressing and
`config_.retryMs` (default 500 ms) otherwise, a refused publish stops the pass
at the oldest pending row, and a row the broker stored but the database could
not mark `sent` stays at the head instead of being republished at the progress
cadence.

**The boot wiring is notification's.** The advice moves below the NATS block and
captures `&changeSink`; the sink is constructed and installed whenever
`nats.url` is configured, whether or not the first `connect()` succeeded — the
outbox is exactly what makes a broker that is down survivable, and a sink that
is never installed drops every change with a `LOG_WARN`. `reconcile()` runs
from the advice, after the schema has been applied, so the drain thread never
outlives the database it reads. The bus is kept on a failed connect for the
same reason (it reconnects in the background).

## The contract change, and its real blast radius

Routing an emit through the outbox makes it awaitable, and rule 21 keeps
blocking IO off the event loop, so the two emits cannot stay `void`. They become
`[[nodiscard]] drogon::Task<void>`, exactly like the `publishAudit` they share
`AuditSink` with and like camera's `emitModule`.

The first reading of this change called it contained ("the only implementers
are productivity's sink and productivity's test fake, and every existing call
site sits inside a coroutine"). An adversarial verification **refuted** that
reading, and it was right: two call sites sit in a plain `void` function —
`ProjectMemberFeatureService::emitMembership` and
`CalendarEventShareFeatureService::emitMembership` — so the functions
themselves had to become coroutines, and their declarations (both `.hxx:53`)
and their four call sites (`create` and `remove`, both already `Task`s, one
`co_await` each) changed with them. The real edit set is 11 files: the contract,
the sink's `.hxx`/`.cc`, the controller test's fake, the four
`{project-member,calendar-event-share}-feature-service.{hxx,cc}` files, and the
three feature services whose only edit in this unit is the `co_await` on an emit
that stopped being `void` (`calendar-event-feature-service.cc:30`,
`project-feature-service.cc:29`, `project-task-feature-service.cc:46`). The
first reading of the blast radius missed those three too: an emit that returns a
`Task` changes every caller that does not await it, so a `Task`-returning emit
is never a one-file edit.

The verification also caught a second thing the first reading had missed: both
broken call sites passed a braced temporary (`{input.ownerId, row.userId}`) into
a coroutine, which rule 18 forbids ("Do not pass temporaries to coroutines that
store references"). Both now build a named `std::vector<int64_t> recipients`
first.

Two flags on the contract change, recorded rather than silently taken:

- `[[nodiscard]]` is on the two pure virtuals and on all three overrides, so
  every one of the funnel's 14 call sites must be awaited — 7 emits (8 counting
  the controller test's fake, which delegates its `emitUser` to `emitUsers`) and
  7 audit diffs. None of them discards the result today (none ever did — the old
  `void` return simply had nothing to discard), so nothing was papered over with
  a `static_cast<void>`.
- This is the first shared-contract change of step 3a that touches a header
  another service compiles. `services/notification` implements `AuditSink`
  only, so its two doubles were untouched — verified by grep, not assumed — but
  the header change is why the full-tree build, and not only
  `--only productivity`, is this unit's gate.

## The schema test the new index breaks

`services/productivity/tests/unit/productivity-schema-test.cc:49-52` counted
indexes with an unfiltered `name LIKE 'idx_%'` and `CHECK(indexes.size() == 13)`,
so `idx_change_outbox_status` would have made it 14. Verification confirmed the
break empirically (`sqlite3 :memory: ".read …/schema.sql"` returns exactly the
13) and confirmed that no other assertion in the tree couples to it: the table
counts are `name IN (…)` lists of the 7 productivity tables, and
`productivity-migration-test.cc`'s two `tables.size() == 7` assertions read a
hardcoded list rather than `sqlite_master`.

The filter is narrowed to the domain's own prefixes
(`idx_project%`/`idx_calendar_event%`/`idx_reminder%`, still 13), which is
camera's precedent — `camera-schema-test.cc:49-52` already narrows to
`idx_camera%`/`idx_zone%` and therefore absorbed camera's own outbox index in
3a-2b without an edit. Notification had no index assertion at all. The count
stays 13 because the assertion is about the domain's indexes and the outbox's
index is not one of them; the outbox table's presence is pinned by
`productivity-change-outbox-sink-test`, which inserts into it and reads back.

The schema file's own header prose said "The 7 productivity tables plus their 13
indexes"; it now names `change_outbox` as this service's own addition. (Camera's
header carries no count, which is why 3a-2b had no prose to fix.)

## Evidence

| Command | Result |
|---|---|
| `./scripts/build-all.sh dev --only productivity` | exit 0, **0 warnings**, **32/32 tests passed**, including the two new suites |
| `ARGUS_NATS_URL=nats://127.0.0.1:4222 ./productivity-change-outbox-sink-test` | **62/62 assertions**, including the broker round trip (the payload read back off the stream byte-identical), the stream the sink creates itself, the 100-emit backlog under its 3 s bound, and the stranded-row leg |
| `./scripts/build-all.sh dev` (full, 17 projects), first run | **exit 1**, on the clang-tidy ratchet only — `performance-move-const-arg` 15 vs baseline 14 and `performance-unnecessary-value-param` 50 vs 49, both from the new suite's `projectRow` helper. Every project's build and tests were green, and `check-deps.sh` reported 63 declarations, 474 edges, 0 forbidden, 0 cycles, 0 unresolved. 490 TUs scanned |
| `scripts/lib/tidy_scan.py` (the gate's own tidy step, re-run after the helper took `const std::string&`) | **exit 0** — 490 TUs, **3139 findings over 45 checks**, baseline 3141, no check above it |
| `./scripts/build-all.sh dev` (full, 17 projects), final run | **exit 0** — 17 projects, **386 tests passed, 0 failed**, no first-party compile warnings (the only `Warning:` lines are third-party configure notes: GGML's ccache probe and a third-party tree modifying `CMAKE_CXX_STANDARD`). `check-deps.sh`: 63 declarations, 474 edges, 0 forbidden, 0 cycles, 0 unresolved. `check-tidy.sh`: 490 TUs, **3139 findings over 45 checks**, baseline 3141, no check above it |

## What this unit deliberately does not do

- **No dedup on the emit leg**, as above: the fan-out reads `users` and the
  client's row is the client's row.
- **No `change_outbox` in the migration tool.**
  `tools/migrate-productivity/productivity-migration.cc` neither requires nor
  copies it, because its `kProductivityTables` list is the 7 legacy tables the
  tool moves out of `argus.db` — the outbox is empty at migration time and the
  service's own boot schema creates it. Camera and notification left theirs the
  same way. Recorded as an observation, not a defect: the table is created by
  the same `schema.sql` every other table is.
- **No new stream in the deploy stack.** The sink ensures its own, self-healing,
  and `argus-deploy` mounts no stream config for any subject.
- **Nothing in `services/sync`.** The consuming side reads the wildcard over
  core NATS; a durable consumer for change subjects is S4 at step level, a
  decision about all four producers at once.
- **No reminder/reminder_detail emit path.** They are sync-read-only (Ruling
  AL) and produce no changes here.
- **No batched settlement in the drain.** A pass reads 64 pending rows in one
  statement and then settles each with its own `markSent`/`recordAttempt`
  update, on the connection pool the service configures with
  `number_of_connections = 1`. Rule 21 would prefer one multi-row statement, but
  the two settlements are not the same write — `markSent` is a status-guarded
  CAS over a set of ids while `recordAttempt` carries a per-row count — so a
  batch would either drop the attempt counter or need a second per-row statement
  anyway. Left as the precedent has it, and noted as a step-level efficiency
  item for all four producers rather than diverging here.

## The review, and what it changed

The unit was reviewed by a fan-out over six dimensions (durability, the
contract's blast radius, threading, test provability, documentation truth, house
conventions); every finding was then put to three independent lenses told to
refute it, and a majority refutation killed it. 57 agents, 11 findings
survived. What they changed:

- **Rule 18, applied to the code.** The four new `emitMembership` call sites
  bound a braced temporary directly to the coroutine's `const EmitMembershipInput&`,
  the same shape this unit had already fixed one level down for the recipients
  vector. Both services now build a named `membershipInput` first — and, because
  rule 19 makes modernising what a change touches part of the change, so do the
  four pre-existing `emitParent({...})` call sites at the same statements.
- **A tautological assertion removed.** `CHECK(emitted.attempts == 0)` in the
  first block of the new suite could not fail on any change to the code under
  test: it read the literal `0` the INSERT writes, and no worker existed in that
  block to change it. The two attempt assertions that survive it follow a
  `Conflict` disposition and a started-but-bus-less drain, which are paths that
  can touch the counter.
- **Three documentation corrections.** This report said the edit set was 9 files
  and enumerated 8, and it said "9 call sites" where the funnel has 14 — both
  fixed above. `packages/lib/nats/AGENTS.md` ruled that "a caller that must know
  the message landed asks for a reply subject, it does not read the publish
  result as an acknowledgement", which stopped being true when 3a-2b added
  `publishWithMsgId`: all three outboxes settle a row on exactly that return
  value. The bullet now separates the core publish from the JetStream one.
- **Two items recorded rather than carried silently.** S1b and the sharpened S8
  above; the reviewers' dissent on both is on the record in the workflow
  journal, and it is right that neither is a defect this unit invented. The
  difference the review makes is that the report now says what the windows are
  instead of naming them and moving on.
- **The gate caught what the review did not.** The first full-tree run failed on
  the clang-tidy ratchet, not on a compile: the new suite's `projectRow` helper
  took its `name` by value and moved it into a `Json::Value`, which binds a
  const reference — one `performance-move-const-arg` and one
  `performance-unnecessary-value-param` finding above baseline. The helper now
  takes `const std::string&`, and the ratchet is back at 3141.

## The items this unit leaves at step level

- **S1 (step-wide): the enqueue is not in the domain write's transaction.** The
  row mutation commits, then the change row is written; a crash in between
  loses that one change. Unchanged from 3a-2b/3a-2c, and now measured for three
  of the four producers.
- **S1b (step-wide): the enqueue's own give-up loses the change.** The retry is
  bounded (`kEnqueueAttempts = 3`, 25 ms apart) and the failure is loud, but a
  write that fails all three times — `SQLITE_FULL`, `SQLITE_IOERR`, a corrupt
  page — leaves the row absent from `change_outbox` for ever: the mutation has
  committed, the handler answers `ok`, and no later boot, drain pass or gRPC
  pull can discover a change that was never recorded anywhere. A lock
  (`SQLITE_BUSY` past the 5000 ms busy timeout) needs sustained contention for
  ~15 s to reach it, so the fail-fast storage errors are the real window. This is
  the only sink path that can lose a real change in normal operation: the other
  two early returns are unreachable from the 7 production emit sites (every one
  builds `info` from a row's `toJson()` or an id-bearing tombstone, and the DTO
  bounds are 160–2000 characters against a 256 KiB ceiling). Recorded, not
  fixed: the repair is the same transaction the plan defers, and a longer retry
  budget would trade this window for a longer block on the event loop.
- **S4/S5/S8 (step-wide), as 3a-2c recorded them:** no durable consumer for any
  change subject; no purge of `sent` outbox rows; the drain worker is joined only
  by the sink's destructor, which runs after `drogon::app().run()` has returned.
  S8 is sharper than 3a-2c stated it: `quit()` resets the database client manager
  before the IO loops stop, and `DbService::client()` reaches it through an
  unguarded `dbClientManagerPtr_->getDbClient(...)`, so a tick landing in that
  window is a null dereference on the ordinary SIGTERM/`docker stop` path, not a
  late-but-valid read. The window opens at that reset and stays open until the
  sink's destructor — `quit()` resets the manager inside the loop lambda and then
  waits on the IO pool, while the join only happens after `run()` returns and
  `main` has shut the gRPC server down — so it is the whole of the remaining
  shutdown and not one retry cadence. The same exposure camera and notification
  carry, and closing it means stopping the worker as part of Drogon's shutdown
  for all four producers at once.
- **S7 (step-wide): rule 20's statement-level comments in test suites.** The two
  new suites carry the same in-function comments every suite in the tree does;
  the sweep is step-level.
- **The `[[nodiscard]]` contract change is a wire-adjacent change**: it alters no
  payload, no subject and no operation, but it changes a header two services
  compile. If a later producer implements `UserChangeSink` synchronously it must
  now return a `Task`.
