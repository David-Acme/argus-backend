# Sub-step 3a-2g — bringing camera's outbox drains up to the checked shape

Scope: `services/camera` — the two defects the 3a-2c review measured in
camera's sinks and left for "whoever closes 3a step 2" (its **S2**), and the
drain cadence S3 recorded. This is the first piece of step 2's closure: step 2
is now complete on all four producers (3a-2b camera `e111ef9`, 3a-2c
notification `706d7cf`, 3a-2d productivity `4841d38`, 3a-2e identity
`f32bd9c`), and its reports deferred eight items to this moment.

3a-2c stated the rule this unit applies: camera's files were the template the
other three copied, and each copy fixed what it found in the process, so the
checked shape now lives in notification, productivity and identity and
camera's is the one to bring up. Its report also named who does it — "it
belongs to whoever closes 3a step 2" — rather than editing a service from
another producer's unit.

The unit's own review then measured that one of the three changes closed only
half of the stall it claimed to close, and that a second change was owed under
rule 23. The change set below is therefore five changes, not three, and the
review section at the end records what each finding did to it.

## Pre-state, measured

| Site | At `HEAD` |
|---|---|
| `services/camera/src/operator/nats-object-event-sink.cc:140-141` | `if (!streamReady_.load(acquire) && bus_->isConnected()) streamReady_.store(ensureStream(bus_, config_), release);` — the latch is one-way. A **refused publish never clears it** (`:162-163` does `recordAttempt` and returns false), so once the ensure has succeeded the `ARGUS_CAMERA` stream is never reconciled again: a broker-side wipe (stream deleted, store reset, a recreate that dropped subjects) leaves the feed publishing into a stream that is not there, for ever, until a process restart |
| `services/camera/src/camera/nats-camera-change-sink.cc:136-139` | `if (bus_->publishWithMsgId(…)) { static_cast<void>(outbox_.markSent(row->eventId, nowMs())); return true; }` — the settlement result is **discarded**, so a row the broker stored but the database could not mark `sent` reports the pass as progress and is republished on the next 50 ms tick |
| `services/camera/src/camera/nats-camera-change-sink.cc:128-150` | `flushOnce()` reads `outbox_.nextPending()` — **one row per pass**, one statement per row. notification, productivity and identity each read `pendingBatch(kDrainBatch = 64)` in one statement and settle each row of it |
| `services/camera/src/camera/nats-camera-change-sink.{hxx,cc}` | no `ensureStream` of its own: the header says the subject's stream "is the `ARGUS_CAMERA` one the object-event sink creates and heals", and `src/main.cc:170` calls `NatsObjectEventSink::ensureStream(natsBus, {})` once at boot |
| `services/camera/src/shared/repositories/change-outbox/` | `NEXT_PENDING` (one `LIMIT 1`) is kept beside the new `PENDING_BATCH`; its only callers are camera's own two suites |
| `services/guard/src/feature/guard/guard-service.cc` | **carries both defects** (corrected below), and its encounter leg drains **inline** from the close coroutine — no worker, no cadence |
| Statement-level comments in the five sinks | camera's change sink carries two blocks (`:105-107`, `:143-144`), camera's object-event sink one (`:105`), identity's one (`:242-244`), notification's and productivity's none |

**The latch was worse than 3a-2c described, and its reachability is the
measured core of this unit.** The object sink runs its ensure *before* its row
check (`:140-141` precedes `:143`), so the latch is set by the worker's first
pass and cleared only by that same sink's own refused publish. With
`[objects] enabled = false` — the shipped default
(`services/camera/config.toml.example:94-95`), which gates the only producer,
`main.cc:181` → `CameraOperatorService`, while the workers are started
unconditionally at `main.cc:275-277` — an object row is never enqueued, so the
clearing branch is never reached. And camera's change sink had **no ensurer of
its own**: `main.cc:170` is a one-shot static call that never touches the
instance latch. After a stream loss, then, the change feed's every publish is
refused and counted, head-of-line, for ever. `NatsBus::onReconnected` only
logs, so a broker restart does not clear it either.

## The five changes

