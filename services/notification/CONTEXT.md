# argus-notification — CONTEXT

## Why the notification service exists

Fase 3 of the `migracion-microservicios` plan splits the notification domain
out of the monolith. This task (F3-1) creates the substrate WITHOUT cutover —
the legacy kept owning every notification write and the gateway kept its
read side unchanged. `argus-notification` mirrors the proven argus-camera
shape (F2-1) and the argus-productivity shape built in the same round: own
binary, own CMake preset, own `notification.db`.

## What it owns (F3-1)

- **notification.db**: the `notification` and `notification_token` tables
  (Ruling AN — single-owner), plus `notification_command` (the create
  idempotency row), `notification_delivery` (one intent per recipient),
  `notification_selftest`, `change_outbox` and `camera_fallback_event` since
  Phase 3d step 1. The schema lands as `database/schema.sql` and is applied at
  boot through `DbService::runScriptFile` — abort on failure. `argus.db` is
  never touched. The two legacy tables carry their own indexes —
  `idx_notification_user_created` and the token pair's unique index
  `idx_notification_token_uniq` — and `camera_fallback_event` carries one on
  `created_at` for its retention sweep and the per-outage queries.
  `idx_notification_token_user` was dropped (2026-10): the unique
  `(user_id, device_hash)` index already leads with `user_id`.
