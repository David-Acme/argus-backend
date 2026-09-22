# argus_contracts_sync

The sync boundary's frozen wire values: the message types, the table names, the
two audit enums, the filter that pages them, the six refusals, and the
change-payload vocabulary the producers and the transport share.

## What this is

A CONTRACT, not a service and not a library, and the largest fan-in of the ten:
14 CMakeLists name `argus::contracts::sync` — 13 consumers plus this package's
own test links. Most of it is headers only, but this is the one contract whose
headers are not free of dependencies — `syncable.hxx` declares virtuals
returning `drogon::Task` and `Json::Value`, and the two change sinks add
`publishAudit` returning `drogon::Task<void>`, which is why the package depends
on Drogon. `sync-errors.hxx` is what pulls in `lib/errors`. The declaration
also carries `lib/text`, and since sub-step 3a-1a1 that edge is no longer
transitive only: `sync-change-test` includes `<text/json-util.hxx>` and the two
audit-event headers include `<text/json-diff.hxx>`. The include root is `src/`,
so a consumer writes `<sync/sync-operation.hxx>`.

The consumers are the sync engine (`packages/sync`), the audit package, the
identity, memory, room, socket and `lib/auth` packages, `clients/llm`, and the
camera, gateway, llm, notification and productivity services. A table named
here is a table some repository syncs. The change vocabulary's consumers are the
producers that hold a sink — camera, productivity and notification — the
identity and memory packages, and the gateway's fan-out that reads the payloads
back; sub-step 3a-1a1 moved it here out of `packages/socket`, which keeps only
its transport.

## Layout

- `src/sync/sync-operation.hxx` — `SyncOperation`, `InitialInfo = 0` through
  `AuthContextChanged = 7`, with `syncOperationToString`/`FromString` over the
  eight spellings (`"initial_info"`, `"sync"`, `"sync_audit_log"`,
  `"sync_user_audit_log"`, `"add"`, `"delete"`, `"log"`,
  `"auth_context_changed"`). 12 files include it.
- `src/sync/table-name.hxx` — `TableName`, 24 enumerators `User = 0` through
  `Memory = 23`, with its round-trip helpers, `kLastTableName` for the sweeps
  that must not miss a new table, and a lookup map; 24 files, the
  second-most-included header of the ten after `<auth/user-role.hxx>`'s 26.
