# argus_contracts_productivity

The productivity boundary's vocabulary: the reminder detail states, the share
access level, the membership refusal enum and the eight service refusals.

## What this is

A CONTRACT, not a service and not a library: one `argus_contracts`
declaration, an INTERFACE target with no translation unit. The include root is
`src/`, so a consumer writes `<productivity/share-access.hxx>` and links
`argus::contracts::productivity`. Two CMakeLists link it —
`services/productivity`, which owns the boundary, and `packages/sync`, because
a project, a task and a calendar event each cross the sync leg carrying the
same access level and detail state.

## Layout

- `src/productivity/productivity-errors.hxx` — the eight refusals
  (`ProjectNotFound`, `TaskNotFound`, `CalendarEventNotFound` and the rest),
  answered by the service instead of a message string so code, status and text
  cannot drift apart; 6 files include it.
- `src/productivity/reminder-detail-status.hxx` — `ReminderDetailStatus`
  (`Pending`, `InProgress`, `Done`, `Blocked`) with its round-trip pair
  (`"pending"`, `"in_progress"`, `"done"`, `"blocked"`); 3 files.
- `src/productivity/share-access.hxx` — `ShareAccess` (`View`, `Edit`), where
  `View` is the default and every unknown spelling falls back to it; 3 files.
- `src/productivity/membership-error.hxx` — `MembershipError` (`None`,
  `ParentNotFound`, `UserNotFound`, `UserNotAllowed`, `SelfShare`): why a
  share could not be granted. The controller maps each case to its own status,
  which is why this is an enum and not a catalog; 7 files.

## Rules

- The two round-trip string pairs are wire values: the database stores
  `"in_progress"` and `"edit"`, and the app sends them back. Renaming one is a
  contract break.
- `MembershipError` is the one vocabulary here with no status of its own. A
  refusal that already knows its HTTP status belongs in
  `productivity-errors.hxx`; an enum a controller has to translate belongs
  here.
- The wire schema is `argus/productivity/v1/*.proto` under
  `packages/contracts/proto/`, of which `packages/clients/productivity`
  compiles `sync.proto`; `calendar.proto` and `project.proto` are schema with
  no client compiling them today.
- Rule 25: the folder IS the module. One `argus_contracts(NAME productivity
  ...)` with an explicit source list, never `file(GLOB)`.

## Tests

- `tests/unit/productivity-contract-vocabulary-test.cc` — the enums'
  round-trips.
- `tests/unit/productivity-contract-catalog-test.cc` — the eight refusals as
  a pinned table, each entry's wire legality, and that no two say the same
  thing.
