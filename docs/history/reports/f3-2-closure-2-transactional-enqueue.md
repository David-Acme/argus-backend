# Closure items 2 and 3 — the enqueue joins the domain write's transaction

Phase 3a, step 2's closure list, items 2 (S1) and 3 (S1b). One unit: four
producers, one invariant, one commit.

## What the two defects were

**S1 — the enqueue is not in the domain write's transaction.** A handler wrote
its row, the row committed, and only then was the change row written. A crash,
a `SIGKILL` or a storage error between the two lost the change for ever: no
later boot, drain pass or gRPC pull can discover a change that was never
recorded. Measured in three of the four producers (3a-2b camera, 3a-2c
notification, 3a-2d productivity).

**S1b — the enqueue's own give-up loses the change.** The outbox insert was
retried a bounded three times, 25 ms apart. A write that failed all three times
— `SQLITE_FULL`, `SQLITE_IOERR`, a corrupt page — was logged and dropped while
the handler still answered ok. The mutation had committed, so the row and its
change permanently disagreed. A lock needs sustained contention for ~15 s to
reach the retry path, so the fail-fast storage errors were the real window.

The repair is one transaction per write path: the domain row, every read the
path needs, and the change row are statements of the same `IMMEDIATE`
transaction, and a refused enqueue throws into it instead of being retried and
given up on.

## The invariant: the unit of work's client is borrowed, never owned

`packages/lib/sqlite/src/sqlite/transaction.{hxx,cc}` is new in this unit and
holds the whole mechanism: `begin(client)`, a `Commit` awaiter and
`rollback(transaction)`.

The load-bearing fact is measured, not assumed: **Drogon has no `commit()`**.
`~TransactionImpl` is what queues the commit through `loop->queueInLoop(...)`.
The transaction therefore commits only when its last `shared_ptr` drops, and
anything that keeps a copy — a struct field, a lambda capture, a container, a
`DbClientPtr` handed to a sink — keeps the caller suspended for ever with no
timeout, no error and a leaked connection.

`TransactionImpl`'s own internals were read to size this
(`/home/acme/.conan2/p/drogo7d4dffcaa9634/s/src/orm_lib/src/TransactionImpl.{h,cc}`):
`thisPtr_` at `h:160` is a strong self-reference installed by `doBegin()`
(`cc:296`), each buffered `SqlCmd` holds its own, and `DbClientImpl::makeTrans`
queues a copy. Measured baseline: `use_count()` is **5** right after `begin()`,
**4** after a statement has run and at commit time. An absolute
`use_count() != 1` assertion was tried as a diagnostic and abandoned: the
baseline is not 1 and it fluctuates, so the check would have been a fragile
constant. What replaced it is the type system.

Every input struct that crosses to a sink or a repository carries
`drogon::orm::DbClient* client{nullptr};` — a **non-owning** pointer the call
site resolves with `transaction.get()`. `.client = transaction` does not
compile, which is the enforcement. The single owner is always a local
`std::shared_ptr<drogon::orm::Transaction>` in the coroutine that opened it,
and `db_transaction::Commit(std::move(transaction))` moves that local in and
resets it, and that reset is what drops the last reference and runs the
destructor that queues the commit.

The rule is written into `packages/lib/sqlite/AGENTS.md`, together with the
read/mutation asymmetry it implies: a read may fall back to the pool
(`client() ? client : pooled.get()`), a mutation may not.

The `{nullptr}` default is deliberate. A bare `T x;` declaration must never
leave a wild pointer, and during this unit it did: leaving the field with no
default member initializer produced a `SIGSEGV` in `change-outbox-sink-test`
and `identity-change-outbox-sink-test`, because a statement dereferenced an
indeterminate pointer. Memory safety outranks a `-Wmissing-field-initializers`
warning, and the default restores exactly the semantics the tree had before.

The camera `update` path in the committed tree carried a live instance of the
trap: a named local `CameraUpdateInput` held the transaction as a
`DbClientPtr`, so the commit never fired — a hung request and a leaked
connection on every update. The raw-pointer field type makes that unwritable.

## The four producers

