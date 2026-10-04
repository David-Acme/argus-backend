# argus-auth

The session and device authority of a single Argus installation. It owns
`auth.db` — refresh tokens, device credentials and cross-device login
challenges — and it answers the two questions the other services' filters ask
on every authenticated request: *is this access token a live session?* and *is
this device credential still active?* It never reads another owner's database:
the user row behind a session comes from identity through
`argus::clients::identity`.

## What it does

- `ValidateToken` (`argus.auth.v1.AuthService`) answers the session verdict:
  the access token is verified with `jwt.secret`, the subject is resolved to a
  user context, the `refresh_token` row must exist, be valid, unrotated and
  unexpired, and — when the caller sends a device hash — it must also carry
  that `device_hash`. The row is required on both paths; the device context
  only decides whether the hash is compared, because it is the transport that
  enforced the binding.
- `CheckDeviceCredential` answers whether a device credential is active, looked
  up by the SHA-256 the caller already holds. The plaintext secret never
  crosses the wire.
- Refresh tokens are single-use and rotated; `revokeUser` invalidates every
  session of a user in one statement, which is what a deactivated account gets.
- `refresh_token` keeps the SHA-256 of both tokens, never the tokens: a copy
  of `auth.db` hands out no session. Lookups hash what the caller presents
  and also accept the presented value as stored, for rows written before the
  change; those expire with their refresh window, so no session is cut and
  no migration rewrites the table. The cross-device challenge keeps its
  tokens in clear only until the waiting device claims them.
- A refresh is refused unless it comes from the agent the session was issued
  to (a missing `User-Agent` is a mismatch, not a skipped check) and, in
  `credential` identity mode, from the same device hash; the one exception is
  the move to the stable agent described under Sessions. A stolen refresh
  token without its device credential used to mint a session bound to the
  thief's hash. In `ip` mode the hash is not compared: it carries the IP,
  and the refresh is exactly how a phone that changed network gets a session
  for its new address.
- The user context (name, last name, language, role, `isActive`) is resolved
  through identity and cached for `[auth] context_cache_seconds`.
- A durable JetStream consumer on the identity change subject drops a cached
  context the moment its `user` row changes, and revokes every session of a
  user that arrives disabled.
- `GET /health` reports the NATS leg when the bus is configured. The `/auth`
  HTTP surface (login, pairing, QR, refresh, logout) is this service's own
  subroute on port 7042 since Phase 3b-2, served over the RPC authority
  underneath it.

## Sessions (2026-10)

A session is one refresh-token family: every row a rotation writes carries
the same `session_id` (32 hex characters, 128 random bits), minted at login,
registration or the approval of a cross-device login and never shown with a
token, a hash or a row id. The family also carries what the sessions screen
shows: `platform` (`SessionPlatform`, `contracts/auth`, CHECK-constrained),
`device_name`, `session_created_at` and `last_seen_at`. The family is active
while its newest row is valid, unrotated and unexpired, so the list is one
query over `(user_id, session_id)` and needs no table of its own.

**Additive, at boot.** `refresh_token` and `device_login_challenge` gain their
columns with `ALTER TABLE ... ADD COLUMN` in the beginning advice, before the
schema file runs (the same guarded shape the change outbox uses), and every
row without a session id receives one from `lower(hex(randomblob(16)))` in the
same pass, so a database from before the change keeps every session. A row
written later without one (a tool, an older binary during a rollback) is
adopted the first time the verdict or a refresh meets it. The migration tool
that copies the legacy tables out of `identity.db` now copies the source's
own columns and refuses only a source column the target lacks; it used to
read both column lists from the target (`"src".pragma_table_info` resolves
to `main`), so its shape check compared the target with itself.

**Platform and name come from the device that holds the session.** The app
sends `User-Agent: Argus/1 (<platform>)`, `X-Argus-Client: <platform>/<version>`
and an optional percent-encoded `X-Argus-Device`. The platform is read from
`X-Argus-Client` first, then from the stable agent, else it is `unknown`; the
name is decoded, stripped of control characters, cut at 64 characters and
dropped whole when it is not UTF-8. For a QR login the poller's headers are
stored on the challenge, so the session is labelled with the device that ends
up holding it, not the approver. A refresh refreshes both and keeps the old
value when the request carries none.

