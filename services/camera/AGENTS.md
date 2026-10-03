# argus-camera — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to camera-service code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **Camera domain only** — this service runs camera/zone data, media and its
   local YOLO26n object detector. It must not compile or load unrelated AI
   engines such as face, LLM, VLM, STT or VAD.
2. **Parameter structs for 3+ params** — any function with 3+ parameters
   must take a struct (designated initializers, every member listed).
3. **Dependency injection** — services/filters hold dependencies as private
   members with `_` suffix; controllers hold instance members, never static
   methods.
4. **Smart pointers** — no raw owning pointers; `std::unique_ptr` with
   custom deleters for C handles; raw pointers only for non-owning access.
5. **File naming** — `.hxx` headers, `.cc` sources, hyphenated
   `*-test.cc` tests. No `.h`/`.cpp`.
6. **100% English** — code, identifiers, docs, commits.
7. **No comments** — none in code, of any kind (root rule 20); the "why"
   goes to CONTEXT.md.
8. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`,
   `LOG_FATAL`); no spdlog.
9. **Health safety** — `GET /health` must never fail or block on NATS, the
   cameras or any downstream service; degraded dependencies degrade logs,
   not health.
10. **Audible actions only from argus-guard** — the operator stays read-only
    toward hardware. Announcing, the alarm tone and arming the siren are
    capability-credential gated `argus.camera.v1.CameraActionService` calls
    made by argus-guard behind `[actions].enabled`; never from the operator
    loop or EventIntelligence. Tests may stub the action RPC service and its
    camera driver, but must never drive real audible hardware (no live siren,
    alarm tone or speaker playback in any build).
11. **No argus.db migrations** — this service owns `camera.db` only
    (`services/camera/database/schema.sql`); it never writes or migrates
    `argus.db`.
12. **Frozen contracts** — HTTP paths, the `{status, info, errors}`
    envelope, `SyncOperation` 0-7, `SYNC_LIMIT=200` and `TableName` 0-23
    never change here; the mobile app must keep working unmodified.
13. **No std::future** — plain `std::thread` + join when parallelism is
    needed.
14. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's
    own code; third-party includes are SYSTEM.

## Layout

```
argus-camera/
  CMakeLists.txt        add_subdirectory-compatible AND standalone buildable
  src/app/main.cc       config load, camera.db wiring, app run
  src/app/rpc/          the gRPC listener — bind, register, shutdown; module
                        argus::camera-rpc-server
  src/config/           camera-domain config resolution — argus::camera-config
                        (camera.db, listener, health thresholds, guard caller
                        secret and the objects/operator/identity resolvers)
  src/feature/actions/  argus.camera.v1 CameraActionService (guard-gated),
                        audio capture, STT transcriber and their
                        action-command repository
  src/feature/camera/   /camera* HTTP surface
  src/feature/camera-control/
                        /camera PTZ, preset, settings and talk surface
  src/feature/health/   grpc.health.v1 service
  src/feature/media/    the camera media WebSocket and its service
  src/feature/monitor/  camera health monitor and its NATS sink
  src/feature/objects/  YOLO26n ncnn object detector
  src/feature/operator/ the operator loop, EventIntelligence, zone provider
                        and evidence upload, with the object-event outbox
  src/feature/settings/ argus::camera-settings — the owner-editable catalog,
                        served by argus.settings.v1 on the gRPC listener when
                        `[grpc] caller_settings` is set
  src/feature/sync/     argus.camera.v1 SyncService owner and the
                        camera_stream repository and schema
  src/feature/zone/     /zone* HTTP surface
  src/shared/           the camera, zone and change-outbox repositories and
                        schemas plus the stream, camera-driver, change-sink
                        and in-flight utils modules 2+ features read and the
                        vocabulary the config module and a feature share
                        (health thresholds, operator zones; tapo is
                        camera-driver's own protocol stack, event-stream's one
                        reader is operator, the change sink's readers are
                        composition and the outbox suites, and geometry is
                        header-only)
  config.toml.example   camera, streaming, YOLO object and operator settings;
                        [grpc] caller_guard, caller_sync, caller_llm and
                        caller_settings
  CONTEXT.md            purpose, ownership, wiring decisions
```

Every feature and the `app/rpc/` listener is a rule-25 module: the folder
holds its own `CMakeLists.txt` declaring its sources and dependencies once,
the root file discovers them (`feature/*/CMakeLists.txt`) and `argus-camera`
links `argus::camera-{config,change-sink,actions,feature,camera-control,zone,
media,health,sync,rpc-server,monitor,operator,settings}` by name (`camera-objects`
arrives through `camera-operator`, its only reader, and is deliberately not
repeated). The
executable links the feature modules
plainly, not whole-archive: every camera controller declares
`HttpController<…, false>` and `src/app/main.cc` registers it by hand, so no
route depends on a static initializer reaching the binary. The three gRPC
services stay in their features (`argus.camera.v1` in `feature/sync`,
`argus.camera.v1.CameraActionService` in `feature/actions`, `grpc.health.v1`
in `feature/health`); `src/app/rpc/` owns only the listener they register
with. `argus.settings.v1.Settings` is the contract's own
`SettingsRpcService` over the `feature/settings` catalog, registered on the
same listener by `src/app/main.cc`.

## Build commands

```bash
# From the monorepo root
./scripts/build-all.sh dev --only camera
```

Driving CMake by hand inside the folder means installing the root graph once
(`./scripts/build-all.sh dev --install-only`) and passing its toolchain; the
exact flag set is in `docs/operations/build-and-test.md` under "Working
inside one project".

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, no comments in code, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
