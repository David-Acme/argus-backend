# 3a step 2 closure, items 2 and 3 — the identity producer's transaction (S1, S1b)

Closure item 2 (S1) was the finding that the identity change-feed enqueue sat
outside the domain write's transaction: the row was committed by one statement
and its change was recorded — or not — by another, so a refusal in between left
a user, an invitation or a person that the sync stream never learned about.
Item 3 (S1b) was the consequence: because the enqueue could not fail the write,
it retried instead, and a retry that eventually gave up left the same partial
state with a log line as the only trace.

This unit closes both for `packages/identity`: every identity path that mutates
a row and then publishes a change now runs as one `db_transaction`
(`packages/lib/sqlite/src/sqlite/transaction.hxx`) — domain write, audit/emit
and outbox row together — and a failed enqueue throws into the caller's
transaction so the whole unit of work rolls back. Side effects that are not the
change feed run after the commit.

## The single owner a Drogon transaction needs

Drogon has no `commit()`; `~TransactionImpl` queues the commit. The transaction
therefore has exactly one owner, a local
`std::shared_ptr<drogon::orm::Transaction>` declared in a scope **outside** the
`try`, so the `catch` can call `db_transaction::rollback(transaction)` (a
null-safe no-op that resets the pointer) and the destructor then finds nothing
to commit. Every restructured path in this unit follows the camera precedent
(`services/camera/src/feature/api/camera/services/camera-feature-service.cc`):
`begin` → domain write + publish inside one `try` → early exits roll back
explicitly → `if (!co_await db_transaction::Commit(std::move(transaction))) throw
ResponseException(IdentityErrors::ChangeNotRecorded);` → `catch (...) { rollback;
throw; }` → side effects after the commit.

One measured trap cost a build cycle and is worth recording: `db_transaction`
needs an explicit `#include <sqlite/transaction.hxx>` in identity, because
nothing in that package brings the header in transitively. Of the five
translation units that use it, three got the include from the start
(`invitation-feature-service.cc`, `portrait-preview-service.cc`,
`identity-rpc.cc`); `auth-service.cc` had `<sqlite/db-service.hxx>` but not the
transaction header, and `user-feature-service.cc` had neither, which produced
`'db_transaction' has not been declared` plus a cascade of misleading
`cannot convert '<brace-enclosed initializer list>'` errors on the designated
initializers that used `transaction.get()`. Both includes are in now.

## Every write path touched

The table lists every identity function that mutates a row and publishes a
change, the transaction it now shares, and the publish sites that moved inside
it. Column three names the function that owns the transaction; column four the
sink calls whose `.client` is now that transaction.

| # | Function | Transaction | Publishes now inside it |
|---|---|---|---|
| 1 | `AuthService::registerUser` (`auth-service.cc:129`) | `begin` `:204`, `Commit` `:299` | `publishCatalog` `:251`, `emitModule` `:263`, `publishModuleAudit` `:271`, `publishAction` `:286` |
| 2 | `AuthService::logout` (`auth-service.cc:514`) | `begin` `:518`, `Commit` `:537` | `publishAction` `:526` |
| 3 | `AuthService::updateMe` (`auth-service.cc:562`) | `begin` `:566`, `Commit` `:597` | `publishUsersAudit` `:588` |
| 4 | `AuthService::issueSession` (`auth-service.cc:635`) | `begin` `:645`, `Commit` `:696` | `publishAction` `:685`; also `issueDeviceCredential`'s credential row (`:626`) and the refresh-token row (`:670`) |
| 5 | `UserFeatureService::update` (`user-feature-service.cc:40`) | `begin` `:48`, `Commit` `:113` | `publishUsersAudit` `:82`, `publishCatalog` `:98`, `recordChange`→`publishAction` `:186`, and `invalidateAllUser` on deactivation (`:109`) |
| 6 | `InvitationFeatureService::create` (`invitation-feature-service.cc:76`) | `begin` `:86`, `Commit` `:105` | `emitInvitation`→`emitModule` `:185`, `recordInvitationAction`→`publishAction` `:195` |
| 7 | `InvitationFeatureService::revoke` (`invitation-feature-service.cc:127`) | `begin` `:133`, `Commit` `:167` | `publishModuleAudit` `:151`, `recordInvitationAction`→`publishAction` `:195` |
| 8 | `PortraitPreviewService::consume` (`portrait-preview-service.cc:113`) | `begin` `:121`, `Commit` `:155` | `publishAction` (the `UserAction::Read` audit event) `:144` |
| 9 | `IdentityRpcService::UpdateUser` (`identity-rpc.cc:102`) | `begin` `:140`, `Commit` `:165` | `emitModule` `:161` |
| 10 | `IdentityRpcService::EnrollPerson` (`identity-rpc.cc:495`) | `begin` `:546`, `Commit` `:589` | `emitModule` `:585` |
| 11 | `IdentityRpcService::PromotePerson` (`identity-rpc.cc:863`) | `begin` `:937`, `Commit` `:965` | `publishModuleAudit` `:954` |

