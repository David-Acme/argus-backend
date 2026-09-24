# Phase 3a step 3 — the audit trail compacts at its retention window

Phase 3a, step 3 of `docs/history/plans/architecture-plan.md`: "Apply the
90-day TTL and its compaction (D15); `contracts/sync` declares the resync
semantics". The decision is D15 (`architecture-plan.md:43`): audit retention is
a 90-day TTL, `sync` prunes and compacts audit rows older than the window, a
client offline longer than it cannot converge by replay and re-bootstraps with
a full `Synchronize`, and the semantics are declared in `contracts/sync`.

## What existed before

`audit_log` and `user_audit_log` grew without bound. The only thing that ever
removed a row was the fan-out's own coalesce: a change to a record merged into
that record's `(table, UTC day)` row, so a burst settled on one row per day —
and a day's rows were never removed at all. Nothing read the two tables'
age, nothing declared a window, and nothing told a client that a cursor had
fallen out of the range the trail could still answer.

The two writers are `AuditLogService::create` / `UserAuditLogService::create`
in `services/sync/src/feature/fanout/services/`, the single writer of the two
tables (rule 1 of the service's `AGENTS.md`), and the paging legs are
`SynchronizedService::syncAuditLog` / `syncUserAuditLog`.

## The decision, as built

### The window is a contract value

`packages/contracts/sync/src/sync/audit-retention.hxx` declares
`audit_retention::kDefaultDays = 90`. It sits in the contract, not in
`services/sync`, for the same reason `SyncLimits::kMaxRows` does: the app has
to know the window is what makes a cursor refusable, and the service that
applies it is not the only party the number concerns. `argus-sync` reads its
own config key, `[sync] audit_retention_days`, and falls back to this constant
when the key is absent — the shape the gateway's `[gateway]` keys already use
(`ConfigService::hasKey` → `getInt`), not guard's `configIntOr`, because the
`<= 0` branch is a live operator answer here rather than dead structure: a
value `<= 0` means "keep every row", which is the escape hatch an operator
wants the day they need the history back. `SyncConfig::resolveAuditRetentionDays()`
is that resolution, called once in `main.cc`.

### The frontier

A new table in `services/sync/database/schema.sql`:

```sql
CREATE TABLE IF NOT EXISTS audit_compaction_state (
  table_name TEXT PRIMARY KEY,
  compacted_through_id INTEGER NOT NULL DEFAULT 0
);
```

One row per audit table. `compacted_through_id` is the highest id any sweep has
deleted, and it only ever moves up:

```sql
INSERT INTO audit_compaction_state (table_name, compacted_through_id)
VALUES (?, ?) ON CONFLICT(table_name) DO UPDATE SET
compacted_through_id = max(compacted_through_id, excluded.compacted_through_id)
```

The monotonic `max` is what makes the frontier a property of the data rather
than of a sweep's schedule: two sweeps that overlap, or a sweep that runs after
a restart while an older one's statement is still in flight, can only agree on
the same value. The frontier is read (never written) by the paging legs.

### The sweep

`services/sync/src/feature/fanout/services/audit-retention-service.{hxx,cc}`:
a `start(int retentionDays)` called from `main.cc`'s beginning advice, a warmup
`runAfter(30 s)` and a `runEvery(24 h)`, both posting `runSweep()` on the app
loop, and a `stop()` the destructor calls. `runSweep` computes
`cutoffMs = now - retentionDays * 86'400'000`, refuses re-entry through a
`running_` flag (with a log line, so a skipped tick is visible), and awaits
`sweep(cutoffMs)` off the loop through `drogon::async_run`, catching
`std::exception` and `...` so a failing sweep can never kill the timer.
`clearTimer` is guarded by `drogon::app().isRunning()` — the same shape
`guard`'s retention sweep uses for the timer half, for the same reason: a timer
id is only clearable while the loop still exists. Guard's tick also captures a
`shared_ptr` lifecycle by value and the process drains the in-flight tick
through `shutdown_signal::onStop`; `argus-sync` registers no drain at all, so
this sweep keeps `[this]` and is never waited for — safe today because the
service is a `main()` local declared before `app().run()` and nothing resumes a
suspended frame after `quit()` returns, and recorded rather than adopted (see
"Flagged, not fixed"). `start` refuses a second call, through a `started_` flag
and a logged skip: the window is read once at boot, so a caller that later
wanted a different one can neither clobber `retentionDays_` to `0` behind
already-live timers nor register a second pair that `stop()` could never
invalidate.

`sweep` alternates the two tables and repeats until a round deletes nothing:

```cpp
while (true) {
  const int64_t auditRemoved = co_await auditLogService_.compact(cutoffMs);
  const int64_t userRemoved = co_await userAuditLogService_.compact(cutoffMs);
  const int64_t roundRemoved = auditRemoved + userRemoved;
  removed += roundRemoved;
  if (roundRemoved == 0)
    break;
}
```

There is no round cap. A cap was written first (50 rounds) and removed: since
each round deletes at least one row whenever it finds a pair, the loop provably
terminates, and a cap only turns a large pre-existing backlog into a sweep that
stalls for a day per 10 000 rows. The loop's bound is the backlog, which is the
thing the sweep exists to shrink.

### The compaction: pairwise, position-preserving

`AuditLogService::compact(int64_t cutoffMs)` (and its user twin, identical but
over `UserAuditLogRepository`):

1. `findCompactionPairs(cutoffMs)` — each row older than the cutoff paired with
   the nearest newer **old** row of the same key:

```sql
SELECT o.id AS older_id, min(n.id) AS newer_id
FROM audit_log o JOIN audit_log n
ON n.record_id = o.record_id AND n.table_name = o.table_name
AND n.id > o.id AND n.event_timestamp < ?
WHERE o.event_timestamp < ?
GROUP BY o.id ORDER BY older_id ASC LIMIT 200
```

   `idx_audit_log_record (record_id, table_name)` is what serves the join; the
   page size is `SyncLimits::kMaxRows`, the sync page size.
2. `findCompactionChanges(ids)` — one statement for both members of every pair
   in the page, not one query per pair.
3. Fold, in ascending `older_id`, through a `pending` map: the older row's diff
   merged into the newer one with `JsonDiff::compareChanges(older, newer)` —
   the daily coalesce's own chain, so a merged row is `{oldest previous, newest
   current}` per key — and the result parked under the newer id. A row that is
   both an older and a newer member of two pairs in the same page (a chain) is
   resolved by that map: the second fold reads the first fold's result, and the
   update pass skips ids that are also being deleted.
4. `compactRow` for every survivor — `UPDATE audit_log SET changes = ? WHERE
   id = ?`, so the survivor keeps its own priority and timestamp.
5. `removeMany(removeIds)` — one `DELETE ... WHERE id IN (...)`.
6. `advanceCompactionFrontier(max older_id)`.

**Why pairwise rather than "the oldest N rows".** A window containing a
record's only surviving row is a row that can never be deleted, so a
batch-of-the-oldest policy clogs: the sweep would re-find the same lone
survivors every round, delete nothing behind them, and never reach the records
whose history is long. Pairing is what guarantees progress — every pair deletes
one row and every remaining row either has a newer old row to pair with or is
the newest old row of its key, which is exactly the row compaction exists to
keep.

**Why it cannot touch a live write.** A **recent** row (younger than the
cutoff) is never a candidate (`WHERE o.event_timestamp < ?`) and never a
partner (`ON ... n.event_timestamp < ?`), and a live change writes rows stamped
`now`, so the sweep's row set and the live fan-out's are disjoint: a change
arriving during a sweep writes rows the sweep cannot select, and the daily
coalesce's insert-then-delete pair is a `create` of a recent row plus a
`remove` of the row it replaces — a row that, being recent, was never the
sweep's either. A sweep may therefore suspend across as many statements as it
likes. The `running_` flag is a cheap second line, not the safety argument.

**The one shape a replay can break that, without breaking anything else.** The
fan-out stores the *event's* timestamp (`AuditLogWriteInput::eventTimestamp`,
stamped by the producer at emit time and carried through its outbox), so a
producer that drains an event older than the window inserts an old-timestamped
row under a fresh high id — a row the sweep may select. The sweep is written so
that this costs a round and nothing more: `findCompactionChanges` reads both
members of every pair before any write, so a row that a concurrent coalesce
removes first aborts the round in `stored.at` (`std::out_of_range`, caught by
`runSweep` and retried on the next tick) with no `compactRow`, no `removeMany`
and no frontier move behind it, and a row that survives re-folding is
idempotent because the fold keeps the oldest `previous` and the newest
`current`. A replay that far behind also implies NATS was unavailable for the
window, which is why this is recorded as the shape of the claim rather than
defended by a test.

**Why the summary is position-preserving.** The survivor's id is above every
row it supersedes, and the client pages ids ascending, so a replica that paged
past the survivor has already applied both the folded row and the survivor's
own diff and is unaffected by the merge; a replica that has not reached the
survivor gets the merged `{oldest previous, newest current}` and lands on the
same value. What the merge drops is the older row's priority and timestamp,
which is why the survivor keeps its own — the fold is about the record's
history, not about when the change was announced.

### The refusal

`SyncErrors::ReplicaTooOld` — `ErrorCode::Conflict`, 409, "Audit cursor is
older than the retention window" — is the eighth refusal in
`packages/contracts/sync/src/sync/sync-errors.hxx`. Both audit legs throw it
before they query:

```cpp
if (body.afterId && *body.afterId > 0) {
  const int64_t frontier = co_await auditLogRepository_.findCompactionFrontier();
  if (*body.afterId < frontier)
    throw ResponseException(SyncErrors::ReplicaTooOld);
}
```

`afterId = 0` — the legal empty baseline a fresh client establishes — is
explicitly outside the gate, so a new install still fills its history from a
compacted table. `afterId >= frontier` pages normally, which is the case that
matters for a client that is merely behind rather than stale. The app keys on
the 409 to drop its replica and re-bootstrap with a full `Synchronize`; the
wire code is the frozen `CONFLICT`, not a new spelling.

The honest limit of the refusal: because the summary is position-preserving, a
cursor just below the frontier would in fact still converge — the survivor
carries the folded row's previous value and its own current. The refusal is the
declared boundary of the window (D15 says the app must handle "replica too
old"), it is what lets a client know its replica is older than the trail rather
than inferring it from a summary it cannot distinguish from a normal diff, and
it is deliberately the *conservative* side: refusing a serviceable cursor costs
one re-bootstrap, serving a stale one costs silent divergence.

## The review

A fresh agent reviewed the unit adversarially — the code, the docs and the
three already-built test binaries (its own runs: 50 + 68 + 4 assertions) — and
reported five findings. Each was verified here against the code before anything
was touched; two produced a change, three are recorded.

1. **The docs stated the position property backwards and derived an
   impossibility from it** — CONFIRMED, and the most consequential finding.
   `sync-engine.md`, the step-3 row itself, `wire-sync-tables.md`, both
   `configuration-keys.md` tables, `contracts/sync/AGENTS.md` and
   `services/sync/CONTEXT.md` all said the survivor "keeps a position at or
   below every row it supersedes" and that a refused cursor "cannot be
   completed by replay". The code says otherwise — the pair is
   `min(n.id) ... WHERE n.id > o.id`, so the survivor's id is strictly above
   every row it supersedes — and that is exactly why a refused cursor would in
   fact still converge, which is what this report's own honest-limit paragraph
   already said. The docs were the artifacts that had drifted, and they are what
   the next unit and the frontend read, so all six now state the refusal as what
   it is: the window's declared, deliberately conservative boundary.
2. **`start()` clobbered the window before its own guard** — CONFIRMED, latent
   (`start` has exactly one caller, `main.cc`'s beginning advice, so nothing
   reaches it today). `retentionDays_ = retentionDays;` ran before the `<= 0`
   early out, and a second call would also have registered a second timer pair
   that `stop()` could never invalidate: `start(90)` followed by any later
   `start(0)` would leave live timers sweeping with `cutoff = now`, every audit
   row a candidate, each key compacted to its newest row and the frontier driven
   high enough to re-bootstrap the fleet with its history gone. Fixed with a
   `started_` flag — the window is read once and a second call is a logged
   no-op.
3. **The sweep holds a raw `this` with no lifecycle guard and no drain** —
   CONFIRMED as a deviation from `guard`'s precedent, which captures a
   `shared_ptr` lifecycle by value in its tick and is drained by
   `shutdown_signal::onStop`; recorded rather than adopted, because `argus-sync`
   registers no drain for any of its legs and the reachable path is safe (a
   `main()` local that outlives `quit()`, with no frame resumed after it). The
   report now says so where it used to claim guard's shape wholesale.
4. **A producer replaying an event older than the window can land a row the
   sweep may select** — CONFIRMED in shape: the fan-out stores the *event's*
   timestamp (`AuditLogWriteInput::eventTimestamp`, stamped at emit time and
   carried through the producer's outbox), so a drain delayed past the window
   writes an old timestamp under a fresh id. The reviewer could not construct a
   client-visible divergence from it and neither could I; recorded as the limit
   of the sweep's "disjoint row sets" claim, which is a claim about live
   writers. The one round it can disturb aborts before any write and is retried.
5. **"into their record's newest row" named the wrong fold target** —
   CONFIRMED: the target is the nearest newer **old** row, which for a record
   still being written is not its newest row. Corrected in the four places that
   said it.

Two of the review's claims did not survive checking. It called the cutoff
comparison unpinned by the test — "the row seeded at exactly `cutoffMs` (id 11)
is the newest of its key and can never pair either way" — but id 11 sits *below*
a newer old row (id 12), so a `<=` in the candidate clause forms a pair that the
test's `== 3` and its survival assertion both reject; the half the reviewer was
right about is the *partner* clause, whose `<=` no seeded row exercised, and
that half is now pinned by two rows that mirror the first pair — id 13 sits
below a row at the cutoff (id 14) where id 12 sits below one (id 11). The stale
CMakeLists count it noticed
is real and moved by this unit: `contracts/sync/AGENTS.md` said 18 where the
tree has 21, one of them `services/sync/src/config`, and the count is corrected
to 20 consumers with seven of them argus-sync's. Its two test-strength gaps that
stood — the page bound never exercised, and the WS envelope's 409 inferred
rather than pinned — are recorded rather than papered over: the bound is now
asserted by a 402-row backlog compacted a page at a time, where a wrong or
missing `LIMIT` fails the run, and the envelope is the path every refusal in the
socket shares (`SyncSocket`'s `ResponseException` catch), which the transport's
own suite covers rather than this one.

Both halves of the cutoff comparison were then checked as mutants against the
real binary, because the review's claim about them was half right and reasoning
is weaker evidence than a failing run. With the candidate clause's `<` turned
into `<=`, the suite fails 11 of its 64 assertions (`compact()` returns 4 where
it should return 3, and the page arithmetic shifts with it); with the partner
clause's `<` turned into `<=`, it fails 12 (the 13/14 pair forms, `compact()`
returns 4, id 13 loses its own diff and the frontier moves to 4 anyway). Both
mutants were reverted byte-for-byte and the restored tree passes 64/64.

## What proves it

- `packages/contracts/sync/tests/unit/audit-retention-test.cc` — the window's
  default (90, positive) and the refusal the app re-bootstraps on: `status ==
  409` and the wire code is `ErrorCode::Conflict`'s own spelling.
- `packages/contracts/sync/tests/unit/sync-contract-catalog-test.cc` — the
  catalog is eight refusals, and the new row's status, code, message and
  legality are pinned with the other seven.
- `services/sync/tests/unit/audit-compaction-test.cc` — the behaviour, against
  the real schema in a temporary database:
  - an empty table compacts to 0 and leaves the frontier at 0;
  - a seeded table (14 module rows over seven records, two of them at exactly
    the cutoff — one below a newer old row and one above an older row, so each
    half of the cutoff comparison has a witness that fails the run if it ever
    became `<=`) compacts to 3 deletions — a two-row record's older row, and a
    three-row chain's head and middle — so the chain settles on one row holding
    the oldest previous and the newest current, the survivor keeps its own
    priority and timestamp, and the recent rows, a lone old row, an old row
    whose only newer neighbour is recent, the old row whose only newer neighbour
    sits at the cutoff and both cutoff rows themselves all survive untouched;
  - the frontier lands on the highest deleted id, a repeat sweep deletes 0 and
    changes nothing, and `advanceCompactionFrontier(2)` after it leaves the
    frontier at 4 — the monotonic `max`;
  - a backlog of 201 pairs (402 rows, one statement) is compacted a page at a
    time: the first sweep deletes exactly `SyncLimits::kMaxRows` and stops at
    the page's own highest id, the second deletes the pair the page left, the
    third deletes nothing, and the last pair's survivor carries its chain's
    whole `a → c` fold — the page bound and the fold across a page boundary;
  - the user twin: rows of different users never pair, a two-row chain for one
    user merges `A → C`, and the user frontier tracks its own table while the
    module frontier is untouched.
- `services/sync/tests/unit/audit-sync-read-test.cc` — the refusal at the wire
  of both legs: with a seeded frontier of 5 (module) and 3 (user), `afterId = 0`,
  `= 5` and `= 3` return row arrays and `afterId = 4` and `= 2` throw with
  status 409 and the `CONFLICT` code.

## Verification

The gate is a full `./scripts/build-all.sh dev`, and it rejected the change
once before it accepted it: the tidy ratchet (rules 16 and 19, measured) went
red on the first run.

**First run — rejected.** Comments and dependencies were clean —
`check-comments: 1256 files checked, 0 comments` and `check-deps: 64
declarations, 511 edges, 0 forbidden, 0 cycles, 0 unresolved, 49 edges deferred
to phase 3 (236 third-party mentions over 22 roots)` — and all 17 projects
built and tested at 100%, but the scan closed with `check-tidy: 511 TUs, 3051
findings over 45 checks, baseline 3049; 1 checks below it` and six `risen:`
lines: `bugprone-easily-swappable-parameters` 53/52,
`bugprone-narrowing-conversions` 67/66,
`bugprone-suspicious-stringview-data-usage` 305/301,
`modernize-avoid-c-arrays` 83/81, `modernize-avoid-c-style-cast` 363/362 and
`modernize-use-designated-initializers` 258/256 — eleven findings net above the
recorded ceilings, a rejection however well the tests pass.

The findings behind the rises were attributed the way item 9 attributed its
falls, not eyeballed: a script parsed `git diff -U0`'s hunk headers (untracked
files wholly added) and printed only findings whose file and line fell inside
an added range. Thirteen did, every one of them this change's own, and each was
fixed at its source rather than suppressed:

- the new test's two `char[]` constants → `std::string_view`, with
  `std::string(...)` where SQL needs one — `modernize-avoid-c-arrays`;
- `auditBacklogInsert(int64_t rows, int columns)` became
  `auditBacklogInsert(const int64_t rows)` over the file's own
  `kBacklogFirstId` — one parameter, not two adjacent integers, which clears
  `bugprone-easily-swappable-parameters` and the `bugprone-narrowing-conversions`
  its call site raised by passing the literals;
- the test's positional `Sqlite3Config{1, db.path(), "default", -1}` and the
  catalog test's new positional row → designated initializers —
  `modernize-use-designated-initializers`;
- `Json::Int64(afterId)` → `static_cast<Json::Int64>(afterId)` in
  `audit-sync-read-test.cc` — `modernize-avoid-c-style-cast`, since `Json::Int64`
  is a typedef and the functional cast is one;
- `QUERY.data()` → `std::string(QUERY)` in the new compaction queries of each
  audit repository — `bugprone-suspicious-stringview-data-usage`.

Those last two repositories also carried seven older `.data()` calls, on lines
this change had not written but in the files it was rewriting; they were fixed
in the same change, which is rule 19's own instruction — a count comes down in
the change that fixes what stands behind it. One further added-line finding
never appeared as a `risen:` line at all: the new test's `TempDb::path()` lacked
`[[nodiscard]]`, and `modernize-use-nodiscard` sat below its ceiling — the
annotations this change added to the two audit repositories had pulled it down —
so only the sum hid it. Scanning this unit's own translation units found it, and
it was fixed with the rest.

**Second run — accepted.** All three gates green, exit 0:

```
check-comments: 1256 files checked, 0 comments
check-deps: 64 declarations, 511 edges, 0 forbidden, 0 cycles, 0 unresolved, 49 edges deferred to phase 3 (236 third-party mentions over 22 roots)
check-tidy: 511 TUs, 3030 findings over 45 checks, baseline 3049; 2 checks below it
check-tidy: worst file services/guard/src/feature/guard/guard-repository.cc (104 findings)
```

with no `risen:` and no `unread:` line, every one of the 17 projects at
`100% tests passed` — 462 tests — and 0 errors and 0 warnings across the whole
build. The two projects the change touches were confirmed on their own as well:
`--only identity` 30/30 (the contract tests compile inside it) and `--only sync`
51/51.

**The re-record.** The fix left the tree lower than the baseline item 9 had
recorded, so the close recorded the tree the change actually leaves, as item 9
did: `scripts/lib/tidy-baseline.txt` now reads `tus` 508 → 511 — this change's
three new translation units — and 3,049 → 3,030 findings over the same 45
checks, with two checks below and none risen. Only two check lines moved, each
one the fix's own doing: `modernize-use-nodiscard` 759 → 749 (the `[[nodiscard]]`
annotations on the two audit repositories and the new test's `TempDb::path()`)
and `bugprone-suspicious-stringview-data-usage` 301 → 292 — thirteen findings in
the two repositories before the fix and none after, of which seven were
`.data()` calls `HEAD` already carried, two lived on the dead `updateChanges`
methods the unit deleted, and four were the unit's own new queries.
Re-recording on a built tree
tightens both halves of the ratchet: the TU floor rises with the tree, and the
ceilings fall to what is actually there, so a later unit cannot silently spend
the falls this one earned. The recording is deliberate and is the only reason
the numbers in this section are not the numbers item 9 left.

## Flagged, not fixed

- **The frontend owes the client half of the refusal.** The wire is additive —
  no path, envelope, `SyncOperation`, `TableName` or existing code changed, so
  §1.8's "declared in the contract" half is satisfied by
  `packages/contracts/sync` — but the app does not yet handle the new 409: it
  must drop its replica and re-bootstrap with a full `Synchronize` when either
  audit leg answers `ReplicaTooOld`, and until it does, a client offline longer
  than the window sees an error where it should re-bootstrap. D15 states the
  obligation, and the plan's Phase 3d step 5 is where the client's endpoint and
  route work lands; it is the frontend repository's change, not this one's.
- **`[[nodiscard]]` is now on the two audit repositories' methods and not on
  their sibling's.** Rewriting `AuditLogRepository` and
  `UserAuditLogRepository` was the moment to annotate their `Task`-returning
  methods (rule 19), which the tidy ratchet counts as findings and which the
  compiler then checks. `user-action-log-repository.hxx`, the third repository
  in the same folder, is untouched, and it cannot be annotated alone: its four
  sync methods override `Syncable`'s virtuals in `contracts/sync`, so
  annotating them means annotating the contract and every `Syncable`
  implementation in the tree (identity, camera, productivity, notification …).
  That is a unit of its own rather than this one's tail.
- **`guard.journal_retention_days` does not behave the way its own doc row
  says.** `docs/operations/configuration-keys.md:224` documents the key as
  "Values <= 0 keep every row", but guard resolves it through
  `configIntOr` (`services/guard/src/main.cc:73-77`), which is
  `value > 0 ? value : fallback` — so a configured `0` or a negative value
  means 90 days, not "keep everything". The two retention keys in the tree
  therefore now answer `<= 0` differently, and the sync one answers it the way
  the doc already describes. Fixing guard is guard's unit (it is a behaviour
  change and owes its own tests and review); the mismatch is recorded here
  because calibrating this unit's resolution is what surfaced it.
- **The two writers' `updateChanges` methods are gone.** The recon for this
  step recorded them as dead code beside the change — `UPDATE audit_log SET
  changes = ?, event_timestamp = ? WHERE id = ?` and its user twin, zero call
  sites, leftovers of the delete-then-insert merge order. `compactRow` is the
  method the sweep needs, so the dead pair and their `UPDATE_CHANGES`
  constants were deleted rather than kept next to it.
- **The sweep keeps `[this]` and no drain.** `guard`'s retention sweep captures
  a `shared_ptr` lifecycle by value in its tick and the guard process drains the
  in-flight tick through `shutdown_signal::onStop`; this sweep does neither, and
  `argus-sync` registers no drain for any leg. Unreachable today (a `main()`
  local declared before `app().run()`, and nothing resumes a suspended frame
  after `quit()` returns), but it is a documented precedent only half adopted:
  adopting it means giving `argus-sync` a drain of its own, which is the
  service's unit and not this one's tail.
- **The sweep's timer leg is exercised only in production.** No test
  instantiates `AuditRetentionService`: `start`, `stop`, `clearTimer`,
  `runSweep`, the `running_` re-entry guard and the `now - days * 86'400'000`
  arithmetic are covered by inspection and by the code path the service drives,
  while the compaction itself — the part that can destroy history — is covered
  by the test above. A test of the timer leg needs a live loop and a 30 s
  warmup, so it was declined rather than faked; the arithmetic is the one piece
  the test recomputes in its own words instead of calling the service.

## Files

- `packages/contracts/sync/src/sync/audit-retention.hxx` (new),
  `src/sync/sync-errors.hxx` (the eighth refusal), `CMakeLists.txt` (the header
  and the new test target), `tests/unit/audit-retention-test.cc` (new),
  `tests/unit/sync-contract-catalog-test.cc`.
- `services/sync/database/schema.sql` (`audit_compaction_state`).
- `services/sync/src/feature/fanout/services/audit-retention-service.{hxx,cc}`
  (new), `audit-log-service.{hxx,cc}`, `user-audit-log-service.{hxx,cc}`,
  `CMakeLists.txt`.
- `services/sync/src/shared/repositories/audit-log/*` and
  `.../user-audit-log/*`: the four compaction queries, `findCompactionPairs`,
  `findCompactionChanges`, `findCompactionFrontier`, `compactRow`,
  `removeMany`, `advanceCompactionFrontier`, and the three parameter structs.
- `services/sync/src/feature/transport/services/synchronized-service.cc` (the
  two gates).
- `services/sync/src/config/sync-config.{hxx,cc}` + `CMakeLists.txt` (the
  window's resolution and the contract it reads), `src/app/main.cc` (construct,
  capture, start).
- `services/sync/config.toml.example`, `argus-deploy/config.sync.toml.example`,
  `docs/operations/configuration-keys.md`.
- `services/sync/tests/CMakeLists.txt`,
  `tests/unit/audit-compaction-test.cc` (new),
  `tests/unit/audit-sync-read-test.cc`.
- Docs: `docs/architecture/wire-sync-tables.md`, `sync-engine.md`,
  `data-storage.md`, `packages/contracts/sync/AGENTS.md`,
  `services/sync/AGENTS.md`, `services/sync/CONTEXT.md`, root `AGENTS.md`, the
  plan.
- `scripts/lib/tidy-baseline.txt` — the close's re-record, described in
  Verification.
