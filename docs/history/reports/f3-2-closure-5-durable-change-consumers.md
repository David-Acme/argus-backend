# Closure item 5 — every change subject gets a durable consumer that outlives its process

Phase 3a, step 2's closure list, item 5 (S4). The row asked for a consumer per
change subject or a reason none is owed. Answering it took four repairs, in the
order measuring them found them: the sync leg and the memory replica were core
subscriptions rather than durables; the durables the tree already held did not
survive a graceful shutdown; a durable backlog made a latent audit-merge race the
normal case; and the bus had two use-after-frees that the new tests reached. The
adversarial review then found what a real backlog does to the boot order — every
consumer bound before `run()` crashed on its first delivery — and three more
repairs, all in "What the review found".

## What the row said

> **S4 — no durable consumer for any change subject.** Recorded as a limitation
> in every producer unit, never as a decision. Either a consumer exists or the
> plan says why none is owed. The API `NatsBus` exposes cannot detect a lost
> durable consumer (no callback, no query), which is also why the stream re-arm
> of 3a-2g/2h deliberately did not un-gate the *subscription*.

At `HEAD`, `sync_fan_out::subscribeChangeFanOut` held one core-NATS wildcard
subscription (`argus.*.v1.change`) and `subscribeActionJournal` a second one on
the journal subject; argus-memory's `CatalogReplica` held three core
subscriptions (identity, camera, and the same wildcard filtered to
`camera_stream`). All of them matched what was published while their process was
up and nothing while it was not, although every producer's stream already
retained seven days of it.

## The consumers as built

**argus-sync.** `ChangeFeedConsumer` holds one durable per change stream, from
`change_feed::defaults()`:

| Stream | Durable | Subject |
|---|---|---|
| `ARGUS_CAMERA` | `argus-sync-camera` | `argus.camera.v1.change` |
| `ARGUS_NOTIFICATION_CHANGE` | `argus-sync-notification` | `argus.notification.v1.change` |
| `ARGUS_PRODUCTIVITY_CHANGE` | `argus-sync-productivity` | `argus.productivity.v1.change` |
| `ARGUS_IDENTITY_CHANGE` | `argus-sync-identity` | `argus.identity.v1.change` |
| `ARGUS_IDENTITY_CHANGE` | `argus-sync-identity-action` | `argus.identity.v1.user-action` |

A durable filter names one subject inside one stream, which is why the identity
stream carries two durables and why the wildcard has no durable form. Each
attaches with `deliverAll = false` — deliver-new, so the first boot after this
landed does not replay a week of changes the old subscription had already
applied into duplicate audit rows — `maxDeliver = 50`, and a 5 s attach retry
while its stream does not exist yet. The stream names moved into
`nats-subject.hxx` beside their subjects, so a producer's `ensureStream` and the
consumer's attach read one constant.

**argus-memory.** `CatalogReplica` holds `argus-llm-catalog-camera` on
`ARGUS_CAMERA` and `argus-llm-catalog-identity` on `ARGUS_IDENTITY_CHANGE`
(`catalog_feed::defaults()`), with `deliverAll = true`: its applies are
idempotent upserts and deletes, so the replay a from-scratch consumer performs
is the backfill that heals a replica the snapshot fill never re-seeds. The
wildcard went too: it existed for `camera_stream` rows, which in the gateway era
were written through `/sync`. Today nothing publishes a `camera_stream` change
at all — camera's repository for the table is read only by its sync
`PullTable` — and the day something does it is camera's row on camera's
subject, which the camera durable carries and `applyCamera` already applies.
`argus-llm`'s `main.cc` now builds the replica whenever `nats.url` is set, so a
bus that connects later still attaches.

