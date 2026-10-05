# argus_contracts_sync

The sync boundary's frozen wire values: the message types, the table names, the
two audit enums, the filter that pages them, the ten refusals, and the
change-payload vocabulary the producers and the transport share.

## What this is

A CONTRACT, not a service and not a library, and the largest fan-in of the
contract packages: 46 CMakeLists name `argus::contracts::sync` — 45 consumers
plus this package's own test links, and seven of the 45 are argus-sync's (its
six modules and its test tree) since sub-step 3a-1d (the newest of the others
is `packages/clients/sync`, which links it as the home of the frame its
control leg carries). Most of it is headers only, but this is the one contract
whose headers are not free of dependencies — `syncable.hxx`
declares virtuals returning `drogon::Task` and `Json::Value`, and the two change
sinks add `publishAudit` returning `drogon::Task<void>`, which is why the package
depends on Drogon. `sync-errors.hxx` is what pulls in `lib/errors`. The
declaration also carries `lib/text`, and since sub-step 3a-1a1 that edge is no
longer transitive only: `sync-change-test` includes `<text/json-util.hxx>` and
the two audit-event headers include `<text/json-diff.hxx>`. The include root is
`src/`, so a consumer writes `<sync/sync-operation.hxx>`.

The consumers are argus-sync — the engine's home since sub-step 3a-1d, the
package it used to live in having been deleted — `lib/auth` (whose role table
is built on `RolePermission` and `TableName`), `clients/llm` and
`clients/sync`, and the auth, camera, guard,
identity, llm (its `memory` feature among its features), notification,
productivity and sync services. A table named here is a table
some repository syncs. The change vocabulary's consumers are the producers
that hold a sink — camera, productivity, notification, identity and auth —
the memory feature, and argus-sync's fan-out that reads the payloads back;
sub-step 3a-1a1 moved it here out of `packages/socket`, whose transport is
now `services/sync`'s `SyncSocket`.

## Layout

