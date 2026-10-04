# NATS subject contracts (v1, frozen)

The NATS event bus carries sync-change events from every Argus service (legacy
or microservice) to the sync service (`argus-sync`), which fans them out to
`/sync` WebSocket clients. Subject naming follows the package convention of the
protobuf contracts (`argus.<domain>.v1`).

## Naming convention

```
argus.<domain>.v1.<event>
```

- Segments are lowercase, separated by `.`; event names use snake_case.
- The `<domain>` segment names the owning domain — `auth`, `camera`,
  `guard`, `identity`, `notification`, `productivity`, `sync` — and matches
  the protobuf domain package (`packages/contracts/proto/argus/<domain>/v1/`)
  where one exists.
- Subjects are frozen once published: new event names may be added under the
  same domain, but an existing subject never changes meaning or payload shape.
- Wildcards (`*`, `>`) are for subscriptions only, never for publishing.

## Concrete subjects required by `/sync` fan-out

| Subject                | Publisher            | Consumer | Purpose                                      |
|------------------------|----------------------|----------|----------------------------------------------|
| `argus.sync.v1.change` | — (superseded) | — (none owed) | the pre-3a generic change subject: every domain publishes on its own `argus.<domain>.v1.change` now, so nothing publishes or consumes this one. The constant stays frozen in `nats-subject.hxx`, and the payload shape below is the shape every domain subject carries |
| `argus.camera.v1.change` | argus-camera (F2-2) | argus-sync (durable `argus-sync-camera`), argus-llm (durable `argus-llm-catalog-camera`) | a camera-domain persisted change (same payload as `argus.sync.v1.change`, plus the module emits); retained on the camera stream `ARGUS_CAMERA` (7 days, file storage, 2-minute duplicate window), which both durables drain |
| `argus.camera.v1.object_detected` | argus-camera (F2-3) | argus-guard (durable JetStream), argus-notification (degraded fallback) | an immutable per-object observation (schemaVersion 3: track/observation ids, identity tri-state, score history, evidence binding); never re-emitted to `/sync` |
| `argus.guard.v1.heartbeat` | argus-guard | argus-notification | readiness heartbeat; while fresh argus-notification's raw camera notifier yields to guard |
| `argus.guard.v1.encounter_closed` | argus-guard | argus-llm (durable JetStream) | finalized, redacted encounter summary; the only camera feed long-term memory reads. Published with `Nats-Msg-Id = <eventId>` on the guard-owned stream `ARGUS_GUARD` (7 days, file storage, 2-minute duplicate window); argus-llm receipts each event in `encounter_closed_inbox` and captures exactly one memory episode per receipt |
| `argus.productivity.v1.change` | argus-productivity (F3-2) | argus-sync (durable `argus-sync-productivity`) | a productivity-domain change: the user-scoped row emits (`SocketEmitDto` + `users`) plus the `kind: audit` user_audit_log diffs argus-sync persists before fanning the rows out. Both legs land in the productivity-owned `change_outbox` first and are published with `Nats-Msg-Id = productivity-change:<32 hex>`, a row settling only on PubAck; the subject is retained on the productivity change stream `ARGUS_PRODUCTIVITY_CHANGE` (7 days, file storage, 2-minute duplicate window), which is the sink's own stream — a stream carries one subject set |
| `argus.notification.v1.change` | argus-notification (F3-2) | argus-sync (durable `argus-sync-notification`) | a notification-domain change: the `kind: audit` markAsRead rows (same payload contract as the productivity subject) and nothing else — the domain's rows reach their users on the delivery subject below, so this sink is the audit-only `AuditSink`. The diffs land in the notification-owned `change_outbox` first and are published with `Nats-Msg-Id = notification-change:<32 hex>`, a row settling only on PubAck; the subject is retained on the notification change stream `ARGUS_NOTIFICATION_CHANGE` (7 days, file storage, 2-minute duplicate window), which is the sink's own stream and not the delivery one — a stream carries one subject set |
| `argus.notification.v1.delivery` | argus-notification | argus-sync (durable JetStream, F3-1c) | one event per pending delivery intent (`deliveryId`, `notificationId`, `userId`, row fields); published with `Nats-Msg-Id = notification-delivery:<deliveryId>` on the notification-owned stream `ARGUS_NOTIFICATION` (7 days, file storage, 2-minute duplicate window); an intent settles only on PubAck; argus-sync receipts each delivery in `notification_delivery_inbox` and drops receipted redeliveries. Delivery guarantee is at-least-once, not exactly-once: a crash between socket dispatch and inbox settlement replays the dispatch on redelivery (one receipt row, possibly two socket emits). Same delivery id plus same canonical payload fingerprint is a replay and dispatches at most once per receipt; same id plus a different fingerprint is a conflict that is never dispatched; persistently failing dispatches dead-letter after a bounded attempt count with a broker Term. |
| `argus.identity.v1.change` | identity domain (F4-6; its sink moved into the identity owner in F3-1c) | argus-llm (durable `argus-llm-catalog-identity`), argus-sync (durable `argus-sync-identity`), argus-auth (durable `argus-auth-identity`) | the memory catalog replica feed: person/user rows written by the identity surface; argus-sync's durable receives the same events and drops the catalog kind, because the identity surface's `/sync` frames arrive on the change vocabulary its sinks publish, never on this catalog feed; argus-auth's durable drops the cache entry behind a session verdict and revokes every session of a user that arrives disabled (Phase 3b-1) |
| `argus.identity.v1.user-action` | identity domain (F3-1c) | argus-sync (durable `argus-sync-identity-action`) | the action journal: one actor doing one thing to one record, whether or not the record changed; argus-sync inserts it verbatim into `user_action_log`, keyed by its `Nats-Msg-Id` — `identity-action:` plus 32 hex the producer mints at enqueue — so a redelivery is ignored and the key is unique without borrowing the journal row's id |
| `argus.auth.v1.user-action` | argus-auth | argus-sync (durable `argus-sync-auth-action`) | the auth action journal: the session acts one actor performs — the sign-in that writes a `User` session row and the sign-out that deletes it — published on the auth-owned stream `ARGUS_AUTH_CHANGE` and inserted verbatim into `user_action_log` beside the identity journal's rows, keyed by its `Nats-Msg-Id` — `auth-action:` plus 32 hex the producer mints at enqueue — so a redelivery is ignored |
| `argus.auth.v1.session` | argus-auth (2026-10) | argus-sync (durable `argus-sync-auth-session`, ordered) | session changes: a `disconnect_session` room-control event per revoked session (its `info` is the `sessionRevoked` frame the session's sockets receive before they close) and one `sessionsChanged` user emit per change of the user's session set. Queued in argus-auth's `change_outbox` inside the revocation's transaction and published with `Nats-Msg-Id = auth-session:<32 hex>` on the auth-owned stream `ARGUS_AUTH_SESSION` (7 days, file storage, 2-minute duplicate window) |
| `argus.notification.v1.push_intent` | argus-notification (F5-5) | argus-relay | a notification push intent carried to the home client through the tunnel transport (not a persisted change; never re-emitted to `/sync`) |

In F3-1c the gateway's sync fan-out moved to `argus-sync` — the F3-1c tags
above name the rows whose consumer changed with it — and in Phase 3d step 1 the
gateway's `camera-notifier` moved to `argus-notification`, which is why the
object_detected and heartbeat rows name that service as their consumer. Every
consumer of a change subject is a durable JetStream consumer: `argus-sync`
holds one per
change stream (`ARGUS_CAMERA`, `ARGUS_NOTIFICATION_CHANGE`,
`ARGUS_PRODUCTIVITY_CHANGE`, and `ARGUS_IDENTITY_CHANGE` twice — the change
subject and the action journal — plus `ARGUS_AUTH_CHANGE` for the auth
journal), argus-auth holds one on
`ARGUS_IDENTITY_CHANGE` (`argus-auth-identity`) and argus-llm holds one per
catalog stream (`ARGUS_CAMERA`, `ARGUS_IDENTITY_CHANGE`). A durable filter names
exactly one
subject inside one stream, so the wildcard `argus.*.v1.change` subscription —
which used to make a new domain's subject free for the sync service — has no
durable form and is gone. Adding a `argus.<domain>.v1.change` subject now adds
a feed to `services/sync`'s `change_feed::defaults()` and, when the catalog
reads it, to `services/llm`'s `catalog_feed::defaults()`; the feed tables
are pinned by `services/sync/tests/unit/change-feed-consumer-test.cc` and
`services/llm/tests/unit/memory-replica-test.cc`, and
`packages/lib/nats/tests/unit/nats-wrapper-test.cc` pins the stream name each
subject is retained on. What that buys is what the wildcard
could not give: a consumer that was down drains the backlog the producer's
stream still retains instead of losing it, and the memory replica catches up
on what changed while argus-llm was down instead of waiting for a snapshot
fill it never repeats (its durables attach after that one fill, which skips
any table a replay has already touched). The backlog
survives only because a durable is created ahead of the subscription and the
subscription then binds to it by name — the client library deletes a consumer
its own subscribe created as soon as that subscription is unsubscribed or
drained, durables included, which would reset the cursor to the stream head on
every shutdown and lose exactly the downtime window the durable exists for.
Created-then-bound, the consumer outlives the process and resumes at its
stored cursor; `packages/lib/nats/AGENTS.md` carries the rule,
`packages/lib/nats/tests/unit/nats-wrapper-test.cc` pins it at the bus (the
backlog survives an unsubscribe and a drain, a second binder is refused, a
changed deliver policy is refused) and
`services/sync/tests/unit/change-feed-live-test.cc` pins it through the sync
leg by detaching a consumer, publishing while it is away, and requiring the
change to be applied when it re-attaches.

## Payload of `argus.sync.v1.change`

The subject is superseded, but the shape below is not: every
`argus.<domain>.v1.change` subject carries exactly this payload.

The payload is a JSON object mirroring the existing `SocketEmitDto` used on
`/sync` (`{operation, option, info}`):

```json
{
  "operation": 4,
  "option": "camera",
  "info": { "id": 1, "created_at": 1735689600 }
}
```

- `operation` — `SyncOperation` numeric value, frozen 0-7 in
  `packages/contracts/proto/argus/sync/v1/contracts.proto` and
  `backend/packages/contracts/sync/src/sync/sync-operation.hxx`.
  Live events use `Add = 4` (full current row), `Delete = 5` (`id` +
  `deletedAt` only), `Log = 6` (field diff from `JsonDiff::createFlatDiff`)
  and `AuthContextChanged = 7`. The bootstrap operations 0-3 are never
  published to the bus; a client that missed events re-syncs via `/sync`.
- `option` — the `TableName` the change belongs to (the `SocketEmitDto` table
  name, e.g. `camera`, `notification`).
- `info` — the row/diff payload exactly as the fan-out emits it to WebSocket
  clients; `argus-sync` does not transform it.

### Routing metadata (additive, F1-4)

The publisher adds routing keys `argus-sync` consumes and never re-emits; the
`{operation, option, info}` triple of a plain emit stays byte-identical to the
`SocketEmitDto` the socket has always emitted on `/sync`. Room-control
events carry an `AuthContextChanged`/`user` triple as envelope metadata only —
`argus-sync` performs the room action and re-emits nothing for it. Published by
the domain services only — `argus-sync` is subscriber-only on this subject and
must not publish (it would double-deliver its own fan-out); its control RPC
feeds the same dispatcher in-process instead.

| Key        | Present on                | Meaning |
|------------|---------------------------|---------|
| `users`    | user-scoped emits         | user ids of the user rooms to emit to. An explicit (possibly empty) array means "user rooms only, never fall back to the module room of `option`". Absent means the module room of `option`. |
| `action`   | room-control events       | Absent (or `"emit"`) is a plain emit. `"disconnect"` closes the user's sockets and emits `info` as the context message. `"disconnect_session"` does the same for the sockets of one session only. `"replace_role_rooms"` re-computes the module rooms of `user` (the triple is envelope metadata, never re-emitted). |
| `user`     | `disconnect`, `disconnect_session`, `replace_role_rooms` | the user id the action applies to. |
| `session`  | `disconnect_session` | the session id (refresh-token family) whose sockets close; the user's other sockets stay. |
| `old_role` / `new_role` | `replace_role_rooms` | `UserRole` string values (`owner`, `resident`, `guard`, `guest`). |

## Payload of `argus.camera.v1.object_detected` (schemaVersion 3)

Published by argus-camera's operator after `EventIntelligence` evaluates the
detections of one aggregation window. Unlike the change subjects it is not a
persisted change: argus-guard consumes it through a durable JetStream consumer
(explicit ack after commit, `Nats-Msg-Id` = `eventId` so the stream's duplicate
window suppresses redeliveries), while argus-notification's `camera-notifier`
keeps a budgeted raw fallback that yields whenever a fresh
`argus.guard.v1.heartbeat` is present. It never reaches `/sync`. The subject is retained on the JetStream
stream `ARGUS_CAMERA` (7 days, file storage, 2-minute duplicate window)
together with `argus.camera.v1.change`.

```json
{
  "schemaVersion": 3,
  "eventId": "1:1735689600123:4",
  "cameraId": 1,
  "cameraName": "Front door",
  "rule": "person_in_alert_zone",
  "severity": "critical",
  "escalated": false,
  "knownPersonId": 7,
  "capturedAt": 1735689600000,
  "detectedAt": 1735689600123,
  "publishedAt": 1735689600123,
  "trackId": 4,
  "dwellMs": 4200,
  "frame": { "width": 1920, "height": 1080 },
  "objects": [
    {
      "class": "person",
      "confidence": 0.91,
      "bbox": { "x": 120, "y": 40, "w": 300, "h": 800 },
      "personId": 12,
      "identity": "unknown",
      "identityState": "unrecognized",
      "identifyAttempts": 2,
      "identityConfidence": 0.72,
      "scoreMedian": 0.88,
      "scoreSamples": 4,
      "zoneWindows": 3,
      "trackWindows": 4,
      "areaSpread": 1.2,
      "trackId": 4,
      "firstSeenMs": 1735689595000,
      "lastSeenMs": 1735689600120,
      "dwellMs": 4200,
      "observationId": "1:4:1735689595000",
      "zoneKind": "alert",
      "signature": "base64-lab-histogram"
    }
  ]
}
```

- `schemaVersion` — 3; v2 consumers that ignore unknown keys keep working
  (the argus-notification and guard parsers read every key with a default).
- `eventId` — producer-unique id (`cameraId:publishedAtMs:sequence`); the guard
  inbox deduplicates by it and JetStream deduplicates redeliveries with it.
- `capturedAt` — first frame of the aggregation window, in ms.
- `rule` — one of the EventIntelligence rules: `known_person`,
  `person_in_alert_zone`, `person_in_monitor_zone`, `person_night`,
  `person_day`, `vehicle_arrival`, `vehicle_night` (or
  `presence_escalating` when presence repeats instead of a vehicle).
- `severity` — `critical`, `warning` or `info`; argus-notification carries it
  verbatim in the body.
- `knownPersonId` — present when the `known_person` rule matched through the
  real identity matcher (`[identity].identify`).
- `objects[].trackId` / `firstSeenMs` / `lastSeenMs` / `dwellMs` — per-person
  track, kept separately for every physical person (no class dedup).
- `objects[].observationId` — `cameraId:trackId:firstSeenMs`; the guard asks
  the camera for exactly this crop and rejects a stale or unbound capture.
- `objects[].zoneKind` — `alert` or `monitor` when the object center sits in
  that zone; absent otherwise.
- `objects[].identityConfidence` — matcher confidence for the person id.
- `objects[].signature` — compact Lab histogram for cross-camera correlation;
  never persisted in guard incidents (redacted before evidence upload).
- `objects[].personId` / `objects[].identity` — unchanged from v2:
  `identity` is `known` (trusted person) or `unknown` (stranger or
  auto-enrolled candidate), present only when `personId > 0`. Absent when
  identity is disabled.
- `objects[].identityState` — `known`, `unrecognized` (a face was analysed
  and matched nobody) or `unobservable` (no analysable face was ever
  observed). Present on track-bound person objects even when `personId` is 0;
  absent when no matcher ran. Consumers map a missing key to `unrecognized`;
  an unknown string value must never read as `known`. A missing key additionally
  means identity was unavailable, which the belief engine scores at zero
  rather than as an unrecognized face.
- `objects[].identifyAttempts` — identification scans performed for this
  track; present alongside `identityState`.
- `objects[].scoreMedian` / `objects[].scoreSamples` — median detector
  confidence over the track's bounded window history and the sample count;
  present when the track was seen in at least one window.
- `objects[].zoneWindows` / `objects[].trackWindows` — windows the track was
  observed in a configured zone / in any window; present with the history.
- `objects[].areaSpread` — max/min box-area ratio over the same history (1.0
  means a perfectly stable box); present with the history.
- `objects` — every detection of the window that survived the rules; bbox is
  frame pixels, top-left origin.
- argus-notification tolerates unknown extra keys and unknown rule/severity
  values (it treats them as data, never as commands).

## Payload of `argus.guard.v1.heartbeat`

Published by argus-guard every `guard.heartbeat_s` (and immediately at boot).
While a heartbeat fresher than `notifications.guard_heartbeat_timeout_s`
(default 30 s) exists, argus-notification's raw `camera-notifier` suppresses
its own notifications and lets guard own the incident. Without a fresh
heartbeat the
fallback only forwards protected-zone hard signals (`critical` severity or
`person_in_alert_zone`) through its own sanity gate (minimum score median and
dwell from the v3 observation keys, matched-known suppression; absent keys
fail open).

```json
{ "service": "argus-guard", "enabled": true, "at": 1735689600 }
```

## Payload of `argus.guard.v1.encounter_closed`

Published by argus-guard when an encounter reaches its terminal summary (stale
sweep or a known resident closing the case). This is the only camera feed
long-term memory may ingest: a finalized, redacted summary — never raw
detections, transcripts or captions.

```json
{
  "eventId": "encounter:42:high:1735689600",
  "encounterId": 42,
  "cameraId": 1,
  "personId": 12,
  "grade": "high",
  "durationS": 18,
  "closedAt": 1735689600
}
```

Delivery guarantee is exactly-one capture per receipt, at-least-once across
a process crash inside the capture-settlement micro-window: same event id
plus same canonical payload fingerprint is a replay and captures at most
once; same id plus a different fingerprint is a conflict that is never
captured; persistently failing captures dead-letter after a bounded attempt
count with a broker Term.

## Payload of `argus.camera.v1.health`

Published by argus-camera's health monitor on a transition only (occlusion,
blur, moved camera, stream down). argus-guard consumes it as data; it never
reaches `/sync`.

