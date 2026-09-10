# Docker deployment

Production-style deployment is container-only: `argus-deploy/` builds one
source image and Compose runs each service from it.

## Image

`argus-deploy/Dockerfile` is a multi-stage Debian trixie build:

- build stage: build tools plus Conan 2.21.0, then
  `./scripts/build-all.sh prod --no-tests` (the same 19 standalone graphs as
  local work) with a BuildKit Conan cache mount;
- runtime stage: only the shared libraries the binaries need
  (`libvulkan1`, `mesa-vulkan-drivers`, gRPC runtime, OpenMP/stdlib), one user
  (`ARGUS_UID`/`ARGUS_GID`) and the 16 runtime binaries in `/opt/argus`.

```bash
docker build -f argus-deploy/Dockerfile -t argus-cutover:local .
cd argus-deploy
ARGUS_UID="$(id -u)" ARGUS_GID="$(id -g)" docker compose up -d
```

## Compose topology

One image, one container per service: `gateway`, `argus-camera`,
`argus-productivity`, `argus-notification`, `argus-tts`, `argus-stt`,
`argus-vlm`, `argus-llm`, `argus-voice`, `argus-relay`,
`argus-tunnel-client`, plus `nats`. Each container overrides the entrypoint
with its binary and mounts its own `config.<service>.toml`.

Opt-in init profiles run the migration CLIs from the same image:
`identity-init`, `camera-init`, `productivity-init`, `notification-init`.
They are idempotent and never touch a live database.

## Volumes and models

- Host model directories mount read-only; images never bake weights.
- Per-service data directories (`camera`, `productivity`, `notification`,
  `memory`) and `database/` hold SQLite files; schema SQL is bind-mounted
  from the owner folders so migration tools always resolve `--schema`.
- `third_party/go2rtc` mounts into the camera container for its managed
  go2rtc binary.
