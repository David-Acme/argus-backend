# Phase 3b steps 2 and 3 — the `/auth` surface in `argus-auth`, the filters on `clients/auth`

Phase 3b is two steps in the plan, and their second halves land here as one
unit because they are the same edge seen from both sides. 3b-2 brings the
`/auth` HTTP surface (login, enrollment/registration, pairing, QR, refresh,
logout, me), the gateway's LAN gate and rate limiter moved onto it, and the
copy of the three session tables' rows off `identity.db`. 3b-3 repoints
`packages/lib/auth`'s filters at `argus::clients::auth`. They cannot be
separated: the moment the gateway stops answering `/auth` and proxies it to
`argus-auth`, every filter in the fleet must ask the same authority for the
session verdict, or a token minted by one is unknown to the other.

- **3b-1** (landed) — the service, its schema, the session-verdict RPC, the
  context cache, the identity change consumer, the SDK client and the wire
  contract.
- **3b-2 (this unit)** — the `/auth` surface, the gateway's edge, the rate
  limiter, the row copy.
- **3b-3 (this unit)** — `packages/lib/auth`'s filters on the client.

## What existed before

`packages/identity` hosted the `/auth` surface as `feature/api/auth/` — the
controller, eight DTOs and the `AuthService` that minted sessions, issued device
credentials, ran the pairing handshake and renamed the user — and the gateway
proxied `/auth` to the identity listener on loopback `7042` over TLS. The
refresh-token rate limiter lived in the gateway
(`services/gateway/src/server/refresh-rate-limiter.{hxx,cc}`) as an in-memory
per-process gate over `PATCH /auth/refresh-token`, and its six test cases sat in
`services/gateway/tests/gateway-test.cc`. `packages/lib/auth`'s `JwtFilter`
verified the access token's signature locally and then asked
`argus.identity.v1.ValidateToken` through `details/identity-access.{hxx,cc}`,
and `DeviceFilter`'s credential mode resolved
`X-Argus-Device-Credential` through the same identity RPC.

3b-1 created `argus-auth` with the three session tables, the
`argus.auth.v1.AuthService` verdict RPC (7043) and `packages/clients/auth`, but
left the surface and the filters where they were: the authority existed with no
caller.

## The decision, as built

### The surface moves whole, and the gateway keeps the edge

`services/auth/src/feature/auth/` is the identity `feature/api/auth/` folder
rebuilt in this service's shape: `controllers/auth-controller.{hxx,cc}`,
`dtos/` (the eight request and response DTOs), `services/auth-feature-service.{hxx,cc}`
and `infra/refresh-rate-gate.{hxx,cc}`. The refusals the surface throws are the
catalog rows in `packages/contracts/auth/src/auth/auth-errors.hxx`, which grew
the surface's own definitions (challenge, refresh token, user, JSON body,
`ChangeNotRecorded`) beside the four the gate filters already threw, so both
halves of the boundary throw from one catalog and the contract's own test pins
every row.

The gateway does not lose its edge: it keeps the public listener, the TLS
termination, the `RemoteGate` LAN check and the reverse proxy, and it gains a
proxy route for `/auth` pointed at `argus-auth`'s own listener instead of
identity's, appended FIRST in the route table with `max_segments = 4` so the
longest-prefix match resolves `/auth/...` before any other route can claim it.
Its own `/auth` handlers and their DTOs are deleted rather than left as a second
implementation.

What makes the LAN gate still win is ordering, not a flag: Drogon runs
pre-routing advices in registration order, and the reverse-proxy plugin
registers its own advice while the config is loaded — so `RemoteGate`'s advice
must be registered *before* `loadConfigJson`, and it is. Registered after, the
proxy's advice would win the race and forward `/auth/register` from any peer.

### Identity keeps the enrollment and stops serving sessions

The other half of the old `feature/api/auth/` is the enrollment path, and it
stays in identity because every row it writes is identity's: the face image, the
person, the portrait object and the invitation redemption. It lands as
`packages/identity/src/feature/api/enrollment/` — `EnrollmentRepository` for the
user insert and `EnrollmentFeatureService`, whose `EnrollmentOutcome` carries the
ten ways the old service could refuse (already registered, not paired, face
extraction failed, face already registered, face not recognized, invitation
required, invitation invalid, owner already exists, face index failed) — and it
is served as `RegisterUser` on `argus.identity.v1.IdentityService`: the image,
the name, the invitation token and the language in; the outcome, the
`UserIdentity` and the person id out. `argus-auth`'s `POST /auth/register` is a
thin caller of it through `argus::clients::identity::registerUser`, so the
multipart parse, the validation DSL and the response shape stay with the surface
while the enrollment itself never crosses a database boundary.

