# Sync wire contracts (frozen)

The WebSocket sync stream (`/sync`) and its gRPC successor share this wire format.
The values in `proto/argus/sync/v1/contracts.proto` are **frozen forever**:

- `SyncOperation` 0-7 (from `backend/src/shared/contracts/sync-operation.hxx`):

  | Value | Name | Meaning |
  |---|---|---|
  | 0 | `InitialInfo` | Current user info (on connect) |
  | 1 | `Synchronize` | Sync of created/deleted data (global + user level) |
  | 2 | `SynchronizeAuditLog` | Sync of GLOBAL atomic updates |
  | 3 | `SynchronizeUserAuditLog` | Sync of atomic updates AT USER LEVEL |
  | 4 | `Add` | Live event: entity/notification created |
  | 5 | `Delete` | Live event: entity deleted |
  | 6 | `Log` | Live event: audit log |
  | 7 | `AuthContextChanged` | Live event: role/active context must refresh |

- `TableName` 0-23 (from `backend/src/shared/enums.hxx`, backend HEAD `5970173`,
  includes `Memory = 23`): `User=0, UserInvitation=1, Person=2, PersonEvent=3,
  Event=4, Reminder=5, ReminderDetail=6, CalendarEvent=7, CalendarEventShare=8,
  Project=9, ProjectMember=10, ProjectTask=11, ContextNote=12, Camera=13,
  CameraStream=14, Zone=15, AuditLog=16, UserAuditLog=17, Notification=18,
  NotificationToken=19, UserActionLog=20, RefreshToken=21, FaceEmbedding=22,
  Memory=23`.

- `SYNC_LIMIT = 200` (from `backend/src/config/app-config.hxx`,
  `AppConfig::SYNC_LIMIT`): the page size every bounded sync/audit query uses.

Rules:

- New operations may only use numbers >= 8. Never renumber or reassign 0-7.
- Normal rows are creation-only after bootstrap: `Synchronize` pages by
  `created_at`; every persisted update publishes a granular audit change
  (`Log`), never a full-row replacement.
- Audit paging uses monotonic SQLite ids: `afterId < id <= endId` ascending,
  `afterId=0` is a valid empty baseline.
- Envelope: frames are `{operation, table(TableName), info}`; the HTTP/WS error
  envelope `{status, info, errors}` and its error codes are frozen in
  `proto/argus/common/v1/base.proto`.

## Fixtures

`fixtures/` holds golden frames recorded from the backend (`/sync` traffic):
one JSON file per scenario, used by backend and frontend tests to detect any
accidental wire drift. Record with the backend golden-frame recorder and commit
the exact bytes.