**Last seen.** The verdict advances `last_seen_at` when the session was last
seen a minute or more ago, with a conditional UPDATE that cannot write twice
in that minute; a refresh writes it on the new row.

**Revocation is a write plus two frames, in one transaction.** Revoking one
session, the others, all of them, or logging out (now only the caller's own
session) invalidates the family rows, then queues, through the change outbox
and in the same transaction, a `disconnect_session` change for every revoked
session and one `sessionsChanged` user emit, on `argus.auth.v1.session`
(stream `ARGUS_AUTH_SESSION`). argus-sync consumes that subject and closes
only the sockets tagged with that session id, after sending them
`{"operation":7,"info":{"reason":"sessionRevoked","sessionId":...}}`. The
imperative control RPC that logout used before is gone from this service: it
disconnected every socket of the user with an `isActive:false` context, which
the current app reads as a deactivation and answers by signing out of every
device. Each revoked session is audited as a journal row (`user`, `delete`)
whose data is the session id, the platform, the scope, the reason and who
revoked it; no token, hash or agent. Without a broker nothing is queued: the
rows are still invalid, so every service refuses the token, but no socket is
told.

**A revoked access token is refused at once.** No verdict is cached: the
context cache holds the user context only, and the session row is read on
every validation, by every service's `JwtFilter`, so the next request after
the commit is refused. The tests measure it under a second, on the
`JwtFilter` path every service runs.

**Refresh-token reuse.** A refresh token now carries a `sid` claim, and every
rotated row keeps the hash of the token it replaced. A presented token that is
not the family's current one is a stale token of that family: when it is the
immediate predecessor, was rotated no more than
`[auth] refresh_reuse_grace_seconds` ago (30) and comes with the same binding
(the same agent and, in credential mode, the same device hash), it is two
requests racing the same rotation and gets a plain 401; anything else revokes
the session with reason `refreshTokenReuse`. A token issued before the claim
existed is found by its hash, or as the predecessor of an active row, so the
first rotation after the upgrade is covered as well.

**From a legacy agent to the stable one, once.** A session is bound to the
exact agent that opened it, and the device hash carries that agent in both
identity modes, so the app's switch to `Argus/1 (<platform>)` would have
signed every device out. A refresh accepts the switch once per session, from
an agent that is not of the stable form to one that is, when the device proves
itself: in credential mode the presented device credential must be the one the
session is bound to (`HMAC(old agent | sha256(credential))` equals the stored
hash), in ip mode the request must come from the address the session was last
bound to (`HMAC(old agent | address)` equals it). The new rows carry the
stable agent; any later change is refused as before. In ip mode the proof is
the address, which a household shares; that is no weaker than the binding it
replaces, an agent string that is no secret, and a phone that changes network
in the same instant signs in once more. Every request in between gets a
`Device mismatch` 401, which the app answers with one refresh.

**Who forwards.** `X-Forwarded-For` counts only when the immediate peer is in
`[device] trusted_proxy_ips` (exact addresses or CIDRs) and
`trust_forwarded_for` is set; loopback is no longer trusted implicitly. No
first-party component writes the header (the tunnel carries the app's TLS
unopened), so every template ships an empty list. `AuthRateGate` keeps its
per-peer ceiling either way.

## Ownership and the three features

`session` owns everything about what a validated token means: the
`refresh_token` repository and mapping, `SessionService`, the context cache and
the identity change consumer. `device` owns the device credential — its
repository and mapping — because the lookup is a different question with a
different table, and nothing in `session` needs the other's tables. `auth`
owns the HTTP surface, its DTOs and the rate gate that guards it.

