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
  the `/media` socket (relayed from the gateway's `/camera-stream`):
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

- The canonical camera build is its standalone graph. From the repository
  root use `scripts/build-all.sh dev --only argus-camera`; direct builds run
  Conan, the matching preset and CTest inside `services/argus-camera`.

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
  reconnection. The gateway subscribes ephemerally over core NATS, so events
  published while it is down are not replayed after a restart; the guard's
  durable consumer is the replay path.
- **Per-track person events**: each eligible person track produces its own
  event with rule, severity, identity, zone, dwell, crop and cooldown bound to
  that track; companions travel only as context and never decide. The
  cooldown advances only after the outbox transaction returns Recorded, so a
  failed enqueue keeps the pending event for the next window. `/health`
  `objectEvents` hydrates from SQLite at boot and counts real overflow drops.
- **Siren lease sweeper**: an expired lease is disarmed and deleted only
  after the driver confirms `alarm=false`. If the camera row or its driver
  is unreachable the lease is kept and retried with an error log; a failed
  disarm is never treated as "off".
- **Budget split (Ruling AD)**: camera side = aggregation window +
  per camera+class cooldown + `max_fps_inference`; gateway side =
  notification budget/silent hours/digest (see argus-gateway CONTEXT.md).
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
the fleet changed. The unit suites live in `tests/unit/` and register in the
camera project's standalone CTest graph.

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
  against the first reference frame: `dark`, `bright`, `blurred`, `moved` or
  `unreachable`. Transitions publish `argus.camera.v1.health`; `GET /health`
  never touches this path.
- `SnapshotStore` keeps the latest full frame and the latest identity person
  crop per camera (bounded, TTL-free, overwritten). `CameraActionService`
  exposes `GetPersonCrop` so argus-guard can assess without touching frames
  directly.
- `CameraActionService` (fleet secret + `[actions].enabled`) is the only
  audible path: `Announce` runs TTS through the existing control feature,
  `Alarm` plays a procedural siren tone over the talk channel, and `SetSiren`
  arms/disarms the vendor alarm. The operator never calls these.

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
