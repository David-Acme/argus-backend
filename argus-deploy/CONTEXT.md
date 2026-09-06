# argus-deploy — CONTEXT

Compose v3 of the migration plan: the Fase 1 cutover stack (gateway + legacy +
nats + rustfs + identity init), the Fase 2 argus-camera service (camera.db
volume + camera-init) and the Fase 3 argus-productivity + argus-notification
services (productivity.db / notification.db volumes + their init profiles).
Decisions and traps live here.

## Image (Ruling N)

`Dockerfile` is ONE multi-stage image from the repo root: the same Debian +
Conan 2.21.0 toolchain as `docker/Dockerfile`, a single `conan install` at
Release, then `cmake --build --preset prod` (backend, all targets,
`ARGUS_BUILD_LABS=OFF`) plus `--target argus-gateway argus-migrate-identity
argus-vulkan-probe argus-migrate-camera argus-camera argus-productivity
argus-notification argus-migrate-productivity argus-migrate-notification`.
All binaries land in `/opt/argus`; the service picks its binary via an
`entrypoint:` override (Docker composes `command:` as ARGUMENTS to the image
`ENTRYPOINT`, so a `command:` "override" here would execute
`/opt/argus/argus-backend /opt/argus/argus-gateway` — the trap 242ffd3
fixed). One conan
dependency set serves all binaries (the root `conanfile.txt` already carries
cnats, mdns and everything else they need when built from the root tree).

`-march=native` applies (AGENTS.md Release tuning): build on the machine that
runs the stack — the self-hosted model — or the binary may SIGILL elsewhere.
`-flto=auto` + many translation units make the build heavy; expect a long
first build (the conan cache is a buildkit cache mount, so rebuilds are fast).

The build stage installs `libvulkan-dev` + `glslc`: ncnn is integrated with
`NCNN_VULKAN=ON` (third_party/CMakeLists.txt) and compiles its Vulkan shaders
with `glslc` at build time. Without them the ncnn build silently drops its
Vulkan backend in the container and the argus-camera detector could never
engage RADV (it would only ever run CPU).

Runtime image adds `curl` (gateway and argus-camera `/health` healthchecks),
`netcat-openbsd` (legacy TCP healthcheck) and `mesa-vulkan-drivers` (RADV,
so a container with `/dev/dri` can drive the GPU) on top of
`docker/Dockerfile`'s runtime set. The image also carries
`argus-vulkan-probe` (Fase 2 Vulkan gate): it reuses ncnn's own Vulkan init
and exits 0 only when `vkCreateInstance` plus at least one physical device
work; otherwise the detector stays on its Vulkan→CPU fallback. Run it with
`docker compose --profile vulkan-probe run --rm vulkan-probe`, which mounts
`/dev/dri` only — no `group_add` — and worked on this host (the device
cgroup plus the host's device ACLs let `--user 1000:1000` reach both nodes;
`renderD128` is 0666 and `card1` carries an ACL). Plain-docker equivalent,
also verified here: `docker run --rm --device /dev/dri --user 1000:1000
--entrypoint /opt/argus/argus-vulkan-probe argus-cutover:local`. On hosts
with restrictive `/dev/dri` ACLs add the HOST `video`/`render` GIDs
numerically via `group_add: [<gid>, <gid>]` — Docker resolves group NAMES
against the host group file, so `--group-add video --group-add render`
fails on hosts whose names differ (observed: "unable to find group render").
A GPU-less host drops the device from argus-camera (and, if the probe is run
there, vulkan-probe) with a compose override carrying `devices: !override []`
— a plain `[]` merge silently KEEPS the device — and the detector degrades to
CPU in-binary.

Model artifacts (GGUF, ncnn blobs) are NOT baked in: `models/` is
dockerignored out of the build context and bind-mounted read-only; provision
with `scripts/setup.sh` / `scripts/setup.sh camera` on the host.

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
  `RUSTFS_API_PORT`) for the host-networked services.