`ValidateToken` and `CheckDeviceCredential` leave identity's proto and its RPC
service — the authority is `argus.auth.v1` now — and so do the tables behind
them: identity's schema loses `refresh_token`, `device_login_challenge` and
`device_credential` with their three indexes (40 lines), `identity-migration`'s
table inventory drops the two it copied, and the repositories, schemas and the
`device-credential-test` suite that read them are deleted in the same change.
`PromotePerson`, the one identity RPC that authorized its caller by validating a
token itself, now asks `argus.auth.v1.ValidateToken` through
`argus::clients::auth` (`IdentityRpcService` took a `Dependencies` struct for it),
so identity is a client of the authority rather than a second copy of it.
`IdentifyPersonResponse` gained `last_name`, because the login that used to read
the user row out of identity's own database now builds the same session user from
the RPC answer; the JSON the app reads keeps every key it had, byte for byte.

### The audit trail follows the surface

What a user does to their account — a login, a registration, a device approval,
a logout, a rename, a session revoked by the authority — has to stay in the
server audit history, and the history is written by `services/sync` from a NATS
journal. Auth publishes it the way identity does. `AuthActionSink`
(`src/feature/session/services/`) implements the `AuthChangeSink` contract
declared beside the identity one in
`packages/contracts/sync/src/sync/auth-change-sink.hxx`, and it writes the
`UserActionEvent` into this service's own `change_outbox` row first and then
flushes to `argus.auth.v1.user-action` on the `ARGUS_AUTH_CHANGE` stream: a
worker thread woken by the enqueue, one statement per drained row, the stream
ensured once, a 256 KB payload ceiling, and a `requestStop`/`drained` pair the
service's shutdown drain reads. The sync change feed gains its sixth durable,
`argus-sync-auth-action`, on that stream and subject, routed through the same
`sync_fan_out::handleActionPayload` the identity journal uses, and the journal's
redelivery key is `auth-action:<outbox id>` — the outbox row's id and the
published `msg_id` are the same number, which is what makes a replay a no-op.

The outbox repository is `feature/session/`'s rather than `shared/`: `session`
is its only reader here, so rule 23's 2+ rule has not been earned.

### The limiter is the service's, keyed on what the edge observed

`RefreshRateGate` is the successor of the gateway's limiter, and it is the same
policy with one difference in what it can see. The guarded route is still
exactly `PATCH /auth/refresh-token`; the key is `DeviceFilter::deviceKey(req)`
(the HMAC over user-agent and peer) falling back to `"ip:" + peer`; the window
is a sliding count of `max_requests` per `window_seconds`; consecutive 4xx
responses lock a key out for `lockout_seconds`; a refusal is
`AuthErrors::TooManyAttempts` (429). The key map is bounded at 4096 entries the
way the deleted limiter bounded it — expired entries pruned on demand and, with
none to prune, the new key refused rather than admitted, so a flood cannot
evict a tracked key's own lockout.
`main.cc` wires it as a pre-routing advice (`check`) plus a post-handling advice
(`recordOutcome`), and `AuthConfig::resolveRateLimit()` reads
`rate_limit.{enabled,window_seconds,max_requests,lockout_threshold,lockout_seconds}`
with the same defaults the gateway had (off / 60 / 10 / 5 / 300) and a
`positiveOr` guard on each.

The honest limit is narrower than it was in the gateway, and the correction is
now recorded in both services' `CONTEXT.md`: the state is per `argus-auth`
process and dies with it, and the address half of the key is whatever the
service resolved. Deployed, `device.trust_forwarded_for = true` with the gateway
in `device.trusted_proxy_ips` makes that the client address the gateway
observed — `reverse-proxy.cc` strips any incoming `X-Forwarded-For` and writes
the peer it saw into it — so the key is per client rather than per gateway hop;
natively, with no gateway in front, it is the peer. A client cannot inject an
address either way.

### The filters reach the authority through one cached client

