# argus-deploy — CONTEXT

Compose v5 of the migration plan: the Fase 1 cutover stack (gateway + legacy +
nats + rustfs + identity init), the Fase 2 argus-camera service (camera.db
volume + camera-init), the Fase 3 argus-productivity + argus-notification
services (productivity.db / notification.db volumes + their init profiles) and
the Fase 4 AI engine services argus-tts/argus-stt/argus-vlm/argus-llm/
argus-memory (models subpaths + the single-owner memory.db volume). Fase 5
adds the tunnel transport pair — argus-relay + argus-tunnel-client — behind
the opt-in `tunnel` profile. Decisions and traps live here.

## Image (Ruling N)

`Dockerfile` is ONE multi-stage image from the repo root: the same Debian +
Conan 2.21.0 toolchain as `docker/Dockerfile`, a single `conan install` at
Release, then `cmake --build --preset prod` (backend, all targets,
`ARGUS_BUILD_LABS=OFF`) plus `--target argus-gateway argus-migrate-identity
argus-vulkan-probe argus-migrate-camera argus-camera argus-productivity
argus-notification argus-migrate-productivity argus-migrate-notification
argus-tts argus-stt argus-vlm argus-llm argus-memory argus-tunnel-client
argus-tunnel-relay`.
All binaries land in `/opt/argus`; the service picks its binary via an
`entrypoint:` override (Docker composes `command:` as ARGUMENTS to the image
`ENTRYPOINT`, so a `command:` "override" here would execute
`/opt/argus/argus-backend /opt/argus/argus-gateway` — the trap 242ffd3
fixed). One conan
dependency set serves all binaries (the root `conanfile.txt` already carries
cnats, mdns and everything else they need when built from the root tree). The
memory stack needs no extra runtime packages: sqlite-vec compiles into the
binary (`SQLITE_CORE`), onnxruntime/llama/cnats are static conan archives and
the runtime stage's `libgomp1`/`libstdc++6` already cover them.

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
  `backend` service. Since F6-2 it serves no camera route and touches no
  camera.db (the camera-db volume mount and the go2rtc bind are gone); it
  binds **127.0.0.1:7025 only** (its config listener is loopback) for the
  identity/voice surfaces.
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
  `nats://nats:4222`. Since F6-2 the camera domain is wholly served by this
  service: the gateway routes `/camera` and `/zone` at every segment depth
  here and relays every `camera:*` frame to the same listener
  (`[camera] sync_url = ws://127.0.0.1:7026/sync`, the target flipped off the
  legacy in F2-2); the legacy serves no camera route and mounts no camera-db
  volume. Talk synthesis reaches argus-tts via the camera config's
  `[tts] remote_url` (host-networked loopback 7029).
- **argus-productivity / argus-notification** (Fase 3, compose v3) live on
  the same `internal` bridge network with only their 7027/7028 listeners
  loopback-published for the host-networked gateway; their `[nats] url`
  points at the internal alias as well.
- **The five AI engine services** (Fase 4, compose v4) live on the same
  `internal` bridge network, which now pins `172.19.0.0/24` so they carry
  static addresses (argus-tts .29, argus-stt .30, argus-vlm .31, argus-llm
  .32, argus-memory .33). The legacy gates dial their loopback-published
  listeners as `127.0.0.1` (host-networked consumer); argus-memory dials
  argus-llm at the static `172.19.0.32:7032` because the internal raw-socket
  wires resolve IPv4 literals only. Loopback-only publishes keep every AI
  wire unreachable from the LAN — "internal, no host publish" means no
  non-loopback exposure: a strict no-publish would strand the only consumer
  (the host-networked legacy), which cannot resolve the bridge aliases.
- The gateway and legacy configs therefore point `[nats] url` and
  `[storage.s3] endpoint` at `127.0.0.1` ports.
