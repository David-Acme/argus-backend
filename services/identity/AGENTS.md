# argus-identity — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for every change in
this service. The MUST-FOLLOW rules below restate the ones that apply to
identity-service code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **Identity only** — this service owns the users, the persons, the face
   embeddings, the learned voice profiles and their call samples, the
   invitations and their redemptions, the pairing state, the portraits and
   the private-portrait capabilities. Sessions, device
   credentials, refresh tokens and the refresh limiter belong to `argus-auth`;
   the filters the routes declare by name (`DeviceFilter`, `JwtFilter`,
   `RoleFilter`, `ValidJsonFilter`, `JwtService`) come from
   `packages/lib/auth`. Biometrics stay here: the face and speaker engines
   never leave this process, argus-camera only ships crops to it and the
   voice relay only ships clips. Only embeddings are persisted — never a
   recording — and a voice match is an identification signal, never an
   authentication factor.
2. **Single-owner database (rule 27)** — this service alone opens
   `database/identity.db`, whose schema is
   `services/identity/database/schema.sql` (this owner's only schema file).
   Sessions and device credentials are read through `argus::clients::auth`,
   never from another owner's database, and the five sync tables left this
   file for argus-sync's own `sync.db` in Phase 3c-2.
3. **Two listeners, one process** — TLS HTTP on `7044` (`[identity] port`) and
   the `argus.identity.v1` gRPC surface on `7040` (`[server] grpc_port`). The
   RPC listener is cleartext and fleet-gated by `[identity] rpc_secret`;
   `main.cc` refuses to start when it is reachable beyond loopback without
   one. An empty secret is legal only while the listener is bound to
   loopback, which is the native default.
4. **The invitation token never reaches storage or a log** — it is 256-bit
   opaque material, persisted as SHA-256 only and consumed atomically with the
   redemption row. `UserInvitationSchema` keeps the hash fields out of
   `toJson()`; never serialize, sync, log or return them.
5. **Portraits are private objects, not sync rows** — the bytes live in
   `S3StorageService` (private bucket). `/portrait-preview/{userId}` mints a
   requester-bound, short-lived, one-use capability and
   `/portrait-preview/{token}/content` consumes it atomically before the
   storage read. Never expose bucket paths, signed URLs, credentials, binary
   or the token in logs; a successful view writes a `UserAction::Read` audit
   event with safe metadata only.
6. **The change feed is a durable outbox, published through the domain sink** —
   features publish through `identity_change::setSink(...)`; the sink owns the
   outbox rows, the two subjects (the change subject the memory catalog and
   the sync engine follow, the action subject the journal subscriber reads) and
   the drain registered with `shutdown_signal::onStop`. Its worker is the
   sink's own thread, so the hook waits for it to report drained before
   Drogon's `quit()` destroys the database client manager the worker reaches
   through `DbService::client()`.
7. **A role update is not a logout** — persist first, then
   `sync_control::sink()->replaceRoleRooms`, then emit `AuthContextChanged`
   with `resync=true` to the user's room; preserve the socket and the user
   room. Only account deactivation invalidates refresh tokens and disconnects
   the device.
8. **Parameter structs for 3+ params** — any function with 3+ parameters takes
   a struct (designated initializers, every member listed).
9. **Dependency injection** — classes hold their dependencies as private
   members with `_` suffix; controllers hold instance members, never static
   methods.
10. **Smart pointers** — no raw owning pointers; raw pointers only for
    non-owning access.
11. **File naming** — `.hxx` headers, `.cc` sources, hyphenated `*-test.cc`
    tests. No `.h`/`.cpp`.
12. **100% English** — code, identifiers, docs, commits.
13. **No comments** — none in code, of any kind (root rule 20); the "why" goes
    to CONTEXT.md.
14. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`, `LOG_FATAL`);
    no spdlog.
15. **No std::future** — plain `std::thread` + join when parallelism is needed.
16. **Frozen contract** — `argus.identity.v1` (`IdentityService` and the
    `SyncService` pull leg) and the wire vocabulary the mobile app and the
    fleet already speak never change here; the app must keep working
    unmodified.
17. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's own
    code; third-party includes are SYSTEM.

## Layout

```
argus-identity/
  CMakeLists.txt        argus_service(NAME argus-identity ...) + feature
                        auto-discovery
  src/app/main.cc       config load, Drogon + the gRPC server, NATS bus,
                        filters, controllers, shutdown drains
  src/app/rpc/          argus.identity.v1 IdentityService (fleet-secret gated)
                        and SyncService (the sync engine's pull leg)
  src/config/           this service's typed config (db, listener, RPC, sync
                        control and face); config.toml.example also carries the
                        sections its features and the shared packages read
                        ([server], [drogon.app], [identity], [cert], [stt],
                        [jwt], [device], [auth], [sync], [nats], [face],
                        [storage] and [storage.s3], [pairing], [remote],
                        [mdns] — the announcement's name and the address it
                        binds)
  src/feature/enrollment/
    repositories/       the enrollment row reads
    services/           EnrollmentFeatureService: RegisterUser's write path
  src/feature/invitation/
    controllers/ dtos/  the /invitation routes and their DTOs
    services/           InvitationFeatureService: create, resolve, redeem
  src/feature/pairing/
    controllers/ dtos/  the /pairing routes and their DTOs
  src/feature/visitor/
    controllers/ dtos/  the /visitor, /visitor-settings and /visitor-crop routes
    repositories/       visitor (gallery reads, merge/split/delete), sighting
                        (the strand-side writes of one sighting) and
                        crop-capability
    services/           VisitorRecognitionService (camera sightings),
                        VisitorFeatureService (the Owner's gallery),
                        visitor-policy (pure thresholds), visit-pattern
  src/feature/face-upgrade/
                        re-embeds an account's legacy face from its portrait
                        when the face model changes
  src/feature/voiceprint/
    controllers/ dtos/  the owner's /voiceprint routes and their DTO
    repositories/       voice-profile, voice-sample and voice-device (this
                        feature alone reads them)
    services/           audio/ (WAV decode, resampling, speech quality),
                        embedding/ (SpeakerEmbeddingService over sherpa-onnx,
                        vector maths), index/ (the voice_vec vec0 index),
                        passive/ (PassivePolicy gates, VoiceCallTracker,
                        PassiveEnrollmentService), voiceprint/
                        (VoiceprintFeatureService, the journal helper)
    vocabulary/         VoiceProfileSource, VoiceSampleState, VoiceprintOutcome
  src/feature/user/
    controllers/        the /user routes and /portrait-preview
    dtos/               user update and portrait capability/image DTOs
    services/           UserFeatureService, PortraitPreviewService,
                        NatsIdentityChangeSink
  src/shared/repositories/
                        user, person, person-tag, person-snapshot,
                        face-embedding, user-invitation, user-portrait,
                        stored-file and portrait-preview-capability (2+ features
                        read them), plus change-outbox as its own module
  src/shared/schemas/   the row mappings of those tables
  src/shared/services/face/     FaceService + FaceDB (vec0 index)
  src/shared/services/storage/  PrivatePortraitService
  src/shared/services/token/    opaque-token: the 256-bit capability tokens
                                and their SHA-256 (invitation, portrait
                                preview)
  src/shared/vocabulary/        person-status
  database/schema.sql   this owner's fourteen tables and their indices
  config.toml.example   identity keys + the peer targets; no AI keys
  tests/unit/           the config, migration, change-outbox,
                        change-transaction, change-outbox-sink, sync-RPC,
                        face-slots, face-embedding, voiceprint-audio,
                        voiceprint-passive and voiceprint suites
  tests/fixtures/voiceprint/  nine LibriSpeech clips (CC BY 4.0, raw PCM)
  tools/migrate-identity/  argus-migrate-identity (argus.db -> identity.db)
  scripts/provision.sh  the deploy-time provisioning of this service: the
                        face models and the SHA-256-pinned speaker model
  CONTEXT.md            purpose, ownership, wiring decisions
```

Six features: `enrollment` owns the `RegisterUser` write path;
`invitation` the create/resolve/redeem cycle; `pairing` the paired state the
frontend's QR flow reads; `retention` the candidate expiry; `user` the profile
updates, the portrait-preview capability and the change sink; `voiceprint`
the voices Argus learns passively from each holder's calls and identifies
afterwards (`argus.identity.v1.VoiceprintService` for the voice relay, an
owner-only HTTP view and forget — see CONTEXT.md). Rule 23's 2+ rule is what puts the repositories
and schemas in `src/shared/`: each is read by two or more of them (and by the
RPC services), so they are earned there rather than parked.

Every `#include` inside this service is relative to `src/` — `<feature/...>`,
`<shared/...>`, `<config/...>`, `<app/...>` — the same include root the package
carried before the move, so no moved file needed its includes rewritten. The
one edit the move made is the retired `api/` segment: thirteen files that
spelled `<feature/api/x/...>` name `<feature/x/...>` here, and the header that
became `src/app/rpc/identity-rpc-service.hxx` dropped it in the same rename.

## Endpoints and ports

The HTTP surface terminates TLS on `7044` and the gRPC surface answers on
`7040`. The compose publishes `7044` on the LAN, where the app dials this
service's routes directly, and `7040` on `127.0.0.1` only, because the RPC
listener is a fleet-internal answer gated by `[identity] rpc_secret` and
reached by its peers as `argus-identity:7040`. A request that arrives on the
`[remote] tunnel_port` listener is what `RemoteGate` refuses for `/pairing`
(`403 REMOTE_NOT_ALLOWED`) unless `[remote] enabled` is set.

## Build commands

```bash
# From the monorepo root
./scripts/build-all.sh dev --only identity
```

The seven unit suites register in this service's standalone CTest graph;
`identity-config-test` covers this service's own config defaults, its section
overrides and the non-loopback RPC listener's secret gate;
`identity-face-slots-test` pins that a disabled face service answers instead
of blocking its caller;
`identity-sync-rpc-test` drives the `SyncService` pull leg end to end (the
role gate, the rule-7b user scope, the tombstones and both sides of the
fleet-secret gate) against a live Drogon loop and an in-process listener, and
`identity-change-outbox-sink-test` skips its live block unless `ARGUS_NATS_URL`
names a broker, and drives an isolated stream, subject and durable name so it
may run against the deployment broker:

```bash
ARGUS_NATS_URL=nats://127.0.0.1:4222 \
  services/identity/build/dev/identity-change-outbox-sink-test
```

The `argus-migrate-identity` tool is what `scripts/provision.sh` and the deploy
`identity-init` step ride; it verifies the migration row by row and is not a
runtime dependency of the service.

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules
> 19-25 (modern C++20, no comments in code, efficiency, DB tuning, feature
> layout + shared SDK, monolith structure, build-by-module-name).