- **argus-camera** (Fase 2) lives on the `internal` bridge network too: its
  only host exposure is the loopback-published 7026 listener the gateway
  proxies to. Its go2rtc (1984/8554, Ruling AH) stays INSIDE the container —
  no compose service and no host publish; the app only ever talks through the
  gateway. The camera config points `[nats] url` at the internal alias
  `nats://nats:4222`.
- **argus-productivity / argus-notification** (Fase 3, compose v3) live on
  the same `internal` bridge network with only their 7027/7028 listeners
  loopback-published for the host-networked gateway; their `[nats] url`
  points at the internal alias as well.
- The gateway and legacy configs therefore point `[nats] url` and
  `[storage.s3] endpoint` at `127.0.0.1` ports.
- Transitional trust note: the camera resolves the caller IP from
  `X-Forwarded-For` only for trusted peers. Through the docker-proxy the
  gateway arrives as the internal network's gateway IP (e.g. 172.18.0.1), so
  `config.camera.toml` must list that address in `device.trusted_proxy_ips`
  (the legacy keeps its loopback trust — the gateway reaches it as 127.0.0.1).
  This dies when the internal identity headers replace the forwarded-for
  trust (plan §Fase 3+).

## Services

| Service | Image | Notes |
|---|---|---|
| gateway | built (`argus-cutover:local`) | TLS 7024, `/health` healthcheck; mounts the productivity and notification volumes next to camera-db |
| legacy | same image, `entrypoint:` override | plain 127.0.0.1:7025 |
| argus-camera | same image, `entrypoint:` override | internal network, loopback 7026 publish; `/health` healthcheck; `/dev/dri` |
| argus-productivity | same image, `entrypoint:` override | internal network, loopback 7027 publish; owns productivity.db; `/health` healthcheck |
| argus-notification | same image, `entrypoint:` override | internal network, loopback 7028 publish; owns notification.db; `/health` healthcheck |
| nats | `nats:2.11.14-alpine` | exact tag pin; core NATS (no JetStream needed) |
| rustfs / rustfs-init | copied verbatim from the root compose | only bind paths, volume/network names differ (`argus-cutover-*`) |
| identity-init | same image | `profiles: [identity-init]`, runs `argus-migrate-identity` |
| camera-init | same image | `profiles: [camera-init]`, runs `argus-migrate-camera` against the camera.db volume |
| productivity-init | same image | `profiles: [productivity-init]`, runs `argus-migrate-productivity` against the productivity.db volume |
| notification-init | same image | `profiles: [notification-init]`, runs `argus-migrate-notification` against the notification.db volume |
| vulkan-probe | same image | `profiles: [vulkan-probe]`, runs `argus-vulkan-probe` with `/dev/dri` |

Ordering: `nats` goes healthy first and both the gateway and argus-camera wait
for `nats: service_healthy` — the gateway's NatsBus connects once at boot with
no retry, so a lost boot race would leave every fan-out subscription silently
dead while all healthchecks stay green. The legacy waits for the gateway
(`service_started`, which also guarantees the image is built) and for
`rustfs-init` completion. The gateway and argus-camera then boot in parallel
and resolve their database handoff inside each process: the gateway opens
camera.db read-only only after argus-camera's boot schema apply has created it
(waiting bounded, 30s, before falling back to the default client), and
argus-camera opens identity.db read-only only after the gateway's boot schema
apply has created it (same bounded wait). Neither service waits on the other's
health, so there is no cycle. A fresh `up -d` without camera-init therefore
works end to end: argus-camera creates camera.db, the gateway picks it up
within seconds and camera CRUD through the gateway serves live rows — but
argus-camera's boot apply then makes camera.db live data, so `camera-init`
can no longer migrate the pre-existing argus.db camera rows (it no-ops on any
schema-current target before reading the source). An installation that wants
the legacy camera rows migrated must run `docker compose --profile camera-init
run --rm camera-init` BEFORE the first boot, while camera.db does not exist
yet. The init tools resolve `--schema` INSIDE the data-dir bind, so both init
services bind the repo-shipped `database/identity-schema.sql` /
`database/camera-schema.sql` read-only over that path — a data dir provisioned
without the schema SQLs still works (the default `../database` deployment dir
already carries them; the single-file binds are no-ops there). The gateway
applies `identity-schema.sql` at boot and
aborts if it fails, so a fresh install creates `identity.db` without the init
profile; argus-camera applies `camera-schema.sql` at boot the same way, so a
fresh install creates camera.db without `camera-init`. On an existing
installation run `docker compose --profile identity-init run --rm
identity-init` (idempotent, guards intact: refuses same-path, requires the 7
source tables, skips cleanly when `argus.db` does not exist yet). Do not run
either init tool while the services hold its target database open — stop the
stack first.

