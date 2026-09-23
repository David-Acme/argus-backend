# argus-notification — CONTEXT

## Why the notification service exists

Fase 3 of the `migracion-microservicios` plan splits the notification domain
out of the monolith. This task (F3-1) creates the substrate WITHOUT cutover —
the legacy keeps owning every notification write and the gateway keeps its
read side unchanged. `argus-notification` mirrors the proven argus-camera
shape (F2-1) and the argus-productivity shape built in the same round: own
binary, own CMake preset, own `notification.db`.

## What it owns (F3-1)

- **notification.db**: the `notification` and `notification_token` tables
  (Ruling AN — single-owner), DDL copied verbatim from
  `database/schema.sql:428-450`. The schema lands as
  `database/schema.sql` and is applied at boot through
  `DbService::runScriptFile` — abort on failure. `argus.db` is never
  touched. No indexes exist on these tables in the legacy schema, so the
  schema file carries none.
- **Foreign keys stay off** (schema file pragma + re-applied after
  `applyPragmas`, which would otherwise turn them on per connection): the
  tables reference `user(id)`, and the user rows live in identity.db, not
  here. The `REFERENCES user(id)` clauses are kept verbatim; enforcement is
  replaced by code — rows are always addressed through the JWT actor.
- **Write-side feature surface, registered in THIS binary only**: the
  notification and notification-token controllers, their feature services
  (markAsRead, registerToken) and DTOs compile from the shared tree into the
  `argus-notification` executable; the notification schema/repository, the
  delivery service and the notification-token repository/service compile into
  `notification-core`. The controllers are
  Drogon `AutoCreation` controllers: their routes register during static
  init, exactly like the legacy binary registers them, and they cannot be
  registered explicitly (Drogon static-asserts against it), so the
  executable-target compilation is what guarantees the routes exist. The
  legacy keeps its own registration until F3-2 — this task changes NO legacy
  build input and NO legacy behavior. Until the F3-2 cutover,
  `notification.db` is a migrate-tool copy, NOT the authoritative store: the
  gateway never routes here, so the registered routes are out-of-contract
  before the cutover.
- **Audit emission (F3-2 cutover, Rulings AQ/Y/AR)**: `markAsRead` no longer
  publishes through `SyncAuditService` — each change goes through the
  `user_change` sink, whose argus-notification binding produces the exact
  per-change rows the legacy `markAsRead` published (`userIds={userId}`,
  `changes` JSON via `JsonDiff::createFlatDiff`, TableName::Notification)
  and emits them over NATS (`argus.notification.v1.change`,
  `docs/architecture/wire-nats-subjects.md`). `argus-sync` persists them verbatim into
  identity.db `user_audit_log`; nothing audit-shaped is written to
  notification.db.
- **Serving live traffic (F3-2, Ruling AR)**: the gateway relays
  `/notification/read` (PATCH) and `/notification-token` (POST) to this
  service with identical paths; the envelope, statuses and validation
  (empty/unknown id handling) are byte-identical with the legacy — verified
  live. notification.db opens WAL with `busy_timeout` and only this service
  opens it (rule 27). The legacy keeps its own markAsRead/token routes
  registered but they are unreachable through the gateway (Ruling AS — quiet,
  not stripped).
- **RPC owner (rule 27)**: `feature/rpc/notification-rpc-service.cc` serves
  `argus.notification.v1.NotificationService` on `server.grpc_port` (7038):
  `CreateNotifications` fans one row per user id and reuses the shared
  create+emit path (the whole fan-out is one multi-row INSERT with
  `RETURNING id`, so the emit mirrors strictly persisted rows), and
  `PullNotifications` serves the user-scoped `/sync` page from the identity
  metadata. `argus-sync`'s `/sync` pulls and the gateway's camera-notifier are
  the only clients; no other service opens notification.db.
- **Identity validation (f7-3)**: the JWT filter validates the caller over
  `argus.identity.v1.ValidateToken` at `[identity] target` — the user row,
  the bound refresh-token session and the device binding are resolved by the
  identity service, which owns them. This service opens NO identity.db: the
  F3-2 read-only client install is gone, and with it the boot-order wait on
  a file another service creates (`nm -C` on the binary shows zero
  UserRepository / RefreshTokenRepository / DeviceCredentialRepository
  symbols). An unreachable identity service means 401, never an open door.
- **CORS**: the legacy answered every preflight in pre-routing and the
  gateway forwards OPTIONS on proxied paths untouched, so this surface keeps
  answering OPTIONS itself (`Cors::handleOptions`).
- **Foreign keys (Ruling AN)**: the notification tables reference user rows
  that live in identity.db, so foreign-key enforcement stays off on every
  connection.
