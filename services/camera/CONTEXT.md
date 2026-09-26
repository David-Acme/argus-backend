# argus-camera — CONTEXT

## Why the camera service exists

Fase 2 of the `migracion-microservicios` plan starts the service split with a
pilot: the camera domain leaves the monolith. This task (F2-1) creates the
substrate WITHOUT cutover — the legacy kept owning every camera write and
the app kept talking through the gateway unchanged. `argus-camera` is a
sibling service: own binary, own CMake preset, own `camera.db`.

## What it owns (F2-1)

- **camera.db**: the camera-domain tables (`camera`, `camera_stream`,
  `zone` plus their 8 indexes), DDL copied verbatim from
  `database/schema.sql`. The schema lands as
  `services/camera/database/schema.sql` (same pattern as
  `services/identity/database/schema.sql`) and
  is applied at boot through `DbService::runScriptFile` — abort on failure.
  `argus.db` is never touched here.
- **Wiring**: Drogon boot with the camera domain only — an app-facing TLS
  listener (`[camera] host/port`, 7026, the instance certificate from
  `[cert]`) announced per logical route over mDNS, a plain gRPC listener
  (`[server] grpc_port`, 7036) for the sync pulls and the guard's action
  surface, `DbService` default
  client on `[camera] db` (default `database/camera.db`), `[drogon.app]`
  mirror, CORS/exception/404/405 plumbing from the shared `lib/http` handlers
  so envelopes are byte-shape-identical. Caller validation rides the callers'
  own credentials (`[grpc] caller_guard` for the action surface) and the user
  metadata the sync pull carries; the operator's known-person matcher reaches
  argus-identity over `[identity] target` behind `[identity] rpc_secret`, and
  no camera code opens `identity.db`. No AI service registry is compiled
  or loaded
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
  `[camera] db`. Camera and zone updates publish the before/after pair
  through `camera_change::sink()->publishAudit`, emitted as a
  `ModuleAuditEvent` (`kind: audit`) over
  `argus.camera.v1.change` (Ruling Y) — no audit rows are persisted locally.
  Creates/deletes emit `Add`/`Delete` change events on the same subject.
- **Media**: `CameraMediaService` handles the native `camera:*` frames of
  this service's own `/media` WebSocket (TLS, port 7026):
  `camera:subscribe` checks the camera row (404 Camera
  not found), subscribes through StreamHub fMP4 with the `0xA7` frame magic,
  and degrades to the legacy `503 go2rtc_not_running` envelope when go2rtc is
  down. Go2rtcManager owns the go2rtc lifecycle here.
- **Identity reads (RPC-only since rule 27)**: this service opens no
  identity.db. `SyncService::refreshContext` resolves the connecting user
  through `IdentityUserDirectory` (injected into the sync socket), a
  `std::shared_ptr<const IUserDirectory>` backed by
  `argus.identity.v1.GetUser`; the filters validate over
  `argus.identity.v1.ValidateToken` at `[identity] target`. Camera.db is the
  only database this process opens.
- **Source registration**: `CameraSourceRegistrar` syncs go2rtc's
  `cam<id>`/`cam<id>-sub` sources at boot and on camera create/update/remove
  (disabled rows drop their sources). The URL derives from the camera row
  (`rtsp://user:pass@ip:port/stream1|stream2`, credentials percent-encoded).
  go2rtc restarts per source change and the frame grab tolerates those
  restarts, so neither the operator nor the HTTP loop is blocked by it.
- **Explicit controller registration**: the camera, zone and camera-control
  controllers live in the shared static library, so their AutoCreation
  registration is linker-dropped there; this service registers them
  explicitly.
- **OPTIONS divergence attribution (F2-4 review)**: unlike the legacy
  (`src/config/application.cc` pre-routing advice answers every OPTIONS with
  `Cors::handleOptions` before routing), argus-camera registers only the
  post-handling CORS advice, so `OPTIONS /camera` 404s here where the legacy
  answers 200. App-safe as shipped: the native client sends no preflight.
- **What stays away**: no voice path, no alarm-triggering code, no AI
  symbols beyond the detector.

## Device control (F6-2): camera-control routes + the TTS wire

