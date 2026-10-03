# Docker deployment

Production-style deployment is container-only: every microservice owns a
`Dockerfile` and `argus-deploy/docker-compose.yml` builds one image per
service. Packages are reusable libraries and compile into the service images;
no package has its own image.

## Host provisioning

`scripts/provision-host.sh` prepares a Linux or macOS host without building
the stack: Docker + Compose v2, the instance PKI, the per-service
`config.<service>.toml` files with unique shared secrets and the external data
tree. It is idempotent; existing CAs, secrets and databases are reused, so an
image update is `docker compose build && docker compose up -d` over the same
state.

```bash
./scripts/provision-host.sh --with-models      # download weights, no start
./scripts/provision-host.sh --start            # build + start the stack
./scripts/provision-host.sh --migrate-volumes  # copy pre-bind named volumes
```

The script writes the gitignored `argus-deploy/.env` (0600) with absolute host
paths and the invoking UID/GID; every value is overridable before the run
(`ARGUS_DATA_DIR`, `ARGUS_CERTS_DIR`, `ARGUS_MODELS_DIR`, `ARGUS_GO2RTC_DIR`).
macOS hosts can prepare config, PKI and Docker (Colima or Docker Desktop), but
the stack targets Linux host networking.

## Images

Each `services/<name>/Dockerfile` is a two-stage Debian trixie build:

- build stage: build tools plus Conan 2.21.0, then
  `./scripts/build-all.sh prod --no-tests --only <name>` from the repo
  root, using shared BuildKit caches for the Conan package and build folders;
- runtime stage: only the shared libraries the binaries need
  (`libvulkan1`, `mesa-vulkan-drivers`, gRPC runtime, OpenMP/stdlib) and the
  service binaries in `/opt/argus`.

Three images carry extra tools from their own build: the identity image ships
`argus-migrate-identity` (identity is its owner since Phase 3c-1), argus-camera ships
`argus-migrate-camera` and `argus-vulkan-probe`, and the productivity and
notification images ship their migration tools. The tunnel image ships the
client and the relay.

Build every image from the repository root, sequentially:

```bash
COMPOSE_PARALLEL_LIMIT=1 docker compose \
  -f argus-deploy/docker-compose.yml \
  --profile tunnel --profile identity-init build
```

Parallel builds race on the shared Conan cache (`Reference ... already
exists`); sequential builds reuse it. Each Dockerfile has its own
`Dockerfile.dockerignore` next to it, which keeps the nested build trees out
of the build context.

## Compose topology

One container per service: `argus-auth`, `argus-identity`,
`argus-camera`, `argus-productivity`, `argus-notification`, `argus-sync`,
`argus-guard`, `argus-settings`, `argus-tts`, `argus-stt`, `argus-vlm`,
`argus-llm`, `argus-voice`, `argus-relay`, `argus-tunnel-client`, plus
`nats`. Each container runs its service image and mounts its own
`config.<service>.toml`. The settings owners (`argus-tts`, `argus-stt`,
`argus-vlm`, `argus-llm`, `argus-voice`) mount theirs writable, because a
change made through `argus-settings` (HTTPS 7045, owner only) is persisted by
the owner into that file; every other config mount is read-only except
identity's.

`argus-auth` publishes its HTTP surface (7042) on every interface, so the app
dials `/auth` directly, and its RPC listener (7043) on `127.0.0.1` only. The
session verdict is a fleet-internal answer gated by `[auth] rpc_secret`:
publishing that port on every interface would hand a reachable host the
ungated session verdict.

Opt-in init profiles run the migration CLIs from the owner service image:
`identity-init` (identity image), `camera-init` and `vulkan-probe` (camera
image), `productivity-init` and `notification-init` (their service images), and
the mirrored pair of the Phase 3c-2 split, `sync-init` and `sync-rollback`
(sync image). They are idempotent and never touch a live database: each runs
with `network_mode: none` and the stack stopped. The sync pair is the one place
a service image sees another owner's data directory, which rule 27 allows only
as this one-shot tool — `sync-init` mounts identity's directory read-only,
`sync-rollback` mounts `sync/` read-only — and neither is part of a normal
`up`.

```bash
./scripts/provision-host.sh --start
```

## Volumes and models

- Host state lives outside the images and is linked in by bind mount, so a
  new image reuses it unchanged:
  - `${ARGUS_DATA_DIR:-./data}` (default `argus-deploy/data/`, gitignored)
    holds one directory per owner: `identity/` (argus-identity,
    `identity.db` + WAL),
    `auth/` (argus-auth, `auth.db` + WAL), `sync/` (argus-sync, `sync.db` +
    WAL), `camera/`, `productivity/`,
    `notification/`, `guard/` and `memory/`;
  - `${ARGUS_CERTS_DIR:-../certs}` holds the instance PKI;
  - `${ARGUS_MODELS_DIR:-../models}` and
    `${ARGUS_GO2RTC_DIR:-../third_party/go2rtc}` mount read-only; images
    never bake weights or binaries.
- `argus-deploy/.env` (created by `provision-host.sh`, `chmod 600`) carries
  those absolute paths, `ARGUS_UID`/`ARGUS_GID` and the port overrides.
  `.env.example` lists the path keys, the ids and the loopback ports; the
  compose file also reads `SYNC_PORT`, `SYNC_CONTROL_PORT`, `RELAY_HOME_PORT`,
  `RELAY_DEVICE_PORT`, `RELAY_HEALTH_PORT`, `RUSTFS_REGION`, one
  `ARGUS_<SERVICE>_MEMORY_LIMIT`/`ARGUS_<SERVICE>_CPU_LIMIT` pair per service,
  `NATS_MEMORY_LIMIT` and `RUSTFS_MEMORY_LIMIT`/`RUSTFS_CPU_LIMIT`, each with
  the default written beside it in `docker-compose.yml`, and any of them can
  be set in `.env`. Instance secrets stay in the `config.*.toml` files, never
  in `.env`.
- Schema SQL is bind-mounted from the owner folders, so migration tools always
  resolve `--schema` even from a freshly provisioned data dir.
- `camera-stream` stays a named volume: go2rtc writes its 0600 credential
  file there, never on a host bind.
- RustFS stores private objects under `${ARGUS_DATA_DIR}/rustfs/objects`
  (bucket-scoped application credentials in
  `${ARGUS_DATA_DIR}/rustfs/secrets`, 0600). argus-identity, argus-camera and
  argus-guard are its three consumers and reach it at `http://rustfs:9000`;
  the host-loopback spelling went with the gateway's storage block in Phase 3d
  step 1, because the gateway read no `storage.` key.
  Camera evidence snapshots land under `cameras/<id>/` and guard incident
  records under `guard/incidents/<id>/`.
- Update flow: `docker compose build && docker compose up -d`. Never
  `docker compose down -v`; it deletes the camera stream volume. One upgrade
  needs its init profile first: an install whose audit rows still live in
  `identity.db` must run `--profile sync-init` on a stopped stack before
  `up -d`, because `argus-sync` otherwise applies its schema to an empty
  `sync.db` and serves an empty audit history, which every client can only
  answer with a full re-bootstrap. A fresh install needs no profile.
