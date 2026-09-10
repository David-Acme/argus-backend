# Sync engine

The app keeps a local replica of its authorized data and converges through a
single WebSocket route.

## Transport

- Route `/sync`, with `DeviceFilter` and `JwtFilter`; device binding is
  enforced on every authenticated transport.
- Messages are `{type, payload}`; responses use
  `SocketEmitDto {operation, option?, info}`; errors are
  `{type:"<type>_error", status, error}`.

## Operations

| Value | Name | Purpose |
|---|---|---|
| 0 | `InitialInfo` | connection bootstrap (`id`, `role`, `isActive`) |
| 1 | `Synchronize` | authorized projection plus creations/deletions |
| 2 | `SynchronizeAuditLog` | global field diffs, role-filtered |
| 3 | `SynchronizeUserAuditLog` | recipient field diffs, filtered by `sub` |
| 4 | `Add` | live creation with the complete row |
| 5 | `Delete` | live deletion with `id` and `deletedAt` |
| 6 | `Log` | live audit diff |

## Bootstrap versus updates

Normal rows are creation-only after bootstrap: `Synchronize` pages by
`created_at`. Every persisted update or revocation is published as a granular
audit diff through `SyncAuditService`, never as a full `Add`.

Audit requests use monotonic SQLite ids: `{findLast:true}` returns a watermark
id, then clients page `afterId < id <= endId` in ascending order. Daily
compaction merges a record's diff and reinserts it with a new id so
reconnecting clients converge.

## Rooms and fan-out

- `RoomManager` keeps module rooms (`1 + TableName`) and one user room per
  user id, with `thread_local` state.
- Services publish persisted changes to NATS (`argus.sync.v1.change` and
  domain subjects); the gateway subscribes `argus.*.v1.change` and fans out.
- Camera sync tables are served by `argus-camera` through the typed
  `argus.camera.v1.SyncService.PullTable` gRPC contract; the gateway applies
  role gating and maps each leg with the same cursor semantics.
