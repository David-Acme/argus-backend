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
- A refresh is refused unless it comes from the agent the session was issued
  to (a missing `User-Agent` is a mismatch, not a skipped check) and, in
  `credential` identity mode, from the same device hash. A stolen refresh
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

## Ownership and the three features

`session` owns everything about what a validated token means: the
`refresh_token` repository and mapping, `SessionService`, the context cache and
the identity change consumer. `device` owns the device credential — its
repository and mapping — because the lookup is a different question with a
different table, and nothing in `session` needs the other's tables. `auth`
owns the HTTP surface, its DTOs and the refresh limiter that guards it.

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