Two helpers carry a borrowed client rather than owning a transaction, because
their only callers are inside one: `UserFeatureService::recordChange`
(`user-feature-service.cc:183`, `input.client` from `UserChangeLogInput`) and
`InvitationFeatureService::emitInvitation` / `recordInvitationAction`
(`invitation-feature-service.cc:177` and `:191`, from `InvitationEmitInput` and
`InvitationActionLogInput`).

**How the list was shown to be complete.** The inventory is the set of change-sink
call sites in the package, and that set is finite and greppable:

```
grep -rn "identity_change::getSink()\|sink->publish\|sink->emitModule" packages/identity/src
```

returns **17 guard blocks and 17 sink calls in source** — one call per guard —
all of them in the five files named above, and each is inside one of the 11
transaction scopes in the table:

- `registerUser` 4, `logout` 1, `updateMe` 1, `issueSession` 1 (`auth-service.cc`);
- `update` 2 plus `recordChange` 1 (`user-feature-service.cc`);
- `create` 2, `revoke` 1 plus `recordInvitationAction` 1 (`invitation-feature-service.cc`);
- `consume` 1 (`portrait-preview-service.cc`);
- `UpdateUser` 1, `EnrollPerson` 1, `PromotePerson` 1 (`identity-rpc.cc`).

Those 17 call sites are reached as 18 invocations along the 11 paths, because
`recordInvitationAction` serves both `create` and `revoke`.

The remaining identity writers publish nothing, so item 1 does not reach them:
`approveDeviceLogin`, `refreshToken`, `createDeviceLogin`, `pollDeviceLogin`,
`portrait-preview-service create`, `identity-rpc TouchPerson` and `TagPerson`,
`InvitationFeatureService::resolve` (read-only) and
`UserInvitationRepository::tryConsume`/`recordRedemption` (no callers). A second
grep confirms identity has no other publish path at all — there is no direct
`publish(` call anywhere under `packages/identity/src`, and
`identity_change::setSink` is installed in exactly one production place,
`services/gateway/src/main.cc:319`.

The brief's "~23 change-sink call sites" and the parent decision report's
identity count of 17 both differ from the measured 17 in-source calls plus one
shared helper reached from two paths (18 invocations). My basis is each
`sink->publish*`/`sink->emitModule` call, which is the smallest unit that can
carry a `.client`.

## `registerUser`, before and after

At `HEAD` the function did open a transaction, but not the `db_transaction` one:

```cpp
auto indexResult = std::make_shared<std::promise<bool>>();
auto indexFuture = std::make_shared<std::future<bool>>(indexResult->get_future());
{
  auto transaction = co_await DbService::client()->newTransactionCoro(
      drogon::orm::TransactionType::Immediate);
  transaction->setCommitCallback([indexResult, indexInput](bool committed) {
    if (!committed) { indexResult->set_value(false); return; }
    indexResult->set_value(FaceService::instance().faceDb().insert({...}));
  });
  ... COUNT_USERS / TRY_CONSUME / INSERT_USER / INSERT_PERSON /
      INSERT_FACE_EMBEDDING / INSERT_REDEMPTION on transaction->execSqlCoro ...
}
const bool indexed = co_await BlockingTask<bool>(
    [indexFuture] { return indexFuture->get(); });
if (!indexed)
  throw ResponseException(503, IdentityErrors::EnrolledFaceIndexFailed);
co_await privatePortraitService_.store(userId, portraitImage);
... publishCatalog, emitModule, publishModuleAudit, publishAction, all after the block ...
co_return co_await issueSession({...});
```