- **camera-control moved out of the legacy**: `feature/camera-control/`
  (routes `/camera/{id}/status|presets|ptz|preset|settings|capabilities|
  talk`) lives here now, same controllers/dtos/services layout as the
  monolith (the `api/` level it was written under went in Phase 4 step 3).
  This service serves every `/camera` and `/zone` segment depth
  itself; the legacy serves no camera route anymore.
- **Talk synthesis is remote-only**: `TtsClient` (tts-remote) is compiled
  into this binary and every synthesis is an HTTP exchange with argus-tts
  (`[tts] remote_url`, default `127.0.0.1:7029`). No in-process TTS engine
  exists here — with the key empty every talk call fails with the 502
  `CAMERA_UNREACHABLE` envelope, never a fallback. Synthesis runs before the
  device call (a TTS failure never opens a talk session for nothing), and a
  failed driver session is dropped so the next call logs in again instead of
  reusing a transport the camera has already closed.
- **Driver stack**: `camera-driver` (registry + Tapo driver) and the tapo
  transport stack are the `src/shared/services/{camera-driver,tapo}` modules
  since Phase 4 step 3 gave each its own declaration (OpenSSL linked); the
  typed config is `argus::camera-config` (`src/config/`) and the camera_change
  sink is `argus::camera-change-sink` (`src/shared/services/change-sink/`)
  since Phase 4 step 9.
  `[tapo]` config keys are read here, mirroring the legacy block.
- **Legacy slimming**: the legacy binary no longer compiles the camera/
  zone features, the camera-driver/tapo/stream stack, nor `camera.db`
  access; `SocketCameraChangeSink` and `CameraAudioSource` were deleted.

## Build wiring (decisions)

- The canonical camera build is its standalone graph. From the repository
  root use `scripts/build-all.sh dev --only camera`; direct builds run
  Conan, the matching preset and CTest inside `services/camera`.

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
- **Listen (fleet-gated, guard only)**: `CameraActionService.Listen` captures
  the camera microphone over go2rtc's RTSP listener
  (`rtsp://<streaming.go2rtc_rtsp>/cam<id>`, ffmpeg PCMA 8 kHz -> 16 kHz mono
  s16) and transcribes through argus-stt; the `/api/stream.mp4` muxer drops
  the audio track, so RTSP is the only path that carries voice.
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
  file retention). Publishing requires the PubAck: an observation the broker
  cannot store returns false and is retained in a bounded in-process queue
  that the sink retries until it is stored, so an outage never drops it. The
  stream is ensured through the shared `NatsBus`, which supervises its own
  reconnection. `argus-notification` subscribes ephemerally over core NATS,
  so events published while it is down are not replayed after a restart; the
  guard's durable consumer is the replay path.
- **Per-track person events**: each eligible person track produces its own
  event with rule, severity, identity, zone, dwell, crop and cooldown bound to
  that track; companions travel only as context and never decide. The
  cooldown advances only after the outbox transaction returns Recorded, so a
  failed enqueue keeps the pending event for the next window. `/health`
  `objectEvents` hydrates from SQLite at boot and counts real overflow drops;
  both numbers are live counts over the retained window, so they read as
  gauges — settled rows age out with the feed — and never as accumulating
  counters.
- **Siren lease sweeper**: an expired lease is disarmed and deleted only
  after the driver confirms `alarm=false`. If the camera row or its driver
  is unreachable the lease is kept and retried with an error log; a failed
  disarm is never treated as "off".
- **Budget split (Ruling AD)**: camera side = aggregation window +
  per camera+class cooldown + `max_fps_inference`; notification side =
  budget/silent hours/digest (see argus-notification CONTEXT.md).
  Inside one aggregation window objects dedupe by class and the pending
  event keeps the dominant severity; a class still cooling down drops the
  whole window and the next window starts fresh. Preprocessing and
  inference never run on the event loop (`BlockingTask`).
- The former camera labs were deleted in F8. Detector validation belongs in
  the production owner or an external diagnostic, never a second build graph.
- Model artifacts (`models/objects/`) come from `scripts/setup.sh camera`:
  yolo26n.pt sha256-pinned download + local raw e2e NCNN export; without the
  artifacts the detector boots disabled — never a fake success.

## Camera sync gRPC leg (F6-5 step 1)