`packages/lib/auth/src/auth/auth-access.{hxx,cc}` is the package's single auth
RPC client: `filterAuthClient()` resolves `auth.target`, else
`auth.rpc_host` + `auth.rpc_port` (7043), plus `auth.rpc_secret`, and caches the
client per `(target, secret)` — the secret is part of the key, so a test that
arms or disarms a fleet secret gets a fresh client without changing the target.
`DeviceFilter`'s credential mode hashes the header and asks
`CheckDeviceCredential`; `JwtFilter` verifies the signature locally and asks
`ValidateToken` for the verdict. Both calls run off the event loop through
`BlockingTask`, and an unreachable authority refuses (401
`AuthenticationRequired`) rather than admitting.

### The rows are copied, and the copy is its own tool

`services/auth/tools/migrate-auth/` holds `auth-migration.{hxx,cc}` (the logic,
as a library) and `migrate-auth.cc` (the `argus-migrate-auth` executable, built
`EXCLUDE_FROM_ALL`: it is a one-shot for the cut-over, not a service). It opens
the source read-only, refuses a source and target that resolve to the same file
and a source without the three tables, applies `database/schema.sql` to the
target, copies `refresh_token`, `device_login_challenge` and `device_credential`
in one `BEGIN IMMEDIATE` transaction with `WHERE NOT EXISTS (id)` so a repeated
run is a no-op, then verifies each table by row count and an FNV-1a checksum
over every column's value and type, ordered by id — the report names the table
that diverged and both checksums.

### One transaction, and the bug the ported suite found

`AuthFeatureService::approveDeviceLogin` writes the device credential, the
session row and the challenge's approval in one transaction. The ported suite
hung on it: `DeviceLoginChallengeRepository::markApproved` was called without a
client and went to the pooled `DbClient` while the transaction held the only
connection, so the await for a free connection never returned — with the
service's configured four connections it would instead have waited on the write
lock the transaction itself held. The identity code this was rebuilt from had no
transaction at all, so the defect was introduced by this unit and not carried
over. Fixed the way the two neighbouring repositories already do it: the input
struct takes `drogon::orm::DbClient* client{nullptr}` and the call passes
`.client = transaction.get()`. The suite's fixture runs a single connection on
purpose — it makes that class of bug a hang rather than a slow failure.

### The suites move with the surface

`services/auth/tests/unit/device-login-test.cc` (2 cases, 57 assertions) is the
identity device-credential suite rebuilt against the new owner: the pinned
fingerprint constants are kept byte for byte, and it drives the real
`DeviceFilter` and `JwtFilter` against an in-process `AuthRpcService` over a
real gRPC channel by pointing `auth.target` at it — both identity modes, the
credential lookup, the device mismatch, the pairing handshake, the one-use poll,
the unreachable authority and the fleet-secret gate.
`services/auth/tests/unit/refresh-rate-gate-test.cc` (6 cases, 59 assertions)
is the gateway's six limiter cases rebuilt on `RefreshRateGate` and
`AuthConfig::resolveRateLimit()`, with the window and lockout rollovers driven
by one-second configs and real 1.1-second waits, since the private `admit` the
old suite called with synthetic timestamps no longer exists.
`services/auth/tests/unit/auth-migration-test.cc` (8 cases, 91 assertions) pins
the copy: an empty target, a target that already holds rows, idempotence, a
tampered target the verification catches, a same-file source, a source without
the tables, and a missing source.

## The review

Three adversarial reviews ran against the unit's tree — one on the ported suites
and the limiter, one on the wiring and the deployment, one on the surface's
protocol and error handling. Every report was treated as a claim: each finding
was reproduced against the source before it was acted on, and two of the fixes
were then pinned by breaking the production code on purpose (below). The three
overlapped on five findings and raised twenty-six distinct ones: fifteen were
confirmed and fixed, two were accepted as deliberate, one turned out to be no
defect at all — the outbox index already matched the drain query's shape — and
eight are flagged, each with what it would take.

**Confirmed and fixed.**

- *The 429 answered without CORS.* `RefreshRateGate::check` is a pre-routing
  advice, and Drogon runs post-handling advices only on the three routing paths,
  so its refusal never reached the CORS advice and a browser would have read a
  cross-origin 429 as a network failure. The gate now applies `Cors::apply`
  itself, the way `RemoteGate` does, and the ported assertion that pins it was
  reinstated. Both reviewers that saw it found it independently.
