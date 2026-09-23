# 3a step 2 closure, items 2 and 3 — the productivity producer's transaction (S1, S1b)

Scope: `services/productivity`'s change feed and nothing else. The plan's
step-2 closure rows 2 and 3 each name four producers; this unit closes both
rows for the productivity producer, reproducing the shape `services/camera`
landed first (`camera-feature-service.cc`, `change-outbox-repository.cc`,
`camera-change-transaction-test.cc`). One file outside `services/productivity`
is touched, `packages/contracts/productivity/`: the new refusal, its pinned
catalog entry and the contract's own AGENTS.md count. No file of
`packages/contracts/sync` is touched — all thirteen sink input structs across
the tree already carry `drogon::orm::DbClient* client{nullptr};`, and the
working tree already had them.

## What the two rows were, in this producer's terms

**Row 2 (S1) — the enqueue is not in the domain write's transaction.** Every
one of the fifteen write paths mutated its row first and called the
`user_change` sink afterwards, on the pooled client, as a second unit of work.
A crash, a kill or a storage error between the `INSERT`/`UPDATE` and the
outbox `INSERT` left the row mutated with no change recorded: no replica, no
`argus-sync` fan-out and no reconnecting client could ever discover it, and the
HTTP caller had already been answered `200`.

**Row 3 (S1b) — the enqueue's own give-up loses the change.** The sink retried
the outbox write `kEnqueueAttempts = 3` times, 25 ms apart, and then logged
`could not be recorded ... the change is lost` and returned. The mutation had
committed and the handler had answered `ok`. That is the window a fail-fast
storage error (`SQLITE_FULL`, `SQLITE_IOERR`, a corrupt page) reaches on the
first attempt, with no contention required — and it is exactly the window S1
opens, retried instead of closed.

Both rows are closed by one mechanism: the domain write, the reads it needs,
the change-feed emit and the audit diff are statements of a single
`TransactionType::Immediate` transaction, and an enqueue that cannot be written
throws into that transaction, so the whole unit of work rolls back and the
caller gets `ProductivityErrors::ChangeNotRecorded` (500) instead of a silent
success.

## Every write path examined, and which ones changed

The service registers fifteen mutation routes (`ADD_METHOD_TO` sites in the
five `*-controller.hxx` files) — five resources, each with create, update and
remove. There are no other writers: no mutation SQL lives outside
`src/shared/repositories/`, and no publisher outside the five feature
services (verified by the two greps quoted under *Evidence*).

| Path | Mutates a row | Publishes the change feed | Now shares one transaction |
|---|---|---|---|
| `ProjectFeatureService::create` — `project-feature-service.cc:51` (open `:55`, commit `:71`, emit `:68`) | yes (`INSERT project`) | yes (`emitUsers`) | **yes** |
| `ProjectFeatureService::update` — `:82` (open `:85`, commit `:128`, audit `:121`) | yes (`UPDATE project`) | yes (`publishAudit`) | **yes** |
| `ProjectFeatureService::remove` — `:138` (open `:142`, commit `:160`, emit `:157`) | yes (soft delete) | yes (`emitUsers`, tombstone) | **yes** |
| `ProjectTaskFeatureService::create` — `project-task-feature-service.cc:64` (open `:68`, commit `:92`, emit `:89`) | yes | yes | **yes** |
| `ProjectTaskFeatureService::update` — `:103` (open `:106`, commit `:155`, audit `:145`) | yes | yes | **yes** |
| `ProjectTaskFeatureService::remove` — `:165` (open `:169`, commit `:193`, emit `:190`) | yes | yes | **yes** |
| `CalendarEventFeatureService::create` — `calendar-event-feature-service.cc:52` (open `:56`, commit `:77`, emit `:74`) | yes | yes | **yes** |
| `CalendarEventFeatureService::update` — `:88` (open `:91`, commit `:137`, audit `:130`) | yes | yes | **yes** |
| `CalendarEventFeatureService::remove` — `:148` (open `:151`, commit `:169`, emit `:166`) | yes | yes | **yes** |
| `ProjectMemberFeatureService::create` — `project-member-feature-service.cc:76` (open `:94`, commit `:133`, `findExisting`+`updateAccess` audit `:112` *or* `create` `:120` + `emitMembership` `:124` + `emitParent` `:128`) | yes | yes | **yes** |
| `ProjectMemberFeatureService::update` — `:144` (open `:147`, commit `:186`, audit `:179`) | yes | yes | **yes** |
| `ProjectMemberFeatureService::remove` — `:196` (open `:200`, commit `:230`, `emitMembership` `:222` + `emitParent` `:226`) | yes | yes | **yes** |
| `CalendarEventShareFeatureService::create` — `calendar-event-share-feature-service.cc:76` (open `:94`, commit `:133`, audit `:112` *or* `create` `:120` + emits `:124`/`:128`) | yes | yes | **yes** |
| `CalendarEventShareFeatureService::update` — `:144` (open `:147`, commit `:186`, audit `:179`) | yes | yes | **yes** |
| `CalendarEventShareFeatureService::remove` — `:196` (open `:200`, commit `:230`, emits `:222`/`:226`) | yes | yes | **yes** |

