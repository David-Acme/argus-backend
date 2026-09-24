# Closure item 7 — the object-event drain's rate against `maxPending` / `overflowDropped`

Phase 3a, step 2's closure list, item 7.

## What the row said

"**The object-event drain's rate against `maxPending` / `overflowDropped`** — Its
repository drops the *oldest* pending row on overflow while its drain reads one
row per pass: the table exists because a backlog is expected, and the drain is
sized as if one were not. The asymmetry is recorded, not batched."

The recording is `f3-2g-camera-drain-parity.md`, in "What this unit does not
change, and why": the drain "recovers one row per 50 ms progress tick (20 rows/s,
2 rows/s while a pass makes no progress at `retryMs = 500`) while the shipped cap
is 5000 (`main.cc`'s fallback; the key is absent from `config.toml.example`), so
a fully backed-up queue takes roughly 250 s to clear and every observation
arriving in that window drops an older one". The item is on the list's
code-and-not-decision side: the fix is to size the drain for the backlog, and
the three change producers already spell the shape it should take.

## The pre-state, measured

`object_event_outbox` is the tree's only bounded queue: a sweep for `maxPending`
returns 22 mentions, every one of them in `services/camera`. The other three
producers' `change_outbox` tables have no cap at all, because a change may never
be dropped, and their drains were batched in 3a-2g (`kDrainBatch = 64`,
`kProgressMs = 50`, `pendingBatch(limit)` in one statement, stopping the pass at
the first row that does not settle).

Camera's object drain had none of that: `flushOnce()` read `NEXT_PENDING`
(`LIMIT 1`), published the row, marked it sent, and the loop then waited 50 ms on
progress or `retryMs` (500 ms shipped, 20 ms in the live suite) without progress.

**One correction to the row's arithmetic.** The 20 rows/s figure is the drain's
rate only when nothing is arriving to wake it. Every `publish()` ends in
`wake_.notify_all()` (`nats-object-event-sink.cc:101`), so a live producer's own
enqueues wake the drain per row and it keeps pace with the producer — it is a
backlog with *no* incoming wakes that the tick paces, which is exactly the case
the row rests on: the broker was unreachable (or the sink stalled) while the
detector kept enqueuing, and then the queue must be cleared by the tick alone.
The suite below is built on that case.

## The change

One behaviour, spelled the way its three siblings spell it:

| File | Change |
|---|---|
| `object-event-outbox-query.hxx` | `NEXT_PENDING` (`ORDER BY created_at ASC LIMIT 1`) becomes `PENDING_BATCH` (`ORDER BY created_at ASC, rowid ASC LIMIT ?`), byte for byte the sibling's shape in `change-outbox-query.hxx:17` |
| `object-event-outbox-repository.{hxx,cc}` | `std::optional<ObjectEventRow> nextPending()` becomes `[[nodiscard]] std::vector<ObjectEventRow> pendingBatch(int limit)`, `{}` for `limit <= 0` and for a null client, reserving the result |
| `nats-object-event-sink.{hxx,cc}` | `flushOnce()` becomes `flush(const ObjectEventRow& row)`; `flushLoop()` walks `outbox_.pendingBatch(kDrainBatch)` and breaks on `stopping_` or on the first row that does not settle; `kDrainBatch = 64` and `kProgressMs = 50` are file-local constants |
| `nats-object-event-sink.hxx` | the publish subject becomes `const std::string subject_`, built once in the constructor — it was rebuilt per pass, and a batched pass would have rebuilt it per row |
| `nats-object-event-sink.cc` | the drain refreshes the health counters **once per pass that settled a row**, where it refreshed after every settled row: the counter read is `OUTBOX_STATS`, a four-way `SUM`/`MIN` with no `WHERE`, so it scans every row the table holds — and the table holds the retention window's settled rows, not just the pending backlog. Batching is what made a per-row one the pass's dominant cost (measured below). The refresh carries **its own `try`/`catch`**, the purge four lines below being the shape: the drain runs on a plain `std::thread`, so an escaping `DrogonDbException` is `std::terminate`, and taking the call out of `flushOnce()` would otherwise have taken it out of the loop's only `try` |
| `nats-object-event-sink.cc` | the failed-`markSent` log line stops asserting a status it does not know: "it stays pending" is false for a row the cap dropped mid-pass and for one another writer already settled, so it now says "it is no longer pending", which is exactly what `MARK_SENT`'s `AND status = ?` guard established |

Three smaller things ride along, each because the function they live in was
rewritten: `flush` gained the `!bus_` guard `publish()` and the sibling already
have (it dereferenced the bus unconditionally); the stream ensure now runs from
the row path instead of the top of every pass, so it re-arms on the next row
after a refused publish exactly as the sibling does (the boot-time
`camera_event_stream::ensure(natsBus, {})` at `main.cc:171` still creates the
stream, and an idle object feed has no row to publish, so nothing waits on it);
and the drain's wait uses `kProgressMs` where it used a literal `50`.

## What proves it

The live block of `object-event-outbox-sink-test.cc` now publishes 100 events
through a sink that has **not** been reconciled — so a real backlog exists and no
wake can arrive — asserts `outbox.stats().pending == rowsBeforeBurst.pending +
100`, then reconciles and waits for `pending == 0`, asserting the 100 rows
settled, the health-equals-outbox check, and the block under three seconds.

Three trees, one suite, one machine, the dev broker, the same 104-row backlog
(the burst plus four stale rows the earlier blocks leave behind):

| Tree | Block | Verdict at 3 s |
|---|---|---|
| `HEAD` (one row per pass) | 5,414,859,535 ns — **5.41 s** | **fails** |
| final tree, `kDrainBatch = 1` | 5,419,895,624 ns — **5.42 s** | **fails** |
| final tree, `kDrainBatch = 64` | 143,434,130 / 145,906,646 ns before the counter change and 145,866,294 ns after it — **0.143 / 0.146 / 0.146 s** | **passes** |

The pre-state number is the row's own arithmetic, reproduced: 104 rows at one
settle per ~51 ms tick is ~5.3 s, and the block's remaining ~0.1 s is the 100
enqueues (~1 ms each, a transaction and a commit wait apiece) — so the drain ran
at **~19.6 rows/s**, the 3a-2g record's 20 rows/s to the digit. The batched drain
settles **at least 64 rows per tick** and settled the 104 measured here in two
passes, so the same block fell from 5.41 s to 0.143 s on the identical workload.

**The first shape of this test did not discriminate, and that is why it looks
the way it does.** The block was first rewritten with the producer draining
concurrently, as it had been, and HEAD's code passed it in under three seconds —
because of the wake: a live producer keeps the one-row-per-pass drain at the
producer's pace. Measuring that is what turned the block into the sibling's
shape (backlog first, reconcile after), which is both the discriminating case and
the case the row is about.

**The bound is the assertion, not a decoration.** `kDrainBatch = 1` in the final
tree is the same 5.42 s as `HEAD` to within 5 ms, so what the bound measures is
the batch and not the loop's rewrite: with the loop's shape kept and the constant
dropped to one, the three seconds are blown by 80%.

### The counters, which the batch exposed and the review measured

`refreshCounters()` reads `OUTBOX_STATS`: four `SUM`/`MIN` `CASE` expressions with
no `WHERE`, a covering-index scan of **every row the table holds**. What the table
holds is not the pending backlog (`maxPending`, 5000 shipped) but the retention
window's settled rows — seven days, purged once a day — so a per-row refresh is
O(table) per settled row. The 50 ms tick used to hide that; batching removed the
tick, and at a table that has run for a week the aggregate becomes the pass's
dominant cost.

Measured on the app path (`outbox.stats()` from the suite's thread, the table
seeded through the app's own client), against the same schema in the bare engine:

| Table size | `stats()` on the app path | `sqlite3` CLI, same query and schema |
|---|---|---|
| 105 rows | 173 µs | — |
| 100,000 rows | **62.8 / 65.8 ms** (two runs) | 12 ms |

And the suite's live block with 100,000 settled rows seeded under it (probe copy
of the suite, bound read at 1 ns; the seed carries a current `sent_at`, because
the drain's own retention pass deletes anything older than the stream's window —
a first attempt seeded 1970 timestamps and the sink purged all 100,000 rows on
its first `reconcile()`):

| Tree | The drain's own aggregates | Block, measured |
|---|---|---|
| per-row refresh (control) | 104 aggregates, ~6.5 s of pure scan work, holding the single connection while the poll loop waited on it | **20.79 s** |
| per-pass refresh (the change) | two aggregates, ~0.13 s | **7.54 s** |

The decisive number is the 13.3 s delta between the two blocks, and it reproduces
because the probe's own poll reads are common to both arms; the drain-to-empty
window alone at this table size is ~0.54 s on the final tree, the poll loop's own
reads (each ~64 ms here, sharing the one connection) making up the rest. What
neither number removes is the ~6.6 s that belongs to the
*enqueue* path: each of the burst's 100 `publish()` calls refreshes after its own
enqueue, and the suite asserts `countersMatch` immediately after a publish, so
that refresh is the counters' contract rather than a cost this item may move.
Recorded, not changed: one aggregate per event on the detector's thread, bounded
by the event rate (at 100k rows, a week of events at a sustained one per six
seconds, that is ~6% of one core and a ~63 ms stall per observation).
Incrementally moving the counters from the enqueue outcome would dodge it and
lose `oldestPendingAtMs`, which `stats()` alone can compute.

## What the batch does not change

- **Order and completeness.** A pass reads up to 64 rows oldest-first and settles
  them in that order, stopping at the first that does not settle, so a row that
  cannot be published keeps the head — the same head-of-line behaviour as before,
  over more rows per pass.
- **The row that is dropped mid-pass.** The overflow policy flips the *oldest*
  pending row to `overflow_dropped` when the count passes `maxPending`
  (`object-event-outbox-repository.cc:97-120`), and the batch's rows are the
  oldest, so an in-memory batch row can be dropped before its own flush. Its
  `markSent` then affects no row — `MARK_SENT` carries `AND status = ?`, so it
  matches nothing — and the drain reports no progress, letting the retry cadence
  take over. The pre-state had the same window over one row; the batch widens it
  to the rows read but not yet flushed, which under a sustained flood at the cap
  costs a pass rather than a row. The window exists whenever the queue is at its
  cap, whether or not a publish is being refused: the drop is the enqueue path's
  own decision and takes no notice of the drain. Recorded, not engineered
  around: distinguishing "dropped" from "already settled" would take a second
  query per failure — what changed is only the log line, which no longer claims a
  status (above).
- **The per-row hook and the per-pass counters.** `syncHook` still fires per
  settled row; `refreshCounters()` moved to once per pass. Health is still exact
  at every point the suite reads it — a publish refreshes after its own enqueue,
  and a pass refreshes immediately after the last row it settled, before it
  waits — so `countersMatch`'s quiescent reads are unchanged. What changes is
  that *during* a pass health may lag by up to one pass (≤64 rows); no reader
  depends on more — the only one is `/health`'s extras (`main.cc:249-257`), which
  cannot block on it — and the age keeps advancing between refreshes, because
  `oldestPendingAgeS` is computed from the pending row's own `created_at`; only
  the base it is measured against needs a refresh.
- **The `rowid ASC` tiebreak** is shape parity, not a repair, and it was measured
  rather than assumed: `sqlite3` with the real schema and its
  `(status, created_at)` index shows `EXPLAIN QUERY PLAN` picking
  `idx_object_event_outbox_status` with no temp B-tree for *both* forms, and with
  rowids deliberately out of insertion order (`5, 3, 1`) both forms return rowid
  order — the index's entries carry the rowid, so the tiebreak makes explicit what
  the plan already gives. It matches the sibling exactly, which is the argument.
- **The cap itself.** `operator.outbox_max_pending` is read by `main.cc:162` with
  a 5000 fallback and is absent from `services/camera/config.toml.example`, so the
  number this item is measured against is invisible to an operator. Changing it is
  a decision about how much backlog to keep, which item 7 does not revisit;
  recorded here rather than fixed.

## Files

- `services/camera/src/shared/repositories/object-event-outbox/object-event-outbox-query.hxx`
- `services/camera/src/shared/repositories/object-event-outbox/object-event-outbox-repository.hxx`
- `services/camera/src/shared/repositories/object-event-outbox/object-event-outbox-repository.cc`
- `services/camera/src/operator/nats-object-event-sink.hxx`
- `services/camera/src/operator/nats-object-event-sink.cc` — the batched pass, the
  per-pass counter refresh and the corrected log line
- `services/camera/tests/unit/object-event-outbox-test.cc` — `pendingBatch` coverage: the
  limit is honoured, a settled row is not in the batch, `attempts` is carried, and
  two rows sharing a `created_at` come back in insertion order
- `services/camera/tests/unit/object-event-outbox-sink-test.cc` — the backlog-first
  live block and the three-second bound

## Verification

- **Project gate** (`./scripts/build-all.sh dev --only camera`), exit 0:
  check-comments 1251 files / 0 comments, check-deps 505 edges / 0 forbidden,
  camera 55/55 tests. 0 errors, 0 warnings on a forced rebuild of the changed
  translation units.
- **check-tidy** on the final tree: 508 TUs, 3049 findings over 45 checks against
  the 3141 baseline, 8 checks below it and none risen — two findings fewer than
  item 6 left.
- **Live suites** against the dev broker, on the final binaries: the sink suite
  29/29 three times on the final tree, the repository suite 40/40.
- **The probes** above: the `HEAD` control and the `kDrainBatch = 1` mutant each
  build the sink suite alone, run it live, and fail on the new bound; the counter
  probe seeds 100,000 settled rows through the app's client and reads the block's
  time off the same bound set to 1 ns; the final files were restored byte for byte
  afterwards and re-verified with `sha256sum`. The bare-engine figure comes from
  `sqlite3` 3.50.6 on the real `database/schema.sql` and the query text verbatim.
  One measurement was discarded as unusable rather than reported: this
  environment's `python3` `sqlite3` module runs the same scan ~100× slower than
  the engine (28 µs/row against 0.12), so the curve was taken from the CLI and
  from the app path instead.

## What the review found

The adversarial review (a fresh agent, opus) returned **no defect that makes the
change wrong** — it verified the batching, the head-of-line behaviour, the
`rowid ASC` parity (including that the pre-change form still passes 40/40) and
the tree it reviewed (81 insertions / 39 deletions over the seven code files —
84 / 41 once the two changes below landed, 91 / 41 once the delta review's own
fix did too), and ran the live
suite 112 times without a `countersMatch` failure. Five findings came back; each
is triaged below — two produced code changes, one a mechanical one, and two are
recorded limits rather than defects.

**F1 — the per-row counter refresh is a full-table aggregate, so the rate claim
holds only while the table is small.** Confirmed, and it is the finding that
changed the item. `OUTBOX_STATS` has no `WHERE` and the settled rows live the
retention window, so the per-row refresh the batch exposed is O(table) per row.
The review proposed a follow-up item; declined, because the batching is what made
the cost the pass's dominant term, so the cost belongs to this item. The drain now
refreshes once per pass, and the measurements above are the evidence and the
scoping: 0.146 s at 105 rows, 7.54 s against 20.79 s on a 100k-row table, with the
enqueue path's own per-event refresh recorded rather than moved.

The review's numbers for the same table did not reproduce as given: it measured
`OUTBOX_STATS` at 11.6 ms and a per-row drain at 13.9 s for 100k rows, while the
app path measures 62.8 ms per call and the control block 20.79 s. Its raw-engine
figure is the one that matches the bare `sqlite3` CLI (11.6 against 12 ms), so the
curve appears to have been taken at the engine rather than through Drogon's
client — the app path is the worse one, so the finding's direction stands and its
magnitude understated it.

**F2 — the failed-`markSent` log line is false for a row the cap dropped, and the
report conflated that cause with a refusing broker.** Confirmed by reading
`MARK_OVERFLOW` (no status guard) against `MARK_SENT` (`AND status = ?`). Both
halves are fixed: the log line no longer claims a status, and the report's
sentence now says the window exists whenever the queue is at its cap. The
review's own reading — that the row is published and then stays *dropped*, not
pending — is what the new wording says.

**F3 — the three-second bound tolerates about one and a half PubAck stalls**
(`MaxWait = 2000` in the bus), so a stall makes the block fail silently rather
than being reported as a stall. Accepted and recorded, not changed: the sibling's
block carries the same bound, the stall could not be induced by the review under
load (≤206 ms) or in the runs here (0.143–0.146 s), and widening the bound would
weaken what it measures (`HEAD` and the `kDrainBatch = 1` mutant are at 5.4 s).

**F4 — the plan row was still open and this report untracked.** Mechanical; both
are done by the commit that carries this file.

**F5 — everything else checked out**, including the counter arithmetic the commit
message repeats, the two small guards that ride along, and that no tidy finding
landed on the changed lines.

### The delta review

The two changes F1 and F2 produced were reviewed in turn, by a second fresh agent
(opus), against the source rather than this report: it verified the batch walk and
the `break` line for line against the three sibling sinks, the `!bus_` guard, the
stream ensure from the row path, the `rowid ASC` tiebreak (both `ORDER BY` forms
pick `idx_object_event_outbox_status` and both return rowid order for rowids
inserted out of order), the `MARK_SENT` status guard, and that the tree it was
handed was restored byte for byte. It reproduced every figure it could — 138–168 µs
at 105 rows, 63.6–69.0 ms at 100k, 13 ms in the CLI, a 6.62–6.83 s enqueue burst,
a 0.54 s drain-to-empty, and the `kDrainBatch = 1` mutant failing the bound at
5,418,958,020 ns, within 1 ms of the figure above — and refuted none.

**The one defect it found was mine, and it is fixed.** Moving the refresh out of
`flushOnce()` took it out of the loop's only `try`: `refreshCounters()` reads
`OUTBOX_STATS` through the client, the drain runs on a plain `std::thread`, and an
escaping `DrogonDbException` is `std::terminate` — the camera service, detection
and streaming with it — where the pre-change call was logged and retried. The
refresh now carries its own `try`/`catch`, the shape the purge statement four
lines below already had, and the whole gate was re-run on the fix.

**Two recorded limits, neither introduced here.** The null-client branch of
`markSent` returns false without reading anything, so "it is no longer pending" is
false there — the branch is unreachable while the drain lives (shutdown stops and
joins the sink before the client can be reset), and for the two reachable causes
the wording is exact, which is more than the old one was. The other is a
consistency debt noticed on the way: the three sibling sinks still log "it stays
pending" for their own zero-row `markSent`, which their `AND status = ?` guard
falsifies the same way; that is their units' wording, and this item does not
reach into them. A pass whose only settle attempt matched zero rows leaves
`progressed` false and so refreshes nothing, and the purge runs after the refresh,
so a purging pass leaves `sent` ahead until the next one — both are pre-existing
(`flushOnce()` had the identical shape), both self-heal at the next refresh, and
neither is reachable from the suites.