- **`argus.camera.v1.SyncService.PullTable`** (port 7036, `[server]
  grpc_port`): serves the camera-domain sync tables (camera, camera_stream,
  zone) to `argus-sync` with the frozen /sync semantics typed into
  `packages/contracts/proto/argus/camera/v1/sync.proto` — per-table
  required_create/required_deleted/find_last legs, (createdAt, id) cursor
  ranges, LIMIT 200 baked into the owner's SQL, tombstones as
  {id, deletedAt}. The implementation calls the same camera/camera_stream/
  zone repositories the CRUD surface uses, so the served rows are the exact
  JSON the sync tables always produced. The caller identity rides
  x-argus-user / x-argus-role / x-argus-device metadata: presence is
  required, the role was validated once upstream by `argus-sync` before the
  call, and
  the camera tables carry no userId row scoping. A grpc.health.v1 Health
  service shares the listener (F6-3 shape).
- No other service opens camera.db: `[camera] db` is this service's own key
  and no other config resolves it. argus-camera stays the single owner of the
  file.

## The camera change feed (3a-2b)

- Every camera or zone change lands in `change_outbox` in camera.db before
  it is published to `argus.camera.v1.change`: the mutation commits first,
  the change row is written after it, and a worker publishes from the table
  and marks a row `sent` only on the JetStream PubAck. A crash between the
  two leaves the row pending and the change is retried at the next boot. The
  object-event sink's bounded in-process queue (see Ruling AF) survives a
  broker outage but not a restart, which is the gap this closes.
- **The event id names the event, not the record.** JetStream dedups on
  `Nats-Msg-Id`, so an id derived from the record alone would swallow a
  record's second change as a replay. Deriving it from the transition's own
  payload makes a redelivery the same id while a record that moves again —
  or returns to a state it already held — is its own row. Rows and audits
  share that rule: `a→b→a→b` is four events, and the audit service merges a
  repeated same-day diff anyway, so a duplicate converges instead of
  double-counting.
- **A refused enqueue does not fail the request.** The mutation has already
  committed, so a 500 would make a retrying client create a second row; the
  write is retried a few times over the one shared connection and then
  logged loudly. A payload past the broker's budget (256 KB) is refused at
  the door instead, because a row the broker would refuse for ever parks
  every change behind it — the drain is oldest-first and retries for ever,
  since a broker outage fails all rows equally but one poisoned row would
  cost real changes. The first refusal is logged, so an operator sees the
  feed stop instead of reading about it every 100 attempts.
- Both sinks declare the stream: the change sink and the object-event sink each
  ensure `ARGUS_CAMERA` through `shared/services/event-stream`, so a refused
  publish re-arms from whichever feed refused. `NatsBus::ensureStream` refuses
  to repurpose a stream carrying different subjects, so the default
  configuration's subject pair has one declaration rather than two that could
  disagree with each other.
- **The drain is stopped before Drogon quits, not after.** Both sinks register
  with `shutdown_signal` at boot, **before `drogon::app().run()`** — which is
  also before `reconcile()` starts their workers:
  the SIGTERM/SIGINT
  handler only requests the stop, the Drogon loop keeps running while the
  worker finishes its publish pass, and `quit()` follows once it reports
  drained (a 10-second deadline bounds the wait and names any drain that never
  reported). The worker is a thread of the sink's own, so nothing in Drogon
  stops it — and `quit()` destroys the database client manager, which a
  worker still publishing from the outbox would reach unguarded. The
  registration comes first because the hook's handlers are what `run()` installs
  Drogon's `sigaction` over, and because a drain registered after the stop was
  requested is only stopped at once, never waited for.
- A PubAck is storage, not delivery. The subject's only live consumer today
  is memory's catalog replica, snapshot-filled at boot by design; app
  convergence runs through the sync engine's own paging over the camera sync
  RPC, so the producer's durability boundary is the right one.

## The folder owns its domain (f7-7a)

The camera CRUD features, the Tapo driver stack, the stream lifecycle and
the camera schema all moved out of the shared `src/` tree into this
folder, prefixes preserved (`src/feature/api/{camera,zone}` then, the
`api/` level flattened to `src/feature/{camera,zone}` in Phase 4 step 3,
and `src/shared/services/{camera-driver,tapo,stream}`), so no include line
in the fleet changed. The unit suites live in `tests/unit/` and register in
the camera project's standalone CTest graph.