Read paths (list/get on all five resources), the sync RPC pull source, the
reminder repositories and the change-outbox drain worker mutate nothing and
publish nothing; they are unchanged.

### How completeness was established

Three checks, in this order:

1. **Route enumeration.** `grep -rn ADD_METHOD_TO services/productivity/src/`
   returns exactly fifteen routes, five resources × three verbs; each maps onto
   one service method, and those fifteen are the rows of the table above.
2. **Publisher enumeration.** `grep -rn "emitUsers(\|publishAudit("
   services/productivity/src/` returns only the sink's own definitions and the
   call sites inside the five feature services — eleven call sites, all inside
   a transaction window. The `emit`/`emitMembership`/`emitParent` helpers and
   `canEdit`/`canWorkOn` are private and every call site of each was listed and
   checked to sit inside a `try` body whose transaction was opened on the same
   frame (call-site lines quoted under *Evidence*).
3. **Mutation enumeration.** `grep -rnE "INSERT INTO|UPDATE |DELETE FROM"
   services/productivity/src/` with `/repositories/` excluded returns nothing:
   no service or controller writes SQL of its own.

What the greps cannot see is a *forgotten* client argument, because a
repository that is not given the transaction client silently takes the pooled
one and, with a one-connection pool, waits forever instead of failing. That is
covered by running both suites: a missed client hangs, and no test hangs (see
*The tests*).

## The repositories and the borrowed client

Every repository that a write path touches gained a borrowed client and
resolves it the camera way, never owning it:

```cpp
const auto pooled = DbService::productivityClient();
auto* client = input.client ? input.client : pooled.get();
```

- `project-repository.cc` — `findById` `:13`, `create` `:36`, `update` `:59`,
  `remove` `:112`; `update`'s two internal re-fetches pass the same client
  (`:91`, `:104`). `project-query.hxx` — `ProjectCreateInput:127` (client
  `:136`), `ProjectUpdateInput:139` (client `:147`).
- `project-task-repository.cc` — `findById` `:13`, `create` `:37`, `update`
  `:63`, `remove` `:118`; re-fetches `:97`/`:110`. `project-task-query.hxx` —
  `ProjectTaskCreateInput:139`, `ProjectTaskUpdateInput:152`.
- `calendar-event-repository.cc` — `findById` `:12`, `create` `:38`, `update`
  `:72`, `remove` `:130`; re-fetches `:109`/`:122`.
  `calendar-event-query.hxx` — `CalendarEventCreateInput:132`,
  `CalendarEventUpdateInput:148`.
- `project-member-repository.cc` — `findById` `:9`, `findAccess` `:31`,
  `memberIds` `:43`, `findExisting` `:58`, `create` `:70` (re-fetch `:77`),
  `updateAccess` `:86` (re-fetch `:92`), `remove` `:98`.
  `project-member-query.hxx` — `ProjectMemberCreateInput:104`,
  `ProjectMemberUpdateInput:112`, `ProjectMemberLookupInput:119`.
- `calendar-event-share-repository.cc` — the exact mirror: `findById` `:9`,
  `findAccess` `:32`, `memberIds` `:44`, `findExisting` `:59`, `create` `:71`
  (re-fetch `:78`), `updateAccess` `:87` (re-fetch `:93`), `remove` `:99`.
  `calendar-event-share-query.hxx` — `CalendarEventShareCreateInput:104`,
  `CalendarEventShareUpdateInput:112`, `CalendarEventShareLookupInput:119`.

The `= nullptr` defaults live in the `.hxx` declarations; every line above is
where the definition begins in the `.cc`.

