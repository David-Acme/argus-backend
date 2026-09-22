# Sub-step 3a-2e — the identity durable change outbox

Scope: `packages/identity` stops publishing its change feed as a fire-and-forget
core NATS publish and starts writing every catalog row, module emit, audit diff
and action-journal row into an identity-owned `change_outbox` first, one worker
publishing from the table and marking a row `sent` only on the JetStream PubAck.
The fourth and last of the producers of §3.6; the shape is copied from
`services/productivity`'s (3a-2d), which was copied from `services/notification`'s
(3a-2c), which was copied from `services/camera`'s (3a-2b).

This unit also changes a shared contract: `IdentityChangeSink`'s
`publishCatalog` and `emitModule` stop being `void` and become
`[[nodiscard]] drogon::Task<void>`.

The plan row this closes:

```
| 3a-2e | `packages/identity` | 17 | plus (B) for the six journal writes |
```

## Pre-state, measured

| Site | At `HEAD` |
|---|---|
| `packages/identity/database/schema.sql` | 13 identity tables (`user`, `person`, `face_embedding`, `person_tag`, `person_snapshot`, `refresh_token`, `device_login_challenge`, `device_credential`, `user_invitation`, `invitation_redemption`, `stored_file`, `user_portrait`, `portrait_preview_capability`) and 16 `idx_*` indexes; no `change_outbox`. 219 lines, header prose names "The 13 identity tables" and the four sync-owned tables this file carries until Phase 3c-2 |
| `NatsIdentityChangeSink` (`src/feature/api/user/services/nats-identity-change-sink.{hxx,cc}`) | one class, five overrides, every one ending in `bus_->publish(...)` — core NATS, the result feeding a `LOG_WARN` at most. Two subjects: `kIdentityChange` = `argus.identity.v1.change` (catalog, emit, both audits) and `kIdentityUserAction` = `argus.identity.v1.user-action` (the journal) |
| `services/gateway/src/main.cc:326-327` | `static const NatsIdentityChangeSink identitySink(natsBus); identity_change::setSink(&identitySink);` — installed inside the `else` of the `nats.url` check, so only when a URL is configured (the `connect()` result at `:319` is not consulted); a missing sink drops every change with a `LOG_WARN` |
| `NatsBus` stream over the two identity subjects | none — no caller ensures one, and `argus-deploy` mounts no stream config for any subject |
| `packages/contracts/sync/src/sync/identity-change-sink.hxx:30,32` | `virtual void publishCatalog(…) const = 0;` / `virtual void emitModule(…) const = 0;` |
| Call sites of the funnel | **17**, exactly: `emitModule` 4 (`identity-rpc.cc:152`, `:568`, `invitation-feature-service.cc:148`, `auth-service.cc:294`), `publishCatalog` 2 (`user-feature-service.cc:87`, `auth-service.cc:270`), `publishModuleAudit` 3 (`identity-rpc.cc:910`, `invitation-feature-service.cc:123`, `auth-service.cc:301`), `publishUsersAudit` 2 (`user-feature-service.cc:77`, `auth-service.cc:569`), `publishAction` 6 (`portrait-preview-service.cc:138`, `user-feature-service.cc:152`, `invitation-feature-service.cc:155`, `auth-service.cc:315`, `:540`, `:649`) |
| Implementers of `IdentityChangeSink` in the tree | exactly one — this package's sink. No test double implements it (grep over `packages` and `services`), so the contract change has no fake to update |
| Consumers of the two identity subjects | `packages/memory`'s catalog replica (`catalog-replica.cc:93`, `kIdentityChange`) and `services/sync`'s fan-out (`sync-fan-out.cc:155`, `kIdentityUserAction`) — **both over core NATS**, so the stream this unit creates exists for the PubAck, not for a consumer |
| Suites under `packages/identity/tests/unit/` | 2: `identity-migration-test.cc` (254 lines) and `device-credential-test.cc` (519 lines). `identity-migration-test.cc:124-129` counts indexes with an unfiltered `name LIKE 'idx_%'` and asserts `== 16` |
| `packages/identity/AGENTS.md`, `CONTEXT.md` | describe the sink and the two test suites; neither mentions an outbox |