- Transitional trust note: the camera resolves the caller IP from
  `X-Forwarded-For` only for trusted peers. Through the docker-proxy the
  gateway arrives as the internal network's gateway IP (the pinned 172.19.0.1), so
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
| argus-tts | same image, `entrypoint:` override | internal network (172.19.0.29), loopback 7029 publish; models/tts subpath ro; `/health` healthcheck |
| argus-stt | same image, `entrypoint:` override | internal network (172.19.0.30), loopback 7030 publish; models/stt subpath ro; `/health` healthcheck |
| argus-vlm | same image, `entrypoint:` override | internal network (172.19.0.31), loopback 7031 publish; models/vision subpath ro; `/dev/dri`; `/health` healthcheck |
| argus-llm | same image, `entrypoint:` override | internal network (172.19.0.32), loopback 7032 publish; models/llm subpath ro; `/health` healthcheck |
| argus-memory | same image, `entrypoint:` override | internal network (172.19.0.33), loopback 7033 publish; memory-db volume (single owner); data dir + camera-db ro opens; models/memory + models/extract ro; gated on nats; `/health` healthcheck |
| argus-relay | same image, `entrypoint:` override | `profiles: [tunnel]`; internal network, loopback 7100/7101/7103 publishes; no database (Ruling CL); `/health` healthcheck |
| argus-tunnel-client | same image, `entrypoint:` override | `profiles: [tunnel]`; host-networked like the gateway (dials the gateway `[remote]` listener and the relay's loopback home publish on 127.0.0.1); no database (Ruling CL); `/health` healthcheck |
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

Fase 4 (Rulings CB/CC/CD/CE, compose v4) adds the five AI engine services:

- `argus-tts` (7029), `argus-stt` (7030), `argus-vlm` (7031) and `argus-llm`
  (7032) are pure internal-wire RPC servers: no database, no JWT, no bus
  consumer. `argus-memory` (7033) owns memory.db, subscribes the change
  subjects over NATS and dials argus-llm for its worker chat. None of them is
  reachable from the gateway — the AI wire is internal-only and the gateway
  proxies NOTHING new (Ruling CE): its config and compose block are unchanged
  from v3.
- **Two topologies (Ruling CC).** The gates live in the legacy's
  `config.legacy.toml` (`[tts]/[stt]/[vision]/[llm]/[memory] remote_url`).
  (a) all-legacy in-process: gates empty — the five services boot idle and
  every engine runs inside the legacy, exactly as compose v3; restore the v3
  legacy budget with `ARGUS_BACKEND_MEMORY_LIMIT=6g
  ARGUS_BACKEND_CPU_LIMIT=4.00`. (b) engines-offloaded (the shipped target
  state): gates at the loopback publishes `http://127.0.0.1:7029`,
  `:7030`, `:7031` (URL form) and `127.0.0.1:7032`, `127.0.0.1:7033`
  (host:port form — the llm/memory raw-socket clients resolve IPv4 literals
  only) — the legacy skips boot-loading the engines entirely. There is no
  in-process fallback once a gate is set; a down engine degrades that leg
  (voice turn loses STT/LLM/TTS, camera talk 502s, memory tools degrade).
  The gate URLs ride the instance config because `ConfigService` has no env
  plumbing and an entrypoint config generator would change the legacy block
  the F3-3 review pinned byte-identical (Ruling CE); the limits and ports ARE
  env-driven. `depends_on` on the AI services is deliberately NOT set on the
  legacy/gateway: the gates default to unset behavior and remote legs degrade
  instead of failing boot.
  **Upgrading from compose v3:** a pre-F4 `config.legacy.toml` carries no
  gate keys, so a compose-only upgrade yields topology (a) (all engines
  in-process) under the NEW 2g/1.50 default budget — an OOM/thrash risk. Set
  the five `remote_url` gates (move to topology (b)), or restore the v3
  budget with `ARGUS_BACKEND_MEMORY_LIMIT=6g` /
  `ARGUS_BACKEND_CPU_LIMIT=4.00`.
- **Boot order (Ruling CC).** Only `argus-memory` has a bus consumer, so only
  it gates on `nats: service_healthy`. The other four have no dependency at
  all (they publish nothing and consume nothing). The legacy and the gateway
  keep their v3 dependencies unchanged.
- **Resource limits (Ruling CD).** The per-service mem_limit/cpus pair
  replaces the legacy's llamaMutex as the engine budget boundary. Derived
  from the ThreadBudget defaults on this reference host (16 hardware threads:
  compute 8, batch 8, heavy 12, light 4, tts 8) and the model footprints:
  argus-tts 2g/1.50, argus-stt 2g/1.50, argus-vlm 2g/2.00, argus-llm
  4g/2.50, argus-memory 2g/1.50 — argus-vlm and argus-llm carry explicit
  DISTINCT values (they never share a cpus pool implicitly). The legacy's
  shipped default DROPS to 2g/1.50 (it keeps face + VAD + drivers only);
  topology (a) restores 6g/4.00 via env. Every value is env-overridable
  (`ARGUS_TTS_MEMORY_LIMIT`, `ARGUS_LLM_CPU_LIMIT`, ...).
- `/dev/dri` is mounted into `argus-vlm` (llama.cpp Vulkan backend, F2-1
  pattern). A GPU-less host drops the device with a `devices: !override []`
  compose override and the engine degrades to CPU in-binary. `argus-llm`
  ships without the device (Ruling CD scopes it to argus-vlm): with
  `gpu_layers = -1` it runs CPU; an operator adding the device pins
  `gpu_layers = 999` in `config.llm.toml`.
- **Models (Ruling CB).** Per-service read-only subpath binds, never the
  whole tree: `models/tts` → argus-tts, `models/stt` → argus-stt,
  `models/vision` → argus-vlm (the GGUF + its mmproj projector),
  `models/llm` → argus-llm, `models/memory` + `models/extract` →
  argus-memory.
- **memory.db single-owner exception (Ruling CB).** The
  `argus-cutover-memory-db` volume is mounted rw into argus-memory and into
  NO other service — the F4-6 replica architecture means nothing else reads
  it (verified: the legacy with `memory.remote_url` set never initializes the
  memory stack, so it never opens memory.db; the gateway has no memory
  client at all). This breaks the shared-volume pattern of camera.db /
  productivity.db / notification.db ON PURPOSE: memory.db is private state of
  the semantic graph, not a synced projection the gateway reads. There is no
  `memory-init` profile and no migrate tool: boot-apply of
  `database/memory-schema.sql` belongs to argus-memory alone, and the
  memory tables in the repo's argus.db are empty schema (the in-process
  topology (a) stack writes there; nothing to migrate). Disclosed honestly:
  no DDL sidecar exists for memory.db and none is needed.
