# Phase 3d step 1a — the camera notification policy moves to where the rows are

The camera-object policy, its notifier and its fallback log left
`argus-gateway` and now live in `argus-notification`, delivering through the
notification service's own create path instead of a gRPC hop back to it. The
gateway's database went with them: `services/gateway/database/schema.sql`, the
`gateway.db` mounts and the `DbService::gatewayClient()` accessor are gone.

This is the first half of Phase 3d step 1; the per-service TLS listener, mDNS
announcement and filter chain (1b) and the deletion of `services/gateway`, its
proxy and `contracts/gateway` (1c) follow, in that order — the gateway cannot
die before every service terminates TLS itself.

## What existed before

`argus-gateway` was the only writer of camera fallback events and the only
publisher of camera notifications, and it did both from `src/sync/`:

- `CameraNotificationPolicy` — budget per camera per rolling hour, silent hours
  with wrap, the digest produced once the window rolls, guard-heartbeat
  readiness, the fallback gate — and `CameraObjectNotifier`, which applied the
  policy to `argus.camera.v1.object_detected` and delivered by calling the
  notification service over gRPC (`NotificationClient`, gateway credential, no
  command id) after asking identity for the notifiable roster.
- `gateway_fallback_event` in `gateway.db`, written through
  `DbService::gatewayClient()` — a client that never fell back to the host's own
  database, so the gateway had to open `database/gateway.db` for it (23 lines of
  schema, a data directory bind-mounted into the container, its own
  `freezeClient` quit hook).
- The fallback counters and the `nats` / `notifications_fallback` blocks of
  `GET /health`.

Two rules were broken by that arrangement, and both were recorded as the
transitory price of moving the writer before the service. Rule 27: the fallback
record was a *notification-domain* row living in a database of the gateway's
own making, opened by a service that owns no notification data. And rule 23's
"one home per capability": the decision that a camera event becomes a
notification row was taken in a service whose only relation to the row was a
gRPC call, while the service that owns the row could not decide anything about
it.

## The decision, as built

### The policy moves, the delivery becomes in-process

`services/notification/src/feature/camera-notification/` holds the feature as
one vertical slice: `services/camera-notification-policy.{hxx,cc}` (moved
method-for-method — `shouldNotify`, `takeDigest`, `fallbackDecision`,
`inSilentHours`, `guardReady`, `markGuardHeartbeat`, the counters),
`services/camera-object-notifier.{hxx,cc}`, and
`repositories/camera-fallback-log/` (query file with its SQL namespace and
`CameraFallbackLogInput`, the repository, and the `fallback-drop-reason.hxx`
vocabulary: an `enum class` with its own lowerCamelCase
`fallbackDropReasonToString` / `fallbackDropReasonFromString` pair, rule 1).
Its own `CMakeLists.txt` declares the module (`argus_module(NAME
notification-camera-notification …)`, rule 25) and the feature's include root,
so no consumer lists a `.cc` file.

`CameraObjectNotifier` no longer holds a notification client. It holds the
delivery dependencies `NotificationService` already takes (`deliverySink`,
`pushSink`, `pushRequired`) — and `NotificationRpcService::Dependencies` became
an alias of that same struct, so the notifier and the RPC service are wired the
same way — and calls
`NotificationService::createManyAndEmit` **in process**, with a `commandId`
derived from the event:

- a camera event carries its own `eventId`, so the command id is
  `camera:<eventId>` and a redelivered event is a `duplicate` — a `LOG_INFO`,
  never a second row. An event without one falls back to
  `camera:<cameraId>:event:<ms>`;
- a digest is `camera-digest:<cameraId>:<flush ms>`, and `takeDigest` clears
  the state it summarised, so a flush with nothing new delivers nothing; a
  second digest minted in the same millisecond would carry the same id and be
  taken for a duplicate, which is the guard the events get too.

The gateway's gRPC route set no command id at all, so the same event delivered
twice wrote two rows; that is the behavioural gain, and it costs nothing when
the event is delivered once.

There was also a credential defect on the path that is now gone, and the move
is what closes it. `NotificationRpcService::CreateNotifications` authorizes
against the **guard** credential (`grpc.caller_guard`, label `argus-guard`),
while the gateway's notifier presented the credential from its own
`[notifications] credential` — which `scripts/lib/common.sh`'s
`fill_deploy_pair` pairs with `grpc.caller_gateway`, a *different* generated
secret. A provisioned deployment therefore answered the gateway's camera
delivery with `UNAUTHENTICATED`, the notifier logged a delivery warning, and the
event left no notification and no fallback row (drops are recorded before
delivery, not after a refused one). Nothing in the gateway's own suite could see
it: that suite injected a recording fake client. In-process delivery has no
credential to present and no wire to be refused on.

