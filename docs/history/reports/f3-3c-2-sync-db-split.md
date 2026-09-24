# Phase 3c step 2 — the five sync tables leave identity's file

`argus-sync` owns `database/sync.db`, and the five tables it is the only writer
of moved into it: `audit_log`, `user_audit_log`, `audit_compaction_state`,
`user_action_log` and `notification_delivery_inbox`. The rows came across with
`services/sync/tools/migrate-sync/` (`argus-migrate-sync`), the journal's
redelivery key was re-minted so it no longer borrows a row id from a table in
another owner's file, and the deploy gained the mirrored pair of profiles that
runs the copy forward and back.

## What existed before

Phase 3a-1c moved the four identity tables' DDL into
`services/sync/database/schema.sql` when the fan-out became their only writer,
but the file they were *applied to* stayed identity's: `[sync] db` pointed at
`database/identity.db`, and the deploy bound identity's data directory into the
argus-sync container. Two consequences were recorded as the transitory price of
moving the writer before splitting the file:

- The audit tables carried `REFERENCES user(id)` — a foreign key into a table
  this owner does not declare. With `foreign_keys = ON` such a reference cannot
  even be *prepared* against this file, so the clause could not have survived
  the split even if the cascade were wanted.
- One owner's database was mounted into another owner's container, which is
  exactly what rule 27 forbids.

The journal's redelivery key was `<prefix> + <change_outbox.id>` — a row id
from the producer's own table, stable because producer and journal happened to
share a file. Nothing had migrated the *rows*: the tables had been applied onto
identity's file and written there, so the split had no tool yet.

## The decision, as built

### The five tables, and the file they live in

`services/sync/database/schema.sql` stays the DDL's home (it already was) and
`[sync] db` now points at `database/sync.db`; the deploy mounts
`${ARGUS_DATA_DIR}/sync`. The audit tables lost their `REFERENCES user(id)`
clauses, and the trade is stated in three places rather than implied: a deleted
user leaves the audit rows that recorded them, which is what an audit trail is
for, and the alternative was a foreign key this service's own file cannot
declare. The five tables' six indexes came along unchanged.

`notification_delivery_inbox` made the trip too. It is this owner's table (the
durable receipts for the delivery stream it consumes), and leaving it behind
would have kept the last sync table in a foreign file — the same rule-27 shape
the split exists to end.

### The journal's redelivery key

The journal is append-only and non-convergent, so `user_action_log.msg_id`
holds the producer's `Nats-Msg-Id` under a partial unique index
(`WHERE msg_id <> ''`) and the insert is `INSERT OR IGNORE`: a redelivered
action is ignored instead of doubled. What changed is how the producer arrives
at that key.

- Both producers mint it at enqueue: 16 bytes from `RAND_bytes(16)`, written as
  32 lowercase hex, prefixed `identity-action:` (identity) or `auth-action:`
  (auth), stored in the `change_outbox` row's `event_id` — the column the
  change rows already used for their deterministic
  `identity-change:<sha256(table|recordId|discriminator)>` keys — and flushed
  as the published `msgId`. A redelivery republishes the same `msgId` because
  the outbox row is stable, and the index is what turns that into a no-op.
- Rows enqueued before the change have `event_id = ''` and flush under the
  row-derived `legacyActionMsgId(id)` their older builds published. The two
  shapes cannot collide — one is 32 hex characters, the other decimal digits —
  so the partial index accepts both and **no journal row is rewritten**.
- `change_outbox_key::actionMsgId` takes `std::array<unsigned char, 16>` rather
  than a span, so the entropy's shape is a type instead of a convention.

`services/auth`'s `change_outbox` had no `event_id` column at all when the new
producer began writing one, so it gained the column (`TEXT NOT NULL DEFAULT ''`),
the partial unique index, and a boot-time guard. The guard is load-bearing, not
defensive: `CREATE UNIQUE INDEX` over a column a legacy file lacks is refused,
`DbService::runScriptFile` returns false when any statement fails, and auth's
beginning advice `LOG_FATAL`s and `_exit(1)`s on false — without
`migrateLegacySchema()` running first, every upgraded deployment would refuse to
boot. The precedent is sync's own journal guard, byte for byte: the same
`COUNT_<TABLE>`/`COUNT_<COLUMN>` probe, the same
`ALTER TABLE ... ADD COLUMN ... NOT NULL DEFAULT ''`, the same partial index.