Two sources could NOT come along, because argus-voice compiles them too:
the PCM resampler and the TTS HTTP client. They became their own modules
rather than either service reaching into the other — `argus::lib::audio` and
`argus::clients::tts` (which also carries `tts-wire.hxx`, the contract the
client and argus-tts both speak). `media-relay.{cc,hxx}` came with the
stream folder then even though only `labs/` used it, and `d2756952` (f8-a2)
deleted it with the rest of the labs surface.

What did NOT move: the camera-domain repositories and schemas
(`camera`, `zone`), which `src/shared/repositories` declares
as the folder's own `argus::camera-repositories` module since sub-step 3a-1b —
`argus-sync`'s `/sync` still reads the same rows, through the camera sync RPC.
`camera_stream` is the one that did move, into `feature/sync` (Phase 4 step 3:
the sync RPC service is its only reader), so `argus_camera-sync` carries
`src` on its include path the same way.

## Operator automation extensions (camera-guard phase 1-2)

- **Zones come from camera.db** when `operator.zones_from_db` is true (default):
  `ZoneProvider` loads enabled `zone` rows with a `zones_refresh_ms` TTL and
  falls back to `operator.zones` only while the DB has never answered, so the
  app zone editor now drives detection.
- **Camera discovery is live**: `camera_rescan_ms` re-reads enabled cameras and
  loops start/stop per camera, so a camera connected after boot is monitored
  and a disabled one stops.
- **Motion gate** (`operator.motion_gate`) skips YOLO on a static downscaled
  frame diff; the **static-box filter** (`operator.ignore_static_persons`)
  drops a person box that has not moved for `static_box_frames` frames.
- **Identity enrichment**: with `[identity].identify`, the
  `IdentityKnownPersonMatcher` JPEG-encodes the person crop and calls
  `argus.identity.v1.IdentifyPerson`/`EnrollPerson` (fleet secret) with
  per-camera throttling and enroll cooldown. The event carries
  `objects[].personId`/`identity`; a known match keeps the `known_person` rule
  and an enrolled unknown keeps the security severity. Enrollment persists the
  person, embedding and snapshot in identity, never here.

## Health monitor, snapshots and guard actions (camera-guard phase 3+)

- `CameraHealthMonitor` samples each enabled camera every `[health].interval_ms`
  and classifies brightness, Laplacian sharpness and normalized scene diff
  against the first reference frame: `dark`, `bright`, `blurred`, `moved`,
  `unreachable`, plus `covered` (dark and featureless). Transitions
  publish `argus.camera.v1.health`; `GET /health` never touches this path.
- Steady states republish every tick (Round 12 heartbeat): guard's
  sustained-tamper check needs a continuous sample stream, and a
  transition-only feed goes permanently stale. One small JSON per camera
  per minute; no new key.
- The `insect` state was dropped (Round 14): whole-frame
  variance-of-Laplacian cannot separate an insect on the lens from a sharp
  static background, so the state fired on every healthy camera. Tape,
  sticker and web occlusions remain covered by `covered`/`blurred`/`moved`.
- `SnapshotStore` keeps the latest full frame and the latest identity person
  crop per camera (bounded, TTL-free, overwritten). `CameraActionService`
  exposes `GetPersonCrop` so argus-guard can assess without touching frames
  directly.
- `CameraActionService` (fleet secret + `[actions].enabled`) is the only
  audible path: `Announce` runs TTS through the existing control feature,
  `Alarm` plays a procedural siren tone over the talk channel, and `SetSiren`
  arms/disarms the vendor alarm. The operator never calls these.
- **Stop and drain (D22)**: `CameraOperatorService` and `CameraHealthMonitor`
  each carry the `requestStop()`/`drained()` pair the `camera-operator` and
  `camera-health` drains adapt, and `drained()` is counted rather than guessed:
  the shared `in_flight::Guard` (`src/shared/utils/in-flight/`) is held by
  exactly the three places that query `camera.db` — the operator's `rescan()`
  and `processFrame()`, the monitor's `loadCameras()` — so the module waits for
  the statement in hand and no longer. Neither stop blocks, as D22 requires:
  each is one `running_` store, and the per-camera stop tokens stay the rescan
  path's own. A stopped service ends each camera loop at that loop's next check
  — `runCamera` re-checks after `grab()` resumes and drops the frame in hand —
  while a frame that starts anyway is served by the freeze (D23). `main`
  requests both again once `run()` returns, and the monitor's destructor does
  the same. What `processFrame` enqueues is written by camera's object-event
  sink, whose own drain covers that leg; the evidence insert that follows an S3
  upload and the retention sweep are loop work no drain owns, and reach the
  database through the frozen client.