- **Static IPv4 addresses.** The internal network now pins
  `172.19.0.0/24` and the five AI services carry fixed addresses
  (.29/.30/.31/.32/.33 matching their ports): argus-memory's `[llm]
  remote_url` must be an IPv4 literal (`172.19.0.32:7032`) because the
  internal raw-socket wires resolve literals only (no DNS). The legacy gates
  do NOT use these addresses — they dial the loopback publishes. Operator
  note: 172.19.0.0/24 sits inside docker's default address pool
  (172.16.0.0/12), so another compose project that already allocated a
  172.19.x range makes `up` fail with an overlap error, and pinning a network
  that was previously auto-assigned forces network/container recreation on
  existing installs.
- The `argus-memory` snapshot sources reuse the established mounts-with-ro-
  opens discipline: the shared data dir (`database/identity.db`) and the
  camera-db volume (`camera/camera.db`) mount rw (a WAL reader must map the
  `-shm`) and open `mode=ro` in-binary. On a fresh install argus-camera
  creates camera.db in parallel, so the camera snapshot fill skips on first
  boot and fills on a later argus-memory restart (per-table emptiness gate).

## Tunnel transport (Fase 5, Rulings CF/CG/CI/CL)

The `tunnel` profile carries the byte-transparent remote transport:

- `argus-relay` is the device-facing entry point (device 7100 for the app,
  home 7101 for the single client link, `/health` 7103). Its block is
  standalone-deployable: lift it minus its `depends_on` (no broker exists
  there) onto a US server with its `config.relay.toml` and the
  `argus-cutover:local` image — it needs no other compose service unless
  `[push]` intents are wanted, and a US deployment fronts the plain 7100
  device listener with its own TLS terminator (the app keeps its normal
  pinned-CA tunnel toward the relay hostname, whose DNS SAN is baked into
  the leaf via `[remote] hostname`).
- `argus-tunnel-client` runs on the home topology and dials OUT: the relay's
  home listener, then the gateway's `[remote] tunnel_port` per stream. It is
  host-networked like the gateway (same transitional Ruling O exception) so
  the loopback publishes reach it; it publishes nothing.
- Both binaries refuse to start with an empty `[tunnel] secret`, which is
  what keeps the pair inert until the instance configs are filled — that, and
  the profile, is the default-off shape.
- Resource limits follow the Ruling CD pattern with tunnel-sized defaults
  (`ARGUS_RELAY_MEMORY_LIMIT` 256m / 0.50 cpu, same for the client): the
  epoll engines are single-threaded and keep no database (Ruling CL).
- Deviation, documented: the brief's "gateway tunnel listener env-driven"
  is realized as config-file-driven. ConfigService has no env plumbing
  (the F4-7 Ruling CE adjudication), so the gateway's remote listener port
  rides the instance `[remote] tunnel_port` in `config.gateway.toml` — the
  compose environment drives the tunnel pair's published ports instead.
- The gateway's `[remote]` listener must stay unpublished from the host's
  other interfaces (it rides the host network with the gateway): it is the
  remote-facing door, and the relay — not the network — is the entry point.

## Two-topology truth table

| Aspect | (a) all-legacy in-process | (b) engines-offloaded (shipped) |
|---|---|---|
| legacy `[tts]/[stt]/[vision]/[llm]/[memory] remote_url` | empty | `http://127.0.0.1:7029/7030/7031`, `127.0.0.1:7032/7033` |
| legacy budget | `ARGUS_BACKEND_MEMORY_LIMIT=6g ARGUS_BACKEND_CPU_LIMIT=4.00` | default 2g/1.50 |
| five AI services | boot idle (healthchecks still pass) | serve the internal wires |
| compose file | the same file, no override | the same file |
| degradation when an engine container is stopped | n/a (engine is in-process) | that leg degrades (voice/turn, talk 502, memory tools ok=false); never a crash, never a boot failure |

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
- argus-tts / argus-stt / argus-vlm / argus-llm / argus-memory:
  `curl -fs http://127.0.0.1:7029..7033/health` (envelope 200, container-local).
