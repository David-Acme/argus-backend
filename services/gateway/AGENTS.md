# argus-gateway — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for
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
5. **100% English** — code, identifiers, docs, commits.
6. **No comments** — none in code, of any kind (root rule 20); the "why"
   goes to CONTEXT.md.
7. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`,
   `LOG_FATAL`); no spdlog.
8. **Health safety** — `GET /health` must never fail or block on any
   downstream service; degraded dependencies degrade logs, not health.
9. **No direct audible/hardware paths** — the gateway never calls
   setAlarm/siren or camera action RPCs; audible intervention belongs to
   argus-guard's fleet-secret gated action surface.
10. **No std::future** — plain `std::thread` + join when parallelism is
    needed.
11. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in gateway
    code; third-party includes are SYSTEM.

## Layout

```
argus-gateway/
  CMakeLists.txt        add_subdirectory-compatible AND standalone buildable
  src/main.cc           config load, filter/proxy wiring, app run
  src/proxy/            the reverse proxy and its route config
  src/server/           the listener/remote-tunnel config and LAN gate
  src/sync/             the `/camera-stream` relay and its control leg
  tests/                doctest suites
  config.toml.example   the gateway's own route/TLS/remote keys
  CONTEXT.md            purpose, ownership, wiring decisions
```

## Build commands

```bash
# From the monorepo root
./scripts/build-all.sh dev --only gateway
```

Driving CMake by hand inside the folder means installing the root graph once
(`./scripts/build-all.sh dev --install-only`) and passing its toolchain; the
exact flag set is in `docs/operations/build-and-test.md` under "Working
inside one project".

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, no comments in code, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