Two things were wrong with it beyond the item's finding. The vec0 index insert
could only be reached through a `setCommitCallback`, which cannot report a
failure back to the coroutine — so the code bridged callback to coroutine with a
`std::promise`/`std::future` pair and a `BlockingTask`, and `std::future` is
forbidden by the tree's rules. And the four publishes ran after the transaction
had already committed, so a refused sink left a user and a person the sync
stream never saw.

After:

```cpp
auto transaction = co_await db_transaction::begin(DbService::identityClient());
try {
  ... the same domain statements on transaction->execSqlCoro ...
  co_await sink->publishCatalog({..., .client = transaction.get()});
  co_await sink->emitModule({.table = TableName::User, .body = emit,
                             .client = transaction.get()});
  co_await sink->publishModuleAudit({..., .client = transaction.get()});
  co_await sink->publishAction({.event = {...}, .client = transaction.get()});
  if (!co_await db_transaction::Commit(std::move(transaction)))
    throw ResponseException(IdentityErrors::ChangeNotRecorded);
}
catch (...) { db_transaction::rollback(transaction); throw; }

const bool indexed = co_await BlockingTask<bool>([embedding = face->embedding,
                                                  personId, faceEmbeddingId] {
  return FaceService::instance().faceDb().insert({...});
});
if (!indexed)
  throw ResponseException(503, IdentityErrors::EnrolledFaceIndexFailed);
co_await privatePortraitService_.store(userId, portraitImage);
co_return co_await issueSession({...});
```

The vec0 insert had to move to the far side of the commit: `FaceDb::insert` runs
on the vec0 connection, and issuing it while this transaction holds the write
lock would have it fight the lock. It now captures the embedding and the two
inserted ids by value into `BlockingTask<void>`, so no callback, no promise, and
`#include <future>` is gone from the file. The "already registered" early return
still ends in `issueSession`, which now opens its own transaction, so a
re-login is one unit of work too.

## The `enqueueAction` decision (item 4)

The brief asks what the right contract is now, and this is the judgement I made
and defend: **the enqueue reports failure by throwing, not by returning a
value.**

At `HEAD` both outbox writes swallowed their own errors and reported them as
data: `ChangeOutboxRepository::enqueue` caught every `std::exception` and
returned `ChangeOutboxDisposition::Failed`, `enqueueAction` returned
`Task<bool>`, and the sink turned either into a bounded retry
(`kEnqueueAttempts = 3`, `kEnqueueRetryMs = 25`) whose last act was
`LOG_ERROR << "... could not be recorded in 3 attempts; the change is lost"`.
The caller had no rollback path at all, so a `false` was not actionable: the
only thing it could do with it was log it, and the change feed had already lost
the row.

Once the enqueue shares the caller's transaction, a boolean return becomes
actively wrong. A failed enqueue is not a fact the caller should be free to
ignore; it is the failure of the unit of work whose whole reason for existing is
that the change gets recorded. The value-returning form invites exactly the
current bug back — a caller that logs and continues commits a partial state.
Throwing removes the retry loop with it: there is nothing to retry, because the
transaction the enqueue participates in is what will be retried (by the client,
against a fresh unit of work), and a retry inside the unit of work is what S1b
named as the bug.

The dispositions that remain are `Enqueued`, `Replay` and `Conflict`, and they
are not failures. `Replay` means the identical transition is already recorded —
`eventId` hashes the payload itself, so an identical `eventId` implies an
identical fingerprint, and the row that exists wins. `Conflict` (same `eventId`,
different fingerprint) is therefore reachable only through a hash collision or a
hand-written row; it stays a logged non-dispatch, and it does not throw, because
the recorded row is the one the consumer will see and the caller's domain write
is already consistent with it. Both are pinned by
`identity-change-outbox-test.cc:172-175`.