- argus-relay (tunnel profile): `curl -fs http://127.0.0.1:7103/health`
  (envelope 200, container-local; the relay binds its health listener on all
  interfaces so a US deployment can probe it remotely).
- argus-tunnel-client (tunnel profile): `curl -fs
  http://127.0.0.1:7104/health` (envelope 200, container-local; the client
  binds health on host loopback only).

## Volume map

| Volume | Mounted into | Content |
|---|---|---|
| `argus-cutover-camera-db` | argus-camera, legacy, gateway — all at `/opt/argus/camera` (rw) | camera.db (+ WAL files) |
| `argus-cutover-productivity-db` | argus-productivity (rw, owner), gateway (rw mount, mode=ro open) — at `/opt/argus/productivity` | productivity.db (+ WAL files) |
| `argus-cutover-notification-db` | argus-notification (rw), gateway (rw, read-write client) — at `/opt/argus/notification` | notification.db (+ WAL files) |
| `argus-cutover-memory-db` | argus-memory ONLY (rw, single owner) — at `/opt/argus/memory` | memory.db (+ WAL files); the volume-map exception (Ruling CB) |
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
  `config.notification.toml.example` / `config.tts.toml.example` /
  `config.stt.toml.example` / `config.vlm.toml.example` /
  `config.llm.toml.example` / `config.memory.toml.example` /
  `config.tunnel.toml.example` / `config.relay.toml.example` encode the
  cutover keys; copy to `config.gateway.toml` / `config.legacy.toml` /
  `config.camera.toml` / `config.productivity.toml` /
  `config.notification.toml` / `config.tts.toml` / `config.stt.toml` /
  `config.vlm.toml` / `config.llm.toml` / `config.memory.toml` /
  `config.tunnel.toml` / `config.relay.toml`
  (gitignored, mode 0600 — the instance files carry real HMAC/JWT keys, so
  copy with `install -m 600` or `chmod 600` after copying) and fill:
  `[jwt] secret/refresh_secret` and
  `[device] fingerprint_secret` (identical in all five — the gateway mints,
  the legacy, argus-camera and the two Fase 3 services verify, and the device
  hash must match across the proxy), the `[tunnel] secret` (identical in the
  two tunnel templates — the HMAC home-link key; empty keeps the pair from
  booting, and it is never baked into any layer or template),
  `[storage.s3]` in the legacy (the
  `argus_s3_*` service-account credentials rustfs-init creates from the docker
  secrets), plus region/bucket, and the `device.trusted_proxy_ips` of every
  bridge-networked service (argus-camera, argus-productivity,
  argus-notification — the internal network's gateway IP, see the network
  section). The gateway's `[productivity] db` /
  `[notifications] db` point at the volume mounts
  (`productivity/productivity.db`, `notification/notification.db`).
  The five AI service configs carry NO secrets at all (no JWT, no device
  filter): the Fase 4 instance files are pure engine knobs, the only
  per-install choices being the offload topology (`[tts]/[stt]/[vision]/
  [llm]/[memory] remote_url` in config.legacy.toml, set to the compose-shape
  loopback publishes by the shipped target state) and the GPU pins.