```json
{
  "cameraId": 1,
  "cameraName": "Front door",
  "status": "blurred",
  "metrics": { "brightness": 118.2, "blur": 4.1, "sceneDiff": 0.62 },
  "detectedAt": 1735689600123
}
```

- `status` — `ok`, `dark`, `bright`, `blurred`, `moved` or `unreachable`.
- `metrics` — brightness mean, Laplacian-variance sharpness and normalized
  scene difference against the first reference frame.

## Payload of `argus.identity.v1.change` (F4-6)

The memory catalog replica feed (Ruling BX): the identity domain publishes its
writes through the `identity_change` sink (`NatsIdentityChangeSink`, its own
file since F3-1c:
`services/identity/src/feature/user/services/nats-identity-change-sink.cc`)
so argus-llm's `CatalogReplica` can keep `catalog_person` current. The
payload's catalog kind is argus-llm's alone — argus-sync's durable on the
same subject receives it and explicitly drops it
(`services/sync/src/feature/fanout/services/sync-fan-out.cc`), never
re-emitting it to `/sync`.

```json
{
  "kind": "identity",
  "table": "person",
  "id": 7,
  "deleted": false,
  "row": { "id": 7, "user_id": 42, "name": "Ana", "alias": "" }
}
```

- `kind` — always `"identity"` (argus-llm ignores events without it).
- `table` — the identity-domain row written: `"person"` (face-enrollment
  rows, `services/identity/src/feature/enrollment/services/enrollment-feature-service.cc`)
  or `"user"` (profile writes,
  `services/identity/src/feature/user/services/user-feature-service.cc`).
  The replica consumes only `person` rows today; `user` rows are published
  for a future consumer and are intentionally ignored by argus-llm — a
  rename reaches the catalog only through the person row's own `name`.
  Other values are ignored by the replica.
