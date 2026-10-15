# Batch 3: durable delivery of sealed pushes

Status: design note, no code. Branch `track/call-durable-note`, cut from
`track/call-backend` at c3d8777a. Facts are verified at that commit.

## 1. Current path

Calls
- `CallEngine::pushRing` (`services/notification/src/feature/call/services/call-engine.cc:259-277`)
  does two things. It publishes a legacy `PushIntent` (type `call`, `:268`) on
  the push_intent sink. It also calls `sealedPush->ring(call)`.
- The sealed ring (`services/notification/src/feature/call/services/push-delivery.cc:97-123`)
  seals once per device and publishes each envelope through
  `NatsCallPushSink` (`.../call/infra/nats-call-push-sink.cc:28`). That calls
  `NatsBus::publish` (`packages/lib/nats/src/nats/nats-bus.cc:599`), which is
  core NATS. The ring counts successes and returns. Nothing is acknowledged, so
  a ring published while no tunnel listens is lost.
- TTL: `kCancelTtlSeconds = 60` (`push-delivery.cc:18`) for a cancel, otherwise
  the call's remaining seconds (`:97`).
- `NatsBus::publishWithMsgId` exists (`nats-bus.cc:645`) and is not used for
  push_delivery.

Tunnel
- `SealedPushBridge::subscribe` (`services/tunnel/src/link/push/sealed-push-bridge.cc:127`)
  is a core NATS subscription. `onDelivery` posts to the loop, `sendOnLoop`
  sends the sealed push on the link and records `deliveryId` and token in a
  ledger.
- PUSH_RESULT arrives at `services/tunnel/src/link/client/link-client.cc:527-540`
  and reaches `SealedPushBridge::onPushResult`, which publishes `push_result`
  on core NATS (`sealed-push-bridge.cc:160-165`).
- Logs: `services/tunnel/src/main-client.cc:82-95` prints "sealed pushes come
  from ... and results go to ..." or "sealed pushes stay off". The durable
  consumer replaces these lines.

Results
- Notification does consume `push_result`. It is a core subscription in
  `services/notification/src/app/main.cc:346-348`, handled by
  `PushOutcomeService::apply`
  (`services/notification/src/feature/notification/services/push-outcome/push-outcome-service.cc:23-37`),
  which acts only on InvalidToken and Unregistered and logs the other outcomes.
  The batch brief said nothing consumes it. That was wrong.

Alerts
- `delivery-page.cc:36-58` publishes an alert intent for each delivery the
  sink accepts (`:56`). The gate at `:10` checks only that a push sink exists
  and `userId > 0`. Today that means every notification delivery to a user,
  when `[push]` is enabled. No type gate exists.
- The sync heartbeat is a third producer of push_intent:
  `services/sync/src/feature/heartbeat/services/heartbeat-feed.cc:169-173`,
  timer-driven, wired in `services/sync/src/app/main.cc:317`.

push_intent consumers (checked)
- Only `argus-tunnel-relay`: `services/tunnel/src/main-relay.cc:70-79`
  subscribes when `[push]` is enabled, and `relay/tunnel-relay.cc:77-101`
  queues, drops when full, and drains to `mux_.sendPush`.
- The home tunnel client does not subscribe to push_intent. It has a frame
  handler, `link-client.cc:480-489`, and `main-client.cc:50` is a stub.
- Deploy default: `argus-deploy/config.relay.toml.example` sets `[push] enabled = false`.
- NATS permissions name push_intent in the notification, relay and sync
  entries of `packages/lib/nats/src/nats/nats-permissions.cc`.

## 2. Design (the rulings)

1. Calls and alerts both travel one path: `push_delivery`, sealed, written
   through `TransactionalOutbox` (`packages/lib/outbox`) and published to
   JetStream with a msg id. No second mechanism. Alerts use the existing
   contract, which already has `fcm-alert` and `apns-alert` vectors in
   `services/tunnel/tests/vectors/push-sealed.json`.
   `combinationAllowed` (`packages/contracts/push/src/push/push-envelope.hxx:89-95`)
   admits APNs with alert only, APNs VoIP with call only, and FCM with either.
2. The push_intent path is removed in the same change. Items to remove, after
   the consumer check above:
   - the relay subscription and push queue (`main-relay.cc:70-79`,
     `tunnel-relay.cc:77-101` and its members);
   - the `[push]` section of `config.relay.toml.example`;
   - the legacy `PushIntent` publishes in `call-engine.cc:259-277` and
     `delivery-page.cc:10-25, 56`;
   - `NatsPushIntentSink` (`packages/lib/nats/src/nats/nats-push-intent-sink.hxx`)
     and its permission entries, once nothing else uses them.
   The heartbeat producer is handled in section 5.
