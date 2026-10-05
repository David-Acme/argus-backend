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
  The copy names the source's own columns and the verification checksums those
  columns on both sides, refusing only a source column the target lacks
  (2026-10). It used to copy with `SELECT *` and compare the column lists of
  `src` and `main` read as `"src".pragma_table_info(...)`, which SQLite
  resolves against the main schema: the check compared the target with itself,
  and a legacy table that predates a later additive column failed the copy on
  its column count. `tests/unit/camera-migration-test.cc` pins both cases.

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
keeps `camera`, `zone` and, until the 2026-10-05 audit, `change-outbox`). Three modules stay in `shared/`
with a measured note instead of a move: `services/tapo` is the protocol
stack `services/camera-driver` (itself shared, two feature readers) is built
on, `services/event-stream` is read by `feature/operator`, the change sink
and `main.cc`, and `utils/geometry` was a header-only helper, not a
repository, schema or service — rule 23's 2+ rule names those three, and
`utils/in-flight` does have two feature readers. (`utils/geometry` moved into
its only reader, `feature/zone/dtos/normalized-polygon.hxx`, after the
2026-10-05 audit.)

The change sink's reader count was the reason `shared/repositories/change-outbox`
stayed until it was replaced by `packages/lib/outbox` (finding #69, below): no feature includes it, because both publishing features reach it
through `camera_change::getSink()` in `contracts/sync`, whose concrete
implementation is `nats-camera-change-sink.cc` — `src/camera/` at this step,
`src/shared/services/change-sink/` since step 9 — and whose install
is `main.cc`. That is the same indirect-2 shape the notification service
documented for its own outbox.

## The outbox is `argus::lib::outbox` (2026-10-05 audit, #69)

The change outbox was one of five copies (auth, camera, identity,
notification, productivity) that had already diverged. Camera's copy is gone;
`NatsCameraChangeSink` now builds the camera payloads and the
`camera-change:` transition ids and hands them to
`outbox::TransactionalOutbox`, which owns the repository, the relay thread,
the retention purge and the drain. What changed for camera, none of it on the
wire (same subject, same stream, same msg ids, same payloads):

- `change_outbox` gained `subject TEXT NOT NULL DEFAULT ''`, appended by the
  boot migration (`NatsCameraChangeSink::repository().migrateSchema()`, fatal
  on failure, right after the action-schema migration) and declared at the end
  of `schema.sql` so a fresh table equals a migrated one. A row written before
  the column existed reads `''` and is published on the configured change
  subject, as before.
- Pending rows leave in `rowid` order (strict insertion order) instead of
  `created_at, rowid`; the two only differed when the clock disagreed with
  insertion.
- The relay now wakes on every commit (`db_transaction::CommitObserver`), which
  the other four copies already did: a row written inside a transaction used
  to wait out `retryMs` because the wake fired before the commit.
- A relay that cannot publish backs off 500 ms → 1 s → 2 s → 4 s → 5 s
  (`outbox::retryDelay`) instead of retrying every 500 ms forever; progress or
  a commit resets it.

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
dead connection. The per-connection subscription limit counts what the hub
actually holds for that socket (`StreamHub::subscriptionsOf`) instead of a
counter beside it: the counter drifted both ways (a stream the hub closed
was decremented again by a later `camera:unsubscribe`, and a reader that
failed before the counter's increment left it one too high), which either
bypassed the limit or ended in a false 429. `camera:unsubscribe` removes a
subscription only for the socket that owns it. The `grpc.health.v1` `Watch`
reactor finishes on cancel or a failed write, so gRPC calls `OnDone` and it
deletes itself (here and in voice); without a `Finish` it never did.

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

## Owner settings

`src/feature/settings/camera-settings.cc` (`argus::camera-settings`) is the
catalog an owner may change through `argus.settings.v1.Settings`, registered
on the camera gRPC listener (7036) beside the sync, action and health
services (`argus::contracts::settings-wire`). Groups are `detection`,
`alerts`, `actions`, `streaming` and `health`.

| Key | Level | Applies | Group | Range | Fallback |
|---|---|---|---|---|---|
| `objects.enabled` | basic | restart | detection | toggle | false |
| `objects.conf` | basic | restart | detection | 0.2-0.9 | 0.45 |
| `operator.night_start` | basic | restart | alerts | 0-23 | 22 |
| `operator.night_end` | basic | restart | alerts | 0-23 | 6 |
| `operator.cooldown_ms` | advanced | restart | alerts | 1000-600000 | 30000 |
| `operator.person_recheck_ms` | advanced | restart | alerts | 5000-600000 | 30000 |
| `actions.enabled` | basic | live | actions | toggle | false |
| `streaming.max_viewers_per_camera` | advanced | live | streaming | 1-16 | 4 |
| `streaming.max_total_viewers` | advanced | live | streaming | 1-64 | 8 |
| `streaming.hub_window_bytes` | advanced | next session | streaming | 32768-1048576 | 131072 |
| `health.enabled` | basic | restart | health | toggle | true |
| `health.interval_ms` | advanced | restart | health | 10000-3600000 | 60000 |
| `health.dark_threshold` | advanced | restart | health | 1-127 | 25 |
| `health.bright_threshold` | advanced | restart | health | 128-254 | 235 |
| `health.blur_threshold` | advanced | restart | health | 1-200 | 18 |
| `health.scene_diff` | advanced | restart | health | 0.05-0.95 | 0.35 |
| `health.rebaseline_after_s` | advanced | restart | health | 60-86400 | 900 |

How each key applies is what the code does with it, not a wish:

- `actions.enabled` is read by `CameraActionRpcService::actionsEnabled()` on
  every Announce, Alarm and SetSiren call, so it is live with no listener.
  It is the owner's consent switch; who may trigger an action is unchanged
  (argus-guard's capability credential only, rule 10).
- The two viewer limits are read under `hubMutex_` on every subscribe; the
  registry's `onChange` in `src/app/main.cc` calls
  `StreamHub::refreshViewerLimits()`, which re-reads both keys under that
  lock. A new limit applies to the next viewer; nobody already watching is
  dropped.
- `streaming.hub_window_bytes` is read by
  `CameraMediaService::streamWindowBytes()` when a media connection creates
  its sink, so it applies to the next media connection (next session); a
  live connection keeps the credit window it started with.
- The detector, the operator loop and the health monitor copy their config
  once at boot (`resolveObjects`, `resolveOperator`, `resolveHealth`), and
  the detector is built with its confidence; those keys are restart.

A fallback is what the camera runs with when the key is absent, so two
absent-key defaults now match `config.toml.example`: an absent
`operator.night_start`/`night_end` resolves to 22/6 (it was 0/0, which
disabled the night window), and an absent `operator.cooldown_ms` resolves to
30000 (it was 0 for the operator while the object-event sink used 30000).
`camera-settings-test` pins every fallback against the resolvers,
`StreamHub` and `streamWindowBytes()`.

Paths, binaries, the go2rtc addresses, the model directory, Vulkan, the
database and schema, the Tapo ports, the remote URLs and every caller secret
stay out of the catalog.

`[grpc] caller_settings` is the only credential the settings service accepts
(service name `settings`); `SyncService` keeps `caller_sync`/`caller_llm`
and `CameraActionService` keeps `caller_guard`. An empty `caller_settings`,
or one equal to any of the other three, registers no settings service.
`camera-settings-test` checks the separation both ways on a live server
holding all three services, using only refusals and the argument checks
that run before any camera is touched (no action reaches hardware).

## A viewer starts on a video keyframe, and can start on the last one

go2rtc's MP4 consumer muxes the camera microphone as a FLAC track beside the
video (`&mp4=flac`, above), one `moof`/`mdat` per sample and per track, and it
flags every audio sample as a sync sample (`0x02000000` in `tfhd`'s default
flags). The hub used to ask only "is the first sample of this fragment a sync
sample?", so every audio fragment looked like a keyframe: a viewer joining a
live upstream, or one resuming after a refused fragment, restarted on the next
audio fragment and was sent video P-frames before any video keyframe — measured
against a go2rtc test source, two joins out of three received a delta frame
first. `Fmp4Reader` now reads the init segment's `moov` once, finds the video
track by its `hdlr` (`vide`), and classifies each fragment by the track its
`traf` carries (`FragmentKind::VideoKey`, `VideoDelta`, `Other`), reading the
sample flags from `trun`'s first-sample flags, its per-sample flags or `tfhd`'s
default, in that order. A viewer resumes only on a `VideoKey`; a refused audio
fragment is dropped without forcing the video to wait for the next keyframe.
With no video track identified (an audio-only source) the flags alone decide,
as before. A 64-bit box header now waits for its sixteen bytes, and a box past
64 MiB resets the reader instead of buffering it.

Each upstream keeps the current group of pictures (`GopCache`: from the last
video keyframe, audio included, bounded by `streaming.hub_gop_cache_bytes`,
2 MiB; a GOP past the cap is dropped until the next keyframe). A subscriber that
asks `camera:subscribe` with `fastStart: true` is sent the init segment and that
GOP at once, reserved against its credit window as one burst (an idle window
admits it, as it admits one oversized keyframe), and then joins the live
fragments. Measured on the sandbox with a 640×360 / 15 fps / GOP 30 test
source: a second viewer's first frame arrived after 60-110 ms with `fastStart`
against 0.7-1.9 s without it. The replay is opt-in because a player that
schedules frames by their timestamps (Android's ExoPlayer pipe) would carry
the replayed GOP's age as permanent latency; the web player decodes and paints
on arrival, so the burst simply catches up. The first viewer of a cold upstream
still waits for go2rtc's first keyframe (300-700 ms on the same source).

## A camera is online when the monitor can see it

`camera.is_online` had a column, a repository field, a sync field and an app
badge, and no writer: every camera, the working ones included, showed as
offline in the app. The health monitor already grabs a frame from each enabled
camera's sub stream on its cadence, so it is the writer now:
`CameraPresenceRecorder` (`feature/monitor`) receives every sample's
reachability and flips `is_online` only on a transition — one reachable frame
marks a camera online, two consecutive misses offline, so a single slow grab
does not flicker the badge. The flip is a normal camera update: the row and its
before/after audit go through the change outbox in one transaction, so the app
receives it as an ordinary audit `Log`. Nothing is written while the state
holds, and a camera Argus put in privacy mode is not sampled, so it keeps its
last state.

The monitor no longer sleeps a whole interval after each round: it rescans the
camera list every five seconds and samples a camera once `interval_ms` has
passed since its previous sample, so a camera added at runtime is sampled (and
marked online) within seconds instead of a minute, while every camera's
heartbeat cadence is unchanged. One camera's failed sample is logged and skipped
instead of ending the loop: an empty JPEG body from go2rtc used to throw out of
`cv::imdecode` and stopped the monitor for the life of the process. The frame
source treats an empty body as no frame.

The operator and the monitor start after the boot's first source apply instead
of before it: they used to sample a go2rtc that had no sources yet, so every
boot published `unreachable` for every camera.

## The zone a person stands in travels by name

A person object carries `zoneName` beside `zoneKind`: the owner's name of the
first alert zone (or, failing one, monitor zone) that contains the box centre,
as `ZoneProvider` loaded it from camera.db. argus-guard asked for it to word its
notifications ("in «Back door»" instead of "in the alert zone"); the key is
additive and absent outside a zone, so a consumer that ignores it reads the
event exactly as before, and `schemaVersion` stays 3.

## Any RTSP camera, any private address, and a driver for the ones Argus cannot steer

- **Stream paths.** go2rtc's sources were always `/stream1` and `/stream2`,
  the Tapo paths, so an RTSP or ONVIF camera from another vendor could be added
  and never stream. `POST/PATCH /camera` accept `streamPath` and
  `subStreamPath` (a leading `/`, letters, digits and `/._~-?=&%+,;:` only, at
  most 200 characters, so the value can sit in the YAML line and the URL
  without quoting) and keep them in the camera's `config` JSON column (until
  now always `{}`, synced to the app as a string); an empty string clears one.
  The registrar reads them through `shared/vocabulary/camera-stream-paths.hxx`
  and falls back to the Tapo paths, so every existing camera keeps its URLs.
  No credential ever enters `config`.
- **Addresses.** One predicate, `shared/utils/network-address`, decides what a
  camera address may be, for the DTOs and for go2rtc's source guard alike: a
  literal IPv4 or IPv6 address in a private range (10/8, 172.16/12,
  192.168/16, fc00::/7 and the v4-mapped forms). Loopback, link-local and the
  unspecified address (127/8, 169.254/16, 0.0.0.0, ::1, ::, fe80::/10) are
  refused since the 2026-10-05 audit (`network_address::isCameraAddress`):
  they pointed the probe, go2rtc and the Tapo client at RustFS, the internal
  gRPC listeners or the cloud metadata address. The guard used string prefixes, which let
  `10.evil.example` through as a host; the DTO already required a literal, and
  now also refuses a public address with a 422 instead of accepting a camera
  whose stream go2rtc would silently never open. An IPv6 camera is bracketed in
  its RTSP URL; it used to produce `rtsp://fd00::5:554/...`, which nothing
  parses. `PATCH /camera` now checks `port` (1-65535) and the same lengths as
  create; both check `retentionDays` (0-60, 0-120 with `retentionIncident`,
  see "Retention" below) and cap the passwords at 128.
- **Stream-only cameras.** RTSP and ONVIF cameras have no control driver, so
  `/camera/{id}/capabilities` answered 502 and the app offered a PTZ pad that
  could only fail. Camera-control now answers them through `StreamOnlyDriver`:
  capabilities all `false` plus `streamOnly: true`, a status that reports the
  stored model, and a refusal that says the camera only streams for every
  control. The driver registry is untouched, so argus-guard's action RPCs keep
  rejecting these cameras with `no_driver` exactly as before.
- **go2rtc's files.** The generated `go2rtc.yaml` holds every camera's
  credentials; it was written with the default mode and chmod-ed to 0600
  afterwards, and `go2rtc.log` was 0644. The config is now written to a 0600
  staging file and renamed into place (go2rtc never reads half a file either),
  the log is created 0600, and the source guard also refuses a space or `#` in
  a URL, the two characters that would end the YAML scalar.

## A camera that goes silent is closed, and its last GOP is not replayed

When a camera drops off the network, go2rtc keeps the hub's
`/api/stream.mp4` response open and simply stops writing to it while it
redials the source, so the hub never saw an end and never sent
`camera:closed`; a viewer that resubscribed was handed the cached GOP from
before the outage and looked live on stale frames. Measured on the sandbox by
stopping the test source under a live viewer: frames stopped at 8 s, a
resubscribe at 16 s replayed ten old frames and the view read "live" twice
before the camera came back. An upstream with viewers that delivers nothing
for 10 s is now closed with `camera:closed` (`reason: "upstream_stalled"`), so
every client hears about it, and `fastStart` replays the GOP only while its
last fragment is under 3 s old. The same run after the change: stall noticed
by the client at 16 s, the upstream closed at 18 s, three refused attempts
read as offline at 28 s, the camera back at 30.0 s and the picture again at
33.1 s.

## A still picture per camera, for the grid

`GET /camera/{id}/snapshot` answers `{ image: "data:image/jpeg;base64,…",
capturedAt }`: the operator's latest frame from `SnapshotStore` when it is
under 10 s old, otherwise one frame grabbed from go2rtc's sub stream, the same
source the operator and the health monitor read. A disabled camera, or one that
sends no picture, is a 502 with its reason. Camera read permission covers it,
exactly as it covers the live view. The app's camera grid refreshes the
thumbnails of online cameras every 30 s while the grid is on screen and keeps
them in memory only. Measured on the sandbox: a 640×360 sub-stream frame is
18 KB of JPEG (24 KB as base64) and the call answered in about 1.4 s
end to end, including the token refresh and go2rtc's decode.

`Go2rtcFrameSource` and `IFrameSource` moved from `feature/operator` to
`shared/services/stream`: the operator, the monitor and camera-control read
them now, and the monitor no longer links the operator module to reach them.

## A revoked session's live view closes with it

The `/media` socket is authenticated once, at the upgrade, so a session
revoked from the sessions view (or ended for refresh-token reuse) used to keep
its open live views streaming: a stolen device kept watching. Each media
socket is now tagged at connect with the user and the session id the auth
verdict put in `JwtContext.sessionId` (`MediaSessionRegistry`,
`feature/media`), and a durable JetStream consumer of argus-auth's session
feed (`argus-camera-auth-session` on `argus.auth.v1.session`, stream
`ARGUS_AUTH_SESSION`, ordered, new messages only) closes exactly the sockets of
a `disconnect_session` change with 1008 `session_revoked`; the socket's close
handler then releases its stream subscriptions. Other sessions of the same
user, and other users, keep streaming. Every other change on the subject
(`sessionsChanged`) is acknowledged and ignored; a malformed
`disconnect_session` is terminated rather than redelivered. A socket opened
without a session id (a token minted before sessions existed) is not tracked;
its access token is refused at its next upgrade like any revoked one. Measured
on the sandbox with two sessions of a throwaway resident watching the same test
camera: the revoked session's socket closed 1 ms after argus-auth logged the
revocation, and the other session's stream carried on.

## A model catalog the drivers read (2026-10, CAMERA2)

`shared/services/camera-catalog` (`argus::camera-catalog`) is one constexpr
table of the cameras Argus knows: nineteen Tapo models and thirteen generic
brand profiles (TP-Link VIGI, Hikvision, Annke, Dahua, Amcrest, Imou, Reolink,
EZVIZ, Uniview, Axis, Foscam, ONVIF, RTSP). Each entry has its driver, a form
factor (`pan-tilt`, `outdoor-pan-tilt`, `cube`, `bullet`, `turret`, `dome`,
`doorbell`, which the app draws), indoor/outdoor, the nominal main and sub
resolution, the default RTSP/ONVIF ports, user and stream paths, and a feature
mask (`ptz`, `presets`, `autoTrack`, `microphone`, `speaker`, `siren`,
`privacy`, `led`, `dayNight`, `motion`, `sdCard`). `GET /camera/catalog` (camera
read) serves it, so the app pre-fills a form from the same table the drivers
use.

- **What the drivers implement decides the features.** A Tapo entry may only
  claim what `TapoDriver` can drive (PTZ, presets, privacy, LED, day/night,
  motion, auto-track, the alarm, the 8800 talk line). A generic profile claims
  nothing because `StreamOnlyDriver` drives nothing; whether it has a
  microphone shows in its stream (the probe and the app read the audio track).
  Doorbells claim only microphone and speaker: the control APIs on them were
  never measured.
- **Models were researched, not guessed**, from the vendor's model pages and
  the RTSP/ONVIF support notes (mains-powered Tapo cameras expose RTSP on 554
  `/stream1` `/stream2` and ONVIF on 2020; battery models do not and are left
  out; D225/D235 stream only hard-wired and always on, hence their note).
  Brand paths come from the vendors' published RTSP URL formats. The resolution
  is nominal; the probe measures the real one. Measured on the owner's C225:
  main 2688×1520, sub 1280×720, so the C225 entry says 2688×1520.
- **A camera remembers its entry** as `catalogId` in its `config` JSON, beside
  the stream paths (`camera_stream_paths::withConfig`, validated against the
  table on create and update, empty clears it). When it is absent the model
  text is matched token by token (`Tapo C225 (EU)` → `tapo-c225`, `C2250` →
  nothing). `TapoDriver::capabilities()` now answers per model: a C310 has no
  pan/tilt, so the app stops offering a PTZ pad on a bullet camera. An unknown
  Tapo model keeps every control, exactly as before.

## Testing a connection before saving: `POST /camera/probe`

The form calls the probe with what the user typed (`driver`, `ip`, `port`,
user/password, cloud user/password, paths, and `cameraId` on an edit so the
stored secrets fill blank password fields). Stored secrets are only ever sent
to the stored `ip` and `port`: a probe for an edited address that leaves blank
a password the camera has stored is a 422 (`StoredCredentialsElsewhere`), so
nobody can aim a camera's password at a host of their choosing. A blank field
whose stored value is empty too (the cloud password of an RTSP camera) is not
a refusal, and a probe of an edited address never borrows any stored secret. One probe runs per user at a
time (429 `ProbeBusy`). The probe runs off the loop and
answers `{ok, steps[], stream, device, catalogId}`: each step (`network`,
`main`, `sub`, and for Tapo `device` and `talk`) is `ok`, `failed`, `skipped`
or `warning` with a code the app words for the user: `unreachable` (nothing
answered: off, wrong address, another network), `refused` (the host answers
but not on that port), `auth_failed`, `not_found` (wrong path), `no_video`,
`protocol` (not RTSP), `no_credentials`, `cloud_password_missing` (video will
work, the speaker will not). The probe is a 200 with `ok: false`, not a
refusal: it is a test result. Only the input is refused (422 for a public
address, as on create).

- `infra/rtsp-probe` sends one `DESCRIBE`, answers a Basic or Digest
  challenge on the same connection (reconnecting once when a server closes
  after the 401), reads the SDP for the video and audio codecs and decodes the
  H.264 SPS from `sprop-parameter-sets` for the size, cropping included, and
  the frame rate when the VUI carries timing. `TapoConnection::open` now says
  `connection refused by` or `no answer from`, which is the line between
  `refused` and `unreachable`.
- The `device` step is a read-only Tapo login plus `getDeviceInfo`
  (`TapoDriver::probe`), giving model and firmware; a recognised model comes
  back as `catalogId`. The talk line is never opened by a probe.
- Measured: against the fake camera every step was `ok` with H264 1280×720 +
  PCMA and a 640×360 sub; a closed port is `refused` at once; an unrouted
  private address is `unreachable` after the 4 s timeout; the owner's C225
  (read-only) answered C225, firmware 1.3.1, 2688×1520 / 1280×720, PCMA.

## What the cameras screen reads: `GET /camera/overview`

One call for the grid, camera read: per enabled camera `lastSeenAt`,
`sampledAt`, `health` (the monitor's own state names, `unknown` before the
first sample), the sub-stream `width`/`height`, `viewers` (from
`StreamHub::viewersByCamera`), `stream` (`codec`, `profile`, `audio`,
`width`, `height`, `fps`, `kbps` of `camN-sub` from go2rtc's `/api/streams`,
the SDP parsed by the probe's own reader, the rate from the video receiver's
byte counter between two calls), `mainActive`, and `lastEvent`; plus `events`,
the 30 most recent detections. Detections go only to roles that read the
`event` table (owner, resident, guard); a guest gets the live data and empty
events.

- `shared/services/stream/camera-live-board` is the in-memory state behind
  it: the health monitor records every sample (reachable, health, frame size),
  the object-event sink records every event it stores, and at boot the sink
  seeds the board from the outbox's retained payloads, so a restart does not
  empty the list. Removing a camera forgets it. Nothing here is persisted
  twice: the outbox stays the record.
- The app's "recent detections" used to read the synced `event` table, which
  no service writes, so it was always empty; the overview is its source now.
- go2rtc's stream JSON carries the camera URL with its credentials; the
  overview reads only codec names, the SDP and byte counters out of it, and
  nothing else leaves the process.

## Talking through a camera: the /media socket carries the voice up

A live conversation rides the same `/media` socket the live view uses, so it
inherits its authentication, the session tagging of `MediaSessionRegistry` and
the revocation consumer: a revoked session's socket closes and its talk session
closes the camera's line with it (measured: 19 ms from the DELETE to the socket
close, and the fake talk channel received the vendor stop).

- **Frames.** `camera:talk:start {cameraId, sampleRate}` (8, 16, 24, 32, 44.1
  or 48 kHz) → `camera:talk:ready {cameraId, sampleRate, packetMs}`; the client
  then sends binary frames `[0xA8, 0x01, 0, 0] + PCM16LE mono`; the server
  sends `camera:talk:state {queuedMs, sentMs, droppedMs, underruns}` every
  second and `camera:talk:closed {cameraId, reason}` at the end (`stopped`,
  `idle` after 20 s without audio, `limit` after 15 min, `line_lost`,
  `socket_closed`, `replaced`, `shutdown`). Refusals are
  `camera:talk:start_error {status, error}`: 403 for a role that may not talk,
  404, 409 `CameraDisabled`, 409 `TalkLineBusy` (one voice per camera), 422
  `TalkUnavailable` (no speaker Argus can reach: a stream-only camera or a
  Tapo without the cloud password), 400 `InvalidTalkFormat`, 502 when the
  camera refuses the line.
- **Who may talk** is `role_access::kCameraActionAccess` in
  `packages/lib/auth/src/auth/role-access.hxx`: owner, residents and guards;
  never guests. The same row decides the HTTP `POST /camera/{id}/talk` (typed
  announcements), so a guard can now also send a typed announcement. The app
  mirrors it in `shared/libs/role-access.ts`.
- **The pipeline** (`feature/talk`): `TalkUplink` resamples with the shared
  stateful `AudioResampler` to 8 kHz and cuts 960-sample packets, keeping at
  most one second queued (the oldest audio is dropped, so a stalled line never
  turns into seconds of delay). `CameraTalkSession` owns one thread: it asks
  the driver for the line, opens it, paces one packet every 120 ms against a
  steady clock (re-anchoring after a stall), and closes the line on every exit
  path. `CameraTalkService` keys sessions by socket, refuses a second voice on
  the same camera, caps the process at four, joins only finished threads (never
  on the event loop) and is a `shutdown_signal` drain.
- **The driver owns the line.** `ICameraDriver::talkLine()` (default: no
  speaker) returns an `ICameraTalkLine`; `TapoDriver` hands out its persistent
  `TapoTalkClient` behind a `timed_mutex` the line holds for its whole life, so
  a guard announcement (`speak`) waits up to 3 s and then answers "someone is
  talking through this camera right now" instead of fighting the call for the
  8800 line. `TapoTalkClient::sendPacket` writes one MPEG-TS part (tables +
  one PES of A-law) without pacing; `TapoVoiceEncoder` keeps the speaker
  equaliser's state across packets (the TTS path re-created it per chunk) and
  uses a fixed 1.6× gain: the per-chunk peak normalisation of announcements
  would pump a microphone's background noise up to full scale.
- **Echo.** The camera ducks its own microphone while its speaker plays (the
  vendor app shows the same), which already keeps the user's voice from
  coming back; the app captures with the platform's echo canceller
  (`getUserMedia` echo cancellation, Android `VOICE_COMMUNICATION` + AEC, iOS
  voice chat) and plays the camera's microphone through that same voice path,
  so the canceller has the reference. That is as far as echo handling goes
  without a device-side AEC Argus could configure.
- **Proven without the owner's speaker**: unit tests drive the session with a
  recording fake line (pacing, idle, busy, refused, lost line) and drive a real
  `TapoDriver` → `TapoTalkClient` against the fake talk channel
  (`tests/support/fake-talk-channel.hxx`, now shared with the client's own
  suite); live, a Python fake of the 8800 channel received a 3 s 440 Hz tone
  from a WebSocket client as 24 parts exactly 120 ms apart, decoded back to
  440 Hz, `queuedMs` 58-118 ms, and the vendor stop at the end.

## What owners expect next (plan, 2026-10)

Done in this pass: catalog per model, connection test, live overview, calls and announcements
for owner/resident/guard, presets (go to, save), motion sensitivity, privacy/LED/night/motion
on cameras without PTZ, SD card state in the device panel, snapshot per camera.

Not done yet, in the order they pay off:

1. **Snapshot to file**: the still exists (`GET /camera/{id}/snapshot`); the app needs a save/
   share action per platform (Tauri dialog, Android/iOS share sheet).
2. **SD-card recordings and a timeline**: `searchDateWithVideo` / `searchVideoOfDay` are read-only
   pytapo calls Argus can add to the driver for a day list; playback is the 8800 media stream in
   `sdvod` mode with encrypted parts (pytapo's `Downloader`), a second media client, and it uses
   one of the camera's three connection slots while it runs.
3. **Argus-side recording** (`record_mode` is stored but nothing records): go2rtc can write MP4
   segments per camera; retention would follow `retention_days`, the evidence uploader's manifest
   shows the shape.
4. **Patrol and preset delete/rename**: `setCruise` (pytapo) and the preset delete the driver
   already sends; needs owner-only UI and a device test, which this pass was not allowed to do.
5. **Siren from the app**: the driver arms and sounds it, but only argus-guard may (rule 10);
   an owner button would need a decision and a consent setting.
6. **Live view audio on the native player**: Android's ExoPlayer may also play the FLAC track of
   the live view; the call path plays its own copy, so a native `muted` prop on `argus-camera`
   is the clean fix once a device is at hand to test it.
7. **ONVIF control** (PTZ, presets, events) for generic cameras: `StreamOnlyDriver` today.

## Two streams, two jobs: the view watches main, every analysis reads sub (2026-10, CAMERA3)

The owner decided the split: the **main** stream (`/stream1`, 2688×1520 on
the C225) is what a person watches and what recording will use; the **sub**
stream (`/stream2`, 1280×720) feeds everything Argus computes. One header
owns that decision, `shared/vocabulary/camera-stream-role.hxx`:
`CameraStream {Main, Sub}`, `CameraStreamRole {LiveView, Analysis,
Listening}` and the constexpr `camera_stream_role::streamFor(role)`.
`Go2rtcManager::sourceFor(cameraId, role)` turns a role into the go2rtc
source name, and no consumer spells `-sub` or `cam<N>` any more:

| Consumer | Role | Stream |
|---|---|---|
| `/media` viewer with no `quality` (the default view) | LiveView | main |
| detector, guard frames, health monitor (`Go2rtcFrameSource`) | Analysis | sub |
| `GET /camera/{id}/snapshot`, thumbnails, the guard's VLM crops (they come from `SnapshotStore`, filled by the operator's frames) | Analysis | sub |
| `GET /camera/overview` stream stats | Analysis | sub |
| the voice loop's microphone capture (`audio_capture`, guard listen) | Listening | sub |
| recording (not built yet: `record_mode` is stored, nothing records) | — | main, by the owner's decision; it adds `Recording → Main` to the table when it lands |

The analysis stream is the only one go2rtc preloads: the registrar marks
each source `preload` from `camera_stream_role::isWarm`, and go2rtc's config
writes the `preload:` block from that flag instead of matching a name suffix.
The main stream is pulled only while someone watches it, which keeps the
camera inside its three-connection budget (sub pull + main viewer + talk
line, measured in "The talk channel" above).

A viewer may still ask for `quality: "sub"` (`camera:subscribe`): the app
offers "Fluida" for slow or remote links and falls back to it by itself
after two stalls in a minute on main (the user's own choice is remembered
per device and per camera; the fallback is not, so the next visit tries
main again). An unknown `quality` value is read as the LiveView default.
The app does not know yet whether it is on mobile data or the tunnel (no
network-type module is linked); phones start on sub by default and every
larger screen on main.

## Capabilities travel on the camera row (2026-10, CAMERA3)

The `capabilities` column was `[]` for every camera: nothing wrote it, and the
app read `GET /camera/{id}/capabilities` from a remote cache that refreshed
only on focus, so a camera switched from RTSP to the Tapo driver kept showing
"video only", no PTZ pad and only "Escuchar" until the screen was reopened.

- `shared/services/camera-driver/camera-capabilities` (`camera_capabilities::of`)
  is the one function both drivers answer with: the Tapo entry's features from
  the catalog (talk only with the cloud password, `microphone` from the
  catalog), and every control off plus `streamOnly` for RTSP/ONVIF.
- Create and update store `camera_capabilities::listOf(...)` — the names of
  what the camera can do, e.g. `["ptz","presets","talk","microphone",...]` or
  `["streamOnly"]` — in the column. The update computes it from the row as it
  will be (driver, model, cloud password, catalog id) and writes it only when
  it changes, in the same transaction, so the audit diff carries it and the
  app's detail screen redraws live. No secret enters the list: `talk` says a
  cloud password exists, not what it is.
- At boot `CameraFeatureService::reconcileCapabilities` rewrites every live
  camera whose stored list differs (the rows created before this change), as
  an ordinary audited update.
- That reconcile froze `argus-camera` whenever `[objects]` was on (2026-10,
  CAMHANG). A coroutine resumes after `co_await execSqlCoro` on the sqlite
  connection's own loop thread, and `camera.db` has one connection. With
  nothing to rewrite, `startAfterSources` resumed there after `findLive()` and
  called `CameraOperatorService::start()`, which ran `rescan()`'s
  `execSqlSync` inline. That thread waited on a query only it could run, so
  every later query queued behind it: snapshot, capabilities and overview
  never answered, and the `camera-operator`, `camera-object-event` and
  `camera-change` drains never reported drained (gdb: the connection thread
  sat in `SqlBinder::exec` under `rescan` ← `start` ←
  `reconcileCapabilities` ← `Sqlite3Connection::execSqlInQueue`). Before
  the reconcile, the last await in that sequence was a `BlockingTask`, so
  `start()` happened to run on a pool worker. Two changes fix it.
  `startAfterSources` now returns to the app loop (`switchThreadCoro`) before
  it starts any unit. `start()` itself no longer queries: `supervise()` runs
  the first `rescan()` on the light lane and then waits `camera_rescan_ms` on
  `sleepCoro`, instead of holding a light-lane worker asleep. The rule: a sync
  database call never runs on a thread that a database callback may resume.
  `camera-operator-start-test` calls `start()` from inside a database
  callback and fails within 5 s if the thread blocks.

## What the Tapo encoder offers: `video` in the device status (2026-10, CAMERA3)

`GET /camera/{id}/status` adds `video {resolution, frameRate, encoding,
frameRates[], resolutions[]}` for Tapo cameras: the status batch now also asks
`getVideoCapability` (`video_capability.main`) and `getVideoQualities`
(`video.main`), pytapo's own read-only calls. Frame rates arrive as codes
(`"65551"` = 0x10000 + 15) and `tapo_video::frameRateOf` decodes both that and
a plain number. Measured on the owner's C225 (firmware 1.3.1, read-only):
main 2688×1520 H.264 at **15 fps**, offering **15, 20 and 25 fps** and
2688×1520 or 1920×1080. TP-Link's datasheet says "15/20/25/30 fps (Default
15 fps)"; this firmware offers no 30 on the main stream. The sub stream's
1280×720 averages 15 fps too (ffprobe on a capture: 115 frames in 7.6 s, with
irregular timestamps, which is why the app's meter averages the frame
durations instead of taking the most common gap).

**There is no frame-rate setting from Argus.** Measured on the C225
(firmware 1.3.1, with the owner's authorisation): `setVideoQualities` and
`setVideoConfig` answer `-40210` (METHOD_DO_NOT_EXIST), `setVideoQuality`
answers `-40106` (UNSUPPORTED_METHOD) for every payload shape tried. The
legacy top-level `{"method":"set","video":{"main":{"frame_rate":"65556"}}}`
answers `error_code 0` and `getVideoQualities` then reads back the new code,
but the stream kept emitting 15 fps (14.98-15.11 fps from RTP timestamps over
10 s, after fresh RTSP sessions, at 20 and at 30): the stored value is not
what the encoder runs. The camera was set back to `65551` on both streams.
The route no longer takes `frameRate`; the status keeps reporting the
encoder's offer (`video.frameRates`, now 15/20/25/30) for information, and
the app shows the measured rate read-only.

## Night vision speaks the camera's words, and refusals are refusals (2026-10, CAMERA3)

`setDayNightModeConfig` takes `inf_type` `on` (night, infrared), `off` (day)
or `auto`, as pytapo does; Argus sent its own `night`/`day`, which the camera
refused with `-40101` inside an otherwise successful batch, so the app showed
"night" while the picture stayed in colour. `tapo_day_night` maps the app's
words to the camera's and back (status now reads `night`/`day`). Measured:
`on` took the live picture's mean saturation from 24.9 to 0.0 within 8 s;
`auto` was restored. `TapoApi::call` now fails on any non-zero inner
`error_code` with the camera's number, so every setting, preset and motor
call reports a refusal instead of a silent success (the motor calls still
read `-64304` as a limit).

## Pan and tilt, measured on the owner's C225 (2026-10, CAMERA3)

Measured on the real camera with the owner's authorisation (fresh go2rtc
frames, each move timed until two consecutive frames differ by under 3 px,
shift by phase correlation and ORB feature matching against a reference;
about 16.7 px per degree on the 1280×720 sub stream):

- **`motorMove {x_coord, y_coord}` is a relative move in degrees**, not an
  absolute position: `+x` pans right, `-x` left, `+y` tilts up, `-y` down,
  exactly what Home Assistant's Tapo integration sends for its arrow buttons
  (`moveMotor(±degrees, 0)` / `(0, ±degrees)`, default 15). A 15° pan moved
  the picture 190-225 px, 10° tilt 136-139 px, ±30° pans ±250 px; two steps
  out and two back returned within 0-16 px (≤ 1°). A move settles in about
  1-2 s.
- **`relativeMove {direction}` is a continuous sweep** in the protocol's
  direction (`0` right, `90` up, `180` left, `270` down) that runs until the
  end of travel unless stopped: from the right limit, `180` alone crossed the
  whole ~350° range to the left limit in about 14 s, which is why the app's
  old arrows (one `relativeMove` per tap) jumped wildly and never came back to
  the same place. **`stopMove {"motor":{"stop":""}}` stops it**: 1.0 s of
  `180` then stop panned about 36°, 0.4 s of `90`/`270` tilted ±11°
  symmetrically.
- **The end of travel is `-64304` (`MOTOR_LOCKED_ROTOR`) inside an otherwise
  successful batch**: the outer answer was `error_code 0`, so the route used to
  report a refused move as a success. `tapo_motor::outcomeOf` reads the inner
  code: `0` → `{moved: true, limit: false}`, `-64304` → `{moved: false,
  limit: true}` (a 200, an answer rather than a refusal), anything else a
  refusal with the camera's code.
- **No home position over the local API**: pytapo exposes only
  `manualCalibrate` (a full sweep) and presets (`getPresetConfig` stores
  absolute `position_pan`/`position_tilt`); the camera had none. The app no
  longer offers "Centrar".

`PATCH /camera/{id}/ptz` therefore takes exactly one of `{x, y}` (a step,
both required, each within ±180), `{angle}` (start a continuous move) or
`{stop: true}`, and answers `{moved, limit}`. The app taps a 10° step and
holds for a continuous move that ends with `stop` on release.

## Camera audio follows the household's privacy choices (2026-10-04)

A camera microphone hears everyone near it, so camera audio cannot be granted
per person. It is on only while the Owner's household switch allows it and
every active person of the household has said yes to camera audio in their own
privacy choices (identity, `services/identity/CONTEXT.md`, "Privacy
choices"); a person who has not decided yet counts as a no. Anyone saying no
turns the microphones off for everyone, which the app explains where the
choice is made.

`CameraAudioPolicy` (`src/shared/services/privacy/`, read by the stream hub
and the actions feature) starts withheld, reads `ListPrivacy` from identity at
boot and every 15 s (light blocking lane), and keeps its last answer while
identity is unreachable. When it flips, `StreamHub::restartUpstreams()` stops
every upstream: viewers receive `camera:closed`, resubscribe, and the new
upstream asks go2rtc for `stream.mp4?src=<name>&video` (video only) instead
of `&mp4=flac`. Measured with a scratch go2rtc and an ffmpeg test source:
`&mp4=flac` carries `avc1` + `fLaC`, `&video` only `avc1`. A stopping upstream
is never handed to a new subscriber. The `Listen` RPC (the assistant
listening through a camera) refuses with `FAILED_PRECONDITION` while audio is
withheld. Talking through the camera's speaker is not capture and is not
gated. `camera-privacy-test` pins the rule and the listener, and
`camera-action-rpc-test` the refused `Listen`.

## Privacy masks: what the owner draws out is never analysed (2026-10, STRANGERS)

The owner asked for areas a camera must not look at for Argus: the street,
the pavement, a neighbour's window. A zone of type `privacy` (fourth value of
`ZoneType`, `packages/contracts/camera`, and of the `zone.zone_type` CHECK) is
a polygon drawn in the same zone editor as the others. The operator fills it
black on every analysis frame (`feature/operator/privacy-mask`, before the
motion gate and the detector), so nothing inside it is detected, tracked,
identified or cropped. The masked frame is also what goes into
`SnapshotStore` (re-encoded once per frame only when a mask exists), and
therefore into the grid stills, the guard's VLM crops, the identity crops and
the detection evidence the uploader stores. The existing `exclude` zone keeps
its meaning (detections whose centre falls inside are dropped, pixels kept).

What a mask does not cover, by design: the live view the owner watches (the
main stream is passed through from go2rtc, unmodified — masking it would mean
transcoding every viewer) and the health monitor, which only computes
brightness/sharpness/scene statistics and stores no picture.

A database created before this change has the old CHECK. SQLite cannot alter
a CHECK, so `ZoneRepository::acceptPrivacyZones()` rebuilds `zone` once at
boot when its stored DDL lacks `'privacy'`: copy into `zone_rebuild` with the
same ids, drop, rename, recreate the three indices, in one transaction with
foreign keys deferred. It is idempotent and no other table references `zone`.

`identity.auto_enroll`, `identity.capture_clear_faces` and
`identity.enroll_cooldown_ms` are gone with the enrollment path they drove:
the matcher calls `IdentityClient::identifyForCamera` with the camera id and
the time of the sighting, and identity decides whether a face is a household
member, a known visitor or a new one (`services/identity/CONTEXT.md`, "Recurring
visitors").

## The live view prefers WebRTC, and the /media socket is its fallback (2026-10-05, CAMRTC)

The owner asked that the live view always try WebRTC first and fall back to
the `/media` WebSocket only when WebRTC cannot be established or drops for
good. The media path is go2rtc's own WebRTC server; argus-camera only carries
the signalling, so the decision about who may watch stays where every other
camera route decides it.

- **Signalling: `POST /camera/{id}/webrtc`** (`feature/webrtc`), full chain
  `DeviceFilter → ValidJsonFilter → JwtFilter → RoleFilter`. The body is
  `{sdp, quality?}` (`quality` is `main` or `sub`, default the LiveView
  role's `main`); the answer is `{type: "answer", sdp, quality, audio}`. It is
  one non-trickle exchange: go2rtc's `POST /api/webrtc?src=` (WHEP,
  `application/sdp`) gathers its own candidates before answering, and learns
  the viewer's address from its first STUN check, so the client never sends
  candidates. The exchange goes through `Go2rtcWebRtcGateway`
  (`feature/webrtc/infra`), the only code that speaks to go2rtc's WebRTC API,
  on its loopback address; the API itself is never published. Who may call
  it is `CameraAction::Watch` in `kCameraActionAccess` (every role, because
  every role reads `camera` and may already open `/media`): without the row
  a POST would read as `camera` + Create and refuse Guard and Guest.
- **The offer is rewritten before go2rtc sees it** (`webrtc_sdp::prepareOffer`).
  Every audio/video section is forced to `recvonly`, because go2rtc treats an
  offer that sends video as a publisher and would add it as a *producer* of
  `cam<id>`, which every other viewer and the operator read. An offer with no
  video section, more than eight sections or a line that is not SDP is a 400
  (`InvalidWebRtcOffer`). While `CameraAudioPolicy` withholds audio, the audio
  section becomes `inactive`: go2rtc creates no transceiver for it and the
  answer carries no audio, the same rule the fMP4 upstream follows.
- **The answer gives the camera's audio its own stream id**
  (`webrtc_sdp::separateAudio`). go2rtc puts both tracks in stream `go2rtc`,
  so the browser lip-syncs them, and the C225's audio timing made Chrome hold
  the picture back: measured on Patio (main, 2688×1520), the video jitter
  buffer grew from ~150 ms to 520–630 ms within 30 s with audio in the same
  stream, ~100–150 ms with the video alone, and 84–116 ms with the audio
  renamed to `go2rtc-audio` while still playing. The fMP4 path never synced
  the two either (its audio is scheduled on its own clock).
- **go2rtc's config** (`Go2rtcManager::renderConfig`): `webrtc.listen` from
  `[streaming] webrtc_listen` (default `:8555`, tcp and udp; empty or not
  `host:port` turns WebRTC off and the route answers 503
  `webrtc_unavailable`), `ice_servers: []` so nothing asks a public STUN
  server and the answer is not held for a server-reflexive candidate, and
  `candidates` from `[streaming] webrtc_candidates` (comma-separated
  `host[:port]` or `stun:port`, each checked before it reaches the YAML).
  Natively go2rtc offers a host candidate per interface and drops Docker
  bridge addresses itself; in the deploy container that leaves nothing a
  phone can reach, so `provision-host.sh` writes the host's LAN address
  (`<lan>:8555`) into `config.camera.toml` beside `mdns.address`, and the
  compose file publishes `${CAMERA_WEBRTC_PORT:-8555}` tcp+udp.
- **Codecs.** H.264 passes through untouched (`profile-level-id` as the
  camera sends it). The C225 sends PCMA 8 kHz on both streams (go2rtc
  `/api/streams`), which browsers and libwebrtc decode natively, so it is
  passed through too. For a camera whose audio is AAC (`MPEG4-GENERIC`),
  WebRTC has no decoder; the config therefore also declares, for every
  source, an on-demand `cam<id>[-sub]-opus: ffmpeg:<source>#video=copy#audio=opus`
  variant, and the service picks it only when the warm sub producer reports
  AAC and audio is allowed. It costs nothing until someone watches it.
  Measured with a scratch go2rtc reading the sandbox restream: passthrough
  costs go2rtc ~0.8–1.0% of one core per viewer; the Opus variant adds an
  ffmpeg process at ~1.0% of a core and 52 MB RSS, and its first frame came at
  2.5 s instead of 1.2–1.9 s.
- **Limits.** WebRTC viewers count against `streaming.max_viewers_per_camera`
  and `streaming.max_total_viewers` together with the hub's: the service
  counts go2rtc's `webrtc/*` consumers of the camera's four source names
  (`CameraWebRtcService::tally`) and refuses with the same 429
  `too_many_viewers[_for_camera]`. A `main` viewer adds the same single
  camera pull a `/media` viewer adds, so the camera's three-connection budget
  is unchanged.
- **A revoked session's view still closes with it.** go2rtc has no API to
  close one consumer, so each exchange is tagged: the User-Agent sent to
  go2rtc is `argus-camera/webrtc <32 hex of SHA-256(userId:sessionId)>`
  (`webrtc_viewer::tagOf`, no session id on go2rtc's API). On a
  `disconnect_session` event the media consumer's new `onRevoked` hook asks
  `WebRtcSessionCloser` to look for a consumer with that tag and, only if one
  exists, restart go2rtc (`Go2rtcManager::restart`, off the event loop). The
  same closer runs when the household's audio choice flips, if any WebRTC
  viewer is open, so a stream that carried audio cannot outlive a "no".
  Every other viewer reconnects by itself (the app retries WebRTC once, the
  hub's viewers resubscribe on `camera:closed`). Measured live: a synthetic
  revocation for a tagged consumer restarted go2rtc within 0.3 s and the
  browser's peer went `disconnected` 5 s later.
- **Measured on the sandbox** (headless Chromium, the app's own live service
  bundled with a fetch shim, Patio C225): first picture over WebRTC 0.28–1.1 s
  after the offer (go2rtc starts a WebRTC consumer on the next keyframe),
  2688×1520 at 15 fps, PCMA audio; signalling round trip 60–200 ms. The fMP4
  path's first picture stays ~0.1 s on a warm camera (it replays the GOP);
  its receive side decodes on arrival with no jitter buffer, where WebRTC
  keeps the ~100 ms above in exchange for loss recovery (NACK/PLI) and
  congestion control.
- **Remote viewers (tunnel): no TURN.** The answer carries only LAN host
  candidates, so a viewer outside the LAN cannot complete ICE; the app gives
  WebRTC 5 s to paint a frame, then plays `/media` through the tunnel as
  before, and backs off before trying WebRTC again (30 s doubling to 5 min).
  Making WebRTC work remotely needs a TURN relay (or the tunnel carrying
  UDP); that is not built.

## Hardening after the cloud audit (2026-10-05)

The fixes for `docs/history/reports/cloud-audit-2026-10-05.md` that touch this
service, and why each looks the way it does.

### Credentials

- **A PATCH that moves a camera forgets its secrets.** Changing `ip` without
  sending a new `password`/`cloudPassword` clears both, and the Tapo
  certificate pin with them: the new host is a new device until the owner
  types the secrets again. Before, go2rtc and the driver sent the stored
  password to whatever address a Resident wrote (#3).
- **Passwords are encrypted at rest.** `camera.password` and
  `camera.cloud_password` are stored as `enc:v1:` + base64(nonce ‖ ciphertext ‖
  tag), AES-256-GCM through OpenSSL EVP, with the column name as associated data
  so a value cannot be moved to the other column
  (`shared/services/secret-box`). The 32-byte key is the instance's own file,
  `[camera] secret_key`, by default `camera-secret.key` beside `camera.db`; it
  is created 0600 on first boot and the service refuses to start if it cannot
  read or create it. Every boot seals any row still in plain text (rows written
  before this change), and every write seals; a read accepts both forms. The
  key must be backed up with the database and kept out of any backup that
  leaves the house: a copy of `camera.db` alone no longer reveals the TP-Link
  account. The key's bytes are never logged; only its path is, once, when it
  is created.
- **No credential is lost on upgrade or by a misplaced key.** The boot pass
  seals a plaintext row only when both of its values seal, and writes it with
  a compare-and-set on the old values, so a failed or concurrent seal leaves
  the row as it was (still readable, sealed on the next boot). A write whose
  password cannot be sealed is refused (500 `SecretNotSealed`) instead of
  storing an empty or plaintext value. Before anything else uses the table,
  `CameraRepository::unreadableSecrets()` opens every sealed value with the
  loaded key; if any does not open, argus-camera refuses to start, and a key
  file it created on that same boot is removed again. That is the case of a
  wrong `[camera] secret_key`, an unmounted data directory or a restored
  database without its key: the stored ciphertext is never overwritten and
  never read back as empty, so restoring the key file brings every password
  back. Only an owner who has really lost the key clears the sealed values
  by hand (`UPDATE camera SET password = '', cloud_password = '' WHERE
  password LIKE 'enc:v1:%' OR cloud_password LIKE 'enc:v1:%'` with the
  service stopped, then types the passwords again in the app).
- **Tapo TLS is pinned on first use.** The cameras present self-signed
  certificates, often with TLS 1.0 and weak ciphers, so verification against a
  CA is impossible and the legacy settings stay. Instead the SHA-256 of the
  leaf certificate is learned at the first successful login and stored in
  `camera.tls_fingerprint`; a later handshake with another certificate fails
  (`TLS certificate changed since it was pinned`). Once a camera has answered
  over secure passthrough (`camera.tapo_secure`), the legacy `md5(password)`
  login is never tried again, so a man in the middle cannot downgrade it. The
  two columns are added by `CameraRepository::acceptTapoTrust` on old
  databases and never leave the process (`toJson` omits them). The talk port
  (8800) is plain HTTP with Digest and is not pinned. Only a camera's own
  driver records what it learned, and only while the row still has the
  address it was learned at and no other pin (`SAVE_TAPO_TRUST` is guarded by
  `ip` and by `tls_fingerprint` being empty or equal): a driver that was
  connected before a PATCH moved the camera cannot write the old pin back,
  and nothing replaces a pin once set. The connection probe enforces a stored
  pin when it tests the stored address but never records one, so a probe
  aimed at another host cannot plant that host's certificate in a camera's
  row.
- The secure-passthrough transport no longer logs `device_confirm`, the
  derived hashes or the nonces (they allowed an offline brute force of the
  password from the logs), nor the raw outer response.
- **go2rtc never sees a literal password in its config.** The generated
  `go2rtc.yaml` names `${ARGUS_SRC_<SOURCE>}` where the user info was, and the
  values travel in go2rtc's environment (`execve`), which go2rtc substitutes
  at load time. Measured with v1.9.14: `/api/streams` shows
  `rtsp://***@host/...`, `/api/config` shows the placeholder, and a refused
  connection logs no credential. `go2rtc.log` stays 0600.

### Addresses and probes

- Loopback and link-local addresses are refused for cameras (DTOs and go2rtc's
  source guard share `network_address::isCameraAddress`) (#58).
- One `/camera/probe` per user at a time (`ProbeSlots`), because a probe holds
  a light-lane worker for up to ~12 s and its `refused`/`unreachable` answers
  are a port scanner (#58).

### Live view

- **The hub never joins a reader under `hubMutex_` or on the event loop.** A
  stopping or dead upstream is retired out of the map under the lock; a
  finished reader is joined on the light lane, a stopping one is joined when it
  has finished, and only `shutdown()` (after `run()` returns) joins directly,
  outside the lock. `upstream_http::open` connects non-blocking and checks the
  upstream's `stopping` flag every 100 ms, so `restartUpstreams()` no longer
  leaves a reader in a 10 s connect (#12).
- **`camera:ack` is the socket's own.** The hub releases credit only to the
  sink that owns the `subId`, and a sink releases at most what it has in
  flight. A sink also closes its socket (`slow_consumer`) if more than
  `max(16 × window, 8 MiB)` sits unacknowledged. Drogon does not expose a
  connection's output buffer, so a client that acknowledges bytes it never
  reads cannot be detected from here; the window and the camera's bitrate are
  the bound in that case (#61).
- `camera:subscribe` refuses a disabled camera (409) (#102).
- **Owner and Resident keep a seat.** Guests and guards may fill the per-camera
  and total viewer limits only up to one below the limit (when the limit is
  above one), both on `/media` and on WebRTC (#60).
- **WebRTC seats are reserved atomically.** `WebRtcAdmission` counts go2rtc's
  WebRTC consumers and the hub's viewers from a snapshot taken before the
  exchange, plus every seat still reserved or released after that snapshot,
  under one lock, so N concurrent offers cannot all pass the check. A user may
  hold `streaming.max_webrtc_per_user` (4) WebRTC views; tags are mapped back
  to users when their exchange succeeds (#60).
- **SDP.** Every `a=candidate`/`a=end-of-candidates` line is removed from the
  offer (go2rtc learns the viewer from its STUN checks), and the answer keeps
  only candidates on a literal, non-loopback, non-link-local address — or only
  the configured `webrtc_candidates` hosts when they are configured without
  `stun:`. mDNS names are dropped (#64).
- **go2rtc restarts are coalesced.** A revocation or an audio flip asks for a
  restart (`Go2rtcManager::requestRestart`); requests within 750 ms become one
  restart, which the supervisor runs. A request that arrives after a restart
  began gets a restart of its own, so a consumer created before the request
  never survives it. If go2rtc does not list its consumers after three tries a
  second apart, the closer restarts it anyway (fail closed). go2rtc has no API
  to close one consumer (verified against v1.9.14: no DELETE per consumer on
  `/api/webrtc` or `/api/streams`), so a restart remains the only lever until a
  WHEP proxy exists (#57).
- **Supervision off the main loop.** `Go2rtcManager::configure()` reads the
  config on the loop; `start()` (write, spawn, `waitReady`) runs on the light
  lane in `startAfterSources`. The supervisor is a `std::jthread` with a stop
  token, sleeps its backoff without `mutex_`, and keeps serving requested
  restarts after the crash-restart budget is spent (#62).
- The fMP4 reader parses by offset (no `substr`/`erase` per box), caps the
  init segment at 4 MiB, and on an invalid box size marks itself corrupt; the
  upstream then closes with `upstream_corrupt` and viewers resubscribe. The
  upstream's status line must be `HTTP/1.x 200` (#100). Each chunk is framed in
  a per-thread buffer; the fragment itself is one shared buffer for every
  subscriber.
- **Media sockets are revalidated.** `MediaAccessCheck` re-verifies each
  `/media` socket's access token (signature and expiry locally, then the
  session verdict at argus-auth) every 60 s; an expired or revoked session is
  closed with `session_expired`, a changed role with `role_changed`, which also
  ends a talk session. An unreachable argus-auth keeps the socket (#102).
- **A live view renews its access in band.** Closing at the access token's
  expiry would cut every live view each token lifetime, so the app sends
  `{"type":"camera:auth","payload":{"token":"<access token>"}}` on the open
  `/media` socket after each refresh. The token is checked like the sweep
  checks the stored one (signature and expiry, then argus-auth's session
  verdict with the socket's own device hash and origin) and must name the
  socket's user; it then replaces the stored token and the socket answers
  `{"type":"camera:auth:ok","payload":{"role":"<role>"}}`. A token that fails,
  belongs to another user or carries another role closes the socket like the
  sweep would (1008 `session_expired` or `role_changed`). One renewal per
  socket every 10 s (`MediaAccessCheck::kMinRenewInterval`, it costs a verdict
  RPC); a faster one gets `camera:auth_error` 429 and changes nothing, a frame
  without a token 400, a socket the check does not track 409. Renewal never
  outlives a revocation: the sweep keeps validating the newest token every 60
  s, and a sweep that judged a token replaced meanwhile does not act on it. A
  client that never renews keeps the old behaviour and is closed once its
  token expires. WebRTC views have no socket and no token after the
  exchange: they end on a session revocation (the auth session feed restarts
  go2rtc through `WebRtcSessionCloser`), not on token expiry, so they need no
  renewal.

### Workers and lanes

- The operator waits between frames on `sleepCoro` and the health monitor
  between rescans too, so neither holds a light-lane worker asleep (#11, the
  same rule as CAMHANG). The operator's decode, privacy mask, motion gate and
  detector run on `BlockingLane::Heavy` (`analyse`), the tracking, matcher and
  outbox on the light lane (`interpret`) (#63). The health monitor decodes and
  measures on the heavy lane (inline only when no event loop runs, as in its
  unit tests) (#101).
- Frame grabs ask go2rtc for `frame.jpeg?src=…&cache=<ms>` with the
  operator's own interval (1 s for the monitor): a frame at most one interval
  old is what the operator samples anyway, and the operator, monitor and
  snapshot route coinciding on a camera then share one ffmpeg decode. The HTTP
  clients to go2rtc are kept alive per event loop and per lane (a camera's
  frames, a source's WebRTC exchanges, the stream list), so one slow exchange
  never queues another camera's frames behind it (`go2rtc_http::client`).
- `ZoneProvider` queries outside its lock, retries a failed read every 5 s at
  most instead of on every frame, and keeps the last zones it read (#101).
- The outbox `COUNT(*)` that enforces `operator.outbox_max_pending` runs every
  32 enqueues, or on every one once the pending count is within 64 of the cap.
- Zones carry `ZoneType` from the moment they are read (config strings are
  parsed once; an unknown configured kind is dropped), and events carry
  `EventSeverity`; strings exist only at the JSON boundary.

### Retention (Peru, Ley 29733 and the video surveillance directive)

The directive of the Peruvian data protection authority on video surveillance
(Directiva 01-2020-JUS/DGTAIPD) keeps footage 30 days by default, 60 at most,
and up to 120 days when it documents an incident. Argus stores no continuous
recording; what it keeps is detection evidence in object storage and the
voice it transcribed for the guard.

- `retentionDays` is 0-60; `retentionIncident: true` (kept in the camera's
  `config` JSON) allows up to 120. Clearing the flag brings a longer value back
  to 60 in the same audited update.
- Evidence expires after the camera's `retentionDays` (7 days when unset, 0
  stores none), capped at 60 days, or 120 with the incident flag, and nothing
  survives 120 days whatever its manifest says. The sweep judges each object
  by its camera's current retention as well as by the `expires_at` written at
  upload (`evidence_query::EXPIRED_EVIDENCE` joins `camera`), so lowering
  `retentionDays` or clearing the incident flag shortens what is already
  stored, and a row from before the caps (up to 3650 days) is held to 60. A
  camera's `config` that is not valid JSON counts as no incident. The sweep runs every 6 hours,
  walks `camera_evidence` by `id`, so one object that cannot be removed no
  longer repeats the same 200 rows, counts its failures, and retries them on
  the next sweep. Without object storage it counts what is waiting and says so.
- `action_command` rows (idempotency records whose `response` holds the
  visitor transcripts of `Listen`) are deleted 7 days after they settle, so a
  transcript never outlives the camera's retention. `idx_action_command_updated`
  serves that purge.
- `Listen` re-checks the household's audio consent right before capture, on
  the lane, not only when the call arrives (#102).

### Sync

- **Guards and guests do not receive a camera's address or account.**
  `PullTable` projects camera rows by the caller's role (`x-argus-role`): for
  anything but Owner and Resident, `ip`, `username`, `cloudUsername` and
  `config` are empty and `port` is 0. The wire is unchanged (same fields). The
  role test is `role_access::readsCameraConnection` and the reduction is
  `camera_projection::reduceRow` (`contracts/camera`'s
  `camera-row-projection.hxx`), the field list argus-sync's fan-out reduces by
  too, so the two cannot drift. The
  live `Add`/`Log` frames for `camera` are fanned out by argus-sync to the
  module room as they are; projecting those per role belongs to argus-sync's
  fan-out (#76).
- **Tombstones re-read their boundary second.** A `findDeleted` page after
  `(T, id)` also returns the rows deleted in second `T` with `id ≤ startId`,
  deduplicated by id, for `camera`, `zone` and `camera_stream`; a row deleted
  in the same second after the client paged past it is no longer lost. The
  wire shape is unchanged; the client sees a few tombstones twice, which is
  idempotent (#52).

### Shutdown

- The gRPC listener is a drain: on SIGTERM it calls `Shutdown` with a 2 s
  deadline on a thread of its own and reports drained when it returns (N1).
  The lease sweeper and command purge (`camera-actions`), evidence uploads and
  sweeps (`camera-evidence`), the media access check and the session feed's
  subscription (a `std::jthread` that retries every 5 s off the loop) are
  drains too.
- `DeviceFilter::requireFingerprintSecret()` runs at startup: a camera without
  `[device] fingerprint_secret` refuses to start instead of hashing devices
  with the JWT secret.
- `scripts/provision.sh` installs go2rtc v1.9.14 only: the asset for the host
  is downloaded to `.part`, checked against its pinned SHA-256 and moved into
  place (macOS assets are zips and are unpacked first).
