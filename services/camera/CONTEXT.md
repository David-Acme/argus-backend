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
  down. Go2rtcManager owns the go2rtc lifecycle here. A fragment is reserved
  against the connection's credit window whole, then chunked: a refusal
  skips it to the next keyframe instead of sending the head of a box, which
  the player cannot append. An idle connection admits one fragment larger
  than its window, since a 1080p keyframe can exceed the 128 KiB default.
  Each subscribe prunes the id map of subscriptions the hub already dropped
  (a dead upstream, a failed send, a closed socket) and joins dead upstreams;
  they used to stay until the 16-bit id space ran out. The reader thread is
  the only one that closes its upstream socket; shutdown sets `stopping` and
  the one-second receive timeout does the rest.
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
  A change reaches go2rtc as one batch (`applySources`): the boot hands every
  enabled camera over at once, and an edit that leaves both URLs as they were
  (a rename, a zone, a record mode) rewrites nothing and restarts nothing — a
  restart drops every camera's stream and the operator's frames with it. Only
  a real change restarts go2rtc, once per batch, off the event loop; the frame
  grab tolerates that restart.
- **Supervision**: the supervisor checks health under the manager's lock, so
  the window an intended restart opens is never mistaken for a crash, and its
  restart budget (`streaming.max_restarts`) is refilled after 60 s healthy.
  It used to be a lifetime count, and supervision stopped for good after the
  eighth blip.
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
- **PTZ is one route and two protocol calls**: `/camera/{id}/ptz` takes
  `angle` (a relative step, 0-359, the protocol's own direction) or `x`
  with `y` (an absolute move) and never both; the driver maps the first to
  `relativeMove`'s `motor.movestep.direction` and the second to
  `motorMove`'s `motor.move.x_coord`/`y_coord`, both as strings — byte for
  byte the shape the vendor app and pytapo send. Measured against the
  device (2026-09-27): a pan already at the end of its travel answers
  `-64304` `MOTOR_LOCKED_ROTOR`, and the driver returns the device's own
  words instead of a silent success, so the app sees the refusal.
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
  service shares the listener (F6-3 shape). Because that metadata is plain
  text, `PullTable` and `ListCatalog` first require a caller credential:
  argus-sync's (`[grpc] caller_sync`, paired with sync's `[camera]
  credential`) or argus-llm's for the catalog snapshot (`[grpc] caller_llm`,
  paired with llm's `[camera] credential`). Without it any container could
  read every camera row, credentials and stream URLs included.
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
- **The scene diff is a correlation, and the reference follows the camera.**
  `sceneDiff` compares the z-scored 160×90 frames (mean absolute difference
  over its value for an inverted image): 0 is the same view, ≈0.71 an
  unrelated one, and the 0.35 threshold means a correlation below ≈0.75. It
  replaced the raw grey difference, which a dimmer room or the IR switch
  moved more than a re-aimed camera did (two unrelated textures of equal
  brightness differ by ≈0.18 on that scale, under its 0.35). The reference
  drifts toward frames well inside the threshold (1/8 per tick), so shadows
  and furniture creep do not accumulate. A new view that holds — stable
  against the previous frame — for `[health].rebaseline_after_s` (900 s)
  becomes the reference: guard's `camera_tamper` needs 300 s of sustained
  `moved`, so real tampering is still reported once, and a re-aimed camera is
  not reported for ever. Aiming through Argus (a move, a preset goto, a
  day/night change, privacy off, a new address) re-baselines at once through
  `CameraSceneLog`, and a camera Argus put in privacy mode is not sampled.
  What Argus did not do (the vendor app, auto-tracking) waits out the window.
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

## The talk channel: the vendor app's shape, the line's budget, and the call