Every write path that mutates a row and then publishes a change now follows one
shape: `auto transaction = co_await db_transaction::begin(<owner>Client());`,
the row write and every read it needs and the sink call inside one `try` with
`.client = transaction.get()`, then
`if (!co_await db_transaction::Commit(std::move(transaction))) throw
ResponseException(<Owner>Errors::ChangeNotRecorded);`, then
`catch (...) { db_transaction::rollback(transaction); throw; }`. Side effects
that are not the change feed run only after that commit.

### camera — 4 sink call sites, all transactional

`services/camera/src/feature/api/{camera,zone}/services/*-feature-service.cc`.
`create`, `update` and `remove` on both features, with the repository reads and
mutations taking the borrowed client (`camera-repository.{hxx,cc}`,
`zone-repository.{hxx,cc}`) and the NATS source add/drop side effect after the
commit. `NatsCameraChangeSink` takes the outbox input struct and has no retry
loop. This half also introduced the shared vocabulary the others now use:
`sync/module-emit.hxx`, the `client` fields on the contracts, and the collapse
of the sink virtuals to input-struct shapes.

Its test is `services/camera/tests/unit/camera-change-transaction-test.cc`
(1 case, 28 assertions): a refusing sink rolls the whole unit back; a
successful create lands the row and its `change_outbox` row; a
`DROP TABLE change_outbox` fails the unit of work and commits nothing.

### notification — 1 write path

`markAsRead` in
`services/notification/src/shared/services/notification/notification-service.cc`
was the only path in the service that mutates a row and then publishes a
change. It now opens one transaction, updates the rows and publishes one user
audit per moved notification inside it, and commits.

- `NotificationMarkReadInput{userId, ids, client}` — a rule-2 struct in
  `notification-query.hxx`, replacing `markAsRead(int64_t, const
  std::vector<int64_t>&)`.
- `NatsNotificationChangeSink::enqueue` now takes the repository's own
  `ChangeOutboxEnqueueInput` and forwards `input.client`; `kEnqueueAttempts`,
  `kEnqueueRetryMs`, the loop and the now-unused `<trantor/net/EventLoop.h>`
  are gone, so a refused enqueue throws into the caller's transaction.
- `ChangeOutboxDisposition::Failed` is removed and the repository throws
  instead of downgrading.
- The oversize-payload drop (>256 KiB, `LOG_ERROR`, no throw) is **gone**: it
  is now the same catalogued refusal an id-less emit throws. The adversarial
  review of this unit found it (finding 1) and the four producers were
  converted together, because the two branches are the same defect — the
  handler answers ok while the change row was never recorded, which is
  exactly S1b's shape left standing one line above the retry loop that was
  just deleted. The policy is not weakened by being deterministic: a
  deterministic give-up is still a give-up. It now throws
  `NotificationErrors::ChangeNotRecorded` into the caller's transaction, so
  the domain write rolls back with it. Reachability was checked before the
  throw was kept: the notification DTOs bound `title` and `body` well under
  the budget, so no live request can reach it today — it is a guard against a
  future payload, and a guard that silently drops is not a guard. See "The
  adversarial review" below.
- One pre-existing assertion changed and had to:
  `notification-change-outbox-test.cc` asserted `== ChangeOutboxDisposition::Failed`,
  a value that no longer exists. It now asserts `CHECK_THROWS_AS(...,
  std::invalid_argument)` — byte for byte the assertion camera's landed test
  carries for the same case. The meaning is not weakened; the same input is
  still refused, and now loudly.
- Deliberately unchanged, with reasons in the unit's report: `create` and
  `createMany` are dead code that publish nothing; `createManyWithCommand`
  already writes inside its own transaction and what follows it is the durable
  delivery publish, not the change feed.
- `services/notification/CONTEXT.md` said the updates "commit first" and a
  refused enqueue "is retried before it is given up on". Both sentences are now
  false of the code and both were rewritten.

Its test is `services/notification/tests/unit/notification-change-transaction-test.cc`
(1 case, 26 assertions), covering the refusing sink, the committed pair, the
idempotent second read (no further audits), the durable sink's outbox row and
the dropped-table failure.