A reused id with a different fingerprint raises
`NotificationCommandConflict` inside `createManyAndEmit`; the delivery path
catches it as it catches every other delivery failure — a `LOG_WARN` with the
title and the reason, never a failure of the camera ingress loop.

### The fallback log is this service's third table

`database/schema.sql` gained `camera_fallback_event` (renamed from
`gateway_fallback_event`, carrying the five-reason CHECK constraint and the
`created_at DESC` index the retention sweep and the per-outage queries read),
and `DbService::client()` replaced `DbService::gatewayClient()` — the notifier
now writes its own service's rows through its own service's client. The
retention purge (`fallback_retention_days`, run from the same one-minute flush
tick) moved with it; `purgeFallbackLog` is best-effort, so a missing store
degrades to the counters as before.

`DbService::gatewayClient()` and `setGatewayClient()` are deleted from
`lib/sqlite` with the file-static client behind them. Unlike the identity,
camera and productivity clients they never fell back to the host's own
database — the gateway's fallback record was a *separate* file on purpose — and
the strip leaves `readOnlyClient` (still used, still without a fallback) and
the three fallback clients untouched. `packages/lib/sqlite/AGENTS.md` records
the difference.

### The inputs and the `/health` blocks move to main.cc

`main.cc` subscribes through `camera_notifier::subscribe(NatsBus&,
CameraObjectNotifier&)` on `argus.guard.v1.heartbeat` (filtered on
`service == "argus-guard"` and `enabled`) and `argus.camera.v1.object_detected`,
marshals both onto IOLoop 0, and registers the one-minute `flushDigests` tick —
the gateway wired exactly the same three things, minus the static notifier it
used to keep alive (the notifier is now a `shared_ptr` owned by `main`, and the
health block captures it by value). The camera subscription is an ephemeral
core-NATS consumer: an event published while the service is down is lost with
no replay, which is the gateway's behaviour and the reason the fallback record
exists at all.

The `nats` and `notifications_fallback` extras of `GET /health` moved to this
service unchanged (same keys, same counters). The gateway's health block is now
the bare envelope.

### What the gateway keeps

Everything else: the public listener, `RemoteGate`, the reverse proxy, the
camera stream relay and media socket, the sync client. `services/gateway/src/`
contains no `DbService` reference any more, its Drogon config no longer declares
a `db_clients` entry, and `[gateway] db` / `[gateway] schema` are no longer
read; the Dockerfile no longer creates the data directory and the deploy no
longer mounts it or the schema.

## Consequences recorded, not hidden

- **`argus-sync` presents the notification credential the gateway used to
  present.** `services/sync/src/feature/transport/infra/notification-sync-gateway.cc`
  reads `[notifications] credential` and calls `PullNotifications`, while
  `services/notification/src/feature/rpc/notification-rpc-service.cc` accepts
  that RPC only from a caller whose secret matches
  `[grpc] caller_gateway` (label `argus-gateway`), and
  `scripts/lib/common.sh`'s `fill_deploy_pair` still pairs the gateway side with
  it. Nothing provisions sync's key, so in a provisioned deployment the sync
  pull leg presents `CHANGE_ME_GATEWAY_NOTIFICATION` and is refused. This is a
  pre-existing gap (it dates from the sync cutover, not from this move) and it
  is **step 1c's work item**: the pair moves to `argus-deploy/config.sync.toml`,
  the caller label is renamed to the caller that actually calls, and the
  gateway's own `[notifications] credential` key goes with the gateway. This
  unit deliberately kept the deploy pair and the gateway key in place so the
  tree stays consistent until that rename lands in one change.