## The enqueue contract, before and after

| | Before | After |
|---|---|---|
| `ChangeOutboxDisposition` | `Enqueued`, `Replay`, `Conflict`, `Failed` | `Enqueued`, `Replay`, `Conflict` (`Failed` deleted) |
| `ChangeOutboxRepository::enqueue` | empty id/payload → `Failed`; any exception → `LOG_ERROR` + `Failed` | empty id/payload → `throw std::invalid_argument`; a client error propagates |
| `ChangeOutboxRepository::enqueueAction` | `Task<bool>`; empty payload → `false`; any exception → `LOG_ERROR` + `false` | `[[nodiscard]] Task<void>`; empty payload → `throw std::invalid_argument`; a client error propagates |
| Client | always the pooled `DbService::client()` | `input.client ? input.client : DbService::identityClient().get()` |
| `NatsIdentityChangeSink::enqueueChange` | 3 attempts × 25 ms `sleepCoro`, then "the change is lost" | one `co_await outbox_.enqueue(input)`; the retry constants are deleted |
| `NatsIdentityChangeSink::enqueueAction` | same retry loop over the bool | one `co_await outbox_.enqueueAction(input)` |
| Wake-up | `wake_.notify_all()` after a successful enqueue | unchanged, still after the write |

`wake_.notify_all()` inside the transaction is safe and stays: the drain selects
`pending` rows, an uncommitted row is invisible to the drain's own connection,
and the notify is what wakes the drain for a `Replay` (an already-committed row)
as well. The oversized-payload drop is untouched, see below.

The callers' side of the contract changed accordingly: `ChangeOutboxEnqueueInput`
and `ChangeOutboxActionInput` gained `drogon::orm::DbClient* client{nullptr}`,
and the sink's private `enqueueChange(eventId, payload)` collapsed into
`enqueue(ChangeOutboxEnqueueInput input)` because the struct now carries the
event id and the client that the two-argument form could not.

## The oversized-payload drop, kept exactly as it is

`NatsIdentityChangeSink::kMaxPayloadBytes` (256 KiB) is still enforced before
the row reaches the database, in both `enqueue` and `enqueueAction`, with the
same `LOG_ERROR` and the same `co_return`. This is a deliberate, tested policy
and this unit does not convert it into a throw: a payload past the broker's
message budget can never be delivered, so rolling back the domain write would
let a broker limit reject a legitimate user action. The sink test pins it
(`identity-change-outbox-sink-test.cc`, the `oversizedAction` case →
`CHECK_FALSE(hasPending(outbox))`).

## Side effects after the commit (item 5)

| Path | Side effect | Where it runs now |
|---|---|---|
| `registerUser` | vec0 `FaceDb::insert` (separate connection) | after `Commit` (`auth-service.cc:307`) |
| `registerUser` | `privatePortraitService_.store` | after the index insert (`:317`) |
| `logout` | `sync_control::disconnectUser` | after `Commit` (`:552`) |
| `updateMe` | none | — |
| `update` | `replaceRoleRooms` + `emitAuthContextChanged(resync=true)` when the role changed | after `Commit` (`:121-132`) |
| `update` | `disconnectUser` when the user was deactivated | after `Commit` (`:134-145`) |
| `update` | `refreshTokenRepository_.invalidateAllUser` when deactivated | **inside**, because it is a row write (`:109`) |
| `invitation create` / `revoke` | none | — |
| `portrait consume` | `privatePortraitService_.read` + base64 | after `Commit` (`portrait-preview-service.cc:163`) |
| `EnrollPerson` | vec0 `FaceDb::insert` | after `Commit` (`identity-rpc.cc:600`) |

The rule 7b order is preserved exactly: a role update persists first, then calls
`replaceRoleRooms`, then emits `AuthContextChanged` with `resync=true`, keeping
the socket and the user room; only deactivation disconnects the device. No
side effect was moved into a transaction, and none was left inside one.

