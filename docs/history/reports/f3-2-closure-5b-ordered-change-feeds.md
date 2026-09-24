# Closure item 5b — each change feed stays in order across a redelivery

Phase 3a, step 2's closure list, item 5b: added by item 5's own measurement,
and a decision rather than a repair.

## What the row said

Item 5 made every change feed durable and applied each one message at a time,
in arrival order. That order held only until something was redelivered. A
message that was nak'd (its write threw), whose ack never arrived (the process
stopped mid-apply) or whose ack window expired while it waited came back after
the messages behind it had been applied. The audit merge then folded an older
diff into a row that already held a newer value and could regress its
`current`; the memory replica's upserts could regress a row the same way.
Before item 5 such a message was lost instead. Two neighbours belonged to the
same decision: a nak was immediate, so a fast-failing write spent `maxDeliver`
in milliseconds and the broker then stopped without a dead letter; and the
audit merge deleted the old row before inserting the merged one, so a failure
between the two lost the day's merged history.

## The decision: order wins over liveness on a change feed

A wrong value on a client stays there silently until the field changes again.
A stalled feed is visible and resumes by itself. For the change feeds, whose
diffs a client replays in order to reach the current row, the first is the
worse failure, so those feeds are ordered and the stall is what they pay.

**Ordered consumers.** `NatsBus::DurableInput` gained `maxAckPending`, with two
named values: `kDefaultMaxAckPending` (256) and `kOrderedMaxAckPending` (1).
Every `DurableInput` in the tree names one of them, and the two feed tables
carry theirs per feed (`change_feed::Feed::maxAckPending`,
`catalog_feed::Feed::maxAckPending`). Six feeds are ordered: sync's camera,
notification, productivity and identity change feeds, and memory's two catalog
feeds. On those the broker delivers nothing behind a message until it is acked
or given up on. The feeds keyed by an id keep 256, because their receipts make
order irrelevant: the delivery consumer, llm's encounters, guard's object
events, and the action journal, whose rows are keyed by `msg_id` and inserted
verbatim. Measured on the dev broker, with `a b c` published and the first
delivery of `a` nak'd with a delay:

| In flight | Applied |
|---|---|
| 256 | `a b c a` |
| 1 | `a a b c` |

**Backoff.** Every nak but the last now waits before the redelivery
(`natsMsg_NakWithDelay`): 1 s, doubling per delivery to a 30 s cap. The nak of
the last delivery is immediate, since the broker drops the message then and a
delay would only hold an ordered feed longer, and it logs an error because the
broker keeps no dead letter. The change feeds allow 10 deliveries, so a message
that can never be written holds its feed for 1 + 2 + 4 + 8 + 16 + 30 × 4 =
151 s. On a one-in-flight consumer the broker then moves on: a poison `m1` with
three deliveries is applied `m1 m1 m1 m2 m3`.

**The merge.** `AuditLogService::create` and `UserAuditLogService::create`
insert the merged row before deleting the one it replaces, and `FIND_EXIST`
takes the newest row: `ORDER BY id DESC` over `INDEXED BY idx_*_record`. The
tree never runs `ANALYZE`, so without the hint SQLite 3.53.4 picked the
timestamp index and sorted the day's rows. With the hint it searches
`(record_id, table_name)` and walks it newest-first, with no sort. If the insert
fails, nothing was deleted and the redelivery merges again. If the delete
fails, an older, redundant row stays behind the merged one. A client pages both
in id order and ends on the merged value (the app's `applyAuditLogs` applies
`current` in id order), and later merges go into the newest row.

## What it costs

- **Throughput is one round trip per message.** 2000 messages in about 2 s
  with an empty apply, measured twice: about a thousand a second, so a day's
  backlog drains in seconds.
- **A failing message stalls its domain's live changes for 151 s**, then is
  dropped with an error in the log. Before item 5 it was dropped on the first
  failure, without a log. A last delivery that ends by an expired ack window
  rather than a nak is dropped without the log.
- **A delivery lost while its subscription survives waits out the 60 s ack
  window.** That is a message in flight when the connection drops and cnats
  reconnects on the same subscription. A process that dies does not wait:
  measured, its successor re-attaches through a fresh deliver subject and gets
  the dead process's message 4 ms after the bind, ahead of the rest.
- **A failure between the merge's two statements leaves a redundant row.**

Considered and not taken:

- **A merge that compares event timestamps.** It would fix the audit leg only,
  and it needs per-field timestamps the row does not keep.
- **A merge in one transaction.** drogon issues the commit from the
  transaction's destructor, reporting it only through a callback, so the ack
  could precede a commit that then fails.
- **A nak of the queued tail on stop.** A one-in-flight feed does not need it.

## What proves it

- `packages/lib/nats/tests/unit/nats-wrapper-test.cc`, "an ordered durable
  redelivers a nak'd message before the next one" (live): `a b c` with the
  first `a` nak'd is applied `a a b c`, and the redelivery comes after the
  backoff (at least 900 ms).
- `services/sync/tests/unit/change-feed-consumer-test.cc` and
  `packages/memory/tests/unit/memory-replica-test.cc` pin each feed's in-flight
  value. The consumer test also forces the merge's insert to fail with a
  temporary trigger and requires the old row and its value to survive. It then
  forces the delete to fail and requires the redelivery of the same step to
  land on the newest row unchanged, and the next step to carry the full
  `v0 → v4` diff.