- `src/sync/role-permission.hxx` — `RolePermission` (`Read = 0`, `Create`,
  `Update`, `Delete`), the third element of the gate's `(role, table,
  permission)` triple and the requirement a tool descriptor declares. No
  round-trip helpers: it never crosses the wire, so nothing parses it back.
  It sits here, beside the table it needs, because the units that name it are
  three tiers apart -- `lib/auth` owns the role table, the tier-3 llm client
  declares tool descriptors, and `sync`, `camera` and `productivity` each gate
  a route by hand (Phase 2 step 4 moved it out of `lib/auth`).
- `src/sync/user-action.hxx` — `UserAction` (`Create = 0`, `Read`, `Update`,
  `Delete`) with `"create"`, `"read"`, `"update"`, `"delete"`; 3 files.
- `src/sync/audit-log-priority.hxx` — `AuditLogPriority` (`Low = 0`, `Medium`,
  `High`), the one vocabulary here with no helpers to round-trip; 7 files.
- `src/sync/sync-filter.hxx` — `SyncFilter` (the range pair `startTime`/
  `startId`, an optional `endTime`, and `userId` for the user-scoped tables)
  and `sync_query::buildSyncQuery`, which picks one of the five query shapes a
  repository passes in and fills its arguments, plus `sync_query::withUser` for
  the ownership predicate's placeholders; 7 files.
- `src/sync/sync-limits.hxx` — `SyncLimits::kMaxRows{"200"}`, the page size
  every bounded sync and audit query ends with; 3 files.
- `src/sync/syncable.hxx` — `Syncable`, the four virtuals (`find`,
  `findDeleted`, `findLast`, `findLastDeleted`) every synced repository
  implements; 17 files.
- `src/sync/sync-errors.hxx` — the six refusals: `UserAccountDisabled` 401,
  `MissingMessageType` and `UnknownMessageType` 400, and the three
  `*SyncUnavailable` answers at 503; 3 files.
- `src/sync/socket-emit-dto.hxx` — `SocketEmitDto`, the triple the transport
  sends: the `SyncOperation`, the `TableName` it is scoped to, and the row as
  `Json`, with the `toJson()` that was a `.cc` in `packages/socket` until
  sub-step 3a-1a1. It is inline here because a contract is an interface target
  and compiles no source of its own; 11 files include it.
- `src/sync/sync-change.hxx` — the `argus.<domain>.v1.change` payload contract:
  the eight frozen routing keys (`users`, `action`, `emit`, `disconnect`,
  `replace_role_rooms`, `user`, `old_role`, `new_role`) and the four builders
  `emitPayload`, `userEmitPayload`, `disconnectPayload` and `roleRoomsPayload`
  over `RoleRoomChange`, which carries the role *names* because the two role
  keys travel as strings; 8 files.
- `src/sync/user-change-sink.hxx` — `UserChangeSink` (`emitUser`, `emitUsers`,
  `publishAudit`) with `UserAuditInput`, plus the two process-wide slots the
  services fill at boot: `user_change::productivitySink` and
  `user_change::notificationSink`; 14 files, the third-most-included header
  here.
- `src/sync/camera-change-sink.hxx` — the camera domain's pair: `CameraChangeSink`
  (`emitModule`, `publishAudit`), `CameraAuditInput`, and the single
  `camera_change` slot `argus-camera` installs; 3 files.
- `src/sync/user-audit-event.hxx` — `UserAuditEvent`, the row a user-scoped
  producer puts on the wire: record id, table, the `ChangesDiff`, the priority,
  the recipients and the timestamp, with `toJson`/`fromJson` over the
  `kind: "audit"` envelope; 4 files.
- `src/sync/camera-audit-event.hxx` — the same shape for the camera domain,
  carrying one optional `create_user_id` where the user-scoped event carries a
  recipient list; 4 files.

## Rules

- Every spelling here is an app-visible value, not an internal name: the app
  switches on `"sync_user_audit_log"` and writes `"in_progress"` back. Renaming
  one is a contract break.
- `kMaxRows` is a wire invariant and not a tuning knob: the client pages by
  re-asking with a new cursor, so the digits are part of the frozen contract
  and are declared once so the SQL constants in the sync and audit repositories
  concatenate the same ones.
- An unknown spelling falls back rather than throwing — `"sync"` for an
  operation, `"user"` for a table, `"create"` for an action — because these
  parse values a newer client may have written. `userActionToString` reaches
  its fallback through `default:` instead of naming `Create`, the same shape
  `contracts/camera` and `contracts/productivity` use in one header each; the
  errors vocabulary's exhaustive-switch rule is the stricter convention, and
  the three files are flagged rather than fixed.
- The change feed's keys are app-visible vocabulary like the spellings above:
  the transport switches on `emit`/`disconnect`/`replace_role_rooms` and reads
  `users`, `user`, `old_role` and `new_role` off the payload, so the eight
  constants are declared once here and no producer spells one by hand.
- No `.proto` lives here. The wire schema is
  `argus/sync/v1/contracts.proto` under `packages/contracts/proto/`, and no
  CMakeLists compiles it today: the sync-capable clients each build their own
  domain's proto, and this one is schema with no generator.
- Rule 25: the folder IS the module. One `argus_contracts(NAME sync ...)` with
  an explicit source list, never `file(GLOB)`.

## Tests

- `tests/unit/sync-contract-vocabulary-test.cc` — `TableName` and `UserAction`
  round-trips (the two the old enums-test covered; `SyncOperation` has helpers
  but never had a round-trip case, and `AuditLogPriority` has none to make) and
  the documented fallbacks.
- `tests/unit/sync-contract-catalog-test.cc` — the six refusals as a pinned
  table, each entry's wire legality, and that no two say the same thing.
- `tests/unit/sync-change-test.cc` — the change vocabulary, moved here from
  `packages/socket` with sub-step 3a-1a1: the emit triple's three keys and the
  absence of routing metadata on the module-wide form, the user-scoped variant
  with and without recipients, the two control actions on their frames, the
  round trip back to a `SocketEmitDto`, and the `argus.*.v1.change` subject the
  payloads travel on. 5 cases, 25 assertions.