3. The home tunnel holds a durable consumer per home (`subscribeDurable`,
   `packages/lib/nats/src/nats/nats-bus.hxx:117`). It acks only after it has
   PUSH_RESULT from the relay, or after a definitive refusal (sealing refused,
   malformed payload, unknown provider). Max-deliver is small, 5, with backoff.
4. TTL: a message older than its TTL is acked and discarded without sending,
   with a counter and a log line. An alert is redelivered within its TTL.
5. Idempotence: the outbox writes the JetStream msg id from `deliveryId`, so a
   producer retry does not publish a second copy.
6. The ready/lost log line comes from the durable consumer's state. No new
   observer goes into `lib/nats`.

## 3. Consumer and ack model

Flow: domain transaction writes an outbox row, the relay thread publishes it
to JetStream with a msg id, the home's durable consumer delivers it,
`SealedPushBridge` sends PUSH_SEALED, the relay sends to FCM or APNs, PUSH_RESULT
comes back, the bridge publishes `push_result` (token invalidation in
notification), and only then is the message acked.

If the bridge crashes between PUSH_RESULT and the ack, the message is
redelivered and the device may get a second push (open question Q4).

## 4. TTL and drop policy

- Ring: TTL is the call's remaining seconds. A ring is never sent late. If its
  age reaches its TTL at delivery, it is acked and dropped.
- Cancel: TTL 60 seconds. A cancel that arrives in time must still reach the
  device, so it is not dropped while it is inside that window.
- Alert: its own TTL (the vectors use 3600 s). It is redelivered until the
  TTL passes.
- Age source: the sealed envelope has no clear timestamp (fields are
  deliveryId, provider, kind, priority, ttlSeconds, collapseId, token, enc,
  ciphertext). The age has to come from the JetStream message timestamp or from
  an expiry field in the NATS JSON, outside the sealed bytes (Q3).

## 5. Out of scope

- The push_intent consumer decision is closed: the path is removed. The sync
  heartbeat push still needs a decision (Q6).
- Per-type push policy. Today every user delivery pushes (section 1). A
  per-type preference is a product decision for later.
- iOS. APNs alerts and VoIP calls keep the rules of `combinationAllowed`. The
  iOS client is not covered here.
- Making `push_result` durable is not decided (Q5).

## 6. Tests (not written yet)

1. Tunnel down during publish: an alert published while the tunnel is down is
   delivered when it returns, within its TTL.
2. Step 4 leg: an alert reaches the emulator although the tunnel was down when
   it was published. This is the end-to-end test of the durable path.
3. A ring with an expired TTL is not sent, and it is acked.
4. Delivery without PUSH_RESULT is redelivered up to max-deliver, then stops
   with a log line.
5. Idempotence by msg id: republishing the same `deliveryId` leaves one message
   in the stream and one push.
6. Removal: no push_intent subject in the permissions or the relay; the
   `[push]` section is gone from the relay config; `check-deps`,
   `check-comments` and `check-routes` pass.
7. The ready/lost line is emitted from the durable consumer's state.

## 7. Open questions for backend-c6

- Q1. Which domain transaction writes the outbox row for a ring (the call
  row) and for an alert (the notification delivery row)?
- Q2. push_delivery carries no home id. Does each message name its home, or does
  each home consumer see every message and drop the tokens it does not own?
- Q3. Age source: JetStream message timestamp, or an expiry field in the NATS
  JSON?
- Q4. Duplicate risk: PUSH_SEALED is sent, PUSH_RESULT is lost, and the message
  is redelivered. Accept the second push, or dedupe in the tunnel ledger
  (persisted, keyed on `deliveryId`) or on the device (`deliveryId` or
  `collapseId`)?
- Q5. Should `push_result` stay core? If it is lost, the token invalidation in
  notification is lost with it.
- Q6. Sync heartbeat push (`heartbeat-feed.cc:169-173`): move it to the sealed
  path, keep it as a silent push, or drop it?
- Q7. The PUSH_INTENT frame capability (`services/tunnel/src/link/protocol/frames.hxx:71`,
  `link-client.cc:480-489`) is a wire change. Cloud's `RelayNode::onPushIntent`
  is not built. Coordinate the removal with the cloud side, or keep a
  capability bit for one release?
- Q8. Calls publish both push_intent (`call-engine.cc:268`) and push_delivery
  today. The relay forwards nothing yet, so no device gets two pushes now.
  Confirm that before the removal.
- Q9. Does a cancel supersede a ring still queued in the stream, or does the
  device handle that by `collapseId`?