Three helpers that would otherwise have crossed rule 2's three-parameter line
became parameter structs with designated initializers, with every member
listed in declared order: `ProjectMemberLookupInput {parentId, userId,
client}`, `ProjectMemberUpdateInput {id, access, client}`, and the same two in
the calendar-event-share query header. The private per-service helpers that
gained the client followed the same rule: `EmitInput {operation, row, client}`
and `CanEditInput {row, actorId, client}` on project and calendar-event,
`EmitInput`/`CanWorkOnInput {projectId, actorId, client}` on project-task, and
`EmitMembershipInput`/`EmitParentInput` each gaining `client` on the two
membership services.

## The client spelling inside the service

The whole service now spells its database one way: `DbService::productivityClient()`.
Before this change the repositories and the sink already used it and three boot
calls in `main.cc` did not (`runScriptFile(path)`, `applyPragmas()`,
`client()->execSqlSync(...)`, which all resolved to the app default); those
three now name the productivity client explicitly:

```cpp
DbService::runScriptFile(productivityDb.schemaPath,
                         DbService::productivityClient());
DbService::applyPragmas(DbService::productivityClient());
DbService::productivityClient()->execSqlSync("PRAGMA foreign_keys = OFF");
```

The one other spelling left in the project — `DbService::client()` in
`tests/unit/productivity-sync-rpc-test.cc:137` — was unified too; that test
never installs a named client, so the two resolve to the same object there.

**Why `productivityClient()` and not `client()`.** `productivityClient()`
returns the named `"productivity"` client when one is registered and falls back
to `client()` otherwise, so in production (where nothing registers it) the two
spellings are the same object, while in the controller test they are not:
`productivity-controller-test.cc:355` registers **identity.db** as the app
default and `:361-363` installs **productivity.db** as the named client. Had
the service been unified onto `client()`, that test would have read and written
the identity database. It also keeps the schema application, the pragmas, every
repository read and every transaction on the same connection, which matters
because the pool holds one connection and `freezeClient(dbPath)` arms the
post-quit client under `client()` — `productivityClient()`'s fallback picks
that frozen client up for the drain worker, which is the point of the
`shutdown_signal::onQuit`/`onStop` registrations in `main.cc:143-147`.

## The enqueue contract, before and after

**The sink's private helper.** `NatsProductivityChangeSink::enqueue` took
`(std::string eventId, std::string payloadJson)` and returned after its own
retry loop; it now takes the repository's own input struct
`(ChangeOutboxEnqueueInput input)` carrying `.client`, and the loop is gone —
`nats-productivity-change-sink.cc:104-116` is nine statements: the oversize
check, two derivations, the one `co_await outbox_.enqueue(input)`, and
`wake_.notify_all()`. `kEnqueueAttempts` and `kEnqueueRetryMs` are deleted, and
with them `<trantor/net/EventLoop.h>` and the `drogon::sleepCoro`. Both
`emitUsers` and `publishAudit` pass `.client = input.client` down to their
`co_await enqueue` (`:66` and `:99`). A throw from `enqueue` now propagates out
of the sink into the feature service's `try`, where `catch (...)` rolls the
transaction back and rethrows.

**The repository.** `ChangeOutboxEnqueueInput` (`change-outbox-query.hxx:38-45`)
gained `drogon::orm::DbClient* client{nullptr};` and `ChangeOutboxDisposition`
lost its `Failed` member — the only producer of `Failed` was the give-up path
that no longer exists. `enqueue` (`change-outbox-repository.cc:11-37`) resolves
borrowed-vs-pooled at `:18-19`, keeps `INSERT OR IGNORE` and the fingerprint
comparison, and now throws instead of returning a status for the two cases that
are programming errors rather than outcomes: an empty event id or payload
(`std::invalid_argument`) and a conflicting insert whose existing row cannot be
read back (`std::runtime_error`).

**The disposition set.** `Enqueued` (the insert landed), `Replay` (the same
transition arrived twice — the event id names the transition, so the payload is
identical), `Conflict` (a different transition already holds that id: logged,
not dispatched, and the committed row stays).

**The commit refusal.** `db_transaction::Commit` is awaited once per write
path; when it reports `false` (Drogon rolled the transaction back instead of
committing it) the path throws
`ResponseException(ProductivityErrors::ChangeNotRecorded)` — the one new
contract entry, `productivity-errors.hxx` after `OwnerAlreadyHasAccess`:
`.code = ErrorCode::InternalError, .status = 500, .message = "The change could
not be recorded"`. That is the case the write and its change could not both
land, and it is answered as a refusal so `ErrorHandler::handleException` turns
it into the envelope and the caller knows the write did not happen.

## Transaction ownership, and the reads inside the window

