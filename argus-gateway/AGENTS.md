# argus-gateway — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root, next to `src/`) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to gateway code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **Parameter structs for 3+ params** — any function with 3+ parameters
   must take a struct (designated initializers, every member listed).
2. **Dependency injection** — services/filters hold dependencies as private
   members with `_` suffix; controllers hold instance members, never static
   methods.
3. **Smart pointers** — no raw owning pointers; `std::unique_ptr` with
   custom deleters for C handles; raw pointers only for non-owning access.
4. **File naming** — `.hxx` headers, `.cc` sources, hyphenated
   `*-test.cc` tests. No `.h`/`.cpp`.
5. **100% English** — code, comments, identifiers, docs, commits.
6. **Minimal comments** — small "what it does" comments only; project-level
   "why" goes to CONTEXT.md.
7. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`,
   `LOG_FATAL`); no spdlog.
8. **Health safety** — `GET /health` must never fail or block on NATS or any
   downstream service; degraded dependencies degrade logs, not health.
9. **Never trigger setAlarm/siren paths.**
10. **No std::future** — plain `std::thread` + join when parallelism is
    needed.
11. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in gateway
    code; third-party includes are SYSTEM.

## Layout

```
argus-gateway/
  CMakeLists.txt        add_subdirectory-compatible AND standalone buildable
  conanfile.txt         Drogon + transitive needs (same versions as root)
  CMakePresets.json     dev preset, binaryDir build/dev inside the folder
  src/main.cc           config load, NatsBus connect attempt, app run
  src/controllers/      HTTP controllers (health today; identity domain in F1-3)
  tests/                doctest suites
  config.toml.example   minimal [gateway] + [nats] sections
  CONTEXT.md            purpose, ownership, wiring decisions
```

## Build commands

```bash
# From the monorepo root (recommended)
cmake --build --preset gateway -j 8

# Standalone
conan install . --output-folder=build/dev -s build_type=Debug --build=missing
cmake --preset dev
cmake --build --preset dev -j 8
ctest --preset dev
```