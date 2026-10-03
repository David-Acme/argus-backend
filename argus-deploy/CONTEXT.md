# argus-deploy — CONTEXT

Compose v7 of the migration plan: the Fase 1 cutover stack (nats +
identity init), the Fase 2 argus-camera service (camera.db volume +
camera-init), the Fase 3 argus-productivity + argus-notification services
(productivity.db / notification.db volumes + their init profiles), the Fase 4
AI engine services argus-tts/argus-stt/argus-vlm/argus-llm (models
subpaths; the single-owner memory.db volume stays declared unmounted since
f8-b3, when argus-memory stopped being a process and became a package
hosted by argus-llm — one of its features since Phase 4 step 7), the Fase 5 tunnel
transport pair — argus-relay + argus-tunnel-client — behind the opt-in
`tunnel` profile and the F6-3 argus-voice pure-gRPC service. The Fase 1
legacy service and the root local compose's RustFS pair are retired (F6-4).
Every domain is served by its own service, and since Phase 3d step 1c no
process proxies another's routes: each app-facing service terminates TLS
itself on its own listener and announces its own `_argus-route._tcp`
instances.
Decisions and traps live here.

## Images (Ruling N, revised F10)

Every microservice owns `services/<name>/Dockerfile`: a Debian + Conan
2.21.0 build stage runs `scripts/build-all.sh prod --no-tests --only
argus-<name>` from the repo root, and a slim runtime stage carries only that
service's binaries. Packages are reusable libraries compiled into the service
images — no package has an image of its own. The identity image carries
`argus-migrate-identity` (identity is its owner, since Phase 3c-1) and the
argus-sync image carries `argus-migrate-sync` (the Phase 3c-2 split of the five
sync tables out of identity's file); argus-camera
carries `argus-migrate-camera` and `argus-vulkan-probe`; productivity and
notification carry their migration tools; the argus-tunnel image carries the
client and the relay. The argus-memory binary is gone since f8-b3, and its
package dissolved into argus-llm at Phase 4 step 7: the whole stack is one of
its features now.

Build all images from the repository root, sequentially:

    COMPOSE_PARALLEL_LIMIT=1 docker compose \
      -f argus-deploy/docker-compose.yml \
      --profile tunnel --profile identity-init build

Parallel builds collide in the shared Conan package cache ("Reference ...
already exists"), so builds run one at a time. The Dockerfile-specific
ignores next to each Dockerfile (`services/*/Dockerfile.dockerignore`)
keep the nested build trees out of the context. Each project resolves its own
Conan dependency set. The memory stack needs no
extra runtime packages: sqlite-vec compiles
into the binary (`SQLITE_CORE`), onnxruntime/llama/cnats are static conan
archives and the runtime stage's `libgomp1`/`libstdc++6` already cover them.

`-march=native` applies (AGENTS.md Release tuning): build on the machine that
runs the stack — the self-hosted model — or the binary may SIGILL elsewhere.
`-flto=auto` + many translation units make the build heavy; expect a long
first build (the conan cache is a buildkit cache mount, so rebuilds are fast).

The build stage installs `libvulkan-dev` + `glslc`: ncnn is integrated with
`NCNN_VULKAN=ON` (the camera project's vendored integration) and compiles its Vulkan shaders
with `glslc` at build time. Without them the ncnn build silently drops its
Vulkan backend in the container and the argus-camera detector could never
engage RADV (it would only ever run CPU).

Runtime image adds `curl` (every app-facing service's `/health` healthcheck)
and `mesa-vulkan-drivers` (RADV, so a container with `/dev/dri` can drive the
GPU). The argus-camera image also carries
`argus-vulkan-probe` (Fase 2 Vulkan gate): it reuses ncnn's own Vulkan init
and exits 0 only when `vkCreateInstance` plus at least one physical device
work; otherwise the detector stays on its Vulkan→CPU fallback. Run it with
`docker compose --profile vulkan-probe run --rm vulkan-probe`, which mounts
`/dev/dri` only — no `group_add` — and worked on this host (the device
cgroup plus the host's device ACLs let `--user 1000:1000` reach both nodes;
`renderD128` is 0666 and `card1` carries an ACL). Plain-docker equivalent,
also verified here: `docker run --rm --device /dev/dri --user 1000:1000
--entrypoint /opt/argus/argus-vulkan-probe argus-camera:local`. On hosts
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

## Network shape

Every service is on the `internal` bridge network. There is no gateway and no
host-networked public surface: Phase 3d step 1c deleted the process that used
to hold the only public port, and each app-facing service now terminates TLS
with the instance certificate on its own listener, announced over mDNS as one
`_argus-route._tcp` instance per logical route (Phase 3d step 1b). The app
resolves those instances and dials them directly; Phase 3d step 5 verifies it
against a real client.

- **The app-facing listeners are published on the LAN**, each under an
  overridable host port: `"${AUTH_PORT:-7042}:7042"`,
  `"${IDENTITY_PORT:-7044}:7044"`, `"${SYNC_PORT:-7025}:7025"`,
  `"${CAMERA_PORT:-7026}:7026"`,
  `"${PRODUCTIVITY_PORT:-7027}:7027"`,
  `"${NOTIFICATION_PORT:-7028}:7028"`,
  `"${GUARD_PORT:-7039}:7039"` and `"${SETTINGS_PORT:-7045}:7045"`. Every one of them serves `/health` over TLS,
  which is what the healthchecks curl (`curl -kfs https://127.0.0.1:<port>/health`
  from inside the container).
- **The internal wires stay `127.0.0.1:`-published**: auth's RPC 7043,
  identity's gRPC 7040, sync's control 7041, camera's gRPC 7036, productivity's
  gRPC 7037, notification's RPC 7038, voice's gRPC 7034 and `/health` 7035,
  camera's go2rtc 1984/8554 (Ruling AH — always INSIDE the container, no
  compose service and no host publish) and the tunnel set 7100/7101/7103.
  Loopback-only publishes keep them unreachable from the LAN; the publishes
  exist for host-side reachability.
- **nats** (`nats:2.11.14-alpine`, core NATS, monitor port 8222) lives on the
  `internal` bridge network, published on host loopback (4222/8222, overridable
  via `NATS_CLIENT_PORT`, `NATS_MONITOR_PORT`) for host-side reachability.
- **argus-camera** (Fase 2) serves its whole domain itself: `/camera` and
  `/zone` at every segment depth, and the client-facing `/media` socket
  (`services/camera/src/feature/media/camera-media-socket.cc`) that carries
  `camera:*` frames and fMP4 over the published 7026 listener; argus-sync
  pulls the camera sync tables from the 7036 gRPC listener (F6-5, repointed at
  the sync split in Phase 3c-2). Its `[nats] url` points at the internal alias
  `nats://nats:4222`. Talk synthesis reaches argus-tts through the camera
  config's `[tts] remote_url` (loopback 7029).
- **argus-productivity / argus-notification** (Fase 3, compose v3) live on the
  same `internal` bridge network with their 7027/7028 app-facing listeners
  published on the LAN and their gRPC listeners (7037/7038) on host loopback;
  their `[nats] url` points at the internal alias as well.
- **The four AI engine services** (Fase 4, compose v4) live on the same
  `internal` bridge network, which pins `172.19.0.0/24` so they carry
  static addresses (argus-tts .29, argus-stt .30, argus-vlm .31, argus-llm
  .32, argus-voice .34; .33 left the map with the argus-memory process,
  f8-b3). The remote wires dial the static literals (argus-voice dials
  argus-stt/argus-tts/argus-llm) because the internal raw-socket
  wires resolve IPv4 literals only. Loopback-only publishes keep every AI
  wire unreachable from the LAN — "internal, no host publish" means no
  non-loopback exposure; the publishes exist for host-side reachability.
- Trust note, now sharper than when the gateway wrote the header: a service
  resolves the caller IP from `X-Forwarded-For` only for a trusted peer
  (loopback always, otherwise `device.trusted_proxy_ips`), and with no HTTP
  proxy in front of the LAN publishes the only peer a service sees for a
  remote client is Docker's own forwarding address — the pinned bridge gateway
  172.19.0.1 — so `device.identity_mode = "ip"` fingerprints that address and
  the user agent rather than the client. `config.camera.toml` lists
  172.19.0.1 in `device.trusted_proxy_ips` for exactly this reason. A host
  that disables Docker's userland proxy preserves the client address instead.
  Phase 5 step 6's real-client verification is where this is settled; the
  credential mode (`device.identity_mode = "credential"`, the
  `X-Argus-Device-Credential` header) is the identity path that does not
  depend on the peer address at all.

## Services

| Service | Image | Notes |
|---|---|---|
| argus-auth | `argus-auth:local` | internal network (alias `argus-auth`), LAN 7042 + loopback 7043 publishes; owns auth.db; runs the refresh limiter, gates `/pairing` and `/auth/register` against remote requests (`RemoteGate`) and mints the device credentials and the sessions every other service validates; `/health` healthcheck |
| argus-identity | `argus-identity:local` | internal network (alias `argus-identity`), LAN 7044 + loopback 7040 (fleet-secret RPC) publishes; owns identity.db; the users, persons, face embeddings, invitations and private portraits; carries the same `RemoteGate` on `/pairing` and `/auth/register`; `/health` healthcheck; config bind rw (the pairing state persists) |
| argus-camera | `argus-camera:local` | internal network, LAN 7026 + loopback 7036 (sync gRPC) publishes; owns camera.db; `/health` healthcheck; `/dev/dri` |
| argus-productivity | `argus-productivity:local` | internal network, LAN 7027 + loopback 7037 (sync gRPC) publishes; owns productivity.db; `/health` healthcheck |
| argus-notification | `argus-notification:local` | internal network, LAN 7028 + loopback 7038 (RPC) publishes; owns notification.db; `/health` healthcheck |
| argus-guard | `argus-guard:local` | internal network, LAN 7039 publish; owns guard.db; no gRPC listener; `/health` healthcheck |
| argus-settings | `argus-settings:local` | internal network (alias `argus-settings`), LAN 7045 publish; no database and no data directory; the owner-only `/settings` surface, which reads every settings owner's catalog over `argus.settings.v1` (`[owners.<name>] target`/`credential`) and forwards changes to it; `/health` healthcheck |
| argus-tts | `argus-tts:local` | internal network (172.19.0.29), loopback 7029 publish; models/tts subpath ro; config bind rw (settings owner); `/health` healthcheck |
| argus-stt | `argus-stt:local` | internal network (172.19.0.30), loopback 7030 publish; models/stt subpath ro; config bind rw (settings owner); `/health` healthcheck |
| argus-vlm | `argus-vlm:local` | internal network (172.19.0.31), loopback 7031 publish; models/vision subpath ro; `/dev/dri`; config bind rw (settings owner); `/health` healthcheck |
| argus-llm | `argus-llm:local` | internal network (172.19.0.32), loopback 7032 publish; models/llm subpath ro; carries the memory stack as a feature since Phase 4 step 7 (its stack hosting landed at f8-b4); config bind rw (settings owner); `/health` healthcheck |
| argus-voice | `argus-voice:local` | internal network (172.19.0.34), loopback 7034 (gRPC) + 7035 (`/health`) publishes; no database; models/vad ro; gated on nats; config bind rw (settings owner); `/health` healthcheck |
| argus-relay | `argus-tunnel:local` | `profiles: [tunnel]`; internal network, loopback 7100/7101/7103 publishes; no database (Ruling CL); `/health` healthcheck |
| argus-tunnel-client | `argus-tunnel:local` | `profiles: [tunnel]`; host-networked (it dials the remote listener a home service opens and the relay's loopback home publish on 127.0.0.1); no database (Ruling CL); `/health` healthcheck |
| nats | `nats:2.11.14-alpine` | exact tag pin; core NATS (no JetStream needed) |
| identity-init | `argus-identity:local` | `profiles: [identity-init]`, runs `argus-migrate-identity` |
| sync-init | `argus-sync:local` | `profiles: [sync-init]`, runs `argus-migrate-sync` (identity.db → sync.db), identity's directory read-only |
| sync-rollback | `argus-sync:local` | `profiles: [sync-rollback]`, runs the same tool with the paths swapped (sync.db → identity.db), sync's directory read-only and identity's writable |
| camera-init | `argus-camera:local` | `profiles: [camera-init]`, runs `argus-migrate-camera` against the camera data directory |
| productivity-init | `argus-productivity:local` | `profiles: [productivity-init]`, runs `argus-migrate-productivity` against the productivity data directory |
| notification-init | `argus-notification:local` | `profiles: [notification-init]`, runs `argus-migrate-notification` against the notification data directory |
| vulkan-probe | `argus-camera:local` | `profiles: [vulkan-probe]`, runs `argus-vulkan-probe` with `/dev/dri` |

Ordering: `nats` goes healthy first and every service that carries a bus waits
for `nats: service_healthy` — the initial connect has no retry, so a lost boot
race would leave the bus disabled, its subscriptions silently absent while all
healthchecks stay green. They then boot in parallel and each opens only its own
database: argus-identity creates identity.db from its own schema and serves the
fleet-secret 7040 leg, argus-sync owns sync.db and pulls the camera,
notification and productivity sync tables over their owners' gRPC legs, and
argus-notification owns the camera notification policy that used to be the
gateway's (Phase 3d step 1a — there is no gateway since step 1c). No service
waits on another's health, so there is no cycle. A fresh
`up -d` without camera-init therefore works end to end: argus-camera creates
camera.db and serves both the CRUD routes and the sync gRPC pulls with live
rows — but
argus-camera's boot apply then makes camera.db live data, so `camera-init`
can no longer migrate the pre-existing argus.db camera rows (it no-ops on any
schema-current target before reading the source). An installation that wants
the legacy camera rows migrated must run `docker compose --profile camera-init
run --rm camera-init` BEFORE the first boot, while camera.db does not exist
yet. Every service bind-mounts its own owner `database/schema.sql`
(`services/identity`, `services/camera`, ...) at the data-dir
`database/schema.sql` path, so a data dir provisioned without schema SQLs
still works (the single-file binds come from the repo). argus-identity
applies `database/schema.sql` at boot and
aborts if it fails, so a fresh install creates `identity.db` without the init
profile; argus-camera applies its own `database/schema.sql` at boot the same
way, so a fresh install creates camera.db without `camera-init`. On an existing
installation run `docker compose --profile identity-init run --rm
identity-init` (idempotent, guards intact: refuses same-path, requires the 7
source tables, skips cleanly when `argus.db` does not exist yet). Do not run
either init tool while the services hold its target database open — stop the
stack first.

Fase 3 (Rulings AT/AU/AV) extends the same shape to productivity.db and
notification.db, each in its own data subdirectory bind-mounted from
`${ARGUS_DATA_DIR:-./data}`:

- `argus-productivity` mounts `${ARGUS_DATA_DIR:-./data}/productivity` rw at
  `/opt/argus/productivity` and applies
  `database/schema.sql` at boot, then serves `argus.productivity.v1.SyncService`
  on 7037. No one else mounts it (rule 27): `argus-sync`'s `/sync`
  pulls for the 7 tables go over that gRPC leg.
- `argus-notification` mounts `${ARGUS_DATA_DIR:-./data}/notification` rw at
  `/opt/argus/notification` and serves
  `argus.notification.v1.NotificationService` on 7038. Its own camera notifier
  creates through the in-process service (Phase 3d step 1) and `argus-sync`'s
  `/sync` notification pulls use `PullNotifications`; the directory is mounted
  by no one else (rule 27).
- `productivity-init` / `notification-init` are the only migration paths
  onto those directories and MUST run BEFORE the first boot (the f8291e4
  lesson, same as camera-init): once the owning service has boot-applied
  the schema the directory is live data and the migrate tool correctly
  no-ops instead of resurrecting argus.db rows over it. On an existing
  installation:
  `docker compose --profile productivity-init run --rm productivity-init`
  (and the notification twin) with the stack stopped; both are idempotent and
  no-op on a schema-current target.
- Boot order (Ruling AV): nats goes healthy first; the services that carry a
  bus then boot in parallel — every one of them gates on
  `nats: service_healthy` because each connects its NatsBus once at boot with
  no retry. argus-productivity and argus-notification no longer wait for an
  identity.db another process created (the people authority is argus-identity's
  own service, Phase 3b-2), and no service waits on another's health, so
  there is no cycle.

Fase 4 (Rulings CB/CC/CD/CE, compose v4) adds the four AI engine services:

- `argus-tts` (7029), `argus-stt` (7030) and `argus-vlm` (7031) are pure
  internal-wire RPC servers: no JWT, no bus consumer and no database.
  `argus-llm` (7032) speaks the same internal wire, but since f8-b4 it also
  consumes the bus (the guard encounter-closed stream, durable
  `argus-llm-encounters`) and hosts the memory stack: memory.db under its
  `${ARGUS_DATA_DIR:-./data}/memory` mount, plus the
  `services/llm/database/schema.sql`, `models/memory` and `models/extract`
  binds. argus-memory (7033) is retired since f8-b3: the memory capacity is
  a feature of argus-llm and the worker chat is an in-process
  call. None of them is
  reachable from the LAN — the AI wire is internal-only and loopback-published.
  The in-process engine topology is retired
  (F6-4): no service carries engines and every engine consumer dials a
  remote gate.
- **Remote gates (Ruling CC, post-retirement shape).** The gates live in the
  consumer instance configs: argus-voice's
  `[stt]/[tts]/[llm] remote_url` (static internal literals) and
  argus-camera's `[tts] remote_url`. argus-memory's `[llm] remote_url` died
  with the process (f8-b3): the memory worker's chat is an in-process call
  inside argus-llm. There is no in-process fallback once a gate is set; a
  down engine degrades that leg (voice turn loses STT/LLM/TTS, camera talk
  502s). The gate URLs ride the instance config because `ConfigService`
  has no env plumbing (Ruling CE); the limits and ports ARE env-driven.
  `depends_on` on the AI services is deliberately NOT set on the consumers:
  a down engine degrades its leg instead of failing boot.
- **Boot order (Ruling CC).** Since f8-b3 none of the Fase 4 four gates on
  `nats: service_healthy` (they publish nothing and consume nothing); the
  bus consumer returns with f8-b4, when argus-llm hosts the memory catalog
  replica and gates on nats. argus-voice
  (F6-3) also carries a bus consumer and gates the same way; no other
  app-facing service carries a bus.
- **Resource limits (Ruling CD).** The per-service mem_limit/cpus pair is
  the engine budget boundary. Derived from the ThreadBudget defaults on this
  reference host (16 hardware threads: compute 8, batch 8, heavy 12, light 4,
  tts 8) and the model footprints: argus-tts 2g/1.50, argus-stt 2g/1.50,
  argus-vlm 2g/2.00, argus-llm 4g/2.50 (argus-memory's 2g/1.50 retired with
  the process at f8-b3; argus-llm absorbs the memory workload at f8-b4) —
  argus-vlm and argus-llm carry explicit DISTINCT values (they never share
  a cpus pool
  implicitly). Every value
  is env-overridable (`ARGUS_TTS_MEMORY_LIMIT`, `ARGUS_LLM_CPU_LIMIT`, ...).
- `/dev/dri` is mounted into `argus-vlm` (llama.cpp Vulkan backend, F2-1
  pattern). A GPU-less host drops the device with a `devices: !override []`
  compose override and the engine degrades to CPU in-binary. `argus-llm`
  ships without the device (Ruling CD scopes it to argus-vlm): with
  `gpu_layers = -1` it runs CPU; an operator adding the device pins
  `gpu_layers = 999` in `config.llm.toml`.
- **Models (Ruling CB).** Per-service read-only subpath binds, never the
  whole tree: `models/tts` → argus-tts, `models/stt` → argus-stt,
  `models/vision` → argus-vlm (the GGUF + its mmproj projector),
  `models/llm` → argus-llm. `models/memory` + `models/extract` are
  argus-llm binds since f8-b4, which hosts the memory stack (their
  argus-memory binds are gone with the process, f8-b3).
- **memory.db single-owner exception (Ruling CB).** memory.db lives in
  `${ARGUS_DATA_DIR:-./data}/memory` (the old `argus-cutover-memory-db`
  named volume is copied there by `scripts/provision-host.sh
  --migrate-volumes`; no `argus-cutover-*-db` volume has been declared
  since f8-b3, argus-memory's retirement) and argus-llm has held the rw
  mount since f8-b4, when it took the memory stack over — still into NO
  other service, the single-owner principle intact (the F4-6 replica
  architecture means nothing else reads it; no app-facing service has a
  memory client at all; argus-voice mounts no databases). This breaks the
  shared-volume pattern of camera.db / productivity.db / notification.db
  ON PURPOSE: memory.db is private state of the semantic graph, not a synced
  projection a reader pulls. There is no `memory-init` profile and no
  migrate tool:
  boot-apply of `database/schema.sql` moved to argus-llm at f8-b4,
  and the memory tables in the repo's argus.db are empty schema (nothing
  to migrate). Disclosed honestly:
  no DDL sidecar exists for memory.db and none is needed.
- **Static IPv4 addresses.** The internal network now pins
  `172.19.0.0/24` and the AI services carry fixed addresses
  (.29/.30/.31/.32/.34 matching their ports; .33 left the map with the
  argus-memory process, f8-b3): the `[llm] remote_url`
  gates must be IPv4 literals (`172.19.0.32:7032`) because the
  internal raw-socket wires resolve literals only (no DNS). Operator
  note: 172.19.0.0/24 sits inside docker's default address pool
  (172.16.0.0/12), so another compose project that already allocated a
  172.19.x range makes `up` fail with an overlap error, and pinning a network
  that was previously auto-assigned forces network/container recreation on
  existing installs.
- The catalog-replica snapshot sources reuse the established
  mounts-with-ro-opens discipline: the shared data dir (`data/identity.db`
  under argus-deploy)
  and the camera-db volume (`camera/camera.db`) mount rw (a WAL reader must
  map the `-shm`) and open `mode=ro` in-binary. The fills move to argus-llm
  at f8-b4 with the rest of the memory stack; their argus-memory mounts are
  gone with the process (f8-b3). On a fresh install argus-camera
  creates camera.db in parallel, so the camera snapshot fill skips on first
  boot and fills on a later host restart (per-table emptiness gate).

## Tunnel transport (Fase 5, Rulings CF/CG/CI/CL)

The `tunnel` profile carries the byte-transparent remote transport:

- `argus-relay` is the device-facing entry point (device 7100 for the app,
  home 7101 for the single client link, `/health` 7103). Its block is
  standalone-deployable: lift it minus its `depends_on` (no broker exists
  there) onto a US server with its `config.relay.toml` and the
  `argus-tunnel:local` image — it needs no other compose service unless
  `[push]` intents are wanted, and a US deployment fronts the plain 7100
  device listener with its own TLS terminator (the app keeps its normal
  pinned-CA tunnel toward the relay hostname, whose DNS SAN is baked into
  the leaf via `[remote] hostname`).
- `argus-tunnel-client` runs on the home topology and dials OUT: the relay's
  home listener, then the home service's remote listener per stream. It is
  host-networked (the transitional Ruling O exception the gateway held before
  it) so the loopback publishes reach it; it publishes nothing.
- Both binaries refuse to start with an empty `[tunnel] secret`, which is
  what keeps the pair inert until the instance configs are filled — that, and
  the profile, is the default-off shape.
- Resource limits follow the Ruling CD pattern with tunnel-sized defaults
  (`ARGUS_RELAY_MEMORY_LIMIT` 256m / 0.50 cpu, same for the client): the
  epoll engines are single-threaded and keep no database (Ruling CL).
- Deviation, documented: the brief's "gateway tunnel listener env-driven"
  is realized as config-file-driven. ConfigService has no env plumbing
  (the F4-7 Ruling CE adjudication), so the remote listener port
  rides the instance `[remote] tunnel_port` — it was `config.gateway.toml`'s
  while the gateway held the edge, and since Phase 3d step 1c it is
  `config.auth.toml`'s and `config.identity.toml`'s, where `RemoteGate` runs
  — the compose environment drives the tunnel pair's published ports instead.
- The remote listener must stay unpublished from the host's other interfaces:
  it is the remote-facing door, and the relay — not the network — is the
  entry point.
- Open consequence for the tunnel's own work (D19 defers it): the client's
  `server.gateway_host`/`server.gateway_port` (`127.0.0.1:7034`) named the
  gateway's remote listener and now names no live listener, because the
  gateway is gone and the home remote listener belongs to argus-auth and
  argus-identity. Nothing in the tree reads those two keys except
  `services/tunnel/src/server/service-config.cc`, which still defaults them to
  `127.0.0.1:7024`. An installation that wants the remote transport must
  repoint the client at the service holding the remote listener it wants, and
  that is the tunnel's own unit of work, not this phase's.

## Engine-degradation truth table

| Aspect | Engines-offloaded (the only shape since F6-4) |
|---|---|
| remote gates | argus-voice `[stt]/[tts]/[llm]`, argus-camera `[tts]` |
| four AI services | serve the internal wires |
| degradation when an engine container is stopped | that leg degrades (voice turn, talk 502); never a crash, never a boot failure |

For acceptance runs, `scripts/seed-golden.py` seeds the golden /sync verify
state into a scratch COPY of the databases (Golden Cam / Golden Zone rows,
`calendar_event.ends_at` NULL, `project_task.assignee_id` 1, the identity
golden rows and a recorder refresh session minted from the config secrets —
secrets are read at runtime, never printed; the refresh token lands in a
0600 file). Run it with the stack stopped, before camera-init.

## Healthchecks

- argus-auth: `curl -kfs https://127.0.0.1:7042/health` (envelope 200).
- argus-identity: `curl -kfs https://127.0.0.1:7044/health` (envelope 200).
- argus-sync: `curl -kfs https://127.0.0.1:7025/health` (envelope 200).
- argus-camera: `curl -kfs https://127.0.0.1:7026/health` (envelope 200).
- argus-productivity: `curl -kfs https://127.0.0.1:7027/health` (envelope 200).
- argus-notification: `curl -kfs https://127.0.0.1:7028/health` (envelope 200).
- argus-guard: `curl -kfs https://127.0.0.1:7039/health` (envelope 200).
- argus-settings: `curl -kfs https://127.0.0.1:7045/health` (envelope 200).
- argus-voice: `curl -fs http://127.0.0.1:7035/health` (envelope 200).
- argus-tts / argus-stt / argus-vlm / argus-llm:
  `curl -fs http://127.0.0.1:7029..7032/health` (envelope 200, container-local).
- argus-relay (tunnel profile): `curl -fs http://127.0.0.1:7103/health`
  (envelope 200, container-local; the relay binds its health listener on all
  interfaces so a US deployment can probe it remotely).
- argus-tunnel-client (tunnel profile): `curl -fs
  http://127.0.0.1:7104/health` (envelope 200, container-local; the client
  binds health on host loopback only).

## Volume map

| Mount | Mounted into | Content |
|---|---|---|
| `${ARGUS_DATA_DIR}/identity` | argus-identity (rw, owner) — at `/opt/argus/database` | identity.db (+ WAL files) |
| `${ARGUS_DATA_DIR}/auth` | argus-auth (rw, owner) — at `/opt/argus/database` | auth.db (+ WAL files) |
| `${ARGUS_DATA_DIR}/sync` | argus-sync (rw, owner) — at `/opt/argus/database` | sync.db (+ WAL files) |
| `${ARGUS_DATA_DIR}/camera` | argus-camera (rw, owner) — at `/opt/argus/camera` | camera.db (+ WAL files) |
| `${ARGUS_DATA_DIR}/productivity` | argus-productivity (rw, owner) — at `/opt/argus/productivity` | productivity.db (+ WAL files) |
| `${ARGUS_DATA_DIR}/notification` | argus-notification (rw, owner) — at `/opt/argus/notification` | notification.db (+ WAL files) |
| `${ARGUS_DATA_DIR}/guard` | argus-guard (rw, owner) — at `/opt/argus/guard` | guard.db (+ WAL files) |
| `${ARGUS_DATA_DIR}/memory` | argus-llm (rw, owner since f8-b4) — at `/opt/argus/memory` | memory.db (+ WAL files) |
| `argus-cutover-camera-stream` | argus-camera at `/opt/argus/stream` | go2rtc.yaml generated by Go2rtcManager (chmod 600, camera credentials) |
| `${ARGUS_DATA_DIR}/rustfs/objects` | rustfs at `/data` | private S3 objects (portraits, camera evidence, guard incident records) |

Every database directory is bind-mounted from `${ARGUS_DATA_DIR:-./data}`
(default `argus-deploy/data/`, gitignored; `scripts/provision-host.sh` creates
it and writes the gitignored `.env` with absolute host paths) and is mounted
by its owner only (rule 27):
cross-domain
reads travel through the typed gRPC legs (camera/productivity/notification)
and NATS change feeds, never through another service's file. Each owner
applies WAL + busy_timeout 5000 at boot; the `*-init` one-shot tools are the
only other writers and run while the stack is stopped. Compose adds no DDL
sidecar — each schema boot-apply belongs to its owner service alone, and
the matching `*-init` profile is the only migration path onto a volume.

## Configuration and secrets (Ruling Q)

- `config.auth.toml.example` /
  `config.identity.toml.example` / `config.guard.toml.example` /
  `config.sync.toml.example` /
  `config.camera.toml.example` /
  `config.productivity.toml.example` /
  `config.notification.toml.example` / `config.tts.toml.example` /
  `config.stt.toml.example` / `config.vlm.toml.example` /
  `config.llm.toml.example` / `config.voice.toml.example` /
  `config.tunnel.toml.example` / `config.relay.toml.example` encode the
  cutover keys (`config.gateway.toml.example` died with its service in
  Phase 3d step 1c, and `config.memory.toml.example` is deleted since f8-b3:
  the memory feature's keys ride the host's config.llm.toml from f8-b4); copy
  to `config.auth.toml` /
  `config.identity.toml` / `config.guard.toml` /
  `config.sync.toml` /
  `config.camera.toml` / `config.productivity.toml` /
  `config.notification.toml` / `config.tts.toml` / `config.stt.toml` /
  `config.vlm.toml` / `config.llm.toml` /
  `config.voice.toml` /
  `config.tunnel.toml` / `config.relay.toml`
  (gitignored, mode 0600 — the instance files carry real HMAC/JWT keys, so
  copy with `install -m 600` or `chmod 600` after copying) and fill:
  `[jwt] secret/refresh_secret` and
  `[device] fingerprint_secret` (identical in the minting/verifying set —
  argus-auth mints, argus-camera, argus-guard,
  argus-productivity, argus-notification, argus-identity and argus-sync
  verify, and the
  device hash must agree across them), the `[auth] target`
  (`argus-auth:7043`) and `[auth] rpc_secret` each of those verifiers
  carries — the fleet gate on the session verdict, so an absent or
  mismatched key makes the authority refuse every call and leaves all their
  authenticated routes answering 401 — and the `[tunnel] secret` (identical
  in the two tunnel templates — the HMAC home-link key; empty keeps the pair
  from booting, and it is never baked into any layer or template).
  `device.trusted_proxy_ips` carries the internal network's gateway IP in
  every bridge-networked service (argus-auth, argus-camera, argus-productivity,
  argus-notification, argus-guard, argus-identity, argus-sync) together with
  `device.trust_forwarded_for = true`: since Phase 3d step 1c no first-party
  peer writes `X-Forwarded-For`, so the address those services fingerprint is
  the bridge's own forwarding address — read the network section's trust note
  before relying on either key.
  `scripts/lib/common.sh` adopts the wiring keys a config lacks from its own
  template and fills the shared ones from the first non-placeholder value
  the deploy directory holds, so an existing installation repairs itself on
  the next provisioning run. The owning services' `[productivity] db` / `[notifications] db`
  (`config.productivity.toml`, `config.notification.toml`) name
  `productivity/productivity.db` and `notification/notification.db` inside
  their bind-mounted data dirs; no service holds a `db` key for a domain it
  does not own — argus-sync reaches the camera, notification and productivity
  tables through their owners' `grpc_target` legs, as every other
  cross-domain read does.
  The AI service configs carry NO secrets at all (no JWT, no device
  filter): the instance files are pure engine knobs, the only
  per-install choices being the remote gates (`[stt]/[tts]/[llm] remote_url`
  in config.voice.toml — the compose-shape static internal literals — plus
  `[tts] remote_url` in config.camera.toml, the loopback publish), the GPU
  pins, and (from f8-b4) the `[memory]`/`[extract]` blocks config.llm.toml
  gains with the hosted memory feature.
- No docker secrets: nothing is baked into images and instance secrets live
  only in the gitignored config files.
- Fase 5 (Rulings CG/CJ): `[remote]` (`tunnel_port`, default 0 = disabled;
  `enabled`, default false) lives in `config.auth.toml` and
  `config.identity.toml` — default-off so the shipped default is
  behavior-identical. `tunnel_port` appends the SECOND listener to that same
  service (`appendRemoteListener` over the service's own listener shape, so it
  mirrors its TLS posture); requests landing on it are classified remote by
  local-port match (`requestIsRemote` compares `req->localAddr().toPort()`
  with the configured port — the blueprint's `network.lan_cidrs` is
  deliberately NOT introduced because behind the byte-transparent relay every
  remote peer is the tunnel and CIDR matching is meaningless) and `/pairing` +
  `/auth/register` answer `403 REMOTE_NOT_ALLOWED` unless
  `[remote] enabled = true`. That gate is `RemoteGate`, a pre-routing advice
  with CORS applied, in both services' `main.cc`, and
  `requireDistinctTunnelPort` refuses a `tunnel_port` that collides with the
  service's own listener before the config is even loaded. Fase 5 (Ruling CI)
  also adds `[remote] hostname` to `config.identity.toml` alone (default empty
  = unchanged certificate output): identity owns the instance leaf, and
  `instanceSans()` (`packages/lib/cert/src/cert/cert-service.cc`) appends it as
  a DNS SAN so the app can configure that hostname as its manual remote server;
  an invalid hostname is logged and ignored. The next leaf rotation bakes it in
  — a restart alone regenerates the leaf only when it is within
  `cert.rotation_threshold_days` of expiry, so with a young leaf the SAN waits
  for the periodic rotation loop (or a forced rotation via
  `rotateServerCertificate()`). The limiter that Ruling CG put beside that
  gate moved with the surface it guards: `[rate_limit]` lives in
  `config.auth.toml` and is argus-auth's
  in-memory limiter + lockout for
  `PATCH /auth/refresh-token` (429 envelope, CORS applied, before any DB
  access), the `RefreshRateGate` pre-routing advice of that service. All
  limiter state is process-local and a restart clears it. Neither service is
  host-networked any more, so `device.trust_forwarded_for` no longer buys a
  real client address in either of them — the network section's trust note is
  where that stands until Phase 5 step 6 settles it.
- argus-camera spawns go2rtc itself (Go2rtcManager fork/exec, Ruling AH) from
  the bind-mounted `third_party/go2rtc` binary and writes its own
  `go2rtc.yaml` (chmod 600, camera credentials) onto the camera-stream
  volume — never a bind. Its 1984/8554 binds stay inside the container.
- `certs/`, `models/` and `third_party/go2rtc` bind-mount read-only from the
  repo (overridable through `ARGUS_CERTS_DIR`, `ARGUS_MODELS_DIR` and
  `ARGUS_GO2RTC_DIR`); the data directory (`${ARGUS_DATA_DIR:-./data}`) is
  writable (identity.db, WAL files) and holds one subdirectory per owner DB
  (`identity/`, `sync/`, `camera/`, `productivity/`, `notification/`, `guard/`,
  `memory/`), so no container mounts another owner's data.
  `ARGUS_DATA_DIR` exists for acceptance runs on a scratch copy of the
  real data directory; the default lives in `argus-deploy/data/` and is
  gitignored.

## AI engines over authenticated gRPC (2026-10)

Voice, camera and guard used to reach STT, TTS, LLM and VLM over their HTTP
routes, which authenticate no caller and, in the LLM's case, take the
`user_id` whose memories the tools read and write from the request body;
the engines listened on `0.0.0.0` inside the internal network. The deploy
templates now:

- open each engine's gRPC leg (`[rpc] address`: tts 7129, stt 7130, vlm
  7131, llm 7132) with one `[rpc.callers]` credential per caller, which the
  gRPC servers already enforce;
- point the callers at it (`grpc_target` and `grpc_credential` in voice's
  `[stt]`/`[tts]`/`[llm]`, camera's `[stt]`/`[tts]`, guard's new `[vlm]` and
  `[llm]`), the transport the clients prefer whenever a target is set; this
  also replaces camera's HTTP path, whose hand-rolled client could not
  resolve a host name at all;
- bind the engines' HTTP listeners to the container's loopback, where only
  the healthcheck reaches them.

`ensure_deploy_configs` pairs every credential (`fill_config_pair` into the
`rpc.callers` table). The pairs are distinct per caller, so a compromised
caller cannot impersonate another.

## NATS requires a password (2026-10)

The broker ran with no authorization, so any container on the internal
network - the camera container that parses untrusted media, the relay -
could publish forged identity changes (signing users out), change-feed
events, encounter summaries for the LLM's memory or push intents. The
compose now starts it with `--user argus --pass ${NATS_PASSWORD}`, every
service's `[nats]` carries `user`/`password` (read by `NatsBus` and passed
with `natsOptions_SetUserInfo`, never in the URL, so it never reaches a
log), and `provision-host.sh` generates the password once for every config
and writes it to `.env`. Per-service subject permissions (an nkey per
service) are the next step and are not done.

## Port map (host)

| Port | Bind | Owner |
|---|---|---|
| 7024 TLS | — | gone: the gateway's public port, deleted with the service in Phase 3d step 1c |
| 7025 TLS | 0.0.0.0 (compose publish) | argus-sync `/sync` WebSocket — the app-facing sync transport, on all interfaces since Phase 3a |
| 7042 TLS | 0.0.0.0 (compose publish) | argus-auth HTTP surface (`/auth`, `/invitation*`, `/pairing`) — LAN since Phase 3d step 1c |
| 7043 gRPC | 127.0.0.1 (compose publish) | argus-auth session verdict — the auth filters' `[auth] target` upstream, gated by `[auth] rpc_secret` |
| 7044 TLS | 0.0.0.0 (compose publish) | argus-identity HTTP surface (`/user*`, `/portrait-preview/*`) — LAN since Phase 3d step 1c |
| 7040 gRPC | 127.0.0.1 (compose publish) | argus-identity people wire — argus-sync's `[identity] target` pull |
| 7026 TLS | 0.0.0.0 (compose publish) | argus-camera (`/camera`, `/zone`, the `/media` socket) |
| 7027 TLS | 0.0.0.0 (compose publish) | argus-productivity |
| 7028 TLS | 0.0.0.0 (compose publish) | argus-notification |
| 7039 TLS | 0.0.0.0 (compose publish) | argus-guard |
| 7045 TLS | 0.0.0.0 (compose publish) | argus-settings (`/settings`, owner only) |
| 7029 plain | 127.0.0.1 (compose publish) | argus-tts (internal, argus-camera `[tts]` upstream) |
| 7030 plain | 127.0.0.1 (compose publish) | argus-stt (internal, argus-voice `[stt]` gate upstream) |
| 7031 plain | 127.0.0.1 (compose publish) | argus-vlm (internal) |
| 7032 plain | 127.0.0.1 (compose publish) | argus-llm (internal, argus-voice `[llm]` gate upstream) |
| 4222 | 127.0.0.1 | nats client |
| 8222 | 127.0.0.1 | nats monitor |
| 7100 plain | 127.0.0.1 (compose publish) | argus-relay device listener — the app's manual remote server entry point (tunnel profile) |
| 7101 plain | 127.0.0.1 (compose publish) | argus-relay home listener — the single client link (tunnel profile) |
| 7103 plain | 127.0.0.1 (compose publish) | argus-relay `/health` (tunnel profile) |
| 7104 plain | 127.0.0.1 (host network, container binds loopback) | argus-tunnel-client `/health` (tunnel profile) |
| 1984 / 8554 | container loopback only | go2rtc spawned by argus-camera (Ruling AH — never published) |
| 8800 | host | Tapo talk channel (camera-side, argus-camera `[tapo]`) |
| 7034 gRPC + 7035 plain | 127.0.0.1 (compose publish) | argus-voice voice wire + `/health` (F6-3) |
| 7036 gRPC | 127.0.0.1 (compose publish) | argus-camera camera-domain sync wire (F6-5) |
| `[remote] tunnel_port` TLS | argus-auth / argus-identity host port | their second (remote) listener (default 0 = disabled; the instance sets a port when the tunnel profile is on — the listener is config-file-driven, not env-driven, see the tunnel section) |

Since Phase 3d step 1b every app-facing service terminates TLS with the
instance certificate and announces one `_argus-route._tcp` instance per logical
route; since step 1c the seven app-facing publishes above are on the LAN and
`scripts/provision-host.sh` writes the host's LAN address into every
`mdns.address`, so the announcement names an address the app can actually
reach. Phase 5 step 6 verifies discovery against a real client.

## Settings owners

`argus-settings` owns no data: `GET /settings` reads the catalog every
configured owner publishes over `argus.settings.v1` and `PATCH
/settings/{owner}` forwards a change to that owner, which validates it against
its own registry and persists it into its own `config.toml`. That is why the
config binds of the settings owners are writable: `argus-tts`, `argus-stt`,
`argus-vlm`, `argus-llm`, `argus-voice`, `argus-guard`, `argus-camera` and
`argus-notification` mount `config.<owner>.toml` without `read_only`
(`ConfigService` rewrites the file in place when a rename over a bind-mounted
file fails). Every other config bind stays read-only, `argus-settings`' own
included.

`provision-host.sh` (through `ensure_settings_owners` in
`scripts/lib/common.sh`) mints one 32-byte secret per owner and writes it on
both sides: the owner's caller slot (`[rpc.callers] settings` for tts, stt,
vlm and llm; `[grpc] caller_settings` for voice, and for camera and
notification once their templates carry it) and `[owners.<owner>] credential`
in `config.settings.toml`, whose `target` it sets to `argus-<owner>:<port>`
from the owner's own gRPC listener. An owner whose config has no caller slot
stays unconfigured (guard has no gRPC listener today). Existing secrets and
targets are never overwritten.