Drogon's `Transaction` has no `commit()`: the commit is
`~TransactionImpl`, reached when the last owning `shared_ptr` goes away, which
is what `db_transaction::Commit`'s `await_suspend` arranges by moving the
caller's pointer into a local and resetting its own. The transaction therefore
lives in a local of the coroutine that opened it and is moved into `Commit`
exactly once; it is never stored in a member, a struct field, a lambda capture
or passed to the sink. The sink receives only the borrowed raw client, never
the owner.

Because productivity's pool holds one connection and Drogon hands a second
waiter an infinite timeout, every statement inside the window must name it —
`findById`, `findAccess`, `memberIds`, `updateAccess` and the repositories'
internal re-fetches included, which is why they take the client and pass it to
their own re-reads.

Where the window opens was chosen per path:

- **Update and remove paths open first** and do their authorization reads
  inside, with the transaction client — they need the `before` snapshot (the
  audit diff) or the tombstone, and reading it inside is what makes the diff
  and the write describe the same instant.
- **The two membership `create` paths authorize outside**: `create` reads the
  parent row, calls `IdentityUserDirectory::findById` over gRPC and checks the
  role *before* opening the transaction, then opens it around
  `findExisting`/`updateAccess`-or-`create` and the emits. Deliberately: the
  identity lookup is network I/O, and holding the service's only database
  connection across an RPC would stall every other request for the duration of
  a remote call.

## The oversized-payload drop, kept exactly as it was

A payload past `kMaxPayloadBytes` (256 KiB) is still logged
(`nats-productivity-change-sink.cc:106-111`) and dropped, with no outbox row
and no throw. That is a deliberate asymmetry with S1b, not an oversight: a
payload the broker's message budget cannot carry is not a *storage* failure,
and no number of retries would ever make it publishable, so failing the write
would roll back a mutation that is perfectly valid and could only be answered
with a permanently unsyncable row. The alternative — committing the row and
dropping the change — is what the drop already does, and it is at least
visible in the log and bounded to one message. The sink test still asserts it:
`productivity-change-outbox-sink-test.cc:211-215` builds a row whose
description is `kMaxPayloadBytes + 1` bytes, waits the emit, and checks the
outbox has nothing pending.

## Deliberately not changed

- **The contracts.** `packages/contracts/sync/src/sync/user-change-sink.hxx`
  already declares `UserEmitInput::client`, `UserAuditInput::client` and (via
  `module-audit-event.hxx`) `ModuleAuditInput::client` with the `{nullptr}`
  default; the working tree carried them before this unit started and they are
  untouched. The same is true of the sink vocabulary this service now
  implements — `emitUsers(const UserEmitInput&)` with no separate `emitUser`
  — which the earlier part of this same step landed across the producers.
- **The controllers, DTOs and schemas.** Nothing in them names a database
  client or a sink.
- **The read paths and the sync RPC pull source.** `PullTable` reads, the
  reminder/reminder_detail legs and the list/get endpoints mutate nothing and
  publish nothing.
- **The drain loop.** `retryMs`, `kDrainBatch = 64`, `kProgressMs = 50` and the
  stop-at-the-oldest-refused-row rule are the *publisher*'s back-off against a
  broker that is down; they are the mechanism that makes the durable outbox
  useful and are untouched. Only the *enqueue*'s private retry disappeared.
- **`markSent`/`recordAttempt`/`pendingBatch`.** Still on the pooled client —
  they run on the drain worker, outside any transaction, and taking the
  transaction client there would be wrong for the same one-connection reason in
  reverse.
- **The `main.cc` boot order.** `reconcile()` still starts from
  `registerBeginningAdvice` after the schema is applied, and the
  `shutdown_signal` registrations are as the previous sub-steps left them.

## The tests

**New:** `services/productivity/tests/unit/productivity-change-transaction-test.cc`
(one test case, **64 assertions**), registered in
`services/productivity/CMakeLists.txt` with an explicit source list (the test
file plus `${PRODUCTIVITY_FEATURE_SOURCES}`, never a glob), the link list that
mirrors `productivity-controller-test` (without `argus_identity`, which nothing
on this path needs), `-Wall -Wextra`, the
`ARGUS_PRODUCTIVITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql"`
compile definition and `add_test`. It mirrors camera's
`camera-change-transaction-test.cc`:

- a `RefusingSink : public UserChangeSink` that throws on demand, records every
  call and records whether the client it was handed is the pooled one or a
  different (transaction) one;
