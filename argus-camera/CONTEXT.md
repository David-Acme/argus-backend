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
- **What stays away**: no camera-control routes (ptz/preset/settings/status/
  presets/capabilities/talk stay on the legacy — Ruling X), no voice path,
  no alarm-triggering code, no AI symbols.

## Build wiring (decisions)

- The canonical camera-service builds are the ROOT presets
  (`cmake --build --preset camera` / `--preset camera-prod`): they reuse the
  root Conan cache. The standalone `argus-camera/` build directory goes stale
  on new `conanfile.txt` requires until `conan install` is re-run there.
