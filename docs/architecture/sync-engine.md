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
| 7 | `AuthContextChanged` | role/active context must refresh |

## Bootstrap versus updates

Normal rows are creation-only after bootstrap: `Synchronize` pages by
`created_at`. Every persisted update or revocation is published as a granular
audit diff through the change sink its domain declares, never as a full `Add`.

Audit requests use monotonic SQLite ids: `{findLast:true}` returns a watermark
id, then clients page `afterId < id <= endId` in ascending order. The fan-out's
daily coalesce folds a record's changes into one row per UTC day — the merged
row is written under a new id and the row it replaces is removed, so a
reconnecting client pages the merged value after the stale one and converges.

The trail is also compacted at a retention window
(`audit_retention::kDefaultDays` = 90 days, `[sync] audit_retention_days` in
`argus-sync`): a sweep folds each row older than the window into the nearest
newer old row of the same key, deletes the older row and advances a monotonic
frontier. The survivor carries the fold under an id above every row it
supersedes, so a replica that already paged past it is unaffected. A cursor
*older* than the frontier would in fact still reach each record's current
value — but on merged diffs rather than on each individual change — so the
audit legs refuse it with `SyncErrors::ReplicaTooOld` (409) as the window's
declared boundary and the app re-bootstraps with a full `Synchronize` instead
of reasoning about a folded history. `afterId = 0` stays the legal empty
baseline, so a fresh client still fills its history. The window and the refusal
are declared in `packages/contracts/sync`; the paging rules are in
[wire-sync-tables.md](wire-sync-tables.md).

## Rooms and fan-out

- `RoomManager` keeps module rooms (`1 + TableName`) and one user room per
  user id, with `thread_local` state.
- Services publish persisted changes to NATS on their own domain subject
  (`argus.<domain>.v1.change`); `argus-sync` holds one durable JetStream
  consumer per domain change stream and fans the events out.
- Camera sync tables are served by `argus-camera` through the typed
  `argus.camera.v1.SyncService.PullTable` gRPC contract; `argus-sync` applies
  role gating and maps each leg with the same cursor semantics.