- `id` — the row id; `deleted` marks a soft delete (the replica tombstones
  `catalog_person` via `deleted_at`; the camera subject handles its own rows
  on `argus.camera.v1.change`).
- `row` — the post-write snapshot; the replica upserts `user_id`, `name` and
  `alias` for person rows. A publish the broker refuses no longer drops the
  change: the row stays in the identity-owned `change_outbox` and is retried,
  so this feed is best-effort only up to the enqueue, and the boot snapshot
  fill still recovers the catalog.

## Payload of `argus.notification.v1.push_intent` (F5-5)

Published per notification row by the `push_intent` sink
(`NatsPushIntentSink`, installed behind `[push] enabled` — default off) when
the owner's `NotificationService::createManyAndEmit` creates rows. Like
`object_detected` it is NOT a persisted change: it mirrors an already-persisted
`notification` row and exists only to trigger a push. No change stream
retains it and it is never re-emitted to `/sync`.

Consumer: `argus-relay` subscribes to this subject and forwards the payload as
a tunnel PUSH control frame to the home client's bounded in-memory intent
queue (Ruling CK: argus-notification is the publisher/policy owner, the tunnel
is the transport owner). The NATS leg is fire-and-forget and therefore
AT-MOST-ONCE: `NatsBus::publish` is a plain core-NATS publish — no JetStream,
no ack, no redelivery — so an intent published while the relay is disconnected
from NATS or mid-restart is silently lost. Both legs are best-effort with drop
accounting; an intent is an accelerator, never a delivery guarantee — the
persisted `notification` row is the source of truth and the final
device-delivery leg rides the existing `/sync` fan-out once the app re-syncs.
The queue never persists to disk (Ruling CL: no database in the tunnel).
Intents carry display information only: they never carry or trigger
alarm/siren semantics.