- **Foreign keys are on; no table names another service's table**
  (2026-10, audit #65). The schema used to declare `REFERENCES user(id)` on
  `notification` and `notification_token`, a table that lives in identity.db
  (rule 27), and to make that DDL loadable the whole database ran with
  `foreign_keys = OFF` — so the cascades it promised never happened and the
  one real internal key (`notification_delivery → notification`) was not
  enforced either. `database/schema.sql` now declares no reference to `user`
  and turns `foreign_keys` on, like `DbService::applyPragmas`. Rows are still
  addressed through the JWT actor or the caller's metadata.
  **An existing notification.db keeps its old DDL**: `CREATE TABLE IF NOT
  EXISTS` does not rewrite a table and this service never migrates a user's
  database silently (root rule 17). At boot `main.cc` asks
  `pragma_foreign_key_list` whether any table still references `user`; if one
  does it turns foreign keys back off for that connection and logs a warning,
  because with enforcement on SQLite refuses every insert into a table whose
  parent table does not exist. Such a database needs an explicit rebuild:
  either a development reset (delete notification.db, the service recreates
  it from `database/schema.sql`) or the SQLite table-rebuild procedure for
  `notification` and `notification_token` (create the new table from the
  schema under a temporary name, `INSERT ... SELECT` the rows, drop the old
  table, rename, recreate the indexes) run by hand with the service stopped.
  The same applies to `notification_token.token UNIQUE` below. Identity
  deactivates users instead of deleting them; the identity and session feeds
  this service consumes since 2026-10-05 delete push tokens (see "Push
  tokens").
- **Write-side feature surface, registered in THIS binary only**: the
  notification and notification-token controllers, their feature services
  (markAsRead, registerToken) and DTOs compile from the shared tree into the
  `argus-notification` executable; the notification schema/repository, the
  delivery service and the notification-token repository/service compile into
  `notification-core` (`argus::notification-shared` since Phase 4 step 9).
  The controllers are
  Drogon `AutoCreation` controllers: their routes register during static
  init, exactly like the legacy binary registers them, and they cannot be
  registered explicitly (Drogon static-asserts against it), so the
  executable-target compilation is what guarantees the routes exist. The
  legacy keeps its own registration until F3-2 — this task changes NO legacy
  build input and NO legacy behavior. Until the F3-2 cutover,
  `notification.db` is a migrate-tool copy, NOT the authoritative store: the
  gateway never routed here, so the registered routes are out-of-contract
  before the cutover.
- **Audit emission (F3-2 cutover, Rulings AQ/Y/AR)**: `markAsRead` no longer
  publishes through `SyncAuditService` — each change goes through the
  `user_change` sink, whose argus-notification binding produces the exact
  per-change rows the legacy `markAsRead` published (`userIds={userId}`,
  `changes` JSON via `JsonDiff::createFlatDiff`, TableName::Notification)
  and emits them over NATS (`argus.notification.v1.change`,
  `docs/architecture/wire-nats-subjects.md`). `argus-sync` persists them verbatim into
  `user_audit_log` in its own `sync.db`; nothing audit-shaped is written to
  notification.db.
- **Serving live traffic (F3-2, Ruling AR)**: this service serves
  `/notification/read` (PATCH) and `/notification-token` (POST) itself on its
  own TLS listener (7028); the envelope, statuses and validation
  (empty/unknown id handling) are byte-identical with the legacy — verified
  live. notification.db opens WAL with `busy_timeout` and only this service
  opens it (rule 27). The legacy keeps its own markAsRead/token routes
  registered but nothing routes to them any more (Ruling AS — quiet,
  not stripped).
- **RPC owner (rule 27)**: `app/rpc/notification-rpc-service.cc` serves
  `argus.notification.v1.NotificationService` on `server.grpc_port` (7038):
  `CreateNotifications` fans one row per user id and reuses the shared
  create+emit path (the whole fan-out is one multi-row INSERT with
  `RETURNING id`, so the emit mirrors strictly persisted rows), and
  `PullNotifications` serves the user-scoped `/sync` page from the identity
  metadata. `argus-sync`'s `/sync` pulls are the only client left outside
  this service; no other service opens notification.db.
- **Identity validation (f7-3)**: the JWT filter validates the caller over
  `argus.identity.v1.ValidateToken` at `[identity] target` — the user row,
  the bound refresh-token session and the device binding are resolved by the
  identity service, which owns them. This service opens NO identity.db: the
  F3-2 read-only client install is gone, and with it the boot-order wait on
  a file another service creates (`nm -C` on the binary shows zero
  UserRepository / RefreshTokenRepository / DeviceCredentialRepository
  symbols). An unreachable identity service means 401, never an open door.
- **CORS**: the legacy answered every preflight in pre-routing, and this
  surface keeps answering OPTIONS itself (`Cors::handleOptions`).
- **Foreign keys (Ruling AN, superseded 2026-10)**: the user references are
  gone from the schema and enforcement is on; see "What it owns" above for
  what an older database needs.
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
- **The feature module links whole-archive into the executable.** Both HTTP
  controllers are Drogon `AutoCreation` controllers: their routes register
  from a static initializer (`methodRegistrator`), so their object files have
  to reach the executable, and Drogon `static_assert`s against registering
  them by hand — which is why the tts reference's explicit `registerController`
  is not an option here (`services/tts` declares `HttpController<TtsController,
  false>`). A plain static-library link drops every route without a word:
  measured after the Phase 4 step 2 conversion, `nm -C` on the linked binary
  held zero `NotificationController` symbols while all 41 suites stayed green,
  because a suite that instantiates the controllers pulls their objects
  itself. The executable therefore links
  `$<LINK_LIBRARY:WHOLE_ARCHIVE,argus::notification-feature>`, and the
  controller test needs no such link.
- The standalone build compiles no AI code: the `third_party/ncnn` block and
  the unused `find_package(OpenCV)` belonged to the `argus_identity` package's
  face services. Phase 3c-1 made identity a service of its own, reached
  through `argus::clients::identity`, so this project adds neither (zero AI
  symbols in the binary, verified with `nm -C`).

## Migration tool

`tools/migrate-notification` (`argus-migrate-notification`) copies
`notification` and `notification_token` from `argus.db` into
`notification.db` and then goes quiet: a schema-current `notification.db`
makes reruns a verified no-op, because after the F3-2 cutover
`notification.db` is live data and the frozen `argus.db` copy must never be
resurrected over it. Nothing is deleted from `argus.db`. Its
`foreign_key_check` ignores user references by design (the user parent rows
live in identity.db) and fails on any other violation.

The copy names the source's own columns and the verification checksums those
columns on both sides, refusing only a source column the target lacks
(2026-10). It used to copy with `SELECT *` and compare the column lists of
`src` and `main` read as `"src".pragma_table_info(...)`, which SQLite
resolves against the main schema: the check compared the target with itself,
and a legacy table that predates a later additive column failed the copy on
its column count. `tests/unit/notification-migration-test.cc` pins both cases.

## The folder owns its domain (f7-7c, a rule-25 module since Phase 4 step 2)

The notification feature tree, the notification-token repository, schema
and service, the notification schema file and the three unit suites moved
out of the shared `src/` tree into this folder, prefixes preserved. Since
Phase 4 step 2 the feature is a module of its own
(`src/feature/notification/CMakeLists.txt` → `argus::notification-feature`)
and so is the RPC owner (`src/app/rpc/` → `argus::notification-rpc`); the
root CMakeLists discovers every `feature/*/CMakeLists.txt` and the
executable and its suites name those modules instead of listing their
sources. The notification-token trio moved once more, from `src/shared/`
into the feature that is its only reader.

What stays in `notification-core` (`argus::notification-shared` since Phase 4
step 9): the `notification` table's own
repository, schema and delivery service, which it compiles here since
sub-step 3a-1b and which both features reach. Since rule 27 this service is
the only writer and reader of those rows: the camera-notifier below creates
in-process and `argus-sync`'s `/sync` page pulls through
`argus.notification.v1`. Both sides of the table are exclusively this
service's, the notification-token side as before.

## Camera notification policy (Phase 3d step 1)

The camera object policy and its notifier moved here from `argus-gateway`'s
`src/sync/` (rule 27: policy over this service's own rows belongs where the
rows are). `src/feature/camera-notification/` now holds
`CameraNotificationPolicy` — budget per camera per rolling hour, silent hours
with wrap, the digest produced once the window rolls, guard-heartbeat
readiness and the fallback gate — `CameraObjectNotifier`, the
`camera-fallback-log` repository and its `fallback-drop-reason` vocabulary.

- **One loop owns the policy state.** The per-camera windows and the
  heartbeat stamp have no lock: every reader and writer runs on IO loop 0.
  The detection and heartbeat handlers were posted there, but the minutely
  digest flush ran on the main loop and iterated the window map while a
  detection could insert into it; the timer now posts the flush to the same
  loop.

- **Delivery is in-process.** `CameraObjectNotifier` calls
  `NotificationService::createManyAndEmit` with a `commandId` derived from the
  event, so camera notifications take the same durable
  `notification_command` idempotency path `CreateNotifications` takes: a
  redelivered camera event is a `duplicate`, not a second row, and a reused
  command id with a different fingerprint raises
  `NotificationCommandConflict`, which the notifier has no caller to answer
  and therefore logs as a failed delivery. The gateway's route — a gRPC
  `NotificationClient` presenting the gateway credential and setting no
  command id — is gone with the gateway.
- **Inputs over NATS**: `argus.camera.v1.object_detected` feeds the policy and
  `argus.guard.v1.heartbeat` marks guard readiness
  (`docs/architecture/wire-nats-subjects.md`); both are marshalled onto the
  loop. The camera subscription is an ephemeral core-NATS consumer, so events
  published while this service is down are lost with no replay (the JetStream
  stream retains them for inspection only) — the fallback path is
  best-effort by design, and the subject is not a sync change: payloads never
  reach `/sync`. The notifiable-user roster comes from
  `argus::clients::identity`, so an unconfigured identity target keeps the
  fallback record but invents no recipient.
- **A hard signal is `EventSeverity::Critical` or a person in an alert
  zone**: the event's `severity` is read through the camera contract's
  `eventSeverityFromString` (root rule 1), not compared as a string.
- **`camera_fallback_event`** is this service's third table: one durable row
  per dropped event with its reason (`non_hard_signal`, `drop_known`,
  `drop_weak_score`, `drop_short_dwell`, `budget_silent`), purged on the
  `fallback_retention_days` window. The gateway's `gateway_fallback_event` and
  the 23-line `gateway.db` it lived in are gone.
- **Why the fallback records what it does (Rounds 8/12/13, carried over)**:
  process-lifetime counters cannot reconstruct a past outage window across a
  restart, and that window is when visibility matters most — so the drops get
  a durable row, deliberately minimal (camera, rule, severity, reason,
  timestamp) and with no sync or endpoint surface. They are still counted per
  reason (`fallbackCounts`, on `/health` under `notifications_fallback`
  alongside a pass counter) and logged with their reason, and they are
  deliberately never written to guard's `guard_decision_journal`: that table is
  the belief gate's calibration population (guard-observed events carrying
  belief scores), and fallback events carry no score and come from a different
  population, so mixing them would corrupt threshold calibration. The write is
  best-effort: a missing store degrades to the counters, never to a failure,
  and the sanity gate fails open on absent wire keys so an older camera
  behaves exactly as before. The gate is not the guard belief engine — a
  degraded path with its own config, no shared module and no guard state.
- Config lives under `[notifications]`: `budget_per_hour`, `silent_start`,
  `silent_end`, `guard_heartbeat_timeout_s` and the `fallback_*` keys.
- `tests/unit/camera-notifier-test.cc` is the moved suite, with the policy,
  the notifier, the fallback gate and the retention purge as it was in the
  gateway.

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
- **A page is claimed before it is published** (2026-10, audit #47).
  `deliverPending` starts from several places at once — every create, the
  reconciler, the self-test, the call sink and the camera notifier, the last
  on another I/O loop — and two drains used to read the same pending rows and
  publish them twice. `claimPending` now stamps `claimed_at` on up to 200
  pending rows in one `UPDATE ... WHERE id IN (SELECT ...) RETURNING id` and
  only the drain that got them publishes them; a second drain sees the claim
  and moves on. A refused publish releases its rows at once, a page whose
  publish throws releases the whole page, and a claim left by a crash expires
  after 30 s (`claimed_at <= now - 30`), so the claim never strands a row.
  The settled rows are marked `sent` in one statement. The column is
  additive: `main.cc` adds `claimed_at` to an older notification.db.
- **A page never outlives its claim** (2026-10-05, review finding D2). Each
  publish waits up to 2 s for its PubAck, so a 200-row page against a slow or
  dying broker could take some 400 s while its claim expired after 30: a
  second drain re-claimed the rows still in flight and published them again,
  and while the delivery event deduplicates on `Nats-Msg-Id`, the push intent
  (a core publish) does not, so the user got the push twice. The page
  (`shared/services/notification/delivery-page`) now stops at the first
  refusal (the broker that refused one row will refuse the next; the rest are
  released at once instead of each costing another 2 s) and stops publishing
  after a 20 s budget, releasing whatever it did not reach; at least one row
  is always attempted so a page always progresses. Budget plus one PubAck
  wait plus a margin is pinned below the 30 s lease by a `static_assert`, so
  the rows a page still holds can never be re-claimed under it.
  `notification-delivery-test` pins both stops with a scripted sink and a
  fake clock. The 2 s is `NatsBus::publishWithMsgId`'s `MaxWait`
  (`delivery_page::kPublishMaxWait` restates it; change both together).
- **The broker is never awaited on the event loop** (2026-10, audit #48).
  `ensureStream` and the page's publishes (each waits for its JetStream
  PubAck) run in a `BlockingTask` on the light lane; the loop only claims,
  releases and marks. A slow broker used to freeze the loop for up to 200
  PubAcks per pass, the one-second call sweep included.
- **`markAsRead`** lands its per-change user audits in the service's own
  `change_outbox` before they are published over `argus.notification.v1.change`
  (see the change-feed section below), in the same transaction as the row
  updates it audits. The sink carries audit diffs only: a notification row
  reaches its user through the durable delivery leg, not through the change
  subject.
- **Push intents** (`[push].enabled`, default off) stay at-most-once
  fire-and-forget accelerators toward `argus-relay`; the `/sync` fan-out
  after durable delivery is the guarantee.
- **A push carries no household detail** (2026-10, audit #46). The relay
  forwards to FCM/APNs, so whatever a push intent carries passes through
  Google or Apple. A notification's intent now carries a generic line in the
  recipient's language (`push_copy::render`: "Argus" / "Tienes un aviso
  nuevo. Abre Argus para verlo.", or "Hay un aviso importante…" when
  `urgency` is `critical` or `time_sensitive`; English when the row's `data`
  says `lang: en`) and `data {notificationId, kind, urgency}` — no title,
  body, camera, place or person. A ringing call's push says "Argus te está
  llamando" / "Abre Argus para contestar." with `data {kind: call, callId,
  urgency, deepLink, expiresAt}` (`callKind` is gone). The app opens and reads
  the real row through `/sync`. The probe (`userId` 0) no longer emits a push.
  **Frontend:** a push's `title`/`body` are no longer the notification's;
  render from the synced row by `notificationId` (or `callId`), and stop
  reading `data.callKind` from a call push.

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
- **The drain wakes at the commit.** `enqueue` runs inside the caller's
  transaction, so its row is invisible until the commit; the sink's
  `db_transaction::CommitObserver` wakes the drain when a commit lands and
  its `WakeSignal` keeps a wake that arrives mid-pass. Before, the wake came
  before the commit, the drain found nothing and every live change waited
  the 500 ms retry period. The repository commits through the shared
  `db_transaction::Commit` for the same reason, instead of its own copy.
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

## Push tokens (2026-10, audit #45)

`notification_token` holds one row per `(user_id, device_hash)` and, since
2026-10, at most one row per `token`: registering a token another user (or
another device) held first deletes that row inside the same transaction, so a
shared phone that changes hands stops being addressed to its previous user.
A fresh database enforces it with `token UNIQUE`; an older one gets the same
behaviour from the delete (see "What it owns" for the rebuild).

Nothing in this tree reads the table: `findByUser` has no caller outside the
controller suite, and the push intent names a `userId`, not a token, so the
relay must keep its own registry. The table and `POST /notification-token`
stay because the app registers through them.

**Revoked sessions and disabled accounts lose their tokens** (2026-10-05).
Each row now records the session that registered it (`session_id`, the
`JwtContext.sessionId` of the request; additive, `main.cc` adds the column to
an older notification.db and existing rows keep `''`). Two durable consumers
(`feature/notification/services/token-revocation`, ordered, ack after the
delete, nak on failure, retried every 5 s until the stream exists):

- `notification-auth-session` on `argus.auth.v1.session`
  (`ARGUS_AUTH_SESSION`): a `disconnect_session` deletes the user's token
  registered by that session. A row registered before the column existed has
  no session and is not touched by this path;
- `notification-identity-user` on `argus.identity.v1.change`
  (`ARGUS_IDENTITY_CHANGE`): a `user` catalog row with `isActive: false`
  (identity's deactivation, which is what `DELETE /user/{id}` does) or with
  `deleted: true` deletes every token of the user, the legacy rows included.

Both start from new messages (`deliverAll = false`): a token registered
before this service consumed the feeds is cleaned by the next revocation or
deactivation, not retroactively. The app re-registers its token after each
login (the row is keyed by device), so a re-enabled account or a new session
gets its push back on the next registration.

**What is not deleted, and why.** Identity never deletes a user: the account
is deactivated and can be re-enabled by the Owner, who expects their
notifications, call preferences and history back. So deactivation removes
only what would deliver to a device (the tokens); the user's `notification`,
`call_preference` and `call` rows stay. `deleted: true` is handled for tokens
only, because no producer emits it for a user today; if identity ever gains a
hard delete, the same consumer is where the user's remaining rows would be
deleted.

## Mark-as-read and acknowledgement bounds (2026-10, audit #95)

`PATCH /notification/read` and `PATCH /notification/ack` refuse more than 500
ids (422, `MAX_ELEMENTS`) instead of answering 500 when the `IN (...)` list
outgrew SQLite's variable limit. `read_at` is bound from the same clock the
audit diff reports, so the stored value and the published `readAt` no longer
differ by the second between `time(nullptr)` and `strftime('now')`.
**Frontend:** batch "mark all as read" in chunks of at most 500 ids.

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
seven days are purged with their deliveries, and the same step drops
`notification_command` idempotency keys older than 30 days — a producer's
retry never comes that late, and the table otherwise kept one row per
command for ever. The user notifications themselves are not pruned here:
they are synced creation-only with no tombstone, so a server-side delete
would leave every device holding rows the server no longer has.

## Stopping cleanly (2026-10, extra finding N1)

Besides the change sink, two drains register with `shutdown_signal` before
`drogon::app().run()`:

- `notification-tasks`, a `TaskGate` shared by every long-lived loop of the
  process — the delivery reconciler, the self-test prober, the call sweep, the
  durable `known_seen` handler and the camera notifier's deliveries, log
  writes and purges. Each run takes a ticket; after the stop request no new
  ticket is handed out (a `known_seen` message is nak'ed and comes back after
  the restart) and the drain reports drained when the last running ticket is
  released, so `quit()` never destroys the database client under a coroutine.
- `notification-grpc`, the gRPC listener: the stop request calls
  `Server::Shutdown` with a 2 s deadline on a thread of its own (the drogon
  loop keeps running, so in-flight handlers that finish on it can answer), and
  the drain is drained when that call returns. `main` joins the thread after
  `run()`; before, the server was shut down only after the loop had stopped,
  with no deadline.

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

`notification-delivery-live-test` is a doctest skip without `ARGUS_NATS_URL` (a failure under `CI=true`)
(e.g. `nats://127.0.0.1:4222`); it runs the fan-out against an isolated
stream, subject, durable name and temporary database. A default run passing
means nothing about the wire — every live-test claim must state the variable
that was set. Never point it at deployment streams.

## Phase 4 step 9: config resolution into `src/config/` (D20)

`src/notification/` is gone. Its typed config is `src/config/
notification-config.{hxx,cc}` (`argus::notification-config`: db path, schema
path, the TLS listener, the gRPC listener and the identity target/secret), and
its two NATS sinks are `src/shared/services/change-sink/` and
`src/shared/services/delivery-sink/` (`argus::notification-change-sink`,
`argus::notification-delivery-sink`). `main.cc` resolves nothing of its own
but `config.toml` loading and the `nats.url` gate on the optional bus; the
push gate it consults is `push_intent::enabledFromConfig()`, read where push
is wired, not a config-module resolver.

## Owner settings

`src/feature/settings/notification-settings.cc`
(`argus::notification-settings`) is the catalog an owner may change through
`argus.settings.v1.Settings`, registered on the notification gRPC listener
(7038) beside `NotificationService` (`argus::contracts::settings-wire`).
Groups are `alerts`, `quiet`, `delivery` and `history`.

| Key | Level | Applies | Group | Range | Fallback |
|---|---|---|---|---|---|
| `notifications.budget_per_hour` | basic | live | alerts | 1-60 | 6 |
| `notifications.fallback_suppress_known` | basic | live | alerts | toggle | true |
| `notifications.fallback_min_score_median` | advanced | live | alerts | 0.05-0.95 | 0.3 |
| `notifications.fallback_min_dwell_ms` | advanced | live | alerts | 100-60000 | 1000 |
| `notifications.guard_heartbeat_timeout_s` | advanced | live | alerts | 5-600 | 30 |
| `notifications.silent_start` | basic | live | quiet | -1-23 | -1 |
| `notifications.silent_end` | basic | live | quiet | -1-23 | -1 |
| `notifications.ack_window_s` | advanced | live | delivery | 3600-604800 | 86400 |
| `notifications.selftest_interval_s` | advanced | restart | delivery | 0-3600 | 300 |
| `notifications.fallback_retention_days` | advanced | live | history | 1-3650 | 90 |

`-1` in either quiet-hour bound means no quiet hours; the window is
`[silent_start, silent_end)` in local time and may wrap midnight. A
`selftest_interval_s` of 0 disables the delivery self-test.

The camera policy keys are live through a refresh, not a re-read per use:
`CameraNotificationPolicy` copies its `Config` and every reader of it
(`handle`, the heartbeat mark, the digest flush and the fallback-log purge)
runs on I/O loop 0. The registry's `onChange` in `src/app/main.cc` calls
`camera_notifier::refresh`, which resolves the config again and posts
`CameraNotificationPolicy::reconfigure` onto that same loop, so the swap
never races a reader. Per-camera hourly windows, suppressed counts and the
last guard heartbeat survive the swap; a lowered budget applies to the
current hour's window. Without NATS there is no notifier and nothing to
refresh. `ack_window_s` is read by `NotificationConfig::resolveAckWindowS()`
on every delivery summary, so it needs no hook. `selftest_interval_s` sets
the `runEvery` period once in `startSelfTestProber()`, so it is restart.

A fallback is what the service runs with when the key is absent. One
absent-key default changed to make that true: an absent `silent_start` or
`silent_end` now resolves to -1 (it was 0, so setting only one bound made a
quiet window up to midnight or from midnight). `notification-settings-test`
pins every fallback against `camera_notifier::resolveConfig()` and the two
`NotificationConfig` resolvers.

The database and schema paths, the identity target and secret, NATS, push
and the caller secrets stay out of the catalog.

`[grpc] caller_settings` is the only credential the settings service accepts
(service name `settings`); `CreateNotifications` keeps `caller_guard` and
`PullNotifications` keeps `caller_sync`. An empty `caller_settings`, or one
equal to either of the others, registers no settings service.
`notification-settings-test` checks the separation both ways on a live
server holding both services.

## Fallback alerts people can read (2026-10)

The fallback path (guard silent, a hard camera signal) used to send
"Front door: person_in_alert_zone" / "Severity critical; detected person"
and an hourly "3 events suppressed (2 person, 1 car)" — the rule names and
counters of the camera, in English, to every reader. It now renders through
`camera-notification-copy` (pure, es/en): "Persona en la zona de alerta ·
Front door" / "La vigilancia de Argus no responde, así que este aviso llega
directo de la cámara. Echa un vistazo a la imagen." and, for the digest,
"Mientras la vigilancia no respondía · Cámara 4" / "La cámara detectó 2
personas y 2 vehículos que no se avisaron uno a uno." The policy's
`takeDigest` returns the per-class counts instead of a sentence, so the
words live in one place.

Each recipient reads their own language: the notifier groups the roster by
the `lang` of each user's identity record (`GetUser`) and creates one batch
per language (command id `<commandId>:<lang>` when there is more than one);
`[notifications] lang` (default `es`) covers a record with no language.
`data` keeps the camera event and adds `kind` (`camera_fallback`,
`camera_fallback_digest`), `threadKey` (`camera:fallback:<cameraId>`),
`urgency` (`time_sensitive` for an alert, `passive` for a digest) and
`lang`, the same shape argus-guard uses for its own notifications so a
client can render, group and later push both the same way.

The TLS listener also reloads a rotated instance certificate
(`certificate_reload::watch`, lib/http 897c23bf) instead of failing 30 days
after a rotation until restarted; argus-guard does the same.

## Kinds of a module that is off (2026-10, the effects wave)

A disabled module must not keep producing what it used to: no summary,
no intruder alert, no agenda call, no ring, while the history already written
stays. argus-notification now reads the enabled set like camera, guard,
identity and productivity (`module_gate::install` in `main.cc`, durable
`argus-notification-modules`, `[modules] target/credential`, the pair that
`ensure_fleet_callers` already minted), and one table decides what is a
module's: `src/shared/vocabulary/notification-kind.hxx`, a map from the
`data.kind` of a notification to a module.

| Module | Kinds |
|---|---|
| `surveillance` | `guard_episode`, `guard_tamper`, `guard_digest`, `guard_arrival`, `camera_fallback`, `camera_fallback_digest` |
| `productivity` | `agenda_event` |
| core (not in the table) | everything else: `guard_panic`, `guard_duress`, `guard_panic_sent`, `guard_response`, `agenda_reminder`, `assistant_reminder`, `call`, `module_request`, the app's own |

Panic, duress, the response plan and reminders are core on purpose: they are
safety and the user's own words, and a household that turned cameras off still
presses the panic button.

- **The funnel refuses.** `NotificationService::createManyAndEmit`, the one
  place every notification row is created (the `CreateNotifications` RPC, the
  camera fallback notifier and the call engine's notices), asks
  `Dependencies::kindAllowed` first; a kind of a module that is off creates
  nothing (`createdCount` 0, no delivery, no push). The RPC then does not hand
  the batch to the call engine either, since it only does for created rows.
- **The call engine refuses its own sources.** `announceAgenda` skips an
  `agenda_event` and `announceArrival` a `guard_arrival` while their module is
  off (an `agenda_reminder` still rings). A call has no schedule of its own
  here: the agenda's schedule is productivity's sweep (it stops announcing
  events, `services/productivity/CONTEXT.md`) and the only `scheduled_call`
  rows are the assistant's reminders, which are core.
- **Rings end.** When the gate reports a module off, `CallEngine::
  cancelForModule` settles every ringing call whose `data.kind` belongs to it
  as missed with `CallCancel` reason `module_disabled`, without the "missed
  call" note (the module is off, there is nothing to catch up on); a panic ring
  next to it keeps ringing. A follow-up queued under a ring of another kind
  still reads out when that ring is answered; that is the one case this does
  not cover.
- **Responses of the module end, a raised panic or duress alert does not.**
  The same call also expires the open `call_response` rows of the module's
  kinds (`guard_episode`, `guard_tamper`; `ResponseUpdate` with `expired` goes
  to the members already reached) so the escalation of an intruder alert stops
  stepping through the household. A `guard_panic` or `guard_duress` response is
  core: it keeps ringing, escalating step by step and asking for the contacts
  until someone answers it, with surveillance on or off.
- **Answering stays reachable, scoped to the recipient.** Reading and
  answering a response (`GET /notification/responses`, `GET` and `PATCH
  /notification/responses/{id}`) never look at the gate: `CallEngine::response`
  and `verdict` require that the caller was reached by that response
  (`call_response_member.reached_at`) and answer 404 otherwise, whatever the
  caller's role or the module's state. A Guard whose role went inactive with
  surveillance reads and resolves the alert it received; a person it never
  reached gets 404 on both. `GET /guard/safety` and `POST /guard/panic` are core
  routes of argus-guard (`kCoreRoutes`, packages/lib/auth), and guard's own
  retries of an alert already raised do not look at the gate either.
- **History stays.** Notifications already written are not touched and still
  sync; the app hides the kinds of an inactive module itself.

Tests: `call-engine-test` ("the notification kinds of a module name that module
and nothing else does", the agenda, the arrival, the cancel, the funnel, "a
raised panic or duress alert survives surveillance going off and its recipients
still answer it" and "a surveillance response ends with the module while a
panic response next to it stays open"); guard's `guard-scenario-test` raises
and escalates a panic and a duress alert with the module off.

## Argus calls you (2026-10, RTC wave)

The owner's words: "When something important happens the LLM can 'call' the
user, to say 'I detected an intruder' or 'someone arrived', even remind us of
the agenda. It works like a call: you can interrupt. When Argus calls you it
starts a conversation and you don't need to confirm. Inside the app it just
activates and tells you; outside, then yes, a call."

**Why the engine lives here.** A call is the loudest form of a notification:
it reaches the same people, in the same language, through the same push
tokens, and when nobody answers it *becomes* a notification. This service
already owns the roster (through identity), the per-user language, the push
intent and the durable notification row, so `src/feature/call/` is a feature
of argus-notification rather than a service of its own; nothing else would
own anything a call needs. The voice side (LiveKit room, the agent speaking
first, the `/rtc/token` route that answers a call) belongs to argus-voice and
argus-sync and is described in RTC-CONTRACT v1, section 5.

### Triggers

| Trigger | Source | Becomes a candidate when | Dedupe key |
|---|---|---|---|
| `guard_critical` | guard's `CreateNotifications` | `kind = guard_episode`, `urgency = critical`, phase `opened` | `threadKey` (`guard:episode:<id>`) |
| `guard_intruder` | same | `urgency = time_sensitive` (danger high: a stranger at night, away, armed, in an alert zone) | same |
| `guard_escalation` | same | phase `escalated` to high or critical | same, so an episode that already called never calls again |
| `guard_arrival` | `argus.guard.v1.known_seen` | a recognized person seen after `calls.arrival_absence_s` (3 h) without a sighting; only for users who opted in, never the person themselves, never a guest | `guard:arrival:<personId>:<at>` |
| `agenda` | productivity's agenda announcer (`CallService.AnnounceAgenda`, `caller_productivity`) | `kind = agenda_event` in the recipient's lead bucket, or `agenda_reminder` (due) | `agenda:event:<id>:<startsAt>` / `agenda:reminder:<id>:<at>` |
| `assistant` | `CallService.ScheduleCall` from argus-llm | the user asked for a timed reminder (`memory.remind` with a time the user said) and the time has come | `assistant:scheduled:<id>` |

Guard needed no change for its episodes: its notification `data` already
carries `kind`, `phase`, `urgency`, `threadKey`, the camera, the environment,
the subject, the reasons and the action, so the engine reads them after
`CreateNotifications` persisted the rows (`NotificationRpcService` hands the
batch to `CallEngine::considerNotification` once it has answered guard; a
duplicate command never calls twice). Tamper and digests never call.

The LLM contract is deliberately narrow: the model cannot call anyone. The
only path is `memory.remind` when the *user's* words carry a time that
`call_time::resolve` turns into an instant (relative phrases within 30 days,
explicit dates within 12 months: "mañana a las nueve", "en 20 minutos", "at 7
pm", "el 3 de marzo"); the call goes to the speaking user only, its topic is
the grounded reminder text with the time phrase cut out, and the engine
validates it again (`ScheduleCall`: 1-300 bytes, at most 20 pending per user,
idempotent by command id, inside the horizon below).

**The schedule horizon is 12 months.** `kScheduleHorizonDays = 366`
(`config/notification-config.hxx`, the only place the number is written,
`CallEngineConfig::scheduleHorizonS`) is 12 months counted as 366 days, so the
same date a year later is always inside whichever year it falls in; it matches
the 12 months the voice date resolver accepts, and it replaced a 30-day literal
that left a reminder resolved for next March as a row with no call. It is not a
`config.toml` key, so an install with an old config changes with the binary
alone: nothing to migrate, nothing to set. A farther reminder is still saved by
argus-llm as a reminder row, but `ScheduleCall` refuses it.

A long-dated call costs one `scheduled_call` row and nothing else: no timer, no
task. `sweep()` reads the due rows through `idx_scheduled_call_due (state,
fire_at)`, a restart finds the row where it was, and the history purge only
deletes rows that are no longer `pending`, so a call 12 months out survives
every purge and every restart until its instant. The due instant is a unix
second: no calendar arithmetic, so Lima (UTC-5, no daylight saving) and any
other zone fire a stored call at the same instant, and the local hour only
decides quiet hours and the do-not-disturb window at the moment it fires.

What `ScheduleCall` answers (argus-llm's `memory.remind` must say a call was set
only on an OK answer, and a duplicate is an OK answer: the call exists):

| Answer | gRPC status | message | Meaning |
|---|---|---|---|
| scheduled | OK | | the row exists (`scheduled_id`) |
| duplicate command id, same call | OK, `duplicate = true` | | it already exists |
| command id reused for another call | ALREADY_EXISTS | `command id reused with another call` | nothing stored |
| more than 20 pending | RESOURCE_EXHAUSTED | `too many pending calls` | nothing stored |
| later than the horizon | OUT_OF_RANGE | `SCHEDULE_TOO_FAR` | nothing stored |
| earlier than the grace (60 s) before now | OUT_OF_RANGE | `SCHEDULE_IN_PAST` | nothing stored |
| bad user, command or topic | INVALID_ARGUMENT | the reason | nothing stored |
| not authorized | UNAUTHENTICATED | | |
| the service failed | INTERNAL, UNAVAILABLE or DEADLINE_EXCEEDED (client) | | unknown: do not say it was set |

`NotificationClient::scheduleCall` maps ALREADY_EXISTS to `Conflict`,
UNAVAILABLE and DEADLINE_EXCEEDED to `Unavailable` and every other failure to
`Rejected`, with the code in `result.status.error_code()` and the text above in
`result.status.error_message()`. Tests: `call-engine-test` ("a call can be
scheduled up to 12 months ahead...", "a refusal for the horizon follows the
configured horizon...", "a call scheduled months ahead is one stored row that
survives a restart and fires at its own instant on the Lima clock"),
`notification-client-test` (the two refusal codes reach the caller).

### Policy (`call_policy::decide`, pure)

Checked in this order; the first rule that answers wins.

| # | Rule | Result | Injectable into a live call |
|---|---|---|---|
| 1 | this user already had a call for this dedupe key (one call per episode) | drop | no |
| 2 | the user's mode for the trigger is `off` | drop (an `assistant` reminder still becomes a note) | no |
| 3 | the user's mode is `notify` | the notification only | no |
| 4 | `calls.enabled` is false | the notification only | no |
| 5 | the user switched calls off (`enabled = false`) | the notification only | no |
| 6 | guard trigger from a muted environment | the notification only | no |
| 7 | do-not-disturb until a time in the future, unless critical and `criticalBypass` | the notification only | yes |
| 8 | inside the user's quiet hours (may wrap midnight), unless critical and `criticalBypass` | the notification only | yes |
| 9 | a call is already ringing for this user | follow-up: joins that call's opening line ("Además, …") | yes |
| 10 | not critical and the last call rang less than `calls.call_gap_s` (300 s) ago | the notification only | yes |
| 11 | not critical and `calls.max_calls_per_hour` (4) calls in the last hour | the notification only | yes |
| 12 | otherwise | ring | yes |

"Critical" is a `guard_critical` trigger or any candidate with urgency
`critical`. "Injectable" means: before acting, the engine asks argus-voice
(`VoiceService.Announce`, RTC-SERVER's) whether the user is already in a
conversation; if so the follow-up line is spoken there at the next quiet
moment ("Además, hay una persona desconocida en Patio.") and recorded as an
`injected` call, so the episode never rings afterwards. A voice service that
is unreachable or not wired never blocks a ring: the probe runs off the loop
on the light blocking lane, and with `[voice] target/credential` unset the
engine skips it.

**Recipients are judged together** (2026-10, audit #49). `consider` reads the
recipients' identity records in one `ListUsers` call
(`CallDirectory::recipients`; a single recipient still uses `GetUser`, and a
failed listing falls back to one `GetUser` each), decides every recipient
against the local tables, then asks argus-voice about all the injectable ones
at once — up to `call_engine::kProbeParallel` (8) probes side by side on
plain threads inside one light-lane job — and only then rings. A panic with
strategy `everyone` used to probe and ring members one after the other, so the
last one rang up to 1.5 s × members late; the probe now costs one deadline for
the whole household. The role a recipient carries is the contract's
`UserRole` (`std::optional`, empty when identity did not say), not a string.

### Who decides what: user preferences and system limits

David (2026-10): "la agenda, 10 minutos antes de un evento — este punto y la
gran mayoría que sea configurarlo lo que consideres que cambian dependiendo
del usuario". Anything that is a matter of taste or routine is the user's;
what protects the household from a runaway engine stays the owner's.

| Preference (user, own row) | Values | Default | Why it is per user |
|---|---|---|---|
| `enabled` | bool | true | someone may not want calls at all |
| `guardCritical`, `guardIntruder`, `guardEscalation`, `guardArrival`, `agenda`, `assistant` | `call` / `notify` / `off` | call ×5, arrival off | how loud each kind of news should be is personal |
| `agendaLeadMinutes` | 0, 5, 10, 15, 30, 60 | 10 | some need 30 min to get ready, some want it as it starts |
| `quietStartHour`, `quietEndHour`, `quietDays` | -1..23, 7-bit mask (bit 0 Sunday) | off, every day | sleep and work routines differ; a window belongs to the day it starts (Fri 22-07 covers Sat 03:00) |
| `dndUntil` | epoch s or 0 | 0 | do-not-disturb with an end time |
| `criticalBypass` | bool | true | whether a critical alert breaks quiet hours and do-not-disturb |
| `mutedEnvironmentIds` | up to 64 ids | none | someone may not care about the restaurant |
| `ringSeconds` | 20..90 | 45 | how long a ring lasts before it is a missed call |
| `pushDelaySeconds` | 0..30 | 4 | how long the open app gets before the phone is pushed |
| `liveAnnounce` | bool | true | whether news may be spoken into a call the user is in; when false the normal `call_incoming` frame arrives and the app shows it as a waiting banner (RTC-APP) instead of joining |
| `lang` | "", es, en | "" (account language) | the calls' language, independent of the account's |

| System limit (owner catalog / config) | Default | Why it is not per user |
|---|---|---|
| `calls.enabled` | true | the owner can switch the whole feature off |
| `calls.max_calls_per_hour` | 4 | anti-fatigue cap for non-critical calls, a safety net |
| `calls.call_gap_s` | 300 | minimum spacing of non-critical calls |
| `calls.arrival_absence_s` | 10800 | what counts as "arrived" is a property of the house's sightings, not of a listener |
| `calls.scheduled_late_s` | 900 | when a delayed reminder call becomes a note |

`calls.ring_timeout_s` and `calls.in_app_grace_s` left the owner catalog and
became `ringSeconds`/`pushDelaySeconds`; productivity's `agenda.lead_minutes`
became `agendaLeadMinutes`. The ring length and push delay of each call are
stored on its row (`expires_at`, `push_after`), so changing a preference never
moves a call already ringing.

**How a per-user lead time works across services.** Productivity owns the
calendar (rule 27) and the notification service owns the preferences, so
neither can do it alone. Productivity announces each upcoming timed event
once per lead bucket (60, 30, 15, 10, 5, 0 minutes before) through
`CallService.AnnounceAgenda {user_ids, lead_minutes, …}` (caller_productivity);
the engine keeps, for each recipient, only the bucket equal to their
`agendaLeadMinutes`, writes their `agenda` notification
(command `<thread>:<lead>:<userId>`) and then judges the call. Reminders are
due-time items: they are announced in bucket 0 and reach every recipient.
An event created or moved inside a bucket sends the buckets already passed at
once, so a user with a 30-minute lead still hears about an event added 10
minutes before it starts. Deleted events stop being announced because
productivity reads live rows only.

Every role reads and writes only its own row: `GET
/notification/call-preferences` and `PATCH /notification/call-preferences`
(PATCH semantics, every field optional, validated with the DSL; 422 with the
field names otherwise).

Several household members: guard notifies its roster, and each recipient is
judged on their own preferences, so an intruder rings everyone who lets it
(they all live there); one member answering does not silence the others.
Agenda calls go to the event's owner and the people it is shared with.

### Delivery

1. **Ring.** A `call` row (`ringing`, `expires_at = now + ringSeconds`, `push_after = now + pushDelaySeconds`) and
   a `/sync` frame through argus-sync's `SyncControlService.EmitToUser`:
   `{operation: 8 (call_incoming), option: notification, info: {callId,
   reason, summary, urgency, kind, episodeId?, cameraId?, cameraName?,
   environmentName?, lang, expiresAt}}`. An app in the foreground answers at
   once: it asks `/rtc/token {callId}`, which claims the call.
2. **Claim.** `CallService.ClaimCall` (caller_sync / caller_voice): the first
   session wins (`answered`), the same session re-claiming gets the same
   answer, any other gets `TAKEN`, a missed or timed-out call `EXPIRED`, a
   call of another user or an `rtc-…` id `NOT_FOUND`. The answer carries the
   opening line, which the agent speaks first, verbatim, once the user's
   microphone track is up. The line carries the context: "Hola, Laura. Te
   llamo por algo urgente de la vigilancia. Hay una persona desconocida en
   Patio, de noche. Le estoy avisando por el altavoz. ¿Quieres que te muestre
   la cámara?"; follow-ups queued while it rang are appended. Every other
   device of the user gets `call_cancel {reason: answered_elsewhere, claimedBy:
   <session id>}`; the claiming device ignores a cancel naming its own session
   (it can arrive before its own `/rtc/token` answer).
3. **Out of the app.** A sweep every second pushes a still-ringing call once
   at its `push_after` (4 s by default): a push intent with `type: call`, the
   generic "Argus te está llamando" line and `data {kind: call, callId,
   urgency, deepLink: argus://call?callId=<id>, expiresAt}` — the reason and
   the place stay in the `call_incoming` frame (see "A push carries no
   household detail"). Push is behind `[push] enabled` and argus-relay, as
   every push is.
4. **Missed.** After `ringSeconds` (45 s by default) unanswered the call is `missed`,
   `call_cancel {expired}` goes out, and a notification of type `call` is
   written with a spoken-style summary ("Te llamé porque había una persona
   desconocida en Patio.") and `data {kind: call, callId, threadKey (the
   source's), urgency, summary, episodeId?, cameraId?}`; follow-ups are added
   to its body. A call ended by the agent with `DECLINED`, or without the
   opening line spoken, is `declined` and leaves the same note. Answered calls
   nobody ended are closed after two hours.

Scheduled reminder calls are fired by the same sweep. One that comes more
than `calls.scheduled_late_s` (15 min) late (the service was down) becomes a
notification instead: a call at the wrong time is worse than a note.

**Native call UI (later phase, not built).** iOS CallKit + PushKit VoIP pushes
and Android `ConnectionService`/a full-screen-intent notification would make
the out-of-app ring a real phone call. Android 14 grants
`USE_FULL_SCREEN_INTENT` by default only to calling and alarm apps, so Argus
would have to declare itself a calling app on Play; iOS VoIP pushes must
report a call to CallKit every time, and critical alerts that pierce
do-not-disturb need Apple's critical-alerts entitlement. The deep link and the
`call` push type are the hooks those layers would use; nothing in the engine
changes.

### Arrivals survive a restart (2026-10, extra finding N16)

`argus.guard.v1.known_seen` is read through a durable JetStream consumer
(`notification-known-seen` on guard's `ARGUS_GUARD` stream, ordered, ack after
`CallEngine::arrival` returns, nak on failure) instead of a core subscription
that lost every sighting published while this service restarted. The guard
ensures that stream at boot, and the feed keeps `subscribeDurable`: the
ensuring `subscribeDurableFeed` requires the consumer subject to be spelled
inside the stream's own subject list (`covered`, an exact string comparison),
so the call feed would have to create `ARGUS_GUARD` carrying only
`argus.guard.v1.known_seen` — and guard's own `ensureGuardStream` then refuses
that stream for ever ("carries different subjects; refusing to repurpose
it"). If the stream is missing anyway (guard not started) the
subscription is retried by a self-owning chain (`KnownSeenRetry`, one strong
reference per pending timer, released on the attach or on the stop) with a
backoff (5 s doubling to 60 s, the durable attach marked `quiet` so the bus
does not log the same missing stream on every attempt, while a durable
policy conflict still logs its reason), one `WARN` when the feed goes
unavailable and one `INFO` when it connects. A replayed sighting still
updates `call_arrival_seen`, but one
older than `call_engine::kArrivalStaleS` (10 minutes) never calls anybody:
"Marta has arrived" an hour late is noise.

`call_arrival_seen` is written after the arrival's work, not before
(2026-10-05, review finding D1). It used to be touched first: when the
identity lookup or the preference read then threw, the message was nakked,
and the redelivery read a `last_seen` equal to its own `at`, a gap of zero,
and dropped the arrival silently. Now `arrival` reads the previous sighting,
announces when it follows an absence, and only then records the new one; a
failed attempt leaves no trace, so the durable retry calls. A redelivery of a
sighting that did succeed reads its own `at`, sees no absence and does
nothing, and the dedupe key (`guard:arrival:<personId>:<at>`) would refuse a
second ring anyway. `call-engine-test` fails the first lookup and pins the
ring on the retry.

### Retention of the call tables (2026-10, audit #51)

The one-second sweep purges, at most once an hour, what the engine no longer
needs after `[calls] retention_days` (default 30): settled `call` rows (not
ringing, queued or answered), fired or cancelled `scheduled_call` rows,
`call_arrival_seen` rows not refreshed in that window, and closed
`call_response` rows (`false_alarm`, `expired`) with their
`call_response_member` rows — the plan copies that hold contacts' phone
numbers. Open responses expire after two hours anyway, so nothing alive is
touched. `call_preference` is the user's setting and is not purged, not even
when the account is deactivated (see "Push tokens").

### Research behind the policy

- Alarm and alert fatigue: an alert must be actionable, and the cost of a
  false call is trust (ISA-18.2 / EEMUA 191 alarm rationalisation, Bliss'
  cry-wolf studies, already the basis of guard's own notification policy).
  Hence calls only for critical and intruder episodes and things the user
  asked for, everything else stays a notification; one call per episode;
  a cooldown and an hourly cap for non-critical calls.
- Monitoring centres call before they act and work down a contact list
  (Ring Alarm's emergency process: the primary contact first, then the next;
  enhanced call verification in US alarm ordinances). Argus calls the
  household members the episode concerns, each by their own preferences.
- Assistants keep proactive speech opt-in and respect do-not-disturb
  (Alexa notifications are opt-in per skill and suppressed by Do Not
  Disturb; iOS Focus with interruption levels, where only critical alerts
  break through). Hence user-level modes per trigger, quiet hours,
  do-not-disturb, and `criticalBypass` as the one explicit exception.
- In a live conversation an interruption is cheaper than a second channel:
  the news is spoken into the call, the way the voice session already offers
  camera events, instead of ringing a device the user is holding.

### Code map

`src/feature/call/` (`argus::notification-call`): `vocabulary/` (trigger,
mode, state), `schemas/`, `repositories/` (`call`, `call-preference`,
`scheduled-call`, `arrival-seen`, `call-response`), `services/call-policy`
(pure),
`call-trigger-classifier` (notification data → candidate), `call-copy`
(es/en opening, follow-up, missed lines), `call-engine` (consider, claim,
end, schedule, arrival, sweep), `call-feed` (the `known_seen` subscription
and the one-second sweep), `call-preference-service`, the controller and its
DTO, and `infra/` (argus-sync signal, identity directory, notification sink).
`src/app/rpc/call-rpc-service` serves `CallService` on the gRPC listener.
Tables: `call_preference`, `call` (unique `(dedupe_key, user_id)`; the
ringing insert is one guarded statement, so two triggers at once cannot ring
the same user twice), `scheduled_call`, `call_arrival_seen`.
Config: `[calls]`, `[sync] control_target/control_secret`, `[voice]
target/credential`, `[grpc] caller_voice/caller_llm/caller_productivity`;
`setup.sh`, `native-stack.sh` and `provision-host.sh` pair the credentials.
Tests: `call-policy-test` (every rule above, the classifier, the copy, call
ids) and `call-engine-test` (a temporary database and fakes for sync, voice,
identity, notifications and push: ring, one per episode, claim races,
follow-ups, injection, push after the grace, missed and declined notes,
quiet hours, do-not-disturb, cooldown, arrivals, scheduled and late calls).

### Intruder response: steps, hand-off and verdict (2026-10, RESPONSE)

David (2026-10-04): per environment the Owner decides who is called, Owner
and Guards together first, then the Residents in order, then the external
contacts; the first responder sees the camera and says "Es real" or "Falsa
alarma"; everyone else sees who is attending; nobody is called twice.

**Who and in what order is guard's; how it rings is ours.** argus-guard owns
the environments, their recipient lists and presence, so it builds the plan
(`services/guard/CONTEXT.md`, "Who is called") and sends it inside the
notification `data` as `response`. `NotificationRpcService` takes `response`
out of `data` before the rows are written, so the plan (user ids, contacts'
phones) never reaches the synced notification rows, and hands both to
`CallEngine::respond`. A notification without a plan, or with one that does
not parse, goes through `considerNotification` exactly as before.

**One response per thread.** `call_response` (unique `dedupe_key` = the
guard `threadKey`) and `call_response_member` (one row per plan entry:
step, `call`/`notify`, `mandatory`, `discreet`, `reached_at`). Several language
batches of one guard notification open one response: the first creates it
(`INSERT OR IGNORE`, members in the same transaction), and each batch marks
its users as reached and considers them. The actor of a panic or duress
(`data.actorUserId`) is dropped from the plan and the batch here too, so they
never ring and never get a `response_update`.

**Per-person overrides of the policy.** `CallPolicyInput` gains
`mandatory` (a guard on duty) and `planNotify` (the Owner listed this person
as notify, or the night rule turned everyone to notify). A mandatory member
skips their own switches (trigger mode, calls off, muted environment,
do-not-disturb, quiet hours) but keeps the system limits (`calls.enabled`,
one call per episode, a ringing call becoming a follow-up, cooldown and the
hourly cap for non-critical). `planNotify` comes after the user's own mode, so
a user who switched the trigger off still gets nothing.

**Escalation.** The one-second sweep advances each `active` response whose
`step_deadline` passed. It moves to the next step and reaches its members: a
notification of their own, "<the summary> Nadie ha contestado todavía.",
then the ring. A step with only notify members gets a zero deadline, so the
next sweep moves on. After the last step the response is `unanswered`, and
everyone reached gets a "Nadie ha contestado · <place>" notification that
names the first two contacts and the emergency number. Argus cannot place a
phone call, so the app offers one tap to call or text them. The window per
step is the environment's `stepSeconds` (default 45, the same as a ring).

**Hand-off.** When a call of a response is claimed, the response becomes
`attended` with that person as responder, every other ringing call of the
thread is closed (`missed`, reason `attended`, no missed-call note) with
`call_cancel {reason: attended, attendedBy, responseId}`, and the escalation
stops. Every reached member gets `response_update`
(SyncOperation 10): the response as JSON, with the attending person's name,
so the app shows "<Name> está atendiendo".

**Verdict.** `PATCH /notification/responses/{id} {verdict: real |
false_alarm}`, by a member the response reached.
- `false_alarm` closes the response, cancels every ringing call of the
  thread (reason `resolved`), and stops the steps.
- `real` (`confirmed`) stops the decider's own ring, since they already
  acted. It then reaches everyone not yet reached at once, with critical
  urgency so their quiet hours with `criticalBypass` let it through, and sends
  the contacts notification ("Alerta confirmada · <place>").
- The note a newly reached member gets says why they are reached now:
  "Nadie ha contestado todavía" for a next step, "<Name> ha confirmado que es
  real" after a verdict, "La situación ha empeorado" after an escalation
  (`ResponseReach`). The live sandbox check caught the confirmed case reading
  "nobody answered".
- A second identical verdict answers the same. The opposite one after a false
  alarm is 409 `ResponseClosed`. `real` can still become `false_alarm`.
- The person who gives the verdict becomes the responder if there was none.
- The verdict is published on `argus.notification.v1.response_verdict` into
  the JetStream stream `ARGUS_NOTIFICATION_VERDICT` (msg id
  `response-verdict:<responseId>:<verdict>:<userId>`, ensured at boot and
  again when a publish fails), off the event loop on the light lane; guard
  reads it through its durable consumer `argus-guard-verdicts` and acks after
  storing the review, so a verdict given while guard is down still lands.
  Guard labels the episode (`false_alarm` / `useful`) for a `guard_episode`.
  Panic, duress and tamper verdicts stay on the response row, because their
  `episodeId` is not an encounter.
`GET /notification/responses` lists the responses that reached the caller (open, or
closed in the last 24 h), each with the caller's own member row from the
same join (one query, not one per response since 2026-10);
`GET /notification/responses/{id}` reads one. A response nobody closed
expires after two hours.

**Copy.** Panic: "Botón de pánico · Casa" / "Tom ha pulsado el botón de pánico
en Casa. Puede necesitar ayuda ahora mismo." Duress: "Alerta silenciosa ·
Casa" / "… Puede estar bajo amenaza: no le llames." Critical tamper:
"Revisa la cámara · Patio". A discreet member (people inside, intruder
outside) hears "Te aviso en voz baja: hay una persona desconocida en Puerta
trasera. No abras y quédate dentro.", and `call_incoming` carries
`discreet: true` so the app rings without sound. `call_incoming` also carries
`responseId` and the plan's `offers` (`camera`, and `siren` only when guard
saw everyone positively away).

**Not built here, on purpose.** The siren is never triggered by this service
(AGENTS rule 11). The app's siren offer goes through the camera controls the
Owner already has. The emergency number is dialled by the phone.

Tests: `call-engine-test` (step by step escalation to the contacts prompt,
hand-off, false alarm and its feedback, real and its escalation, a guard on
duty with calls off, notify members, discreet copy, panic actor never
reached, escalation phase reaching everyone, listing per member, broken plan
fallback) and `call-policy-test` (mandatory and plan-notify rules, panic/
duress/tamper copy, contacts copy, defensive plan parsing).

## The outbox is `argus::lib::outbox` (2026-10-05 audit, #69)

`src/shared/repositories/change-outbox` was one of five diverged copies and is
gone, together with `notification-change-outbox-test`, whose generic cases are
the library's suites now (they run in this service's CTest graph).
`NatsNotificationChangeSink` keeps the user-audit payload and the
`notification-change:` transition id and hands the row to
`outbox::TransactionalOutbox`. What changed, none of it on the wire (same subjects, streams, msg ids and
payloads) and none of it visible to another service:

- `change_outbox` gained `subject TEXT NOT NULL DEFAULT ''`, appended by the
  boot migration right after the schema runs (fatal on failure) and declared at
  the end of `schema.sql`; a row from before it reads `''` and is published on
  the configured change subject.
- Pending rows leave in insertion (`rowid`) order rather than `created_at,
  rowid`.
- A relay that cannot publish backs off exponentially to 5 s instead of
  retrying every 500 ms.
- `notification-shared` no longer links an outbox module it never included.

## argus-llm tells the user about a kept request (2026-10, the context plan)

`CreateNotifications` admitted the guard alone. argus-llm is now admitted too
(`grpc.caller_llm`, the credential it already uses to schedule calls), but only
for what a pending intent needs (`services/llm/CONTEXT.md`, "Pending intents"):
the type must be `assistant_task`, exactly one recipient, and no `response`
plan in `data` (so it can never start a call). Anything else from that caller
is `PERMISSION_DENIED`; an unknown credential stays `UNAUTHENTICATED`; the
guard's behaviour is unchanged. `assistant_task` is a core kind (it is not in
`kModuleKinds`), so it is created while any module is off. The row is a plain
title and body for the user's list, with `data {kind: "assistant_task",
commandId}`; the command id is derived from the intent so a retry is a
duplicate, not a second notice.

`notification-rpc-test` pins the admitted notice and the five refusals (another
type, two recipients, none, a call plan, a wrong credential).

## Module requests (2026-10, module effects)

`NotificationModuleRequest` answers argus-settings' `RequestModule` call (the
settings wire, on the existing settings credential) behind `POST
/modules/{id}/request`. It lists the household through identity, picks the
active Owners and writes one `module_request` notification for each in that
Owner's language (`module-request-copy`): title "Piden un módulo" / "Module
request", body "<name> quiere usar el módulo <module>. ¿Lo activas?" /
"<name> would like to use the <module> module. Turn it on?", `data { kind:
"module_request", moduleId, requestedBy, requestedByName, action:
"enable_module", threadKey, urgency: "active", lang }`. `module_request` is a
core kind (not in `kModuleKinds`): it must reach the Owner whatever is on. The
command id `module_request:<module>:<userId>:<local day>:<owner>` makes the same
person's second request for the same module the same day a duplicate, which the
route answers as `duplicate: true` instead of telling the Owner twice. Without an
identity target, or when identity does not answer, the call fails (the route
answers 503) rather than reporting a request nobody will read
(`notification-module-request-test`).

