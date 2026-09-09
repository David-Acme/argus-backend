# argus-memory — CONTEXT

## Why the memory service exists

F4-6 of the `migracion-microservicios` plan extracts the memory stack out
of the legacy monolith (Rulings BW-CA). `argus-memory` is a capacity-only
sibling service (like argus-vlm): the legacy voice session never talks to
it; the CONSUMERS are the background workers (memory formation, compaction,
profiling, procedures). It mirrors the argus-llm (F4-5) scaffold.

## What it owns

- **The memory stack** (shared-tree `MemoryService`, compiled into this
  binary via the PORTED pattern): `VecDb` (sqlite-vec), `SqliteGraph`
  (memory.db graph tables), `EntityResolver`, `GraphRecall`,
  `MemoryFormation`, extraction (NuExtract gguf + lexicon/tiered/temporal)
  and embeddings (onnxruntime + unigram tokenizer). Owned BY VALUE by the
  controller (the adapter shape — no singleton). The stack is THE capacity
  of this service: boot fails loudly rather than serving 503s to the legacy
  workers. Feed handlers marshal onto the Drogon loop — cnats dispatcher
  threads must not block.
- **`memory.db`** with the memory tables VERBATIM from
  `database/schema.sql`, the `memory_vec` partitions from
  `database/memory-schema.sql` (boot-applied, idempotent), and the four
  catalog replicas (`catalog_person`/`catalog_camera`/`catalog_zone`/
  `catalog_stream`). The schema file is configurable
  (`[memory] schema_file`) so the shared queries and `VecDb` apply the same
  DDL this service ships.
- **The catalog replica feed (Ruling BX)**: `argus.identity.v1.change`
  (new subject; the legacy publishes person/user rows through the
  `identity_change` sink installed in application.cc and the gateway
  main.cc) plus `argus.camera.v1.change` replay camera/person changes into
  the replicas. Camera_stream rows are written via `/sync` and only ride
  `argus.*.v1.change`, so the replica also subscribes the sync wildcard
  filtered to `option == "camera_stream"`. On boot, replica tables still
  empty get ONE snapshot fill from the read-only `[identity]`/`[camera]`
  clients; populated tables are never re-seeded. The fill runs even when the
  change feed never connects (`CatalogReplica::seedSnapshot` static entry —
  main.cc calls it when NATS is absent or failed), because otherwise a no-NATS
  boot would serve an empty catalog forever. Feed handlers accept two event
  shapes: the camera audit diff (`kind == "audit"`, field-level changes for
  one row) and the plain SocketEmitDto change shape (`{operation, option,
  info}`). Person and camera deletes tombstone (the source tables'
  `deleted_at` predicate); zone and stream rows are physical deletes, since
  their gazetteer query has no `deleted_at` filter.
- **The face index stays in the legacy**: `[memory] create_face_vec = false`
  skips `face_vec` creation (`ConfigService::hasKey` + a dual-shape read,
  since `getString` cannot surface TOML booleans); the key absent keeps the
  pre-cutover legacy behavior (create it).
- **The internal wire (Ruling BY)**: `POST /memory/v1/remember`, `/recall`,
  `/forget`, `/procedure-run`, `/capture`, `/compact`,
  `/durable-transcript` — frozen `{status, info, errors}` envelope. Every
  tool-shaped endpoint dispatches through the controller's OWN
  `ToolRegistry` fed by `MemoryService::registerTools`, so the wire and the
  legacy tool loop cannot drift. Errors: 400 `BAD_REQUEST`, 422
  VALIDATION_ERROR (`fields` keyed by the DTO member names), 503
  `MEMORY_NOT_LOADED`, frozen 404/405.
- **The sqlite3_config ordering (Ruling BW)**: the read-only
  `[identity]`/`[camera]` snapshot clients install BEFORE
  `loadConfigJson` — Drogon's first sqlite3 client creation performs
  `sqlite3_config(SQLITE_CONFIG_MULTITHREAD)` — while the memory stack's
  raw connections open only on the beginning advice (`deferStore`
  pattern), exactly the legacy ordering.
- **The chat port (Ruling BZ)**: worker `chat()` rides `argus-llm`'s
  `POST /llm/v1/chat` through `WireMemoryChat` (header-only over
  `LlmHttpClient`). The port never polls a remote engine: `busy()` is
  in-process-only state (`InProcessMemoryChat` keeps the legacy
  `isBusy()` semantics for the legacy binary). Back-pressure here is the
  bounded work queue (`[memory] queue_bound`, default 64): at the bound,
  non-extract jobs drop with a WARN and an extract job evicts the oldest
  non-extract job instead of polling `isBusy()`.
- **The `[server]` listener** is internal-network only (loopback by
  default): the wire is never exposed through the gateway.

## The legacy side (Ruling BZ/CA)

The legacy keeps `InProcessMemoryChat` (the `IMemoryChat` port over the
in-process `LlmService` singleton) until the cutover commit sets
`memory.remote_url`; when set, `RemoteMemoryServiceAdapter` covers the
adapter's tool surface over the wire. The queue-depth gate replaces the
`isBusy()` polling loop the same way in both processes.

## What it did NOT change

- The app móvil never talks to this service; no gateway routing, no new
  app-facing contract.
- No migrations: `memory-schema.sql` is additive and applied idempotently
  at boot; `argus.db` is never touched.
- Models stay in the shared `models/memory/` tree — never copied.
- The gateway still owns the `/user` fan-out natively; identity change
  events are only the catalog-replica feed.

## Compose volume

The docker compose must mount the shared `models/` tree (at least
`models/memory`) and the `database/` directory into this service's working
directory — the ONNX/GGUF artifacts and `memory.db` are read relative to
the config keys.