```json
{
  "userId": 42,
  "notificationId": 17,
  "type": "camera",
  "title": "Front door",
  "body": "Person detected",
  "createdAt": 1735689600123
}
```

- `userId` — the notification row's user id.
- `notificationId` — the persisted `notification` row id (lets the device
  correlate the intent against the `/sync` row).
- `type`, `title`, `body` — the notification row's display fields, verbatim.
- `createdAt` — the row's creation time as a millisecond Unix epoch.

Size bound: the tunnel PUSH frame cannot carry more than 256 KiB of payload,
while the NATS subscription accepts up to the server's maximum, so the relay
rejects intents that are empty or above 256 KiB at its queue ingress and
counts them as drops (`pushDropped`) instead of forwarding them.

## Payload of `argus.identity.v1.user-action` (F3-1c)

The identity domain's action journal: one actor doing one thing to one record,
whether or not the record changed. It is append-only, and `argus-sync` inserts
every event verbatim into `user_action_log` — the table's single writer, in the
service's own `sync.db`. The catalog feed keeps
`argus.identity.v1.change`; this subject carries only the journal.

```json
{
  "user_id": 42,
  "record_id": 7,
  "table_name": "user",
  "action": "update",
  "old_data": { "id": 7, "name": "Ana" },
  "new_data": { "id": 7, "name": "Ana Ruiz" },
  "ip_address": ""
}
```

- `user_id` — the actor the action is attributed to.
- `record_id` — the id of the record acted on.
- `table_name` — the row's table in its `TableName` spelling (`user` and
  `user_invitation` at the sites publishing today).
- `action` — `create`, `read`, `update` or `delete`
  (`packages/contracts/sync/src/sync/user-action.hxx`); a `read` journals the
  access itself, not a change.
- `old_data` / `new_data` — the before/after snapshots of the record; a `read`
  publishes an empty `old_data` and the event it recorded in `new_data` (a
  portrait view publishes `{"event": "portrait_preview", ...}`).
- `ip_address` — the actor's source address; the identity sites publish an
  empty string today.
- `UserActionEvent::fromJson`
  (`packages/contracts/sync/src/sync/user-action-event.hxx`) accepts an event
  only when `user_id`, `record_id`, `table_name` and `action` are present and
  typed; the two snapshot keys and the address default when absent.