Portrait objects remain private server storage and never a sync row — the
boundary is untouched. On the invitation side, `ResponseInvitationDto.token`
still exists only in the HTTP response; the emit and the audit use
`UserInvitationSchema::toJson()`, which excludes the token hash, and no token or
hash is logged or placed in a payload.

## The repository layer (item 2)

Mutation methods borrow the client through the input struct that already
describes the write, as the brief asks — each gained a trailing
`drogon::orm::DbClient* client{nullptr}` and each `.cc` resolves it the same way:

```cpp
const auto pooled = DbService::client();
auto* client = input.client ? input.client : pooled.get();
```

`auto*` rather than `const auto*`, because `execSqlCoro` is non-const, and the
pooled `shared_ptr` outlives the call. Files: `user-query.hxx` (`UserUpdateInput`,
which already carried it), `change-outbox-query.hxx`
(`ChangeOutboxEnqueueInput`, `ChangeOutboxActionInput`), `device-credential-query.hxx`,
`face-embedding-query.hxx`, `person-snapshot-query.hxx`,
`portrait-preview-capability-query.hxx`, `refresh-token-query.hxx`,
`user-invitation-query.hxx` (`UserInvitationCreateInput`,
`UserInvitationRevokeInput`) and `person-query.hxx` (`PersonCreateInput`).

Reads that a transaction must see its own uncommitted rows through took an
optional trailing parameter instead — `findById(id, client = nullptr)`,
`findAll(client = nullptr)`, `hasOtherActiveOwner(id, client = nullptr)`,
`promote(id, client = nullptr)`, `findByTokenHash(hash, client = nullptr)`,
`invalidateAllUser(userId, client = nullptr)`. Every one of them is a
two-parameter signature, so rule 2 is not engaged and no new input struct was
invented for a read; nothing in this unit introduced a bare multi-parameter
signature either.

A detail that makes the new test meaningful: `DbService::identityClient()` falls
back to `client()` when the identity client has not been installed, which is the
case in the unit tests. The test's `transactional()` assertion (the sink saw a
client that is not the pooled one) therefore proves the call sites pass the
transaction and not a fallback.

## The test

`packages/identity/tests/unit/identity-change-transaction-test.cc`, registered
in `packages/identity/CMakeLists.txt:267-284` the way its siblings are — an
explicit source list, the `ARGUS_IDENTITY_SCHEMA` compile definition pointing at
`database/schema.sql`, `-Wall -Wextra`, no glob:

```cmake
add_executable(identity-change-transaction-test
    tests/unit/identity-change-transaction-test.cc)
target_link_libraries(identity-change-transaction-test PRIVATE
    argus_identity
    doctest::doctest)
target_compile_definitions(identity-change-transaction-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
```

It mirrors camera's `camera-change-transaction-test.cc` and proves three things
with a `RefusingSink` that records the client it was handed and throws on every
method:

- **a refused publish commits nothing** — invitation `create` throws
  (`std::runtime_error`) and `user_invitation` stays empty; a user role update
  throws and the stored role stays `guest`; `revoke` throws and `revoked_at`
  stays null;
- **a success lands the row and its outbox rows together** — with an accepting
  sink the invitation row exists, the sink saw the transaction (not the pooled
  client), and with a durable `NatsIdentityChangeSink` the invitation create
  produces 2 outbox rows (one `argus.identity.v1.change` plus one
  `argus.identity.v1.user-action`, both carrying the invitation id) while a user
  rename produces 3 (two change rows plus one action row, the first carrying
  `"current":"Ana Maria"`);
- **a dropped `change_outbox` table fails the unit of work** — after
  `DROP TABLE change_outbox`, invitation create, user rename and revoke all
  throw, and none of them commits: the invitation count is unchanged, the name
  is still `Ana Maria`, the revocation is still null.

## Evidence

