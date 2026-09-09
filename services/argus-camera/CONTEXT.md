# argus-camera — CONTEXT

## Why the camera service exists

Fase 2 of the `migracion-microservicios` plan starts the service split with a
pilot: the camera domain leaves the monolith. This task (F2-1) creates the
substrate WITHOUT cutover — the legacy keeps owning every camera write and
the app keeps talking through the gateway unchanged. `argus-camera` is a
sibling service (same shape as `argus-gateway`): own binary, own CMake
preset, own `camera.db`.

## What it owns (F2-1)

- **camera.db**: the camera-domain tables (`camera`, `camera_stream`,
  `zone` plus their 8 indexes), DDL copied verbatim from
  `database/schema.sql`. The schema lands as
  `argus-camera/database/schema.sql` (same pattern as
  `argus-identity/database/schema.sql`) and
  is applied at boot through `DbService::runScriptFile` — abort on failure.
  `argus.db` is never touched here.
- **Wiring**: Drogon boot with the camera domain only — `[server]`
  internal plain listener (loopback 7026 default), `DbService` default
  client on `[camera] db` (default `database/camera.db`), `[drogon.app]`
  mirror, CORS/exception/404/405 plumbing identical to the gateway so
  envelopes are byte-shape-identical. Caller validation rides the identity
  RPC (`[identity] target`); the config-gated named identity client
  (`[identity] db`, read-only) now backs only the sync socket's user reads;
  absent key boots identity-free. No AI service registry is compiled or loaded
  (no ncnn/llama/opencv/onnxruntime code paths).
- **`GET /health`**: standard `ApiResponse` envelope
  `{status: 200 (int), info: {service: argus-camera, uptimeSeconds},
  errors: null}`; never depends on cameras or NATS.

## What it did NOT do before the cutover (F2-1; superseded below)

- **No behavior switch (Ruling W)**: the gateway kept serving `/camera*`
  and `/zone` from the legacy, which kept writing `argus.db`. Reading
  camera tables from `camera.db` then would have served a frozen snapshot,
  so the service registered no camera routes in F2-1.
- **No stream ownership (F2-1)**: the legacy still owned every running
  stream and the Tapo talk channel before F2-2.
- The migrate tool (`tools/migrate-camera`, Ruling V) copies camera rows
  from `argus.db` into `camera.db` and then goes quiet: a schema-current
  `camera.db` makes reruns a verified no-op, because after the F2-2 cutover
  `camera.db` is live data and the frozen `argus.db` copy must never be
  resurrected over it. Nothing is deleted from `argus.db`.

## What it owns since the cutover (F2-2)

- **Camera/zone CRUD**: the shared `CameraController`/`ZoneController` and
  their feature services/DTOs compile into this binary (same sources as the
  legacy, byte-identical contracts), reading/writing the default client on
  `[camera] db`. Camera and zone updates emit the exact SyncAuditService
  diff as a `CameraAuditEvent` (`kind: audit`) over
  `argus.camera.v1.change` (Ruling Y) — no audit rows are persisted locally.
  Creates/deletes emit `Add`/`Delete` change events on the same subject.
- **Media**: `CameraMediaService` handles the native `camera:*` frames of
  this `/sync` socket: `camera:subscribe` checks the camera row (404 Camera
  not found), subscribes through StreamHub fMP4 with the `0xA7` frame magic,
  and degrades to the legacy `503 go2rtc_not_running` envelope when go2rtc is
  down. Go2rtcManager owns the go2rtc lifecycle here.
- **Identity reads (narrowed in f7-3)**: `[identity] db` opens mode=ro as the
  named identity client (`DbService::setIdentityClient` slot; SQLite URI
  filenames are enabled before the first `sqlite3_open` so the `mode=ro` URI
  parses). It now serves only the sync socket — `SyncService::refreshContext`
  resolves the connecting user's row and the `user` sync table is read from
  the same repository. The filters stopped reading it in f7-3 and validate
  over `argus.identity.v1.ValidateToken` at `[identity] target`; absent db
  key boots identity-free (sync user reads degrade, authentication does
  not). The gateway creates identity.db at its own boot, which on a fresh
  install may land after ours, so the open waits bounded for the file.
- **Explicit controller registration**: the camera, zone and camera-control
  controllers live in the shared static library, so their AutoCreation
  registration is linker-dropped there; this service registers them
  explicitly.