### productivity — 15 mutation routes across five features

`project`, `project-task`, `calendar-event`, `project-member` and
`calendar-event-share`, each with its own private emit input struct carrying
the client (`EmitInput`, `CanEditInput`, `CanWorkOnInput`, `EmitMembershipInput`,
`EmitParentInput`), plus the repositories' reads and mutations
(`findById`, `remove`, `memberIds`, `findAccess`, `findExisting`,
`updateAccess`, and the `create`/`update` inputs), and two new rule-2 structs
(`ProjectMemberLookupInput`, `ProjectMemberUpdateInput`) with their
calendar-event-share mirrors.

- The sink's private `emit` helper is reached through a single
  `emitUsers(const UserEmitInput&)`; the singular `emitUser` virtual is gone
  from the contract, so the two single-recipient `emitParent` sites now emit to
  a one-element recipient list. Same rows, same frames.
- The retry loop is gone and `ChangeOutboxDisposition::Failed` with it.
- `ProductivityErrors::ChangeNotRecorded` is the contract's ninth refusal, and
  the catalog test pins it (8 → 9) so it cannot sit outside the wire-legality
  and uniqueness invariants.
- **The client spelling is unified on `DbService::productivityClient()`.**
  This corrects the assumption the unit was briefed with. The brief claimed the
  two spellings resolved to the same object in production and in the controller
  test; the second half is false.
  `productivity-controller-test.cc` registers *identity.db* as the app default
  and installs *productivity.db* as the named client, so unifying onto
  `client()` would have pointed the repositories at another domain's tables in
  that suite. `productivityClient()` is the spelling that resolves to
  productivity's own database in the service (where nothing installs it and it
  falls back to the host's client) and in that test (where it is installed).
  The three boot calls in `main.cc` that still spelled `client()` now name it
  too.
- No reminder write path exists in this service: the reminder repositories are
  held only by the sync RPC service for pull reads, matching `CONTEXT.md`. The
  brief's count of fifteen write methods was right; the reminder repositories
  contribute none of them.

Its test is `services/productivity/tests/unit/productivity-change-transaction-test.cc`
(1 case, 64 assertions), covering the refusing sink on create, update and
remove, the committed pair, the durable sink's subjects and the dropped-table
failure.

### identity — 17 sink call sites, 11 transactional paths

Across `auth-service.cc`, `user-feature-service.cc`,
`invitation-feature-service.cc`, `portrait-preview-service.cc` and
`identity-rpc.cc`.

- **`registerUser`** is the one that had a transaction and did not use it: a raw
  `DbService::client()->newTransactionCoro(...)` closed before the four sink
  calls, which then ran in autocommit. It is now one `db_transaction` around
  the five inline writes, the `TRY_CONSUME` redemption, a
  `findById(invitation->id, transaction.get())` **added so the audit can see
  the uncommitted redemption row**, the catalog emit, the module emit, the
  invitation audit and the action. The old raw path also carried a
  `setCommitCallback` + `std::promise`/`std::future` handshake, which rule 15
  forbids outright; it is gone with the path. The vec0 face-index insert moves
  after the commit: a vector index no SQLite transaction covers must never
  index a person whose transaction rolled back.
- **`user-feature-service.update`** does the last-owner guard, the update, the
  recipients read, the users audit, the catalog row, the action and the
  refresh-token invalidation of a deactivation inside one transaction, and
  performs `replaceRoleRooms` and the `AuthContextChanged` emit (resync) only
  after the commit — which is rule 7b's ordering, and it is now the ordering the
  code has.
- **`portrait-preview-service.consume`** is the sharpest case: the one-use
  capability's consumption and the `UserAction::Read` audit are now the same
  transaction, and the private storage read happens after the commit. Before,
  the audit was a separate autocommit statement with the byte read in between —
  S1 on the privacy path. Audit metadata stays safe: the event name and the
  portrait's user id, no token, no bucket path, no bytes.
- `logout`, `updateMe` and `issueSession` (with a new
  `IssueDeviceCredentialInput`) are transactional; `IdentityRpcService`'s
  `UpdateUser`, `EnrollPerson` and `PromotePerson` wrap their writes and emits
  and place the vec0 insert after the commit.
