# Phase 3b step 1 — `services/auth`, `clients/auth` and the session verdict

Phase 3b is two steps in the plan: extract `services/auth` (sessions,
credentials, device credential, pairing/registration, rate limiting, the
credential-validation RPC) and create `clients/auth`; then purge `lib/auth` of
database access and point `JwtFilter` at the new client. Step 1 is landed here
in three parts, of which this report covers the first:

- **3b-1 (this unit)** — the service, its schema, the session-verdict RPC, the
  context cache, the identity change consumer, the SDK client and the wire
  contract, plus the deployment registration that makes it a first-class owner.
- **3b-2** — the `/auth` HTTP surface (login, pairing, QR, refresh, logout), the
  gateway's rate limiter and LAN gate moved onto it, and the row copy off
  `identity.db`.
- **3b-3** — `packages/lib/auth`'s filters pointed at `argus::clients::auth`,
  after which the gateway's session code and its `device_login_challenge`
  handlers are dead.

The service ships without a caller on purpose: it is the authority, and nothing
asks it anything until 3b-3 repoints the filters. What 3b-1 proves is that the
authority exists, that its verdict is the verdict the identity gate already
gave, and that its deployment wiring is complete.

## What existed before

The three session tables — `refresh_token`, `device_login_challenge` and
`device_credential` — lived in `identity.db`, owned by `packages/identity`, the
package the gateway hosts. `packages/lib/auth` had already lost its database
access in f7-3: `JwtFilter` verifies the access token's signature locally, then
asks `argus.identity.v1.ValidateToken` through the shared client in
`packages/lib/auth/src/auth/details/identity-access.{cc,hxx}`, off the event
loop with `BlockingTask`. The plan's step-2 wording ("purge `lib/auth` of
database access") therefore described work f7-3 had already done; what was left
was for the verdict to have an owner of its own and a client of its own.

## The decision, as built

### Three tables, no user row

`services/auth/database/schema.sql` is the identity schema's three tables with
their indexes, and it is deliberately not a verbatim copy: the two
`REFERENCES user(id) ON DELETE CASCADE` clauses on `refresh_token.user_id` and
`device_credential.user_id` are dropped, because `user` is a table this database
does not have (rule 27), and with `foreign_keys = ON` an insert into a child of
a missing parent fails outright.

Dropping the cascade changes no answer the service gives: the verdict resolves
the user through identity before it reads the session row, so a user that
identity no longer knows refuses on the context, and a deactivation revokes the
sessions through the change consumer rather than through SQL. Everything else —
column names, types, NOT NULLs, defaults and the `is_valid`/`is_used`/
`is_active`/`status` CHECK constraints — is identical, which is what makes
Phase 3b-2's move a copy rather than a translation.

No CHECK-constrained column is spelled as a raw string in code: `is_valid`,
`is_used` and `is_active` are integers written as `0`/`1` by their repositories,
and `device_login_challenge.status` has no writer yet (3b-2 brings the pairing
handshake that will write it).

### The verdict is identity's verdict, one owner over

`SessionService::validate` refuses in the identity gate's order, check for
check, and the order is load-bearing:

| Order | `IdentityRpcService::ValidateToken` (before) | `SessionService::validate` (now) |
|---|---|---|
| 1 | access token does not verify → refuse, no DB | same |
| 2 | no `sub`, or `sub` not a positive integer → refuse | same |
| 3 | user row absent → refuse | identity context absent → refuse |
| 4 | `!isActive` → `User account is disabled` | same |
| 5 | (with device context) `refresh_token` row absent → refuse | `refresh_token` row absent → refuse, **on both paths** |
| 6 | row expired → `Token expired` | same |
| 7 | `device_hash` differs → `Device mismatch` | same, and only when a device context was sent |
| 8 | answer with the user, `expires_at` from the row | same, `0` when the path was access-only |

Three differences, all deliberate. Step 3 now travels through
`argus::clients::identity`, because the user row belongs to identity. The
`reason` string is set only for the three cases the old filter surfaced to the
client, because a caller has no business learning which check failed. And the
session row is required on *both* paths rather than only when a device context
was sent: a token whose row was revoked or rotated must stop validating
everywhere, and the device context decides only whether the hash is compared,
because it is the transport that enforced the binding.