- **Audit recipients**: `publishAudit` keeps the same recipient set the
  legacy `SyncAuditService::publishUsers` kept — non-positive ids out,
  duplicates collapsed.
- **`GET /health`**: standard `ApiResponse` envelope
  `{status: 200 (int), info: {service: argus-notification, uptimeSeconds},
  errors: null}`; never depends on any downstream service.
- **What stays away**: no read-path controller (notification list/read
  snapshots keep flowing through `argus-sync`'s `/sync` pulls over
  `argus.notification.v1`), no /sync socket, no AI symbols (verified
  with `nm -C`), no alarm-triggering code.

## Build wiring (decisions)

- The canonical build is the service's standalone graph. From the repository
  root use `scripts/build-all.sh dev --only notification`; direct builds
  rerun Conan before the matching preset and CTest.
- The standalone build compiles ncnn only because `argus_identity` compiles
  the face services, whose headers need it; nothing references those
  objects, so they drop at link time (zero AI symbols).

## Migration tool

`tools/migrate-notification` (`argus-migrate-notification`) copies
`notification` and `notification_token` from `argus.db` into
`notification.db` and then goes quiet: a schema-current `notification.db`
makes reruns a verified no-op, because after the F3-2 cutover
`notification.db` is live data and the frozen `argus.db` copy must never be
resurrected over it. Nothing is deleted from `argus.db`. Its
`foreign_key_check` ignores user references by design (the user parent rows
live in identity.db) and fails on any other violation.

## The folder owns its domain (f7-7c)

The notification feature tree, the notification-token repository, schema
and service, the notification schema file and the three unit suites moved
out of the shared `src/` tree into this folder, prefixes preserved. The
write-side source list is one `NOTIFICATION_FEATURE_SOURCES` variable
shared by the executable and the controller suite, replacing the two
hand-kept copies.

What did NOT move: the `notification` table's own repository, schema and
delivery service, which `notification-core` compiles here since sub-step
3a-1b. Since rule 27 this service is the only writer and reader of those rows:
the gateway's camera-notifier creates through `argus.notification.v1` and
`argus-sync`'s `/sync` page pulls through the same contract. Both sides of the table are
exclusively this service's, the notification-token side as before.

## Durable command inbox and delivery intents (F11 / R5)

- **`notification_command`** makes `CreateNotifications` idempotent: one row
  per `command_id` with the expected fan-out count and the SHA-256
  fingerprint of the normalized batch. A reused id with the same payload
  replays the persisted outcome; a reused id with a different payload raises
  `NotificationCommandConflict` (`ALREADY_EXISTS`). The whole fan-out commits
  in one `IMMEDIATE` transaction (multi-row `INSERT ... RETURNING id` plus
  multi-row delivery intent rows), so the emit mirrors strictly persisted
  rows.
- **`notification_delivery`** holds one intent row per recipient
  (`pending`/`sent`). `NotificationService` takes its sinks as injected
  `Dependencies` (`deliverySink`, `pushSink`, `pushRequired`) — no sink
  singletons, no Core-NATS fallback. `deliverPending` ensures the
  `ARGUS_NOTIFICATION` stream, then drains pending intents in 200-row pages:
  each publishes through `NatsNotificationDeliverySink` (JetStream
  `publishWithMsgId`, `Nats-Msg-Id = notification-delivery:<deliveryId>`,
  settle only on PubAck), fires the best-effort push intent, and marks the
  row sent. A refused publish leaves the intent `pending` for the 60-second
  delivery reconciler (`startDeliveryReconciler`); nothing is ever lost on a
  broker outage, and a restart replays from the table.
- **`markAsRead`** lands its per-change user audits in the service's own
  `change_outbox` before they are published over `argus.notification.v1.change`
  (see the change-feed section below), in the same transaction as the row
  updates it audits. The sink carries audit diffs only: a notification row
  reaches its user through the durable delivery leg, not through the change
  subject.
- **Push intents** (`[push].enabled`, default off) stay at-most-once
  fire-and-forget accelerators toward `argus-relay`; the `/sync` fan-out
  after durable delivery is the guarantee.

## The notification change feed (3a-2c)

- Every mark-as-read audit lands in `change_outbox` in notification.db before
  it is published: the row updates, the audit diff and the change row are
  statements of one `IMMEDIATE` transaction (S1), and a worker publishes from
  the table and marks a row `sent` only on the JetStream PubAck. A broker
  outage, a crash in between or a restart leaves the rows pending and they
  drain at the next boot; before this the audit was a fire-and-forget core
  publish that a broker outage dropped without a trace. A refused enqueue
  throws into that transaction instead of being retried and given up on (S1b):
  the whole read rolls back, the handler answers an error, and no state exists
  in which a committed mutation has no change row — the fail-fast storage
  errors (`SQLITE_FULL`, `SQLITE_IOERR`, a corrupt page) are the window the
  give-up used to leave open, and only the caller can answer them.
