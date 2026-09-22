# Sub-step 3a-2b — the camera durable change outbox

The first of the four producer outboxes `architecture-plan.md:832` (Phase 3a,
step 2) asks for, and the first unit to make the plan's durability rule real
for a change feed: the camera domain's four fire-and-forget emit sites now
enqueue a durable row and a drain thread publishes it, marking it sent only
after the JetStream PubAck.

The sub-step's decision report (`f3-2a-producer-outbox-decision.md:5` §5) split
the 36 measured call sites into four units — camera (4), notification (1),
productivity (14), identity (17) — plus 3a-2f only if measurement demands it.
This unit is the camera one, and it is first because camera already carried the
closest pattern: a durable observation outbox
(`services/camera/src/shared/repositories/object-event-outbox/`) whose drain
shape, status-guarded CAS and repository layout are reused here rather than
invented.

**One correction to the parent report's count, measured.** Camera reaches its
sink from **six** sites, not four: the four `emit` calls the decision report
counted, plus two `publishAudit` calls that were already `co_await`ed
(`camera-feature-service.cc:106`, `zone-feature-service.cc:71`) while the
publish inside them was still fire-and-forget — an awaited coroutine whose own
`bus_->publish` return value only fed a `LOG_WARN`. All six are durable after
this unit, so the outbox covers four call sites the decision report's tally did
not name. The 36 was therefore understated by two for camera; the remaining
units' counts (notification 1, productivity 14, identity 17) are unaffected by
this correction and are re-measured when each unit starts.

## Pre-state, measured

At `HEAD`, the camera domain's whole change path was two publishes with nothing
behind them:

| Site | At `HEAD` |
|---|---|
| `CameraFeatureService::emit` (`camera-feature-service.hxx:24`) | `void`; built the `SocketEmitDto` and called `sink->emitModule` |
| `ZoneFeatureService::emit` (`zone-feature-service.cc:6`) | the same, for zones |
| `emit` call sites | `camera-feature-service.cc:67` (`Add`), `:125` (`Delete`), `zone-feature-service.cc:44` (`Add`), `:90` (`Delete`) |
| `publishAudit` call sites | `camera-feature-service.cc:106`, `zone-feature-service.cc:71` — awaited, but the publish inside was unchecked |
| `NatsCameraChangeSink::emitModule` | `(void)table; bus_->publish(nats_subject::kCameraChange, …)` — core NATS, return value discarded |
| `NatsCameraChangeSink::publishAudit` | `bus_->publish(...)` with a `LOG_WARN` on failure, and the change gone |

So a broker outage, a restart or a rejected publish dropped a camera or zone
change with no record that it existed, and the sink did not even read the table
it was emitting for — camera and zone events were indistinguishable above the
payload's `option` key.

Two facts about the consumer side were measured before the design was chosen,
because they decide whether a replayed event is safe:

- `AuditLogService::create` (`services/sync/src/feature/fanout/services/audit-log-service.cc:41`)
  merges a record's same-day diffs through `JsonDiff::compareChanges` (`:51`)
  and re-creates the row, so an audit event delivered twice converges on the
  same `changes` rather than double-counting.
- the journal leg is insert-only, so a replay of an `Add`/`Delete` frame is the
  consumer's existing idempotence question, not a new one this unit creates.

## The design the measurement supports

**The event id names the transition, not the row.** `eventId` takes a
`ChangeOutboxKeyInput` of `{table, recordId, discriminator}`
(`change-outbox-key.hxx:13-24`) and hashes them into
`"camera-change:" + sha256Hex(table | recordId | discriminator).substr(0, 32)`;
the discriminator is **the transition's own payload**, for an emit and for an
audit alike (`nats-camera-change-sink.cc:57`, `:85`). A record-addressed id
would collapse a record's second change into a replay of the first and lose it.

