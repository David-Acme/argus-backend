# Data storage

Argus stores domain data in separate SQLite databases, one owner each. There
is no shared monolith database: `argus.db` is retired and must not appear.

## Owners and files

| Database | Owner | Schema | Runtime path |
|---|---|---|---|
| `identity.db` | `argus-gateway` (`packages/argus-identity`) | `packages/argus-identity/database/schema.sql` | `database/identity.db` |
| `camera.db` | `argus-camera` | `services/argus-camera/database/schema.sql` | `database/camera.db` |
| `productivity.db` | `argus-productivity` | `services/argus-productivity/database/schema.sql` | `database/productivity.db` |
| `notification.db` | `argus-notification` | `services/argus-notification/database/schema.sql` | `database/notification.db` |
| `memory.db` | `argus-llm` (`packages/argus-memory`) | `packages/argus-memory/database/schema.sql` | `database/memory.db` |
| `guard.db` | `argus-guard` | `services/argus-guard/database/schema.sql` | `database/guard.db` |

`guard.db` holds incidents, encounters and their transitions, assessments,
expected guests, the observation inbox, the action outbox, the
`encounter_closed` fan-out outbox, dead letters and the evidence retention
manifest. It has no migration CLI: `guard_schema::migrate()` applies the
additive schema in-process at boot.

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
- `notification_delivery_inbox` (`identity.db` schema, written by the
  gateway): per-delivery receipts (`received`/`dispatched`/`conflict`/
  `dead_lettered`) with the canonical payload fingerprint. A conflicting
  fingerprint for a known id is never dispatched; an unknown status fails
  closed to `dead_lettered`.
- `encounter_closed_inbox` (`memory.db`): per-encounter receipts with the
  same states, so each `encounter_closed` event captures exactly one memory
  episode.
- `object_event_outbox` (`camera.db`) and `guard_observation_inbox` /
  `guard_action_outbox` / `guard_encounter_outbox` (`guard.db`): the producer
  and consumer sides of the camera→guard leg, keyed by `eventId` and by
  deterministic `commandId`.

Incident evidence binaries live in the private object store (RustFS over S3),
referenced by `guard_evidence` / `camera_evidence` retention manifests; only
the manifests are database rows.

## Access rules

- Every query lives in `src/shared/repositories/`; no ad-hoc SQL in features.
- `DbService` opens Drogon's async client and reapplies pragmas at every boot
  (WAL, `synchronous=NORMAL`, busy timeout, mmap, foreign keys).
- The sync engine reads identity rows through the gateway and other owner
  tables through typed contracts; see [sync-engine.md](sync-engine.md).
- `DbService::installExtensions()` registers `sqlite-vec` (vec0) after
  Drogon's first connection; `VecDb` owns the vector tables.

## Vector and full-text search

- `sqlite-vec` (vendored in `third_party/sqlite-vec`) provides vec0: face
  embeddings (`face_vec`, 128-dim cosine) and memory vectors (`memory_vec`,
  partitioned per user/person).
- FTS5 (`bm25`, `unicode61`, `trigram`) is enabled in the Conan `sqlite3`
  option and backs memory recall and search.