- *The window's re-arm was unpinned.* The rollover case asserted the refusal
  after the window passed and never that the key is admitted again, so a gate
  that simply cleared the map on expiry passed. Fixed, then negative-controlled:
  with `admit` returning early for a rolled key the new assertion fails.
- *A failure during a lockout extended it.* `recordFailure` re-armed
  `lockedUntil` from the moment of the failure, which made the lockout sliding —
  an attacker who kept failing stayed locked out forever and a legitimate client
  that kept retrying could not get back in on the documented `lockout_seconds`.
  It now returns early while `now < lockedUntil`, and the case pins both halves
  with a 500 ms failure inside a one-second lockout and a 600 ms wait that ends
  it.
- *The challenge approval ignored its compare-and-set.*
  `DeviceLoginChallengeRepository::markApproved` returned whether the update
  matched, and the service threw the answer away — so two devices approving the
  same challenge, or an approval racing an expiry, would both have minted a
  session. The service now throws `ChallengeNotFound` when the CAS matched
  nothing, before the transaction commits. Pinned by a deterministic race: a
  one-shot hook on the scripted identity client mutates the row in the window
  between the status pre-check and the transaction, so the CAS affects zero rows
  and the refusal is the only possible outcome; with the throw disabled the case
  fails at `device-login-test.cc:498`.
- *The device-login status was a raw string.* `status` in the challenge DTO, the
  service and the suite spelled `"pending"`/`"approved"`/`"expired"` by hand,
  which is rule 1's forbidden shape for a column with a `CHECK` constraint. It is
  now `DeviceLoginStatus` in `packages/contracts/auth/src/auth/device-login-status.hxx`
  with its own `deviceLoginStatusToString`/`FromString` pair, used at the
  database and DTO boundaries — and the JSON the app reads is unchanged, because
  the serializer still emits the same three strings.
- *The deploy template had no `[device]`.* `argus-deploy/config.auth.toml.example`
  shipped without the fingerprint secret, so the device hashes this service mints
  would not have been the ones the fleet's filters compute — every authenticated
  request would have refused with a device mismatch. Both the template and
  `scripts/lib/common.sh`'s adoption of it now carry the twelve keys
  (`device.{fingerprint_secret,identity_mode,trust_forwarded_for,trusted_proxy_ips}`,
  `auth.{target,rpc_host,rpc_port,rpc_secret}`,
  `identity.{target,rpc_host,rpc_port,rpc_secret}`), so a config generated before
  this unit is repaired in place instead of 401ing forever.
- *The five peer templates had no `[auth]`.* Camera, guard, notification,
  productivity and sync would each have failed every authenticated request once
  the filters switched to the authority. All five templates now carry the target
  and the shared secret, and provisioning fills them from the generator's shared
  value rather than a fresh random one.
- *7042 was published on every interface.* The limiter and the LAN gate moved
  into `argus-auth`, but the compose still published the HTTP port on `0.0.0.0`,
  which would have handed any reachable host the enrollment path with no
  `RemoteGate` in front of it. Both ports are now bound to `127.0.0.1`, and the
  reason is written down in `argus-deploy/CONTEXT.md`,
  `services/auth/{AGENTS,CONTEXT}.md` and
  `docs/operations/deployment-docker.md`.
- *`[rate_limit]` moved without its configuration.* The keys stayed documented as
  the gateway's in `argus-deploy/CONTEXT.md` and `services/gateway/CONTEXT.md`
  while the code that reads them moved. Both documents and the auth template now
  name `argus-auth` as the owner and the pre-routing gate as the reader.
- *`GatewayErrors::TooManyRemoteAttempts` was dead.* The limiter that threw it is
  gone, and the row, its catalog entry and the prose that counted five refusals
  went with it — the catalog is four, and its suite pins four.
- *The device-login suite's own hazards*: a leaked `REQUIRE_MESSAGE` capture, a
  fixture directory named from a fixed pid, an `identity_mode` dependence on test
  order, and a three-argument helper that rule 2 wants as a struct. All four
  fixed; the helper now takes its parameters in a struct.

**Accepted, with the reason.**

- *The suite's `CHECK(approved)` became `REQUIRE_NOTHROW`* — a throwing CAS is
  now the expected path in one case and the failure path in another, so the
  weaker assertion is the right one, and the new race case covers what it lost.
- *The ported suite no longer cross-checks identity's user row* for the session
  it mints, because rule 27 forbids this service's tests from reading identity's
  database, and the fixture scripts the identity answers instead.