The first revision of this unit discriminated an `Add`/`Delete` by the *sync
operation* instead, on the argument that each happens once per record. Review
measured the trap that leaves: the contract accepts any `SyncOperation`
(`packages/contracts/sync/src/sync/camera-change-sink.hxx:16-17`), so a later
site that reached for `emitModule` on an update would compute the same id as
the first update of that record with a different payload — a `Conflict`,
logged and never dispatched, so the client would keep the first revision for
ever. Payload discrimination is strictly simpler and removes the trap: the
canonical JSON of a frame is deterministic (jsoncpp orders keys and
`json_util::toString` is compact), a genuine redelivery recomputes the same id
and is a `Replay`, and a record that moves again — or returns to a state it
already held, the `a→b→a→b` cycle — is a new event. `CameraFeatureService::remove`
emits the row itself as the delete payload (`camera-feature-service.cc:126`),
so even a replayed delete recomputes its own id.

32 hex digits plus the 14-character prefix is 46 characters, inside the
JetStream `Nats-Msg-Id` header budget; the tree's nearest precedents are
`"notification-delivery:" + id` and guard's `eventId`. The `|` separator is
load-bearing and pinned: `(camera, 17, "add")` and `(camera, 1, "7add")` must
not collide.

**The fingerprint is the payload's hash**
(`change-outbox-key.hxx:36-43`), and the repository turns the plan's §3.5 rule
into three dispositions (`change-outbox-repository.cc:20`): a fresh
`INSERT OR IGNORE` is `Enqueued`, the same id with the same fingerprint is
`Replay` (nothing written), and the same id with a different fingerprint is
`Conflict` — logged with `LOG_ERROR` and **never dispatched**, because the row
already on disk is the transition the id names. Payload discrimination makes
that branch reachable only through a hash collision for this producer; it stays
because the repository is the contract's guard, not this sink's convenience.

**A failed enqueue never fails the mutation, and is retried before it is
given up on.** `enqueue` maps any DB error to `ChangeOutboxDisposition::Failed`
(`change-outbox-repository.cc:35`) instead of letting the exception escape: the
enqueue is awaited on the request's own coroutine, so an unwinding exception
would turn a create that committed into a 500 the client retries, re-creating a
row whose change was never recorded. The sibling object-event outbox already
maps a write failure to a value rather than throwing, so this follows its
precedent. Review measured what the first revision left open — a `Failed` was a
silent, permanent hole, since the one shared connection can refuse the write
while the drain holds it — so the sink now retries the write three times, 25 ms
apart (`nats-camera-change-sink.cc:100-113`), and only then logs that the
change is lost. The residual window is a *permanent* failure (disk full, a
read-only remount), which no in-process retry can close; the transactional fix
is recorded under S1 below.

**The drain marks a row sent only on the PubAck.** `flushOnce`
(`nats-camera-change-sink.cc:120`) publishes with `publishWithMsgId` — which
never degrades to core NATS — and calls `markSent`, whose SQL is the
status-guarded CAS `UPDATE … WHERE event_id = ? AND status = ?`; a failed
publish calls `recordAttempt` and leaves the row pending. The loop wakes on a
50 ms tick while it is draining and on `retryMs` (500 ms in production) when
the outbox is empty or blocked.

**The payload bound is the head-of-line defence.** `nextPending` returns the
oldest pending row (`change-outbox-query.hxx:17-19`), so one row the broker
refuses for ever parks every later change. The only such class reachable from
this domain is an oversized payload (`js_Publish` → `NATS_ERR_MAX_PAYLOAD`, and
`camera.config` is unbounded TEXT behind no DTO `MAX_LENGTH`), so a payload past
256 KB is refused at the door with a `LOG_ERROR` (`nats-camera-change-sink.cc:88`)
rather than written. The drain itself retries for ever by design: a broker
outage fails every row equally, so an attempt cap would drop real changes. The
first refusal is logged, and every hundredth after it
(`nats-camera-change-sink.cc:131-138`), so an operator sees the feed stop
instead of reading about it four minutes later.

**The change sink does not ensure its stream.** `NatsBus::ensureStream` refuses
to repurpose a stream that carries different subjects, so a change sink asking
for `{"argus.camera.v1.change"}` alone would hit that refusal and could cost
object-event capture its stream. The camera change subject lives in the
`ARGUS_CAMERA` stream the object-event sink creates at boot and heals in its own
flush loop, so the change sink relies on it: a publish that cannot land leaves
the row pending and the next tick retries, which is the outbox's whole point.

## What changed