## Adaptive cadence and best-shot (guard flow v2)

The operator never runs a fixed frame rate: it idles at `max_fps_inference`,
steps to `active_fps` while motion is processed and holds `burst_fps` for
`burst_ms` after a person appears, so a brief clear-face frame is captured
without paying full-rate inference all day. Dwell thresholds per zone and
day/night gate the person events, and every event carries `trackId`, `dwellMs`
and a compact Lab histogram signature for cross-camera correlation. The
identity matcher implements best-shot: it scores each crop (area x sharpness),
keeps the best result inside `best_shot_ms` and only spends an identity call
when a clearly better frame arrives.

## Observation identity v2, leases and retention (guard flow v2.2)

- Every published event is `schemaVersion: 2` with an `eventId`
  (`cameraId:publishedAt:sequence`) used both for JetStream duplicate
  suppression and for argus-guard's inbox.
- The per-camera tracker assigns tracks with a global greedy IoU pass that
  cannot reuse a track inside one frame, and freezes a person per track; person
  objects are never deduplicated by class, so two people stay two records with
  their own `trackId`, `dwellMs`, `firstSeenMs` and `observationId`
  (`cameraId:trackId:firstSeenMs`).
- Identity verdicts are cached per `(camera, track)` inside the best-shot
  window, never per camera, so one person's verdict cannot bleed into another.
- `SnapshotStore` keeps the latest crop per track (bounded ring) and
  `GetPersonCrop` serves exactly the requested track; guard rejects a capture
  outside the observation window.
- `Listen` captures 16 kHz PCM with an energy endpointer (early stop on
  trailing silence, `speech_detected` in the response) instead of a fixed
  recording.
- Camera action commands carry a `commandId`, encounter id and expiry; the
  `action_command` inbox makes retries idempotent. Siren arming is a lease in
  `siren_lease`; a sweeper disarms expired leases even when guard is gone.
- Detection evidence uploads record a `camera_evidence` retention manifest and
  a daily worker deletes expired objects.

## Observation identity v3 and track history (Round 6)

Events are `schemaVersion: 3`. The matcher reports a tri-state per track
instead of collapsing everything into `unknown`: `known` (matched),
`unrecognized` (a face was analysed and matched nobody) or `unobservable`
(no analysable face was ever observed — gate reject, encode failure or
identification disabled). The legacy `identity` string keeps its v2 meaning
for compatibility and is still emitted only when `personId > 0`; the new
`identityState`/`identifyAttempts` ride on every track-bound person object.
Identification scans are counted per track in the matcher cache, so the
attempt count is instrumentation, not a heuristic.

Each track also carries a bounded window history (10 samples of detector
confidence and box area) with in-zone and total window counters. The event
publishes the median confidence with its sample count plus the area spread,
so guard's belief engine reads real history instead of an instantaneous
score. Additive keys only: v2 consumers that ignore unknown keys keep
working.

## Phase 4 step 3: one module per folder (f4-3)

The service's build stopped being a single `camera-core` archive that listed
52 sources by path and handed the same list to the executable and nine test
targets. Every
feature, the shared service folders, the domain folder beside them and the
gRPC listener are now rule-25 modules: each folder declares its own sources
and dependencies once, the root file discovers `feature/*/CMakeLists.txt`,
`src/camera`, `src/shared/{repositories,services}` and `src/app/rpc`
explicitly, and `argus-camera` links `argus::camera-{core,actions,feature,
camera-control,zone,media,health,sync,rpc-server,monitor,operator}` by name.
`argus_service()` now bootstraps the executable (so it carries `-Wall
-Wextra` on the line the old file spelled separately, and the `$ORIGIN` rpath
and `ARGUS_PORTS 7026 7036` the hand-rolled `add_executable` block never set),
and no test target lists a `.cc` file any more — each links the module that
owns the code it exercises.