Cross-device login: the approved tokens are handed only to the device
that created the challenge (the poll's device hash must equal the
creator's; any other poller keeps reading `pending`), a challenge past its
expiry answers `expired` even after it was approved, and creating a
challenge first deletes the expired ones. Before, whoever polled first -
anyone who saw the QR - received the approver's tokens, and an approved
challenge that nobody polled kept live tokens in `auth.db` forever.

The device hash did not hold in `credential` mode: a device that has no
credential yet (every desktop showing the QR) is hashed as the empty string,
so any poller without a credential matched it (2026-10). The challenge is now
bound to `DeviceFilter::deviceKey` (HMAC of agent and address) in both modes,
and above that to a proof only the creating device knows: `POST
/auth/device-login` may carry `pollHash`, the SHA-256 of a random proof the
app keeps in memory, and the poll then presents the proof in
`X-Argus-Login-Proof`; when a challenge has a poll hash, nothing else unlocks
it. The hash lives in memory beside the pending device secrets, so a restart
falls back to the device key. Handing out the tokens is a single conditional
UPDATE (`approved` to `expired`), so two pollers can never both receive them.

A refresh rotates in one transaction: marking the presented token used,
pruning the user's stale rows and inserting the new pair commit together, so
a failed insert no longer leaves a used token and no session.

`AuthRateGate` (`[rate_limit]`, on unless the key says otherwise) limits
the unauthenticated entry points - `POST /auth/login`, `POST
/auth/register`, `POST /auth/device-login` - and `PATCH
/auth/refresh-token`, per route and per client address (the address
`DeviceFilter::resolveIp` trusts), with a lockout after consecutive
refusals. It used to guard the refresh alone, disabled by default and keyed
by user agent and address, so a face login could be retried without limit
and a rotated `User-Agent` skipped any limit; and once 4096 keys were
tracked it refused everyone. A full table now evicts an entry that is not
locked out. A household behind one address shares the budget (ten
requests per route per minute by default). When the address comes from a
trusted `X-Forwarded-For`, the socket's own peer address carries a second,
twenty-times larger budget, so rotating the header no longer buys unlimited
attempts. A 5xx answer (identity down) counts neither as a success nor as a
failure: an outage no longer locks the household out.

No feature reads another's repository, so rule 23's 2+ rule puts each of the
three in its own feature and keeps `src/shared/` empty.

## Wiring decisions

**Schema at boot, not migrations.** `main.cc` applies
`services/auth/database/schema.sql` in a beginning advice before the listeners
answer, then `DbService::applyPragmas()`. The file is idempotent (`CREATE TABLE
IF NOT EXISTS`), so a fresh install and a restart take the same path. The three
session tables are the ones `identity.db` served before Phase 3b-2 copied them
here with the `/auth` surface — same columns, same CHECK constraints, so the
split was a copy rather than a translation.

**The context cache is keyed by user and dropped by event.** A role change, a
rename or a deactivation must reach every service's next request; a TTL alone
would leave a window of up to 30 seconds and a revocation that depends on the
clock is not a revocation. The TTL bounds how long identity is trusted to be
reachable without asking again — it is a load valve, not the invalidation
mechanism. `[auth] context_cache_seconds = 0` disables the cache entirely,
which is what the verdict tests run with when they count identity calls, and a
disabled cache stores nothing at all rather than storing an entry that has
already expired. A fetch carries the cache generation it started in and only
writes its entry if that generation is still current, so a change that lands
while identity is being asked cannot be overwritten by the answer to the older
question.

**The change consumer is tolerant of a cold broker.** `start()` subscribes
once; if the stream is not there yet it retries every 5 s from a Drogon timer
instead of failing the boot. `main.cc` logs a warning and keeps serving when
the broker is unreachable — a session that was validated before the outage
keeps working for its TTL, and every validation afterwards asks identity
directly.

**One durable handler shape.** `ordered_delivery::handler` is the only place a
durable message becomes a coroutine: it copies the payload and the settlement
out of the cnats callback, hops onto IOLoop 0 with `runInLoop`, runs the body
through `async_run`, and acks on success or naks on a thrown exception. Every
consumer this service grows reuses it rather than open-coding the marshal.

**A user change is two effects, in one order.** The cache entry is dropped
first, so a concurrent request cannot re-populate it with the row that is being
replaced; then, if the row arrives with `isActive` false, the user's sessions
are revoked. A revocation that has nothing left to invalidate is the normal
case for a replayed message and is logged at debug level, not as a failure: the
statement itself succeeds either way, and a statement that throws naks the
message instead of acking a change that never landed. The consumer registers
itself with `shutdown_signal::onStop` and reports drained when no handler body
is in flight, so the database freeze that follows cannot run underneath a
revocation.

The action sink's drain wakes at the commit: `enqueue` runs inside the
caller's transaction, so its `db_transaction::CommitObserver` wakes the drain
when the commit lands, and its `WakeSignal` keeps a wake that arrives
mid-pass, instead of a wake before the commit that found nothing and left the
row waiting the retry period.

**The RPC listener is a fleet secret, not a route.** `[auth] rpc_secret` must
be the same value in every service's config. An empty value is legal only
while the listener is bound to loopback, which is the native development
default; `main.cc` refuses to start when the listener is reachable beyond
loopback without one. The deploy template binds `0.0.0.0` behind that secret —
which is what lets the peer containers the secret exists for reach
`argus-auth:7043` — and the compose publishes 7043 on `127.0.0.1` only.

**Ports.** 7042 is this service's HTTP subroute (TLS with the instance
certificate), 7043 its RPC listener. The compose publishes 7042 on the LAN,
where the app dials `/auth` directly, and 7043 on `127.0.0.1` only, where the
fleet reaches it as `argus-auth:7043` over the bridge. `RemoteGate` sits in
front of the whole surface as a pre-routing advice: a request that arrives on
the `[remote] tunnel_port` listener is refused `REMOTE_NOT_ALLOWED` for
`/pairing` and `/auth/register` unless `[remote] enabled` is set.