- `src/sync/sync-operation.hxx` — `SyncOperation`, `InitialInfo = 0` through
  `CallCancel = 9`, with `syncOperationToString`/`FromString` over the
  ten spellings (`"initial_info"`, `"sync"`, `"sync_audit_log"`,
  `"sync_user_audit_log"`, `"add"`, `"delete"`, `"log"`,
  `"auth_context_changed"`, `"call_incoming"`, `"call_cancel"`). 16 files
  include it. `0`-`7` are the frozen list; `CallIncoming = 8` and
  `CallCancel = 9` (2026-10, "Argus calls you") are additive: argus-notification's
  call engine rings a user's devices with them through
  `SyncControlService.EmitToUser` (option `notification`, info
  `{callId, reason, summary, urgency, kind, …}` / `{callId, reason}`), and
  argus-sync relays them like any user emit — its fan-out never validates the
  operation's range, so an older sync passes them through unchanged
  (`services/notification/CONTEXT.md`, "Argus calls you").
  `Heartbeat = 11` (`"heartbeat"`, 2026-10, the dead man's switch) is
  additive as well: argus-sync answers `{type:"heartbeat"}` with it, sends one
  after `InitialInfo` and pushes one to a user's room when their presence
  changes (`services/sync/CONTEXT.md`, "Dead man's switch"); `10` is
  `ResponseUpdate`.
- `src/sync/table-name.hxx` — `TableName`, 24 enumerators `User = 0` through
  `Memory = 23`, with its round-trip helpers, `kLastTableName` for the sweeps
  that must not miss a new table, and a lookup map; 35 files, the
  second-most-included header of the contract packages after
  `<auth/user-role.hxx>`'s 36.
- `src/sync/role-permission.hxx` — `RolePermission` (`Read = 0`, `Create`,
  `Update`, `Delete`), the third element of the gate's `(role, table,
  permission)` triple and the requirement a tool descriptor declares. No
  round-trip helpers: it never crosses the wire, so nothing parses it back.
  It sits here, beside the table it needs, because the units that name it are
  three tiers apart -- `lib/auth` owns the role table, the tier-3 llm client
  declares tool descriptors, and `sync`, `camera` and `productivity` each gate
  a route by hand (Phase 2 step 4 moved it out of `lib/auth`).
- `src/sync/user-action.hxx` — `UserAction` (`Create = 0`, `Read`, `Update`,
  `Delete`) with `"create"`, `"read"`, `"update"`, `"delete"`; 7 files.
- `src/sync/audit-log-priority.hxx` — `AuditLogPriority` (`Low = 0`, `Medium`,
  `High`), the one vocabulary here with no helpers to round-trip; 7 files.
- `src/sync/sync-filter.hxx` — `SyncFilter` (the range pair `startTime`/
  `startId`, an optional `endTime`, and `userId` for the user-scoped tables)
  and `sync_query::buildSyncQuery`, which picks one of the five query shapes a
  repository passes in and fills its arguments, plus `sync_query::withUser` for
  the ownership predicate's placeholders; 9 files.
- `src/sync/sync-limits.hxx` — `SyncLimits::kMaxRows{"200"}`, the page size
  every bounded sync and audit query ends with; 4 files.
- `src/sync/syncable.hxx` — `Syncable`, the four virtuals (`find`,
  `findDeleted`, `findLast`, `findLastDeleted`) every synced repository
  implements; 19 files.
- `src/sync/audit-retention.hxx` — `audit_retention::kDefaultDays` (90), the
  window the audit tables are compacted at and the one value the app's
  "replica too old" path is derived from: a cursor older than what `sync` has
  compacted is refused with `ReplicaTooOld`, and `afterId = 0` stays the legal
  empty baseline. `sync` reads the window from `[sync] audit_retention_days`
  and falls back to this constant; 3 files.
- `src/sync/stream-retention.hxx` — `stream_retention`: the feed window the
  NATS change sinks and their readers build their JetStream config from —
  seven days in milliseconds, seconds and nanoseconds, the two-minute
  duplicate window, and the interval and retry cadence a settled-row purge
  runs at; 10 files.
- `src/sync/sync-errors.hxx` — the ten refusals: `UserAccountDisabled` 401,
  `MissingMessageType` and `UnknownMessageType` 400,
  `ReplicaTooOld` 409, `TooManyFrames` 429 (2026-10-05, additive: a socket
  that outruns its frame budget, `services/sync/CONTEXT.md`, "Audit of
  2026-10-05"), and the five `*Unavailable` answers at 503
  (`NotificationSyncUnavailable`, `CameraSyncUnavailable`,
  `ProductivitySyncUnavailable`, `IdentitySyncUnavailable`,
  `VoiceUnavailable`); 9 files.
- `src/sync/socket-emit-dto.hxx` — `SocketEmitDto`, the triple the transport
  sends: the `SyncOperation`, the `TableName` it is scoped to, and the row as
  `Json`, with the `toJson()` that was a `.cc` in `packages/socket` until
  sub-step 3a-1a1. It is inline here because a contract is an interface target
  and compiles no source of its own; 19 files include it.
- `src/sync/sync-change.hxx` — the `argus.<domain>.v1.change` payload contract:
  the `kind` triple every payload carries (`kind`, `audit`, `identity`), the
  four catalog-row envelope keys the catalog consumers read a row out of
  (`table`, `id`, `deleted`, `row`), the eight frozen routing keys (`users`,
  `action`, `emit`, `disconnect`, `replace_role_rooms`, `user`, `old_role`,
  `new_role`) and the four builders `emitPayload`, `userEmitPayload`,
  `disconnectPayload` and `roleRoomsPayload` over `RoleRoomChange`, which
  carries the role *names* because the two role keys travel as strings;
  14 files.
- `src/sync/module-emit.hxx` — `ModuleEmitInput`: the `TableName` a
  module-scoped emit is scoped to, the `SocketEmitDto` body, and the borrowed
  `DbClient*` of the unit of work the change must be recorded in. It moved out
  of the two sink headers so both name one input, and the pointer type is what
  forbids a copy of the transaction; 6 files.
- `src/sync/user-change-sink.hxx` — `AuditSink` (`publishAudit`) with
  `UserAuditInput`, and `UserChangeSink`, which derives from it and adds the
  one user-scoped emit (`emitUsers`, over `UserEmitInput` and the recipient
  list it carries), plus the two process-wide slots the services fill at boot:
  `user_change::productivitySink` (a `UserChangeSink`) and
  `user_change::notificationSink` (an `AuditSink`, because the notification
  domain publishes diffs and never emits rows); 17 files, the most-included
  sink header here.
- `src/sync/camera-change-sink.hxx` — the camera domain's pair: `CameraChangeSink`
  (`emitModule`, `publishAudit`), `ModuleAuditInput`, and the single
  `camera_change` slot `argus-camera` installs; 4 files.
- `src/sync/auth-change-sink.hxx` — the auth domain's sink: `AuthChangeSink`
  (`publishAction`) with `AuthActionPublishInput` (a `UserActionEvent` plus
  the borrowed `DbClient*` of the unit of work the action must be recorded
  in), and the one `auth_change` slot `argus-auth` installs through its
  `AuthActionSink`; 3 files.
- `src/sync/identity-change-sink.hxx` — the identity domain's five-call sink:
  `IdentityChangeSink` (`publishCatalog`, `emitModule`, `publishModuleAudit`,
  `publishUsersAudit`, `publishAction`) with `IdentityCatalogInput` and
  `ActionPublishInput`, and the one `identity_change` slot `argus-identity`
  installs; 8 files.
- `src/sync/user-audit-event.hxx` — `UserAuditEvent`, the row a user-scoped
  producer puts on the wire: record id, table, the `ChangesDiff`, the priority,
  the recipients and the timestamp, with `toJson`/`fromJson` over the
  `kind: "audit"` envelope; 7 files.
- `src/sync/module-audit-event.hxx` — the same shape for the module-scoped
  event (camera and identity produce it today), carrying one optional
  `create_user_id` where the user-scoped event carries a recipient list;
  11 files.
- `src/sync/user-action-event.hxx` — `UserActionEvent`, the action-journal row:
  the acting user, the record, the table, the `UserAction`, the before/after
  data and the request's ip address, with the snake_case `toJson`/`fromJson`
  the action subject carries (the identity and auth sinks publish them, the
  audit fan-out reads them back); 4 files.
- `src/sync/sync-control-sink.hxx` — `SyncControlSink`, the three synchronous
  room operations a service performs on the socket another service holds
  (`replaceRoleRooms`, `disconnectUser`, `emitToUser`), and the one
  `sync_control` slot `argus-sync` installs; 5 files.
- `src/sync/sync-forwarder.hxx` — `SyncFrameInput` (`conn`, `message`, `raw`),
  the `SocketFrameError` refusal the transport sends back, the
  `sendSocketFrameError` helper (inline, because a contract compiles no source)
  and `SyncForwarder`, the four-call side-channel interface (`onConnect`,
  `forwardText`, `forwardBinary`, `onClose`) a service implements when it
  carries frame types the sync tables do not serve themselves. It moved here in
  sub-step 3a-1d from `packages/sync`, because more than one service renders its
  frames and rule 27 forbids a service including another service's source. Its
  one implementation is `services/sync`'s `VoiceGrpcRelay` (the voice leg
  installed through `SyncSocket::setForwarder`); `services/camera`'s media
  socket carries the same `SyncFrameInput` frames and the same
  `sendSocketFrameError` answer on its own `/media` route. 6 files.

## Rules

- Every spelling here is an app-visible value, not an internal name: the app
  switches on `"sync_user_audit_log"` and writes `"in_progress"` back. Renaming
  one is a contract break.
- `kMaxRows` is a wire invariant and not a tuning knob: the client pages by
  re-asking with a new cursor, so the digits are part of the frozen contract
  and are declared once so the SQL constants in the sync and audit repositories
  concatenate the same ones.
- The audit window is a contract value, not only a housekeeping one: `sync`
  compacts rows older than it into the nearest newer old row of the same key,
  so replay across the window converges on each record's current value, though
  on merged diffs rather than on each individual change. A client whose cursor
  is older than what has been compacted is refused with `ReplicaTooOld` and
  re-bootstraps with a full `Synchronize`; `afterId = 0` keeps establishing an
  empty baseline.
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
  `argus/sync/v1/contracts.proto` under `packages/contracts/proto/`, and it is
  compiled by exactly one CMakeLists: `packages/clients/sync`, which lists it
  beside its own `sync.proto` because the control schema imports it. That is
  what makes the frozen enums and `SyncFrame` reachable as C++ at all — a
  contract ships no generator of its own — and it is why the service that will
  serve these calls links the client package, the way `services/camera` links
  `clients/camera-actions` for `actions.proto`.
- Rule 25: the folder IS the module. One `argus_contracts(NAME sync ...)` with
  an explicit source list, never `file(GLOB)`.

## Tests

- `tests/unit/sync-contract-vocabulary-test.cc` — `TableName` and `UserAction`
  round-trips (the two the old enums-test covered), the `SyncOperation`
  round-trip with its numeric values pinned (the call operations made the case
  worth having), and the documented fallbacks; `AuditLogPriority` has none to
  make.
- `tests/unit/sync-contract-catalog-test.cc` — the ten refusals as a pinned
  table, each entry's wire legality, and that no two say the same thing.
- `tests/unit/audit-retention-test.cc` — the window's default and the refusal
  the app re-bootstraps on: 90 days, and `ReplicaTooOld` carrying the frozen
  `CONFLICT` wire code at 409.
- `tests/unit/stream-retention-test.cc` — the feed window pinned in every unit
  it is spelled in, the three spellings agreeing with each other, and the
  duplicate window and purge cadences holding their order; 2 cases,
  10 assertions.
- `tests/unit/sync-change-test.cc` — the change vocabulary, moved here from
  `packages/socket` with sub-step 3a-1a1: the emit triple's three keys and the
  absence of routing metadata on the module-wide form, the user-scoped variant
  with and without recipients, the two control actions on their frames, the
  round trip back to a `SocketEmitDto`, and the `argus.*.v1.change` subject the
  payloads travel on. 5 cases, 24 assertions.
