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
  `services/notification/database/schema.sql:22`, the token pair's unique
  index at `:36-37` and `idx_notification_token_user` at `:72` — and
  `camera_fallback_event` carries one on `created_at` (`:100-101`) for its
  retention sweep and the per-outage queries.
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
