# Retiring the three writerless sync tables (D8)

Date: 2026-10-19. Status: stage 0 landed, stages 1–3 queued. Owner rule: no supported
client may break, and data removal is never the side effect of a feature build.

## What is retired, and why it cannot go at once

Three tables have no writer and no reader in the current app:

| Table | Left the app | Server answer today |
|---|---|---|
| `event` | frontend `5ee5a8f`, schema v10 | `emptyTableNode` (`services/sync/src/feature/transport/services/synchronized-service.cc`, `pullTable`) |
| `camera_stream` | frontend `32c8f32`, schema v11 | the camera sync RPC (`CameraSyncTable::CameraStream`) |
| `reminder_detail` | frontend `32c8f32`, schema v11 | the productivity sync RPC (`ReminderDetailRow`) |

The server answers a table only when the pull body carries its key
(`SynchronizedService::sync`, `kBodyFields`), so a current app costs nothing: it never
asks. An installed app of schema v10 or v11 still asks, and its sync loop expects a node
for every key it sent. Removing the rows would break exactly those installs, and the
server has no way today to tell which app version is on the other end: no pull body,
`InitialInfo` request or handshake carries a schema version.

## Stage 0 — measure (landed with this plan)

`noteWriterlessPull` logs one INFO line per process and per table the first time a pull
asks for one of the three ("a client still pulls the writerless table …"). A process
that has run for a week without the line has served no such client. Nothing else
changes.

## Stage 1 — the app says which schema it has

- The app sends `clientSchema` (its WatermelonDB schema version, an integer) in the
  `Synchronize` body, additive and optional, in the same release that next bumps the
  schema. The server reads it into `SynchronizedDto` and keeps it on the connection.
- No server behaviour depends on it yet. The value is logged with the stage 0 line, so
  the line names the schema that asked.

## Stage 2 — stop answering, per connection

- A connection whose `clientSchema` is at least 12 is never answered for the three
  keys, even if it sends them by mistake (the key is dropped, as an unreadable table is).
- A connection without `clientSchema` (v10, v11 and anything older) keeps today's
  answers. Nothing breaks.

## Stage 3 — remove the code, once no old client is supported

Precondition, an owner decision: the minimum supported app is a schema-12 build, read
from the stage 0 line staying silent for 30 days on the owner's installation.

- `SynchronizedService`: drop the three `kBodyFields` rows, the `TableName::Event`
  branch of `pullTable` and `emptyTableNode` if nothing else uses it; drop the three
  `SynchronizedDto` fields (`synchronized-dto.hxx`).
- The owner RPCs: `CameraSyncTable::CameraStream` and the productivity
  `ReminderDetailRow` leave the sync gateways; their proto messages keep their field
  numbers as `reserved`.
- The enumerators stay for ever: `TableName`'s values are on the wire and
  `sync-contract-vocabulary-test` pins every one of them.
- The owner tables (`camera_stream` in camera's database, `reminder_detail` in
  productivity's) are dropped by a boot step in the same shape as D7's
  `retireEncounterOutbox`, in a later release, never in the release that removes the
  code.

## A writer that comes back

F1 (Argus-side recording) could feed `camera_stream` and F6 `reminder_detail`. A
feature that does so brings the table back to the app as a new schema version, never by
reviving the dropped one, and it starts from stage 3's state, not from today's.

## Acceptance per stage

- Stage 0: the INFO line appears once per table per process; the pull answers are
  unchanged (the sync suites' golden frames).
- Stage 1: a body with and without `clientSchema` parses; the golden frames are
  unchanged.
- Stage 2: a test pulls the three keys with `clientSchema` 12 and gets no node for them,
  and with no `clientSchema` gets today's nodes.
- Stage 3: the full sync suite and the frozen-frame e2e pass with the rows gone, and the
  app's own sync tests pass against the new server.