- `NatsIdentityChangeSink` lost its retry loops; a refused enqueue throws.
- `IdentityErrors::ChangeNotRecorded` is the contract's thirty-first refusal,
  pinned by its catalog test (30 → 31).

Its test is `packages/identity/tests/unit/identity-change-transaction-test.cc`
(1 case, 39 assertions), covering invitation create, user update and promotion,
invitation revocation, the durable sink's per-subject counts
(`kIdentityChange`, `kIdentityUserAction`), the rename diff payload and the
dropped-table failure.

## Containment

The contract changes reach no unit outside the four producers. Measured with a
grep for implementors (`public CameraChangeSink`, `public UserChangeSink`,
`public IdentityChangeSink`, `public AuditSink`) and for callers
(`emitModule(`, `emitUsers(`, `emitUser(`, `publishAudit(`, `publishCatalog(`,
`publishUsersAudit(`, `publishModuleAudit(`, `publishAction(`) outside the four
producer trees: the only implementors are the four NATS sinks and their test
stubs, and there are no callers in `services/gateway`, `services/sync`,
`services/llm`, `packages/memory` or `packages/lib`. The gateway links
identity's sink but neither implements nor calls the virtuals.

Five pre-existing test files needed mechanical updates to the new shapes
(`notification-change-outbox-test.cc`, `identity-change-outbox-test.cc`,
`identity-change-outbox-sink-test.cc`, `productivity-controller-test.cc`,
`productivity-sync-rpc-test.cc`): the
designated initializers gain `.client = nullptr`, and the assertions that named
the removed `Failed` member or a `Task<bool>` that became `Task<void>` were
rewritten to the nearest stronger form.

## Recorded, not fixed

- **`approveDeviceLogin` and `refreshToken`** (three writes each in
  `auth-service.cc`) stay without a transaction. Neither publishes a change, so
  item 2 does not reach them; they are pre-existing atomicity gaps, recorded
  rather than silently changed in a unit about the change feed.
- **`UserInvitationRepository::tryConsume` and `recordRedemption` are dead
  code.** Verified: no caller outside their own repository, and `registerUser`
  redeems inline through `user_enrollment_query::INSERT_REDEMPTION`. A
  duplicate of a path that moved; removal belongs to a unit that owns the
  decision, not to this one.
- **Two outbox key shapes.** Identity's `change_outbox` keys on an integer `id`
  and carries a `subject` column plus a second `INSERT_ACTION` path; camera,
  notification and productivity key on a text `event_id` with one insert path.
  Not this unit's business — but item 4's retention policy has to work on both,
  and both carry `created_at`/`sent_at` to key it on.
- **The benign double rollback.** A path that rolls back explicitly and then
  throws into the `catch` rolls back twice, and Drogon logs
  `Transaction roll back error - TransactionImpl.cc:175` for it. Camera's
  landed reference test emits the same line. The behaviour is correct
  (`isCommitedOrRolledBack_` is what the destructor reads); the log line is
  noise, and it appears on the refusing-sink and early-return paths by design.
- **`db_transaction` is not yet the only commit mechanism.** guard's encounter
  drain still carries its own local `TransactionCommitAwaiter`. Moving it onto
  `db_transaction::Commit` is a change to a unit that is not a change producer,
  and it is deferred rather than smuggled in here.