Two declarations inside the moved repositories were deleted rather than
carried: `feature/actions/repositories/action-command/CMakeLists.txt` and
`feature/operator/repositories/object-event-outbox/CMakeLists.txt`. Rule 25
lets a parent discover modules, but only through the glob the parent actually
uses (`feature/*/CMakeLists.txt`), so a module nested one level deeper would
have been a directory nobody builds. The tree keeps no such declaration
anywhere else: a feature compiles its own repositories in its own module, and
the two repositories are the feature's own (the notification-token family is
the same shape).

The gRPC listener left `main.cc`: `src/app/rpc/camera-rpc-server.{hxx,cc}`
resolves `GrpcListenerConfig` (the `server.grpc_port` key, 7036 by default),
registers the services it is handed and owns the shutdown, so `main.cc` no
longer resolves a listener or names a port. The three gRPC services
themselves stay where their domain is — `argus.camera.v1` in `feature/sync`,
`CameraActionService` in `feature/actions`, `grpc.health.v1` in
`feature/health` — because `app/` is process composition only and
`camera-action-rpc-service` holds a repository and the guard-credential
policy, which rule 23 keeps out of it. The listener takes `grpc::Service*`
pointers, so the modules stay independent of each other.

`shared/` was re-measured against the 2+ rule rather than assumed, and four
families moved into their single reader: `action-command` into
`feature/actions`, `object-event-outbox` and `evidence` into
`feature/operator`, and the `camera_stream` repository and schema into
`feature/sync` (the sync RPC service is their only reader; the shared module
keeps `camera`, `zone` and `change-outbox`). Three modules stay in `shared/`
with a measured note instead of a move: `services/tapo` is the protocol
stack `services/camera-driver` (itself shared, two feature readers) is built
on, `services/event-stream` is read by `feature/operator`, the change sink
and `main.cc`, and `utils/geometry` is a header-only helper, not a
repository, schema or service — rule 23's 2+ rule names those three, and
`utils/in-flight` does have two feature readers.

The change sink's reader count is the reason `shared/repositories/change-outbox`
stayed: no feature includes it, because both publishing features reach it
through `camera_change::getSink()` in `contracts/sync`, whose concrete
implementation is `nats-camera-change-sink.cc` — `src/camera/` at this step,
`src/shared/services/change-sink/` since step 9 — and whose install
is `main.cc`. That is the same indirect-2 shape the notification service
documented for its own outbox.

## Phase 4 step 9: config resolution into `src/config/` (D20)

`src/camera/` is gone. The two config files the folder held are `src/config/`
now — `camera-config.{hxx,cc}` and `operator-config.{hxx,cc}`, compiled once
by `argus::camera-config` — and the NATS change sink that shared the folder is
`src/shared/services/change-sink/` (`argus::camera-change-sink`). Two types
that only a feature declared are `src/shared/vocabulary/` now, because the
config module names them too: `HealthThresholds` (out of
`feature/monitor/health-event.hxx`) and `OperatorZone` (out of
`feature/operator/event-intelligence.hxx`). `main.cc`
calls the resolvers and wires what they return —
`CameraConfig::resolveDb`, `resolveListener`, `resolveHealth`,
`resolveGuardCallerSecret`, and `operator_config::resolveObjects`,
`resolveOperator`, `resolveIdentity`. What it keeps of its own is
`config.toml` loading (`ConfigService::load`, `ConfigService::drogonConfig`),
the `nats.url` gate on the optional bus — a key `lib/nats` resolves for itself
when `NatsBus` is constructed, so the gate only decides whether the bus is
built — and the two `operator.*` knobs spliced into
`NatsObjectEventSink::Config` at the point that optional sink is built.

## The talk channel: reference shape, the two-step gate, and the media slots

The speaker path is the Tapo local media API on port 8800: a `POST /stream`
multipart request whose HTTP Digest challenge
(`realm="TP-Link IP-Camera"`, `algorithm="MD5"`, `encrypt_type="3"`, `qop="auth"`)
is answered with `admin` as the username and the **uppercase** hex of the
cloud password — SHA-256 when the challenge says `encrypt_type="3"`, MD5
otherwise, exactly go2rtc's rule. The `plain`/`md5` derivation ladder this
client used to try is gone: every extra attempt is another connection to a
camera that counts them (below), and one derivation per challenge is what
the references do. The challenge is answered **on the same TCP connection
that issued it** — pytapo's `HttpMediaSession` re-sends the request head on
its one socket and go2rtc's `req.Write(conn)` retry does the same — and
`tests/unit/tapo-talk-client-test.cc` pins both the shape and the
derivation with a fake Streamd that recomputes the digest and only accepts
an Authorization naming the nonce it issued on that connection.

