# argus-gateway — CONTEXT

## Why the gateway exists

The Argus backend is being split from a single monolith into small services
(Fase 1 of the `migracion-microservicios` plan). The gateway is the first new
service of that split: a real Drogon application inside the monorepo (sibling
of `src/`) that progressively takes over the public HTTP surface while the
legacy backend keeps running untouched on its own listener.

## What it owns

- **Health**: `GET /health`, no auth, `{status: "ok", info: {service,
  uptimeSeconds}}`. Health never depends on NATS being reachable. The
  `{status: "ok"}` envelope is a scaffold-time placeholder taken from the
  task brief: F1-3 MUST align `/health` and every new gateway route to the
  standard `ApiResponse` envelope `{status: <int>, info, errors}`.
- **NATS event bus client**: connects to the shared event bus (`[nats]` in
  config) so later phases can fan sync-change events without touching the
  legacy backend. Connecting is optional: with no `nats.url` configured the
  gateway logs and continues.
- F1-3 will add the identity domain here; F1-4 will move the `/sync` listener
  ownership here. No domain routes, filters, or DB clients yet.

## What proxies to legacy

Nothing yet. During the migration the gateway will front the legacy backend
for the domains it already owns and proxy the remaining paths; the listener
port `7024` in `config.toml.example` is a placeholder matching the legacy
backend port until F1-4 settles ownership.

## Build wiring (decisions)

- The gateway is added from the root project with
  `add_subdirectory(argus-gateway EXCLUDE_FROM_ALL)`, so
  `cmake --build --preset dev` still builds ONLY the backend. Build it with
  the root build preset `cmake --build --preset gateway` (targets
  `argus-gateway` + `gateway-test`), or with `--target`.
- `argus_common` was moved into `src/shared/CMakeLists.txt` (same target,
  same sources, same flags) so both the root project and the gateway consume
  one definition without duplicating the source list.
- The gateway also builds standalone: it reuses `../src/shared` via
  `add_subdirectory` and its own `conanfile.txt` (Drogon 1.9.13, cnats
  3.13.0, tomlplusplus 3.3.0, doctest 2.4.12 — same versions as the root,
  same Drogon/sqlite3 options for Conan cache reuse). Its tests register into
  ctest only in the standalone tree, keeping the root ctest at its 7 backend
  suites.
- Dependency weight note: when linked from the root project the gateway
  inherits `argus_common`'s public ncnn dependency (for hardware-profile);
  standalone builds do not pull ncnn at all. The gateway itself needs only
  Drogon + argus_common.
- Canonical build shape: `cmake --build --preset gateway` (root tree) is the
  canonical way to produce deployable gateway binaries. The standalone tree
  compiles `argus_common` WITHOUT ncnn, so HardwareProfile-derived behavior
  (`CapabilityTier`, the vulkan fields, `deriveTier` in
  `src/shared/wrapper/hardware-profile/hardware-profile.cc`) differs between
  the two shapes; the first gateway consumer of HardwareProfile must know
  this and must not ship binaries from the standalone tree.
- Controller registration: gateway controllers MUST be declared as
  `HttpController<T, false>` and registered explicitly in `main.cc` with
  `app().registerController(std::make_shared<T>())` before `run()`. Drogon's
  automatic registration is dropped by the linker for controllers whose
  object code lives in a static library (`gateway-core`), which silently
  yields 404s; explicit registration avoids it.
- Run from a directory containing `config.toml` (copy
  `config.toml.example`); `config.toml` is gitignored.
