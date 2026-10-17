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
<name>` from the repo root, a `test` stage runs that service's ctest with
tests on, and a slim runtime stage carries only that service's binaries.
Packages are reusable libraries compiled into the service
images — no package has an image of its own. The identity image carries
`argus-migrate-identity` (identity is its owner, since Phase 3c-1) and the
argus-sync image carries `argus-migrate-sync` (the Phase 3c-2 split of the five
sync tables out of identity's file); argus-camera
carries `argus-migrate-camera` and `argus-vulkan-probe`; productivity and
notification carry their migration tools; the argus-tunnel image carries the
client and the relay. The argus-memory binary is gone since f8-b3, and its
package dissolved into argus-llm at Phase 4 step 7: the whole stack is one of
its features now.

Build all images from the repository root, one at a time, through the capped
wrapper:

    ./scripts/build-images.sh [--only <project>] [--memory <gb>]

It builds each image with `docker buildx build` against a `docker-container`
builder whose own container carries a hard cgroup memory limit (12 GiB by
default), refuses a tree with uncommitted changes, refuses to run beside
another series, and refuses to build at all when the builder's limit cannot be
confirmed. `docker build --memory` does nothing under BuildKit and `docker
compose build` accepts no memory limit, so the limit has to live on the
builder's container: the uncapped path is what let a runaway build step reach
a global kernel OOM on a 30 GiB host. Parallel builds also collide in the
shared Conan package cache ("Reference ... already exists"), which is the
second reason the images build one at a time. Per image the `test` stage runs
first and the shipped `runtime` stage is tagged only when it is green; the
runtime stage copies service binaries, migration tools and shared libraries by
name, so no test binary is in a shipped image. Each run records the commit,
both stages' seconds and exit statuses, the image id and the image size in
`build/images/build-times.tsv`, with one build log and one ctest summary per
image beside it.

The Dockerfile-specific
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

### gRPC and protobuf: an image and a native dev build do not resolve the same pair

This is the surprise to prevent, and it is the reason the test stage exists.

- **An image builds and runs against the system gRPC 1.51 and the system
  protobuf.** Trixie's `libgrpc++-dev` + `libprotobuf-dev` are what the build
  stage installs, `argus_contracts_substrate()` takes its system branch
  (`pkg-config grpc++`, apt's `protoc`, `grpc_cpp_plugin`) and the runtime
  stage carries `libgrpc++1.51t64` + `libgrpc29t64`, so the binary links the
  same 1.51 it was compiled against.
- **A native `dev` build uses the hand-built gRPC 1.82.1** at
  `~/.local/argus-thirdparty/grpc`. That host has no system gRPC, so
  `find_package(gRPC CONFIG QUIET)` fails and
  `cmake/argus-module.cmake` falls back to that prefix.
- **The flag that selects it is `ARGUS_SYSTEM_PROTOBUF`**, passed to cmake by
  `scripts/build-all.sh` as `-DARGUS_SYSTEM_PROTOBUF=${ARGUS_SYSTEM_PROTOBUF:-OFF}`.
  `OFF` is the dev default; the build stage of every service Dockerfile sets
  `ENV ARGUS_SYSTEM_PROTOBUF=ON`, which the `test` stage inherits. Turning it
  ON is what removes the second abseil flavor and with it the CQ bridge
  (`argus_grpc_absl_bridge()` returns early), which is the single-abseil
  design.
- **Consequence for a change that touches gRPC:** a patch that compiles
  natively proves the 1.82.1 side only. The image's `test` stage is the 1.51
  side, and it is the only place that leg is exercised.

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
- **nats** (`nats:2.11.14-alpine`, core NATS + JetStream) lives on the
  `internal` bridge network, its client port published on host loopback (4222,
  overridable via `NATS_CLIENT_PORT`). The monitor binds 127.0.0.1:8222 inside
  its own container (`nats.conf`), where only the healthcheck reaches it; it is
  no longer published, since `/connz` and `/subsz` name every client and
  subject. On a deploy host, 127.0.0.1:4222 is the production broker: no test
  ever points there, and a test that needs a broker starts its own on a random
  port. The publish stays on 127.0.0.1 alone, and the broker has a single
  client listener whose users all come from `auth.conf`, so every connection
  through it authenticates the way the bridge's do. `deploy-mounts-test` pins
  the bind address, the single listener, the `auth.conf` include and the
  absence of `no_auth_user`.
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
- Trust note: a service resolves the caller IP from `X-Forwarded-For` only
  when the immediate peer is in `device.trusted_proxy_ips` (exact addresses
  or CIDRs; loopback is no longer implicit) and `device.trust_forwarded_for`
  is set. Nothing in this stack writes that header: the gateway that did is
  gone, the tunnel relays the app's TLS unopened, and Docker's port
  forwarding adds no header. So the templates ship `trust_forwarded_for =
  false` and an empty list, and provisioning no longer fills the bridge
  gateway 172.19.0.1 into it: listing that address let any client that
  reached a published port through it choose its own address with the
  header. Where Docker's userland proxy forwards a connection, the peer a
  service sees is that gateway, so `device.identity_mode = "ip"`
  fingerprints the gateway and the user agent rather than the client; a host
  that disables the userland proxy preserves the client address. An
  installation provisioned before this change keeps `172.19.0.1` in its own
  configs until the owner clears it; put an address back only for a real
  reverse proxy that writes the header.
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
| argus-llm | `argus-llm:local` | internal network (172.19.0.32), loopback 7032 publish; `/dev/dri`; models/llm subpath ro; carries the memory stack as a feature since Phase 4 step 7 (its stack hosting landed at f8-b4); config bind rw (settings owner); `/health` healthcheck |
| argus-voice | `argus-voice:local` | internal network (172.19.0.34), loopback 7034 (gRPC) + 7035 (`/health`) publishes; no database; models/vad ro; gated on nats; config bind rw (settings owner); `/health` healthcheck |
| argus-relay | `argus-tunnel:local` | `profiles: [tunnel]`; internal network, loopback 7100/7101/7103 publishes; no database (Ruling CL); `/health` healthcheck |
| argus-tunnel-client | `argus-tunnel:local` | `profiles: [tunnel]`; host-networked (it dials the remote listener a home service opens and the relay's loopback home publish on 127.0.0.1); no database (Ruling CL); `/health` healthcheck |
| nats | `nats:2.11.14-alpine` | exact tag pin; core NATS (no JetStream needed) |
| rustfs | `rustfs/rustfs:1.0.0-beta.12` | exact tag pin; private S3 objects under `${ARGUS_DATA_DIR}/rustfs/objects`, root keys and RPC secret from compose secret files; re-run the prefix-policy proof (`scripts/rustfs-prefix-policy-test.sh`) and the init proof (`scripts/rustfs-init-test.sh`) before any RustFS upgrade |
| rustfs-init | `rustfs/rustfs:1.0.0-beta.12` | one-shot, after rustfs is healthy; `scripts/lib/s3-init.sh` creates the private bucket, the shared application service account and one prefix-scoped account per writer (identity `portraits/` and `faces/`, camera `cameras/`, guard `guard/incidents/`) through the S3 and admin APIs, signed by curl (`--aws-sigv4`); each writer's keys are generated by the server and its answer written straight into `${ARGUS_DATA_DIR}/rustfs/accounts/<service>/credentials.json` (0600), kept on a re-run and issued again, with the old account removed, when the account or the file is lost; the root keys reach curl only in a 0600 config file and the account's keys only in its piped request body, never in an argument, an environment or a shell variable. It runs on the server's image because that image carries curl and `rustfs/rc` does not, and `rc admin service-account create` takes keys only as arguments |
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
  `services/llm/database/schema.sql`, `models/memory`, `models/decide` and
  `models/extract` binds (`models/decide` carries the Laya decider bundle and
  `models/extract` the GLiNER extractor bundle; without them the turn runs on the
  rules, the explicit triggers and the rule text; `provision-host.sh` copies the two
  pilot bundles into a custom `ARGUS_MODELS_DIR`).
  argus-memory (7033) is retired since f8-b3: the memory capacity is
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
- `/dev/dri` is mounted into `argus-vlm` and `argus-llm` (llama.cpp Vulkan
  backend, F2-1 pattern; the owner accepted the LLM half on 2026-10-03,
  superseding Ruling CD). A GPU-less host drops the device from both with a
  `devices: !override []` compose override and each engine degrades to CPU
  in-binary. The device alone changes nothing: each engine reports the `gpu`
  settings capability when llama.cpp finds a Vulkan device
  (`llama_supports_gpu_offload()`), and only then does the "Máxima calidad"
  profile, or the owner in Configuración, set `vision.gpu_layers` /
  `llm.gpu_layers` to 999 (a restart key: the engine picks it up when its
  container restarts). `gpu_layers = -1` keeps the CPU path. Measured on the
  reference host's Radeon Vega 8 (services/vlm/CONTEXT.md): the VLM answers a
  one-sentence describe in 0.61 s instead of 1.09 s (1.8x).
- **Models (Ruling CB).** Per-service read-only subpath binds, never the
  whole tree: `models/tts` → argus-tts, `models/stt` → argus-stt,
  `models/vision` → argus-vlm (the GGUF + its mmproj projector),
  `models/llm` → argus-llm, `models/decide` → argus-llm (the Laya decider
  bundle), `models/extract` → argus-llm (the GLiNER extractor bundle).
  `models/memory` is an argus-llm bind since f8-b4, which hosts the memory
  stack (its argus-memory bind is gone with the process, f8-b3).
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
  it) so the loopback publishes reach it; it publishes no port. It publishes
  its link state on `argus.tunnel.v1.link` (the `tunnel` row of
  `nats-permissions.conf`, which argus-sync's row subscribes to). Being
  host-networked, it cannot resolve the `nats` alias, so its `[nats] url` is
  `nats://127.0.0.1:4222`, the broker's loopback publish. It connects there
  with its own nkey (`seeds/tunnel.seed`, mounted read-only at
  `/run/secrets/nats.seed`) on the same listener and the same `auth.conf` as
  every other client, and with no other credential. It waits for a healthy
  `nats`. If an owner moves `NATS_CLIENT_PORT`, this url has to follow.
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
- The remote listeners are published on the host loopback only —
  `127.0.0.1:${AUTH_TUNNEL_PORT:-7142}` (argus-auth) and
  `127.0.0.1:${IDENTITY_TUNNEL_PORT:-7144}` (argus-identity) — and the
  host-networked client dials `127.0.0.1:7142` (`config.tunnel.toml`
  `gateway_port`, also the binary's default). It used to name 7034, voice's
  plaintext gRPC port, and the remote listener was not published at all, so
  "fixing" the tunnel by pointing it at 7042 would have classified every
  remote request as LAN (`RemoteGate` off, presence "at home").
  `scripts/provision-host.sh --with-tunnel` (or a `.env` whose
  `COMPOSE_PROFILES` already names `tunnel`) adds the profile to `.env`, sets
  `[remote] tunnel_port` (7142 / 7144) and `tunnel_profile = true` in the auth
  and identity configs — both refuse to boot with the profile on and no port —
  points the client at 7142 and writes one 32-byte `[tunnel] secret` into the
  client and relay configs. A single client reaches one listener: identity's
  remote routes need per-stream targets (see "Deferred hardening").

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
| `${ARGUS_DATA_DIR}/rustfs/accounts` | rustfs-init at `/run/argus/accounts` (rw) | one directory per writer (`identity`, `camera`, `guard`, 0700), each holding that writer's `credentials.json` (0600) |
| `${ARGUS_DATA_DIR}/rustfs/accounts/<service>` | that writer alone (identity, camera, guard), at `/run/secrets/s3` (ro) | its own `credentials.json`, which `[storage.s3] credentials_file` names; the directory, not the file, is mounted, so a file `rustfs-init` issues again by rename is the one the service reads |
| `${ARGUS_CA_DIR}` (default `${ARGUS_DATA_DIR}/pki`) | argus-identity at `/opt/argus/ca` | `ca.key`, `ca.srl`, `pairing.code` (0600) |
| `${ARGUS_DATA_DIR}/nats/auth.conf` | nats at `/etc/nats/auth.conf` (ro) | the broker's `authorization` block |
| `./nats-permissions.conf` | nats at `/etc/nats/nats-permissions.conf` (ro) | each service's allow-lists, one `ARGUS_NATS_<SERVICE>` variable per row of the permission table |
| `${ARGUS_DATA_DIR}/nats/seeds/<service>.seed` | that service alone, at `/run/secrets/nats.seed` (ro) | its NATS nkey seed (0600) |

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
  `[jwt] secret` (and `refresh_secret` in `config.auth.toml` alone: the
  verifiers no longer carry it, provisioning removes it from their existing
  configs and regenerates auth's if it ever equals `secret`) and
  `[device] fingerprint_secret` (identical in the minting/verifying set —
  argus-auth mints, argus-camera, argus-guard,
  argus-productivity, argus-notification, argus-identity and argus-sync
  verify, and the
  device hash must agree across them), the `[auth] target`
  (`tls:argus-auth:7243`) and the `[auth] credential` each of those verifiers
  carries — its own key to the session verdict, paired with argus-auth's
  `[rpc.callers]` entry for it (the fleet-wide `[auth] rpc_secret` of an
  older install is honoured only until every verifier is paired), so an
  absent or mismatched key makes the authority refuse every call and leaves
  all their authenticated routes answering 401 — and the `[tunnel] secret` (identical
  in the two tunnel templates — the HMAC home-link key; empty keeps the pair
  from booting, and it is never baked into any layer or template).
  `device.trusted_proxy_ips` is empty and `device.trust_forwarded_for` false
  in every bridge-networked service's template (argus-auth, argus-camera,
  argus-productivity, argus-notification, argus-guard, argus-identity,
  argus-sync, argus-settings): since Phase 3d step 1c no first-party peer
  writes `X-Forwarded-For` — read the network section's trust note before
  changing either key.
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
  gains with the hosted memory feature; `[decide]` and `[extract]` name the
  Laya and GLiNER bundles and select no engine (the engines of the earlier
  cutover, replaced by Laya and GLiNER; see the retirement record
  [docs/history/reports/legacy-router-extractor-retired.md](../docs/history/reports/legacy-router-extractor-retired.md),
  2026-10-17).
- No docker secrets for the services: nothing is baked into images and
  instance secrets live only in the gitignored config files (rustfs and its
  init job read theirs from compose secret files).
- No provisioning secret passes through a shell value. `provision-host.sh`,
  `setup.sh`'s config steps and the helpers in `scripts/lib/` generate a
  secret straight into the file that keeps it (`openssl rand | ...`), copy it
  from one config to another through awk and a pipe, and compare or validate
  it inside awk, so it is never an argument, an assignment or a word a
  `set -x` trace prints: the JWT, refresh and fingerprint secrets, the shared
  NATS password, every RPC credential, the LiveKit and tunnel secrets, the
  pairing code, the S3 keys and the NATS seeds. `scripts/deploy-mounts-test.sh`
  runs provisioning (tunnel profile included) and the native config steps
  twice each under `bash -x` and fails if a re-run changes a config or a
  secret, or if any trace carries a secret by either of two checks. By name:
  every config value whose key is `secret`, `password` or `credential` (or ends
  in `_secret`, `_password`, `_credential`), starts with `caller_` or sits
  under `[rpc.callers]`. By value: every value of the generated secret files,
  whatever their names (the `rustfs/secrets/` files, the writers'
  `rustfs/accounts/*/credentials.json` keys, each NATS seed as text and as
  payload, the pairing code and every line of a private key), and every
  alphanumeric run of 16 or more characters inside them, so a partial leak (a
  bucket's random part, one line of a key) fails too.
- `[device] identity_mode` is argus-auth's decision: provisioning copies
  `config.auth.toml`'s value (default `credential`) into every config that has
  the key, so a verifier never fingerprints differently from the issuer.
  `[device] lan_networks` is written once, when empty, from the CIDR of the
  interface carrying the detected LAN address (`192.168.1.0/24`, ...); the
  Docker bridge range is not in it, so a connection Docker's proxy forwards
  from the host is not taken for a LAN client.
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

## NATS authorization (2026-10)

The broker starts with `-c /etc/nats/argus.conf` (the committed `nats.conf`:
client port, monitor on the container's loopback, JetStream store) which
includes `auth.conf`, a file `provision-host.sh` writes into
`${ARGUS_DATA_DIR}/nats/auth.conf` (0644 inside the 0700 data tree; the broker
runs as root with every capability dropped, so it reads by mode, not by
`DAC_OVERRIDE`). `auth.conf` renders one `{nkey: …}` user per row of
`nats-permissions.conf` and nothing else: since 2026-10-16 the shared `argus`
user with the password `config.auth.toml` used to carry is gone from the
rendering, from the templates and from the provisioning pass, together with
the `[nats] password` secret it was generated from. `NATS_PASSWORD` is not
written to `.env` either; an existing line there is inert. Re-run provisioning
before the first `up -d` after this change, or nats fails to start on the
missing include.


The broker ran with no authorization, so any container on the internal
network - the camera container that parses untrusted media, the relay -
could publish forged identity changes (signing users out), change-feed
events, encounter summaries for the LLM's memory or push intents. The first
fix was one shared user, `argus`, with a password every service's `[nats]`
carried as `user`/`password` (passed with `natsOptions_SetUserInfo`, never
in the URL, so it never reached a log); the password travelled in `auth.conf`,
never on the command line or in `.env`. That user was a step, not the target:
it carried the whole account, so any one service's copy leaked every grant.
Since 2026-10-16 every service connects with its own nkey and the grants of
its row ("One NATS user per service" below), and the shared user is removed
from the rendering, the templates and the provisioning pass rather than kept
as a fallback. `NatsBus` still authenticates with `user`/`password` when a
config carries both, which is what the live permission tests' own throwaway
probe user uses; no template, no image and no provisioned config does.

## One NATS user per service (S3, 2026-10)

Every row of the permission table in
`packages/lib/nats/src/nats/nats-permissions.cc` is a broker user: users with
permissions inside the one account, not separate accounts, because JetStream
streams belong to an account (ruling of 2026-10-12, `realtime-and-backlog-plan`
section 5). The table is rendered into the committed `nats-permissions.conf`,
one `ARGUS_NATS_<SERVICE>` variable per row, and `provision-host.sh` writes
`${ARGUS_DATA_DIR}/nats/auth.conf` as an include of that file plus one
`{nkey: "U…", permissions: $ARGUS_NATS_<SERVICE>}` user per variable, and
nothing else: the users list holds no shared `argus` user.

- **Keys.** Users authenticate with an nkey, so the broker holds public keys
  only: `auth.conf` stays 0644 inside the 0700 data tree and holds no secret at
  all. Each service's seed is
  `${ARGUS_DATA_DIR}/nats/seeds/<service>.seed` (0600, the directory 0700),
  issued once and reused on every re-run, and mounted read-only into that
  service alone at `/run/secrets/nats.seed`; the broker never sees it.
- **How a seed is made.** OpenSSL, which provisioning already requires for the
  PKI, generates 32 random bytes and derives the Ed25519 public key from them;
  the seed reaches OpenSSL on its standard input, as a PKCS#8 PEM, never as an
  argument. The nkey text framing (the user and seed prefix bytes, the
  CRC-16/XMODEM checksum, unpadded base32) is done in
  `scripts/lib/nats-users.sh` by POSIX awk reading a pipe, and `od` reads the
  public key back, so a seed is never a shell value; on 2026-10-13 it gave the
  earlier shell implementation's keys for 80 seeds under gawk, `gawk --posix`,
  mawk 1.3.4 and busybox awk. The `nk` tool would be a new host dependency,
  nats-server has no generator, and a generator linked against cnats would
  need a build on the deployment host. The derivation is pinned to the seed
  and public key pair the cnats tests and the NATS documentation use
  (`SUACSSL3…` / `UDXU4RCS…`), in `scripts/deploy-mounts-test.sh`.
- **Every row gets a user.** nats-server refuses a config variable nothing
  uses ("unknown field"), so the rendering iterates the variables of
  `nats-permissions.conf` and issues a seed and a user for each of them.
- **Services connect by configuration.** Each NATS client's template carries
  `seed_file = "/run/secrets/nats.seed"` (its compose mount),
  `inbox_prefix = "_INBOX.<service>"` and an empty `nkey`; provisioning adopts
  those keys into the existing `config.<service>.toml` and writes the public key
  of that service's own seed into `nkey`. `NatsBus` connects with the key when
  `nkey` and `seed_file` are both set; the shared `user`/`password` pair the
  templates used to carry is retired from an existing config by the same
  provisioning pass (`retire_obsolete_deploy_keys`).
- **Reloading the broker.** Compose does not see a change in the content of a
  mounted file, so a later change to `nats-permissions.conf` (a new grant, a
  new row) reaches the running broker only through a reload: re-run
  `provision-host.sh`, then `docker compose kill -s HUP nats` (nats-server
  re-reads `auth.conf` and its include and keeps its connections), and only then
  start the services that need the new grants.
- **Checking it.** `scripts/deploy-security-check.sh` is the read-only check
  of a deployed stack: per service it proves the config carries its own key,
  that the key is the one its seed holds, that `auth.conf` admits it and no
  shared `argus` user, and, through the broker's monitor, that a connection of
  that service is present with it while no connection presents a key no config
  names. It is the owner's check on his server, not a gate that blocks a
  deploy.
  The gate that sequenced this removal - `scripts/nats-users-check.sh`, with
  `scripts/nats-users-check-test.sh` and their recorded broker answers - was
  deleted with it on 2026-10-16: it existed to answer "does every deployed
  service already connect with its own key, so the shared user can go", and
  that question is answered.
- **Deploying this change.** The broker now includes `nats-permissions.conf`
  and every NATS client mounts its seed with `create_host_path: false`, so
  re-run `scripts/provision-host.sh` before `docker compose up -d`: without the
  seeds those services fail to start, and without the new `auth.conf` the
  broker does.

## Container hardening (2026-10)

Every service in the compose file merges `x-hardening`: `cap_drop: [ALL]`,
`no-new-privileges:true` and `stop_grace_period: 20s`. Every Argus process
runs as the unprivileged `ARGUS_UID`, binds ports above 1024 and needs no
capability (no raw sockets, so no ARP spoofing on the bridge from a
compromised camera container); the 20 s grace is what lets the shutdown
module's 15 s drain deadline finish, run its quit hooks and freeze SQLite
before Docker's SIGKILL — Docker's default 10 s cut it at the deadline itself.

`x-readonly-root` adds `read_only: true` with a 64 MB tmpfs on `/tmp` and on
`/opt/argus/uploads` (Drogon's spill directory for large bodies) to the
services whose only writes land on their own mounts: argus-auth, -sync,
-productivity, -notification, -guard, -settings, the relay, the tunnel client
and vulkan-probe. A settings owner's config bind stays writable — the bind is
its own mount, and `ConfigService` falls back to an in-place rewrite when it
cannot create its temporary next to the file. livekit-tls is read-only with
tmpfs on `/tmp` (its pid file) and `/var/cache/nginx`. The rest keep a
writable root on purpose, until each writer is mapped: argus-identity,
-camera (go2rtc and ffmpeg scratch, the stream volume), -tts (on-demand
engine provisioning), -stt, -vlm and -llm (Mesa/Vulkan shader caches under
`$HOME`), -voice (the LiveKit FFI), rustfs, nats and the one-shot init tools.

## The instance CA (2026-10)

`ca.key` used to live in `certs/`, which every TLS service and livekit-tls
mount: a remote-code bug in argus-camera (which parses untrusted RTSP/fMP4)
could read it, sign a leaf for `argus.local` and impersonate the instance to an
app that pins this CA. Now:

- the CA key and the pairing code live in the signer directory
  `${ARGUS_CA_DIR}` (default `${ARGUS_DATA_DIR}/pki`, 0700, written to `.env`),
  bind-mounted at `/opt/argus/ca` into argus-identity alone — the only process
  that signs (leaf rotation) or verifies a pairing. `config.identity.toml`
  names `ca_key = "ca/ca.key"` and `pairing_code = "ca/pairing.code"`;
- `certs/` keeps `ca.pem`, `server.pem` and `server.key`: everyone else mounts
  it read-only, identity read-write because it rotates the leaf there;
- a CA created from now on is valid for 10 years (was 25), is `pathlen:0` and
  carries `nameConstraints` (`.local`, `localhost`, the host name, the mDNS
  name, `remote.hostname`, and the loopback/private/CGNAT/link-local ranges),
  so a stolen key cannot mint a certificate a client accepts for a public
  name; identity's rotation leaves out any SAN the constraints forbid;
- every leaf carries `serverAuth`, `CA:FALSE` and, when provisioning knows the
  LAN address, that address as an IP SAN.

Existing installations: `provision-host.sh` moves `certs/ca.key` and
`certs/pairing.code` into the signer directory and rewrites identity's two
paths when they still name `certs/`; run it with the stack stopped, then
`up -d`. Their CA keeps its 25-year validity and no constraints — replacing it
means every paired device pairs again, so it is the owner's call: move
`certs/ca.pem` aside and re-run provisioning. Anyone who could read `certs/`
before the move should be assumed to have had the key.

livekit-tls now runs `livekit-tls-entrypoint.sh`: nginx in the foreground and
a poll of `server.pem` (mtime and inode, every 60 s) that runs `nginx -t` and
`nginx -s reload` when identity rotates the leaf. It used to keep the first
leaf until a restart, so calls failed about 90 days after boot.
`scripts/livekit.sh` starts the native front the same way.

## The internal CA (S1, 2026-10)

The internal gRPC legs get mutual TLS from a CA of their own, separate from the
instance CA the apps pin (ruling of 2026-10-12): a key that leaks from one signs
nothing the other trusts. Step 1 issues the CA and the certificates; step 2
gives `packages/lib/grpc` the credentials that load them (a client target
written `tls:<host>:<port>` gets mutual TLS, every other target stays
plaintext); step 3a gives `packages/lib/http` the health report
(`tls_health::report`: how many CAs the loaded trust bundle holds, the days
left on the certificate and on the CA that issued it, and a warning under 30
days for either); step 4 loads them in each service and opens its mutual TLS
listener, one service per commit (below). Deploy-time check: build the images
(`docker compose build`) before any config names a `tls:` target. The images
link Debian trixie's gRPC 1.51, and the internal TLS code has so far been
built only against the development host's 1.84; it uses only the
`Ssl*Credentials` API both versions share, so a failed image build is the
one place a difference would show.

- `scripts/lib/internal-pki.sh` (`ensure_internal_pki`) keeps, under
  `${ARGUS_DATA_DIR}/certs/internal` (0700):
  - `ca.key` (0600) and `ca.pem`: an ECDSA P-256 CA valid for 10 years,
    `pathlen:0`, named `O=Argus, OU=internal-v1, CN=Argus internal CA <random>`,
    so two CAs never share a name during a rotation;
  - `trust.pem`: the CAs a service trusts, the current one and, after a
    rotation, the previous one until it expires;
  - one 0700 directory per service that speaks internal gRPC (`auth`, `camera`,
    `guard`, `identity`, `llm`, `notification`, `productivity`, `settings`,
    `stt`, `sync`, `tts`, `vlm`, `voice`) with `cert.pem`, `key.pem` (0600)
    and `ca.pem` (a copy of `trust.pem`): an ECDSA P-256 certificate valid for
    2 years, `CN=argus-<service>`, SAN `argus-<service>`, `localhost`,
    `127.0.0.1` and `::1`, for server and client use, so one certificate
    serves both ends of a leg. That directory is all a service will mount.
- `provision-host.sh` runs it on every pass, and `setup.sh` natively (data dir
  `${ARGUS_DATA_DIR:-argus-deploy/data}`). A certificate is issued again, with
  a new key, when it is missing, no longer matches its key, was issued by
  another CA, carries an older profile or has fewer than 60 days left; the rest
  is kept.
- A CA with fewer than 60 days left is rotated in two provisioning passes, and
  each pass is followed by a rolling restart, one service at a time, never a
  fleet-wide stop. The first pass adds a next CA to `trust.pem` and keeps
  every certificate: restarting a service gives it the two-CA bundle, and
  while the others wait their turn every certificate in use is still from the
  current CA, which they all trust. A pass a day or more later makes the next
  CA current and issues every certificate again; the day is counted from the
  next CA's `notBefore`, which is written into the certificate and travels
  with it, never from the file's time, which a copy, a restore or an rsync
  moves. `trust.pem` keeps the old CA until it expires: by then every service
  holds the old and the new CA, so an old-CA certificate and a new-CA
  certificate verify against each other through the shared bundle in both
  directions (`internal-pki-test` pins both), and services restart one at a
  time with no outage. Rotation never swaps material in a running process: a
  service loads its certificate, its key and its trust bundle once, at start,
  before it opens its first gRPC channel (`installInternalTls` refuses a
  second install and one after the first channel), so both passes rely on the
  restart, one per service, and nothing needs a hot reload. Only a CA that
  cannot be used (missing, a key that does not match, an older profile,
  expired) or one that expires within a day is replaced in a single pass;
  provisioning then asks for a restart of every service.
- The per-caller secret stays the authorisation on top of mutual TLS: the
  certificate says which service is calling, the secret still decides what it
  may call. Plaintext listeners stay until the owner's deploy check; removing
  them is a later cleanup unit, like the S3 and S4 cleanups.
- `scripts/internal-pki-test.sh` checks the CA and every certificate
  (validity, use, names, permissions, what each directory holds), that the
  list of services is the set whose deploy templates speak internal gRPC,
  renewal at 59 days and not at 61, a changed key, an older profile, both
  passes of a CA rotation (the next CA waits a day by its `notBefore`, then
  takes over: a next CA whose file looks two days old but whose `notBefore` is
  fresh waits, one whose file is fresh but whose `notBefore` is two days old
  takes over; old-CA and new-CA certificates talk to each other in both
  directions under the two-CA bundle), a takeover at once for a CA expiring
  within a day, and the
  drop of an expired previous CA, mutual TLS between two
  services with `openssl s_server`/`s_client` (a client that expects another
  name, a client certificate from another CA and a client without one are
  refused, each for that reason), and a `set -x` trace that carries no line
  of a private key. `deploy-mounts-test` checks that the provisioning dry run
  issues every certificate and that no trace carries a line of their keys.

### The mutual TLS listener (S1 step 4)

Each service opens a second gRPC listener beside its plaintext one. It serves
the same gRPC services, and mutual TLS with the service's own certificate is
the only way in. The wiring is the same for every service (ruling of 2026-10-13):

- `[internal_tls] dir` is where the service reads `cert.pem`, `key.pem` and
  `ca.pem`. In the container it is `/run/secrets/internal-tls`, a read-only
  bind of `${ARGUS_DATA_DIR}/certs/internal/<service>` with
  `create_host_path: false`, so a missing directory fails the start rather
  than mounting an empty one. Natively, `setup.sh` writes the absolute
  `<data dir>/certs/internal/<service>` (`internal_pki_configure_native`) into
  each native config whose template carries the key. The native template
  leaves it empty. Empty means the service loads nothing, opens no mutual TLS
  listener, and its `/health` reports `internalTls.loaded: false`.
- `[internal_tls] address` is `<host>:<plaintext gRPC port + 200>`, with host
  `0.0.0.0` in the container and `127.0.0.1` natively. Empty loads the
  material (for `tls:` client targets) and opens no listener. The listener
  belongs to the gRPC server of the plaintext one, so it opens only where that
  one does: natively tts leaves `[rpc] address` empty, so neither opens until
  a developer sets it.
- The service loads its directory right after `ConfigService::load`, before
  any channel exists (`installInternalTlsFrom`). A directory that is named but
  unreadable stops the start with a fatal log. The service never falls back
  to plaintext alone.
- The mutual TLS port is published on `127.0.0.1` exactly where the plaintext
  gRPC port is published today, and nowhere else.
- `/health` carries `internalTls` (`tls_health::report`).
  `provision-host.sh` and `setup.sh` bring both keys into existing configs
  (`adopt_wiring_keys`).

| Service | Plaintext gRPC | Mutual TLS |
|---|---|---|
| voice | 7034 | 7234 |
| camera | 7036 | 7236 |
| productivity | 7037 | 7237 |
| notification | 7038 | 7238 |
| identity | 7040 | 7240 |
| sync | 7041 | 7241 |
| auth | 7043 | 7243 |
| settings | 7047 | 7247 |
| tts | 7129 | 7329 |
| stt | 7130 | 7330 |
| vlm | 7131 | 7331 |
| llm | 7132 | 7332 |
| guard | 7139 | 7339 |

The relay and the tunnel client speak no internal gRPC and get no listener.

The rule and the start live once in lib/grpc. `argus::client::listenPorts`
and `startListening` (`grpc/internal-tls.hxx`) take a plaintext address, a
TLS address, the loaded material and the services. They open the mutual TLS
port beside the plaintext one only with both material and an address, and
return the server with both bound ports, or no server when either fails to
bind. lib/grpc's `internal-tls-test` runs the full handshake suite. Each
service keeps a thin delegate in its `src/app/rpc/`, with a short
composition test under its own certificate name: two ports, plaintext only,
one handshake. A service whose main used to build its server inline gets
such a delegate.

Wired so far: tts, settings, productivity, notification, sync, auth, guard,
voice, stt, vlm, llm, camera and identity.
- tts and settings publish no gRPC port, so 7329 and 7247 are not published
  either.
- settings opens its mutual TLS port on the modules RPC server, beside
  7047, through startModulesRpc
  (services/settings/src/app/rpc/modules-rpc-server).
- productivity publishes its plaintext 7037 on 127.0.0.1, so 7237 is
  published there too (`PRODUCTIVITY_GRPC_TLS_PORT`). Its
  ProductivityRpcServer opens both ports through startProductivityRpc,
  which also refuses an empty or null service.
- notification publishes its plaintext 7038 on 127.0.0.1, so 7238 is
  published there too (`NOTIFICATION_GRPC_TLS_PORT`). Its main used to
  build the gRPC server inline and now calls startNotificationRpc
  (services/notification/src/app/rpc/notification-rpc-server).
- sync publishes its plaintext control RPC 7041 on 127.0.0.1, so 7241 is
  published there too (`SYNC_CONTROL_TLS_PORT`). Its main used to build
  the control server inline and now calls startSyncControlRpc
  (services/sync/src/app/rpc/sync-control-rpc-server). If the control RPC
  cannot listen on either port, sync does not start (fatal since step 4; see
  services/sync/CONTEXT.md, "The control RPC must listen").
- auth publishes its plaintext RPC 7043 on 127.0.0.1, so 7243 is published
  there too (`AUTH_RPC_TLS_PORT`). Its main used to build the RPC server
  inline and now calls startAuthRpc
  (services/auth/src/app/rpc/auth-rpc-server). If the RPC cannot listen on
  either port, auth does not start (services/auth/CONTEXT.md, "The verdict
  RPC must listen"). The plaintext verdict path answers a recorded golden
  both with and without the material loaded.
- guard publishes no gRPC port, so 7339 is not published either. Its main
  used to build the RPC server inline and now hands the services it serves
  (settings, presence, tools and duress, each only when its callers are
  configured) to startGuardRpc (services/guard/src/app/rpc/guard-rpc-server).
  A port that cannot be bound stops the start, as it already did. Its own
  `/health` handler, outside the shared envelope, carries `internalTls` at the
  top level beside `status` and `service`.
- voice publishes its plaintext gRPC 7034 on 127.0.0.1, so 7234 is published
  there too (`VOICE_TLS_PORT`). Its delegate, startVoiceRpc, sits in
  `services/voice/src/app/rpc/voice-rpc-server` (`argus::voice-rpc`) since D4
  gave voice the `src/app/` the reviewer ruling of 2026-10-13 found missing,
  the ruling under which it had sat in `feature/voice` beside the gRPC services
  it serves. A port that cannot be bound stops the start, as it already did.
- stt publishes no RPC port, so 7330 is not published either. Its
  SttRpcServer opens the plaintext 7130 and the mutual TLS port through
  startListening and mounts the internal certificate read-only.
- vlm publishes no RPC port, so 7331 is not published either. Its
  VlmRpcServer opens the plaintext 7131 and the mutual TLS port through
  startListening and mounts the internal certificate read-only.
- llm publishes no RPC port (its HTTP 7032 stays the only one), so 7332 is not
  published either. Its LlmRpcServer opens the plaintext 7132 and the mutual
  TLS port through startListening and mounts the internal certificate
  read-only.
- camera publishes its plaintext gRPC 7036 on 127.0.0.1, so 7236 is published
  there too (`CAMERA_GRPC_TLS_PORT`). Its CameraRpcServer opens both ports
  through startCameraRpc, and a port that cannot be bound stops the start
  (services/camera/CONTEXT.md, "Mutual TLS (S1 step 4)").
- identity publishes its plaintext RPC 7040 on 127.0.0.1, so 7240 is published
  there too (IDENTITY_RPC_PORT is not in .env.example, so no TLS variable either).
  Its main used to build the RPC server inline and now calls startIdentityRpc
  (services/identity/src/app/rpc/identity-rpc-server), which carries the 12 MiB
  cap on both ports. If the RPC cannot listen on either port, identity does not
  start (services/identity/CONTEXT.md, "The RPC must listen"): eight services
  read it, and an identity without its RPC would fail them all with no visible
  cause.

For every deploy template that declares
`[internal_tls]`, `deploy-mounts-test` checks:
- its own read-only certificate mount;
- the port plus 200 in both templates;
- both `dir` values;
- the publish rule;
- adoption into an older deploy config;
- the directory the native setup writes.

Its red controls are a borrowed, a missing and a writable mount, a port off
by one, and a published mutual TLS port.
`scripts/port-map-test.sh` reads `[internal_tls] address` beside the other
listeners.

### Dial targets (step 5)

Every internal client dials its callee's mutual TLS port. In the deploy
templates each dial target key carries the `tls:` scheme and the callee's
`[internal_tls] address` port: `tls:argus-identity:7240`, `tls:argus-auth:7243`,
`tls:argus-settings:7247` and so on, down the table above. The 45 keys in ten
templates (`argus-deploy/config.*.toml.example`) are the complete set, and the
settings owner targets that `scripts/lib/common.sh` writes for a deployment
(`ensure_settings_owners`, every mode) take the owner's TLS port the same
way. A `tls:` target with no installed material refuses at channel creation,
so a caller without its certificate fails rather than falling back to
plaintext. An empty `auth.target` or `identity.target` is a boot error with
the message `... is empty; run scripts/setup.sh` (the lib/auth resolvers and
the auth service's `resolveIdentity`), and no plaintext default is left. Every dialing service already mounts
`/run/secrets/internal-tls`, so compose and `.env` carry no change for this.

The plaintext listeners stay in place; the callers no longer use them.

Natively the callers dial the same way, one path everywhere: the native
templates (`services/*/config.toml.example`) and the native settings-owner
wiring carry `tls:127.0.0.1:<TLS port>`. The 35 template keys and the 11 owner
targets are one list, `internal_pki_native_dial_rows` in
`scripts/lib/internal-pki.sh`. `scripts/setup.sh` issues the native PKI and
then runs `migrate_native_dial_targets`, which moves each listed key still at
`127.0.0.1:<port>` to `tls:127.0.0.1:<TLS port>` in an existing checkout and
prints one line per key. Nothing else in a config changes, and a second run
changes nothing. Keys a developer added by hand stay as they are; only the
listed keys move. A native service needs its certificate: a config whose
`[internal_tls] dir` is empty keeps its plaintext targets through the
migration, `setup.sh` warns to run `scripts/setup.sh`, and the service refuses
to start where a client is built at start (`lib/grpc` throws
"... run scripts/setup.sh to issue the internal PKI and point [internal_tls]
dir at it"). The native demo stack (`scripts/native-stack.sh`) copies the
native configs, so it dials the same way with the same certificate, and its
`prepare` refuses a service whose config has none.

## Init jobs and rule 27 (2026-10)

`camera-init`, `productivity-init` and `notification-init` read the legacy
`argus.db` from identity's data directory, and they now mount it read-only
(the tools already open it `SQLITE_OPEN_READONLY`; their schema moved to
`/opt/argus/schema/schema.sql` so no mount point has to be created inside the
read-only directory). Run them with the stack stopped: a read-only WAL
database opens only when its `-wal` was checkpointed away. They remain the
documented exception to rule 27 — a one-shot migration tool of one owner
reading another owner's directory, never while the stack is up. `sync-rollback`
is the one job that writes another owner's file (`identity.db`), because
undoing the split is exactly that; it stays behind its own opt-in profile.

## Upgrading a host

Upgrade the server before the app: a new app against a server that predates ROUTE_NOT_FOUND reads a missing route as NOT_FOUND.

The other upgrade steps stay where they are written: the rolling restart after
each pass of a CA rotation ("The internal CA"), re-running
`scripts/provision-host.sh` before `docker compose up -d` ("One NATS user per
service", and item 4 of "Deferred hardening"), the RustFS prefix proof before
any RustFS upgrade (the `rustfs` row of "Services"), building the images before
any config names a `tls:` target ("The internal CA"), and the owner cleanup
checks of S3 ("One NATS user per service") and of S4
(the S4 line of `docs/history/plans/realtime-and-backlog-plan.md`).

## Deferred hardening (plan, not done in this branch)

The owner decided to keep this pass bounded. These four are the next steps,
in order:

1. **mTLS on the internal gRPC legs.** In progress as S1 (see "The internal
   CA" above): the CA and one certificate per service are issued under
   `${ARGUS_DATA_DIR}/certs/internal`. Next, `packages/lib/grpc` loads them,
   servers listen with mutual TLS beside their plaintext port, clients present
   their certificate, each service's health warns when a certificate has fewer
   than 30 days left, and each service mounts its own directory read-only. The
   per-caller secrets stay as the authorisation on top of mutual TLS; the
   plaintext listeners go only after the owner's deploy check.
2. **TLS on the tunnel home link.** TLS 1.3 on the relay's home port with a
   relay leaf the client pins by SPKI hash (`[tunnel] relay_pin`), the HMAC
   challenge kept inside as the second factor. PUSH bodies then stop
   travelling in clear, and frames after AUTH are integrity-protected. The
   OPEN frame gains a target byte at the same time, so one client can reach
   auth's and identity's remote listeners.
3. **Per-service NATS users.** In progress as users inside the one account,
   not accounts (see "One NATS user per service" above): one nkey per service
   in `auth.conf`, each with the publish and subscribe allow-lists and the
   `$JS.API` subjects of its row in the permission table. argus-sync then also
   refuses the control actions (`disconnect`, `replace_role_rooms`) from any
   feed but auth's session feed (a services/sync change, not done).
4. **Per-service S3 credentials.** `rustfs-init` creates one service account
   per writer with a prefix-scoped policy (identity: `portraits/` and `faces/`,
   camera: `cameras/`, guard: `guard/incidents/`, the prefixes their code
   writes), and each writer reads only its own pair: its account directory is
   mounted read-only into that writer alone at `/run/secrets/s3`, and
   provisioning names the file there in its config as `[storage.s3]
   credentials_file`, so a compromised camera can no longer read private
   portraits. One bucket is enough because RustFS enforces the policy:
   `scripts/rustfs-prefix-policy-test.sh` (2026-10-13, RustFS 1.0.0-beta.12,
   run against binaries exported from the pinned images on a scratch port and
   data directory) refuses every cross-prefix read, write, delete, copy,
   multipart upload and listing and every bucket-level call with 403, and its
   whole-bucket control fails. Re-run it before any RustFS upgrade. The init
   job's keys left command arguments on 2026-10-13 (the `rustfs-init` row
   above); the per-service accounts take keys the server generates, its JSON
   answer written straight into a 0600 file, because the admin API generates
   and confines them when the request names none. `S3StorageService` reads that
   file on every operation and signs with those keys alone: since 2026-10-16
   the shared `access_key`/`secret_key` pair is gone from the identity, camera
   and guard templates and from the provisioning (`configure_storage`,
   `sync_storage_credentials`), and a writer whose file is empty, unreadable
   or malformed is refused with the path named rather than signing with a
   shared secret (`realtime-and-backlog-plan`, S4).
   Deploying this change needs `scripts/provision-host.sh` re-run before
   `docker compose up -d`: the writers mount their account directory with
   `create_host_path: false`, and provisioning is what creates it.

## Port map (host)

| Port | Bind | Owner |
|---|---|---|
| 7024 TLS | — | gone: the gateway's public port, deleted with the service in Phase 3d step 1c |
| 7025 TLS | 0.0.0.0 (compose publish) | argus-sync `/sync` WebSocket — the app-facing sync transport, on all interfaces since Phase 3a |
| 7042 TLS | 0.0.0.0 (compose publish) | argus-auth HTTP surface (`/auth`, `/invitation*`, `/pairing`) — LAN since Phase 3d step 1c |
| 7043 gRPC | 127.0.0.1 (compose publish) | argus-auth session verdict — the auth filters' `[auth] target` upstream, gated per caller by `[rpc.callers]` |
| 7044 TLS | 0.0.0.0 (compose publish) | argus-identity HTTP surface (`/user*`, `/portrait-preview/*`) — LAN since Phase 3d step 1c |
| 7040 gRPC | 127.0.0.1 (compose publish) | argus-identity people wire — argus-sync's `[identity] target` pull, gated per caller by `[rpc.callers]` |
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
| 8222 | container loopback only | nats monitor (healthcheck only, not published) |
| 7142 TLS | 127.0.0.1 (compose publish, `AUTH_TUNNEL_PORT`) | argus-auth `[remote] tunnel_port`, dialled by the tunnel client |
| 7144 TLS | 127.0.0.1 (compose publish, `IDENTITY_TUNNEL_PORT`) | argus-identity `[remote] tunnel_port` |
| 7100 plain | 127.0.0.1 (compose publish) | argus-relay device listener — the app's manual remote server entry point (tunnel profile) |
| 7101 plain | 127.0.0.1 (compose publish) | argus-relay home listener — the single client link (tunnel profile) |
| 7103 plain | 127.0.0.1 (compose publish) | argus-relay `/health` (tunnel profile) |
| 7104 plain | 127.0.0.1 (host network, container binds loopback) | argus-tunnel-client `/health` (tunnel profile) |
| 1984 / 8554 | container loopback only | go2rtc spawned by argus-camera (Ruling AH — never published) |
| 8555 tcp + udp | 0.0.0.0 (compose publish, `CAMERA_WEBRTC_PORT`) | go2rtc's WebRTC media listener (DTLS-SRTP only; its signalling is argus-camera's `POST /camera/{id}/webrtc`, the go2rtc API stays inside the container); `provision-host.sh` writes `<lan>:8555` into `[streaming] webrtc_candidates` |
| 8800 | host | Tapo talk channel (camera-side, argus-camera `[tapo]`) |
| 7034 gRPC + 7035 plain | 127.0.0.1 (compose publish) | argus-voice voice wire + `/health` (F6-3) |
| 7036 gRPC | 127.0.0.1 (compose publish) | argus-camera camera-domain sync wire (F6-5) |
| 7046 TLS | 0.0.0.0 (host network) | livekit-tls: LiveKit signalling for the apps (`wss://`, the instance certificate) |
| 7234 / 7236 / 7237 / 7238 / 7240 / 7241 / 7243 mutual TLS | 127.0.0.1 (compose publish, `*_TLS_PORT`) | argus-voice, camera, productivity, notification, identity, sync and auth: the mutual TLS port beside each published plaintext gRPC port |
| 7247 / 7329 / 7330 / 7331 / 7332 / 7339 mutual TLS | not published | argus-settings, tts, stt, vlm, llm and guard: internal-network only, reached by their callers over the bridge |
| 7234 to 7339 mutual TLS, native | 127.0.0.1 (native run) | the thirteen listeners of a native dev run, each its service's plaintext port + 200 (`scripts/setup.sh` configures them). The argus-cutover-* containers predate these ports and publish none of them, so they do not conflict |
| 7880 plain | 127.0.0.1 + 172.19.0.1 (host network) | livekit HTTP API + signalling, for argus-sync, argus-voice and the TLS front only |
| 7881 tcp / 7882 udp | 0.0.0.0 (host network) | livekit ICE over TCP / muxed ICE over UDP (call media, DTLS-SRTP) |
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
stays unconfigured. It also writes `[owners.<owner>] config_file`, the host
path of the owner's `config.<owner>.toml`, which Configuración shows beside
every key instead of the container's `/opt/argus/config.toml`. Existing
secrets, targets and paths are never overwritten.

A stack provisioned before argus-settings existed has none of this: its
`config.<owner>.toml` files carry no `[rpc]` listener and no settings caller,
and `config.settings.toml` does not exist, so the settings surface sees no
owner and Configuración stays empty (the app now names every unconnected
service instead of showing nothing). Re-running `provision-host.sh` adopts
the wiring keys into the existing files without touching their values, mints
the credentials, writes `config.settings.toml`, and a rebuild plus
`docker compose up -d` brings the owners up with their settings listener.
This was checked on a scratch copy of the 2026-09 owner configs.

## LiveKit: the realtime call media server (2026-10-04)

Two services carry WebRTC calls (`services/voice/CONTEXT.md`, "Realtime calls
over WebRTC"):

- `livekit` (`livekit/livekit-server:v1.13.7`, Apache-2.0, the latest stable
  release on 2026-10-04; 384 MB cap, about 20 MB resident idle) on the host
  network. Its HTTP API and signalling port 7880 bind only `127.0.0.1` and
  `172.19.0.1`, the gateway of `argus-cutover-internal`, where argus-sync
  (the Twirp room API) and argus-voice (the agent's connection) reach it from
  the bridge; it is never on the LAN in clear. Media: 7881/tcp (ICE over TCP)
  and 7882/udp (one muxed UDP port instead of a 10 000-port range, enough for
  a household and one firewall rule). Host networking lets LiveKit announce
  the host's own LAN addresses as ICE candidates and follow an address
  change; with a bridge and published ports it would need `rtc.node_ip`
  pinned to an address DHCP can change.
- `livekit-tls` (`nginx:1.28.3-alpine`, 32 MB cap): LiveKit does not terminate
  TLS on its signalling port (upstream's config says to put it behind a TLS
  load balancer), and every app-facing Argus listener presents the instance
  certificate the app pins. An nginx `stream` server on 7046 terminates TLS
  with `certs/server.pem` and passes the bytes to 127.0.0.1:7880, so the
  WebSocket and LiveKit's own HTTP paths cross it unchanged. Its config,
  `livekit-tls.conf`, holds no secret and is committed.

`livekit.yaml` is the per-installation copy of `livekit.yaml.example`
(written by `provision-host.sh` when absent, gitignored, no secret inside):
the API key pair is in `livekit-keys.yaml` (0600, gitignored, read through
`key_file`; LiveKit refuses a key file other users can read), written from
`config.sync.toml`'s `[rtc] api_key` / `api_secret`, which
`ensure_livekit_key_pair` generates the first time (`argus` + 12 hex, 48 hex
of secret). Rooms auto-create on the first join, close 20 s after the last
participant leaves and 60 s after creation when nobody joined; at most 8
participants; Opus (+RED), VP8 and H.264 allowed, the last two for camera
video later.

**TURN** is configured off (`turn.enabled: false`). When the tunnel exists:
set `turn.enabled: true`, `turn.domain` to the public name, `turn.tls_port`
5349 (or 443 behind a TLS-terminating proxy with `external_tls: true`) and
`turn.udp_port` 3478, give it a certificate (`cert_file`/`key_file`, mounted
read-only), open those ports and restart `livekit`. Clients receive the TURN
servers in LiveKit's join response; nothing in Argus changes.

Native development uses `scripts/livekit.sh up|down|status|logs|token`: the
same two images as capped containers (`argus-dev-livekit`,
`argus-dev-livekit-tls`), 7880 on loopback only, the key pair from
`services/sync/config.toml` (copied into the native stack's sync config when
one exists), the rendered files under `build/livekit/`.