| File | Change |
|---|---|
| `packages/contracts/sync/src/sync/camera-change-sink.hxx` | `emitModule` becomes `[[nodiscard]] drogon::Task<void>`: a durable enqueue is a DB write and cannot be synchronous on the event loop |
| `services/camera/src/camera/nats-camera-change-sink.{hxx,cc}` | rewritten around `ChangeOutboxRepository`, `reconcile()`, `flushLoop`/`flushOnce`, the payload bound and a `Config{retryMs, publishSubject}` for tests |
| `services/camera/src/feature/api/camera/services/camera-feature-service.{hxx,cc}` | `emit` becomes `[[nodiscard]] drogon::Task<void>`; its two call sites are `co_await`ed |
| `services/camera/src/feature/api/zone/services/zone-feature-service.{hxx,cc}` | the same |
| `services/camera/src/main.cc` | the beginning advice captures the sink and starts it with `reconcile()` |
| `services/camera/src/operator/nats-object-event-sink.cc` | the copied wake predicate dropped, so `notify_all` can wake the drain |
| `services/camera/database/schema.sql` | `change_outbox` plus `idx_change_outbox_status` |
| `services/camera/src/shared/repositories/change-outbox/` | the module: status enum, key helpers, query constants and dispositions, the coroutine repository |
| `services/camera/CMakeLists.txt` | the module's `add_subdirectory`, its `camera-core` link, and the two suites |
| `services/camera/tests/unit/change-outbox{,-sink}-test.cc` | the unit's suites |
| `services/camera/CONTEXT.md` | the change feed's design "why" |
| `docs/architecture/data-storage.md` | the change-feed producer outbox named in the durable-delivery catalogue, the two guard tables' direction corrected, and the id rule stated as it now is |
| `packages/lib/nats/AGENTS.md` | the consumer list the sync retirement invalidated |
| `packages/contracts/sync/AGENTS.md` | the camera change sink's audit input named correctly |