## The verdict order

`SessionService::validate` refuses in the order the identity gate did, and the
order is load-bearing: an access token that does not verify is refused without
touching the database, a disabled account is refused before the session row is
read, and the device hash is compared last, so a valid token from another
device cannot learn whether the session exists. `reason` is set only for the
cases the old `JwtFilter` surfaced to the client (`Token expired`, `Device
mismatch`, `User account is disabled`); everything else refuses with an empty
reason, because the caller has no business knowing which check failed.

An access-only validation (no device context) answers `valid` with
`expires_at = 0`: it is the question the WebSocket upgrade asks, where the
device binding was already enforced by the transport. The session row is still
required, so a token whose row was revoked or rotated refuses there too — the
difference between the two paths is the hash comparison, never whether the
session exists.

## Test harness

`tests/unit/session-verdict-test.cc` covers the verdict order, the cache, the
fleet gate, the device credential lookup, the refusal of a revoked or rotated
session row on both validation paths, the effect a revocation has on the rows
themselves, and — under `ARGUS_NATS_URL` — the live consumer, over a real gRPC
server and a real `AuthClient` against a temporary database.

`tests/unit/session-management-test.cc` boots on a database with the old
`refresh_token` and `device_login_challenge` shapes and a live session in it,
runs the boot migration, and then drives the sessions end to end through the
real filters: the legacy session keeps validating and gets an id; sessions
opened by QR carry the poller's platform and name; one revocation, the others,
all, and a logout each end exactly the sessions they name, and the revoked
access token is refused by `JwtFilter` in under a second; the frames and audit
rows a recording sink captures carry no token, hash or agent; a rotated
refresh token inside the window is a race and outside it, or from another
agent, revokes the family; the legacy agent moves to the stable one once, only
from its own address or credential; `last_seen_at` moves at most once a
minute.

The suite quits Drogon from `main()` **after** `doctest::Context::run()`
instead of letting the exit-time static destructors do it. Measured: a binary
that boots Drogon with a SQLite `DbClient`, connects a `NatsBus`, drains it and
then lets Drogon's teardown run during static destruction corrupts the heap on
a `DrogonIoLoop` thread (`corrupted double-linked list`, SIGABRT, exit 134) —
with or without gRPC, subscriptions, messages or the consumer in the path, and
with the bus leaked rather than destroyed. The same binary that quits Drogon
during normal execution is clean on every run. This matches production, where
`shutdown_signal::onStop` drains and then holds `quit()` until the drains
report, and it is why this suite has its own `main()` while the sibling suites
(the repo's `SharedBoot` idiom) keep the exit-time quit — none of them connects
NATS.

`ARGUS_NATS_URL` (e.g. `nats://127.0.0.1:4222`) arms the live leg; without it
that case prints a message and passes. The suite names its stream, subject and
durable after its own pid, so it never touches a deployment stream. The stream
it creates is not reclaimed: `maxAge` bounds the messages, not the stream.

## Build

```bash
./scripts/build-all.sh dev --only auth
```

## Open items

None. Phase 3b-2 brought the `/auth` surface and the row migration off
`identity.db` — the three session tables are this service's schema now — and
Phase 3b-3 landed the filter leg: `JwtFilter` asks the verdict through
`argus::clients::auth` (`packages/lib/auth/src/auth/jwt-filter.cc:50-54`), so
no other unit opens `auth.db`.
