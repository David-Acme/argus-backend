# 3a step 2 closure, items 2 and 3 — the notification producer's transaction (S1, S1b)

Scope: the notification service's change feed only. The plan's step-2 closure
rows 2 and 3 each name four producers; this unit closes both rows for
`services/notification`, reproducing the shape `services/camera` landed first
(`camera-feature-service.cc`, `change-outbox-repository.cc`,
`camera-change-transaction-test.cc`). No file outside `services/notification`
is touched, and no file of `packages/contracts/sync` is touched: all thirteen
sink input structs already carry `drogon::orm::DbClient* client{nullptr};`.

## What the two rows were, in this producer's terms

**Row 2 (S1) — the enqueue is not in the domain write's transaction.** The
notification read path selected the unread rows, updated them, and only then
wrote the `user_audit_log`-bound change into `change_outbox`, on the pooled
client, as a second unit of work. A crash, a kill, or a storage error between
the `UPDATE` and the outbox `INSERT` left the row marked read with no change
recorded: the sync fan-out could never discover it.

**Row 3 (S1b) — the enqueue's own give-up loses the change.** The sink retried
the outbox write `kEnqueueAttempts = 3` times, 25 ms apart, and then gave up
silently: `ChangeOutboxDisposition::Failed` was returned to `publishAudit`,
which ignored it. The mutation had committed and the handler had answered `ok`.
That is the window the fail-fast storage errors (`SQLITE_FULL`, `SQLITE_IOERR`,
a corrupt page) reach on the first attempt, with no contention required.

Both rows are now closed by one mechanism: the domain write and the change row
are statements of a single `TransactionType::Immediate` transaction, and an
enqueue that cannot be written throws into that transaction, so the whole unit
of work rolls back and the refusal reaches the caller as an error instead of a
silent success.

## Every write path examined, and which ones changed

| Path | Mutates a row | Publishes the change feed | Now shares one transaction |
|---|---|---|---|
| `NotificationService::markAsRead` — `notification-service.cc:128` | yes (`UPDATE notification`) | yes (`UserAuditInput` through `user_change::getNotificationSink()`) | **yes** |
| `NotificationRepository::create` — `notification-repository.cc:162` | yes | no | no — no caller anywhere in the tree (dead) |
| `NotificationRepository::createMany` — `notification-repository.cc:182` | yes | no | no — no caller anywhere in the tree (dead) |
| `NotificationRepository::createManyWithCommand` — `notification-repository.cc:206` | yes | no | no — its own transaction already covers its insert; see below |
| `NotificationRepository::markDelivered` — `notification-repository.cc:344` | yes | no (a delivery settle, not a change) | no |
| `NotificationService::deliverDurable` / `deliverPending` — publishes `notification.delivery` and the push intent | no | no (delivery funnel, not the feed) | no — it is already "publish, then settle" and is out of this unit |
| `NotificationTokenService::registerToken` | yes | no | no |
| `NotificationService::runSelfTest` (probe insert / state / purge) | yes | no | no |

`markAsRead` is the only write path in the service that mutates a row and then
publishes a change; it is correspondingly the only one that gained a
transaction. The sweep that establishes this: `user_change::getNotificationSink`
has exactly one reader in the whole service (`notification-service.cc:135`,
installed from `main.cc:118`), and the notification `Add` sync leg does not
travel through the change feed at all — it is pulled
(`PullNotifications`, `FIND_SYNC_AFTER` in `notification-repository.cc:509`),
so a created notification owes no outbox row.

### `NotificationService::markAsRead` — `notification-service.cc:127-160`

```cpp
auto transaction = co_await db_transaction::begin(DbService::client());
try {
  const auto changes = co_await repository_.markAsRead(
      {.userId = userId, .ids = ids, .client = transaction.get()});
  ... for each change: co_await sink->publishAudit(UserAuditInput{..., .client = transaction.get()});
  if (!co_await db_transaction::Commit(std::move(transaction)))
    throw std::runtime_error("notification read commit failed");
}
catch (...) {
  db_transaction::rollback(transaction);
  throw;
}
```