## The design

**The module is copied, not packaged** — `packages/identity/src/shared/
repositories/change-outbox/` holds its own `argus_module(NAME
identity-change-outbox …)`, as §3.6 requires and as camera, notification and
productivity already do — all three landed theirs at that exact path, so the
fourth matches it name for name. Rule 23's 2+ test is not what puts it there:
its reader is the `user` feature's sink alone, and the placement follows the
established shape rather than a second reader. That is recorded rather than
argued, because a fifth producer copying this path is the point.

**The copy is a variant, and the report says so.** This is the first producer
whose feed carries **two subjects**, and the first whose journal leg has no row
to key on. Three things therefore differ from the three copies, each for a
measured reason:

- **A `subject` column.** The three earlier copies publish from the table to one
  subject, so the subject is a constant. Identity's drain publishes rows that
  belong to `argus.identity.v1.change` and rows that belong to
  `argus.identity.v1.user-action`, and the row has to say which. Inferring it
  from the payload — "has `kind`, or has `operation`, so it is a change" — is
  exactly the fragility rule 25's one-declaration-per-subject rule exists to
  avoid; the row states its subject and the drain reads it.
- **Settlement keys on the row's own `id`, not on `event_id`.** Reading (B) for
  the journal is the reason: a journal row's identity is its own position in the
  outbox, not a hash of content a `read` never changed. That makes `event_id`
  nullable, and a status-guarded CAS over a nullable column is not a CAS — so
  `markSent`/`recordAttempt` take the row id and the guard moves to
  `WHERE id = ? AND status = ?`. The guard, not the key, is what the CAS is.
- **Two insert paths.** `enqueue()` writes the (A) legs with a content-derived
  `event_id`; `enqueueAction()` omits the column entirely, so the row's
  `event_id` stays NULL without binding a NULL parameter. `INSERT OR IGNORE`
  plus the `UNIQUE` index still deduplicates the (A) legs, and SQLite treats
  NULLs as distinct under a UNIQUE index, so the journal keeps every row.

Everything else — the key prefix `"identity-change:"`, the dispositions, the
`fingerprintJson` dedup, `kMaxPayloadBytes`, `kEnqueueAttempts`, the status
values, the 64-row pending batch, the 50 ms progress cadence — is identical to
the three copies on purpose: an operator should find one table with one
behaviour under one name in every producer's database.

**Reading (B), applied to the six journal writes.** A `UserActionEvent` records
that an actor did a thing to a record; a portrait view changes no row, so two
views of two portraits are two audit rows with identical content and (A) would
collapse them into one — a lost audit row. The journal row's identity is
therefore the outbox row's own id, carried into the JetStream `MsgId` as
`"identity-action:" + to_string(id)`: the `notification-delivery:<deliveryId>`
shape the delivery inbox already uses for the same reason. Its fingerprint is
still the canonical payload hash, so the two writers racing over one prior state
is still visible; what is structural is the conflict case — a journal insert can
never conflict, because a NULL `event_id` never collides.

**Both legs' payloads are published byte for byte as built.** The catalog leg
writes `{kind: "identity", table, id, deleted, row}`; the emit leg writes
`sync_change::emitPayload(frame)` (the `SocketEmitDto` triple, no `kind`); the
two audit legs write `ModuleAuditEvent::toJson()` / `UserAuditEvent::toJson()`
(`kind: "audit"`); the journal writes `UserActionEvent::toJson()`. The dedup and
the non-positive-id drop the two audit legs already had are kept — they are the
set the legacy `publishUsers` kept, and removing them would be a behaviour change
smuggled into a durability change. The emit leg keeps the no-id drop it needs:
its event id comes from `body.obj["id"]`, and an emit that carries no id, or one
that is not integral, has nothing to be keyed by and is logged and dropped rather
than recorded under a wrong name.