**Flagged** — the Drogon transaction defect, the limiter's key-map saturation
and its local/remote scope, the gateway's case-sensitive restricted-path check,
the `validate_cert = false` hop, the migration tool's rule-25 deviation, the
untracked fixture leftovers and the absent route-level HTTP suite — each in the
next section but one, with what it would take.

## What proves it

**The wire did not move.** The moved controller header diffs against HEAD's
identity one in four lines and no others — the include, `<string>`, the
constructor that takes the identity client and the service member's type. Every
`ADD_METHOD_TO` line and every filter chain beside it is byte-identical, all nine
routes (`/auth/login`, `/auth/register`, `/auth/status`, `/auth/device-login`,
`/auth/device-login/{1}`, `/auth/device-login/{1}/approve`,
`/auth/refresh-token`, `/auth/logout`, `/auth/me`). The eight DTOs diff in
includes and initialises: default member initializers where the old structs had
none, `Json::Int64(...)` around the 64-bit ids, the `DeviceLoginStatus` enum
where the status was a string, and not one JSON key removed or renamed — the
`.cc` files are line-identical except for those casts. The gateway's route table
still names `/auth` first, at `max_segments = 4`, with the longest-prefix match
unable to reach any other route; its own deleted `/auth` handlers leave no second
implementation behind.

**Every refusal is a catalog row.** `auth-errors.hxx` grew nineteen definitions
(invalid multipart, missing challenge id, too many attempts, challenge not
found, challenge expired, user not found, refresh token invalid or expired, login
challenge generation failed, device credential issuance failed, change not
recorded, identity unavailable, server not paired, face extraction failed, face
not recognized, face already registered, invitation required, invitation
invalid or expired, owner already exists, enrolled face index failed) beside the
four the gate filters already threw, and the contract's own suite pins each
row's code, status and message against a table written by hand.

**Two fixes are pinned by breaking them.** With the challenge's CAS refusal
disabled (`if (false && !approved)`) the new race case fails at
`device-login-test.cc:498`; with the window rollover returning early in `admit`
the re-arm assertion fails. Both mutations were reverted and the production
files re-read afterwards, and the suites then pass 100%.

**The copy verifies itself.** `auth-migration-test` seeds a source with rows in
all three tables, and the tool's own verification re-reads the target: row count
per table plus an FNV-1a checksum over every column's value and type ordered by
id. The suite's tampered-target case proves the check bites (it rewrites one
column and expects the mismatch to name the table and both checksums), and
idempotence, a same-file source, a source missing the tables and a missing source
are each their own case — 8 cases, 91 assertions.

**The provisioning was exercised, not assumed.** Fresh native generation was run
into scratch directories: all shared keys land as one value, `auth`'s own
`rpc_secret` stays empty natively, the second run changes nothing, a config with
a mismatched value is repaired, and the file keeps mode 0600. Deploy
provisioning fills all five shared keys, writes `argus-auth:7043` into the peers
and `trusted_proxy_ips = 172.19.0.1` for the gateway, and a config generated
before this unit — one with no `[auth]` section at all — gains the section with
the shared secret rather than a fresh one.

**The journal is pinned at both ends.** `change-feed-consumer-test` now requires
six feeds and checks the sixth's stream, subject and durable by name, then
delivers an `argus.auth.v1.user-action` message and asserts the journal row it
produces carries `auth-action:1` as its redelivery key — the shape that makes a
replay a no-op. `session-verdict-test` holds the verdict's order, the cache, the
fleet gate and the identity change consumer's effects.

## Verification

The unit's own projects were built and tested as it was written, and the
whole-tree gates ran at the close.

**Per project, from scratch, 0 errors and 0 warnings.** `./scripts/build-all.sh
dev --only auth` recompiled the unit's auth translation units on the forced
path, and `--only identity`, `--only gateway` and `--only sync` followed for the
owners whose files the unit touches; every suite passed and not one compiler
warning was printed. The unit's own suites, run as binaries:

| Suite | Cases | Assertions |
|---|---|---|
| `session-verdict-test` | 8 | 79 (92 with `ARGUS_NATS_URL` armed) |
| `device-login-test` | 2 | 57 |
| `refresh-rate-gate-test` | 6 | 59 |
| `auth-migration-test` | 8 | 91 |
| `identity-migration-test` | 6 | 98 |