Fase 3 (Rulings AT/AU/AV) extends the same shape to productivity.db and
notification.db, each on its own dedicated named volume:

- `argus-productivity` mounts `argus-cutover-productivity-db` rw and applies
  `database/productivity-schema.sql` at boot; the gateway mounts the same
  volume and opens `productivity/productivity.db` mode=ro (sync reads). The
  gateway waits bounded (30s) for the boot apply the same way it does for
  camera.db, then falls back to the default client with a warn.
- `argus-notification` mounts `argus-cutover-notification-db` rw; the gateway
  mounts it rw too and opens `notification/notification.db` READ-WRITE (its
  camera-notifier writes, Ruling AR) — the only cross-service rw db pair in
  the stack. The gateway waits bounded (30s) here the same way.
- `productivity-init` / `notification-init` are the only migration paths onto
  those volumes and MUST run BEFORE the first boot (the f8291e4 lesson, same
  as camera-init): once the owning service has boot-applied the schema the
  volume is live data and the migrate tool correctly no-ops instead of
  resurrecting argus.db rows over it. On an existing installation:
  `docker compose --profile productivity-init run --rm productivity-init`
  (and the notification twin) with the stack stopped; both are idempotent and
  no-op on a schema-current target.
- Boot order (Ruling AV): nats goes healthy first; the gateway, argus-camera,
  argus-productivity and argus-notification then boot in parallel — every one
  of them gates on `nats: service_healthy` because each connects its NatsBus
  once at boot with no retry. argus-productivity/argus-notification wait
  bounded (30s) for the gateway-created identity.db inside their own boot
  (read-only open), so there is no compose dependency on the gateway and no
  cycle.

For acceptance runs, `scripts/seed-golden.py` seeds the golden /sync verify
state into a scratch COPY of the databases (Golden Cam / Golden Zone rows,
`calendar_event.ends_at` NULL, `project_task.assignee_id` 1, the identity
golden rows and a recorder refresh session minted from the config secrets —
secrets are read at runtime, never printed; the refresh token lands in a
0600 file). Run it with the stack stopped, before camera-init.

## Healthchecks

- gateway: `curl -kfs https://127.0.0.1:7024/health` (envelope 200).
- legacy: `nc -z 127.0.0.1 7025` — the legacy has NO `/health` route
  (gateway-native, F1-3b); a TCP check is the honest equivalent until one
  exists. A listener that accepts but wedges would still report healthy.
- argus-camera: `curl -fs http://127.0.0.1:7026/health` (envelope 200).
- argus-productivity: `curl -fs http://127.0.0.1:7027/health` (envelope 200).
- argus-notification: `curl -fs http://127.0.0.1:7028/health` (envelope 200).

## Volume map

| Volume | Mounted into | Content |
|---|---|---|
| `argus-cutover-camera-db` | argus-camera, legacy, gateway — all at `/opt/argus/camera` (rw) | camera.db (+ WAL files) |
| `argus-cutover-productivity-db` | argus-productivity (rw, owner), gateway (rw mount, mode=ro open) — at `/opt/argus/productivity` | productivity.db (+ WAL files) |
| `argus-cutover-notification-db` | argus-notification (rw), gateway (rw, read-write client) — at `/opt/argus/notification` | notification.db (+ WAL files) |
| `argus-cutover-camera-stream` | argus-camera at `/opt/argus/stream` | go2rtc.yaml generated by Go2rtcManager (chmod 600, camera credentials) |
| `argus-cutover-rustfs-data` | rustfs at `/data` | object storage |

