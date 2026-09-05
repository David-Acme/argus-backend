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

## What it does NOT do yet

- **No behavior switch (Ruling W)**: the gateway keeps serving `/camera*`
  and `/zone` from the legacy, which keeps writing `argus.db`. Reading
  camera tables from `camera.db` now would serve a frozen snapshot, so the
  service registers no camera routes in F2-1.
- **No stream ownership**: `[tapo]`/`[streaming]` keys exist so the F2-2
  drivers and go2rtc lifecycle read real values, but the legacy still owns
  every running stream and the Tapo talk channel.
- The migrate tool (`tools/migrate-camera`, Ruling V) copies camera rows
  from `argus.db` into `camera.db` and then goes quiet: a schema-current
  `camera.db` makes reruns a verified no-op, because after the F2-2 cutover
  `camera.db` is live data and the frozen `argus.db` copy must never be
  resurrected over it. Nothing is deleted from `argus.db`.