- **A second private copy of the same mechanism.**
  `notification-repository.cc:26-57` defines its own `TransactionCommitAwaiter`
  for `createManyWithCommand`. It is the same shape `db_transaction::Commit`
  now provides, in a service that already links `lib/sqlite`. Found by the
  adversarial review (finding 5's second item) and left alone: it is not a
  correctness defect, it is a duplicate, and collapsing it is a one-line change
  to a function this unit already touched — which is exactly why it should be
  its own decision rather than a side effect here.
- **`TouchPerson` and `TagPerson` mutate a synced table and publish nothing.**
  `identity-rpc.cc:652` updates `person.lastSeenAt` and `:705-708` writes an
  observation, and neither emits a catalog row, an audit diff or an action.
  This is a *different* defect class from S1 and it is worse: S1 lost a change
  that was at least attempted, while these two never attempt one, so the
  clients' replicas of `person` drift with no event that will ever correct
  them. It is not one of the nine closure items, and the repair is a decision
  about change-feed volume — `lastSeenAt` moves on every recognition — not a
  mechanical move into a transaction that does not exist yet. Recorded here so
  it is not lost, and it should be the first item of a sync-volume unit.

## The full gate

`./scripts/build-all.sh dev` runs three gates of its own: `check-comments.sh`
and `check-deps.sh` before anything is built, and `check-tidy.sh` at the end of
a full run, over every project's compile database.

**The first full run of this change set failed on the tidy gate (exit 1).**
Everything before it was green — `check-comments: 1241 files checked, 0
comments`, `check-deps: 64 declarations, 493 edges, 0 forbidden, 0 cycles, 0
unresolved`, and all seventeen projects at `100% tests passed`. The tidy phase
then reported three risen checks over 503 translation units, 3107 findings
across 45 checks against a baseline of 3141 with two checks already below it:

| check | measured | baseline | delta |
|---|---|---|---|
| `bugprone-narrowing-conversions` | 69 | 68 | +1 |
| `bugprone-unchecked-optional-access` | 229 | 224 | +5 |
| `modernize-use-designated-initializers` | 304 | 302 | +2 |

Rules 16 and 19 are measured, not reviewed, and item 9 owns the one
`--write-baseline` of this phase. A rise is therefore fixed, never
re-baselined, and all eight findings were located in this change set:

- **`modernize-use-designated-initializers` (+2).** The four catalog tests this
  unit extends pin their table with positional aggregate braces, one finding
  per entry. The two entries the delta names are the ones added here; the fix
  is the check's own prescription, applied with `clang-tidy --fix` to all four
  tables (`camera`, `identity`, `notification`, `productivity`) rather than to
  the new entries alone, so the five sibling pinned tables do not disagree on
  the shape of a catalog row. A drop, not a rise: the four tables' entries
  leave the count.
- **`bugprone-unchecked-optional-access` (+5).** Three `REQUIRE(x.has_value())`
  guards followed by `x->field` in the two new transaction tests — one in
  `camera-change-transaction-test.cc`, four in
  `productivity-change-transaction-test.cc`. The analyzer does not see through
  doctest's `REQUIRE` macro, so the guard is invisible to it. The fix is not a
  pragma but a better assertion: each site now reads the row back from SQLite
  (`cameraName`, `projectName`, `eventTitle`, and a new `taskIdByTitle`) and
  asserts the *persisted* value, which is the stronger claim about a
  transactional write. Two small read-back helpers were added to
  productivity's suite for it.
- **`bugprone-narrowing-conversions` (+1).** `auth-service.cc` assigned
  `insertId()` (an `unsigned long long`) to `int64_t` in three places inside
  the enrollment path this unit rewrote. All three are now explicit
  `static_cast<int64_t>` — the two that were already in the baseline came down
  with them.

Nothing else in the tree rose. The other candidates a per-file rescan turned up
(`insertId()` assignments in the four producers' `create()` paths, the five
`SyncQueryParts` sites in `packages/contracts/sync/src/sync/sync-filter.hxx`)
are pre-existing and sit inside the baseline; the file carrying the
`SyncQueryParts` findings is not touched by this change set.

The `--fix` run had one effect outside the four catalog tests: it rewrote
`ErrorDefinition::withMessage`
(`packages/lib/errors/src/errors/error-definition.hxx:16`) to the designated
form too, because clang-tidy's fixer follows the header filter into every
header a fixed translation unit includes. That is the same check's own
prescription in a one-line function, it is a drop rather than a rise, and it
stays.

With the fixes in place all four producers were rebuilt and re-tested
individually, and each ended `EXIT=0` with `100% tests passed`: `camera` 54/54,
`notification` 43/43, `productivity` 38/38, `identity` 28/28. In front of each
build, `check-comments: 1243 files checked, 0 comments` (1241 before this change
set, +2 for the two new first-party files) and `check-deps: 64 declarations,
494 edges, 0 forbidden, 0 cycles, 0 unresolved`.

One hygiene note the run exposed: running a sink test **by hand from the
repository root** leaves its `*.db`, `-wal` and `-shm` files there (the tests
open a relative filename, and CTest gives them the build directory instead), and
`check-comments.sh` then fails on the unclassified `.db` rather than on a
comment. The files were removed; the gate was not weakened for them.

### The second run: one risen check, and the site behind it

The fixes above left the four producers green one by one, so the full gate was
run again over the whole tree. Everything before the tidy phase was green again
— `check-comments: 1243 files checked, 0 comments` (two more first-party files
than the first run, the two new ones), `check-deps: 64 declarations, 494 edges,
0 forbidden, 0 cycles, 0 unresolved`, and all seventeen projects at `100% tests
passed`. The scan fell to **3050 findings over 504 translation units** against
the 3141 baseline, with four checks below it — and still exited 1, on one risen
check the first run did not report:

| check | measured | baseline | delta |
|---|---|---|---|
| `bugprone-throwing-static-initialization` | 40 | 39 | +1 |

The diff adds no `static` and none of the new files declares one, so the site
was measured rather than guessed: `clang-tidy` with that single check over the
tree's 504 translation units (the gate's own `translation_units()` and `tidy()`,
driven from a scratch script) and the distinct findings listed by
`(file, line, check, message)`, which is the gate's own key. One of the 40 is
new, and it is not a variable at all in the sense the search assumed:

