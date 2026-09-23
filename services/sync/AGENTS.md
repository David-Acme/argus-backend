# argus-sync — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to sync-service code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **Sync transport and sync state only** — this service serves `/sync`, owns
   the rooms and the fan-out, persists the audit trail and the action journal,
   drains the notification delivery stream and answers the control RPC. It runs
   no AI capacity (face, llm, vlm, tts, stt, vad stay elsewhere) and exposes no
   HTTP route other than `/health` and the WebSocket upgrade.
2. **Single writer of its four tables** — `audit_log`, `user_audit_log`,
   `user_action_log` and `notification_delivery_inbox` are written here and
   nowhere else. Producers publish an event on NATS; never a second writer,
   never a producer INSERT.
3. **The transitory database is declared, not hidden** — the schema is
   `services/sync/database/schema.sql` (this owner's only schema file) and the
   file it is applied to is identity's `database/identity.db` until Phase 3c-2
   splits these tables into `sync.db`. Rule 27's exception lives in
   `[sync] db` and nowhere else; do not "fix" it by copying a foreign schema.
4. **Normal rows are creation-only after bootstrap** — `Synchronize` pages by
   `created_at`; an update or a revocation is published as a granular audit
   change, never as a row the feed re-sends. Never switch the paging leg to
   `updated_at`/`syncAt`.
5. **Audit requests page by monotonic id** — `{findLast:true}` answers a
   `watermarkId`, then `afterId < id <= endId` in ascending order;
   `afterId = 0` establishes an empty baseline and `nextCursorId` is returned
   only after the bounded query shape is accepted. `changes` is a
   `JsonDiff::createFlatDiff` pair set — never a replacement record.
6. **The control RPC injects frames into any user's room** — it is gated by
   `sync.control_secret` (the same value in every service's config) and the
   service refuses to start when that listener is reachable beyond loopback
   without it. The listener is cleartext; keep it loopback-bound (the compose
   publishes 7041 on `127.0.0.1` only).
7. **One dispatcher, two transports** — the control RPC rebuilds its typed
   frame into the change feed's envelope and hands it to the same dispatcher
   the NATS leg uses. Never add a second path into `RoomManager`, and never
   hand-build a `Log` payload or emit a full `Add` for an update.
8. **No other service's database** — camera, productivity and notification
   tables are paged through `argus::clients::{camera,productivity,notification}`
   and the voice leg relays through `argus::clients::voice`. This service opens
   exactly one SQLite file.
9. **The engine lives here** — `transport` holds `SyncSocket`/`SyncService`/
   `SynchronizedService`, the `synchronized-dto.hxx` DTOs and the three
   pull-source contracts; the audit, user-audit and action-journal rows are
   `src/shared/repositories/` and `src/shared/schemas/` because both features
   read them (rule 23's 2+ rule), while the two writers over them,
   `audit-log-service` and `user-audit-log-service`, sit in `fanout`, their one
   reader. The forwarder vocabulary is `argus::contracts::sync`'s, never a copy.
10. **Portrait and invitation privacy** — Guard never receives invitation data
   or portrait bytes, and Resident/Guest receive only their own user row. The
   scope lives in `role_access` and `SynchronizedService`; route permission
   alone is not enough.
11. **Parameter structs for 3+ params** — any function with 3+ parameters
    must take a struct (designated initializers, every member listed).
12. **Dependency injection** — services/filters hold dependencies as private
    members with `_` suffix; controllers hold instance members, never static
    methods.
13. **Smart pointers** — no raw owning pointers; raw pointers only for
    non-owning access.
14. **File naming** — `.hxx` headers, `.cc` sources, hyphenated `*-test.cc`
    tests. No `.h`/`.cpp`.
15. **100% English** — code, identifiers, docs, commits.
16. **No comments** — none in code, of any kind (root rule 20); the "why"
    goes to CONTEXT.md.
17. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`, `LOG_FATAL`);
    no spdlog.
18. **No std::future** — plain `std::thread` + join when parallelism is
    needed.
19. **Frozen protocol** — `SyncOperation` 0-7, `TableName` 0-23,
    `SYNC_LIMIT = 200`, the `{type, payload}` request and
    `{operation, option, info}` response envelopes and the `voice:*` frame
    names never change here; the mobile app must keep working unmodified.
20. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's
    own code; third-party includes are SYSTEM.

## Layout

```
argus-sync/
  CMakeLists.txt        add_subdirectory-compatible AND standalone buildable
  src/app/main.cc       config load, gRPC control listener, NATS legs, app run
  src/app/rpc/          argus.sync.v1.SyncControlService (fleet-secret gated)
  src/config/           this service's typed config ([sync], [cert], upstreams)
  src/feature/transport/
    controllers/        SyncSocket, the /sync WebSocket controller
    dtos/               the sync DTOs (synchronized-dto.hxx)
    services/           SyncService, SynchronizedService
    repositories/       the event rows SynchronizedService pages
    schemas/            event and person-event row mapping
    infra/              the three domain pull sources, the socket registrar
                        and the voice gRPC relay the forwarder rides
  src/feature/fanout/
    repositories/       delivery inbox (query + repository + receipt)
    services/           sync fan-out, audit fan-out, delivery consumer
                        and the two audit writers they persist through
  src/shared/repositories/  audit_log, user_audit_log, user_action_log
  src/shared/schemas/       their three row mappings
  src/shared/services/      RoomManager
  src/shared/infra/         the notification row JSON both features render
  database/schema.sql   this owner's four tables and their five indexes
  config.toml.example   sync keys + the upstream targets; no AI keys
  tests/{unit,e2e,fixtures}
  CONTEXT.md            purpose, ownership, wiring decisions
```

Two features, not four: `transport` owns the socket, the engine's composition
and the three pull-source adapters; `fanout` owns everything a change event or
a delivery does on arrival, the two audit writers included. Between them sit
`src/shared/`'s repositories and schemas (the paging leg and the fan-out both
read them), `src/shared/services/room/` (the transport, the fan-out and
`main.cc` all hold the registry) and the one `src/shared/infra/` header,
`notification-row-json.hxx`, which both features render rows through.

The top-level CMake auto-discovers feature folders and links
`argus::sync-transport`, `argus::sync-fanout`, `argus::sync-config`,
`argus::sync-control-rpc`, `argus::sync-repositories` and
`argus::sync-services` by name. The control leg arrives as
`argus::clients::sync`, which is also what compiles `argus/sync/v1/sync.proto`.

## Endpoint and ports

`/sync` terminates TLS on `7025` and the control RPC listens on `7041`; both
are this service's subroute, and the compose publishes 7025 on all interfaces
because the app dials the socket directly — a WebSocket upgrade cannot ride the
gateway's reverse proxy. The control port stays loopback-bound.

## Build commands

```bash
# From the monorepo root
./scripts/build-all.sh dev --only sync
```

Driving CMake by hand inside the folder means installing the root graph once
(`./scripts/build-all.sh dev --install-only`) and passing its toolchain; the
exact flag set is in `docs/operations/build-and-test.md` under "Working
inside one project".

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, no comments in code, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
