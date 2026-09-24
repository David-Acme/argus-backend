# Closure item 6 — the burst-cardinality precondition in the producer live suites

Phase 3a, step 2's closure list, item 6.

## What the row said

"Every producer's live suite drains a burst without asserting how many rows the
burst produced, so a suite that silently wrote nothing still passes. All four
share the omission, so it is applied to all four together."

Each producer's live suite ends its burst block on an assertion about the
*absence* of pending rows (`CHECK_FALSE(hasPending(outbox))`, or
`pending == 0`). An outbox that never received a row satisfies it, and a burst
loop that never ran satisfies it too — the block's timing assertion (under three
seconds) is satisfied just as well by an empty loop. Measured, with the loop's
bound turned to zero: the pre-change suites pass.

## The change

The tree's only burst loops are four, one per producer: a
`for (int64_t recordId = 200; recordId < 300; ++recordId)` publishing into that
producer's change outbox. Each block now brackets its loop with two counts read
from the outbox itself:

| Read | When | What it proves |
|---|---|---|
| `rowsAfter(watermark) == 100` | after the loop, before `reconcile()` | the burst wrote exactly its 100 rows |
| `sentRowsAfter(watermark) == 100` | after the outbox empties | all 100 rows were published and settled |

The watermark is `SELECT COALESCE(MAX(rowid), 0) FROM change_outbox` taken
before the loop; the counts are `COUNT(*)` over `rowid > watermark` (and
`status = 'sent'` for the second). `rowid`, not `id`: three of the four tables
are keyed by `event_id TEXT PRIMARY KEY` and have no `id` column at all —
measured, the first cut of this change used `id` and three suites failed to
build with `no such column: id` — while identity's is keyed by
`id INTEGER PRIMARY KEY AUTOINCREMENT`, where `rowid` is that column. One shape
therefore reads all four.

The second count is an end-to-end assertion rather than a bookkeeping one: every
sink marks a row sent only inside the branch where `publishWithMsgId` returned
true, which is the broker's PubAck (`nats-camera-change-sink.cc:142`,
`nats-identity-change-sink.cc:246`, `nats-notification-change-sink.cc:140`,
`nats-productivity-change-sink.cc:161`). A hundred sent rows therefore means the
burst was written *and* accepted by the broker, not merely enqueued.

**One limit of the watermark, measured.** Three of the four tables are keyed by
`event_id TEXT NOT NULL PRIMARY KEY`, which is not a rowid alias, and SQLite
restarts rowids at 1 for a table that empties. A hypothetical delete of the
table *inside* the burst window would leave the watermark above the new rows'
rowids and the first count would read 0 — a loud failure, never a false green.
Nothing deletes inside the window: the retention sweep only deletes rows whose
`sent_at` is days old, and every sink's destructor joins its drain worker before
the next block starts. The alternative formulations do not survive that
hypothetical either — a `COUNT(*)` delta goes negative when the table is
emptied — so the exposure is inherent to counting rows in a table another
process may prune, and it is recorded rather than engineered around.

## A fifth suite of the same class

The sweep that fixed the four — a grep for producer loops across every suite
gated on `ARGUS_NATS_URL`, plus every suite that drains an outbox — found one
more: `services/camera/tests/unit/object-event-outbox-sink-test.cc` starts a
`Worker` that publishes 40 object events and then waits for
`outbox.stats().pending == 0`. Its only other assertion about that drain is
`countersMatch(liveSink.health(), outbox.stats())`, which compares the sink's
health counters against the outbox's own SQL counts — 0 == 0 on an empty burst.
Measured with the producer's bound at zero: the pre-change suite passes, exactly
as the row describes. The row named four, but its principle is about the hole,
not the count of files, so this one is closed with them, in that file's own
idiom — it counts with `stats()` rather than raw SQL, and `stats()` is a single
statement, so one snapshot is consistent: the row total before the producer loop
and the same total after the drain empties, 40 apart.

## What proves it

Four mutant runs, each built into the suite it targets and run live against the
dev broker, with the pre-change file as its control:

| Mutant | Result |
|---|---|
| camera's burst loop to `recordId < 200` (writes nothing), **pre-change** suite | **passes**, 48 assertions — the hole the row describes |
| the same mutant, with the two new counts | **fails** on both: `rowsAfter(watermark) == 100` is `0 == 100`, `sentRowsAfter(watermark) == 100` is `0 == 100` (50 assertions, 2 failed) |
| object-event's producer loop to `index < 0` (writes nothing), **pre-change** suite | **passes**, 26 assertions — the fifth suite's hole |
| the same mutant, with the new count | **fails** on it: `rowsAfterBurst.pending + rowsAfterBurst.sent == rowsBeforeBurst.pending + rowsBeforeBurst.sent + 40` is `4 == 44` (27 assertions, 1 failed) |