```
packages/contracts/notification/tests/unit/notification-contract-catalog-test.cc:18:33:
initialization of 'kCatalog' with static storage duration may throw an exception
that cannot be caught [bugprone-throwing-static-initialization]
```

— the catalog table the notification boundary's own suite pins, declared as
`const std::vector<CatalogEntry> kCatalog{...}`. A namespace-scope
`std::vector` is a static-storage-duration object initialised at run time, and
`vector`'s constructor allocates, so the check is right about it. The same
declaration in the three older catalog suites (`camera`, `identity`,
`productivity`, all tracked at `HEAD`) is already inside the baseline, which is
why a fourth one moves the count and only by one.

The fix is the shape the tree already uses for a compile-time table
(`packages/lib/phrase/src/phrase/details/vocabulary-en.hxx:9`): a
`constexpr std::array<CatalogEntry, N> kCatalog{{...}}`, with `#include <array>`
in place of `#include <vector>`, applied to all four catalogs so the sibling
tables keep one shape. `N` is the count each suite already asserts
(`CHECK(kCatalog.size() == 1)` in notification, `9` in camera and productivity,
`31` in identity), and the extra brace layer is what lets the existing
designated entries initialize the array's element. `std::array` of a literal
type is constant-initialised, so the check has nothing to report: re-measured on
all four translation units with that check and
`modernize-use-designated-initializers` together, all four come back clean. The
three older tables leave the count with it, and the table is now evaluated at
compile time instead of on every test run.

### The third run: green, and this change set's verdict

With the four catalogs in their new shape the whole gate was run a third time:
**exit 0**. `check-comments: 1243 files checked, 0 comments`; `check-deps: 64
declarations, 494 edges, 0 forbidden, 0 cycles, 0 unresolved`; all seventeen
projects at `100% tests passed`, **437 tests** and not one compiler warning; and
the tidy phase at **504 translation units, 3046 findings over 45 checks against
the 3141 baseline, five checks below it and none above** — 95 findings fewer
than the baseline, the catalog tables' entries and the `ErrorDefinition` line
among them. No check was re-baselined to get there: item 9 still owns this
phase's single `--write-baseline`.

## The adversarial review

A fresh agent reviewed the change set read-only, with the three producer
reports handed to it as **claims to falsify**, not as facts: it re-read every
call site, every sink virtual and every touched assertion against the tree and
ran nothing but greps and file reads. It found five things. Four were real
defects and are fixed here; the fifth is a different defect class and is
recorded above.

