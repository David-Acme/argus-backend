# argus-productivity — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to productivity-service code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **Productivity domain only** — this service runs the calendar/project/
   reminder data domain. It must not compile or load any AI service registry
   (face, llm, vlm, tts, stt, vad stay in the legacy), no stream/media, no
   socket relay, and takes no labs.
2. **Parameter structs for 3+ params** — any function with 3+ parameters
   must take a struct (designated initializers, every member listed).
3. **Dependency injection** — services/filters hold dependencies as private
   members with `_` suffix; controllers hold instance members, never static
   methods.
4. **Smart pointers** — no raw owning pointers; `std::unique_ptr` with
   custom deleters for C handles; raw pointers only for non-owning access.
5. **File naming** — `.hxx` headers, `.cc` sources, hyphenated `*-test.cc`
   tests. No `.h`/`.cpp`.
6. **100% English** — code, identifiers, docs, commits.
7. **No comments** — none in code, of any kind (root rule 20); the "why"
   goes to CONTEXT.md.
8. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`,
   `LOG_FATAL`); no spdlog.
9. **Health safety** — `GET /health` must never fail or block on any
   downstream service; degraded dependencies degrade logs, not health.
10. **Never trigger setAlarm/siren paths** — no code path here may ever arm
    the siren.
11. **No argus.db migrations** — this service owns `productivity.db` only
    (`services/productivity/database/schema.sql`); it never writes or migrates
    `argus.db`.
12. **Frozen contracts** — HTTP paths, the `{status, info, errors}` envelope,
    `SyncOperation` 0-7, `SYNC_LIMIT=200` and `TableName` 0-23 never change
    here; the mobile app must keep working unmodified.
13. **No std::future** — plain `std::thread` + join when parallelism is
    needed.
14. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's
    own code; third-party includes are SYSTEM.

## Layout

```
argus-productivity/
  CMakeLists.txt        standalone buildable: module graph + test targets
  src/app/main.cc       config load, productivity.db wiring, app run
  src/app/rpc/          the gRPC listener (argus::productivity-rpc-server)
  src/config/           argus::productivity-config — productivity.db and the
                        listener
  src/feature/<feature>/  one vertical slice per resource: controllers/,
                        dtos/, services/ — the folder IS the module
  src/feature/reminder/ argus::productivity-reminder — the reminder routes and
                        the ReminderService RPC over one feature service
  src/feature/sync/     argus::productivity-sync — sync RPC + reminder detail rows
  src/shared/repositories/  rows 2+ features read
  src/shared/services/change-sink/  argus::productivity-change-sink — the
                        NATS change sink over `argus::lib::outbox`
  config.toml.example   the productivity roster ([server], [drogon.app],
                        [productivity], [cert], [jwt], [device], [identity],
                        [nats], [mdns]; no AI keys)
  CONTEXT.md            purpose, ownership, wiring decisions
```

Each feature and shared folder declares its own module through
`argus_module`; consumers link by name (`argus::productivity-<feature>`) and
never list `.cc` files. `main.cc` registers every controller explicitly — each
is a Drogon `HttpController<T, false>`, so no route depends on static-init
registration.

## Build commands

```bash
# From the monorepo root
./scripts/build-all.sh dev --only productivity
```

Driving CMake by hand inside the folder means installing the root graph once
(`./scripts/build-all.sh dev --install-only`) and passing its toolchain; the
exact flag set is in `docs/operations/build-and-test.md` under "Working
inside one project".

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, no comments in code, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