- The broker probes: ordering at 256 and 1, the poison message, the
  throughput, `MaxAckPending` applied to an existing consumer, and the unclean
  stop.
- Every other live suite still passes with the backoff: guard's dead letter
  after two or three deliveries, llm's poison encounter, and the delivery
  consumer's poison path.

## Verification

- **Full gate** (`./scripts/build-all.sh dev`), exit 0:
  - check-comments: 1251 files, 0 comments.
  - check-deps: 505 edges, 0 forbidden, 0 cycles.
  - Every project's tests at 100%, with no first-party warning: cert 2,
    sqlite 2, identity 29, memory 22, intent 4, gateway 34, sync 49, camera 55,
    productivity 39, notification 44, guard 57, tts 23, stt 8, vlm 9, llm 35,
    voice 26, tunnel 12.
  - check-tidy: 508 TUs, 3051 findings against the 3141 baseline, 7 checks
    below it and none risen.
- **Every live suite**, run against the dev broker on the final binaries in
  every project that builds it:
  - `nats-wrapper-test`: 12 cases, 100 assertions, in ten copies.
  - Identity's outbox sink: 115, in six copies.
  - Camera's two sinks: 48 and 26.
  - Guard's three: 7, 9 and 18.
  - llm's `encounter-closed-live-test`: 22.
  - Notification's sink (48) and delivery suite (21).
  - Productivity's sink: 68.
  - Sync's `notification-delivery-live-test` (32) and
    `change-feed-live-test` (11).
- **Offline suites:** `change-feed-consumer-test` passes at 91 assertions and
  `memory-replica-test` at 97.

## Files

- `packages/lib/nats/src/nats/nats-bus.{hxx,cc}`: `maxAckPending` and its two
  values, the backoff with its immediate last nak, and the last-delivery error.
- `services/sync/src/feature/fanout/services/`: `change-feed-consumer.{hxx,cc}`
  (the per-feed value), `notification-delivery-consumer.cc`,
  `audit-log-service.cc`, `user-audit-log-service.cc`.
- `services/sync/src/app/main.cc` and
  `src/shared/repositories/{audit-log,user-audit-log}/*-query.hxx`.
- `packages/memory/src/memory/catalog-replica.{hxx,cc}`,
  `services/llm/src/shared/services/encounter-closed/encounter-closed-consumer.cc`,
  `services/guard/src/feature/guard/guard-service.cc`.
- Tests that build a `DurableInput` or a feed: `nats-wrapper-test.cc`,
  `identity-change-outbox-sink-test.cc`, camera's `change-outbox-sink-test.cc`,
  `notification-change-outbox-sink-test.cc`,
  `productivity-change-outbox-sink-test.cc`, `guard-dlq-live-test.cc`,
  `change-feed-live-test.cc`.
- `change-feed-consumer-test.cc` and `memory-replica-test.cc`. The fixture in
  `audit-sync-read-test.cc` now creates the two record indexes the real schema
  has.
- Docs: `packages/lib/nats/AGENTS.md`, `services/sync/CONTEXT.md`,
  `packages/memory/CONTEXT.md`, and the plan (row 5b done).

## What the review found

An adversarial review measured the change against the broker and found no
ordering defect: order held on every path it drove, including an ack-wait
expiry, a same-inbox re-bind, a fresh-inbox re-bind after a SIGKILL, and a
redelivery queued behind a slow first delivery. Its findings, each checked
against the code before anything changed:

1. **Fixed: the last nak waited its delay.** The broker drops the message only
   when that delay expires, so a poison message held its feed 181 s, not the
   151 s the docs claimed, and the error was logged 30 s before the feed moved.
   The last nak is now immediate.
2. **Fixed (docs): the unclean-stop cost was wrong.** A successor re-attaches
   through a fresh deliver subject and gets the in-flight message at once;
   reproduced here (4 ms). The 60 s wait belongs to a delivery lost while its
   subscription survives, and the docs now say so.
3. **Fixed: the merge lookup sorted.** The report's first draft claimed an
   index served `ORDER BY id DESC`; `EXPLAIN QUERY PLAN` against the linked
   SQLite showed the timestamp index and a temp B-tree, and 27 ms instead of
   2.6 ms over 50k rows in one day. `INDEXED BY idx_*_record` removes the sort.
4. **Fixed: the action journal was ordered for nothing.** It is keyed by
   `msg_id` like the other id-keyed feeds, so it paid the poison stall with no
   ordering benefit. The feed tables now carry a per-feed value, and the
   journal keeps 256.
5. **Fixed: two test gaps.** Nothing pinned that the change feed and the
   replica request one in flight; both feed tables' values are now checked.
   The delete-failure case now redelivers the same step, not the next one.
6. **Fixed (docs):** the error log covers a nak'd last delivery only.
7. **Fixed: a string copy per message.** The durable name is now copied only
   for a last delivery.
8. **Nits.** Two over-long lines and a doubled parenthetical were fixed. The
   leaked live-suite streams are item 6's.