`flock /tmp/argus-build.lock ./scripts/build-all.sh dev --only identity`, exit
code 0, with every changed source touched first so the same run recompiled all
ten touched translation units (`grep -c "warning:"` = 0, `grep -c "error:"` =
0). Verbatim:

```
check-comments: 1241 files checked, 0 comments
check-deps: 64 declarations, 493 edges, 0 forbidden, 0 cycles, 0 unresolved, 49 edges deferred to phase 3 (227 third-party mentions over 22 roots)
100% tests passed, 0 tests failed out of 28
Total Test time (real) =   6.51 sec
[setup] All selected projects built and tested (profile: dev).
```

Per-binary doctest counts for the identity suite:

| Binary | Cases | Assertions |
|---|---|---|
| `identity-change-transaction-test` (new) | 1 | 39 |
| `identity-change-outbox-test` | 2 | 56 |
| `identity-change-outbox-sink-test` | 1 | 93 |
| `device-credential-test` | 2 | 55 |
| `identity-migration-test` | 6 | 116 |

`--only` skips `scripts/check-tidy.sh` by design, so rules 16 and 19 have not
been measured over this change; the new code avoids raw index loops, C-style
casts and parameter pairs, and the struct-over-parameters shape is the one the
measured note says escapes the swappable-parameter ratchet. Nothing was
committed.