- **The event id names the transition, not the record** — the same rule as the
  camera feed. It is the hash of the table, the record id and the payload's
  own canonical JSON, so a redelivery recomputes the same id while a record
  that moves again, or returns to a state it already held, is its own event.
  Recipients are deduplicated and a non-positive user id is not a recipient,
  because the wire event is addressed to the users it names.
- **The change leg owns its own stream.** `publishWithMsgId` is a JetStream
  publish with no core-NATS fallback, and `ARGUS_NOTIFICATION` carries the
  delivery subject alone — `NatsBus::reconcileStream` refuses to repurpose a
  stream whose subject set differs — so the change subject is retained on
  `ARGUS_NOTIFICATION_CHANGE`, created self-healing by the drain: an ensure
  that fails is retried on the next tick instead of latching, and a publish
  the broker refuses clears the latch, because a stream that disappears under
  a running process looks exactly like that. The name lives in the sink's own
  `Config`, not in `lib/nats`: nothing outside this service names it.
- **One pass drains a batch.** A `PATCH /notification/read` is one burst of
  audits, so the drain reads up to 64 pending rows per pass, publishes them
  oldest-first and waits 50 ms while it is progressing, the retry cadence
  otherwise. A refused publish still stops the pass at the oldest pending row,
  so nothing behind it is overtaken; a row the broker stored but the service
  could not mark `sent` also stays at the head rather than being republished on
  every tick. The batch is one read and not one per row: settlement is per row
  by construction, the PubAck is the whole point, but the read is not.
- **The sink is installed whenever NATS is configured**, connected or not: a
  broker that is down at boot is the case the outbox exists for, so the audit
  must be recorded then too. The drain waits out the disconnected bus and
  ensures its stream on the first tick after the reconnect, which is why it
  does not ride the delivery reconciler's 60-second rhythm.
- **The drain is stopped before Drogon quits, not after.** The sink registers
  with `shutdown_signal` at boot, **before `drogon::app().run()`** — which is
  also before `reconcile()` starts its worker:
  SIGTERM/SIGINT only
  requests the stop, the loop keeps running while the worker leaves its pass,
  and `quit()` follows once the worker reports drained (a 10-second deadline
  bounds the wait). The worker is a thread of the sink's own, so Drogon does
  not stop it, and `quit()` destroys the database client manager the worker
  reaches through `DbService::client()`. The registration comes first because
  the hook's handlers are what `run()` installs Drogon's `sigaction` over, and
  because a drain registered after the stop was requested is only stopped at
  once, never waited for.

## Delivery proof (Round 11)

The chain used to end at gateway dispatch. Clients now confirm display
with `PATCH /notification/ack {"notification_ids": [...]}` (same role
shape as `/notification/read`); the confirmation lands on the delivery
row (`acked_at`, millisecond legs beside the legacy second stamps), only
for sent rows, idempotent. `GET /notification/delivery-summary` reports
pending/unacked/unacked-old (past `ack_window_s`, default 24h),
dispatch-to-settle latency percentiles and the synthetic probe. The probe
(`startSelfTestProber`, every `selftest_interval_s`, default 300s) pushes
one user-0 `probe` row through create, broker publish and settle and
records the outcome in `notification_selftest`; no device can see it, and
a failed probe is a warn plus a row, never silent. Probe rows older than
seven days are purged with their deliveries.

## No-NATS survival (Round 6, Gate A)

A deployment with no `[nats].url` starts, serves RPCs and leaves intents
pending — the missing delivery sink is a configuration state, never a fatal
error. Three layers hold that contract:

- `deliverPending()` returns `DeliverPendingOutcome` (`Settled`,
  `NoSinkInstalled`, `StreamUnavailable`) instead of throwing for a missing
  sink; every intent stays pending for a later reconciler or a configured
  restart.
- `startDeliveryReconciler()` refuses to schedule when no sink is installed
  and wraps the drain in `try/catch`, so a database failure degrades to a
  warn instead of feeding `AsyncTask::unhandled_exception` (LOG_FATAL +
  `std::terminate`).
- A missing push sink no longer aborts the drain: push is a best-effort
  accelerator, so the delivery settles with a warn while push stays silent.
  A sink that exists but cannot reach the broker still keeps intents pending
  (settle only on PubAck) — that guarantee is unchanged.

## Live tests (opt-in, never green by default)

`notification-delivery-live-test` skips silently without `ARGUS_NATS_URL`
(e.g. `nats://127.0.0.1:4222`); it runs the fan-out against an isolated
stream, subject, durable name and temporary database. A default run passing
means nothing about the wire — every live-test claim must state the variable
that was set. Never point it at deployment streams.
