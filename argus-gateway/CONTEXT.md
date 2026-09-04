# argus-gateway — CONTEXT

## Why the gateway exists

The Argus backend is being split from a single monolith into small services
(Fase 1 of the `migracion-microservicios` plan). The gateway is the first new
service of that split: a real Drogon application inside the monorepo (sibling
of `src/`) that progressively takes over the public HTTP surface while the
legacy backend keeps running untouched on its own listener.

## What it owns

- **Health**: `GET /health`, no auth, standard `ApiResponse` envelope
  `{status: 200 (int), info: {service, uptimeSeconds}, errors: null}`
  (F1-2 ruling; never depends on NATS being reachable).
- **Identity domain (F1-3)**: the identity HTTP surface (`/auth/*`,
  `/pairing`, `/invitation/*`, `/user`, `/portrait-preview/*`) served from
  the `argus_identity` static library — the exact sources the legacy backend
  links, so routes, filter chains (`DeviceFilter → ValidJsonFilter →
  JwtFilter → RoleFilter`) and DTO validation are identical by construction.
  Controllers are `HttpController<T, false>` registered explicitly in
  `src/identity/identity-registrar.cc`; the backend does the same explicit
  registration in `Application::run()` (static-lib auto-creation is dropped
  by the linker).
- **identity.db**: the identity domain reads ONE database resolved from
  `[identity] db` (default `database/identity.db`), injected both as the
  Drogon default client and as the `database.file` runtime override so
  `VecDb`/face-db write face embeddings there too. At boot the gateway
  applies F1-3a's `identity-schema.sql` (`[identity] schema`) — the 7
  identity tables plus the audit/portrait substrate the identity write paths
  touch (`audit_log`, `user_audit_log`, `user_action_log`, `user_portrait`,
  `stored_file`, `portrait_preview_capability`), all copied verbatim from
  `database/schema.sql` — and aborts if it fails; it never touches `argus.db`
  and never runs the backend migrations.
- **NATS event bus client**: connects to the shared event bus (`[nats]` in
  config) so later phases can fan sync-change events without touching the
  legacy backend. Connecting is optional: with no `nats.url` configured the
  gateway logs and continues.
- **FaceService**: config-gated on `[face] enabled` (absent section or
  `false` boots without face models). Enabled with models present it loads
  like the backend; with models missing it degrades to a warn and facial
  login stays disabled. It never touches alarm/siren paths (none exist in
  the identity surface).
- F1-4 will move the `/sync` listener ownership here.

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
  one definition without duplicating the source list. `argus_identity`
  (F1-3) follows the same pattern in `src/identity/CMakeLists.txt`.
- The gateway also builds standalone: it reuses `../src/shared`,
  `../src/identity`, `../third_party/sqlite-vec` and `../third_party/ncnn`
  via `add_subdirectory`, and its own `conanfile.txt` (Drogon 1.9.13, cnats
  3.13.0, tomlplusplus 3.3.0, doctest 2.4.12, jwt-cpp 0.7.2,
  nlohmann_json 3.11.3, mdns 1.4.3, opencv 4.13.0 — same versions as the
  root, same Drogon/sqlite3 options for Conan cache reuse; re-run
  `conan install` after pulling: the identity deps were added in F1-3).
  Its tests register into ctest only in the standalone tree, keeping the
  root ctest at its 7 backend suites.
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
