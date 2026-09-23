# Closure item 4 — settled rows get a lifetime, and it is the stream's

Phase 3a, step 2's closure list, item 4 (S5). One decision, six tables, one
declaration.

## What the defect was

Every outbox marked a row `sent` on its PubAck and then kept it for ever. The
row's only remaining reader is the replay guard — the fingerprint lookup that
makes a repeated transition a no-op instead of a second publish — so nothing
ever removed it and each table grew with its feed, without bound and without a
reader that would notice. The 3a-2c unit recorded it as B4 rather than fixing
it alone:

> A daily purge of rows older than the stream's own retention (7 days here) is
> the obvious shape, but it must be the same decision in all four databases or
> an operator will find four different lifetimes under the same table name.

That is the whole requirement: one policy, and the same one in every producer.

## The decision

**A settled row lives exactly as long as the stream that carries it.** Every
producer's `change_outbox`, camera's `object_event_outbox` and guard's
`guard_encounter_outbox` delete their `sent` (and `overflow_dropped`) rows once
they are older than the feed's window, on a daily sweep. Keeping them shorter
than the stream would let a re-derived transition re-publish while the broker
could still have folded it; keeping them longer is pure growth. The window is
the stream's, so the two agree by construction and neither has to be reasoned
about per table.

The window itself was declared seven times — once in each producer's
`ensureStream`, once in camera's `event-stream`, once in the guard's stream
arm, once in the object sink's cooldown sweep, and (found by this unit's
review) once more in notification's delivery sink. All seven now read one
header, `packages/contracts/sync/src/sync/stream-retention.hxx`:

```cpp
namespace stream_retention
{
inline constexpr int64_t kRetentionMs = 7LL * 24 * 60 * 60 * 1000;
inline constexpr int64_t kRetentionSeconds = kRetentionMs / 1000;
inline constexpr int64_t kRetentionNs = kRetentionMs * 1000000;
inline constexpr int64_t kDuplicatesNs = 2LL * 60 * 1000000000;
inline constexpr int64_t kSettledPurgeIntervalMs = 24LL * 60 * 60 * 1000;
inline constexpr int64_t kSettledPurgeRetryMs = 60LL * 60 * 1000;
}
```

The header is named for the policy, not for one subject family: the delivery
feed (`argus.notification.v1.delivery`) is a durable feed with the same
retention and no change feed, and the change feeds are not the only readers.
It lives in `packages/contracts/sync` because that package already owns the
change vocabulary all five producers spell — `lib/nats` is tier 1 and holds no
domain data, so the shared constant cannot live there (rule 25's tiers). The
guard's stream arm is its first tier-5 → tier-2 edge, which the tier table
permits; `check-deps.sh` still reports 0 forbidden and 0 cycles.

Three units are involved and the header carries one constant each:

- **milliseconds** (`kRetentionMs`) for the four `change_outbox` tables and
  the object outbox — all stamped with `nowMs()`;
- **seconds** (`kRetentionSeconds`) for `guard_encounter_outbox` — stamped with
  `std::time(nullptr)`;
- **nanoseconds** (`kRetentionNs`, `kDuplicatesNs`) for every stream's
  `maxAgeNs`/`duplicatesNs`.

The mixed-unit failure mode is real, not theoretical: an ms cutoff against
guard's seconds deletes a row that was settled a second ago (verified against
the schema — `changes() = 1`), which is why `stream-retention-test` pins that
`kRetentionSeconds * 1000 == kRetentionMs` and that the ns constant divides
back to the ms one, and why the guard's own suite pins the boundary from both
sides (`updated_at <= 899` deletes nothing, `<= 900` deletes the row).

## The six tables and their predicates

| Table | Predicate | Status value from |
|---|---|---|
| `change_outbox` × 4 owners | `DELETE FROM change_outbox WHERE status = ? AND sent_at <= ?` | `changeOutboxStatusToString(ChangeOutboxStatus::Sent)` |
| `object_event_outbox` (camera) | `DELETE FROM object_event_outbox WHERE status IN (?, ?) AND sent_at <= ?` | `objectEventStatusToString` for `Sent` and `OverflowDropped` |
| `guard_encounter_outbox` (guard) | `DELETE FROM guard_encounter_outbox WHERE status = 'sent' AND updated_at <= ?` | inline literal |