**The first full run passed every project and failed the tidy ratchet.** It
built and tested all eighteen projects, every suite green, and its
`check-comments` and `check-deps` legs were clean, but `check-tidy` exited 1
with ten checks risen:

```
risen: bugprone-easily-swappable-parameters  54 findings, baseline 52
risen: bugprone-throwing-static-initialization  37 findings, baseline 36
risen: modernize-avoid-c-style-cast  365 findings, baseline 362
risen: modernize-return-braced-init-list  52 findings, baseline 49
risen: modernize-use-designated-initializers  272 findings, baseline 256
risen: modernize-use-emplace  47 findings, baseline 46
risen: modernize-use-scoped-lock  285 findings, baseline 283
risen: modernize-use-starts-ends-with  46 findings, baseline 45
risen: performance-inefficient-string-concatenation  19 findings, baseline 17
risen: performance-move-const-arg  15 findings, baseline 14
```

The rises are net of the findings the unit's own deletions took away, so they
were resolved to lines rather than counted: a targeted `clang-tidy` pass over
the 46 translation units that carry the unit's files, deduplicated to 118
findings, of which the 107 in files the unit touches were filtered to the lines
it added (`git diff HEAD -U0` hunk ranges, and the whole of every new file).
Every site that survived the filter is fixed — **48 findings**, a number the
re-recorded floor derives exactly (each risen count minus what the check reads
after the fixes: 25 designated initializers, 6 c-style casts, 6 scoped locks, 3
braced returns, 2 swappable pairs, 2 concatenations, 1 throwing static
initialization, 1 `emplace`, 1 `starts_with`, 1 no-op move):

- **The lock spellings** — `std::lock_guard<std::mutex>` became
  `std::scoped_lock` in the filters' cached auth client, the limiter's three
  methods and `AuthFeatureService`'s two pending-secret helpers.
- **The functional casts** — `Json::Int64(x)` became
  `static_cast<Json::Int64>(x)` in the `/auth/status` body, four response DTOs
  and the `AuthContextChanged` payload.
- **The catalog table and two structs** — the auth contract's twenty-three
  pinned rows, `PendingDeviceSecret` in the service and `Refusal` in the
  device-login suite are written as designated initializers.
- **One no-op move** — the pre-routing advice's `cb(std::move(resp))`, whose
  `AdviceCallback` takes a `const HttpResponsePtr&`, became `cb(resp)`.
- **Five in the migration tool** — the table list became a
  `constexpr std::array<std::string_view, 3>` (constant-initialized, so it no
  longer allocates at load), `rfind("--", 0) == 0` became `starts_with`, the
  copy statement builds with `+=` instead of four chained `operator+`, and
  `validateDistinctPaths` takes an input struct instead of two
  `const std::string&`.
- **Five in its two suites** — `auth-migration-test` returns braced lists (3)
  and emplaces into its row vector (1), and `refresh-rate-gate-test`'s
  `writeConfig` takes an input struct instead of two bare strings (1).

**The cleanup's own compiles surfaced a build warning, which is fixed.** A
fresh compile of `auth-migration-test.cc` printed `<command-line>: warning:
'ARGUS_AUTH_SCHEMA_PATH' redefined`: the migration library declares the schema
path `PUBLIC`, so the test that also defined it saw two `-D`s whose values
differ only in their `../` spelling. The test's own define is deleted and it
inherits the tool's absolute default, exactly as `identity-migration-test`
inherits `ARGUS_IDENTITY_SCHEMA_PATH` from its own tool; the recompile is
clean.

**The closing full run, `./scripts/build-all.sh dev`, exit 0.** It built and
tested all eighteen projects — cert 2, sqlite 2, identity 30, memory 23, intent
4, auth 26, gateway 35, sync 51, camera 56, productivity 40, notification 45,
guard 58, tts 25, stt 8, vlm 9, llm 37, voice 27, tunnel 12 — every suite
`100% tests passed`, **490 test executables**, and its output carries no
`warning:` line anywhere. One rerun was needed and it was not this unit's: the
attempt before it aborted in `tts-wire-test` on
`trantor/net/Channel.cc:43: void trantor::Channel::remove(): Assertion
'events_ == kNoneEvent' failed`, an assertion inside trantor's event loop,
raised from a suite this unit does not touch and not reproduced — `--only tts`
passed 25/25 immediately after and the closing run passed it again. Its gates:

