# argus-deploy — AI Agent Instructions

Deployment folder of the monorepo (phase 6 compose v7). Read `CONTEXT.md` before
any change; the cutover shape and its exceptions are documented there.

## Scope

- `docker-compose.yml` — cutover stack: gateway, nats, argus-camera,
  domain services, identity migration init tool.
- `../services/argus-<name>/Dockerfile` — one image per microservice; packages
  are compiled into the service images (no package image).
- `config.gateway.toml.example` and one template per domain service —
  per-installation copies (`config.gateway.toml`, ...) are gitignored and hold
  the instance secrets.

## MUST-FOLLOW Rules

- Rule 27: never mount one service's database or data directory into another
  service. Cross-domain reads go through the SDK clients
  (`packages/argus-contracts/sdk`) or NATS events; only the owner gets its own
  DB volume and schema.
- Never bake secrets into images. Instance secrets live in the gitignored
  config files.
- `identity-init` is opt-in (`--profile identity-init`): it runs
  `argus-migrate-identity` (source argus.db → target identity.db) and is
  idempotent. The gateway applies `database/schema.sql` at boot, so a fresh
  install works without the init profile.
- This stack uses the `argus-cutover` project, `argus-cutover-*` volumes and
  networks only; never touch other compose projects or their volumes.
- No C++ code in this folder: changes here are compose/config only.
  Dockerfiles live in the service folders they build.
- 100% English; minimal comments.

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, comment discipline, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