**The change legs own their own stream.** `publishWithMsgId` is JetStream-only
and never degrades to core NATS (`nats-bus.hxx:54-57`), and nothing else in the
tree ensures a stream over either subject, so the sink ensures
`ARGUS_IDENTITY_CHANGE` itself — one stream over **both** subjects (7 days, file
storage, 2-minute duplicate window), self-healing: a failed ensure is retried on
the next tick rather than latching, and a publish the broker refuses clears the
latch. The name lives in the sink's own `Config`, not in `lib/nats` — nothing
outside this package names it.

**The drain is the improved shape 3a-2c landed**: `pendingBatch(kDrainBatch=64)`
in one statement, `flush(row)` per row, wait 50 ms while progressing and
`config_.retryMs` (default 500 ms) otherwise, a refused publish stops the pass at
the oldest pending row, and a row the broker stored but the database could not
mark `sent` stays at the head instead of being republished at the progress
cadence. Ordering is `ORDER BY id ASC` rather than the copies'
`created_at ASC, rowid ASC`: with `id INTEGER PRIMARY KEY AUTOINCREMENT` the two
are the same order, and `created_at` has one-second resolution where the id does
not.

**The boot wiring is productivity's.** `packages/identity` is a package compiled
into `argus-gateway`, so the wiring lives in the gateway's `main.cc`: the sink
becomes a `std::shared_ptr<NatsIdentityChangeSink>` (from a function-local
`static const` object) held in a local of `main`, installed whenever `nats.url`
is configured whether or not the first `connect()` succeeded — the outbox is
exactly what makes a broker that is down survivable, and a sink that is never
installed drops every change with a `LOG_WARN`. `reconcile()` runs from the
gateway's beginning advice, after the identity schema has been applied, so the
drain thread never outlives the database it reads. The bus is kept on a failed
connect for the same reason (it reconnects in the background).

## The contract change, and its real blast radius

Routing an emit through the outbox makes it awaitable, and rule 21 keeps blocking
IO off the event loop, so the two `void` legs cannot stay `void`. They become
`[[nodiscard]] drogon::Task<void>`, exactly like the three legs that were already
coroutines.

3a-2d's review established the rule this unit has to apply rather than
rediscover: **a `Task`-returning emit changes every caller that does not await
it, so it is never a one-file edit.** The 6 sites are:

- `identity-rpc.cc:152`, `identity-rpc.cc:568`, `auth-service.cc:294`,
  `auth-service.cc:270` — already inside coroutines (`AuthService::registerUser`
  spans `:130-331` and holds both of its sites), so each needs a `co_await` and
  nothing else. `user-feature-service.cc:87` is the same case for the other
  promoted method: its `update` is already a coroutine.
- `invitation-feature-service.cc:141` — `emitInvitation` is a plain `void`
  function, so it must **become** a coroutine, its declaration
  (`invitation-feature-service.hxx:33`) changes with it, and its one caller
  (`invitation-feature-service.cc:87`, inside the `create` coroutine) gains a
  `co_await`.

`UserFeatureService::emitAuthContextChanged` is the near miss worth recording:
it is also a `void` helper over a sink, but over a *different* one —
`sync_control::getSink()->emitToUser(...)`, which this unit does not touch — so
it stays a plain `void` function.

`[[nodiscard]]` is on the two pure virtuals and on all five overrides, so every
one of the funnel's 17 call sites must be awaited. None of them has a result to
discard today (the old `void` return simply had nothing to discard), so nothing
is papered over with a `static_cast<void>`.

This is the second shared-contract change of step 3a that touches a header
another service compiles — and the first whose implementer set is a single class
in a package rather than in a service. No test double implements
`IdentityChangeSink`, verified by grep rather than assumed; the full-tree build
is this unit's gate anyway, because the header is compiled by `argus-gateway`
and by `argus-sync` alongside the package.

## The schema test the new index breaks