- `check-comments: 1310 files checked, 0 comments`
- `check-deps: 71 declarations, 582 edges, 0 forbidden, 0 cycles, 0 unresolved,
  50 edges deferred to phase 3 (248 third-party mentions over 22 roots)`
- `check-tidy: 529 TUs, 2957 findings over 45 checks, baseline 3030; 10 checks
  below it` — exit 0, nothing risen.

**The floor is re-recorded on the verified tree, and it moved down.** Seven of
the ten risen checks returned exactly to their recorded floor, which is what
makes the 48 above a measurement rather than a count; three fell below it
because the unit's deletions took older findings of the same checks with them
(`modernize-avoid-c-style-cast` 365 → 359,
`modernize-use-designated-initializers` 272 → 247,
`modernize-use-scoped-lock` 285 → 279). Seven further checks fell with those
deletions — `modernize-use-nodiscard` 749 → 730,
`bugprone-suspicious-stringview-data-usage` 292 → 275,
`bugprone-unchecked-optional-access` 223 → 209,
`bugprone-narrowing-conversions` 66 → 63, `bugprone-empty-catch` 11 → 9,
`modernize-use-auto` 55 → 54,
`performance-unnecessary-copy-initialization` 23 → 22 — so the tree reads
**2957 findings over 45 checks** where the floor recorded 3030, the difference
being exactly those ten deltas (−73). `scripts/check-tidy.sh --write-baseline`
re-recorded the floor at `529 TUs` (was 524: the unit adds translation units
and the gate only fails when it sees fewer).

## Flagged, not fixed

- **The limiter counts local attempts too.** One review read it as a remote-only
  guard, since it arrived with the LAN gate. The intent is the opposite: every
  attempt on the guarded route is counted, local ones included, because a
  compromised client on the LAN is exactly the case the lockout exists for.
  Recorded as intent rather than changed, so a later reader does not "fix" it
  into a remote-only check.
- **The migration tool hand-writes its include directories** instead of going
  through a module helper, which is a rule-25 deviation `check-deps` cannot see
  as an edge. The two sibling `tools/migrate-*` do the same and the tool is a
  one-shot binary rather than a library, so it is recorded; folding the three
  tools into one shape is a build-infrastructure decision.
- **Drogon's transaction commit is not rollback-safe, and five services share
  the shape.** `TransactionImpl::~TransactionImpl()` issues the COMMIT and, when
  it fails, calls only `commitCb(false)`: it never rolls back and never marks the
  transaction as finished, and the connection goes back to the pool with the
  failed transaction still open, so every later statement on that connection
  fails. Two of this unit's paths also call `db_transaction::rollback` after
  `db_transaction::Commit(std::move(transaction))`, which moves the
  `shared_ptr` — the rollback on the moved-from handle is a silent no-op. The
  fix belongs to the transaction wrapper in `packages/lib/sqlite` and not to any
  one caller: identity, camera, productivity and notification write transactions
  the same way, and `Transaction::commit()` is not in Drogon's public API, so
  the wrapper is the only place the commit can be made to fail loudly. Recorded
  here rather than changed, because a change to the shared wrapper is that
  package's own unit of work and its own run of the gate.
