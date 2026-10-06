# argus-settings — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for every change in
this service. The MUST-FOLLOW rules below restate the ones that apply.

## MUST-FOLLOW Rules

1. **No setting of its own** — every setting lives in its owner's
   `config.toml`, validated by the owner's `SettingsRegistry`; this service
   only reads catalogs and forwards changes over `argus.settings.v1` through
   `argus::clients::settings`. Its one database, `settings.db`
   (`database/schema.sql`), holds the module manager's state only
   (`module_state`, `module_job`, `module_purge`, `module_audit` and the
   `module_journal` cursor of the action journal; CONTEXT.md, "Modules"), and
   it reaches another service's models or data only through
   that service's settings wire. `modules.json` is the module catalog,
   validated at boot (an invalid one turns every `/modules` route into 503).
   The other file it reads is `profiles.json` (`settings.profiles_path`), at boot:
   profiles are data, never code, and a profile names only keys that trade
   speed against quality on measured evidence (CONTEXT.md, "Profiles") —
   never a security or privacy key.
2. **Owner only** — every route runs `DeviceFilter → (ValidJsonFilter on
   PATCH and POST) → JwtFilter → RoleFilter`. `/settings` maps to no table in
   `role-access.hxx`, so every non-owner role is refused there; `/modules` is
   declared in `kModuleAccess`, which opens only `GET /modules` to every role
   (the controller projects it per role). Never add an imperative role check
   here.
2b. **The module engine never blocks the loop** — `ModuleEngine` runs its
   jobs on its own worker thread; a request reaches it through the `*Async`
   variants (a `BlockingTask` on the light lane). Every database change is one
   transaction; a module is never half enabled.
3. **One deadline per owner** — `GET /settings` reads the owners in parallel,
   off the event loop, each under `settings.list_timeout_ms`; a dead owner is
   `reachable: false`, never a failed page.
4. **Refusals are thrown** — unknown owner `SettingsErrors::UnknownService`
   (404), unreachable owner `SettingsErrors::Unavailable` (503), rejected
   changes a `ValidationException` keyed by setting key (422), a failed write
   `SettingsGatewayErrors::WriteFailed` (500), an unknown profile
   `SettingsGatewayErrors::UnknownProfile` (404), no profile file
   `SettingsGatewayErrors::ProfilesUnavailable` (503). A profile apply is a
   200 with per-key results even when owners refuse or miss it.
5. **Never log a value** — log the owner, the user id and the applied keys
   only; a text setting can hold anything.
6. **Parameter structs for 3+ params**, private `_` members, no raw owning
   pointers, `.hxx`/`.cc`, no comments of any kind (root rules 2, 4, 16, 20).
7. **Build gate** — 0 errors and 0 warnings (`-Wall -Wextra`).

## Layout

```
services/settings/
  CMakeLists.txt        standalone: config + feature modules, argus-settings, tests
  src/app/main.cc       composition: listener, filters, CORS, advice, mDNS,
                          settings.db, NATS, the module engine and its RPC
  src/app/rpc/          argus::settings-rpc — the Modules/ModuleStates server
  src/config/           argus::settings-config — listener, owners, deadlines
  src/feature/settings/ argus::settings-gateway — controllers/, dtos/,
                          infra/ (profile file, hardware facts), services/
                          (gateway, profile planner and service) and the
                          feature's error catalog
  src/feature/modules/  argus::settings-modules — the module manager:
                          controllers/, dtos/, infra/ (catalog file, owners
                          over the settings wire, NATS sink, disk/host probe),
                          repositories/ (module-state, module-job,
                          module-purge, module-audit, module-journal), schemas/, services/
                          (engine, resolver, hardware check, throttle,
                          action journal)
  database/schema.sql   settings.db (module tables only)
  modules.json          the module catalog
  tests/unit/           gateway and profiles (in-process owners), DTOs, config,
                          modules (catalog, resolver, hardware, throttle, JSON),
                          module engine (SQLite + fake owners), modules HTTP/RPC
  config.toml.example   native template (setup.sh fills owners)
  profiles.json         the owner profiles and the recommendation rules
  Dockerfile            argus-settings:local
```

## Build

```bash
./scripts/build-all.sh dev --only settings
```