The speaker path is the Tapo local media API on port 8800: a `POST /stream`
multipart request whose HTTP Digest challenge
(`realm="TP-Link IP-Camera"`, `algorithm="MD5"`, `encrypt_type="3"`, `qop="auth"`)
is answered with `admin` as the username and the **uppercase** hex of the
cloud password — SHA-256 when the challenge says `encrypt_type="3"`, MD5
otherwise. The two hex cases in that sentence are not interchangeable and
mixing them costs a day: the **derived password** is uppercase hex (go2rtc's
`%32X`, pytapo's `pwd_digest`), while the **digest itself** — HA1, HA2 and
the response — is computed and compared in **lowercase** (pytapo's
`.hexdigest()`, RFC 2617's canonical form). Uppercasing HA1/HA2 changes the
MD5 input, so the response comes out as a different value entirely, not the
same value in another case; the camera then answers 401 on a correct
credential, indistinguishable from a busy line. Commit `a17ef9be` made that
mistake, `18f51d71` undid it, and both `tapo-crypto-test` (the exact
response for fixed inputs) and the talk suite's fake (a verifier in the
camera's canonical form) now pin it. The dialect is per-device and not
predictable from model or firmware (a C120 on 1.4.3 accepts only the MD5
derivation while advertising `encrypt_type="3"`), so a rejected digest
retries the other derivation exactly once, bounded at two connections per
open. The challenge is answered **on the same TCP connection that issued
it** and never reused: the nonce is single-use.

**The uplink is the app's own cadence.** Measured from the vendor app's
captured stream and the decompiled client: 1504-byte parts every 120 ms —
PAT, PMT and six MPEG-TS packets carrying exactly 960 A-law bytes with PTS
deltas of 10800 ticks — under `stream_type 0x90` and PES `stream_id 0xC0`,
with standard PCMA `0x06` rejected as silence. The client sends that shape,
never front-loads (these cameras discard excess) and keeps the average period
rather than resetting the clock on lateness. The framing itself has the
`plain` mode of old firmware only as history; the parts go out plaintext with
`X-If-Encrypt: 0` exactly as go2rtc's backchannel and the app's own muxer do.

**The line is held, and only an explicit stop releases it.** No reference
implements a keepalive; a session that is not stopped keeps the 8800 line
busy, so the next client authenticates, receives no session id and plays
silence — the 401-shaped state this tree chased for a day, cleared only by
the camera's own timeout or a restart. The release is the vendor's request,
sent as an AES-128-CBC JSON part on the same connection before the socket
closes: `{"type":"request","params":{"stop":"null","method":"do"}}`. The
client derives the cipher material from the Key-Exchange header on every
authentication (key = MD5(nonce:hashedPassword), iv = MD5(username:nonce),
`username="none"` meaning media encryption is off) and sends that stop from
`close()`, so every session this service opens is released on the way out.

**The talk connection carries the speaker, never the microphone.** The
uplink is all this port does: go2rtc's own Tapo backchannel opens the talk
request on a connection of its own and reads the camera's audio from the
media stream, and the device agrees — a session opened here and left silent
receives nothing at all (measured 2026-09-27: zero parts in six seconds, with
and without a silent uplink priming the session). The camera's microphone is
the sub stream's `pcm_alaw` at 8 kHz, which this service already holds warm
for the operator's frames, so `Listen` captures it as 16 kHz mono through
go2rtc's RTSP listener: one transport for the voice in, one for the voice
out, and no second connection for either. The camera ducks its microphone
while it speaks — the same behaviour is visible in the vendor app — so a
conversation is half duplex: speak, stop, listen, never both at once. Video
does not come from here either: RTSP is the documented, maintained path and
one pull through go2rtc serves every viewer.

**Nothing outlives its camera.** Updating a camera evicts its cached
driver, so the next control, talk or siren call reconnects with the new
address and credentials instead of the stale ones; removing it also clears
its frame and person crops from `SnapshotStore`. A `camera:subscribe` that
resumes after its socket closed is dropped instead of storing a sink for a
dead connection, and a stream the hub closes (`upstream_closed`/`failed`)
releases its slot in the per-connection subscription count, which used to
leak until a long-lived socket hit a false 429. The `grpc.health.v1`
`Watch` reactor deletes itself when the stream ends (here and in voice).

**A camera address is a literal IP.** `ip` must parse as IPv4 or IPv6 on
create and on update: it is concatenated into the RTSP URLs and the Tapo
client's endpoint, and go2rtc's private-host check compares prefixes, so a
Resident could set `10.evil.example` (or `10.0.0.1@attacker`) and send the
camera's credentials to an outside host. `recordMode` on update is checked
against its values instead of falling back to `events`.

**Arming the siren sounds it.** `SetSiren` used to enable only the
camera's detection alarm (`setAlertConfig`), which sounds on the camera's
own motion trigger, so guard's siren effect was silent unless the camera
happened to detect motion itself. A siren request now also starts the
camera's manual alarm (`{"method":"do","msg_alarm":{"manual_msg_alarm":
{"action":"start"}}}`, the request pytapo's `startManualAlarm` sends) and
the disarm - the explicit one or the lease sweeper's - stops it. A model
that refuses the manual alarm is logged and still gets the detection
alarm armed. Not exercised against the device here: sounding the siren is
the owner's own test.

**Every event says whether it is night.** The object event carries
`night`, the operator's own `night_start_hour`/`night_end_hour` verdict for
the frame. The rule alone could not say it: `person_night` is only emitted
for a person outside every zone, so an intruder in a monitor zone at 03:00
looked like daytime to guard.

**No face is not a stranger.** The known-person matcher maps identity's
`face_found = false` to `Unobservable`: the person was scanned but no face
was visible, so guard does not count them as a stranger, and auto-enrolment
is skipped for a crop that has no face to enrol. A response without the
field (an older identity) keeps the previous meaning, `Unrecognized`, and a
failed identify call counts as no verdict.

**A capture that endpoints keeps what it heard.** The ffmpeg pull reads a
live stream, so it never ends on its own before `-t`; when the endpoint
detector closes the turn, the reader stops and ffmpeg is terminated, and
its exit status no longer decides the result. Before, the closed pipe made
ffmpeg exit with a write error and every endpointed capture - the normal
case, a visitor who stops talking - came back `InvalidAudio` with its
samples discarded. `camera-audio-capture-test` drives the real fork/exec
path against a stand-in `ffmpeg` on `PATH` that streams a voice and then
silence forever. The pipe is opened `O_CLOEXEC`, so a concurrent capture
or the go2rtc spawn never inherits its write end.

**Two budgets bound every consumer.** FAQ 2742 states the local limit: three
concurrent live viewers, RTSP and ONVIF connections counted in. Overload
surfaces as 401 or "Invalid authentication data" on the *new* connection
(go2rtc #1801: "your camera just overload"), which is why this service holds
exactly one camera-facing RTSP pull — the sub stream, shared by the
operator's frames and the voice capture — plus the talk session's single
persistent connection, and why a viewer that asks for `main` quality is the
only consumer allowed to add a pull, on demand and only while it watches.
The sub stream is preloaded (`preload:` in the generated config), so that one
pull stays open while the operator polls frames: go2rtc dials the camera once
and every frame grab, every voice capture and every viewer rides the same
producer. Measured on the live line (2026-09-27), the same twelve seconds at
an idle 2 Hz detection cadence held 17 fresh RTSP sessions before that line
and one after it.
Measured on the live line (2026-09-27): with a `main` viewer watching
beside the sub pull and the persistent talk session the camera sat at
three connections, its whole local budget, and a talk cycle still
authenticated and opened a new session; when the viewer stopped, the
`main` pull went away on go2rtc's own idle timeout and the camera was left
holding the talk session alone. A
burst of probes also fills the budget; the probe is a diagnostic, run when
the line is quiet and never in a loop.

The Streamd advertises `X-Preconn: 1` and `X-Hb: 5` in its 401; no reference
client implements that handshake. ONVIF is not an alternative backchannel:
TP-Link's own FAQ puts this model at Profile S, and Profile S has no audio
output — two-way audio is Profile T, which the C225 does not implement.
TP-Link documents no third-party path to the speaker at all, so everything
above is reverse-engineered, unsupported and firmware-volatile: the probe
remains the arbiter, and the device-side ladder for a 401 that used to work
is 2FA off, Third-Party Compatibility re-toggled with the camera online,
firmware 1.3.2 Build 260811, re-add the camera, factory reset.

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

## Detector and health frames come from the sub stream

`Go2rtcFrameSource` — the one frame source the operator and the health
monitor share — asks go2rtc for `camN-sub`, the 1280×720 substream, where it
used to pull `camN` and decode 2688×1520. The detector's input is 640 px and
the VLM downscales to `[vision] max_input_px`, so the 2K frame bought no
signal and cost a decode plus a resize on every tick; the crops the guard
assesses are lighter for it too. `Go2rtcManager::subStreamName` now carries
the one spelling of the `-sub` suffix (the registrar and the stream hub read
it instead of appending it themselves), and the app's live view is untouched
— it already picked `sub` by default. Measured: `frame.jpeg?src=cam1-sub`
answers 1280×720 where `cam1` answers 2688×1520.

## Reaction latency belongs to the dwell, not to the dialogue

The installation's owner asked for a greet that lands as the person arrives,
in Home mode, and for an away-mode announcement that does the same. The
chain was measured end to end: the guard greets on the first observation of
an encounter and the away announce speaks the templated `announce_text` — no
model round trip in either path — so the only real delay was the operator's
zone dwell before an event is published at all. `dwell_alert_ms` is 400 and
`dwell_night_ms` 800 now (was 3000/8000, the night figure kept higher
because night noise is what it damps), in the code default, both example
configs and the operator's own config, which puts the first
`person_in_alert_zone` event within an inference tick of a person appearing.
Everything downstream — greeting, announcement, the assessment that follows
— is unchanged; `dwell_monitor_ms` still delays monitor-zone events only.
The assessment's VLM and LLM runs stay where they were: they shape the
decision and the spoken line's grounding, never the first reaction.