An access-only validation (no device context — what the WebSocket upgrade asks)
answers `valid` with `expires_at = 0`, the convention identity used.

### The context is a cache with a short life and an event that ends it

`SessionContextCache` is keyed by user and holds one `UserContext` per entry
(name, last name, language, role, `isActive`). `resolve` answers from the cache
when the entry is live, otherwise asks identity through `GetUser` on a
`BlockingTask`. Three properties matter:

- a `[auth] context_cache_seconds` of `0` disables the cache entirely — nothing
  is stored at all, rather than an entry that has already expired — which is
  what the verdict tests run with when they count identity calls;
- identity unreachable, or answering without a user, is `nullopt`, which the
  verdict turns into a refusal: the cache can delay an answer, never invent one;
- the entry is dropped the moment the identity change feed carries a change to
  that `user` row, so a role change or a deactivation reaches the next request
  instead of waiting out the TTL. The TTL is a load valve — how long identity is
  trusted to be reachable without being asked again — not the invalidation
  mechanism.

### A user change is two effects, in one order

`IdentityChangeConsumer` holds the durable `argus-auth-identity` on
`argus.identity.v1.change` (stream `ARGUS_IDENTITY_CHANGE`, `maxDeliver` 10,
`maxAckPending` the ordered maximum the bus exposes). It acts only on the
envelope it owns — `kind: "identity"` and `table: "user"` — and ignores the
person rows and the audit kinds that share the subject. For a user row it
forgets the cache entry first and then, only if the row arrives with
`isActive: false`, revokes every session of that user.

The order is the point: forgetting first means a concurrent request cannot
re-populate the entry from the row that is being replaced — and the fetch that
request may already have started carries the generation it began in, so its
answer cannot land either. A revocation that found nothing to invalidate is the
normal case for a replayed message: it is logged at debug level and acked,
because the statement itself succeeded. A statement that *throws* is a different
thing — the marshal naks it, so the change is redelivered rather than swallowed.

`start()` tolerates a cold broker — if the stream is not there yet it retries
every 5 s from a Drogon timer instead of failing the boot — and `main.cc` logs a
warning and keeps serving when NATS is unreachable at all, because a session
validated before the outage keeps working for its TTL and every validation
afterwards asks identity directly.

### One durable handler shape

`src/feature/session/infra/ordered-delivery.hxx` is the only place in this
service where a durable message becomes a coroutine. It copies the payload and
the settlement out of the cnats callback, hops onto IOLoop 0 with `runInLoop`,
runs the body through `async_run`, and acks on success or naks on a thrown
exception. Nothing is captured by reference into a frame that suspends — the
shape rule 18 of the root `AGENTS.md` documents as a use-after-free.

### The RPC listener is a fleet secret, not a route

`argus.auth.v1.AuthService` answers `ValidateToken` and
`CheckDeviceCredential` on a cleartext gRPC listener that is loopback-bound by
default (7043), beside an HTTP listener (7042, TLS with the instance
certificate) that today serves only `/health` and gains `/auth` in 3b-2.

