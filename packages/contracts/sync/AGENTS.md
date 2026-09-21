# argus_contracts_sync

The sync boundary's frozen wire values: the message types, the table names, the
two audit enums, the filter that pages them and the six refusals.

## What this is

A CONTRACT, not a service and not a library, and the largest fan-in of the ten:
13 CMakeLists link `argus::contracts::sync`. Most of it is headers only, but
this is the one contract whose headers are not free of dependencies —
`syncable.hxx` declares virtuals returning `drogon::Task` and `Json::Value`,
which is why the package depends on Drogon. `sync-errors.hxx` is what pulls in
`lib/errors`. The declaration also carries `lib/text`, which no header here
includes: a pre-existing transitive link edge, left in place because consumers
may be reaching `text` through it, and flagged in the step's report. The
include root is `src/`, so a consumer writes `<sync/sync-operation.hxx>`.

The consumers are the sync engine (`packages/sync`), the audit package, the
identity, memory, room, socket and `lib/auth` packages, `clients/llm`, and the
camera, gateway, llm, notification and productivity services. A table named
here is a table some repository syncs.

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