The four producers' statements are the same SQL in four repositories, each
bound with its owner's own status enum (rule 1) and its own client
(`cameraClient()`, `productivityClient()`, `identityClient()`, `client()`).
The object outbox covers both settled statuses because they are exactly the
rows that can never be published again: `NEXT_PENDING` selects `pending` only
and `MARK_SENT` requires `pending`, so no path resurrects either.

The guard's inline `'sent'` is deliberate and recorded: `guard_encounter_outbox`
has no CHECK constraint on `status` (unlike every producer's table), no
`EncounterOutboxStatus` enum exists anywhere in the tree, and the same query
file already inlines `'pending'`/`'sent'`. Rule 1 as written does not reach a
column without a constraint; inventing an enum to satisfy a rule that does not
apply would be the drift the rule exists to prevent.

**Pending rows are never touched, at any cutoff.** A pending row's `sent_at`
is 0, so it sorts below every cutoff, and the `status` clause is what protects
it. The suites pin this the way the review recomputed it: for camera's state
(`a=sent@4000, b=sent@5100, c=sent@5200, d=pending@0`) the purge returns 2 for
a 5150 cutoff and 0 when repeated, a pending-only mutation would return 1, a
status-less one 3, and the pending row is still the head of `pendingBatch(1)`
after a cutoff of 999999. The same shape holds for identity (where the action
journal row's NULL `event_id` and status-guarded settlement make the pending
head the second assertion), the object outbox (`sent` 0 / `overflow_dropped` 0
/ `pending` 5 after the sweep) and guard (`(899) == 0`, `(900) == 1`, then a
freshly enqueued pending row survives `99999`).

## The cadence

Each sink's drain worker owns its purge, so nothing new is scheduled and no
table has two writers:

```cpp
const int64_t now = nowMs();
if (now >= nextPurgeMs_) {
  nextPurgeMs_ = now + stream_retention::kSettledPurgeIntervalMs;
  try {
    const int64_t purged = outbox_.purgeSent(now - stream_retention::kRetentionMs);
    if (purged > 0)
      LOG_INFO << "…: purged " << purged << " settled row(s) past the stream's retention";
  }
  catch (const std::exception& e) {
    nextPurgeMs_ = now + stream_retention::kSettledPurgeRetryMs;
    LOG_WARN << "…: purge failed (" << e.what() << "); the settled rows stay and the purge is retried";
  }
}
```

`nextPurgeMs_{0}` makes the first pass after boot purge, which is also the
recovery path for a host that was off over the window; after that it is one
delete per 24 h per table. The retry arm is the review's finding 2 fixed: the
first shape advanced the gate *before* the statement, so a purge that failed
waited a full day while the log said it retried on the next tick. Now a failed
purge re-arms at `kSettledPurgeRetryMs` (an hour) — it actually retries, the
message says what happened to the rows, and a persistent failure is loud
hourly instead of once a day.

Guard's purge rides its existing daily sweep rather than a new timer, and it
sits *before* the `!storage_.isConfigured()` early return: the outbox is a
database concern and its sweep must not depend on object storage being
configured. A failing sweep is already caught and logged by the sweep's own
wrapper, so guard's shape did not change.

## What is deliberately excluded

`notification_delivery` and `guard_action_outbox` are **not** in the policy,
and the reason is the same for both: they are product records that their own
features read back — a delivery intent whose settlement the notification
feature inspects, a command whose `commandId` the guard looks up for an
outcome — not feed rows whose last reader is a replay guard. Their lifetime
belongs to the feature that reads them, not to the stream that carries them.
The exclusion is recorded in `docs/architecture/data-storage.md`, next to the
policy itself, so the next reader finds the boundary rather than the gap.

No index was added. Every `change_outbox` already carries a `status`-leading
index — `(status, created_at)` in camera, notification and productivity,
`(status, id)` in identity, whose pending batch pages by id — and a daily
bounded delete is not a hot query. Adding a second index for it is what rule
22 warns against.

## The review

The adversarial review returned four findings, all notes, none blocking. Two
were fixed in this unit:

1. **The delivery stream still spelled its own window** (its local
   `kRetentionNs`/`kDuplicatesNs` in `nats-notification-delivery-sink.cc`).
   Found by reading the tree rather than the diff — the file was untouched, so
   "declared once" was false with a seventh copy left standing. Fixed: it now
   reads `stream_retention::`, which is also what made the header's name a
   policy name rather than `change-feed.hxx`.
2. **The purge's failure path promised a retry it did not make** (the gate
   advanced before the statement). Fixed as shown above, with the retry
   cadence added to the declaration.