**The two suites link the module by name.** Both test targets first compiled
the repository's `.cc` directly — the shape the sibling
`object-event-outbox-test` had used — which is the one thing rule 25 forbids
("NO consumer lists `.cc` files"; explicit source lists stay inside the
module's own `CMakeLists`). `argus_module` propagates `DEPENDS` and `INCLUDES`
as `PUBLIC` (`cmake/argus-module.cmake:95-98`), so
`target_link_libraries(change-outbox-test PRIVATE argus::camera-change-outbox
doctest::doctest)` brings the include root and `lib::sqlite`/`lib::text`
transitively and the target drops its source list, its
`target_include_directories` and its two explicit links. The sibling carried
the same deviation, so it was fixed in the same change rather than left as the
precedent that produced it.

Two more test targets in this file duplicate sources the same way —
`camera-talk-cutover-test` (`CMakeLists.txt:354`) and `camera-action-rpc-test`
(`:404`) each re-list 17–22 files from `camera-core`, which is a much larger
copy than the two suites above and is not this unit's to unwind. They are named
here so the later build-hygiene pass has the measurement. The suites' other
duplication is the `AppRunner` / `waitForBoot` scaffold, and `grep -rln 'class
AppRunner'` over the tree measures it at **42 copies** in 12 units — 12 in
`services/guard`, 8 in `services/camera` (this unit's two included), 7 in
`services/notification`, 3 each in `llm` and `sync`, 2 each in `productivity`
and `vlm`, and one each in `gateway`, `lib/cert`, `identity`, `stt` and `tts`.
It is carried for the same reason — extracting it means a shared test-support
header plus 42 include paths, a change that belongs with the pass above rather
than inside a durability unit. It is named here because a count that large is
evidence the pattern wants a home, not evidence that one more copy is fine.

**Two doc errors were corrected because this unit made them visible, both
measured rather than assumed.** `docs/architecture/data-storage.md` catalogues
the durable-delivery tables and did not name the table this unit adds; the same
bullet described `guard_observation_inbox` / `guard_action_outbox` /
`guard_encounter_outbox` as "the producer and consumer sides of the
camera→guard leg", but `guard_action_outbox` is keyed by `command_id` and
carries commands in the guard→camera direction
(`services/guard/database/schema.sql:183`), and `guard_encounter_outbox` is the
guard's own `encounter_closed` producer leg — only the observation inbox is
that leg's consumer side. `packages/lib/nats/AGENTS.md` listed the package's
consumers as "identity, memory, socket, sync and seven services"; `socket` was
retired into `services/sync` by 3a-1d. Read off the link sites, the eleven are
`contracts/sync`, `identity`, `memory` and eight services (camera, gateway,
guard, llm, notification, productivity, sync, tunnel) — the count was right and
only the names were stale. Review added two more of the same kind: the storage
doc's id rule still described the pre-state discriminator this unit replaced,
and `packages/contracts/sync/AGENTS.md` named a `CameraAuditInput` that the
header it documents does not declare (it takes `ModuleAuditInput`,
`camera-change-sink.hxx:20`).

## Review findings applied

The unit's code review returned six findings; all six are applied, none
carried.

| # | Finding | What changed |
|---|---|---|
| F1 | A `Failed` enqueue was a silent, permanent loss of an already-committed change | The bounded retry above, plus the loud final log |
| F2 | An operation-derived discriminator would silently withhold every change after the first for any future non-`Add`/`Delete` emit | The discriminator is the payload for every operation; the key test pins the two-departures and return-to-a-state cases |
| F3 | `body.obj["id"].asInt64()` was unvalidated: it could throw out of the coroutine (500 after a committed write) or collapse records onto id 0 | `emitModule` refuses a non-integral id with a `LOG_ERROR` instead of throwing or naming id 0 |
| F4 | The live leg could not pass twice inside the stream's duplicate window | Per-run stream, subject and payload, `deliverAll` on the consumer (below) |
| F5 | One unpublishable row parked the feed with ~250 s between log lines | The first refusal is logged, then every hundredth |
| F6 | The report and two docs described a revision that no longer existed | This report, plus the two doc corrections above |

F2 and F4 are the two the review found by reading rather than by running, and
both were then reproduced here before being fixed. F4's reproduction is the
sharper lesson and is recorded under the tests below.

## What the tests prove

`change-outbox-test.cc` pins the key rules — determinism, table/record/
discriminator separation, the `|` separator, the prefix, the 46-character
MsgId budget, payload discrimination (including the `a→b→a→b` pair),
fingerprint equality and inequality — and drives the repository against the
real schema: the incomplete input is `Failed`, two enqueues are `Enqueued`, the
oldest comes back first, the same id with the same fingerprint is `Replay`, the
same id with a different fingerprint is `Conflict` **with the stored payload
and `attempts` untouched**, `markSent` is a one-way CAS, `recordAttempt` counts
without settling and counts again, and two rows sharing a millisecond leave by
insert order.

`change-outbox-sink-test.cc` links `camera-core` and drives the sink itself:
`emitModule` lands the row whose id is `change_outbox_key::eventId` over the
body's own canonical JSON and whose payload is byte-identical to
`json_util::toString(body.toJson())` (the wire does not change); a replay after
`markSent` writes nothing. `publishAudit` writes the flat diff, and the
`a→b→a→b` cycle produces a *second* row with a different id — the property the
pre-state discriminator would have broken — while a no-op audit writes nothing.
A payload past the bound is refused and writes nothing. With no bus at all the
row waits with `attempts == 0` instead of being dropped, and with a bus whose
subject cannot be published (a wildcard) the row stays pending with a counted
attempt and the change queued behind it waits rather than overtaking it.
26 assertions without a broker, 34 with one.

Its live leg is opt-in through `ARGUS_NATS_URL`, and it was **run** here —
three consecutive runs, all green:

```
$ ARGUS_NATS_URL=nats://127.0.0.1:4222 ctest -R change-outbox
100% tests passed, 0 tests failed out of 2   (×3)
[change-outbox-test]      42 assertions | 42 passed
[change-outbox-sink-test] 34 assertions | 34 passed
```

**The leg's own history is the unit's clearest measurement.** Draining to empty
is not proof the message was *stored* — a false `markSent` looks the same — so
the leg reads the event back off a durable subscription and compares it byte
for byte with the published frame. That assertion failed on its first three
live runs, and reading the broker explained it rather than the test: the stream
held the message and reported **zero consumers**, because the frame carried a
fixed event id, the first run's copy was already in the stream inside its
120 s duplicate window, so the republish was deduplicated — accepted, marked
sent, nothing new to deliver — while the fresh consumer, being new-only, never
saw the earlier copy. The dedup the design leans on had made the *test* blind.
A stream, a subject and a payload per run, plus `deliverAll` to close the
window between asking for a consumer and the broker creating it, makes the leg
deterministic; that is the shape it has now.

Two further measurements from the same runs: `NatsBus::drain()` closes the
connection (`nats-bus.cc:694-712`, `connected_ = false`), so a test that drains
the bus and then expects a later publish to fail is measuring its own teardown
— the failure path is exercised with an unpublishable subject instead; and the
sink's scoped lifetime matters, because a live sink left running publishes the
rows of the next block on its own subject.

The ordering tiebreaker was measured rather than assumed: the drain's
`ORDER BY created_at ASC, rowid ASC` (`change-outbox-query.hxx:18`) makes two
same-millisecond changes leave in the order they were enqueued, which matters
because an `Add` and a `Delete` of one record published out of order would
resurrect a deleted row in the client. `EXPLAIN QUERY PLAN` over the real
schema returns a plain index search — `SEARCH change_outbox USING INDEX
idx_change_outbox_status (status=?)`, no temp B-tree — so the tiebreaker is
free. The sibling outbox keeps `created_at` alone; its events are independent
detections, so it does not need the tiebreaker and was not changed.

## Gate evidence

- `./scripts/build-all.sh dev --only camera`: 48 tests, 0 failed, 0 errors,
  0 warnings — and 48 again with `ARGUS_NATS_URL` set, so every opt-in live leg
  in the service ran too.
- `./scripts/build-all.sh dev` (full, so `check-deps.sh` and `check-tidy.sh`
  run) — the run's own closing lines:
  - `check-deps: 61 declarations, 464 edges, 0 forbidden, 0 cycles, 0
    unresolved, 45 edges deferred to phase 3 (215 third-party mentions over 22
    roots)`.
  - `check-tidy: clang-tidy 22.1.8 (/usr/bin/clang-tidy)`, `484 TUs, 3141
    findings over 45 checks, baseline 3141`, `worst file
    services/guard/src/feature/guard/guard-repository.cc (104 findings)`.
  - `All selected projects built and tested (profile: dev).`
- **The full gate was run three times, and the first two failed it.** Rule 19
  makes the ratchet part of the change, so the failures are recorded rather
  than the passing run alone:
  - **Run 1** — `3145 findings, baseline 3141`, three risen checks:
    `modernize-use-scoped-lock 297/295` (two block-scoped locks in the live
    leg), `performance-unnecessary-value-param 50/49` (the durable handler's
    settlement parameter taken by value) and
    `bugprone-implicit-widening-of-multiplication-result 73/72`
    (`256U * 1024U` widening into `std::size_t`). All four findings were fixed
    rather than baselined: the two locks are `std::scoped_lock`, the handler
    takes `const NatsBus::DurableSettlement&`, and the bound is
    `std::size_t{256} * 1024`.
  - **Run 2** — `3142 findings`, one risen check:
    `modernize-raw-string-literal 21/20`. Running that single check over the
    unit's translation units
    (`clang-tidy -p services/camera/build/dev <TU> --checks=-*,modernize-raw-string-literal`)
    located the one finding at `change-outbox-test.cc:75:59`, an escaped JSON
    literal. It flags only some of the file's escaped literals: a probe
    measured the diagnostic to be length-sensitive — a 13-character
    `"{\"info\":1}"` is not flagged while the 19-character
    `"{\"name\":\"patio\"}"` is — and the sibling suite's escaped literals are
    arguments of doctest macros. The file now writes all of them as raw
    strings (`R"({"name":"patio"})"`), which is how JSON reads best in C++
    anyway, so which ones the check would have counted stops mattering.
  - **Run 3** — the closing lines above: back to the baseline exactly, 3141 of
    3141, no `risen:` line, `tus` unchanged at 484.
- The unit adds no finding of any counted check, and the measurement is the
  per-TU one with the gate's own filter
  (`clang-tidy -p services/camera/build/dev <TU> --checks=<.clang-tidy's list>
  --header-filter=/(packages|services)/.*\.hxx$`): every file this unit wrote
  reports **zero** findings of its own — the four new headers, the
  repository's `.cc`, the sink's `.hxx` and `.cc`, both suites — and so do the
  edited `main.cc`, `camera-feature-service.cc` and `zone-feature-service.cc`.
  The three edited files that do report findings report them on lines this unit
  did not write, each measured against its own hunks:
  `camera-feature-service.hxx` three `modernize-use-nodiscard` (lines 15, 16,
  18; the unit's change is the `emit` declaration below them),
  `zone-feature-service.hxx` the same three on lines 16, 17 and 19 (the unit's
  change is at line 23), and `nats-object-event-sink.cc` eleven (C-style casts,
  `std::lock_guard`, `rfind`, a by-value config — lines 41 to 134, while the
  unit's two edits are a member declaration at line 23 and the wake predicate
  at line 180). Findings the first drafts carried were designed out rather than
  added to the baseline:
  the repository's query constants are `const char*` instead of the sibling's
  `std::string_view` + `.data()`
  (`bugprone-suspicious-stringview-data-usage`), the tests' `Sqlite3Config`
  uses designated initializers, their JSON ids use `static_cast<Json::Int64>`,
  and their optional rows go through a `pendingRow` helper instead of `->` or
  `.value()` after a `REQUIRE(has_value())` — the analyzer follows neither
  doctest macro, and `-Wunused-result` forced the sink's two CAS results to be
  discarded explicitly.
- **The prediction this report first carried was wrong, and the measurement is
  the correction.** It expected `modernize-use-nodiscard` to fall and the
  baseline to be lowered in this change. Neither happened: `tus` rose 481 → 484
  (the repository TU and the two suites; headers are not TUs) and no check's
  count moved in either direction, so `scripts/lib/tidy-baseline.txt` is
  unchanged at 3141. The reason is worth recording, because it is the rule
  rather than the exception: every function this unit marked `[[nodiscard]]`
  returned `void` at `HEAD`, and the check exempts `void` functions, so the
  markings closed the door on *new* findings without retiring an old one. A
  unit only moves that count down when it marks a function that already
  returned a value. Review re-measured the claim and agreed with it
  (`scripts/` is untouched by this unit).

## What this unit deliberately does not do

- **No `health()`/`stats()` on the change sink.** The observation sink has both
  because the operator loop reads them; nothing reads the change sink's. Rule
  24: no structure ahead of its consumer. The review's F5 asked for visibility
  instead of a surface, which is what the first-attempt log gives. If a health
  surface is wanted later it is one query over the same table the sibling's
  `stats()` already counts.
- **No transactional enqueue (the decision report's S1), still deferred**, and
  now with its residue named: the enqueue is awaited immediately after the
  domain write, not inside its transaction, so a crash in the window between
  the two loses that one change, and a permanent DB failure loses it with a
  log. S1 would mean making eleven autocommit repository methods transactional
  across four services, in files Phase 4 relocates — a change of its own, and
  the window is strictly smaller than the fire-and-forget gap it replaces.
- **No second outbox for zones.** Zones travel the camera domain's sink and
  change subject by design (`zone-feature-service.hxx:22`), so they share the
  camera table, id prefix and drain; a `zone_change_outbox` would be a second
  writer for one feed.
- **No stream ensure in the sink**, for the refusal reason above.
- **No change to what a PubAck means.** It is storage, not delivery: the
  subject's only live consumer today is memory's catalog replica, which is
  snapshot-filled at boot by design, and app convergence runs through the sync
  engine's own paging over the camera sync RPC. The producer's durability
  boundary is the right one, and the claims here should not be read as
  end-to-end delivery.
- **The repository's home follows its neighbours, and the measurement is
  recorded rather than the rule bent quietly.** Rule 23 earns `shared/` on a
  second reader; the change outbox has one (`nats-camera-change-sink`), and so
  do two of the five other entries in `services/camera/src/shared/repositories/`
  (`action-command`, `object-event-outbox` — read off the include sites, one
  production reader each). Camera is a pre-migration service (rule 23's own
  paragraph: it keeps `src/camera/`, `src/operator/`, `src/monitor/` and
  `src/objects/` beside `feature/`), so these six repositories sit in `shared/`
  as the layout's existing shape, and the sink that reads this one is a domain
  folder rather than a feature slice — where a single-reader repository belongs
  once camera's features land is the migration's call, not a durability unit's.
  Placing this one elsewhere would have made it the only repository outside
  `shared/repositories/` without settling that question.
