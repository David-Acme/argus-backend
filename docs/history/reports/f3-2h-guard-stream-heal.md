# Sub-step 3a-2h — guard's encounter drain heals its own stream

Scope: `services/guard` — the two defects 3a-2c measured and 3a-2g's review
re-confirmed and deferred to "its own unit"
(`f3-2g-camera-drain-parity.md:154-164` and `:224`). Guard is not one of the
four change producers (3a-2a's decision, `f3-2a-producer-outbox-decision.md`):
its encounter outbox is a different table on a different stream, drained
inline rather than by a worker. What it shares with the producers is the
publish leg, and that is where the two defects are.

This closes the last service of 3a step 2 that carries the checked drain's
publish shape. The step's remaining items (S5 retention, S6 baseline, S7 the
comment sweep, S8 the shutdown leg, the burst-cardinality precondition, the
object-event drain asymmetry, S1/S1b and S4) are separate units and are not
touched here.

## Pre-state, measured

| Site | At `HEAD` |
|---|---|
| `guard-service.cc:401-450` | `trySubscribe()` does the `ARGUS_GUARD` ensure **and** the durable subscribe on `config_.eventStream` in one function — one function, two streams — and sets `subscribed_ = true` at `:443` |
| `guard-service.cc:805-809` | the 5 s retry is gated `if (!guard.alive() \|\| subscribed_) return;`. `subscribed_` (declared `guard-service.hxx:461`) is **written only at `:443` and read only at `:807`** — grepped over `src/`. It is never cleared |
| `guard-service.cc:406-411` | the ensure itself, with `kGuardStreamRetentionNs`/`kGuardStreamDuplicatesNs` as locals of `trySubscribe()`. It is **the only `ensureStream` in the service** — grepped over `src/` and `tests/`: the other two hits are the DLQ suites creating their own throwaway streams |
| `guard-service.cc:974-991` | `flushEncounterOutbox()`: `co_await repository_.markEncounterSent(row.eventId, now);` **discards the `Task<bool>`** at `:986`. A refused publish records an attempt and continues to the next row — guard's loop has **no head-of-line stall** |
| `guard-query.hxx:156-158` | `PENDING_ENCOUNTER_OUTBOX` is already `ORDER BY created_at ASC LIMIT 100` in one statement, so **no batching change is owed** — unlike camera, whose `nextPending()` read one row per pass and was the subject of 3a-2g |
| `guard-query.hxx:160-162` | `MARK_ENCOUNTER_SENT` is `UPDATE ... WHERE event_id = ?` with **no `AND status = 'pending'`**, unlike the sibling outboxes (`services/camera/src/shared/repositories/change-outbox/change-outbox-query.hxx:20-22`, the same guard in the other three) |
| `guard-service.cc:944-971`, `:931` | the drain is invoked from two places: the close path (`publishEncounterClosed` → `async_run`) and the encounter sweep, which runs `flushEncounterOutbox()` then `checkTamperSweep()`. Both wrap it in `try`/`catch`, so a thrown database error is logged and the rows left in the pass stay pending. There is no worker thread and no progress/retry cadence |
| `packages/lib/nats/src/nats/nats-bus.cc:483-488` | `publishWithMsgId` logs one `LOG_WARN` naming subject and msgId for **every** refused publish, undamped. The pre-state feed was therefore never silent: a missing stream emitted one bus line per pending row per pass — it was guard's own layer that said nothing |
| `nats-bus.cc:493-529`, `:562-566`, `:570-577` | `ensureStream` falls through to `reconcileStream` on `JSStreamNameExistErr`; reconcile **refuses** a stream that cannot be inspected and one "carrying different subjects; refusing to repurpose it". `streamInfo` answers `std::nullopt` for a stream that is not there — `StreamStatus.exists` is only ever true |
| `database/schema.sql:205-216` | `guard_encounter_outbox` has an index on `(status, created_at ASC)` and **no CHECK on `status`**, so rule 1's enum rule does not reach this table — unlike `guard_action_outbox`'s 8-value CHECK. Nothing in `src/` **deletes** a row of this table (only `INSERT OR IGNORE`/`SELECT`/`UPDATE`), and the event id is a non-empty `encounter:<id>:<grade>:<at>` (`guard-query.hxx:575-581`) |
| `tests/unit/guard-saga-test.cc:592-598` | the only test that touches the table asserts the row is enqueued `pending` with the right event id and payload. **Nothing in the tree ever drives the drain**, so the publish leg had no coverage at all |
| `guard-service.hxx:94-95` | `guardStream` defaults to the literal `ARGUS_GUARD` and is **not wired from `config.toml`**: `main.cc:245` and `:247` resolve only `guard.consumer_durable` and `guard.event_stream`, and `config.toml.example` carries no other NATS key. A live test can point it anywhere |
| sibling cadence | `services/camera/src/camera/nats-camera-change-sink.cc:144` ensures **per row** inside `flush()`, `:148` checks `markSent`, `:181-183` **breaks** the pass on a failed row, `:160-164` damps its per-row refusal log to the first attempt and every 100th (`kStuckLogEvery`, `:25`), and the loop waits `kProgressMs` (50 ms, `:29`) after progress and `config_.retryMs` (500 ms default) after a stall |