- refuse-then-succeed on project create/update/remove, project-task
  create/remove, project-member update/remove, calendar-event create/update
  and calendar-event-share remove — after each refusal the row state is read
  back with SQL and asserted unchanged;
- `sink.transactional()` asserted after every successful publish: the client
  the sink sees is **not** the pooled client, which is what proves the emit
  travelled inside the transaction rather than after it;
- a durable `NatsProductivityChangeSink` (bus `nullptr`, so it never publishes)
  to prove the row and the change land together: one `pendingBatch(10)` row
  whose payload contains the new project id, then `DROP TABLE change_outbox`,
  then create/update/remove again — all three throw, the row states are
  unchanged, and the earlier row survives;
- `PRAGMA foreign_keys = OFF` after `runScriptFile`, exactly as production does
  (`main.cc:136`), because the schema's foreign keys point at identity's `user`
  table which this test does not create.

**Updated:** `productivity-change-outbox-test.cc` (the `Failed` disposition
check became `CHECK_THROWS_AS(..., std::invalid_argument)`),
`productivity-change-outbox-sink-test.cc` (every emit call site moved to the
`UserEmitInput` shape), `productivity-controller-test.cc` (`RecordingSink` now
implements `emitUsers` only), and
`packages/contracts/productivity/tests/unit/productivity-contract-catalog-test.cc`
(`ChangeNotRecorded` added to the pinned table and the count raised 8 → 9 —
without this the new refusal would have sat outside the catalog's
wire-legality and "no two say the same thing" checks).

No test was changed to make a failure go away; no pre-existing assertion was
weakened. The three updated tests changed only the shape of the sink they call
and the enumeration of the disposition set.

### Which test covers which write path

Both suites were read line by line for this, because a missing client argument
hangs rather than fails:

| Path | `productivity-change-transaction-test` | `productivity-controller-test` |
|---|---|---|
| project create / update / remove | yes (all three) | yes (`:378`, `:416`, `:674`) |
| project-task create / update / remove | create, remove | create, update (`:522`, `:554`) |
| project-member create / update / remove | update, remove | all three (`:462`, `:485`, `:494`) |
| calendar-event create / update / remove | create, update | create, remove (`:587`, `:664`) |
| calendar-event-share create / update / remove | remove | all three (`:629`, `:646`, `:655`) |

All fifteen are exercised at least once by one of the two, and ten of them also
prove the atomicity directly — every path the transaction test walks has a
refused-then-succeeded pair and a row-state assertion between them. The five it
does not reach are the two membership `create`s (they need the identity RPC
harness the controller test has), the `update`s of project-task and
calendar-event-share, and calendar-event `remove`; all five are walked
end-to-end by the controller test, and the transaction-test rows above show
that the other ten are the ones where an emit can be made to fail on demand.

## Evidence

The canonical run, quoted verbatim:

```
[setup] === comments ===
check-comments: 1241 files checked, 0 comments
[setup] === dependencies (section 2.4) ===
check-deps: 64 declarations, 493 edges, 0 forbidden, 0 cycles, 0 unresolved, 49 edges deferred to phase 3 (227 third-party mentions over 22 roots)
```

```
 1/38 Test  #1: productivity-schema-test ................   Passed    0.02 sec
 2/38 Test  #2: productivity-migration-test .............   Passed    0.06 sec
 3/38 Test  #3: productivity-change-outbox-test .........   Passed    0.03 sec
 4/38 Test  #4: productivity-change-outbox-sink-test ....   Passed    0.20 sec
 5/38 Test  #5: productivity-controller-test ............   Passed    0.13 sec
 6/38 Test  #6: productivity-change-transaction-test ....   Passed    0.05 sec
 7/38 Test  #7: productivity-sync-rpc-test ..............   Passed    0.05 sec
11/38 Test #11: productivity-contract-vocabulary-test ...   Passed    0.00 sec
12/38 Test #12: productivity-contract-catalog-test ......   Passed    0.00 sec
38/38 Test #38: productivity-sync-client-test ...........   Passed    0.02 sec

100% tests passed, 0 tests failed out of 38

Total Test time (real) =   7.05 sec
[setup] All selected projects built and tested (profile: dev).
```

Per-suite doctest counts, each run directly:

| Suite | Test cases | Assertions |
|---|---|---|
| `productivity-change-transaction-test` (new) | 1 | 64 |
| `productivity-change-outbox-test` | 2 | 41 |
| `productivity-change-outbox-sink-test` | 1 | 50 |
| `productivity-controller-test` | 1 | 163 |
| `productivity-sync-rpc-test` | 1 | 30 |
| `productivity-schema-test` | 3 | 18 |
| `productivity-migration-test` | 9 | 202 |
| `productivity-contract-vocabulary-test` | 3 | 16 |
| `productivity-contract-catalog-test` | 3 | 136 |
| `productivity-sync-client-test` | 2 | 24 |