- Docker secrets stay the existing `docker/runtime/secrets/*` files: nothing
  new, nothing baked into images.
- Fase 5 (Rulings CG/CJ): the gateway template gains `[remote]`
  (`tunnel_port`, default 0 = disabled) and `[rate_limit]` (`enabled`,
  default false) — both default-off so the shipped default is
  behavior-identical. `tunnel_port` opens the SECOND gateway listener that
  the argus-tunnel client (Fase 5) will forward to; requests landing on it
  are classified remote (by local-port match, `remote_ctx` request
  attribute — the blueprint's `network.lan_cidrs` is deliberately NOT
  introduced because behind the byte-transparent relay every remote peer
  is the tunnel and CIDR matching is meaningless) and `/pairing` +
  `/auth/register` answer `403 REMOTE_NOT_ALLOWED` unless
  `[remote] enabled = true`. The listener mirrors the public one's TLS
  posture. Fase 5 (Ruling CI) also adds `[remote] hostname` (default empty
  = unchanged certificate output): when set it is appended as a DNS SAN to
  the instance leaf so the app can configure that hostname as its manual
  remote server; the next leaf rotation bakes it in — a restart alone
  regenerates the leaf only when it is within
  `cert.rotation_threshold_days` of expiry, so with a young leaf the SAN
  waits for the periodic rotation loop (or a forced rotation via
  `rotateServerCertificate()`), and once the leaf is re-signed the
  running gateway hot reloads it. `[rate_limit]` is the gateway's
  in-memory limiter + lockout for
  `PATCH /auth/refresh-token` (429 frozen envelope before any DB access);
  all limiter state is process-local and a restart clears it. The gateway
  template must keep `device.trust_forwarded_for` off: the limiter key
  includes the client IP, so enabling it there would let a remote client
  spoof the IP half of its own key.
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
| 7029 plain | 127.0.0.1 (compose publish) | argus-tts (internal, legacy gate upstream — never proxied by the gateway) |
| 7030 plain | 127.0.0.1 (compose publish) | argus-stt (internal, legacy gate upstream) |
| 7031 plain | 127.0.0.1 (compose publish) | argus-vlm (internal, legacy gate upstream) |
| 7032 plain | 127.0.0.1 (compose publish) | argus-llm (internal, legacy gate + argus-memory worker chat upstream) |
| 7033 plain | 127.0.0.1 (compose publish) | argus-memory (internal, legacy gate upstream) |
| 4222 | 127.0.0.1 | nats client |
| 8222 | 127.0.0.1 | nats monitor |
| 7100 plain | 127.0.0.1 (compose publish) | argus-relay device listener — the app's manual remote server entry point (tunnel profile) |
| 7101 plain | 127.0.0.1 (compose publish) | argus-relay home listener — the single client link (tunnel profile) |
| 7103 plain | 127.0.0.1 (compose publish) | argus-relay `/health` (tunnel profile) |
| 7104 plain | 127.0.0.1 (host network, container binds loopback) | argus-tunnel-client `/health` (tunnel profile) |
| 9000 | 127.0.0.1 | rustfs S3 |
| 1984 / 8554 | container loopback only | go2rtc spawned by argus-camera (Ruling AH — never published) |
| 8800 | host | Tapo talk channel (camera-side, legacy `[tapo]`) |
| `[remote] tunnel_port` TLS | gateway host/container port | gateway remote listener (default 0 = disabled; the instance sets 7034 when the tunnel profile is on — the listener is config-file-driven, not env-driven, see the tunnel section) |