**The drain batches.** `flushOnce()` becomes `flush(row)` and `flushLoop()`
walks `outbox_.pendingBatch(kDrainBatch)` in one statement, stopping the pass
at the first row that does not settle. `kDrainBatch = 64` and `kProgressMs =
50` are the constants the other three spell, so all four producers now drain
one table with one behaviour at one bound — which is what S3 asked for.

**The settlement is honoured.** A publish that succeeds but cannot be marked
`sent` leaves the row at the head, logs, and reports the pass as no progress —
so the retry cadence applies instead of the 50 ms progress cadence, and the
row is not republished at that speed. Byte for byte the fix 3a-2c landed as A2.

**The ensure re-arms instead of latching.** A refused publish clears
`streamReady_`, so the next pass reconciles the stream again before publishing
— the shape `nats-bus.hxx`'s own contract implies (an ensure is
create-or-reconcile, so re-running it is idempotent).

**Both feeds declare the stream.** `NatsCameraChangeSink` gains its own
`streamReady_` latch and `ensureStream()`, called from `flush()` when the latch
is clear and cleared on a refused publish, so the feed that carries
user-visible changes heals itself on its own next change even with the object
feed idle or disabled. It does not carry its own copy of the object sink's
declaration: both go through a new `shared/services/event-stream/` module.

That sharing is forced, not stylistic. `NatsBus::reconcileStream`
(`packages/lib/nats/src/nats/nats-bus.cc:560-579`) compares **sorted** subject
vectors and refuses a stream "carrying different subjects; refusing to
repurpose it". Two declarations of one stream that disagreed would therefore
refuse each other for ever, and a change sink that declared
`{argus.camera.v1.change}` alone would break the object feed's stream the
moment it recreated it. The module is what keeps them one declaration:

```cpp
// Empty fields keep the production stream and its subject pair; an override
// narrows the declaration to the one subject it names.
struct EnsureInput
{
  std::string streamName;
  std::string changeSubject;
  std::string objectSubject;
};
```

The comment sits above the type rather than on each field because rule 20
admits comments at class, namespace or function scope only, and the tree's
first-party headers comment a constant or a method and never a struct field —
measured, zero occurrences. The module is a new file, so this unit had to
choose rather than inherit.

The default input declares `{argus.camera.v1.change, argus.camera.v1.object_detected}`
on `ARGUS_CAMERA`; an override declares the single subject it names. That rule
reproduces the object sink's existing four configurations exactly — including
the live test that sets both a stream name and a subject — so the boot-time
`main.cc:170` ensure moves to the module unchanged in behaviour. The two
callers pass their own field:

```cpp
// change sink
camera_event_stream::ensure(bus_, {.streamName = config_.streamName,
                                  .changeSubject = config_.publishSubject,
                                  .objectSubject = {}});
// object sink
camera_event_stream::ensure(bus_, {.streamName = config_.streamName,
                                   .changeSubject = {},
                                   .objectSubject = config_.publishSubject});
```

`NatsObjectEventSink::ensureStream` (public static) is deleted rather than kept
as a pass-through, and its `kJetStreamRetentionNs`/`kJetStreamDuplicatesNs`
constants move into the module. `services/camera/CONTEXT.md` claimed "the sink
never ensures the stream" — true before this change, false after — so that
bullet was rewritten in the same change.

**The dead single-row read is gone.** `NEXT_PENDING` and `nextPending()` are
deleted and both camera suites read `pendingBatch(1)` through the siblings'
`pendingRow(const std::vector<ChangeOutboxRow>&)` helper, as rule 23 requires
of the change that introduces a replacement. The repository's now-unused
`<optional>` include goes with them. camera's `NatsCameraChangeSink::Config`
also gains the `streamName` the three siblings carry, so a suite can point the
sink at a stream of its own instead of reconciling one on the production name.

## What this unit does not change, and why

- **The statement-level comments stay.** Measured above, they are in three of
  the five sinks, so deleting camera's two would make camera the odd one out
  and leave the rule violated next door — the same argument 3a-2c made about
  test suites, and the reason **S7** is a sweep with one reading rather than a
  per-file edit. This unit adds none.
