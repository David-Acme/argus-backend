# argus-memory — CONTEXT

## f8-b3: the service becomes a package (2026-09-08)

The user's ruling: the LLM's memory should not be a wire hop away — the
tool-calling loop needs the memory tools in process, and the memory
worker gets LlmService as a direct call. What died with the process:
`src/main.cc`, the `/memory/v1/*` HTTP surface and its controller, port
7033 with its compose service and config template, and the remote wire
adapter (`memory-remote` / `RemoteMemoryServiceAdapter`) that existed
for the retired legacy's Ruling BY cutover gate.

`catalog-replica.cc` is the one piece that assumes a running process (a
NATS subscriber with read-only identity/camera snapshot seeds), so it is
its own `memory-catalog` target: linking the memory package does not
drag cnats into a consumer that does not want it. The host that wants the
replica links `memory-catalog` explicitly.

The database key inverted: memory no longer rides the host's
`[database] file`; it owns `[memory] db_file` (fallback `[database] file`,
final default `database/memory.db`) and `memoryDbFile()` refuses the
retired argus.db outright.

## Why the memory capacity exists

F4-6 of the `migracion-microservicios` plan extracted the memory stack out
of the legacy monolith (Rulings BW-CA). `argus-memory` was a
capacity-only sibling service (like argus-vlm): the voice session never
talked to it; the CONSUMERS are the background workers (memory formation,
compaction, profiling, procedures) and, since f8-b3, the LLM's own tool
loop.

## What it owns

- **The memory stack** (shared-tree `MemoryService`, compiled into this
  binary via the PORTED pattern): `VecDb` (sqlite-vec), `SqliteGraph`
  (memory.db graph tables), `EntityResolver`, `GraphRecall`,
  `MemoryFormation`, extraction (NuExtract gguf + lexicon/tiered/temporal)
  and embeddings (onnxruntime + unigram tokenizer). Owned BY VALUE by the
  host (no singleton). The stack is THE capacity of the package: the
  host's boot gate fails loudly rather than running without memory. Feed
  handlers marshal onto the Drogon loop — cnats dispatcher threads must
  not block.
- **`memory.db`** with the memory tables VERBATIM from
  `database/schema.sql`, the `memory_vec` partitions from
  `database/memory-schema.sql` (boot-applied, idempotent), and the four
  catalog replicas (`catalog_person`/`catalog_camera`/`catalog_zone`/
  `catalog_stream`). The schema file is configurable
  (`[memory] schema_file`) so the shared queries and `VecDb` apply the same
  DDL the package ships.
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
- **The sqlite3_config ordering (Ruling BW)**: the read-only
  `[identity]`/`[camera]` snapshot clients must install BEFORE
  `loadConfigJson` — Drogon's first sqlite3 client creation performs
  `sqlite3_config(SQLITE_CONFIG_MULTITHREAD)` — while the memory stack's
  raw connections open only after the loop begins (the `deferStore`
  pattern). The host's wiring (f8-b4) must preserve this ordering.
- **The chat port (Ruling BZ)**: the production chat leg is
  `InProcessMemoryChat` — the `IMemoryChat` port over the host's
  in-process `LlmService`, no wire hop. `WireMemoryChat` survives
  header-only because the back-pressure suite exercises the port over
  the wire shape. Back-pressure is the bounded work queue
  (`[memory] queue_bound`, default 64): at the bound, non-extract jobs
  drop with a WARN and an extract job evicts the oldest non-extract job.
## What it did NOT change

- The mobile app never talks to the memory capacity; no gateway routing,
  no app-facing contract.
- No migrations: `memory-schema.sql` is additive and applied idempotently
  at boot; `argus.db` is never touched.
- Models stay in the shared `models/memory/` tree — never copied.
- The gateway still owns the `/user` fan-out natively; identity change
  events are only the catalog-replica feed.

## The host's mounts (f8-b4)

The compose mounts the shared `models/` subpaths (`models/memory` +
`models/extract`) and the memory database into the HOST's working
directory — the ONNX/GGUF artifacts and `memory.db` are read relative to
the config keys.