- **The limiter's key map saturates instead of evicting.** At 4096 tracked keys
  with nothing expired, a *new* key is refused rather than admitted — deliberate
  and ported (the deleted gateway limiter did the same, so a flood can never
  evict a tracked key's lockout), but a client rotating user agents can push
  other clients out. `rate_limit.enabled` ships false, so it does not bite
  today; the honest fix is an eviction policy that never drops a locked key,
  which is a policy decision rather than a port.
- **`RemoteGate`'s path comparison is case-sensitive.** A pre-routing advice
  sees the raw request path while Drogon's router matches case-insensitively,
  so `/AUTH/register` reaches the route without passing the gate. This is
  pre-existing gateway behaviour and not this unit's — the auth limiter's own
  predicate is case-insensitive for exactly this reason, and pins it with
  `/auth/Refresh-Token` — and it dies with the gateway in Phase 3d.
- **`validate_cert = false` on the gateway's `/auth` route.** The loopback hop
  dials the instance certificate without validating it. The knob is per route
  now (this unit added it), so turning it on is a config change once the
  gateway's trust store carries the instance CA; the route is the same shape
  the identity listener had before it.
- **The migration suite leaves an empty directory per run.**
  `/tmp/argus-auth-migration-test-<pid>/` is created and never removed — six had
  accumulated by the time this unit's run finished. Inert, and the sibling
  suites' fixtures have the same habit, but a test that cleans up after itself
  is one line.
- **No route-level HTTP suite.** The nine routes are covered through their DTOs
  and the service, and the two filters are driven end to end, but nothing boots
  the controller over an HTTP listener and asserts the routes' status codes,
  headers and CORS behaviour. The gateway's e2e suite would be the place for it,
  and it is being deleted in Phase 3d; the successor is a decision for the
  frontend/backend contract tests.
- **`/auth/has-admin`.** The shipped app calls it; no route answers it, in
  identity before this unit or in `argus-auth` now. Pre-existing, and a
  frontend/backend contract question rather than a defect in the move.
- **A 10 MB image against a 4 MB transport.** The DTOs cap the enrollment and
  login images at 10 MB while gRPC's default receive limit is 4 MB, so the
  largest accepted payload would be refused by the transport before the DTO's
  own validation sees it. Nothing in the app sends anything close; the limits
  should agree before something does.
- **`PATCH /auth/me` with an empty name is now a silent no-op.** The identity
  service wrote the empty name; the ported one returns before calling identity,
  so the account's name can no longer be cleared and the client sees 200 instead
  of a write. The app's rename flow never sends an empty name, and the narrowing
  is recorded rather than restored.
- **The deploy documentation's service table** still has no rows for `rustfs`,
  `argus-sync` and `argus-guard`, so its inventory is complete only for the
  services this unit touched. A documentation gap, carried from the earlier
  phases.

## Files

163 paths, of which 111 are modifications or deletions and 52 are new. The one
untracked file at the repository root that is *not* part of this unit —
`go2rtc.yaml`, which carries RTSP credentials — stays untracked and unstaged.

- **`services/auth` (59)** — the new `src/feature/auth/` (controller, the eight
  DTO pairs, `AuthFeatureService`, `RefreshRateGate`), the device-login-challenge
  repository and schema, the change-outbox repository and `AuthActionSink`,
  `tools/migrate-auth/` (the migration library and the
  `argus-migrate-auth` executable), the three new suites, and the edits to
  `main.cc`, `AuthConfig`, the existing repositories' client parameter, the
  schema, the template, `CMakeLists.txt` and both documents.
- **`packages/identity` (51)** — the new `feature/api/enrollment/`, the deleted
  `feature/api/auth/` (controller, sixteen DTO files, the `AuthService`), the
  deleted repositories and schemas for the three tables, the deleted
  `device-credential-test.cc`, and the edits to the proto, `identity-rpc`, the
  user feature service, the schema, the migration tool and its suite.
- **`services/gateway` (14)** — the `/auth` proxy route and its `validate_cert`
  knob, the `RemoteGate` and `ProxyConfig` edits, the deleted limiter (2 files),
  the trimmed suite, `main.cc`, the template and `CONTEXT.md`.
- **`packages/contracts` (11)** — nineteen rows and the enum in the auth
  contract, its two suites, `CMakeLists.txt` and `AGENTS.md` (twenty
  definitions → twenty-three); `AuthChangeSink` and the subject constants in
  the sync contract; the four-refusal gateway catalog, its pinned table and
  `AGENTS.md`; the identity proto.
- **`packages/lib` (8)** — `auth-access.{hxx,cc}` (the cached auth client), the
  `DeviceFilter` and `JwtFilter` repoint, `CMakeLists.txt`, `AGENTS.md`, and the
  two new NATS subject constants.
- **`packages/clients` (2)** and **`services/sync` (2)** — the identity client's
  `registerUser` in place of `validateToken`/`checkDeviceCredential`, and the
  change feed's sixth durable with its suite.
- **`argus-deploy` (9)** and **`docs/operations` (1)** — the seven templates
  (`config.auth.toml.example` new, six peers gained `[auth]`), the compose
  loopback binds and `CONTEXT.md`, plus the deployment document.
- **`scripts` (3)** — `lib/common.sh`'s key adoption and sharing, `setup.sh`'s
  wiring pass, and `lib/tidy-baseline.txt` re-recorded on the verified tree.
- **`docs/history` (2)** — this report and the architecture plan's Phase 3b
  entry.
- **`services/productivity` (1)** — one line in the controller suite, which
  builds `IdentityRpcService` in-process and now passes its `Dependencies`
  struct.
