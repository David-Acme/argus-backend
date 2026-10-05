# argus-outbox

The transactional change outbox every owner that publishes a change feed
uses: a row written to `change_outbox` in the same SQLite transaction as the
domain change, then relayed to NATS JetStream with a `Nats-Msg-Id` so the
stream drops a redelivery. Tier 1 (`argus_lib_outbox`, `argus::lib::outbox`):
it links `lib/errors`, `lib/nats`, `lib/runtime`, `lib/sqlite`, `lib/text`,
Drogon and OpenSSL's `RAND_bytes`, and nothing from `contracts/`,
`clients/` or a service. What a service owns — the payloads, the msg-id
prefixes, the subjects, the stream definition, the retention numbers (from
`contracts/sync`'s `stream-retention.hxx`) and the refusal it throws (its own
catalog's `ChangeNotRecorded`) — reaches the library through `OutboxConfig`.

It replaced five copies (finding #69 of
`docs/history/reports/cloud-audit-2026-10-05.md`) that had already drifted
apart; the union of their behaviour is what lives here.

## Layout

- `src/outbox/transactional-outbox.{hxx,cc}` — `TransactionalOutbox`: the
  writer and the relay. `record` writes a row whose msg id the caller derives
  from the transition (a replay is a no-op, a different payload under the same
  id is a logged `Conflict` that is not dispatched); `append` mints a
  128-bit random id under a prefix for journal rows that must never dedup, and
  refuses (`503` + the configured refusal) when the entropy source fails.
  Both refuse a payload past `kMaxPayloadBytes` (256 KiB, the broker's
  message budget) with the configured refusal before touching the table.
  `reconcile` starts the relay, `requestStop`/`drained` make it a
  `shutdown_signal` drain (`drainOf(sink, name)` on the service's sink, which
  forwards them).
- `src/outbox/outbox-repository.{hxx,cc}` — `OutboxRepository`, parameterised
  by a client accessor (`DbService::cameraClient`, `identityClient`, …) so the
  writer and the relay always read the same database. `migrateSchema` is the
  additive boot migration: it adds `event_id` (auth's pre-event-id journal) and
  `subject` (the keyed tables of camera, notification and productivity) when
  they are missing, and does nothing when the table does not exist.
- `src/outbox/outbox-query.hxx` — the SQL and the parameter structs.
- `src/outbox/outbox-key.{hxx,cc}` — `transitionId` (prefix + 32 hex of
  SHA-256 over `table|recordId|discriminator`), `uniqueId`, `drawEntropy`,
  `legacyId` (prefix + row id, the msg id of a row written before `event_id`
  existed) and `fingerprint`.
- `src/outbox/outbox-status.hxx` — `OutboxStatus` and its string pair (the
  `CHECK (status IN ('pending', 'sent'))` column).
- `src/outbox/outbox-backoff.hxx` — `OutboxTiming` and `retryDelay`.

## One table shape over two layouts

The copies kept two layouts of `change_outbox`: camera, notification and
productivity key it by `event_id TEXT PRIMARY KEY`; auth and identity number
it with `id INTEGER PRIMARY KEY AUTOINCREMENT` and carry a per-row `subject`.
The library reads both through `rowid` (the alias of `id` in the second
layout, the implicit row id in the first), so the only on-disk change is the
additive `subject TEXT NOT NULL DEFAULT ''` on the keyed tables. A row with an
empty subject is published on `OutboxConfig::defaultSubject`; a row with an
empty or NULL `event_id` is published under `legacyId(legacyIdPrefix, rowid)`.
Pending rows leave in `rowid` order, which is insertion order in both
layouts, and the relay never skips a stuck row: every later change waits
behind it.

## The relay

One `std::thread` per outbox, started by `reconcile`, never a lane job: the
loop lives as long as the process, and parking it on a bounded
`BlockingTask` lane would take a slot for good (rule 13c). The writer side
runs on the event loop through `execSqlCoro`; the relay's synchronous SQL
and the NATS publish run only on that thread (rule 21). It sleeps on a
`WakeSignal` that three things ring: an insert outside a transaction, every
committed transaction (`db_transaction::CommitObserver`), and `requestStop`.
It drains at most `timing.batch` rows per pass, waits `progressMs` after a
pass that published something, `retryMs` when idle, and backs off
exponentially (`retryMs`, ×2 per consecutive failed pass, capped at
`maxRetryMs`) while the broker is down or refuses the head row. Sent rows
older than `retention.keepSentMs` are purged every `retention.purgeEveryMs`
(`purgeRetryMs` after a failed purge). On stop the loop exits after the row in
flight; undelivered rows stay pending in the table for the next boot.

## Tests

`outbox-key-test` (ids, fingerprint, status pair, backoff),
`outbox-repository-test` (both layouts, the two migrations, NULL event ids,
replay/conflict, insertion order, attempts, purge, a missing client) and
`transactional-outbox-test` (record, append, the payload budget, a row
inside a rolled back and a committed transaction, the drain, and — when
`ARGUS_NATS_URL` names a broker — a legacy row published on the default
subject under its legacy id). They register in the CTest graph of every
service that adds this package.