- **The object-event drain's one row per pass.** Its table is a different one
  (`object_event_outbox`) with a droppable policy — a bounded queue with
  `maxPending` and an `overflowDropped` counter, where a change outbox has no
  bound because a change may never be dropped — and it has no sibling to match.
  **The justification this report first gave for it was wrong and is corrected
  here**: it said the queue "is meant to be drained at the tick it is produced",
  which the policy's own code contradicts, since `maxPending` and
  `overflowDropped` exist only because a backlog is expected
  (`object-event-outbox-repository.cc:99-122` drops the *oldest* pending
  observation once the count passes the cap). The real asymmetry is measured:
  the drain recovers one row per 50 ms progress tick (20 rows/s, 2 rows/s while
  a pass makes no progress at `retryMs = 500`) while the shipped cap is 5000
  (`main.cc`'s fallback; the key is absent from `config.toml.example`), so a
  fully backed-up queue takes roughly 250 s to clear and every observation
  arriving in that window drops an older one. Recorded as its own item; batching
  it is not free, because `refreshCounters()` and the `syncHook` sit in the
  per-row path.
- **Guard's drain.** **Corrected:** this report first recorded guard as having
  "no defect of either kind". That was wrong, and the unit's review upheld the
  contradiction. Measured again: `trySubscribe()` (`guard-service.cc:401-450`)
  performs the ensure *and* the durable subscribe in one function and sets
  `subscribed_ = true` at `:443`, while the 5 s retry is gated `if
  (!guard.alive() || subscribed_) return;` at `:805-809` — and `subscribed_` is
  written only at `:443` and read only at `:807`, never cleared. So guard's
  `ARGUS_GUARD` ensure is latched exactly like camera's was, and
  `guard-service.cc:986` does `co_await repository_.markEncounterSent(row.eventId, now);`
  discarding the `Task<bool>`. Guard carries both defects and belongs to its own
  unit; its already-batched, non-breaking loop stays as it is.
- **Include spelling outside the module.** `event-stream.cc` includes its own
  paired header by quote. Measured over the service: of camera's 64 `.cc`
  files, 49 open with a quoted own header — every `shared/services/*` module
  but one (`stream-hub.cc`, `evidence-uploader.cc` and the rest of the folder
  follow it) — and 13 open with the angle form and the full path from `src/`
  for their own header, all of them under the top-level `monitor/`, `objects/`
  and `operator/` folders plus `shared/services/stream/snapshot-store.cc`. The
  remaining two are `main.cc`, which is a consumer of `camera/camera-config.hxx`
  and correctly names it by path, and `feature/actions/audio-capture.cc`, which
  opens with `<algorithm>`. The consumers of this module — both sinks and
  `main.cc` — take it by the angle form, as a consumer of another folder
  should. The pre-existing stragglers are a spelling sweep, not a duplicated
  copy or dead code, so rule 23 does not reach them and this unit left them
  alone.

## Evidence

| Gate | Result |
|---|---|
| `./scripts/build-all.sh dev --only camera` | `EXIT=0`, **50/50** tests, **0** compiler warnings. Re-run after the last edit of the unit, so the row describes the committed tree: the full run below had already compiled camera before that edit |
| `change-outbox-sink-test` with `ARGUS_NATS_URL=nats://127.0.0.1:4222` | **39/39** assertions, `Status: SUCCESS`, two consecutive runs, each on a stream name of its own |
| The same binary with `ARGUS_NATS_URL` unset | **26/26** assertions, `Status: SUCCESS` |
| `./scripts/build-all.sh dev` (full) | 17 ctest summaries, every one `100% tests passed, 0 tests failed` — **400** tests, **0** compiler warnings. The 21 case-insensitive `Warning:` hits in the log are the vendored trees' configure-time notices (a `CMAKE_CXX_STANDARD` reset, ccache absent), not diagnostics on first-party code. The run ends with the orchestrator's own success line, which `set -euo pipefail` reaches only after every project's ctest *and* the tidy gate have passed |
| `scripts/check-deps.sh` | 64 declarations, 478 edges, 0 forbidden, 0 cycles, 0 unresolved, 49 deferred |
| `scripts/check-tidy.sh`, re-run on the frozen tree | 494 TUs, **3139** findings over 45 checks, baseline 3141; 2 checks below it and none above. `tidy_scan.py:295` returns non-zero on any risen count, any scan failure or a short TU list, so a summary with no `risen:` line is the gate passing — and the re-run's numbers are byte-identical to the full run's, which is what an include-spelling change to one TU must do |

**The heal case was falsified before it was trusted.** The new block points a
sink at a stream and a subject that no stream covers and asserts the row
settles and the stream now exists — 3 assertions, the live leg's 36 → 39. It
was checked by rebuilding with `ensureStream()` returning `false` (the
pre-state, which had no ensure at all) and re-running the live leg: the block
failed on all three assertions at `:331`, `:332`, and a **later block failed
too** (`:356`), because the un-settled row sat at the head and blocked the row
behind it — the defect's own head-of-line stall, visible in the suite. `39 |
36 passed | 3 failed`, `Status: FAILURE`. Restoring the ensure returns 39/39.
A change sink that cannot ensure its own stream can no longer pass this suite.

**The burst case was falsified the same way.** With `pendingBatch(1)` — the
pre-state's one row per pass — the burst block failed at `5161628765ns < 3s`,
i.e. 100 rows at the 50 ms progress tick and nothing else, `35/36`
assertions. A drain that regresses to one row per pass cannot pass this suite
either. The reviewer that reported the reviewed file mutating mid-run
(`kDrainBatch = 1`) saw this window: it was my own temporary falsification, not
a defect, and it was reverted with the editor.

The drain batch also made the outbox repository's class comment false — it
said the drain "publishes one pending row at a time" — so the comment was
corrected in the same change, as rule 20 requires.

## The review, and how its findings were applied

Six dimensions produced 11 candidates; adversarial verification confirmed 9 and
refuted 2. They fall into four families, and every one is dispositioned here —
nothing carried.

| Family | Findings | Disposition |
|---|---|---|
| The change feed could not heal its own stream | [1] "the re-arm is reachable only from the object feed's own refused publish … with an idle/disabled object feed the change feed is still stuck", [2] "the re-armed latch only covers the object feed", [3] "the batched drain cannot re-create the stream it publishes into", [6] "the unit's claim that the four change drains now run one behaviour is false on the ensure dimension" | **Closed by change 4.** All four prescribe the design implemented above; [6] names it precisely ("one shared `EnsureStreamInput` … name `ARGUS_CAMERA`, subjects {change, object_detected}, the same maxAge/duplicates"), and [1] independently measured the constraint that shaped it — a per-sink copy fails because `reconcileStream` "refuses to repurpose" a stream whose subjects differ. [1] also reproduced the pre-state stall live, in-process, deleting `ARGUS_CAMERA` broker-side: the change row never settled over a 4 s window, and a single refused *object* publish healed it in one pass, which is what pins the reachability |
| `nextPending()`/`NEXT_PENDING` left dead | [4] "camera is the only one of the four producers whose repository still advertises the read its drain no longer uses", [5] and [8] the same under different dimensions | **Closed by change 5.** [5] also names the `<optional>` include, which went with it |
| Guard's two defects | [7] "the report's verification that guard's drain has no defect of either kind is wrong" | **Out of scope**, own unit. The report's claim was corrected above; the reviewer's fix (un-gate the ensure from `subscribed_`, or clear it on a refused publish, and check `markEncounterSent`'s result) is recorded for that unit |
| The object-event drain's justification | [9] "the report's justification … is contradicted by the code it names (`maxPending`/`overflowDropped`), which exists only because a backlog is expected" | **Justification corrected**, not batched, in "What this unit does not change" above. [9] explicitly allows either; the drain-rate-vs-overflow asymmetry it computes is recorded as its own item |

The two refuted candidates were the burst block's missing cardinality
precondition — refuted because all four sibling suites share the identical
omission and this unit's mandate was parity, so fixing camera alone would make
it the odd one out. Recorded as an item to apply to all four together, the way
S7 treats the comments.
