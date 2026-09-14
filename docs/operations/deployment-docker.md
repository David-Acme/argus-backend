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

Each `services/argus-<name>/Dockerfile` is a two-stage Debian trixie build:

- build stage: build tools plus Conan 2.21.0, then
  `./scripts/build-all.sh prod --no-tests --only argus-<name>` from the repo
  root, using shared BuildKit caches for the Conan package and build folders;
- runtime stage: only the shared libraries the binaries need
  (`libvulkan1`, `mesa-vulkan-drivers`, gRPC runtime, OpenMP/stdlib) and the
  service binaries in `/opt/argus`.

Three images carry extra tools from their own build: the gateway ships
`argus-migrate-identity` (identity is its package), argus-camera ships
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

One container per service: `gateway`, `argus-camera`, `argus-productivity`,
`argus-notification`, `argus-tts`, `argus-stt`, `argus-vlm`, `argus-llm`,
`argus-voice`, `argus-relay`, `argus-tunnel-client`, plus `nats`. Each
container runs its service image and mounts its own `config.<service>.toml`.

Opt-in init profiles run the migration CLIs from the owner service image:
`identity-init` (gateway image), `camera-init` and `vulkan-probe` (camera
image), `productivity-init` and `notification-init` (their service images).
They are idempotent and never touch a live database.

```bash
./scripts/provision-host.sh --start
```

## Volumes and models

- Host state lives outside the images and is linked in by bind mount, so a
  new image reuses it unchanged:
  - `${ARGUS_DATA_DIR:-./data}` (default `argus-deploy/data/`, gitignored)
    holds one directory per owner: `identity/` (the gateway,
    `identity.db` + WAL), `camera/`, `productivity/`, `notification/`,
    `guard/` and `memory/`;
  - `${ARGUS_CERTS_DIR:-../certs}` holds the instance PKI;
  - `${ARGUS_MODELS_DIR:-../models}` and
    `${ARGUS_GO2RTC_DIR:-../third_party/go2rtc}` mount read-only; images
    never bake weights or binaries.
- `argus-deploy/.env` (created by `provision-host.sh`, `chmod 600`) carries
  those absolute paths, `ARGUS_UID`/`ARGUS_GID` and the port overrides;
  `.env.example` documents every supported key.
- Schema SQL is bind-mounted from the owner folders, so migration tools always
  resolve `--schema` even from a freshly provisioned data dir.
- `camera-stream` stays a named volume: go2rtc writes its 0600 credential
  file there, never on a host bind.
- RustFS stores private objects under `${ARGUS_DATA_DIR}/rustfs/objects`
  (bucket-scoped application credentials in
  `${ARGUS_DATA_DIR}/rustfs/secrets`, 0600). The gateway reaches it at
  `http://127.0.0.1:9000`; internal services at `http://rustfs:9000`.
  Camera evidence snapshots land under `cameras/<id>/` and guard incident
  records under `guard/incidents/<id>/`.
- Update flow: `docker compose build && docker compose up -d`. Never
  `docker compose down -v`; it deletes the camera stream volume.