- **OPTIONS divergence attribution (F2-4 review)**: unlike the legacy
  (`src/config/application.cc` pre-routing advice answers every OPTIONS with
  `AppConfig::handleOptions` before routing), argus-camera registers only the
  post-handling CORS advice, so `OPTIONS /camera` 404s here where the legacy
  answers 200. The gateway proxy forwards OPTIONS fine (F1-5 scoped its own
  pre-routing advice to gateway-native paths); the divergence lives in this
  service. App-safe as shipped: the native client sends no preflight.
- **What stays away**: no voice path, no alarm-triggering code, no AI
  symbols beyond the detector.

## Device control (F6-2): camera-control routes + the TTS wire

- **camera-control moved out of the legacy**: `feature/api/camera-control/`
  (routes `/camera/{id}/status|presets|ptz|preset|settings|capabilities|
  talk`) lives here now, same controllers/dtos/services layout as the
  monolith. The gateway routes every `/camera` and `/zone` segment depth to
  this service; the legacy serves no camera route anymore.
- **Talk synthesis is remote-only**: `TtsClient` (tts-remote) is compiled
  into this binary and every synthesis is an HTTP exchange with argus-tts
  (`[tts] remote_url`, default `127.0.0.1:7029`). No in-process TTS engine
  exists here — with the key empty every talk call fails with the 502
  `CAMERA_UNREACHABLE` envelope, never a fallback. Synthesis runs before the
  device call (a TTS failure never opens a talk session for nothing), and a
  failed driver session is dropped so the next call logs in again instead of
  reusing a transport the camera has already closed.
- **Driver stack**: `camera-driver` (registry + Tapo driver) and the tapo
  transport stack compile into `camera-core` (OpenSSL linked); `[tapo]`
  config keys are read here, mirroring the legacy block.
- **Legacy slimming**: the legacy binary no longer compiles the camera/
  zone features, the camera-driver/tapo/stream stack, nor `camera.db`
  access; `SocketCameraChangeSink` and `CameraAudioSource` were deleted.

## Build wiring (decisions)

- The canonical camera-service builds are the ROOT presets
  (`cmake --build --preset camera` / `--preset camera-prod`): they reuse the
  root Conan cache. The standalone `argus-camera/` build directory goes stale
  on new `conanfile.txt` requires until `conan install` is re-run there.

## Object detection (F2-3): detector, operator, event intelligence

- **The ncnn exception**: this service links ncnn (Vulkan) for the YOLO26n
  detector ONLY. No face/llm/vlm/tts/stt/vad code or symbols are compiled in
  (verified with `nm -C`); YOLO26n is AGPL-3.0, so `IObjectDetector` is the
  only seam and `argus-camera/NOTICE` carries the attribution — swap the
  model artifact (e.g. RF-DETR-Nano) without touching the service.
- **Export shape reality**: the Ultralytics `format="ncnn"` export falls back
  to the one2many head; the raw end-to-end export (patched postprocess +
  PNNX, `scripts/export-yolo26-ncnn-e2e.py`) gives `(1, 8400, 4+nc)` XYXY
  with no TopK in the graph. The detector reads blob names via ncnn's
  `input_names()/output_names()`, detects the output shape at runtime and
  decodes only row_length 6 (e2e+TopK) or 4+nc (raw one2one + C++ TopK).
- **Vulkan selection differs from FaceService on purpose**: the detector
  takes HardwareProbe's `vulkan` flag (any device, integrated included) and
  degrades to CPU per instance on runtime failure; FaceService gates on
  `vulkanDiscrete`. The inference slots are released only after a successful
  load (the FaceService deadlock pattern: a failed init must leave the
  service disabled, never a held semaphore), and the mutex guards only the
  snapshot grab — inference runs on a shared snapshot so concurrent cameras
  overlap, bounded by the semaphore.
- **Frames come from go2rtc** (`/api/frame.jpeg?src=cam<id>` via
  Go2rtcFrameSource, resolved per grab because Go2rtcManager resolves its
  address only at init) — never from a device driver (Ruling AB). The Tapo
  `ICameraDriver` detection walker is deferred to Fase 4.
- **Identity is deferred behind `IKnownPersonMatcher`** (default
  `NoKnownPersonMatcher`): rules 3-8 keep their own severity; the real
  matcher arrives in Fase 4 with the identity domain. Absent (or no-match)
  matcher keeps rules 3-8 severities; present and matching, rule 2
  `known_person` dominates.
