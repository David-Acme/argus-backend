# NATS subject contracts (v1, frozen)

The NATS event bus carries sync-change events from every Argus service (legacy
or microservice) to the gateway, which fans them out to `/sync` WebSocket
clients. Subject naming follows the package convention of the protobuf
contracts (`argus.<domain>.v1`).

## Naming convention

```
argus.<domain>.v1.<event>
```

- Segments are lowercase, separated by `.`; event names use snake_case.
- The `<domain>` segment matches the protobuf domain packages
  (`common`, `camera`, `ai`, `productivity`, `notification`, `memory`, `sync`,
  `gateway`).
- Subjects are frozen once published: new event names may be added under the
  same domain, but an existing subject never changes meaning or payload shape.
- Wildcards (`*`, `>`) are for subscriptions only, never for publishing.

## Concrete subjects required by `/sync` fan-out

| Subject                | Publisher            | Consumer | Purpose                                      |
|------------------------|----------------------|----------|----------------------------------------------|
| `argus.sync.v1.change` | every mutating service | gateway  | a persisted change that must reach `/sync` |
| `argus.camera.v1.change` | argus-camera (F2-2) | gateway  | a camera-domain persisted change (same payload as `argus.sync.v1.change`) |
| `argus.camera.v1.object_detected` | argus-camera (F2-3) | argus-guard (durable JetStream), gateway (degraded fallback) | an immutable per-object observation (schemaVersion 3: track/observation ids, identity tri-state, score history, evidence binding); never re-emitted to `/sync` |
| `argus.guard.v1.heartbeat` | argus-guard | gateway | readiness heartbeat; while fresh the gateway's raw camera notifier yields to guard |
| `argus.guard.v1.encounter_closed` | argus-guard | argus-llm (durable JetStream) | finalized, redacted encounter summary; the only camera feed long-term memory reads. Published with `Nats-Msg-Id = <eventId>` on the guard-owned stream `ARGUS_GUARD` (7 days, file storage, 2-minute duplicate window); argus-llm receipts each event in `encounter_closed_inbox` and captures exactly one memory episode per receipt |
| `argus.productivity.v1.change` | argus-productivity (F3-2) | gateway  | a productivity-domain change: user-scoped emits plus `kind: audit` user_audit_log diffs the gateway persists before fanning the rows out |
| `argus.notification.v1.change` | argus-notification (F3-2) | gateway  | a notification-domain change: user-scoped emits plus the `kind: audit` markAsRead rows (same payload contract as the productivity subject); Add emits move to the durable delivery subject below when its sink is installed |
| `argus.notification.v1.delivery` | argus-notification | gateway (durable JetStream) | one event per pending delivery intent (`deliveryId`, `notificationId`, `userId`, row fields); published with `Nats-Msg-Id = notification-delivery:<deliveryId>` on the notification-owned stream `ARGUS_NOTIFICATION` (7 days, file storage, 2-minute duplicate window); an intent settles only on PubAck; the gateway receipts each delivery in `notification_delivery_inbox` and drops receipted redeliveries. Delivery guarantee is at-least-once, not exactly-once: a crash between socket dispatch and inbox settlement replays the dispatch on redelivery (one receipt row, possibly two socket emits). Same delivery id plus same canonical payload fingerprint is a replay and dispatches at most once per receipt; same id plus a different fingerprint is a conflict that is never dispatched; persistently failing dispatches dead-letter after a bounded attempt count with a broker Term. |
| `argus.identity.v1.change` | legacy backend (F4-6) | argus-memory | the memory catalog replica feed: person/user rows written by the identity surface; the gateway's wildcard subscription drops it (the gateway owns `/user` fan-out natively) |
| `argus.notification.v1.push_intent` | argus-notification / gateway (F5-5) | argus-relay | a notification push intent carried to the home client through the tunnel transport (not a persisted change; never re-emitted to `/sync`) |

The gateway subscribes with the wildcard `argus.*.v1.change` — universally
valid across nats-server versions, while a mid-subject `>` requires nats-server
2.10+ — so later domains can add their own `argus.<domain>.v1.change` subject
without a gateway change.

## Payload of `argus.sync.v1.change`

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
  `proto/argus/sync/v1/contracts.proto` and `src/shared/contracts/sync-operation.hxx`.
  Live events use `Add = 4` (full current row), `Delete = 5` (`id` +
  `deletedAt` only), `Log = 6` (field diff from `JsonDiff::createFlatDiff`)
  and `AuthContextChanged = 7`. The bootstrap operations 0-3 are never
  published to the bus; a client that missed events re-syncs via `/sync`.
- `option` — the `TableName` the change belongs to (the `SocketEmitDto` table
  name, e.g. `camera`, `notification`).
- `info` — the row/diff payload exactly as the gateway would emit it to
  WebSocket clients; the gateway does not transform it.

### Routing metadata (additive, F1-4)