`packages/identity/tests/unit/identity-migration-test.cc:124-129` counts indexes
with an unfiltered `name LIKE 'idx_%'` and asserts `CHECK(indexes.size() == 16)`,
so `idx_change_outbox_status` would make it 17. This is the same break 3a-2d hit
in productivity's suite, and the same reasoning applies: the assertion is about
**the identity domain's** indexes, and the outbox's index is not one of them. The
identity indexes share no single prefix (`idx_user_*`, `idx_person_*`,
`idx_face_embedding_*`, `idx_refresh_token_*`, `idx_user_invitation_*`,
`idx_invitation_redemption_*`, `idx_stored_file_*`, `idx_user_portrait_*`,
`idx_portrait_preview_capability_*`), so the filter is narrowed by exclusion
(`AND name NOT LIKE 'idx_change_outbox%'`) rather than by a nine-way prefix list.

The table count at `:117-122` is a `name IN (…)` list of the 9 identity tables,
so adding `change_outbox` does not touch it, and the `moved.empty()` assertion
at `:131-135` names sync's four tables, which this unit does not add.

`tools/migrate-identity` is unaffected: its `kIdentityTables`
(`identity-migration.cc:18`) is the 9-table list the tool moves out of
`argus.db`, `report.tables.size() == 9` reads that list and not
`sqlite_master`, and the target's per-table checksums cover only those 9. The
outbox is created by the same `schema.sql` every other table is — the three
earlier producers left theirs the same way.

The schema file's own header prose names "The 13 identity tables"; it gains the
outbox as this package's own addition, the way productivity's did.

## Evidence

_Filled after the runs; no predicted numbers._