The pre-change control is the file at HEAD, restored for the probe and reverted
afterwards; the final suites are byte-identical to the ones the gate builds.
HEAD has neither assertion in any of the four (`git show HEAD:<file> | grep -c
rowsAfter` is 0 in all four), so before this change nothing in the block could
fail on an empty burst.

## Files

- `packages/identity/tests/unit/identity-change-outbox-sink-test.cc`,
  `services/camera/tests/unit/change-outbox-sink-test.cc`,
  `services/notification/tests/unit/notification-change-outbox-sink-test.cc`,
  `services/productivity/tests/unit/productivity-change-outbox-sink-test.cc`:
  the four helpers, the watermark and the two counts. The helper shape is
  deliberately the same in all four: the suites already duplicate
  `waitForDrain`, `hasPending`, the fixture and the boot wait, and there is no
  shared test-support unit for a service's own tests to reach for.
- `services/camera/tests/unit/object-event-outbox-sink-test.cc`: the 40-row
  delta around its producer loop.

No production file changes: this item is a test precondition only.

## Verification

- **Full gate** (`./scripts/build-all.sh dev`), exit 0:
  - check-comments: 1251 files, 0 comments.
  - check-deps: 64 declarations, 505 edges, 0 forbidden, 0 cycles, 0 unresolved.
  - Every project's tests at 100%: cert 2, sqlite 2, identity 29, memory 22,
    intent 4, gateway 34, sync 49, camera 55, productivity 39, notification 44,
    guard 57, tts 23, stt 8, vlm 9, llm 35, voice 26, tunnel 12.
  - check-tidy: 508 TUs, 3051 findings over 45 checks against the 3141
    baseline, 7 checks below it and none risen.
- **The fifth suite landed after that gate**, so the project it belongs to was
  verified alone the way `AGENTS.md` prescribes for a single project:
  `./scripts/build-all.sh dev --only camera`, exit 0, the same comment and
  dependency gates, camera 55/55. `scripts/check-tidy.sh` re-run on the final
  tree: 508 TUs, 3051 findings over 45 checks against the 3141 baseline, 7
  checks below it and none risen.
- **Every changed suite live**, against the dev broker on the final binaries:
  identity's sink 117/117 in all seven builds (its own project plus the six
  services that link it), camera's change sink 50/50, camera's object-event sink
  27/27, notification's sink 50/50, productivity's sink 70/70.

## What the review found

An adversarial review attacked vacuity, flakiness, the key shape, the `sent`
claim, the fifth suite's arithmetic, the completeness of the sweep and the
tree's rules, and found **no defect that makes the change wrong**. Its own
measurements: each of the five suites run ten times against the broker, 50 clean
live runs; clang-tidy on the five changed translation units reporting no finding
on any added line (the findings it does report are all in headers, or the
pre-existing `modernize-use-designated-initializers` at
`object-event-outbox-sink-test.cc:134`, which the baseline already carries and
the diff does not touch); and a `sqlite3` probe for the rowid semantics. Each
finding below was checked here before anything changed:

1. **Recorded, not fixed: the watermark's rowid assumption.** The review's one
   substantive finding — a delete inside the burst window would break the first
   count — is reproduced here (`sqlite3`: a `TEXT PRIMARY KEY` table that empties
   hands its next row rowid 1; an `AUTOINCREMENT` table continues at 4) and
   written into the technique's own paragraph above, together with the reasons it
   cannot fire today and the fact that no alternative formulation fares better.
2. **Declined: the drain loops are more patient than the timing check.** The
   loops wait up to 10 s and 20 s while the block's own assertion allows 3 s
   (camera: `attempt < 200`, the other three `attempt < 400`). Both bounds are
   pre-existing, not this change's, and the shape is deliberate: the loop waits
   long enough for a slow drain to be *diagnosed* by the timing assertion rather
   than reported as a premature pending row.
3. **Declined: four copies of the helper block.** The review confirmed the
   idiom — `AppRunner`, `waitForBoot`, `pendingRow`, `hasPending` and
   `waitForDrain` are already per-suite in all four — and that shared test
   support would have to cross four separate CMake projects, which rule 24's
   "no structure ahead of its consumer" argues against.
4. **Kept: the empty-result guard in `firstInteger`.** The review called
   `rows.empty()` unreachable, which it is for `COUNT(*)` and `COALESCE(MAX(…))`,
   but the file's own `pendingRow` guards the same way right after a `REQUIRE`,
   so the guard is what the surrounding code looks like.

