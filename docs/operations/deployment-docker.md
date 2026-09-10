# Docker deployment

Production-style deployment is container-only: every microservice owns a
`Dockerfile` and `argus-deploy/docker-compose.yml` builds one image per
service. Packages are reusable libraries and compile into the service images;
no package has its own image.

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
cd argus-deploy
ARGUS_UID="$(id -u)" ARGUS_GID="$(id -g)" docker compose up -d
```

## Volumes and models

- Host model directories mount read-only; images never bake weights.
- Per-service data directories (`camera`, `productivity`, `notification`,
  `memory`) and `database/` hold SQLite files; schema SQL is bind-mounted
  from the owner folders so migration tools always resolve `--schema`.
- `third_party/go2rtc` mounts into the camera container for its managed
  go2rtc binary.
