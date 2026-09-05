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
  `database/camera-schema.sql` (same pattern as `identity-schema.sql`) and
  is applied at boot through `DbService::runScriptFile` — abort on failure.
  `argus.db` is never touched here.
- **Wiring**: Drogon boot with the camera domain only — `[server]`
  internal plain listener (loopback 7026 default), `DbService` default
  client on `[camera] db` (default `database/camera.db`), `[drogon.app]`
  mirror, CORS/exception/404/405 plumbing identical to the gateway so
  envelopes are byte-shape-identical. A config-gated named identity client
  placeholder (`[identity] db`, read-only) reuses the legacy
  `DbService::setIdentityClient` slot for the F2-2 caller validation; absent
  key boots identity-free. No AI service registry is compiled or loaded
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
- **Identity reads**: `[identity] db` opens mode=ro as the named identity
  client (`DbService::setIdentityClient` slot); absent key boots
  identity-free.
- **OPTIONS divergence attribution (F2-4 review)**: unlike the legacy
  (`src/config/application.cc` pre-routing advice answers every OPTIONS with
  `AppConfig::handleOptions` before routing), argus-camera registers only the
  post-handling CORS advice, so `OPTIONS /camera` 404s here where the legacy
  answers 200. The gateway proxy forwards OPTIONS fine (F1-5 scoped its own
  pre-routing advice to gateway-native paths); the divergence lives in this
  service. App-safe as shipped: the native client sends no preflight.
- **What stays away**: no camera-control routes (ptz/preset/settings/status/
  presets/capabilities/talk stay on the legacy — Ruling X), no voice path,
  no alarm-triggering code, no AI symbols.

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
  `vulkanDiscrete`.
- **Frames come from go2rtc** (`/api/frame.jpeg?src=cam<id>` via
  Go2rtcFrameSource, resolved per grab because Go2rtcManager resolves its
  address only at init) — never from a device driver (Ruling AB). The Tapo
  `ICameraDriver` detection walker is deferred to Fase 4.
- **Identity is deferred behind `IKnownPersonMatcher`** (default
  `NoKnownPersonMatcher`): rules 3-8 keep their own severity; the real
  matcher arrives in Fase 4 with the identity domain.
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
- The labs (`argus-camera/labs/`) build on demand via the camera presets
  (`--target argus-object-bench` / `argus-camera-probe`), EXCLUDE_FROM_ALL.
- Model artifacts (`models/objects/`) come from `scripts/setup.sh camera`:
  yolo26n.pt sha256-pinned download + local raw e2e NCNN export; without the
  artifacts the detector boots disabled — never a fake success.