`fleetAuthorized` mirrors identity's: an empty secret on the server side
authorizes, which is the native development default; a non-empty one requires a
constant-time match on the `x-argus-fleet` metadata (`kFleetSecretKey`, the same
key identity's gate reads). `main.cc` refuses to
start (`LOG_FATAL` and `_exit(1)`) when the listener is reachable beyond
loopback and the secret is empty, because an ungated verdict RPC is a session
oracle. The compose publishes 7043 on `127.0.0.1` only, and the deploy template
carries the same placeholder as every other fleet secret
(`CHANGE_ME_SAME_IN_EVERY_SERVICE`), filled once per installation by
`ensure_deploy_configs`.

`CheckDeviceCredential` answers by the SHA-256 the caller already holds; the
plaintext secret never crosses the wire, never reaches a log and never lands in
a table.

### The contract and the client

`packages/contracts/proto/argus/auth/v1/auth.proto` declares
`argus.auth.v1.AuthService` with the two RPCs and the four messages.
`SessionUser` mirrors identity's `UserIdentity` field for field — same numbers,
same names — so the later repoint is a type swap rather than a wire change, and
`ValidateTokenResponse` keeps `valid`, `reason`, `user` and `expires_at` in the
positions identity's already had.

`packages/clients/auth` mirrors `packages/clients/identity`: the same guard,
the same `argus_clients(NAME auth PROTO …)` declaration with the proto compiled
by the client package (a contract ships no generator), and a `AuthClient` whose
two calls are virtual so a test can stand in for the wire. The fleet secret
travels as metadata; no caller builds a stub, a URL or a retry policy of its own.

### Deployment

`argus-auth` is the eighteenth project in `scripts/build-all.sh`: its own
`config.toml.example`, a `Dockerfile` mirroring `services/sync`'s two-stage
trixie build, a `Dockerfile.dockerignore` derived from the same, a compose
service with the peer-services shape (`networks: internal` with an alias, the
data directory and the schema bind-mounted, 7042 published and 7043 on
loopback), and the provisioning/setup lists that give it a data directory and a
local config. The deploy template joins the `config.*.toml.example` set, so
`ensure_deploy_configs` fills the fleet secret it declares without any further
wiring.

## The review

Two read-only adversarial reviews ran against the finished unit — one on the service's code, one on
its wiring and the documentation that follows it. Every finding below was re-checked against the tree
before anything changed; the ones that were real are fixed in this unit, and the verdicts are recorded
because not every finding was wholly right.

### Wiring and documentation (nine findings: seven real and fixed, one already fixed, one recorded)

| # | Finding | Verdict |
|---|---|---|
| 1 | `packages/clients/auth/AGENTS.md` named a consumer (`packages/lib/auth`'s `details/auth-access.cc`) and three config keys (`auth.target`, `auth.rpc_host`, `auth.rpc_port`) that do not exist | **Real.** Future-tense 3b-3 work written as present fact: the file does not exist, `lib/auth` links only `clients::identity`, and the only `AuthClient` construction in the tree is the package's own test — `argus-auth` links the target for the generated stub. Rewritten: the package resolves nothing itself, and 3b-3 is named as the step that adds the consumer. |
| 2 | `docs/operations/deployment-docker.md`'s container inventory omitted `argus-sync` and `argus-guard` | **Real.** Both are compose services. Added. |
| 3 | The same file's data-directory inventory omitted `gateway/` | **Real.** `provision-host.sh` creates eight directories; the doc named seven. Added. |
| 4 | `.env.example` said to be missing `AUTH_PORT`/`AUTH_RPC_PORT`, and the doc paragraph enumerating what the compose reads beyond it | **Already fixed.** This unit had put both keys in `.env.example` before the review read it. The doc paragraph lists keys the compose reads that are *absent* from `.env.example`; the auth ports are no longer absent, so naming them there would now be wrong. No change. |
| 5 | A native install generates per-project secrets that cannot match across services | **Real, pre-existing, recorded** — the generator defect this unit had already flagged. The review added one fact worth keeping: the native gateway template carries no `[identity] rpc_secret`, so its gate is open and only the JWT leg fails today. Folded into the noted item. |
| 6 | The docs call the RPC listener "loopback-bound" while the deploy template binds `0.0.0.0` | **Real**, and a rule read literally would have broken deploy reachability. Fixed in three files: an empty secret is legal *only while* the listener is bound to loopback; the deploy template binds every interface behind that secret, which is what lets peer containers reach `argus-auth:7043`; only the published port is loopback-only. |
| 7 | `build-model.md`'s sqlite-vec "Compiled by" list names `cert` | **Real.** `packages/lib/cert/CMakeLists.txt` mentions neither sqlite-vec nor sqlite and no `cert` compile database compiles it. Dropped. The neighbouring `ncnn` row carried the same stale `cert` entry *and* omitted `guard`, so both rows were measured and fixed together. |
| 8 | The client-package counts were not updated for the new client | **Real, fixed — and re-measured twice.** The first pass fixed the Key Files row (`AGENTS.md`: "all ten" → "all twelve", enumeration completed, `auth` and `sync` added) and `services-and-packages.md` ("the eleven SDK clients" → twelve). A second, independent count of rule 25's "today" paragraph then found three more figures the same drift had left stale, none of them introduced by this unit: the client sentence still said "seven of the ten clients wrap a generated gRPC stub — six of them pass `PROTO`" where the tree has **twelve** clients, **eight** passing `PROTO` (the six, plus `sync` since Phase 3a and `auth` here) and **nine** wrapping a stub (`tts` reaches it through `argus::contracts::tts`); the paragraph's dependency census said 20 grouped packages naming an `argus::` alias by hand where the tree has **21** (nine contracts, the `response` and `tts` wire modules among them, six clients, six libs); and it said four ungrouped packages add four more where **two** of the three that remain name one (`identity`, `intent` — `memory` calls no helper, and `audit` and `room` left `packages/` in Phase 3a). All four figures re-measured against the tree, not against the prose. |
| 9 | `AGENTS.md` had the filter chains "asking for" the verdict RPC in the present tense | **Real.** The same tense defect as finding 1, in the rules file. Rewritten to name Phase 3b-3, and `events-and-contracts.md`'s "replaces the filters' direct database access" — false since f7-3 left the filters calling `argus.identity.v1` — now names the call that is actually replaced. |

The review also confirmed by measurement, rather than by reading this report: the deploy template's
`0.0.0.0` matches all twelve peer examples while the native template keeps sync's `127.0.0.1` shape;
every config key `auth-config.cc` reads is present in both templates; the fleet-secret fill path is
idempotent and shared with the gateway; the compose block is field-for-field the sync/guard shape and
`docker compose config --quiet` is clean; the Dockerfile's apt set carries no library `ldd` does not
ask for; `Dockerfile.dockerignore` excludes every secret and every build tree; and rule 20 holds at
1297 files, 0 comments.

### The service and its suite (thirteen findings: eleven real and fixed, two recorded)

| # | Finding | Verdict |
|---|---|---|
| 1 | The access-only validation path answered without reading the session row, so a revoked or rotated token still validated on the WebSocket upgrade | **Real, fixed by strengthening.** The row is now required on both paths; only the hash *comparison* is what a device context adds. Deliberate: it is a behaviour change against the gateway's `JwtFilter`, in the safe direction, and it is pinned by the case below. |
| 2 | The generator gives `argus-auth` its own `[jwt] secret` and `[identity] rpc_secret`, and both legs are therefore broken on a fresh install | **Half real.** The JWT leg is the defect this unit had already flagged and is recorded below. The identity leg is not broken: an empty server secret authorizes, so a mismatched client secret still reaches identity. Recorded, not changed — build-infrastructure scope. |
| 3 | The revocation WARN fires when there was nothing left to revoke, and the effect of a revocation on the rows was untested | **Real, fixed.** The log line is debug-level and says "no live session left to revoke"; two cases now pin the effect (`liveRowsOf`). |
| 4 | `CONTEXT.md` claimed a concurrent request cannot re-populate the cache during an invalidation, which the code did not do | **Real, fixed.** The claim was true only because the TTL writes were unsynchronised in the wrong direction: a fetch now carries the cache generation it started in and writes only if that generation is still current. |
| 5 | `SessionService`'s constructor took four parameters | **Real, fixed** (rule 2): `SessionService::Dependencies`, the struct the tree already uses for this shape. |
| 6 | The RPC listener binds before the schema advice runs, so a request could arrive before the tables exist | **Real, recorded as parity.** Measured across the fleet: `camera`, `notification` and `productivity` all bind in the same order. Left as-is rather than making this one service differ. |
| 7 | The identity change consumer registered no drain, so the database freeze could run underneath a revocation | **Real, fixed.** `requestStop()`/`drained()` now report an in-flight count, and `main.cc` registers it with `shutdown_signal::onStop`. |
| 8 | `packages/clients/auth/AGENTS.md` stated a present-tense consumer for the client | **Real, fixed.** The passage now says which phase repoints `packages/lib/auth` and what the client *is for* rather than who calls it today. |
| 9 | `context_cache_seconds = 0` still inserted an entry | **Real, fixed.** A disabled cache now stores nothing at all. |
| 10 | `CONTEXT.md` named Phase 3c-2 and, 100 lines later, Phase 3b-2 for the same row move | **Real, fixed.** The plan is explicit for 3b-2. |
| 11 | The suite never revoked anything | **Real, fixed.** A case asserts a revocation ends every live session row of the user, and that revoking again reports there was nothing to do. |
| 12 | The client test read the seen headers with `.at()`, which throws instead of failing the assertion | **Real, fixed.** `contains()` then `at()`, so a missing header reports a failed check. |
| 13 | The durable marshal caught only `std::exception` | **Real, fixed.** A second `catch (...)` naks, so a non-`std::exception` failure redelivers instead of being swallowed. |

## What proves it

`tests/unit/session-verdict-test.cc` (629 lines) drives a real gRPC server
behind a real `AuthClient` over a temporary database, and covers:

- a valid session answering the user the filters read;
- the refusal order, pinned by the fixtures rather than by prose: a disabled
  account refuses over a live session row, an expired row refuses before the
  device hash is compared, and an unreachable identity refuses a token whose
  row is live — which is what shows the context is consulted rather than
  assumed;
- the context cache answering a repeat validation without asking identity
  (this is the case that counts identity's calls);
- the fleet secret gating every session verdict;
- device credentials answering active only for a live secret hash;
- a session row that was revoked or rotated refusing on *both* validation
  paths, and a revocation clearing every live row the user holds;
- an identity change dropping the cached context (the live NATS leg).

The live leg arms itself from `ARGUS_NATS_URL` and passes with a printed notice
without it, and it names its stream, subject and durable after its own pid so it
may run against a deployment broker without touching a deployment stream.

The suite carries its own `main()` instead of the repo's `SharedBoot` idiom
because of a measured defect in the harness rather than in the service: a binary
that boots Drogon with a SQLite `DbClient`, connects and drains a `NatsBus`, and
then lets Drogon's teardown run during exit-time static destruction corrupts the
heap on a `DrogonIoLoop` thread (`corrupted double-linked list`, SIGABRT, exit
134) — reproducibly, with or without gRPC or subscriptions in the path, and with
the bus leaked rather than destroyed. The same binary that quits Drogon during
normal execution is clean on every run, which is also what production does:
`shutdown_signal::onStop` drains, then holds `quit()` until the drains report.

## Verification

| Check | Result |
|---|---|
| `./scripts/build-all.sh dev --only auth` | exit 0 — 0 errors, **0 warnings**, 21/21 tests. Every one of the unit's 33 sources was touched first, so the compile output is a full recompile of the unit rather than an incremental build that would hide a warning. |
| `session-verdict-test` | 8 cases, **79 assertions**, all pass; **92 assertions** with `ARGUS_NATS_URL=nats://127.0.0.1:4222` armed against a live broker, which is the identity change consumer's leg. |
| `auth-grpc-client-test` | 3 cases, 26 assertions, all pass. |
| `scripts/check-comments.sh` | `1297 files checked, 0 comments`. |
| `scripts/check-deps.sh` | `70 declarations, 544 edges, 0 forbidden, 0 cycles, 0 unresolved, 49 edges deferred to phase 3`. |
| `scripts/check-tidy.sh` | exit 0 — `524 TUs, 3030 findings over 45 checks, baseline 3030`, no `risen:` line and no `; N checks below it` suffix, so every one of the 45 checks sits exactly on its floor. The 13 TUs over the previous scan's 511 are this unit's own (ten service sources, `auth-client.cc`, two suites), and the finding total is unchanged from the floor the previous unit recorded: the unit's new code adds no finding on any check. The baseline was then re-recorded on the verified tree — one line changed, `tus 511` → `tus 524`, all 45 counts already at their measured values |
| `./scripts/build-all.sh dev` (full) | exit 0 — **18/18 projects**, every suite `100% tests passed, 0 tests failed`, **483 tests** (462 + this unit's 21), **0 compiler warnings and 0 errors in first-party code**. The 21 `warning:` lines are third-party CMake notices (ncnn 7, glslang 7, ggml 3, openfst 1, ccache 3) and no line matches a compiler diagnostic. `check-comments` 1297 files / 0 comments, `check-deps` 70 declarations / 544 edges / 0 forbidden / 0 cycles / 0 unresolved / 49 deferred, `check-tidy` 524 TUs / 3030 findings / baseline 3030 with nothing risen |

The unit's own gate rejected it once, and the rejection is the reason `auth-grpc-client-test` reads
the way it does. The first full scan put **one** check above its floor — `bugprone-unchecked-optional-access`
235 against a baseline of 223 — and all twelve findings sat in that suite's verdict case, on the
`.value()` calls the code review's finding 12 had prompted: `REQUIRE(response.has_value())` is a
doctest macro, whose expansion the analyzer cannot follow, so the guard proved nothing to it. The
fix is the shape the tree already uses for exactly this (`cert-san-test.cc:218`): a plain
`if (!response.has_value()) { FAIL("…"); return; }` guard followed by `->` access, which the check
understands and which still reports a doctest failure. Re-measured on that translation unit alone:
zero findings.

## Flagged, not fixed

- **The dev config generator hands each project its own copy of a shared
  secret.** `scripts/lib/common.sh:ensure_project_config` fills `jwt secret`,
  `jwt refresh_secret`, `device fingerprint_secret` and `identity rpc_secret`
  with an *independent* random value per project, while every one of those keys
  has to hold the same value across services to work at all. Measured on a
  generated copy of this service's template: `identity.rpc_secret` length 64,
  `jwt.secret` length 96, `auth.rpc_secret` empty. A freshly generated native
  install therefore 401s cross-service calls; the checked-in configs predate the
  generator's behaviour and are empty, which is the value that works. This is
  build-infrastructure scope, and the fix is one shared value per installation
  generated once — so `auth.rpc_secret` was deliberately *not* added to that
  heredoc, and the trap is recorded rather than reproduced.
- **The exit-time Drogon teardown heap corruption** described above is in the
  harness, not in this service, and it is recorded here rather than fixed
  because every other suite in the tree avoids it by not connecting NATS.
- **`device_login_challenge` ships with no reader or writer.** It is created by
  this service's schema because the table moves here as part of the same
  ownership change, and Phase 3b-2 brings the pairing handshake that uses it.
  Recorded rather than left implicit so the empty table is not read as an
  oversight.

## Files

| Path | What it is |
|---|---|
| `services/auth/database/schema.sql` | The three session tables and their indexes |
| `services/auth/src/app/main.cc` | Boot: config, NATS, schema, listeners, the refuse-to-start gate |
| `services/auth/src/app/rpc/auth-rpc-service.{hxx,cc}` | `argus.auth.v1.AuthService`, fleet-gated |
| `services/auth/src/config/auth-config.{hxx,cc}` | `[auth]`, `[cert]`, `[identity]`, the RPC listener |
| `services/auth/src/feature/session/services/session-service.{hxx,cc}` | The verdict |
| `services/auth/src/feature/session/services/session-context-cache.{hxx,cc}` | The user context cache |
| `services/auth/src/feature/session/services/identity-change-consumer.{hxx,cc}` | The durable that drops and revokes |
| `services/auth/src/feature/session/services/user-context.hxx` | The context the verdict carries |
| `services/auth/src/feature/session/infra/ordered-delivery.hxx` | The durable → coroutine marshal |
| `services/auth/src/feature/session/{repositories,schemas}/refresh-token/` | `refresh_token` access and mapping |
| `services/auth/src/feature/device/{repositories,schemas}/device-credential/` | `device_credential` access and mapping |
| `services/auth/tests/unit/session-verdict-test.cc` | The suite |
| `services/auth/{AGENTS.md,CONTEXT.md,config.toml.example,CMakeLists.txt}` | This owner's instructions, decisions and config |
| `packages/contracts/proto/argus/auth/v1/auth.proto` | The frozen `argus.auth.v1` wire |
| `packages/clients/auth/**` | The SDK: `AuthClient`, its test, its AGENTS.md |
| `argus-deploy/config.auth.toml.example` | The deploy config template |
| `argus-deploy/.env.example` | The two ports the compose reads for this service |
| `services/auth/{Dockerfile,Dockerfile.dockerignore}` | The image |
| `argus-deploy/docker-compose.yml` | The `argus-auth` service and its ports |
| `scripts/{build-all.sh,setup.sh,provision-host.sh,lib/common.sh}` | The eighteenth project's wiring |
| `packages/contracts/sync/src/sync/sync-change.hxx` | The four catalog-row envelope constants |
| `packages/identity/src/feature/api/user/services/nats-identity-change-sink.cc` | The sink that reads them |
| `AGENTS.md`, `docs/**`, `packages/contracts/sync/AGENTS.md` | The documentation that follows the files |
