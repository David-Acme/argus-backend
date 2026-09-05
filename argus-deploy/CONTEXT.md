# argus-deploy — CONTEXT

Compose v1 of the Fase 1 migration plan (gateway + legacy + nats + rustfs +
identity init), so the F1-5 cutover topology runs as containers. Decisions and
traps live here.

## Image (Ruling N)

`Dockerfile` is ONE multi-stage image from the repo root: the same Debian +
Conan 2.21.0 toolchain as `docker/Dockerfile`, a single `conan install` at
Release, then `cmake --build --preset prod` (backend, all targets,
`ARGUS_BUILD_LABS=OFF`) plus `--target argus-gateway argus-migrate-identity`.
All binaries land in `/opt/argus`; the service picks its binary via an
`entrypoint:` override (Docker composes `command:` as ARGUMENTS to the image
`ENTRYPOINT`, so a `command:` "override" here would execute
`/opt/argus/argus-backend /opt/argus/argus-gateway` — the trap 242ffd3
fixed). One conan
dependency set serves both binaries (the root `conanfile.txt` already carries
cnats, mdns and everything the gateway needs when built from the root tree).

`-march=native` applies (AGENTS.md Release tuning): build on the machine that
runs the stack — the self-hosted model — or the binary may SIGILL elsewhere.
`-flto=auto` + many translation units make the build heavy; expect a long
first build (the conan cache is a buildkit cache mount, so rebuilds are fast).

Runtime image adds `curl` (gateway `/health` healthcheck),
`netcat-openbsd` (legacy TCP healthcheck) and `mesa-vulkan-drivers` (RADV,
so a container with `/dev/dri` can drive the GPU) on top of
`docker/Dockerfile`'s runtime set. The image also carries
`argus-vulkan-probe` (Fase 2 Vulkan gate): it reuses ncnn's own Vulkan init
and exits 0 only when `vkCreateInstance` plus at least one physical device
work; otherwise the detector stays on its Vulkan→CPU fallback. Run it with
`docker compose --profile vulkan-probe run --rm vulkan-probe` (mounts
`/dev/dri` and adds the container user to the `video`/`render` device
groups) or plain `docker run --rm --device /dev/dri --group-add video
--group-add render argus-cutover:local /opt/argus/argus-vulkan-probe`.

## Network shape (Ruling O, transitional exception)

- **gateway** — `network_mode: host`. The public surface: TLS 0.0.0.0:7024,
  mDNS advertised here only.
- **legacy** — `network_mode: host`, mirroring the root compose's opt-in
  `backend` service: go2rtc (1984/8554), RTSP and mDNS camera discovery need
  the host, and the Tapo talk channel (`media_port = 8800`) listens on the
  LAN for cameras. It binds **127.0.0.1:7025 only** (its config listener is
  loopback).
- The legacy trusts `X-Forwarded-For` **only from loopback peers** (and
  `device.trusted_proxy_ips`), so the gateway must reach it as 127.0.0.1 —
  a bridge-networked gateway cannot reach a host loopback bind. That is why
  the gateway is host-networked too; there is no `edge` bridge network in
  this phase. Only the gateway binds a public port, which is the ruling's
  intent. When Fase 2 moves camera/media into their own services this shape
  gets revisited.
- **nats** (`nats:2.11.14-alpine`, core NATS, monitor port 8222) and **rustfs**
  live on the `internal` bridge network, published on host loopback
  (4222/8222/9000, overridable via `NATS_CLIENT_PORT`, `NATS_MONITOR_PORT`,
  `RUSTFS_API_PORT`) for the two host-networked services.
- The gateway and legacy configs therefore point `[nats] url` and
  `[storage.s3] endpoint` at `127.0.0.1` ports.

## Services

| Service | Image | Notes |
|---|---|---|
| gateway | built (`argus-cutover:local`) | TLS 7024, `/health` healthcheck |
| legacy | same image, `entrypoint:` override | plain 127.0.0.1:7025 |
| nats | `nats:2.11.14-alpine` | exact tag pin; core NATS (no JetStream in Fase 1) |
| rustfs / rustfs-init | copied verbatim from the root compose | only bind paths, volume/network names differ (`argus-cutover-*`) |
| identity-init | same image | `profiles: [identity-init]`, runs `argus-migrate-identity` |
| vulkan-probe | same image | `profiles: [vulkan-probe]`, runs `argus-vulkan-probe` with `/dev/dri` |

Ordering: the legacy waits for the gateway (`service_started`, which also
guarantees the image is built) and for `rustfs-init` completion. The gateway
applies `identity-schema.sql` at boot and aborts if it fails, so a fresh
install creates `identity.db` without the init profile; on an existing
installation run `docker compose --profile identity-init run --rm
identity-init` (idempotent, guards intact: refuses same-path, requires the 7
source tables, skips cleanly when `argus.db` does not exist yet). Do not run
it while the services hold `identity.db` open — stop the stack first.

## Healthchecks

- gateway: `curl -kfs https://127.0.0.1:7024/health` (envelope 200).
- legacy: `nc -z 127.0.0.1 7025` — the legacy has NO `/health` route
  (gateway-native, F1-3b); a TCP check is the honest equivalent until one
  exists. A listener that accepts but wedges would still report healthy.

## Configuration and secrets (Ruling Q)

- `config.gateway.toml.example` / `config.legacy.toml.example` encode the F1-5
  cutover keys; copy to `config.gateway.toml` / `config.legacy.toml`
  (gitignored) and fill: `[jwt] secret/refresh_secret` and
  `[device] fingerprint_secret` (identical in both — the gateway mints, the
  legacy verifies, and the device hash must match across the proxy),
  `[storage.s3]` in the legacy (the `argus_s3_*` service-account credentials
  rustfs-init creates from the docker secrets), plus region/bucket.
- Docker secrets stay the existing `docker/runtime/secrets/*` files: nothing
  new, nothing baked into images.
- The gateway links no go2rtc code, so it neither mounts nor spawns go2rtc.
- The legacy's go2rtc config is no bind: `Go2rtcManager` writes `go2rtc.yaml`
  itself (chmod 600, camera credentials) from `[streaming]` keys into the
  container workdir; only the `third_party/go2rtc` binary is bind-mounted
  read-only, exactly like the root compose's `backend` service. If the host
  already runs a native go2rtc on 1984/8554, move the legacy's
  `[streaming] go2rtc_api/go2rtc_rtsp` to free ports.
- `certs/`, `models/`, `database/` are bind-mounted from the repo like the
  root compose's `backend` service; `database/` is writable (identity.db, WAL
  files).

## Port map (host)

| Port | Bind | Owner |
|---|---|---|
| 7024 TLS | 0.0.0.0 | gateway (public) |
| 7025 plain | 127.0.0.1 | legacy (internal, gateway upstream) |
| 4222 | 127.0.0.1 | nats client |
| 8222 | 127.0.0.1 | nats monitor |
| 9000 | 127.0.0.1 | rustfs S3 |
| 1984 / 8554 | 127.0.0.1 | go2rtc spawned by the legacy (manager defaults; `[streaming]` keys override) |
| 8800 | host | Tapo talk channel (camera-side) |