Two are recorded rather than fixed, and both are now written where a reader
will meet them:

3. **The replay guard is now bounded by the window.** A fingerprint row is
   what makes a repeated transition a no-op; delete it after seven days and a
   transition re-derived *after* the window is a second audit row and a second
   emit rather than a replay (the broker's own dedup is two minutes, so it
   does not fold it either). The id is derived from the row, the transition and
   the payload, so one id published twice is one write arriving twice rather
   than two transitions, and every producer's id is minted inside the domain
   write rather than on a sweep that could re-derive one much later. The
   honest statement is nonetheless that the guarantee is bounded, not
   absolute, so the bound is recorded in `docs/architecture/data-storage.md`
   as the policy's stated consequence instead of being left implicit.
4. **The object sink's health numbers changed meaning.** `objectEvents.sent`
   and `overflowDropped` are live `COUNT(*)` over the table, so they now fall
   back as settled rows age out: they are gauges over the retained window, not
   accumulating counters. Recorded in `services/camera/CONTEXT.md`, next to
   the bullet that already described the boot hydration, so a future alert
   built on them reads them for what they are.

The review could not falsify: that pending rows survive at every cutoff (it
rebuilt each suite's state in `/tmp` and recomputed the counts both ways); that
nothing calls `markSent` before a durable publish (all five drains publish
first, mark after, and guard's refused publish routes to `recordEncounterAttempt`);
that a purge cannot run after the database is gone (the sinks' purge reaches
SQLite through the same post-reset client D23 armed, and the 10 s drain
deadline window is the one that already existed for flush/markSent); that the
cadence cannot spin or hammer; and the rules, line by line.

## Verified

Every affected project was rebuilt and its suites run by the orchestrator, then
run again by hand from the binaries:

| Project | `./scripts/build-all.sh dev --only …` | Suites |
|---|---|---|
| `identity` (the outbox, the sink) | exit 0, 0 warnings | 29/29 |
| `gateway` (hosts identity) | exit 0, 0 warnings | 34/34 |
| `camera` (two streams, two outboxes) | exit 0, 0 warnings | 55/55 |
| `notification` (change + delivery streams) | exit 0, 0 warnings | 44/44 |
| `productivity` | exit 0, 0 warnings | 39/39 |
| `guard` (the encounter outbox) | exit 0, 0 warnings | 57/57 |

Run by hand, after the rebuild:

```
change-outbox-test                       2 cases / 49 assertions   camera
object-event-outbox-test                 1 / 31                    camera
notification-change-outbox-test          2 / 48                    notification
productivity-change-outbox-test          2 / 48                    productivity
identity-change-outbox-test              2 / 60                    identity
guard-saga-test                          1 / 131                   guard
stream-retention-test                    2 / 8                     contracts/sync
```

Gates: `scripts/check-comments.sh` 1245 files / 0 comments.
`scripts/check-deps.sh` 64 declarations, 495 edges, 0 forbidden, 0 cycles,
0 unresolved. `scripts/check-tidy.sh` 505 TUs, 3047 findings over 45 checks
against the 3141 baseline, 5 checks below it and none risen.

## What this unit does not change

The retention decision answers S5 only. The durability of the *consumer* side
(S4, closure item 5) is untouched: the sync fan-out's subscription to the change
subjects is still a plain core-NATS subscription, so a change published while
`argus-sync` is down is retained by the stream for this same seven days and
then delivered to nobody. That is the next item, and the window this unit
declared is what it will have to work within.
