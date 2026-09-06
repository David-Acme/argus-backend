# argus-notification — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root, next to `src/`) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to notification-service code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **Notification domain only** — this service runs the notification data
   domain. It must not compile or load any AI service registry (face, llm,
   vlm, tts, stt, vad stay in the legacy), no stream/media, no socket relay,
   and takes no labs.
2. **Single-owner database (Rulings AN/AR)** — this service alone owns
   `notification.db`; the gateway writes camera-notifier notifications
   through its read-write named client and keeps no notification substrate
   of its own.
3. **Parameter structs for 3+ params** — any function with 3+ parameters
   must take a struct (designated initializers, every member listed).
4. **Dependency injection** — services/filters hold dependencies as private
   members with `_` suffix; controllers hold instance members, never static
   methods.
5. **Smart pointers** — no raw owning pointers; `std::unique_ptr` with
   custom deleters for C handles; raw pointers only for non-owning access.
6. **File naming** — `.hxx` headers, `.cc` sources, hyphenated `*-test.cc`
   tests. No `.h`/`.cpp`.
7. **100% English** — code, comments, identifiers, docs, commits.
8. **Minimal comments** — small "what it does" comments only; project-level
   "why" goes to CONTEXT.md.
9. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`,
   `LOG_FATAL`); no spdlog.
10. **Health safety** — `GET /health` must never fail or block on any
    downstream service; degraded dependencies degrade logs, not health.
11. **Never trigger setAlarm/siren paths** — no code path here may ever arm
    the siren.
12. **No argus.db migrations** — this service owns `notification.db` only
    (`database/notification-schema.sql`); it never writes or migrates
    `argus.db`.
13. **Frozen contracts** — HTTP paths, the `{status, info, errors}` envelope,
    `SyncOperation` 0-7, `SYNC_LIMIT=200` and `TableName` 0-23 never change
    here; the mobile app must keep working unmodified.
14. **No std::future** — plain `std::thread` + join when parallelism is
    needed.
15. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's
    own code; third-party includes are SYSTEM.

## Layout

```
argus-notification/
  CMakeLists.txt        add_subdirectory-compatible AND standalone buildable
  conanfile.txt         Drogon + transitive needs (same versions as root)
  CMakePresets.json     dev preset, binaryDir build/dev inside the folder
  src/main.cc           config load, notification.db wiring, app run
  src/notification/     notification-domain config resolution
  src/controllers/      HTTP controllers (health; write-side feature surface
                        since this task)
  src/server/           internal listener resolution
  config.toml.example   notification-domain keys only ([server],
                        [notifications], [jwt], [device]; no AI keys, no
                        [identity])
  CONTEXT.md            purpose, ownership, wiring decisions
```

The write-side feature sources (controllers, services, DTOs) compile from the
shared tree into this executable only; the notification schema rides
`argus_sync` and the notification-token repository/service compile from the
shared tree.

## Build commands

```bash
# From the monorepo root (recommended)
cmake --build --preset notification

# Standalone
conan install . --output-folder=build/dev -s build_type=Debug --build=missing
cmake --preset dev
cmake --build --preset dev -j 8
```