**Settlement.** Both consumers settle three dispositions
(`DurableDisposition`, shared by sync's change feed and delivery consumer):
**ack** when a frame is applied or knowingly ignored, **term** when it will never
parse, **nak** when the write threw, so the broker redelivers it. At `HEAD` the
fan-out ran its inserts under `drogon::async_run` with a catch that logged
"dropped"; `AuditFanOut`'s writers are now coroutines that propagate, and the
failure becomes a redelivery instead of a loss.

**The journal's redelivery guard.** `user_action_log` is append-only and does
not converge the way the audit tables do, so it is keyed: `msg_id` carries the
producer's `Nats-Msg-Id` under a partial unique index (`WHERE msg_id <> ''`)
and the insert is `INSERT OR IGNORE`, the ignored insert surfacing as
`std::nullopt` and an ack. `UserActionLogSchema::toJson` does not carry the
column, so the frozen wire is unchanged. `CREATE TABLE IF NOT EXISTS` cannot
widen a live table, and the new index is refused by a database that predates
the column, so the beginning advice adds it first with a guarded
`ALTER TABLE ... ADD COLUMN` — the shape camera, notification and guard already
use. The first draft answered this with rule 17b's explicit reset; the review
pointed out that sync's file is still `identity.db` until Phase 3c-2, so that
reset would take users, persons and faces with it.

## The durables were not durable

The delivery consumer, guard's object-event consumer and llm's encounter
consumer already attached through `NatsBus::subscribeDurable`, and the wire
document promised what that bought: a consumer that was down drains the backlog
instead of losing it. Measured, it did not.

cnats deletes a consumer that **its own subscribe call created** as soon as that
subscription is unsubscribed or drained. `js_Subscribe` sets `jsi->dc = true`
only in the branch where it creates the consumer (`js.c:3035`), and both
`natsSubscription_Unsubscribe` and the drain path call `jsSub_deleteConsumer`
when it is set (`sub.c:769`, `sub.c:1011`) — the flag does not exempt a durable.
`NatsBus::drain()` unsubscribes everything before closing, so the graceful
`SIGTERM` path deleted every durable and the next boot created a fresh one at
the stream head. The bus's own live case shows it against the dev broker: built
with `HEAD`'s `attachDurable`, a message published after an `unsubscribe` and
one published after a `drain` are never delivered to the re-attached
subscriber.

The header prescribes the shape that avoids it — create with `js_AddConsumer`,
then bind with `jsSubOptions.Stream` and `.Consumer` — and that is the fix:
`NatsBus::ensureDurable` creates the consumer (tolerating
`JSConsumerNameExistErr` and `JSConsumerExistingActiveErr`), `attachDurable`
binds, cnats never sets `dc`, and a teardown leaves the consumer and its cursor
in place. It is in the bus, so it covers every durable at once.

Measuring the create-then-bind path corrected what the first draft of this unit
documented. `js_AddConsumer` is create-or-update (`js_UpdateConsumer` is the
same call): on an existing durable the editable fields follow the code —
`maxDeliver` 5 → 50 is applied, and the deliver subject moves to the new inbox
once nothing is bound to the old one — while the deliver policy is fixed at
creation. Asking for a different `deliverAll` is refused with "deliver policy
can not be updated" (`10012`) and the attach fails on every retry, so a feed
whose policy changes takes a new durable name. A second process cannot bind
while one is bound: its create is refused with `10013` ("consumer name already
in use", tolerated) and its bind with "consumer is already bound to a
subscription", until the first has left; its retry then resumes from the shared
cursor. The bus's warnings now carry the broker's reason through
`nats_GetLastError`, so each of these refusals says why in the log.

## A durable backlog is a burst, and the audit merge was not safe under one

`AuditLogService::create` merges a change into its `(record, table, UTC day)`
row as a read, a delete and an insert — three statements across three awaits —
and the durable handler started every delivery's coroutine as soon as it
arrived. Two changes to one record applied together both read the same row and
both insert. Measured through the real handler: a 20-change burst for one record
left **17 rows**, and the surviving diff ended at `v3` instead of `v20`, so a
client replaying the audit converges on the wrong value. The race predates this
unit (the wildcard subscription ran the same fire-and-forget coroutines), but a
durable turns it from a coincidence into the normal case — the backlog a restart
drains is exactly a run of queued changes.

`durable_delivery::handler` now applies each feed **one message at a time, in
arrival order**: deliveries queue on IOLoop 0, a drain coroutine runs the next
only after the previous one has settled, and it hops back to IOLoop 0 with
`switchThreadCoro` whenever an await resumed it on the database thread, so the
queue is only ever touched on one thread. The single SQLite connection already
serialized the statements themselves, so nothing is lost in throughput. The
same burst now lands as one row, `v0 → v20`. Room emits did not need the hop:
`RoomManager` posts every emit onto each IO loop itself.

**What the serial apply does not give is order across a redelivery**, and the
unit records that rather than solving it: a nak'd message, or one whose ack
never arrived because the process stopped mid-apply, is redelivered after the
messages behind it and can regress a merged `current`. Before this unit such a
message was lost. The fixes each put a cost on every message, so it is closure
item 5b — a decision — in the plan.

## Two use-after-frees in the bus

Both were latent at `HEAD`; the serial apply and the new live case widened their
windows enough to crash.

- **An ack after its subscription was destroyed.** A durable message is
  settled later, from the loop, through closures that hold the `natsMsg`;
  `natsMsg_Ack` reaches `msg->sub->conn`. A consumer's `stop()` destroys the
  subscription, so an ack in flight at that moment reads freed memory —
  `notification-delivery-live-test` crashed with `SIGSEGV` in
  `natsMutex_Lock` under `natsMsg_Ack` once its consumer stopped between a
  dispatch and its settlement. The durable subscription is now a
  `SharedSubscriptionPtr`, and the message's deleter owns a reference, so the
  subscription (and, through cnats's own reference, its connection) lives until
  the last message is settled and destroyed; a late ack fails on a closed
  connection.
- **A closed callback after the bus was destroyed.** cnats delivers the closed
  callback on its own async thread, later, with the bus as the closure; a bus
  destroyed right after `drain()` was then locked through a dangling pointer
  (`SIGABRT`, `std::mutex::lock` → `EINVAL`, in `NatsBus::onClosed`). The bus
  now counts the connections it opened, `onClosed` always decrements and
  notifies, and `drain()` waits — bounded at 5 s, with a warning if it expires —
  until every one has reported closed. In a service this is the exit path after
  `run()` returns.

**The shutdown order in sync's `main.cc`** had the same class of defect one
level up: the bus was declared after the two consumers, so it was destroyed
first and each consumer's destructor called `bus->unsubscribe` through a
dangling pointer. The bus is now declared first and destroyed last.

## Every change subject, and who consumes it

| Subject | Consumer |
|---|---|
| `argus.camera.v1.change` | `argus-sync` (`argus-sync-camera`), `argus-memory` (`argus-llm-catalog-camera`) |
| `argus.notification.v1.change` | `argus-sync` (`argus-sync-notification`) |
| `argus.productivity.v1.change` | `argus-sync` (`argus-sync-productivity`) |
| `argus.identity.v1.change` | `argus-sync` (`argus-sync-identity`), `argus-memory` (`argus-llm-catalog-identity`) |
| `argus.identity.v1.user-action` | `argus-sync` (`argus-sync-identity-action`) |
| `argus.notification.v1.delivery` | `argus-sync` (`argus-sync-delivery`) |
| `argus.camera.v1.object_detected` | `argus-guard` (durable) |
| `argus.guard.v1.encounter_closed` | `argus-llm` (`argus-llm-encounters`) |
| `argus.sync.v1.change` | **none owed** — nothing publishes it: every domain publishes on its own subject, and the constant's remaining readers are suites. The wire document's row ("Publisher: every mutating service / Consumer: argus-sync (F3-1c)") was stale and now says so |

`argus.guard.v1.heartbeat` and `argus.notification.v1.push_intent` are core-NATS
notifications with no durable form by design (a heartbeat's value is its
freshness; a push intent mirrors an already-persisted row), and neither is a
change subject.

## What proves it

- `packages/lib/nats/tests/unit/nats-wrapper-test.cc`, "a durable consumer
  outlives the subscriber that bound it" (live, `ARGUS_TEST_NATS_URL`): the
  backlog survives an `unsubscribe` and a `drain`, a second binder is refused
  while the first is bound and takes over once it has left, and a changed
  deliver policy is refused. Rebuilt against `HEAD`'s `attachDurable` in a
  scratch mutant it fails exactly on the backlog — "two" after the unsubscribe
  and "three" after the drain are never delivered — and on the refusal, which
  the old bus accepted because it had deleted the consumer.
- `services/sync/tests/unit/change-feed-live-test.cc` (live, `ARGUS_NATS_URL`):
  the same property through `ChangeFeedConsumer` — detach, publish, require the
  change not applied, re-attach, require it applied.
- `services/sync/tests/unit/change-feed-consumer-test.cc`: the feed table, the
  routing and settlement of every subject over a real database (module audit
  converging on one row, journal redelivery ignored on `msg_id`, the identity
  catalog kind acked without a write, `Term` for malformed payloads), and the
  20-change burst through `durable_delivery::handler` landing as one `v0 → v20`
  row — 17 rows before the serial apply.
- `notification-delivery-live-test`, which crashed on the late ack, passes
  three runs out of three with the shared subscription.
- `packages/memory/tests/unit/memory-replica-test.cc` pins the replica's feed
  table, applies the `camera_stream` and `camera` rows through `applyCamera`,
  and requires a write against a missing table to throw (the nak path) rather
  than be acked.
- `change-feed-consumer-test.cc` also boots on a legacy `user_action_log` —
  `HEAD`'s DDL with a row in it — and requires the migration to add `msg_id`
  and its index, keep the row, and be a no-op before the table exists and on a
  second run.
- The bus's live case also requires the `Nats-Msg-Id` a message was published
  with to reach the handler, and the bound subscriber to keep receiving after a
  rival's attach was refused.

## Recorded, not fixed

- **Order across a redelivery, immediate naks, the non-atomic merge** —
  closure item 5b, one decision.
- **The retry timers call the bus on the main loop.** `subscribeDurable` makes
  two JetStream requests under the bus mutex, and the change feed's and the
  catalog replica's 5 s retries run it from a loop timer — the shape the
  delivery consumer already had at `HEAD`. It runs only while a stream is
  missing (both timers now stop once every feed has attached), and a local
  broker answers in about a millisecond. A producer that is never deployed
  keeps its feed's warning coming every 5 s.
- **The journal's key is unique only while identity and sync share a file.**
  `identity-action:<change_outbox id>` never repeats inside one `identity.db`
  (`AUTOINCREMENT`), but once Phase 3c-2 lets identity's file be reset alone
  its ids restart and the unique index would drop real rows; the plan's 3c-2
  row now says the split re-keys it.
- **The dev broker keeps every live suite's stream.** The suites name their
  streams per process and never delete them; the dev broker held 2168 of them
  when this unit measured it (2208 by the review), and a durable now stays
  with each. Item 6 touches the same suites.
- **A stale gateway binary.** `services/gateway/build/dev/notification-delivery-live-test`
  (2026-09-22) fails against the broker; its source left with the delivery
  consumer in 3a-1c and nothing builds it any more.

## Verification

- Full gate `./scripts/build-all.sh dev`, exit 0: check-comments 1251 files, 0
  comments; check-deps 64 declarations, 505 edges, 0 forbidden, 0 cycles; every
  project's suite at 100% — cert 2, sqlite 2, identity 29, memory 22, intent 4,
  gateway 34, sync 49, camera 55, productivity 39, notification 44, guard 57,
  tts 23, stt 8, vlm 9, llm 35, voice 26, tunnel 12 — with no first-party
  warning; check-tidy 508 TUs, 3050 findings over 45 checks against the 3141
  baseline, 7 checks below it and none risen. The run before the review's
  fixes had failed on four risen checks; see finding 9.
- The live suites, run by hand against the dev broker with both
  `ARGUS_NATS_URL` and `ARGUS_TEST_NATS_URL` set, in every project that
  builds them: `nats-wrapper-test` (11 cases, 92 assertions), sync's
  `change-feed-live-test` (11) and `notification-delivery-live-test` (32),
  camera's two outbox sinks (48, 26), identity's outbox sink (115),
  notification's sink (48) and delivery suite (21), productivity's sink (68),
  guard's three (7, 9, 18) and llm's `encounter-closed-live-test` (22). The
  burst, the delivery suite and the bus's live case were each run three times.
- The bus's live case against `HEAD`'s `attachDurable` in a scratch mutant:
  fails on the backlog after an unsubscribe and after a drain, and on the
  refused policy change.

## Files

- `packages/lib/nats/src/nats/nats-bus.{hxx,cc}` — `ensureDurable` and the
  bind, the `Nats-Msg-Id` read, `SharedSubscriptionPtr`, the closed-callback
  count and the broker's reason in the warnings; `nats-subject.hxx` — the
  stream names, the wildcard constant removed.
- `services/sync/src/feature/fanout/services/change-feed-consumer.{hxx,cc}`,
  `durable-disposition.hxx`, `durable-delivery.hxx` (the serial feed),
  `sync-fan-out.{hxx,cc}`, `audit-fan-out.{hxx,cc}`,
  `notification-delivery-consumer.{hxx,cc}`, `src/app/main.cc`,
  `database/schema.sql`, `src/shared/repositories/user-action-log/*`.
- `packages/memory/src/memory/catalog-replica.{hxx,cc}`, `services/llm/src/main.cc`.
- The producers' sinks and camera's event stream read the stream names from
  `nats-subject.hxx`.
- Tests: `nats-wrapper-test.cc`, `sync-change-test.cc`,
  `change-feed-consumer-test.cc`, `change-feed-live-test.cc`,
  `memory-replica-test.cc`, `audit-sync-read-test.cc`,
  `notification-delivery-{inbox,live}-test.cc`, `services/sync/tests/CMakeLists.txt`.
- Docs: `packages/lib/nats/AGENTS.md`, `services/sync/{AGENTS,CONTEXT}.md`,
  `packages/memory/CONTEXT.md`, `docs/architecture/{wire-nats-subjects,
  sync-engine,events-and-contracts}.md`, the plan (row 5 done, row 5b added).

## What the review found

Two reviews. The first was cut off by the session ending before it reported, but
the probes it had run against the broker were in its transcript; reproduced by
hand, they are what corrected the first draft's claim that an existing durable
keeps its config (it takes the editable fields and refuses a new deliver
policy). The second reviewed the final tree and left twelve findings. Each was
checked against the code before anything changed.

1. **Bug, fixed — a backlog at bind crashed the process, on every restart.**
   The handlers marshal with `drogon::app().getIOLoop(0)`, which is null until
   `run()` creates the pool (`HttpAppFrameworkImpl.cc`), and sync's two
   consumers, llm's replica and llm's encounter consumer all bound before
   `run()`. With a real backlog the first message arrives within a millisecond
   of the bind, is never acked, and crashes the next boot too. Guard already
   started its consumer from a beginning advice; the other four now do, and in
   sync that advice is also the first moment the schema exists.
2. **Bug, fixed — the replica's replay starved its snapshot fill.** The fill
   skips any table that holds a row, and the replay landed first, so a fresh
   replica kept the handful of rows the retained window carried. The replica
   now subscribes after the fill, from the same advice, whether the fill
   succeeded or not.
3. **Bug, fixed — every existing install would fail to boot argus-sync**, and
   the documented remedy reset identity's data. Fixed with the guarded
   `ALTER` above, pinned from a legacy table.
4. **Risk, bounded — the ack window can expire behind the serial queue.** The
   reviewer's probe (400 messages at 200 ms per apply) applied 99 twice. Every
   durable now holds at most 256 unacknowledged messages — measured, the broker
   applies that to an existing consumer — and the remaining window is part of
   item 5b.
5. **Risk, recorded — the audit merge is not atomic.** Delete and insert are
   separate statements; pre-existing, and part of item 5b.
6. **Risk, fixed — the replica acked failed writes.** `SqliteStmt` never
   throws, so a failed upsert was acked; the apply path now throws on a failed
   statement and the message is nak'd, pinned by the replica test.
7. **Risk, recorded — a nak is immediate**, so a fast-failing write spends
   `maxDeliver` in milliseconds and the broker stops without a dead letter;
   item 5b. The "no such table" source the review named is gone with finding 1.
8. **Rules, fixed.** `handleActionPayload` takes an input struct (rule 2),
   `ChangeFeedConsumer::Config` lists every member, and the inbox is owned by a
   deleter instead of by hand (rule 16).
9. **The gate, fixed.** The run the review saw failed the tidy ratchet on four
   checks; the new findings were this unit's (two optional conversions, two
   unchecked reads in the new test, one `auto`, two by-value parameters), and
   six more in the files it touched were cleared with them.
10. **Notes.** The retry timers now stop once every feed has attached; the
    main-loop requests and the warning of a never-deployed producer are
    recorded above.
11. **Tests, added.** The `Nats-Msg-Id` read and the bound subscriber after a
    refused rival are now in the bus's live case; the leaked streams go to item
    6 and the journal key to Phase 3c-2.
12. **A stray `go2rtc.yaml`** at the repository root carries RTSP credentials; it
    is untracked, was not committed, and is not this unit's.

What the review confirmed by its own measurement: the cnats deletion mechanism
and the create-then-bind fix (a mutant of the old attach fails both live
cases), the late ack (an ASan probe of the old ownership crashes in `_ackMsg`),
the balance of `openConnections_` on every path, the serial apply (the
non-serial mutant failed 5 of 5 with 15–17 rows), and that nothing publishes
`camera_stream` or `argus.sync.v1.change`.
