# argus-auth — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for every change in
this service. The MUST-FOLLOW rules below restate the ones that apply to
auth-service code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **Sessions and device identity only** — this service owns the refresh
   tokens, the device credentials and the cross-device login challenges. It
   runs no AI capacity, no camera work and no sync transport, and it exposes
   no route other than `/health` and its own `/auth` subroute (Phase 3b-2).
2. **Single-owner database (rule 27)** — this service alone opens
   `database/auth.db`, whose schema is `services/auth/database/schema.sql`
   (this owner's only schema file). User rows, roles and the person directory
   are read through `argus::clients::identity`; never read another owner's
   database, and never reach into `services/identity`'s sources.
3. **The user context is a cache, never a source of truth** — role, name,
   language and `isActive` come from identity. `SessionContextCache` answers at
   most `[auth] context_cache_seconds` (default 30, `0` disables it) and the
   identity change consumer drops the entry the moment a `user` row changes, so
   a revoked account stops validating within one event.
4. **Token order is the verdict order** — `SessionService::validate` refuses in
   the order the identity gate did: unverifiable access token, no subject, no
   identity context, disabled account, missing/expired session row, device
   mismatch. `reason` is set only where the filter chain reads it; the rest
   refuse silently.
5. **The session verdict is gated per caller** — `[rpc.callers]` holds one
   credential per peer that verifies a session (camera, guard, identity,
   notification, productivity, settings, sync), each paired with that peer's
   `[auth] credential`. The old `[auth] rpc_secret` is accepted only while
   some caller is still unpaired. An open gate is legal only while the
   listener is bound to loopback, which is the native default; `main.cc`
   refuses to start when the listener is reachable beyond loopback without
   a paired caller or a legacy secret. The deploy template binds `0.0.0.0` behind that secret, which is what
   lets the peer containers reach `argus-auth:7043`; the compose publishes that
   port on `127.0.0.1` only, and the HTTP surface on the LAN.
6. **Never serialize an invitation, portrait or credential secret** — a device
   credential is looked up by the SHA-256 the caller already holds
   (`CheckDeviceCredential`); the plaintext never crosses the wire, never
   reaches a log and never lands in a table.
7. **Parameter structs for 3+ params** — any function with 3+ parameters must
   take a struct (designated initializers, every member listed).
8. **Dependency injection** — classes hold their dependencies as private
   members with `_` suffix; controllers hold instance members, never static
   methods.
9. **Smart pointers** — no raw owning pointers; raw pointers only for
   non-owning access.
10. **File naming** — `.hxx` headers, `.cc` sources, hyphenated `*-test.cc`
    tests. No `.h`/`.cpp`.
11. **100% English** — code, identifiers, docs, commits.
12. **No comments** — none in code, of any kind (root rule 20); the "why"
    goes to CONTEXT.md.
13. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`, `LOG_FATAL`);
    no spdlog.
14. **No std::future** — plain `std::thread` + join when parallelism is
    needed.
15. **Frozen contract** — `argus.auth.v1.AuthService` (`ValidateToken`,
    `CheckDeviceCredential`) and the `RefreshToken`/`UserAction`/`TableName`
    vocabulary the sync engine already serves never change here; the mobile app
    must keep working unmodified. Additive fields are the only growth: the
    verdict's `session_id` (field 5) is one.
16. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's own
    code; third-party includes are SYSTEM.

## Layout

```
argus-auth/
  CMakeLists.txt        add_subdirectory-compatible AND standalone buildable
  src/app/main.cc       config load, listeners, NATS bus, app run
  src/app/rpc/          argus.auth.v1.AuthService (fleet-secret gated)
  src/config/           this service's typed config ([auth], [identity],
                        [rate_limit] and the certificate pair)
  src/feature/session/
    repositories/       refresh_token queries and the row mapping
    schemas/            refresh_token row mapping
    services/           SessionService, SessionContextCache,
                        IdentityChangeConsumer, SessionRevocation (the one
                        path that ends a session) and session-events (the
                        sessionRevoked / sessionsChanged /
                        userSessionsChanged frames and their audit rows)
    infra/              ordered-delivery: the one durable handler shape this
                        service's consumers are built from
  src/feature/device/
    repositories/       device_credential lookup by secret hash, and the
                        device_login_challenge compare-and-set
    schemas/            device_credential and device_login_challenge mapping
  src/feature/auth/
    controllers/        the seventeen /auth routes (three of them the
                        caller's own sessions, four the owner's view of
                        every user's sessions, one the QR approver's
                        details)
    dtos/               the request and response DTOs of that surface
    services/           AuthFeatureService: sessions, credentials, challenges;
                        SessionManagementService: list and revoke the
                        caller's own sessions, and the owner's list and
                        revocation of any user's
    infra/              auth-rate-gate: the [rate_limit] pre-routing gate;
                        client-identity: platform and device name from the
                        X-Argus-Client / X-Argus-Device / User-Agent headers
  database/schema.sql   this owner's four tables — the three session tables
                        and the change outbox — with their six indexes
  config.toml.example   auth keys + the identity target; no AI keys
  tests/unit/           the session-verdict, device-login, refresh-gate,
                        session-management and migration suites (the first
                        has a live NATS leg)
  CONTEXT.md            purpose, ownership, wiring decisions
```

Three features, not four: `session` owns what a validated token means (the
verdict, its context cache and the identity change feed that invalidates it);
`device` owns the credential lookup and the login challenge's one-use
compare-and-set; `auth` owns the HTTP surface, its DTOs and the refresh
limiter that guards it. All three are feature-local — rule 23's
2+ rule has not been earned by any repository yet, so nothing sits in
`src/shared/`.

The top-level CMake auto-discovers feature folders and links
`argus::auth-config`, `argus::auth-auth`, `argus::auth-session`,
`argus::auth-device` and `argus::auth-rpc` by name. The wire arrives as
`argus::clients::auth`, which is also what compiles
`argus/auth/v1/auth.proto`.

## Endpoint and ports

The HTTP surface terminates TLS on `7042` and the RPC listener answers on
`7043`. The compose publishes `7042` on the LAN, where the app dials this
service's routes directly, and `7043` on `127.0.0.1` only, because the session
verdict is a fleet-internal answer gated by each caller's own credential
(`[rpc.callers]`) and reached over the bridge as `argus-auth:7043`. A request that arrives on the
`[remote] tunnel_port` listener is what `RemoteGate` refuses for `/pairing` and
`/auth/register` (`403 REMOTE_NOT_ALLOWED`) unless `[remote] enabled` is set.

## Build commands

```bash
# From the monorepo root
./scripts/build-all.sh dev --only auth
```

The suite's live leg arms itself from `ARGUS_NATS_URL` and skips without it; it
drives an isolated stream, subject and durable name, so it may run against the
deployment broker.

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, no comments in code, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
