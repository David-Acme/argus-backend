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