### The migration tool

`argus-migrate-sync` is a library plus a CLI over it (`sync-migration.hxx`'s
two entry points, `applySyncSchema` and `migrateSync`), and its shape is:

- **Plan.** The source is attached read-only (a percent-encoded `file:` URI;
  when that fails, a plain attach is attempted and reported as
  `sourceReadWrite`, and when both fail the message carries both errors). Each
  of the five tables is looked up in the source; a present table's columns are
  read from **both** sides and compared as a *set* — a difference refuses the
  run with both lists in the message. Nothing is copied from a table whose
  shape the two sides do not agree on.
- **Copy.** One `BEGIN IMMEDIATE`, then per table a `CREATE TEMP TABLE
  sync_copied_<T>` capturing the keys this run will copy and an
  `INSERT ... SELECT` naming the columns explicitly. The column list comes from
  **the target**, so a source whose columns sit in a different order — a file
  that reached `msg_id` through `ALTER TABLE ... ADD COLUMN` — migrates
  correctly instead of shifting every value after the new column.
- **Verify.** Both sides are checksummed (FNV-1a, each column framed with its
  own name before its type byte and value) through a join on the copied-key
  set, and row counts must agree. `skippedRows = sourceTotal − copiedRows`
  states what the key guard skipped instead of leaving it implied, and the CLI
  prints it per table.
- **Commit.** `COMMIT`, or `ROLLBACK` on any failure — the verification runs
  *inside* the transaction, so a refused migration leaves the target exactly as
  it was and the report names the table and both checksums.

Verification is scoped to the keys that run copied on purpose: a re-run is
honest against a live target that has since folded or pruned audit rows, since
it neither copies nor re-verifies rows it already moved. Same-file sources are
refused by canonical path *and* by `std::filesystem::equivalent`, so a hard
link cannot present itself as a different file. Every helper with three or more
parameters takes a struct (rule 2).

### The deploy and the rollback

`argus-sync`'s data mount moved to `${ARGUS_DATA_DIR}/sync`, and the stack
gained the two one-shot profiles that move the rows: `sync-init` (identity.db →
sync.db) and `sync-rollback` (sync.db → identity.db). They are separate
services because their mounts are opposites — `sync-init` mounts identity's
directory read-only and `sync/` writable, `sync-rollback` the other way round —
so the forward service *cannot* be run backwards: a swapped invocation there
could not create its target. Both run `network_mode: none` against a stopped
stack, and both are the one-shot exception rule 27 allows: one owner's data
directory visible to the other owner's migration tool.

One upgrade needs the profile: an install whose audit rows still live in
`identity.db` must run `--profile sync-init` before `up -d`. Without it the
service applies its schema to an empty `sync.db` and serves an empty audit
history, which every client can only answer with a full re-bootstrap. That is
documented where an operator meets it — the update flow in
`docs/operations/deployment-docker.md`, this service's `CONTEXT.md` — rather
than left to be discovered.

## The review

Two adversarial reviews ran over the unit (the migration tool and its suites;
the deployment and the documentation), and every finding below was reproduced
by me before it was acted on.

**The migration tool's six.**

- **D1 — the shape check was dead code, and the copy was positional.** The
  source's columns were read with `SELECT name FROM "src".pragma_table_info(t)`.
  SQLite ignores the schema qualifier on a table-valued pragma function in that
  position and answers with **main**'s columns, so the check compared the
  target against itself and passed every time — while `INSERT ... SELECT *`
  copied by position. A file that reached `msg_id` through `ALTER TABLE` would
  have migrated with every column after it shifted by one. Reproduced by hand
  before the fix; the correct form is `pragma_table_info(?, ?)` (table,
  schema — the schema is the *second* argument, bound), and the copy is now
  driven by the target's own column list.
- **D2 — verification ran after the commit.** A mismatch reported failure on a
  target that had already been written: the partial state the step exists to
  prevent. Fixed by restructuring into plan → begin → copy → verify → commit,
  and the public `verifyCopiedRows`/`SyncVerificationInput` seam was deleted
  rather than kept working.
