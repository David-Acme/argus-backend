# Data storage

Argus stores domain data in separate SQLite databases, one owner each. There
is no shared monolith database: `argus.db` is retired and must not appear.

## Owners and files

| Database | Owner | Schema | Runtime path |
|---|---|---|---|
| `identity.db` | `argus-identity` and `argus-sync` (the five sync tables) | `services/identity/database/schema.sql`, `services/sync/database/schema.sql` | `database/identity.db` |
| `auth.db` | `argus-auth` | `services/auth/database/schema.sql` | `database/auth.db` |
| `camera.db` | `argus-camera` | `services/camera/database/schema.sql` | `database/camera.db` |
| `productivity.db` | `argus-productivity` | `services/productivity/database/schema.sql` | `database/productivity.db` |
| `notification.db` | `argus-notification` | `services/notification/database/schema.sql` | `database/notification.db` |
| `memory.db` | `argus-llm` (`packages/memory`) | `packages/memory/database/schema.sql` | `database/memory.db` |
| `guard.db` | `argus-guard` | `services/guard/database/schema.sql` | `database/guard.db` |

`guard.db` holds incidents, encounters and their transitions, assessments,
expected guests, the observation inbox, the action outbox, the
`encounter_closed` fan-out outbox, dead letters and the evidence retention
manifest. It has no migration CLI: `guard_schema::migrate()` applies the
additive schema in-process at boot.

`auth.db` holds `refresh_token` (the single-use rotated sessions),
`device_login_challenge` (the cross-device pairing handshake) and
`device_credential` (the per-device secret's SHA-256). It carries no identity
row: the user context behind a session is resolved through
`argus::clients::identity` and cached for `[auth] context_cache_seconds`. The
three tables move here from `identity.db` as a copy — same columns, same CHECK
constraints — in Phase 3b-2, when the `/auth` surface that writes them moves to
`argus-auth`.

Runtime paths are relative to each process working directory. In the Compose
stack the working directory is `/opt/argus`, so they resolve under
`/opt/argus/database`, bind-mounted from the gitignored `argus-deploy/data/`
on the host.

Each owner applies its schema at boot and owns a migration CLI
(`argus-migrate-identity`, `-camera`, `-productivity`, `-notification`) that
migrates legacy `argus.db` data when present. Migrations run from the owner
service images as opt-in Compose init profiles.

## Durable-delivery tables

Every JetStream durable consumer receipts messages in an inbox table owned by
the consuming database, so redeliveries settle without re-executing effects:

- `notification_command` + `notification_delivery` (`notification.db`):
  idempotent fan-out creates (SHA-256 fingerprint per `command_id`) and the
  pending/sent delivery intents the broker must acknowledge.
- `notification_delivery_inbox` (`identity.db`, written by `argus-sync`):
  per-delivery receipts (`received`/`dispatched`/`conflict`/
  `dead_lettered`) with the canonical payload fingerprint. A conflicting
  fingerprint for a known id is never dispatched; an unknown status fails
  closed to `dead_lettered`.
- `encounter_closed_inbox` (`memory.db`): per-encounter receipts with the
  same states, so each `encounter_closed` event captures exactly one memory
  episode.
- `object_event_outbox` (`camera.db`) and `guard_observation_inbox`
  (`guard.db`): the two sides of the camera→guard leg, keyed by `eventId`.
- `change_outbox` (`camera.db`, written by `argus-camera`): the camera
  domain's change-feed producer side. One row per transition — a camera or
  zone add, delete or audit — keyed by an id derived from the table, the
  record and the transition's own payload, so a redelivered event is a replay
  while a record that moves again, or returns to a state it already held, is
  its own row; a row is marked `sent` only on the JetStream PubAck.
- `change_outbox` (`notification.db`, written by `argus-notification`): the
  notification domain's producer side of the same shape, one row per
  mark-as-read transition, keyed the same way and settled the same way. The
  table carries the same name in every producer's database on purpose: it is
  the same thing in each, and an operator should find it under one name.
- `change_outbox` (`productivity.db`, written by `argus-productivity`): the
  productivity domain's producer side of the same shape, one row per
  transition — a project, task, member, calendar event or share row emit, or
  the audit diff of one — keyed and settled the same way.
- `change_outbox` (`identity.db`, written by `argus-identity`): the identity
  domain's producer side of the same shape, one row per transition — a
  person/user catalog row, a row emit or the audit diff of one. It is the one
  copy that publishes on **two** subjects (the catalog change subject and the
  action journal), so the row carries the `subject` it goes to rather than the
  drain inferring it from a payload that need not name its own kind — and its
  journal rows are the variant within the variant: an action addresses
  `identity-action:<id>`, the row's own position, because a portrait view
  changes no row and a content-derived id would merge two views of one
  portrait into a single audit row. Those rows leave `event_id` NULL, so
  settlement is a status-guarded compare-and-set over the row id instead.
- `guard_action_outbox` (`guard.db`): the guard→camera direction, keyed by the
  deterministic `commandId`. `guard_encounter_outbox` (`guard.db`) is the
  guard's own `encounter_closed` producer leg, keyed by `eventId`.

Settled rows are not kept for ever: every producer's `change_outbox`,
camera's `object_event_outbox` and guard's `guard_encounter_outbox` hold
their `sent` (and `overflow_dropped`) rows exactly as long as the stream
that carries them and delete what is older on a daily sweep, so no outbox
outgrows its feed. The window, the duplicate window and the sweep cadences
are declared once in `packages/contracts/sync/src/sync/stream-retention.hxx`
and read by every feed's stream creation — the change feeds, the delivery
feed and the guard's `ARGUS_GUARD` — and by every outbox sweep. Two
consequences are deliberate: a fingerprint row's replay guard lasts exactly
that window, so an identical transition re-derived and re-published after it
is a second audit row and a second emit rather than a replay; and a
producer's `change_outbox`/`object_event_outbox` gauges count the retained
window, not all history. `notification_delivery` and `guard_action_outbox`
are deliberately outside the policy: they are product records their own
features read back (a delivery intent, a command's outcome) and not feed
rows, so their lifetime follows the feature that reads them rather than the
stream that carries them.

Incident evidence binaries live in the private object store (RustFS over S3),
referenced by `guard_evidence` / `camera_evidence` retention manifests; only
the manifests are database rows.

## Access rules

- Every query lives in `src/shared/repositories/`; no ad-hoc SQL in features.
- `DbService` opens Drogon's async client and reapplies pragmas at every boot
  (WAL, `synchronous=NORMAL`, busy timeout, mmap, foreign keys).
- The sync engine reads identity rows from `identity.db` and other owner
  tables through typed contracts; see [sync-engine.md](sync-engine.md).
- `DbService::installExtensions()` registers `sqlite-vec` (vec0) after
  Drogon's first connection; `VecDb` owns the vector tables.

## Vector and full-text search

- `sqlite-vec` (vendored in `third_party/sqlite-vec`) provides vec0: face
  embeddings (`face_vec`, 128-dim cosine) and memory vectors (`memory_vec`,
  partitioned per user/person).
- FTS5 (`bm25`, `unicode61`, `trigram`) is enabled in the Conan `sqlite3`
  option and backs memory recall and search.
