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
| `argus.camera.v1.object_detected` | argus-camera (F2-3) | gateway  | an evaluated detection event (not a persisted change; never re-emitted to `/sync`) |
| `argus.productivity.v1.change` | argus-productivity (F3-2) | gateway  | a productivity-domain change: user-scoped emits plus `kind: audit` user_audit_log diffs the gateway persists before fanning the rows out |
| `argus.notification.v1.change` | argus-notification (F3-2) | gateway  | a notification-domain change: user-scoped emits plus the `kind: audit` markAsRead rows (same payload contract as the productivity subject) |
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

## Payload of `argus.camera.v1.object_detected` (F2-3)

Published by argus-camera's operator after `EventIntelligence` evaluates the
detections of one aggregation window. Unlike the change subjects it is not a
persisted change: the gateway's `camera-notifier` consumes it, applies the
notification budget and turns it into `notification` rows — it never reaches
`/sync`. The subject is retained on the JetStream stream `ARGUS_CAMERA`
(7 days, file storage) together with `argus.camera.v1.change`; stream creation
is best-effort (core NATS publish works without it). That retention is
server-side only: the gateway's consumer is an ephemeral core-NATS subscriber,
so events published while the gateway is down are not replayed when it
restarts — the stream exists for later inspection, not for redelivery.

```json
{
  "cameraId": 1,
  "cameraName": "Front door",
  "rule": "person_in_alert_zone",
  "severity": "critical",
  "escalated": false,
  "knownPersonId": 7,
  "detectedAt": 1735689600123,
  "frame": { "width": 1920, "height": 1080 },
  "objects": [
    { "class": "person", "confidence": 0.91, "bbox": { "x": 120, "y": 40, "w": 300, "h": 800 } }
  ]
}
```

- `rule` — one of the EventIntelligence rules: `known_person`,
  `person_in_alert_zone`, `person_in_monitor_zone`, `person_night`,
  `person_day`, `vehicle_arrival`, `vehicle_night` (or
  `presence_escalating` when presence repeats instead of a vehicle).
- `severity` — `critical`, `warning` or `info`; the gateway notification
  carries it verbatim in the body.
- `knownPersonId` — present only when the `known_person` rule matched (Fase 4
  fills the real matcher; until then it never appears).
- `objects` — every detection of the window that survived the rules; bbox is
  frame pixels, top-left origin.
- The gateway tolerates unknown extra keys and unknown rule/severity values
  (it treats them as data, never as commands).

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
  rows, `src/feature/api/auth/services/auth-service.cc`) or `"user"` (profile
  writes, `src/feature/api/user/services/user-feature-service.cc`). The
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
the shared `NotificationService::createAndEmitMany` creates rows. Like
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
