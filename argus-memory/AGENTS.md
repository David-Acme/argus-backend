# argus-memory — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root, next to `src/`) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to memory-service code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **Memory capacity only** — this service runs the memory stack
   (llama.cpp for NuExtract, onnxruntime for embeddings, sqlite-vec) and
   nothing else (no sherpa/opencv/mtmd/tts/face/tapo/ncnn symbols; verified
   with `nm -C`, Ruling BW).
2. **Internal wire only** — the service serves the legacy workers over
   loopback plain HTTP (`/memory/v1/*`); no JWT, no CORS, no gateway
   routing. Never expose it publicly. No auth: the loopback bind is the
   trust boundary.
3. **Frozen envelope** — every JSON response uses the
   `{status, info, errors}` envelope (`ApiResponse`).
4. **No database migrations** — `database/memory-schema.sql` is applied
   at boot by the stack's schema runner; never hand-edit the schema of a
   live `memory.db`. Catalog replicas are filled by replay/snapshot, never
   by DDL.
5. **Database discipline** — the `[identity]`/`[camera]` source clients
   are READ-ONLY snapshots (`file:...?mode=ro`); scratch copies only for
   any seeding or test fixture. Never point this service's config at the
   real `database/argus.db`, `database/identity.db` or
   `database/camera.db` outside acceptance runs that only read them.
6. **Parameter structs for 3+ params** — any function with 3+ parameters
   must take a struct (designated initializers, every member listed).
7. **Dependency injection** — services/controllers hold dependencies as
   private members with `_` suffix; the controller owns the memory stack
   BY VALUE (the adapter shape — no singleton).
8. **Smart pointers** — no raw owning pointers.
9. **File naming** — `.hxx` headers, `.cc` sources, hyphenated
   `*-test.cc` tests. No `.h`/`.cpp`.
10. **100% English** — code, comments, identifiers, docs, commits.
11. **Minimal comments** — small "what it does" comments only;
    project-level "why" goes to CONTEXT.md.
12. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`,
    `LOG_ERROR`, `LOG_FATAL`); no spdlog.
13. **No siren paths** — never trigger `setAlarm`/siren code paths.
14. **Models are never copied** — the ONNX/GGUF artifacts are read from
    the shared `models/memory/` tree through config keys.
15. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's
    own code; third-party includes are SYSTEM.

## Layout

```
argus-memory/
  CMakeLists.txt        add_subdirectory-compatible AND standalone buildable
  conanfile.txt         Drogon + tomlplusplus + nlohmann_json (same pins)
  CMakePresets.json     dev preset, binaryDir build/dev inside the folder
  src/main.cc           config load, sqlite3_config ordering, stack boot gate
  src/controllers/      HTTP controllers (health + /memory/v1/* wire)
  src/memory/           wire DTOs (validation DSL) + catalog replica feed
  src/server/           internal listener resolution
  config.toml.example   [memory]/[extract]/[llm]/[identity]/[camera]/[nats]
  CONTEXT.md            purpose, ownership, wiring decisions
```

## Build commands

```bash
# From the monorepo root (recommended)
cmake --build --preset memory --target argus-memory

# Standalone
conan install . --output-folder=build/dev -s build_type=Debug --build=missing
cmake --preset dev
cmake --build --preset dev -j 8
```

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, comment discipline, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