- **The 9-rule table (Appendix B.3, fixed order)**: 1 `exclude_zone` drop,
  2 `known_person` (dominates 3-8 when matched), 3 `person_in_alert_zone`
  Critical, 4 `person_in_monitor_zone` Warning, 5 `person_night` Warning,
  6 `person_day` Info, 7 `vehicle_arrival` Info, 8 `vehicle_night` /
  escalating presence Warning, 9 `ignored_class` drop.
- **Ruling AF is absolute**: the operator never touches hardware. The only
  output channel is `IObjectEventSink` → `NatsObjectEventSink` publishing
  `argus.camera.v1.object_detected` (JetStream stream `ARGUS_CAMERA`, 7d
  file retention, best-effort ensure; core NATS publish works without it).
  The retention is server-side inspection only: the gateway subscribes
  ephemerally over core NATS, so events published while it is down are not
  replayed after a restart.
- **Budget split (Ruling AD)**: camera side = aggregation window +
  per camera+class cooldown + `max_fps_inference`; gateway side =
  notification budget/silent hours/digest (see argus-gateway CONTEXT.md).
  Inside one aggregation window objects dedupe by class and the pending
  event keeps the dominant severity; a class still cooling down drops the
  whole window and the next window starts fresh. Preprocessing and
  inference never run on the event loop (`BlockingTask`).
- The labs (`argus-camera/labs/`) build on demand via the camera presets
  (`--target argus-object-bench` / `argus-camera-probe`), EXCLUDE_FROM_ALL.
- Model artifacts (`models/objects/`) come from `scripts/setup.sh camera`:
  yolo26n.pt sha256-pinned download + local raw e2e NCNN export; without the
  artifacts the detector boots disabled — never a fake success.

## Camera sync gRPC leg (F6-5 step 1)

- **`argus.camera.v1.SyncService.PullTable`** (port 7036, `[server]
  grpc_port`): serves the camera-domain sync tables (camera, camera_stream,
  zone) to the gateway with the frozen /sync semantics typed into
  `argus-contracts/proto/argus/camera/v1/sync.proto` — per-table
  required_create/required_deleted/find_last legs, (createdAt, id) cursor
  ranges, LIMIT 200 baked into the owner's SQL, tombstones as
  {id, deletedAt}. The implementation calls the same camera/camera_stream/
  zone repositories the CRUD surface uses, so the served rows are the exact
  JSON the sync tables always produced. The caller identity rides
  x-argus-user / x-argus-role / x-argus-device metadata: presence is
  required, the role was validated once at the gateway before the call, and
  the camera tables carry no userId row scoping. A grpc.health.v1 Health
  service shares the listener (F6-3 shape).
- The gateway no longer opens camera.db read-only: `[camera] db` is gone
  from the gateway config and the camera-db compose mount is gateway-only
  history. argus-camera stays the single owner of the file.

## The folder owns its domain (f7-7a)

The camera CRUD features, the Tapo driver stack, the stream lifecycle and
the camera schema all moved out of the shared `src/` tree into this
folder, prefixes preserved (`src/feature/api/{camera,zone}`,
`src/shared/services/{camera-driver,tapo,stream}`), so no include line in
the fleet changed. The unit suites moved to `tests/unit/` and register
themselves under `ARGUS_ROOT_PROJECT`; they must opt back into the default
build (`EXCLUDE_FROM_ALL FALSE`) because this folder is added excluded,
otherwise ctest registers tests whose binaries never build.

Two sources could NOT come along, because argus-voice compiles them too:
the PCM resampler and the TTS HTTP client. They became their own modules
rather than either service reaching into the other — `argus::audio` and
`argus::tts-client` (which also carries `tts-wire.hxx`, the contract the
client and argus-tts both speak). `media-relay.{cc,hxx}` came with the
stream folder even though only `labs/` uses it; it is stream-domain code
and labs is out of scope for this arc.

What did NOT move: the camera-domain repositories and schemas
(`camera`, `camera_stream`, `zone`), which `argus_sync` still compiles
because the gateway's `/sync` reads the same rows. `argus_camera-rpc`
therefore still carries `../src` on its include path — the one raw reach
left in this folder, and it goes away when the sync split lands.