**1. The silent give-up survived in all four sinks — inside the transaction,
without throwing.** The oversize branch and the missing-`id` branch still did
`LOG_ERROR` + `co_return`, so the caller's `Commit` succeeded and the handler
answered ok: the exact S1b end state, one line above the retry loop that this
unit deleted, and now *more* dangerous because the domain row commits with it.
The reviewer's reachability check (every HTTP DTO string bounded at ≤2000
chars) is why this was not a live incident, and its own caveat is why it still
had to be fixed: identity's gRPC and the sync-RPC surface are inputs it could
not size. Fixed in all five branches (camera `emitModule`/`enqueue`,
productivity `emitUsers`/`enqueue`, identity `emitModule`/`enqueue`/
`enqueueAction`, notification `enqueue`) — each now logs and throws its
contract's `ChangeNotRecorded` into the caller's transaction, so the domain
write rolls back with the refused change. Notification is safe by construction
on the `id` branch (its `recordId` is a typed field, not parsed from JSON) and
needed only the oversize one.
The four live suites pinned the old drop (`CHECK_FALSE(hasPending(outbox))`
right after an unwrapped call), so they had to change with it: every one of the
nine sites now asserts `CHECK_THROWS_AS(..., ResponseException)` **and** the
empty outbox. The claim in an earlier draft of this report that the drop "is
kept exactly as it was" was false of the shipped code and has been rewritten
where it stood.

**2. camera's new refusal was declared and thrown but not pinned.** The
contract gained `ChangeNotRecorded` and six sites throw it, while
`camera-contract-catalog-test.cc` still listed eight entries and asserted
`kCatalog.size() == 8`. Identity and productivity had both grown their pinned
tables; camera had not, so a wrong `ErrorCode`, status or message on the new
entry would have passed the wire-legality and uniqueness loops. Fixed: nine
entries, `CHECK(kCatalog.size() == 9)`. **The gate had already missed it** —
the camera catalog test passed green in the full run — which is the argument
for the table being pinned, not against.

**3. notification's commit failure threw an uncatalogued `std::runtime_error`.**
`markAsRead` threw `std::runtime_error("notification read commit failed")`
where the other three producers throw their contract's `ChangeNotRecorded`.
`packages/contracts/notification/src/notification/` had no errors header at
all, so the boundary had nowhere to catalog a refusal, and
`ErrorHandler::handleException` puts `e.what()` on the wire for a non-
`ResponseException` — an internal string where a client-visible one belongs.
Fixed: `packages/contracts/notification/src/notification/notification-errors.hxx`
is new with the one refusal, `notification-service.cc` throws it,
`packages/contracts/notification/CMakeLists.txt` declares the header and its
`argus::lib::errors` dependency, and a catalog test pins it (the notification
contract's table is new, so its first entry is pinned from the start).

**4. `sync/module-emit.hxx` was the one public header of the twenty not
declared in its own `CMakeLists.txt`.** The header the whole borrowed-client
invariant travels in — `ModuleEmitInput`, included by the camera, productivity
and identity sinks — was absent from the sync contract's `SOURCES`, so any
consumer driven by that list saw a sync contract with nineteen headers. Fixed:
declared in its home, per rule 25.

**5. Two things the report did not name, and one of them is worse than the
defect being fixed.** `TouchPerson` (`identity-rpc.cc:652`) and `TagPerson`
(`:705-708`) mutate synced `person` rows and publish nothing at all, so a
replica can never converge on them; and notification's repository
(`notification-repository.cc:26-57`) carries a private copy of the commit
awaiter that `db_transaction` now provides. Both are in "Recorded, not fixed"
above, with the reason each is not this unit's repair.

Two things the review listed as *unverifiable* are recorded rather than
answered: whether a >256 KiB payload is reachable through identity's gRPC or
the sync-RPC surface (no HTTP path reaches it; those two were not sized), and a
residual hazard in the helper itself — awaiting `Commit` on a transaction that
was already rolled back would hang for ever, because Drogon fires the commit
callback only when `!isCommitedOrRolledBack_`. The reviewer verified
mechanically that no path rolls back and then commits, so it is not a live
defect; it is also not defended against in `transaction.cc`, and no test would
catch a future one. That is a note for whoever next touches the helper.