**0 errors, 0 warnings.** Ninja prints warnings inline, so as a second and
stronger check every first-party translation unit of the project was forced
through the compiler once: `touch` over all
`services/productivity/{src,tests}/**/*.cc` followed by a direct
`ninja -C services/productivity/build/dev argus-productivity
productivity-change-outbox-test productivity-change-outbox-sink-test
productivity-controller-test productivity-change-transaction-test
productivity-sync-rpc-test productivity-schema-test
productivity-migration-test` rebuilt **97 translation units** with
`grep -c ": warning:"` = 0 and `grep -c ": error:"` = 0.

The greps the completeness argument rests on:

```
$ grep -rn "ADD_METHOD_TO" services/productivity/src/          # 15 routes
$ grep -rn "emitUsers(\|publishAudit(" services/productivity/src/
   # only the sink's definitions and 11 call sites, all inside a transaction
$ grep -rnE "INSERT INTO|UPDATE |DELETE FROM" services/productivity/src/ \
      --include=*.cc --include=*.hxx | grep -v /repositories/   # empty
$ grep -rn "co_await emit(\|co_await emitMembership(\|co_await emitParent(\|co_await canEdit(\|co_await canWorkOn(" \
      services/productivity/src/   # every call site inside a try whose transaction is open
$ grep -rn "DbService::client()" services/productivity/src/      # empty
```

One note on the logs: the new test's `DROP TABLE change_outbox` section makes
Drogon log `Transaction roll back error` (TransactionImpl.cc:175). It is
Drogon's own bookkeeping, not a lost rollback — when a statement inside the
transaction fails, Drogon issues a `rollback` itself *and* the service's
`catch (...)` asks for one, and the second one has no transaction left to roll
back. The assertions are what matter and they pass: after the table is dropped,
the project row is absent (`liveProjects("Orphan Project") == 0`), the rename
did not happen (`projectName(durable.id) == "Durable Project"`) and the delete
did not happen (`liveProject(durable.id) == 1`).

## Where the tree disagrees with the brief

1. **`setProductivityClient` and the two spellings.** The brief stated that the
   controller test is its only caller and that the two spellings resolve to the
   same object there. Both halves are wrong in a way that changes the decision:
   `productivity-controller-test.cc:355-356` registers **identity.db** as the app's
   default client, and `:361-363` then installs **productivity.db** as the
   named client — so in that test `DbService::client()` is the identity
   database and `DbService::productivityClient()` is this service's own.
   Unifying onto `client()` would have pointed every repository at identity's
   tables. The unification therefore went to `productivityClient()`, which is
   also the spelling the repositories, the sink and the outbox already used;
   the divergence was three calls in `main.cc` plus one seeding line in
   `productivity-sync-rpc-test.cc`.
2. **Reminders are not a write path here.** The brief lists reminders among the
   paths to bring inside a transaction. Productivity has no reminder write path
   at all: `ReminderRepository` and `ReminderDetailRepository` are held only by
   `productivity-sync-rpc-service.hxx:23-24`, for the pull legs, and
   `services/productivity/CONTEXT.md:90` records the ruling ("no
   reminder/reminder_detail write path anywhere, sync-read-only"). Their
   repositories and schemas compile into `productivity-core` and nothing calls
   a mutation on them. Nothing was changed there because there is nothing to
   change.
3. **The oversized drop survives S1b by design.** The brief's row 3 says a
   failed enqueue must fail the write. The drop is not a failed enqueue: it is
   a decision not to enqueue at all, taken before any SQL runs. Keeping it is a
   judgement call made explicit here so a reviewer can disagree with it in one
   place.

## Unresolved

Nothing blocks this unit. Two things a reviewer should know:

- The tidy gate (rules 16 and 19) is skipped by `--only` by design, so this
  unit did not re-measure the clang-tidy baseline; the full
  `./scripts/build-all.sh dev` run at the end of the wave is where that number
  moves.
- `packages/identity` is compiled by this project's graph (productivity's
  CMakeLists adds it for `argus_identity`, which only
  `productivity-controller-test` links). During this unit the identity tree was
  being edited by a parallel agent and broke that project's build twice; the
  final run quoted above is green on both trees as they stand.
