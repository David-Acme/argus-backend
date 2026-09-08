# argus-camera — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root, next to `src/`) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to camera-service code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **Camera domain only** — this service runs the camera/zone data + media
   domain. It must not compile or load any AI service registry (face, llm,
   vlm, tts, stt, vad stay in the legacy until Fase 4) and takes no labs.
2. **Parameter structs for 3+ params** — any function with 3+ parameters
   must take a struct (designated initializers, every member listed).
3. **Dependency injection** — services/filters hold dependencies as private
   members with `_` suffix; controllers hold instance members, never static
   methods.
4. **Smart pointers** — no raw owning pointers; `std::unique_ptr` with
   custom deleters for C handles; raw pointers only for non-owning access.
5. **File naming** — `.hxx` headers, `.cc` sources, hyphenated
   `*-test.cc` tests. No `.h`/`.cpp`.
6. **100% English** — code, comments, identifiers, docs, commits.
7. **Minimal comments** — small "what it does" comments only; project-level
   "why" goes to CONTEXT.md.
8. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`,
   `LOG_FATAL`); no spdlog.
9. **Health safety** — `GET /health` must never fail or block on NATS, the
   cameras or any downstream service; degraded dependencies degrade logs,
   not health.
10. **Never trigger setAlarm/siren paths** — the operator is read-only
    toward hardware; the audible alarm test is the user's personal task, so
    no code path here may ever arm the siren.
11. **No argus.db migrations** — this service owns `camera.db` only
    (`argus-camera/database/schema.sql`); it never writes or migrates `argus.db`.
12. **Frozen contracts** — HTTP paths, the `{status, info, errors}`
    envelope, `SyncOperation` 0-7, `SYNC_LIMIT=200` and `TableName` 0-23
    never change here; the mobile app must keep working unmodified.
13. **No std::future** — plain `std::thread` + join when parallelism is
    needed.
14. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's
    own code; third-party includes are SYSTEM.

## Layout

```
argus-camera/
  CMakeLists.txt        add_subdirectory-compatible AND standalone buildable
  conanfile.txt         Drogon + transitive needs (same versions as root)
  CMakePresets.json     dev preset, binaryDir build/dev inside the folder
  src/main.cc           config load, camera.db wiring, app run
  src/camera/           camera-domain config resolution
  src/controllers/      HTTP controllers (health today; /camera*, /zone in F2-2)
  src/server/           internal listener resolution
  config.toml.example   camera-domain keys only ([server], [camera], [tapo],
                        [streaming]; no AI keys)
  CONTEXT.md            purpose, ownership, wiring decisions
```

## Build commands

```bash
# From the monorepo root (recommended)
cmake --build --preset dev --target argus-camera

# Standalone
conan install . --output-folder=build/dev -s build_type=Debug --build=missing
cmake --preset dev
cmake --build --preset dev -j 8
```

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, comment discipline, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
