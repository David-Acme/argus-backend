# Phase 5 step 4 — the durability drills

Plan row: `docs/history/plans/architecture-plan.md` "4 | Durability drills:
kill `sync` during a write and confirm the audit diff arrives after restart
(outbox + PubAck)".

## What the row asks for

One claim — an audited write survives the death of the process that consumes
it — and the two mechanisms it names: the producer's **outbox** and the
broker's **acknowledgement**. A claim like that is worth what its failure
modes are worth, and four of them are distinct in this design:

1. the consumer is already down when the write commits, so the message waits
   for a process that does not exist;
2. the consumer dies *holding* a delivery, so the broker has to redeliver it
   into a process that never acknowledged it;
3. the broker itself is down, so no acknowledgement can arrive at all;
4. the producer dies *with* the write, so the queue has to survive a kill it
   could not have drained.

Each has a different answer in the code and none of them is the same drill,
so the instrument runs four: `outage`, `frozen`, `broker`, `producer`.

## 1. The spine, as the code states it

**The write and the queue entry are one transaction.**
`AuthFeatureService::logout`
(`services/auth/src/feature/auth/services/auth-feature-service.cc:416`)
opens a transaction, invalidates the user's refresh tokens, enqueues the
audit action on the *same* `transaction.get()`, and throws
`AuthErrors::ChangeNotRecorded` when the commit does not land. There is no
window in which the domain row exists and the queue entry does not, and a
request that cannot record what it did is refused rather than accepted.

**The acknowledgement gates the ledger.** `AuthActionSink::flush`
(`services/auth/src/feature/session/services/auth-action-sink.cc:117`) calls
`markSent` only after `publishWithMsgId` returns true, and that returns true
only on a JetStream PubAck. A failed publish records an attempt and leaves
the row `pending`; `flushLoop` retries every `kActionRetryMs` (500 ms,
`services/auth/src/app/main.cc:42`), breaks out of its batch at the first
row it cannot publish — so the queue drains in the order it was written —
and purges settled rows only past the stream's 7-day retention. While the bus
is disconnected the flush returns before publishing, so a broker that is
known to be gone costs no attempts.

**The join key is unique on disk.** `change_outbox.event_id` and
`user_action_log.msg_id` are the same `auth-action:<32 hex>` string, and
`user_action_log` carries
`CREATE UNIQUE INDEX idx_user_action_log_msg_id ... WHERE msg_id <> ''`
(`services/sync/database/schema.sql:72`). That index is what turns the
transport's at-least-once into exactly-once: `AuditFanOut::insertAction`
(`services/sync/src/feature/fanout/services/audit-fan-out.cc:66`) inserts
through `INSERT OR IGNORE` and logs `is already recorded; the redelivery is
ignored` when the row is already there.

**The consumer.** The feed is declared beside the other five
(`services/sync/src/feature/fanout/services/change-feed-consumer.cc:36`):
stream `ARGUS_AUTH_CHANGE`, subject `argus.auth.v1.user-action`, durable
`argus-sync-auth-action` — the spellings come from
`packages/lib/nats/src/nats/nats-subject.hxx:27`. The subscription asks for
`maxDeliver = 10` (`change-feed-consumer.hxx:40`) and the bus sets `AckWait`
to 60 s (`packages/lib/nats/src/nats/nats-bus.cc:434`), which is the window
the frozen axis measures.

## 2. The instrument

**Two verbs the runner did not have.** `scripts/native-stack.sh` gained
`sigkill <service>` (the process dies where it stands, no drain) and
`freeze <service>` (SIGSTOP: sockets open, state frozen, released only by
the kill). `kill` stays the graceful SIGTERM path it was. The additions are
in the tree's own runner rather than in the drill so the hard kill is
reproducible by hand, in one command, against the same sandbox everything
else uses.

**The drill**: `scripts/durability-drill.py`, subcommands
`outage | frozen | broker | producer | all | state`. It refuses to run
against a sandbox it cannot identify — every axis opens with the same
`ensure_up` gate, which fails closed when a service answers `/health` but no
process matches this profile's binary path, so no check can pass vacuously
against another stack — and it refuses to start at all when the golden
dataset is absent (§5.5). It drives the write through
`scripts/golden-http.py`'s own seeder and client, reads the two databases
directly, and reads the broker's consumer state over the monitoring endpoint
rather than guessing at it.

## 3. Measured, on one freshly booted sandbox

One sandbox, booted from scratch (`native-stack.sh down`, `rm -rf
build/native-stack`, `up`), seeded with the golden fixture rows, then one
`durability-drill.py all`: **79 checks, 79 passed, 0 failed** — 17 `outage`,
21 `frozen`, 24 `broker`, 17 `producer`.