`argus-cutover-camera-db` is ONE file on ONE named volume shared by the three
services (Ruling AG): the legacy resolves the camera-domain rows there
(Ruling X), the gateway opens it mode=ro for the camera sync reads (Ruling
Z). All three configs point `[camera] db` at `camera/camera.db`. Cross-
process access is safe by construction: every opener applies WAL +
busy_timeout 5000 (argus-camera at boot, the legacy when it attaches
`[camera] db`, the gateway when it opens the read-only client). Compose adds
no DDL sidecar — the camera schema boot-apply belongs to argus-camera alone,
and `camera-init` is the only migration path onto the volume.

## Configuration and secrets (Ruling Q)

- `config.gateway.toml.example` / `config.legacy.toml.example` /
  `config.camera.toml.example` / `config.productivity.toml.example` /
  `config.notification.toml.example` encode the cutover keys; copy to
  `config.gateway.toml` / `config.legacy.toml` / `config.camera.toml` /
  `config.productivity.toml` / `config.notification.toml`
  (gitignored) and fill: `[jwt] secret/refresh_secret` and
  `[device] fingerprint_secret` (identical in all five — the gateway mints,
  the legacy, argus-camera and the two Fase 3 services verify, and the device
  hash must match across the proxy), `[storage.s3]` in the legacy (the
  `argus_s3_*` service-account credentials rustfs-init creates from the docker
  secrets), plus region/bucket, and the `device.trusted_proxy_ips` of every
  bridge-networked service (argus-camera, argus-productivity,
  argus-notification — the internal network's gateway IP, see the network
  section). The gateway's `[productivity] db` /
  `[notifications] db` point at the volume mounts
  (`productivity/productivity.db`, `notification/notification.db`).
- Docker secrets stay the existing `docker/runtime/secrets/*` files: nothing
  new, nothing baked into images.
- The gateway links no go2rtc code, so it neither mounts nor spawns go2rtc.
- argus-camera spawns go2rtc itself (Go2rtcManager fork/exec, Ruling AH) from
  the bind-mounted `third_party/go2rtc` binary and writes its own
  `go2rtc.yaml` (chmod 600, camera credentials) onto the camera-stream
  volume — never a bind. Its 1984/8554 binds stay inside the container.
- The legacy with `[camera] db` configured no longer spawns go2rtc
  (`Camera domain delegated; go2rtc stays with argus-camera`) — in the
  container stack its `[streaming]` keys are inert.
- `certs/`, `models/` are bind-mounted read-only from the repo like the root
  compose's `backend` service; the database directory
  (`${ARGUS_DATA_DIR:-../database}`) is writable (identity.db, WAL files).
  `ARGUS_DATA_DIR` exists for acceptance runs on a scratch copy of the real
  database directory — the default is the repo `database/` and unset env
  reproduces the F1-6 shape byte-identically.

## Port map (host)

| Port | Bind | Owner |
|---|---|---|
| 7024 TLS | 0.0.0.0 | gateway (public) |
| 7025 plain | 127.0.0.1 | legacy (internal, gateway upstream) |
| 7026 plain | 127.0.0.1 (compose publish) | argus-camera (internal, gateway upstream) |
| 7027 plain | 127.0.0.1 (compose publish) | argus-productivity (internal, gateway upstream) |
| 7028 plain | 127.0.0.1 (compose publish) | argus-notification (internal, gateway upstream) |
| 4222 | 127.0.0.1 | nats client |
| 8222 | 127.0.0.1 | nats monitor |
| 9000 | 127.0.0.1 | rustfs S3 |
| 1984 / 8554 | container loopback only | go2rtc spawned by argus-camera (Ruling AH — never published) |
| 8800 | host | Tapo talk channel (camera-side, legacy `[tapo]`) |