One pre-existing test was updated rather than left broken, and it is a
consequence of the contract change, not a weakened assertion:
`identity-change-outbox-test.cc` used to assert the action enqueue's return
value. With `enqueueAction` now returning `Task<void>`, `CHECK_FALSE(sync_wait(...))`
no longer compiles ("forming reference to void"), so the two accepted calls are
bare `drogon::sync_wait(...)` statements (the notification sibling's shape) and
the empty-payload case is pinned with
`CHECK_THROWS_AS(..., std::invalid_argument)`. The notification unit's build hit
this break in parallel and flagged it; it is fixed here.

## Deliberately not changed

- `AuthService::approveDeviceLogin` (`auth-service.cc:348-394`) performs three
  writes — a device credential, a refresh-token row and the challenge's
  `markApproved` — and publishes no change, so item 1 does not reach it. It
  stays without a transaction. That is a real atomicity gap of a different class
  (a crash between them leaves a challenge marked approved whose session row is
  missing), it is pre-existing, and it needs its own call-site audit and its own
  decision rather than a silent fix inside this unit. Flagged, not fixed.
- `AuthService::refreshToken` (`auth-service.cc:447-512`) rotates refresh tokens
  (`markUsed`, `pruneStale`, `create`) and publishes nothing; same reasoning,
  same gap, also pre-existing.
- `createDeviceLogin` / `pollDeviceLogin` write challenge rows only and publish
  nothing.
- `PortraitPreviewService::create` (`portrait-preview-service.cc:89`) mints a
  one-use capability and publishes no domain change; the audit event belongs to
  `consume`, which is the path this unit wrapped. Unchanged.
- `IdentityRpcService::TouchPerson` (`identity-rpc.cc:623`) and `TagPerson`
  (`:672`) mutate person rows without publishing a change, so they were not in
  the sink inventory. Unchanged.
- `UserInvitationRepository::tryConsume` and `recordRedemption` have no callers
  at all (measured: only their declaration and definition exist; the enrollment
  path uses `user_enrollment_query::TRY_CONSUME`/`INSERT_REDEMPTION` inline
  inside `registerUser`'s transaction). This is pre-existing dead code left in
  place — removing repository API is a separate reviewable step — and it is
  flagged here for a follow-up.
- The oversized-payload drop policy, for the reason given above.

## Where the tree disagrees with the brief

- The brief describes `registerUser` as a function that "already opens a
  transaction but its sink calls sit OUTSIDE it". That is true in effect, but
  the transaction at `HEAD` was not `db_transaction`: it was
  `DbService::client()->newTransactionCoro(Immediate)` with a
  `setCommitCallback`, and the vec0 insert rode that callback through a
  `std::promise`/`std::future` pair. So the change there is larger than moving
  four calls inward — the transaction API itself was replaced, the forbidden
  `std::future` removed, and the vec0 insert moved behind the commit because it
  cannot run on the transaction's connection.
- The call-site count: the brief says "~23" and the parent decision report
  counted 17 for identity; the measured number is 17 `sink->publish*`/
  `sink->emitModule` calls in source (17 `getSink()` guards) across 5 files,
  reached as 18 invocations because one helper serves two paths. The
  enumeration basis is stated above so the numbers can be reconciled.
- One deliberate deviation from the camera precedent:
  `services/camera` added its own `ChangeNotRecorded`-equivalent behaviour but
  its contract catalog test stayed at 8 entries, leaving the new error out of
  the pinned catalog. Here the new `IdentityErrors::ChangeNotRecorded`
  (`packages/contracts/identity/src/identity/identity-errors.hxx`, an
  `ErrorCode::InternalError` / 500 entry) is pinned in
  `packages/contracts/identity/tests/unit/identity-contract-catalog-test.cc`,
  whose count went 30 → 31. The error is thrown on a refused commit, so it is
  part of the wire vocabulary and hiding it from the catalog would defeat the
  catalog.
- Cross-unit dependency worth naming: this unit adds `client` to identity's own
  contract (`packages/contracts/sync/src/sync/identity-change-sink.hxx` —
  `IdentityCatalogInput::client`, the new `ActionPublishInput`, `emitModule`
  taking `ModuleEmitInput`, `publishAction` taking `ActionPublishInput`), but it
  *relies on* three shared artifacts that this unit did not author:
  `ModuleAuditInput::client`
  (`packages/contracts/sync/src/sync/module-audit-event.hxx`) and
  `UserAuditInput::client` (`packages/contracts/sync/src/sync/user-change-sink.hxx`)
  are working-tree edits by sibling units, and `ModuleEmitInput` itself
  (`packages/contracts/sync/src/sync/module-emit.hxx`) is a new file of this
  same closure workstream — `git cat-file -e HEAD:<path>` reports it is not at
  `HEAD`, so it is untracked rather than unmodified. The same is true of the
  `db_transaction` module every path in the table uses:
  `packages/lib/sqlite/src/sqlite/transaction.{hxx,cc}` and the two lines of
  `packages/lib/sqlite/CMakeLists.txt` that compile it are new and uncommitted.
  Identity's `publishModuleAudit`/`publishUsersAudit`/`emitModule` call sites
  cannot compile without those members, so whichever unit lands last must keep
  them.

## Headers other projects compile

`packages/identity` is `add_subdirectory`'d by camera, gateway, guard,
notification and productivity, so every header this unit changed travels into
those builds. The changed headers are `auth-service.hxx` (new
`IssueDeviceCredentialInput` and the private `issueDeviceCredential` signature),
`user-feature-service.hxx` (`UserChangeLogInput::client`),
`invitation-feature-service.hxx` (`InvitationActionLogInput::client` and the new
`InvitationEmitInput`), the eight `*-query.hxx` files listed under the repository
section, and the repository headers that gained optional client parameters
(`user-repository.hxx`, `person-repository.hxx`,
`user-invitation-repository.hxx`, `portrait-preview-capability-repository.hxx`,
`refresh-token-repository.hxx`). Every addition is additive and defaulted, and
no consumer outside `packages/identity` constructs any of the changed structs —
measured by grepping `packages/` and `services/` for each struct name; the one
type they share, `UserAuditInput`, lives in the sync contract and is not this
unit's to change. **Camera was not rebuilt by me**, per the brief; it compiles
identity's library, and identity's own build compiled all ten touched
translation units with `-Wall -Wextra` and zero warnings.

One cross-check I did not run myself: the notification unit's build
(`./scripts/build-all.sh dev --only notification`), which also
`add_subdirectory`s `packages/identity` and therefore compiles the same headers
and the same three identity test binaries, came back green on this tree — 42/42
ctest, 0 errors, no compiler warnings — as reported by that unit's agent. That
is one non-camera project's confirmation that the additive header changes do not
disturb a consumer; camera itself is still David's run.