**`outage` — the consumer is gone before the write (17 checks).** `sync` is
stopped, the audited write is issued, and it is accepted: the outbox row is
`sent` with `event_id=auth-action:31a2ceec…`, on the producer's own subject,
because the *auth* service's job ends at the broker's PubAck. Nothing is in
`user_action_log` while the consumer is down (`0 → 0`), and the broker holds
the message for the process that will read it (`num_pending=1`,
`num_ack_pending=0`). `sync` is restarted, and the restart, the health gate
and the delivery take **0.5 s** together; the row it writes is the logout's
own (`table_name=user`, `action=delete`, `record_id == user_id`,
`ip_address=''`), exactly once, and the broker's held count returns to zero.

**`frozen` — the consumer dies holding a delivery (21 checks).** `sync` is
SIGSTOPed, the write is issued, the broker delivers it and waits
(`num_ack_pending=1`), and the frozen process applies nothing
(`user_action_log 1 → 1` — the one row is the golden fixture's). Past the
broker's window the redelivery happens: measured at **60.1 s** against a
declared 60 s `AckWait`, still `num_ack_pending=1`, so the message is not
merely retried but retried into a consumer that still has not acknowledged
it. `sync` is then SIGKILLed with that delivery in flight, restarted, and the
row that lands is the same `auth-action:26a16835…`: once, with the logout's
own content, no duplicated `msg_id`, and the consumer's
`ack_floor.stream_seq` equals the `stream_seq` the broker held while the
process was frozen (`52`). That last check is the one that pins the
*identity* of the acknowledged message rather than its count.

**`broker` — the acknowledgement that never comes (24 checks).** The broker
container is stopped (found by the port it publishes, not by name), and two
audited writes are issued against it: both answer 204, both are queued
(`sent_at=[0, 0]`), and nothing reaches `user_action_log` (`2 → 2`). The
broker returns **2.6 s** later; both actions are then marked `sent` —
`created_at` `…894466`/`…894581` against `sent_at` `…902596`/`…902597`, so
the queue waited **8.1 s** for the acknowledgement — they drain in the order
they were written (`[3, 4]`), each arriving exactly once, and `auth.db`
reports `ok` afterwards. This axis is also the one that measures the
*absence* of a failure mode: a broker outage costs the write path nothing,
because the write path ends at the outbox.

**`producer` — the producer dies inside a stream of writes (17 checks).**
One thread commits audited writes continuously (`PATCH /auth/logout`, each
one a fresh session) while `auth` is SIGKILLed mid-burst: `accepted=4
refused=0 interrupted=1` — four writes committed and acknowledged, one
request cut in half by the kill, and no request ever refused. After the
restart `auth.db` is `ok`, every queued action is `sent` (`0 still pending`),
all four recovered actions reach the audit log, each exactly once, no
`msg_id` is stored twice, nothing arrives that no producer row claims, and
every recovered action is a `delete user`. The `interrupted=1` is the one
number here that is timing rather than contract: it is whatever the kill
happened to land on.

## 4. Coverage: what was already there, and what none of it covered

Re-measured rather than assumed, because "the tree has tests for this" is the
kind of claim that stops being true quietly:

- `services/auth/tests/unit/change-outbox-test.cc` covers the **repository**:
  the enqueue, the status transitions, the purge. It does not touch the sink.
- `services/sync/tests/unit/change-feed-consumer-test.cc` drives `handle()`
  offline: routing, a malformed payload, the refusal path. No broker, no
  delivery, no ack.
- `services/sync/tests/unit/change-feed-live-test.cc` is the closest thing
  the tree had: a real broker, a real durable, an isolated stream and durable
  name, and the attach/detach drain measured on them — but its payloads are
  synthetic **module** audits written into `audit_log`, so the auth action
  path, its envelope and its `msg_id` are not in it.

What no test asserted before this step: the sink against a live broker (the
`markSent`-after-PubAck rule), the join key between `change_outbox.event_id`
and `user_action_log.msg_id`, and any of it across the death of a process —
which is the whole of what the row asks. Those are what the drill adds, on
the production stream and the production durable, in a sandbox.

## 5. Defects found while building the instrument

All five are in the drill or in the runner, none in the services. They are
recorded because each one is a way a durability claim can be *stated* without
being measured.

1. **A redelivery counter that resets.** The frozen axis first asserted
   `num_redelivered >= 1` after the restart, and failed against a redelivery
   that had demonstrably happened. The counter is per active delivery
   session: it increments while a client holds the delivery and resets to
   zero when the client reconnects, so a check read after the restart can
   never see it. Measured with a live probe (SIGSTOP, deliver, watch, kill,
   restart) and replaced with three checks read *during* the hold — the
   redelivery is observed, it waited for the window, it is still
   unacknowledged — plus the `ack_floor.stream_seq` identity proof after the
   restart.
2. **A burst that counted its own bug as a refusal.** `run_burst` unpacked
   `commit_action(...)` as a 2-tuple while it returns 3, so every successful
   cycle raised `ValueError`, was swallowed as a failure, and the axis
   reported `accepted=0 refused=4/5` while four rows sat in the database. It
   now takes `[0]` and keeps the three outcomes apart
   (`accepted`/`refused`/`interrupted`) so a client-side defect can no longer
   masquerade as a server refusal.
3. **Two checks that could not fail.** The kill trigger was satisfied by
   *any* new outbox row rather than by an accepted logout, and the pid lookup
   was profile-blind: with a sandbox started from another profile the service
   is not this drill's to kill, and "auth was killed" passes by finding no
   process. The first now requires an accepted write; the second is closed by
   the `ensure_up` gate, which fails closed instead of continuing.
4. **A broker probe that read the wrong body.** `broker_up` compared
   `/healthz`'s answer to the string `ok`; the endpoint answers
   `{"status":"ok"}`. `the broker is stopped` would have passed against a
   running broker, and `the broker came back` could never pass. Found by
   running the axis: the same broken probe is what turned a missing golden
   dataset (below) into a 401 with no explanation.
5. **An implicit precondition.** `--token-only` mints a recorder session for
   user `1`; after `rm -rf build/native-stack` the identity database has no
   user `1`, the session verdict cannot resolve the subject, and *every*
   audited write is refused with `401 UNAUTHORIZED` — which reads like a
   durability failure and is a missing fixture. The drill now checks the
   golden owner before it runs and refuses with the command that fixes it.

## 6. What the drill deliberately does not assert

- **A second redelivery, or the tenth.** `max_deliver=10` is not exhausted;
  the axis proves the first redelivery and the acknowledgement that follows
  it, not the dead-letter end of the policy.
- **Retention.** The outbox's purge and the stream's 7-day `max_age` are not
  driven — the drill's own writes settle in seconds, and driving retention
  would mean writing rows dated in the past into a running service's
  database.
- **A concurrent producer fleet.** One producer, one consumer. The outbox's
  ordering guarantee is measured within one producer's queue.
- **The `interrupted` count.** It is reported, never asserted: it is a
  property of where the SIGKILL landed, not of the contract.
- **The identity change feed's retry loop.** `argus-auth-identity` logs
  `stream not found` until the first identity change creates
  `ARGUS_IDENTITY_CHANGE`; that is the publisher-creates-the-stream design
  and it behaves identically with the broker up, so it is not evidence about
  a broker outage.

## Gates

`check-comments.sh`: 1417 files checked, 0 comments (the drill is the one
file this step adds, and it is comment-free like every script before it).
`check-deps.sh`: 135 declarations, 911 edges, 0 forbidden, 0 cycles, 0
unresolved. `check-routes.sh`: 75 declarations over 12 units, 0 added, 0
removed. `build-all.sh dev`: exit 0, every project's ctest green,
`check-tidy` at 553 translation units / 2875 findings over 45 checks against
the 2901 baseline. This step's diff touches no `.cc` or `.hxx` file — the
runner and the drill are shell and Python — so those three numbers are step
3's, unchanged.

## How to run it

```bash
./scripts/native-stack.sh down && rm -rf build/native-stack
flock -o /tmp/argus-build.lock ./scripts/native-stack.sh up
python3 scripts/seed-golden.py --stack-dir build/native-stack
python3 scripts/durability-drill.py all
python3 scripts/durability-drill.py state
```

`all` runs the four axes in order; any one of them can be named on its own.
`state` only reads: the outbox rows and their status, the action journal, the
duplicated message ids, the broker's consumer counters and the integrity of
the three databases.

The drill stops the **shared** broker container for the `broker` axis and
always restarts it (a `finally` around the axis, so a failure restores it
too). Run it against the sandbox, not against a stack someone is using.

## Files

| File | |
|---|---|
| `scripts/durability-drill.py` | new — the four axes, the state view, the golden-owner gate, the broker control |
| `scripts/native-stack.sh` | `sigkill` and `freeze`, and the usage text that names them |
| `.gitignore` | `/build/` (the sandbox), `go2rtc.yaml`, `__pycache__/` (the seeder's import cache) |
| `docs/history/plans/architecture-plan.md` | the row this report closes |