| Command | Result |
|---|---|
| `./scripts/build-all.sh dev --only identity` | `0` — 25/25 tests, 0 errors, 0 warnings (the log's only two `Warning:` lines are CMake configure notices from vendored ncnn, present in every run) |
| `ARGUS_NATS_URL=nats://127.0.0.1:4222 ./identity-change-outbox-sink-test` | `0` — 1/1 cases, 108/108 assertions, and 10 more consecutive runs identical. Without a broker the suite reports 90/90: the live block is skipped |
| `./scripts/build-all.sh dev` (full, 17 projects) | `0` — 400 tests, 0 failed, 0 errors, 0 warnings; `check-deps` 64 declarations / 478 edges / 0 forbidden / 0 cycles / 0 unresolved / 49 deferred; `check-tidy` 493 TUs, 3139 findings over 45 checks, baseline 3141, 2 checks below it |
| `scripts/check-deps.sh` | `64 declarations, 478 edges, 0 forbidden, 0 cycles, 0 unresolved, 49 edges deferred to phase 3 (221 third-party mentions over 22 roots)` |
| `scripts/check-tidy.sh` (re-run on the frozen tree, after the review's edits) | `0` — 493 TUs, 3139 findings, baseline 3141, 2 checks below it |

The tidy ratchet bit once, exactly as it did at 3a-2d: the first full run after
the unit landed took `modernize-avoid-c-style-cast` from 362 to **363** and the
gate exited 1. The scan's own per-TU invocation named the one line —
`identity-change-outbox-sink-test.cc:162`, `Json::Int64(portraitUserId)`, a
functional-style cast the check flags — and `static_cast<Json::Int64>(…)` is
what the check accepts. The three new TUs also took the scan from 490 to 493,
which is why the fix had to be a real one rather than a baseline bump: the
baseline records `tus` and a shrinking scan fails the gate too.

## The review, and what it changed

The adversarial pass over the landed unit produced three confirmed findings
(and refuted eight). All three are applied, not carried:

- **The live block raced its own drain** (`identity-change-outbox-sink-test.cc`).
  `reconcile()` started the flush worker *before* the catalog row was published,
  so the head-row read could find the row already settled and `pendingRow`'s
  `REQUIRE(!rows.empty())` aborted the case. The three released siblings all
  order it publish → read → reconcile (productivity `:333-337`, notification
  `:268-271`; camera reads `expected` off the input DTO instead), and identity
  had inverted it while keeping the siblings' comment. Reproduced rather than
  reasoned about: restoring the inverted order plus a 300 ms deschedule after
  the publish gives `REQUIRE( false )` at `:82`, 95 assertions, 94 passed,
  `Status: FAILURE` — deterministically, on this tree. Reordered to the sibling
  shape; three runs plus the ten above are 108/108.
- **The live sink was not scoped.** Its worker ran until the end of the live
  block, so the `stranded` block's `CHECK(attempted)` was co-served by a second
  worker publishing the wildcard row (removing `stranded.reconcile()` still
  passed the check), and the `bursts` block's "under 3 s" no longer isolated the
  one-pass drain it names. All three siblings brace their live sink for exactly
  this reason. Braced, with the sibling's comment; both blocks now measure what
  they claim.
- **Two architecture docs were left behind.** `docs/architecture/
  data-storage.md`'s durable-delivery inventory listed three `change_outbox`
  producers and omitted identity's, although 3a-2b/3a-2c/3a-2d each added their
  own bullet in their own commit; it gains the fourth, carrying the two facts
  that make this copy a variant (the `subject` column and the id-keyed
  settlement of the journal leg). `docs/architecture/wire-nats-subjects.md`'s
  `argus.identity.v1.change` payload section still promised "publication
  failure logs a WARN and is dropped (best-effort feed)" — true before this
  unit and false after it, since a refused publish now leaves its row in the
  outbox and retries. Both are corrections of statements this unit made stale,
  the same class of correction 3a-2c recorded as C9.

The subject **table** at `wire-nats-subjects.md:34-35` is deliberately
unchanged, and the review confirmed that is right rather than an omission: those
rows state each subject's publisher, consumer and purpose, none of which moved,
and the plan's 3a-2 row requires existing rows and payloads to stay untouched.

## What this unit deliberately does not do

- **No dedup on the emit leg**, as the three copies: the fan-out dedups
  recipients itself and the emit leg is the client's own row.
- **No `change_outbox` in the migration tool**, as above.
- **No new stream in the deploy stack.** The sink ensures its own, self-healing,
  and `argus-deploy` mounts no stream config for any subject.
- **Nothing in `services/sync`.** Both consuming subscriptions are core NATS; a
  durable consumer for the identity subjects is S4 at step level, a decision
  about all four producers at once.
- **No batched settlement in the drain**, the same deliberate non-goal 3a-2d
  recorded: `markSent` is a status-guarded CAS over a set of ids while
  `recordAttempt` carries a per-row count, so a batch would either drop the
  attempt counter or need a second per-row statement anyway.

## The items this unit leaves at step level

- **S1 (step-wide): the enqueue is not in the domain write's transaction.** The
  row mutation commits, then the change row is written; a crash in between loses
  that one change. Unchanged from 3a-2b/3a-2c/3a-2d, and this unit completes the
  measurement for all four producers.
- **S1b (step-wide): the enqueue's own give-up loses the change**, with the same
  bounded retry (`kEnqueueAttempts = 3`, 25 ms apart) and the same reachability:
  `SQLITE_FULL`/`SQLITE_IOERR`/a corrupt page fail all three times and the row is
  absent from `change_outbox` for ever, while a `SQLITE_BUSY` past the 5000 ms
  busy timeout needs sustained contention for ~15 s.
- **S4/S5/S8 (step-wide), as 3a-2c recorded and 3a-2d sharpened them:** no
  durable consumer for any change subject; no purge of `sent` outbox rows; the
  drain worker is joined only by the sink's destructor, which runs after
  `drogon::app().run()` has returned, while `quit()` resets the database client
  manager before the IO loops stop. Identity's sink carries the same exposure —
  and one more surface of it, because the gateway holds **two** database clients
  (`default` on `identity.db`, `gateway` on the fallback record) and the reset
  reaches both.
- **S7 (step-wide): rule 20's statement-level comments in test suites.**
- **The `[[nodiscard]]` contract change is a wire-adjacent change**: it alters no
  payload, no subject and no operation, but it changes a header two services
  compile.
- **One identity-specific observation for step 3:** `identity.db` is the one
  database two owners write — this package's tables and `services/sync`'s four
  until Phase 3c-2 splits them. A `change_outbox` in it is identity's, and
  sync's own outbox (3a-2f, only if measured) must not take the same name in the
  same file. Recorded here because the collision would be silent under
  `CREATE TABLE IF NOT EXISTS`.