The publisher adds routing keys the gateway consumes and never re-emits; the
`{operation, option, info}` triple of a plain emit stays byte-identical to the
`SocketEmitDto` the legacy `SocketService` emits on `/sync`. Room-control
events carry an `AuthContextChanged`/`user` triple as envelope metadata only —
the gateway performs the room action and re-emits nothing for it. Published by
the legacy backend only — the gateway is subscriber-only and must not publish
(it would double-deliver its own fan-out).

| Key        | Present on                | Meaning |
|------------|---------------------------|---------|
| `users`    | user-scoped emits         | user ids of the user rooms to emit to. An explicit (possibly empty) array means "user rooms only, never fall back to the module room of `option`". Absent means the module room of `option`. |
| `action`   | room-control events       | Absent (or `"emit"`) is a plain emit. `"disconnect"` closes the user's sockets and emits `info` as the context message. `"replace_role_rooms"` re-computes the module rooms of `user` (the triple is envelope metadata, never re-emitted). |
| `user`     | `disconnect`, `replace_role_rooms` | the user id the action applies to. |
| `old_role` / `new_role` | `replace_role_rooms` | `UserRole` string values (`owner`, `resident`, `guard`, `guest`). |

## Payload of `argus.camera.v1.object_detected` (schemaVersion 3)

Published by argus-camera's operator after `EventIntelligence` evaluates the
detections of one aggregation window. Unlike the change subjects it is not a
persisted change: argus-guard consumes it through a durable JetStream consumer
(explicit ack after commit, `Nats-Msg-Id` = `eventId` so the stream's duplicate
window suppresses redeliveries), while the gateway's `camera-notifier` keeps a
budgeted raw fallback that yields whenever a fresh `argus.guard.v1.heartbeat`
is present. It never reaches `/sync`. The subject is retained on the JetStream
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
  (the gateway and guard parsers read every key with a default).
- `eventId` — producer-unique id (`cameraId:publishedAtMs:sequence`); the guard
  inbox deduplicates by it and JetStream deduplicates redeliveries with it.
- `capturedAt` — first frame of the aggregation window, in ms.
- `rule` — one of the EventIntelligence rules: `known_person`,
  `person_in_alert_zone`, `person_in_monitor_zone`, `person_night`,
  `person_day`, `vehicle_arrival`, `vehicle_night` (or
  `presence_escalating` when presence repeats instead of a vehicle).
- `severity` — `critical`, `warning` or `info`; the gateway notification
  carries it verbatim in the body.
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
- The gateway tolerates unknown extra keys and unknown rule/severity values
  (it treats them as data, never as commands).

## Payload of `argus.guard.v1.heartbeat`

Published by argus-guard every `guard.heartbeat_s` (and immediately at boot).
While a heartbeat fresher than `notifications.guard_heartbeat_timeout_s`
(default 30 s) exists, the gateway's raw `camera-notifier` suppresses its own
notifications and lets guard own the incident. Without a fresh heartbeat the
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

The memory catalog replica feed (Ruling BX): the legacy publishes identity
writes through the `identity_change` sink (`NatsIdentityChangeSink`, installed
in `src/config/application.cc`) so argus-memory's `CatalogReplica` can keep
`catalog_person` current. The subject is consumed ONLY by argus-memory — the
gateway's `argus.*.v1.change` subscription matches it and explicitly drops it
(`argus-gateway/src/sync/camera-fan-out.cc`), never re-emitting it to `/sync`.

```json
{
  "kind": "identity",
  "table": "person",
  "id": 7,
  "deleted": false,
  "row": { "id": 7, "user_id": 42, "name": "Ana", "alias": "" }
}
```

- `kind` — always `"identity"` (argus-memory ignores events without it).
- `table` — the identity-domain row written: `"person"` (face-enrollment
  rows, `argus-identity/src/feature/api/auth/services/auth-service.cc`) or
  `"user"` (profile writes,
  `argus-identity/src/feature/api/user/services/user-feature-service.cc`). The
  replica consumes only `person` rows today; `user` rows are published for a
  future consumer and are intentionally ignored by argus-memory — a rename
  reaches the catalog only through the person row's own `name`. Other values
  are ignored by the replica.
- `id` — the row id; `deleted` marks a soft delete (the replica tombstones
  `catalog_person` via `deleted_at`; the camera subject handles its own rows
  on `argus.camera.v1.change`).
- `row` — the post-write snapshot; the replica upserts `user_id`, `name` and
  `alias` for person rows. Publication failure logs a WARN and is dropped
  (best-effort feed; the boot snapshot fill recovers the catalog).

## Payload of `argus.notification.v1.push_intent` (F5-5)

Published per notification row by the `push_intent` sink
(`NatsPushIntentSink`, installed behind `[push] enabled` — default off) when
the owner's `NotificationService::createManyAndEmit` creates rows. Like
`object_detected` it is NOT a persisted change: it mirrors an already-persisted
`notification` row and exists only to trigger a push. The gateway's wildcard
`argus.*.v1.change` subscription does not match it and it is never re-emitted
to `/sync`.

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
