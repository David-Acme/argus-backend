# argus-deploy — AI Agent Instructions

Deployment folder of the monorepo (phase 6 compose v7). Read `CONTEXT.md` before
any change; the cutover shape and its exceptions are documented there.

## Scope

- `docker-compose.yml` — the deployment stack: nats and rustfs as the shared
  infrastructure, the thirteen argus services, the tunnel/relay pair behind the
  `tunnel` profile, and the one-shot migration init tools.
- `../services/<name>/Dockerfile` — one image per microservice; packages
  are compiled into the service images (no package image).
- `nats.conf` (the broker config; its `auth.conf` include is generated per
  installation) and `livekit-tls.conf` + `livekit-tls-entrypoint.sh` (the
  TLS front and the loop that reloads it when the leaf rotates).
- One `config.<service>.toml.example` template per service —
  per-installation copies (`config.auth.toml`, ...) are gitignored and hold
  the instance secrets.

## MUST-FOLLOW Rules

- Rule 27: never mount one service's database or data directory into another
  service. Cross-domain reads go through the SDK clients
  (`packages/clients/<domain>/src`) or NATS events; only the owner gets its own
  DB volume and schema.
- Never bake secrets into images. Instance secrets live in the gitignored
  config files.
- `identity-init` is opt-in (`--profile identity-init`): it runs
  `argus-migrate-identity` (source argus.db → target identity.db) on the
  argus-identity image and is idempotent. `sync-init` is opt-in the same way
  (`--profile sync-init`): it runs `argus-migrate-sync` (source identity.db →
  target sync.db — the five sync tables, including its own mailbox) on the
  argus-sync image, verifies every row it copied and re-runs as a no-op. The
  rollback is its own profile (`--profile sync-rollback`), because the forward
  service mounts identity's directory read-only: the rollback service runs the
  same tool with the paths swapped, so it mounts `sync/` read-only and
  identity's directory writable. Both are the one-shot exceptions to rule 27 —
  one owner's data directory is visible to the other owner's migration tool —
  and neither runs while the stack is up. argus-identity and
  argus-sync apply their own `database/schema.sql` at boot, so a fresh install
  works without either init profile.
- Every service merges `x-hardening` (no capabilities, no new privileges, a
  20 s stop grace period); a new service does too, and takes `x-readonly-root`
  unless it writes outside its own mounts (CONTEXT.md, "Container
  hardening"). Only argus-identity mounts the CA signer directory.
- The `camera-init`, `productivity-init` and `notification-init` tools mount
  identity's directory read-only (they read the legacy `argus.db`); that is
  the same documented exception, never a writable mount.
- This stack uses the `argus-cutover` project, `argus-cutover-*` volumes and
  networks only; never touch other compose projects or their volumes.
- No C++ code in this folder: changes here are compose/config only.
  Dockerfiles live in the service folders they build.
- 100% English; no comments (root rule 20).

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, no comments in code, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