## The defect, and what it is not

**The latch was guard's version of the defect 3a-2g fixed in camera.** The
`ARGUS_GUARD` stream was created by `trySubscribe()`'s first success and never
again, because that function is gated off by the very flag it sets. `NatsBus`
offers no stream-delete and `onReconnected` merely logs (`nats-bus.cc:82-91`),
so a stream deleted broker-side left every `publishWithMsgId` on
`argus.guard.v1.encounter_closed` refused for the life of the process:
`recordEncounterAttempt` counted, the row stayed pending, and guard's own layer
logged nothing about it.

**What heals is the deletion, not a repurposed stream.** When the stream exists
but does not declare `argus.guard.v1.>`, `reconcileStream` refuses it on
purpose, so `ensureGuardStream()` returns false on every pass too — the latch
can never turn true, and the feed stays down until an operator redeclares the
stream. That refusal is deliberate package behaviour (3a-2a's decision), not
something this unit changes: the re-arm heals a deleted stream and reports the
rest.

**The settlement half is an assertion, not a repair.** The second defect 3a-2c
and 3a-2g deferred was the discarded `Task<bool>`. Measured, that result cannot
be false for a row that exists: `MARK_ENCOUNTER_SENT` has no status guard, Drogon
reports `sqlite3_changes()` (which counts a row the statement matched even when
the values are unchanged), and nothing deletes an outbox row — so
`markEncounterSent` returns false exactly when the row is not there, which the
drain has just read it from. The check is kept because it is the family's shape
and it is the honest thing for a future worker (S8) that could settle
concurrently, but this unit does **not** claim to repair a reachable defect with
it, and the report's earlier framing of it as one of the two defects was wrong.
The reachable failure on this path is a *thrown* write error, and both call
sites already catch it.

## The six changes

**The ensure is extracted.** `ensureGuardStream()` (private, `[[nodiscard]]`)
holds the `ARGUS_GUARD` declaration and its two retention constants;
`trySubscribe()` calls it instead of inlining it.

**The drain re-arms instead of trusting the boot.** A new
`std::atomic<bool> encounterStreamReady_` is checked before the pass's first
publish, set from `ensureGuardStream()`, and cleared on a refused publish — so
the next drain reconciles the stream again. An ensure is create-or-reconcile
and therefore idempotent (`nats-bus.hxx:67-68`), which is what makes re-running
it the right answer rather than latching. The ensure sits **before** the read
rather than inside the loop as the siblings' per-row call does: one pass has
one broker state, and the pass that *discovers* a missing stream still refuses
its rows and heals the next one. The two differ in single-pass progress, not in
outcome — every row converges, and guard has no worker to retry within the
pass, so the per-row call would buy at most one sweep.

**The settlement result is read.** `markEncounterSent`'s boolean is checked and
a false logs the family's line (identical to camera's). The loop does **not**
break, because guard's drain is not a worker: breaking would delay every later
row by up to a full sweep. See above for what the boolean can and cannot mean.

**A refusal is announced once per pass.** `NatsBus` already logs every refused
publish, so the new line is not the feed's only signal — it is guard's own
bounded summary of the pass, on top of the bus's per-row line. The siblings
instead damp a per-row line to the first attempt and every 100th
(`nats-camera-change-sink.cc:158-165`) because their worker ticks every 50–500 ms
(`:184-185`), which for one stuck head row is up to 1 200 lines a minute; guard's
retry unit is the pass itself — one per encounter close and one per 60 s sweep.

**`flushEncounterOutbox()` becomes public.** It is the seam the new suite
drives, and the precedent is `checkTamperSweep`, public "driven by the
encounter sweep timer" because tests drive it too. Nothing else about its
visibility changes: it is still called from the close path and the sweep.

