# argus-deploy — AI Agent Instructions

Deployment folder of the monorepo (Fase 1 compose v1). Read `CONTEXT.md` before
any change; the cutover shape and its transitional exceptions are documented
there.

## Scope

- `docker-compose.yml` — cutover stack: gateway, legacy, nats, rustfs (+ init),
  identity migration init tool.
- `Dockerfile` — the single source-built image (both binaries + identity tool).
- `config.gateway.toml.example` / `config.legacy.toml.example` — per-service
  configuration templates; per-installation copies (`config.gateway.toml`,
  `config.legacy.toml`) are gitignored and hold the instance secrets.

## MUST-FOLLOW Rules

- The root `docker-compose.yml` is the dev tooling: never modify it from here.
  `rustfs` and `rustfs-init` in this compose are copied verbatim from it (only
  relative paths, volume and network names differ).
- Never bake secrets into images. Instance secrets live in the gitignored
  config files; docker secrets stay under `docker/runtime/secrets/`.
- `identity-init` is opt-in (`--profile identity-init`): it runs
  `argus-migrate-identity` (source argus.db → target identity.db) and is
  idempotent. The gateway applies `identity-schema.sql` at boot, so a fresh
  install works without the init profile.
- Never run or restart the `argus-local` compose project or its volumes from
  here; this stack uses the `argus-cutover` project, `argus-cutover-*` volumes
  and networks only.
- No C++ code in this folder: changes here are compose/Dockerfile/config only.
  Follow the backend `AGENTS.md` for anything that leaks into source.
- 100% English; minimal comments.
