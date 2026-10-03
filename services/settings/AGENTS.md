# argus-settings — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for every change in
this service. The MUST-FOLLOW rules below restate the ones that apply.

## MUST-FOLLOW Rules

1. **No data of its own** — this service opens no database and persists
   nothing. Every setting lives in its owner's `config.toml`, validated by the
   owner's `SettingsRegistry`; this service only reads catalogs and forwards
   changes over `argus.settings.v1` through `argus::clients::settings`.
2. **Owner only** — both routes run `DeviceFilter → (ValidJsonFilter on
   PATCH) → JwtFilter → RoleFilter`. `/settings` maps to no table in
   `role-access.hxx`, so every non-owner role is refused there; never add an
   imperative role check here.
3. **One deadline per owner** — `GET /settings` reads the owners in parallel,
   off the event loop, each under `settings.list_timeout_ms`; a dead owner is
   `reachable: false`, never a failed page.
4. **Refusals are thrown** — unknown owner `SettingsErrors::UnknownService`
   (404), unreachable owner `SettingsErrors::Unavailable` (503), rejected
   changes a `ValidationException` keyed by setting key (422), a failed write
   `SettingsGatewayErrors::WriteFailed` (500).
5. **Never log a value** — log the owner, the user id and the applied keys
   only; a text setting can hold anything.
6. **Parameter structs for 3+ params**, private `_` members, no raw owning
   pointers, `.hxx`/`.cc`, no comments of any kind (root rules 2, 4, 16, 20).
7. **Build gate** — 0 errors and 0 warnings (`-Wall -Wextra`).

## Layout

```
services/settings/
  CMakeLists.txt        standalone: config + feature modules, argus-settings, tests
  src/app/main.cc       composition: listener, filters, CORS, advice, mDNS
  src/config/           argus::settings-config — listener, owners, deadlines
  src/feature/settings/ argus::settings-gateway — controllers/, dtos/,
                          services/ and the feature's error catalog
  tests/unit/           gateway (in-process owners), DTOs, config
  config.toml.example   native template (setup.sh fills owners)
  Dockerfile            argus-settings:local
```

## Build

```bash
./scripts/build-all.sh dev --only settings
```