- **`[notifications] credential` stays in the gateway's two templates**, and
  this is the one dead key the strip leaves behind on purpose. Its reader (the
  gateway's notification client) died here, and rule 23 would ordinarily take
  the key with it — but the key is the anchor of `fill_deploy_pair`'s third
  pair, `replace_toml_value` re-adds a key its table does not hold, and that
  pair is also the only thing that mints `grpc.caller_gateway`'s secret.
  Removing the key and the pair line while the
  caller label still says `argus-gateway` would leave both sides holding the
  published placeholder `CHANGE_ME_GATEWAY_NOTIFICATION`, which
  `callerCredentialsFromPairs` accepts as a real secret: the notification RPC
  would then authenticate a string the repository ships. Dropping a key must
  not turn an authenticated surface into an unauthenticated one, so the key,
  the pair line and the label are one edit — 1c's.
- **Camera notifications no longer take a network hop.** The gateway's
  `[notifications] grpc_target` key (deploy template only), its `[nats]` block,
  its `[push]` block and the five `fallback_*` / `budget_*` policy keys are
  removed from both templates, because no line of the gateway reads them any
  more; the notification service is still reached over gRPC by `argus-sync`
  through the same surface as before. Removing a key is safe here only because
  the provisioning helpers guard every write with `toml_key_exists` — the one
  unguarded writer, `fill_deploy_pair`, touches only `[notifications]
  credential` on the gateway side, and that key stays.

## Review

Fresh adversarial reviewers read the finished unit against the tree, without
the implementation's own reasoning. Their reports came back as claims and were
re-verified against the code before anything changed; each finding below ended
as fixed, refuted with its evidence, or recorded for a later step.

Fixed here:

- **The gateway's dead `[storage]` block.** `argus-deploy/config.gateway.toml.example`
  still carried `[storage]` / `[storage.s3]` and `scripts/provision-host.sh`
  wrote the shared RustFS application credentials into it, while
  `services/gateway/src/` reads no `storage.` key and links no `lib::storage`.
  Both went; argus-identity, argus-camera and argus-guard are the three storage
  consumers. With the credential gone the gateway's
  `depends_on: rustfs-init` had nothing left to order against and is deleted
  from `argus-deploy/docker-compose.yml`.
- **The deploy `CONTEXT.md` boot-order paragraphs** (both occurrences) still
  gave the gateway a NatsBus, an `identity.target` leg, the 7036 camera pull
  and the identity.db creation other services waited for. Rewritten to name the
  services that actually gate on `nats: service_healthy`, with the camera sync
  pull attributed to argus-sync and identity.db to argus-identity. One reviewer
  called the "no retry" premise stale: it holds for the **initial** connect
  (`packages/lib/nats/src/nats/nats-bus.cc` retries nothing there and returns
  false), while a later disconnect re-attaches every subscription — so the
  reasoning stays, now naming only bus-carrying services.
- **The sync → notification credential gap is worse than "unprovisioned".**
  `fill_deploy_pair` mints argus-notification's `[grpc] caller_gateway` from the
  gateway's key, mints nothing for `argus-deploy/config.sync.toml`, and
  `PullNotifications` accepts only the minted secret — so a provisioned
  installation does not merely leave the edge on a placeholder, it is refused
  `UNAUTHENTICATED` (`notification-rpc-service.cc:216`). Recorded with that
  consequence in `docs/operations/configuration-keys.md` on both sides of the
  edge and left to 1c, which owns the pair.
- **`services/notification/AGENTS.md`** described the deploy template as
  carrying no `[identity]` block; it carries one since this unit.
- **`services/notification/CONTEXT.md`** said a reused command id raises
  `NotificationCommandConflict` without saying what the notifier does with it;
  the notifier has no caller to answer, so it logs a failed delivery.

Refuted:

- A finding cited `services/gateway/config.toml.example:90-98` for the storage
  block; the gateway's own template has no `[storage]` section at all — only the
  deploy template did, which is where the fix landed.
- A finding read `configuration-keys.md`'s notification rows as an inaccurate
  "each shared only with its single caller"; the row was accurate for the guard
  credential as it stood, and what was actually missing was the
  `grpc.caller_gateway` row, now added on both the deploy and the service
  template, each naming the RPC it authorizes.

## Verification

- `services/notification` — 37/37 ctest tests, **0 warnings**; the moved suite
  `camera-notifier-test` runs 18 cases / 86 assertions and passes (it grew by
  one case while moving: the command-id idempotency case, which the gateway's
  suite could not have — the gateway had no command id to test).
- `services/gateway` — 25/25 ctest tests (27 before, for two reasons: the moved
  suite's `add_test` went with it, and `notification-client-test` no longer
  builds here because the gateway no longer declares
  `argus::clients::notification` — the client's own test still runs in the
  `sync`, `notification` and `guard` projects, which do), **0 warnings**,
  `gateway-test.cc` unchanged in its 24
  cases except for the two edits the strip forced: the runtime-override case
  uses an inert `[test]` key — `ConfigService::setRuntimeString` writes a
  process-wide override map that `load()` does not clear, so a test that sets a
  *production* key pollutes every later `ProxyConfig::resolve()` in the same
  process — and the config-section case asserts `[remote] tunnel_port` instead
  of a NATS key the gateway no longer reads.
- `packages/lib/sqlite` — 2/2.
- `./scripts/check-comments.sh` — 1338 files, 0 comments.
- The full orchestrator (`./scripts/build-all.sh dev`), first run: 18/18
  projects, **444 test executions**, 0 first-party warnings, 0 errors;
  `check-comments` 1338 files / 0 comments; `check-deps` 79 declarations / 676
  edges / 0 forbidden / 0 cycles / 0 unresolved / 23 edges deferred to phase 3;
  `check-tidy` **failed** — 539 translation units / 2940 findings over 45 checks
  against the 2940 baseline, with two checks above theirs:
  `modernize-avoid-c-style-cast` 360 (baseline 359) and `modernize-use-nodiscard`
  726 (baseline 724). All three findings sit on lines this unit added, which was
  established by scanning the reconstructed deleted gateway files under the same
  two checks with a synthetic compile database: the old header carried the same
  five `nodiscard` findings and the old cc carried none of the cast findings, so
  the new digest assignment was the +1 and the moved test's two helper accessors
  the +2. Fixed by restoring the moved original's `digest["cameraId"] = cameraId;`
  (the `Json::Int64(cameraId)` spelling was introduced here) and marking the
  test's `path()` and `applySchema()` helpers `[[nodiscard]]`. The unit's other
  findings in the moved files are net-neutral against the baseline and stay, in
  line with the sibling repositories, which carry no `[[nodiscard]]` either.
- The full orchestrator, re-run after those fixes: **exit 0** — 18/18 projects,
  444 test executions, 0 failures, 0 first-party warnings and 0 errors;
  `check-comments` 1338 files / 0 comments; `check-deps` 79 declarations / 676
  edges / 0 forbidden / 0 cycles / 0 unresolved / 23 deferred; `check-tidy`
  539 TUs, 2937 findings over 45 checks against the 2940 baseline, 1 check below
  it, none risen — both checks that had risen are back at or under their
  baselines and the tree reads three findings lighter than the run that
  rejected it.

## Files

Moved (deleted from the gateway, created here):
`services/gateway/src/sync/camera-notifier.{hxx,cc}` →
`services/notification/src/feature/camera-notification/services/`;
`services/gateway/src/sync/fallback/{fallback-log-query.hxx,fallback-log-repository.{hxx,cc}}`
→ `.../repositories/camera-fallback-log/camera-fallback-log-{query.hxx,repository.{hxx,cc}}`;
`services/gateway/src/shared/vocabulary/fallback-drop-reason.hxx` →
`.../repositories/camera-fallback-log/fallback-drop-reason.hxx`;
`services/gateway/tests/camera-notifier-test.cc` →
`services/notification/tests/unit/camera-notifier-test.cc`;
`services/gateway/database/schema.sql` → the `camera_fallback_event` block of
`services/notification/database/schema.sql`.

Deleted: `DbService::gatewayClient()` / `setGatewayClient()` and the file-static
client behind them (`packages/lib/sqlite/src/sqlite/db-service.{hxx,cc}`).

Touched: `services/gateway/{CMakeLists.txt,src/main.cc,tests/gateway-test.cc,Dockerfile,Dockerfile.dockerignore,.gitignore,config.toml.example,AGENTS.md,CONTEXT.md}`,
`services/notification/{CMakeLists.txt,src/main.cc,config.toml.example,database/schema.sql,src/feature/rpc/notification-rpc-service.hxx,AGENTS.md,CONTEXT.md}`,
`packages/lib/sqlite/AGENTS.md`, `scripts/provision-host.sh`,
`argus-deploy/{docker-compose.yml,CONTEXT.md,config.gateway.toml.example,config.notification.toml.example}`,
`docs/architecture/{events-and-contracts,services-and-packages,wire-nats-subjects}.md`,
`docs/operations/{configuration-keys,deployment-docker,shadow-mode-runbook}.md`,
`packages/contracts/CONTEXT.md`, `AGENTS.md`.
