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

Runtime paths are relative to each process working directory. In the Compose
stack the working directory is `/opt/argus`, so they resolve under
`/opt/argus/database`, bind-mounted from the gitignored `argus-deploy/data/`
on the host.

Each owner applies its schema at boot and owns a migration CLI
(`argus-migrate-identity`, `-camera`, `-productivity`, `-notification`) that
migrates legacy `argus.db` data when present. Migrations run from the owner
service images as opt-in Compose init profiles.

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