**The stream and its subject family are overridable.** `guardSubjectFilter` and
`guardEncounterSubject` join `guardStream`, empty meaning production. They are
needed because the new suite cannot use the production pair: a stream whose
subjects another stream already claims is refused rather than created
(`JSStreamSubjectOverlapErr = 10065`), and any broker that has run guard already
carries a stream for `argus.guard.v1.>`, so a per-run *name* alone could never
be created while the production *subjects* were used — and the publish would
then succeed through the leftover stream, making pre- and post-state
indistinguishable. `guardStream`'s own default now reads
`nats_subject::kGuardStream` instead of repeating the literal, so the name has
one home.

## What this unit does not change, and why

- **The `subscribed_` latch itself.** Un-gating `trySubscribe()` would re-run
  the *durable subscribe* as well as the ensure on every 5 s tick, which is a
  different concern and a different fix: a durable consumer that was lost is
  undetectable with the API `NatsBus` exposes (no callback, no query — measured
  in 3a-2g and restated as **S4**). This unit un-gates the stream, not the
  subscription.
- **The drain's `LIMIT 100`.** It is already one bounded statement, so the
  batching half of 3a-2g has no counterpart here. A backlog above 100 drains
  100 per pass; recorded, not changed.
- **`MARK_ENCOUNTER_SENT`'s missing status guard.** Adding it would make the
  false case reachable only under a concurrent settlement the drain cannot have
  on the loop, and the family's message ("could not be marked sent; it stays
  pending") would then be wrong for that case. Recorded with the assertion
  framing above.
- **The missing `CHECK` on `guard_encounter_outbox.status`.** Adding one to a
  live table is a migration with a compatibility question (existing rows), not
  a publish-leg fix. Recorded.
- **`subscribeAdvisories()`'s hardcoded `ARGUS_CAMERA`** (`guard-service.cc:473`)
  rather than `config_.eventStream`, and **`guardStream`'s absence from
  `config.toml.example`**. Both are pre-existing and neither is on the publish
  leg this unit fixes. Recorded.
- **`publishHeartbeat()`'s core `publish`.** It carries no `msgId` and no
  durability by design — a heartbeat is superseded by the next one, so a lost
  heartbeat is not a lost change. Untouched.
- **The dlq-service live suite's production ensure.** `guard-dlq-service-live-test`
  calls `start()`, so it has always ensured the production `ARGUS_GUARD` even
  though its source stream is isolated. Making its guard stream isolated too
  would need a third subject family (its guard stream must not overlap its own
  `ARGUS_GUARD_DLQTEST`), which is not what this unit is about. CONTEXT.md now
  says so instead of claiming the suite is fully isolated.

## Evidence

| Gate | Result |
|---|---|
| `./scripts/build-all.sh dev --only guard` | **EXIT=0**, 53/53 tests, 0 warnings (run twice: once on the change, once on the restored tree after the falsification) |
| the new suite with `ARGUS_NATS_URL=nats://127.0.0.1:4222` | **18/18 assertions, SUCCESS** — twice on distinct per-run families |
| the same binary with `ARGUS_NATS_URL` unset | **1 case, 0 assertions, SUCCESS** (the documented skip) |
| the new suite against the pre-state | **falsified**: with HEAD's drain body restored in place the suite fails at the helper's `REQUIRE(status.has_value())` (`guard-encounter-drain-live-test.cc:74`) — 10 of 11 assertions pass before the fatal one, because the pre-state publishes on the production subject through the broker's leftover `ARGUS_GUARD` stream and settles the row |
| `guard-dlq-service-live-test` (the `trySubscribe` regression check) | **9/9 assertions, SUCCESS** |
| the tree after the falsification | restored byte for byte (`md5 6c96f1465592ce51401ee53c35640eea`), then rebuilt and re-run: 18/18 |
| `scripts/check-deps.sh` | 64 declarations, **481 edges, 0 forbidden, 0 cycles, 0 unresolved**, 49 deferred to phase 3 |
| the tidy gate, first full run | **failed**: `bugprone-unchecked-optional-access` 228 findings against the 224 baseline, over the suite's four `created->` accesses after a `REQUIRE(has_value())` — a shape the check does not recognise. Fixed by adopting the sibling live suites' `streamStatus()` helper (`value_or`), not by a new idiom, and by dropping the vacuous `CHECK(created.exists)` |
| the tidy gate, second full run | **failed again**, on the next check: `modernize-use-ranges` 62 against 61, over the suite's `std::find(begin, end, …) != end()` subject check. Fixed by spelling it `std::ranges::find(subjects, …) != subjects.end()`, which rule 19 prefers anyway. Both rises were the new test translation unit and nothing else — no check moved in any other file |
| `./scripts/build-all.sh dev` (full, with the tidy gate), third run | **clean**: exit 0, 17 projects, **401 tests, 0 failures, 0 compiler warnings**; `check-tidy` **495 TUs / 3140 findings over 45 checks against the 3141 baseline, no check risen** (one sits below it). The two earlier runs of the same command differed only in the two rises above, both from the new test translation unit |