- **H1 — checksums did not frame columns.** Per-column values were hashed in
  select order with no name, so two rows with permuted values could hash
  identically. Each column's name bytes are now folded in before its type and
  value.
- **H2 — a hard link defeated the same-file check.** Canonical paths of two
  names for one inode differ. `std::filesystem::equivalent` closes it when the
  target exists.
- **H3 — a collision-driven skip was invisible.** A run that skipped rows
  looked like a run that copied them. `skippedRows` is reported per table and
  printed.
- **H5 — the suites shared a temp directory.** `makeFixture()` created a fixed
  path and `remove_all`'d it, so two concurrent runs destroyed each other's
  databases. The directory carries the pid.

**One defect in the producer, found by the review and fixed.** `services/auth`'s
`change_outbox` had no `event_id` while the sink written in this unit reads and
writes exactly that column: with NATS configured, every login, logout and device
action would have failed to record its outbox row, which is the account-activity
audit trail. Fixed with the column, the partial index and the boot guard
described above.

**Two gate findings, both closed rather than absorbed.** The first full run
built and tested all eighteen projects green and then failed the ratchet with
`modernize-use-nodiscard` at 736 findings against a 729 baseline (537 TUs,
2952 findings, baseline 2945). Both migration tools hand-spelled their include
directories (`../../../../packages/lib/...`), so clang-tidy recorded headers
the tree already analyzes under a second path spelling and counted their
findings twice. Fixed the honest way: both tools link `argus::lib::sqlite` and
`argus::lib::text` by name, which is also rule 25 — consumers link by name and
include paths travel with the target — and normalizes the identity tool's
spelling, which the measured floor had carried since it was written. The second
run measured 538 TUs (the unit's four new ones) and 2941 findings against the
same floor with **one** check risen by one finding: the new auth suite's
`insertLegacyRow` returned `execSqlSync(...).insertId()`, whose `unsigned long
long` narrows into the `int64_t` its caller compares against. Closed the way
this tree's own repositories already write it — bind the result, then
`static_cast<int64_t>(result.insertId())` — rather than by re-recording the
floor, and auth was rebuilt green (27/27, 0 warnings) before the closing scan.

**Four deploy and documentation findings.** The report this file is (the plan
cited it before it existed); the rollback documented as "the same command with
the paths swapped" when the forward service mounts identity's directory
read-only, so the swap could not run — now the `sync-rollback` profile, with the
mount asymmetry stated; the init-profile inventory in
`docs/operations/deployment-docker.md` missing `sync-init` entirely; and the
rule-27 exception left implicit, now phrased as the deliberate one-shot it is in
`argus-deploy/AGENTS.md`, its `CONTEXT.md` and the deployment doc.

## What proves it

- `sync-migration-test` — 12 cases, 350 assertions: the target's schema shape
  (five tables, no foreign keys, six indexes); the copy with every table's rows
  counted, checksummed and `skippedRows = 0`; the ids preserved so the client's
  monotonic watermark continues; a second run copying nothing
  (`skippedRows == 2`, which is what makes the assertion non-vacuous) and a
  pruned row coming back; a shared key kept rather than replaced
  (`copiedRows == 1`, `skippedRows == 1`); a colliding `msg_id` rolling the
  whole copy back with all four other tables left empty; **a source whose
  journal columns are out of order** (`msg_id` last, as `ALTER TABLE` produces)
  migrating with `msg_id` and `created_at` landing in the right columns — the
  case that fails against the pre-fix implementation; **a source with a
  different column set** refused by name with the target untouched; a partial
  target schema refused; a source without the tables reporting five absences
  and copying nothing; a source that is the target refused, including through a
  hard link; and a missing source refused.
- `change-outbox-test` (auth, new) — 2 cases, 32 assertions: the column's exact
  shape (`TEXT NOT NULL DEFAULT ''`), both indexes present, a minted id's round
  trip, a duplicate minted id refused, and the boot path against a legacy file:
  `runScriptFile` returns false before the guard, the guard adds the column
  idempotently, the schema then applies, the index exists, the legacy row
  survives with an empty `eventId` and flushes under the fallback shape.
- `identity-change-outbox-sink-test` — extended to pin the key end to end: the
  published `msgId` equals the outbox row's stored `eventId`, both journal rows
  carry minted `identity-action:` ids, and the two differ from each other.

## Verification

- `./scripts/build-all.sh dev --only sync`, `--only auth`, `--only identity`:
  0 errors, 0 warnings in first-party code; 45/45, 27/27 and 32/32 tests.
- The full orchestrator: 18/18 projects, 445 test executions, 0 failures and
  not one compiler warning in the whole run,
  `check-comments: 1336 files checked, 0 comments`,
  `check-deps: 78 declarations, 666 edges, 0 forbidden, 0 cycles, 0 unresolved,
  23 edges deferred to phase 3 (267 third-party mentions over 22 roots)`.
- `check-tidy` on the final tree: **538 TUs, 2940 findings over 45 checks**
  against the 2945 floor, nothing risen and no unread translation unit — exit
  0. The floor was then re-recorded at that measurement, which is how the
  unit's four new translation units and the one closed finding enter the
  ratchet.

## Flagged, not fixed

- **A copied id below the target's `max(id)`.** With the copy in one
  transaction against a stopped service the case is unreachable, and the
  verification set makes a re-run blind to it by design; recorded rather than
  coded around.
- **`ARGUS_SYNC_SCHEMA_PATH` resolves only on the build host.** The CMake
  definition bakes the *build* tree's absolute source path and the header's
  fallback — what the suites that define nothing get — is
  `services/sync/database/schema.sql`, resolved against the working directory.
  Neither reaches a runtime tree, so in a container `--schema` is effectively
  required, which the compose always passes. The five sibling migration tools
  carry the identical pair; changing it is a unit of its own.
- **The mailbox keys on another owner's row id.**
  `notification_delivery_inbox.delivery_id` is notification's
  `notification_delivery.id`, and the broker's dedup id is
  `notification-delivery:<deliveryId>`. Should a delivery row be re-created in
  notification's file (a restored backup, or a reset whose ids restart) the same
  `delivery_id` can arrive with a different fingerprint, which the mailbox
  answers as a `conflict` that is never dispatched. The id belongs to the
  notification contract, so changing it is that contract's change, not this
  split's.

## Files

Schema and configuration: `services/sync/database/schema.sql`,
`services/sync/config.toml.example`, `services/sync/src/config/sync-config.cc`,
`services/sync/Dockerfile`, `services/sync/CMakeLists.txt`,
`argus-deploy/config.sync.toml.example`, `argus-deploy/docker-compose.yml`.

The tool and its suites: `services/sync/tools/migrate-sync/` (new — the
library, its CLI, its CMakeLists), `services/sync/tests/unit/sync-migration-test.cc`
(new), `services/sync/tests/CMakeLists.txt`,
`services/identity/tools/migrate-identity/CMakeLists.txt`.

The key: `services/auth/database/schema.sql`,
`services/auth/src/feature/session/repositories/change-outbox/` (four files),
`services/auth/src/feature/session/services/auth-action-sink.cc`,
`services/auth/src/app/main.cc`, `services/auth/tests/unit/change-outbox-test.cc`
(new), `services/auth/tests/CMakeLists.txt`,
`services/identity/src/shared/repositories/change-outbox/` (three files),
`services/identity/src/feature/user/services/nats-identity-change-sink.cc`,
`services/identity/tests/unit/identity-change-outbox-test.cc`,
`services/identity/tests/unit/identity-change-outbox-sink-test.cc`.

Docs: `docs/architecture/data-storage.md`,
`docs/architecture/services-and-packages.md`,
`docs/architecture/wire-nats-subjects.md`, `docs/operations/configuration-keys.md`,
`docs/operations/deployment-docker.md`, `docs/history/plans/architecture-plan.md`,
`services/sync/AGENTS.md`, `services/sync/CONTEXT.md`,
`services/identity/AGENTS.md`, `services/identity/CONTEXT.md`,
`services/notification/CONTEXT.md`, `argus-deploy/AGENTS.md`,
`argus-deploy/CONTEXT.md`, `scripts/build-all.sh`, `scripts/provision-host.sh`,
`scripts/lib/tidy-baseline.txt` (the floor re-recorded on the verified tree),
`AGENTS.md`.
