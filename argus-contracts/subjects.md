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

The gateway subscribes with the wildcard `argus.>.v1.change`, so later domains
can add their own `argus.<domain>.v1.change` subject without a gateway change.

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