The single-owner rule is honoured: the `std::shared_ptr<drogon::orm::Transaction>`
lives in the coroutine that opened it and nowhere else. The client handed to
the repository and to the sink is `transaction.get()` — a borrowed raw pointer,
never a copy of the shared ownership, which is what the sqlite package's
"borrowed, never owned" rule requires and what the field type enforces
(`.client = transaction` does not compile).

### `NotificationRepository::markAsRead` — `notification-repository.cc:584-631`

The placeholder building and the before/after capture are unchanged. The two
statements now run on the borrowed client
(`notification-repository.cc:609-610`), then the same client for the `UPDATE`
(`:629`):

```cpp
const auto pooled = DbService::client();
auto* client = input.client ? input.client : pooled.get();
```

The select-then-update pair is what the sync rule asks for ("select the unread
rows before mutation, update only those rows"); inside one `Immediate`
transaction it is also atomic with respect to a second connection, which it was
not before.

Its three parameters became one struct, `NotificationMarkReadInput`
(`notification-query.hxx:214-219`), declared in the query file per rule 3 and
built inline with designated initializers at the call site per rule 2:

```cpp
struct NotificationMarkReadInput
{
  int64_t userId{0};
  std::vector<int64_t> ids;
  drogon::orm::DbClient* client{nullptr};
};
```

`markAsRead` keeps its place in the header (`notification-repository.hxx:55`)
and its two named parameters on the service, which does not take 3+. The
feature service (`notification-feature-service.cc:20-23`) and the controller
(`notification-controller.cc:11-17`) are untouched: the transaction never
crosses the service boundary. An empty `ids` cannot reach the transaction from
HTTP — `NotificationReadDto` validates `ARRAY_NOT_EMPTY` plus
`MIN_ELEMENTS(ids, int64_t, 1)` — so the transaction always has work to commit.

### The sink — `nats-notification-change-sink.cc:49-95`

`publishAudit` ends with the payload's client forwarded into the enqueue
(`:78-79`); `enqueue` (`:82-95`) lost the retry loop entirely and now lets a
failed write propagate:

```cpp
co_await enqueue({.eventId = id, .payload = payload, .client = input.client});
...
drogon::Task<void> NatsNotificationChangeSink::enqueue(ChangeOutboxEnqueueInput input) const
{
  if (input.payload.size() > kMaxPayloadBytes) { LOG_ERROR << ...; co_return; }
  input.fingerprint = change_outbox_key::fingerprintJson(input.payload);
  input.at = nowMs();
  co_await outbox_.enqueue(input);
  wake_.notify_all();
}
```

`kEnqueueAttempts` / `kEnqueueRetryMs` and the `<trantor/net/EventLoop.h>`
include that served only their `drogon::sleepCoro` are gone. The wake on the
drain's condition variable is unchanged, and it still fires only after the row
exists.

### The repository — `change-outbox-repository.cc:11-37`

```cpp
const auto pooled = DbService::client();
auto* client = input.client ? input.client : pooled.get();
const auto inserted = co_await client->execSqlCoro(INSERT_EVENT, ...);
if (inserted.affectedRows() > 0)
  co_return ChangeOutboxDisposition::Enqueued;
```

A refused insert now throws instead of returning a disposition the caller may
ignore: `std::invalid_argument` for an input with no event id or no payload
(kept from before, and now the shape camera's own test asserts), and
`std::runtime_error` for the one state that means corruption — an insert that
affected no row and left no row behind. `Replay` and `Conflict` keep their
meaning and their logging.

## The enqueue contract, before and after

| | before | after |
|---|---|---|
| Input | `ChangeOutboxEnqueueInput{eventId, fingerprint, payload, at}` | the same struct plus `drogon::orm::DbClient* client{nullptr}` |
| Client | always the pooled `DbService::client()` | borrowed when provided, pooled otherwise — camera's exact resolution |
| Disposition | `Enqueued`, `Replay`, `Conflict`, **`Failed`** | `Enqueued`, `Replay`, `Conflict` (`Failed` removed) |
| Retry | 3 attempts, 25 ms apart, then `Failed` | none; the storage error propagates on the first attempt |
| Caller on failure | ignored the disposition, answered `ok` | the throw rolls the caller's transaction back and answers an error |
| Malformed input | `invalid_argument` | unchanged |

`Failed` was removed rather than left unreachable, because an enum value that
can only mean "the change this call recorded will never exist" is exactly the
give-up contract S1b replaces; keeping it would leave the next caller free to
swallow it again.

The unit's documentation was kept true in the same change:
`services/notification/CONTEXT.md`'s change-feed bullet stated that the row
updates commit first and that a refused enqueue "is retried before it is given
up on", and now states the one `IMMEDIATE` transaction and the throw.
`services/notification/AGENTS.md` names neither contract and is unchanged.

## The oversized-payload drop, kept exactly as it was

`enqueue` still returns without recording when `payload.size() >
kMaxPayloadBytes` (256 KiB, `nats-notification-change-sink.hxx:40`), after a
`LOG_ERROR`. That is deliberate and unchanged by this unit: the broker's
message budget is a hard bound, the alternative is a retry loop against a
payload that can never fit, and the shape is the same in camera. It is also
tested (`notification-change-outbox-sink-test.cc`, the `oversized` case) and
that test is green.

## Deliberately not changed

- **`NotificationRepository::create` and `createMany`.** Verified dead: no
  caller anywhere in `services/notification`. They publish no change, so S1
  does not reach them, and deleting them is not this unit's decision.
- **`createManyWithCommand`.** It already wraps its insert in its own
  `TransactionCommitAwaiter`, so its domain write is atomic; what follows it is
  a NATS delivery publish (a different event, on a different subject, with its
  own durable funnel), not the change feed.
- **`deliverDurable`.** It publishes the delivery event and the push intent
  and only then marks the delivery settled, so a crash loses a settle and the
  row is re-published on the next pass — at-least-once by design, and no change
  row is involved.
- **`NotificationTokenService::registerToken`.** Its upsert publishes nothing.
- **The contracts.** `packages/contracts/sync`'s thirteen sink input structs
  and their `DbClient* client{nullptr}` defaults are untouched.
- **`packages/lib/sqlite`.** No change: the borrowed-client rule and the
  `db_transaction` helpers are used as they stand.

## The test

`services/notification/tests/unit/notification-change-transaction-test.cc`,
registered in `services/notification/CMakeLists.txt:316-335` the way its
siblings are (explicit source list, `ARGUS_NOTIFICATION_SCHEMA` compile
definition, `notification-core` + `argus::lib::{nats,sqlite}` + doctest,
`-Wall -Wextra`, `add_test`). One test case, 26 assertions, covering the four
things both rows ask for:

1. **A refusing sink commits nothing.** `RefusingSink` throws; the call raises
   `std::runtime_error`; all three seeded rows are still unread and both
   targeted `read_at` values are still 0. It also records the client it was
   handed, so the test asserts the sink was given the transaction's client and
   not the pooled one (`sink.transactional()`).
2. **A succeeding read commits both halves.** Two audit calls, the two rows
   readable as read, `read_at > 0`, exactly one unread left.
3. **A replay enqueues no second change.** The same ids again: no further sink
   calls, still one unread.
4. **The durable sink writes the outbox row inside the transaction.**
   `NatsNotificationChangeSink` with no bus (so nothing is drained) leaves
   exactly one pending row, prefixed `notification-change:`, carrying the
   third id's payload with `attempts == 0`. Then `DROP TABLE change_outbox` and
   a fourth read: the call raises and commits nothing (unread count 1, its
   `read_at` 0) — S1b's failure path exercised directly, which the old retry
   loop could not be under `drogon::sync_wait` (that was the historical report's
   B5, closed by removal rather than by a new seam).

One pre-existing assertion had to change, in
`tests/unit/notification-change-outbox-test.cc:127-132`: the incomplete-input
case asserted `ChangeOutboxDisposition::Failed`. That value no longer exists
and the repository now throws for that input, so the test asserts
`CHECK_THROWS_AS(..., std::invalid_argument)`, byte for byte the shape
`services/camera/tests/unit/change-outbox-test.cc:132-133` uses. The assertion
encoded the give-up contract, so this is the cause fix, not a weakened test.

## Evidence

`flock /tmp/argus-build.lock ./scripts/build-all.sh dev --only notification` —
the project-level gate, tests included, **0 errors and no compiler warning**
(the only warnings in the whole log are ncnn's and glslang's configure-time
`CMAKE_CXX_STANDARD` notices, third-party and pre-existing):

```
100% tests passed, 0 tests failed out of 42

Total Test time (real) =  10.27 sec
[setup] All selected projects built and tested (profile: dev).
```

(Two consecutive runs of that command are green; the numbers quoted are the
second, run against the final state of the tree.)

The 42 are the notification project's own 14 plus the tests of the subprojects
its build tree pulls in (identity, the contracts, the libs, the clients). The
fourteen notification-owned tests, run on their own:

```
100% tests passed, 0 tests failed out of 14
```

schema, migration, controller, rpc, no-nats, delivery, change-outbox,
change-outbox-sink, change-transaction, ack, delivery-live, push-intent,
contract-vocabulary, client.

The new binary standalone:

```
[doctest] test cases:  1 |  1 passed | 0 failed | 0 skipped
[doctest] assertions: 26 | 26 passed | 0 failed |
[doctest] Status: SUCCESS!
```

The two static gates of the same run also pass on their own:
`check-comments: 1241 files checked, 0 comments` and `check-deps: 64
declarations, 483 edges, 0 forbidden, 0 cycles, 0 unresolved`.

One coupling worth recording: the notification project's build tree adds
`packages/identity` as a subproject (the service links `argus::identity` for
auth), so an in-flight identity edit blocks this project's build and its
`ctest` too. The first two runs of the command above failed in
`packages/identity/src/feature/api/user/services/user-feature-service.cc:46`
(`db_transaction has not been declared`), a file this unit does not touch; the
next two ran green with no change of mine in between.

## Where the tree disagrees with the brief

- **`Drogon has no commit()`.** Confirmed by reading `transaction.hxx`/`.cc`:
  `db_transaction::Commit` is an awaiter over the transaction's
  `setCommitCallback`, and the destructor is what queues a rollback. The
  transaction therefore has exactly one owner in
  `NotificationService::markAsRead`, and `catch (...) { rollback; throw; }` is
  the only other place it is named.
- **`Failed` is removed from `ChangeOutboxDisposition`**, and with it one
  assertion in the pre-existing outbox test (see above). No other producer's
  copy of that enum is affected: each service owns its own disposition type.
- **A "Transaction roll back error - TransactionImpl.cc:175" line appears on
  stderr while the new test runs.** It is Drogon's own log for the rollback of
  a transaction whose connection the test then drops, and the landed camera
  reference test emits the same line; the assertions around it prove the
  rollbacks did their work (3 unread after the refusal, 1 after the dropped
  table). Not a defect of this unit and not suppressed.
- **clang-format is not a gate here.** `clang-format --dry-run --Werror`
  (v22.1.8) flags most files in the tree including untouched ones; two lines I
  introduced that it would have moved at the 80-column boundary were fixed by
  hand, and no tree-wide reformat was run.
- **B5 of `f3-2c-notification-change-outbox.md` is closed by this unit**, not
  by a test seam: the retry path it could not exercise no longer exists.
