# argus-notification — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to notification-service code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **Notification domain only** — this service runs the notification data
   domain. It must not compile or load any AI service registry (face, llm,
   vlm, tts, stt, vad stay in the legacy), no stream/media, no socket relay,
   and takes no labs.
2. **Single-owner database (Rulings AN/AR, rule 27)** — this service alone
   owns and opens `notification.db`, including its own camera-notification
   feature (`src/feature/camera-notification/`, moved here by Phase 3d step
   1): the notifier subscribes to `argus.camera.v1.object_detected` and
   `argus.guard.v1.heartbeat` and creates through the in-process
   `NotificationService`, never through a client to itself. `argus-sync`'s
   `/sync` notification pulls use `argus.notification.v1` PullNotifications;
   no other service mounts the volume.
3. **Parameter structs for 3+ params** — any function with 3+ parameters
   must take a struct (designated initializers, every member listed).
4. **Dependency injection** — services/filters hold dependencies as private
   members with `_` suffix; controllers hold instance members, never static
   methods.
5. **Smart pointers** — no raw owning pointers; `std::unique_ptr` with
   custom deleters for C handles; raw pointers only for non-owning access.
6. **File naming** — `.hxx` headers, `.cc` sources, hyphenated `*-test.cc`
   tests. No `.h`/`.cpp`.
7. **100% English** — code, identifiers, docs, commits.
8. **No comments** — none in code, of any kind (root rule 20); the "why"
   goes to CONTEXT.md.
9. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`,
   `LOG_FATAL`); no spdlog.
10. **Health safety** — `GET /health` must never fail or block on any
    downstream service; degraded dependencies degrade logs, not health.
11. **Never trigger setAlarm/siren paths** — no code path here may ever arm
    the siren.
12. **No argus.db migrations** — this service owns `notification.db` only
    (`services/notification/database/schema.sql`); it never writes or migrates
    `argus.db`.
13. **Frozen contracts** — HTTP paths, the `{status, info, errors}` envelope,
    `SyncOperation` 0-7, `SYNC_LIMIT=200` and `TableName` 0-23 never change
    here; the mobile app must keep working unmodified.
14. **No std::future** — plain `std::thread` + join when parallelism is
    needed.
15. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's
    own code; third-party includes are SYSTEM.

## Layout

```
argus-notification/
  CMakeLists.txt        add_subdirectory-compatible AND standalone buildable
  src/app/main.cc       config load, notification.db wiring, gRPC server, app run
  src/app/rpc/          argus.notification.v1 owner (create + pull) —
                        module argus::notification-rpc
  src/notification/     notification-domain config resolution and the NATS sinks
  src/feature/notification/
                        controllers/, dtos/, repositories/, schemas/, services/
                        — the HTTP write-side surface, the delivery-proof
                        surface and the token family; module
                        argus::notification-feature
  src/feature/camera-notification/
                        camera object policy + notifier and the fallback log;
                        module argus::notification-camera-notification
  src/shared/           the notification repository, schema and service both
                        features read, plus the change outbox module
  config.toml.example   notification-domain keys only ([server],
                        [notifications], [jwt], [device], [identity] — the
                        roster the camera notifier resolves recipients from;
                        no AI keys)
  CONTEXT.md            purpose, ownership, wiring decisions
```

Every feature and the `app/rpc/` owner is a rule-25 module: the folder holds
its own `CMakeLists.txt` declaring its sources and dependencies once, the
root file discovers them (`feature/*/CMakeLists.txt`) and the executable
links `argus::notification-feature`, `argus::notification-rpc` and
`argus::notification-camera-notification` by name. The feature module goes in
**whole-archive** (`$<LINK_LIBRARY:WHOLE_ARCHIVE,argus::notification-feature>`)
because its two HTTP controllers are Drogon `AutoCreation` controllers: their
routes come from a static initializer, so a plain archive link would drop
them silently (Drogon refuses explicit registration of such controllers, so
the tts pattern of registering them by hand is not available). The
notification-token repository, schema and service live inside
`src/feature/notification/`, the only feature that reads them;
`notification-core` compiles the notification table's own repository, schema
and delivery service, which both features reach.

## Build commands

```bash
# From the monorepo root
./scripts/build-all.sh dev --only notification
```

Driving CMake by hand inside the folder means installing the root graph once
(`./scripts/build-all.sh dev --install-only`) and passing its toolchain; the
exact flag set is in `docs/operations/build-and-test.md` under "Working
inside one project".

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, no comments in code, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
