# argus-memory — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for every change in
this package. The MUST-FOLLOW rules below restate the ones that apply to
memory code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **Memory capacity only** — this package is the memory stack
   (llama.cpp for NuExtract, onnxruntime for embeddings, sqlite-vec) and
   nothing else (no sherpa/opencv/mtmd/tts/face/tapo/ncnn symbols; verified
   with `nm -C`, Ruling BW).
2. **No process, no listener** — the package compiles into its host
   (argus-llm, the brain); there is no `/memory/v1/*` wire, no JWT, no
   CORS, no gateway routing. The host's in-process call is the only
   surface.
3. **No database migrations** — the memory schema is applied at boot by
   the stack's schema runner; never hand-edit the schema of a live
   `memory.db`. Catalog replicas are filled by replay/snapshot, never
   by DDL.
4. **Database discipline** — the `[identity]`/`[camera]` source clients
   are READ-ONLY snapshots (`file:...?mode=ro`); scratch copies only for
   any seeding or test fixture. Never point the config at the real
   `identity.db`/`camera.db` outside acceptance runs
   that only read them, and never at the retired `argus.db`.
5. **Parameter structs for 3+ params** — any function with 3+ parameters
   must take a struct (designated initializers, every member listed).
6. **Dependency injection** — classes hold dependencies as private
   members with `_` suffix; the host owns the memory stack BY VALUE (no
   singleton).
7. **Smart pointers** — no raw owning pointers.
8. **File naming** — `.hxx` headers, `.cc` sources, hyphenated
   `*-test.cc` tests. No `.h`/`.cpp`.
9. **100% English** — code, comments, identifiers, docs, commits.
10. **Minimal comments** — small "what it does" comments only;
    project-level "why" goes to CONTEXT.md.
11. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`,
    `LOG_ERROR`, `LOG_FATAL`); no spdlog.
12. **No siren paths** — never trigger `setAlarm`/siren code paths.
13. **Models are never copied** — the ONNX/GGUF artifacts are read from
    the shared `models/memory/` tree through config keys.
14. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's
    own code; third-party includes are SYSTEM.

## Layout

```
argus-memory/
  CMakeLists.txt        memory-core + memory-catalog (add_subdirectory AND standalone)
  conanfile.txt         Drogon + tomlplusplus + nlohmann_json (same pins)
  CMakePresets.json     dev preset, binaryDir build/dev inside the folder
  src/memory/           wire DTOs (validation DSL) + catalog replica feed
  src/shared/           the memory stack, extraction, embeddings, repositories
  tests/unit/           replica + back-pressure suites
  config.toml.example   consumer-side [memory]/[extract]/[identity]/[camera]/[nats]
  CONTEXT.md            purpose, ownership, wiring decisions
```

## Build commands

```bash
# From the monorepo root
./scripts/build-all.sh dev --only argus-memory

# From packages/argus-memory
conan install . --output-folder=build/dev -s build_type=Debug --build=missing
cmake --preset dev
cmake --build --preset dev -j 8
ctest --test-dir build/dev --output-on-failure
```

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, comment discipline, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