The falsification is precise about what the suite pins: the **heal** — the pass
that settles a row is also the pass that creates the stream, so an absent
per-run stream is the assertion that fails when the ensure does not run. It does
**not** pin the settlement change: no assertion can fail if
`markEncounterSent`'s result is discarded again, because the boolean cannot be
false for a row the pass just read as pending.

## The review, and how its findings were applied

Five dimensions over the diff, each candidate finding refuted by two
independent verifiers (47 agents). 13 findings survived, 8 were refuted; every
surviving finding was applied in this unit.

| # | Finding | Disposition |
|---|---|---|
| 1 | Report and CONTEXT claimed the stalled feed was silent and the new count line its only signal; `NatsBus` logs one undamped WARN per refused publish (`nats-bus.cc:483-488`) | **Applied** — both texts now say the bus line exists and guard's is a per-pass summary on top of it |
| 2 | The re-arm heals a deleted stream only; a stream repurposed with different subjects is still refused forever, and the report did not say so | **Applied** — recorded in the defect section and in CONTEXT, with `reconcileStream`'s deliberate refusal named |
| 3 | Report framed the settlement check as repairing a reachable defect; `MARK_ENCOUNTER_SENT` has no status guard, nothing deletes a row, so it cannot be false for a present row | **Applied** — reframed as an assertion in the report and CONTEXT; the SQL is left as it is, with the reason recorded |
| 4 | Report's damping rationale misstated the sibling cadence (50 ms after progress / 500 ms stalled → up to 1 200 lines a minute, not 12 000) | **Applied** — the report cites the constants |
| 5 | The live suite's per-pid stream is never reclaimed, so a later run that reuses that pid fails its own precondition assertion | **Applied** — the run suffix is now the pid plus the steady-clock count, and the absence check stays |
| 6 | CONTEXT's "the stream it creates is left to JetStream retention" is false: `maxAge` bounds messages, not the stream | **Applied** — CONTEXT says the stream is never reclaimed |
| 7 | CONTEXT's "drains every pending row" overstates the pass: `LIMIT 100` | **Applied** — "drains up to 100 pending rows" |
| 8 | The suite cannot pin change 3, and its discrimination rests on the stream-creation requirement | **Applied** — recorded in Evidence, and measured again here |
| 9 | Statement-level comment block inside the test body (rule 20) | **Applied** — removed; the hermeticity rationale now lives in CONTEXT |
| 10 | A second statement-level comment above the `streamInfo` check (rule 20) | **Applied** — removed (the `TEST_CASE` lead comment already carries it) |
| 11 | The four-line comment above the two new `Config` fields is a doc block where the file uses one line (rule 20) | **Applied** — one line, rationale in CONTEXT |
| 12 | `guardStream`'s literal duplicates `nats_subject::kGuardStream`, a drift hazard for argus-llm's consumer | **Applied** — the default reads the constant (a refuter had called it out of scope; the drift is real and the edit is two lines) |
| 13 | The dlq-service suite is not fully isolated: `start()` still ensures the production `ARGUS_GUARD` | **Applied** — CONTEXT states it instead of claiming otherwise |

Refuted, and why: the per-row versus once-per-pass ensure (**the two differ in
single-pass progress only; both converge, and the report now says so rather than
claiming equivalence** — its factual core was accurate, so the wording was
corrected); the one-sided override of the two new fields (unreachable: nothing
but the suite sets them); `kGuardSubjectFilter` having one reader (that header
is not a 2+-reader registry — three of its neighbours have one
non-header reader each); the thirteenth `AppRunner` copy (rule 23's
duplicated-copy clause needs a replacement, and no shared helper exists — it
stays on the carried list); and the remaining three report-wording challenges
that the corrected text already covers.

## Carried out of this unit

Recorded, not fixed: `guardEncounterSubject`'s invariant of lying inside
`guardSubjectFilter` is unchecked; leftover test streams accumulate on the dev
broker because `NatsBus` has no stream-delete; the 13 `AppRunner` copies under
`services/guard/tests/unit/`; `subscribeAdvisories()`'s hardcoded `ARGUS_CAMERA`;
`guardStream`'s absence from `config.toml.example`; and the step-2 closure items
S5, S6, S7, S8, the burst-cardinality precondition, the object-event drain
asymmetry, S1/S1b and S4.