**The 401 that read as unexplainable was a provisioning gate.** Measured on
2026-09-26 against the live C225 (firmware 1.3.1 Build 260514): with the
correct credential and digest shape every attempt answered 401 for hours —
pytapo's own `HttpMediaSession` and go2rtc 1.9.14 included — while the
control channel (443) accepted both stored credential sets and RTSP kept
streaming. After the TP-Link account's **two-step verification was disabled**
and the camera was rebooted, the very same digest was accepted: five
consecutive opens returned `200 OK` with a `Key-Exchange` header. That
matches the community's record of TP-Link's server-side provisioning
locking local media authorisation (go2rtc #781/#849/#1494,
HomeAssistant-Tapo-Control) and PR #1832's finding that account 2FA breaks
the local `tapo://` auth. The camera can be re-provisioned again whenever it
calls home, so a 401 with a credential that used to work is a provisioning
question first, not a code question — the probe is the arbiter, and the
device-side ladder is: 2FA off, Third-Party Compatibility re-toggled with
the camera online, firmware 1.3.2 Build 260811, re-add the camera, factory
reset.

**The media service counts sessions, and leaks them.** The camera serves a
small number of media sessions — the operator's model is two, one for the
Tapo app and one for an external client, which is why every stream consumer
is fed through go2rtc instead of from the camera directly. A burst of
session attempts (~50 probes in two minutes) left the 8800 service
answering 401 to a correct digest for over fifteen minutes of idleness; only
a camera restart cleared it, matching go2rtc #1836 ("Line is Busy" until a
restart). The client is built around that: one connection per open, one
authenticated attempt per connection, a rejected digest closes the socket
and reports instead of trying more derivations, and `TapoDriver::speak`
serialises talk per camera with its own mutex so two utterances can never
take two slots. One `speak` costs one session, held for the utterance and
closed at scope end. The probe is the exception that takes many connections
by design — run it when the line is quiet, never in a loop.

The Streamd advertises `X-Preconn: 1` and `X-Hb: 5` in its 401; no reference
client implements that handshake and nothing suggests the camera requires
it. ONVIF is not an alternative backchannel: this model exposes Profile S
only, with no `media2` service and no audio output.

## The camera microphone in the fMP4 the app plays

The camera publishes its microphone on RTSP — `pcm_alaw`, 8 kHz mono, on both
`stream1` and `stream2` — but the app used to receive a silent stream, and the
loss was ours: `StreamHub` pulled `/api/stream.mp4?src=camN` bare, and
go2rtc's MP4 consumer with no media filter negotiates H264/H265 video and
**AAC audio only**, so a source whose audio is PCMA is muxed video-only. The
upstream now asks `&mp4=flac`: go2rtc's `ParseQuery` adds PCMA/PCMU/PCM/PCML
to the offered audio codecs, and its MP4 consumer transcodes them to FLAC
inside the fMP4 (`pcm.FLACEncoder`, no external process). Measured against
the live C225: the bare URL produced an init with a single `vide`/`avc1`
track; with `mp4=flac` the same URL produces `vide`/`avc1` +
`soun`/`fLaC` at 8000 Hz mono, and `ffprobe` reads both tracks from the
captured bytes. The WS framing is untouched — init and media boxes are
opaque to `ws-frame`, so the app's player just gains a track it can decode
(Android's MediaCodec plays FLAC in MP4). A camera with no audio track
changes nothing: the offer is per-codec, not per-stream.

The backend's own listening ear is a different path and was never affected:
`feature/actions/audio-capture.cc` shells out to `ffmpeg` against go2rtc's
RTSP restream (`rtsp://127.0.0.1:8554/camN`), which passes `pcm_alaw` through
untranscoded — measured with `ffprobe` against the restream — so the voice
loop's microphone capture keeps working exactly as before.

