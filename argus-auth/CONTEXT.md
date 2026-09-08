# argus-auth — context

## Origin

Extracted in f7-4 from `src/filter/` plus `src/shared/services/jwt/`. A
pure move: 13 files, zero content edits. The include prefixes (`filter/…`,
`shared/services/jwt/…`) were preserved inside `src/`, so not one of the
`#include` lines in the fleet changed.

The extraction had to wait for f7-3. Until the auth RPC landed, the
filters read identity's repositories directly, so the package would have
depended on `argus-identity` while `argus-identity` depends on this
package for its own routes — a genuine cycle. It is last in the module
sequence for that reason.

## Why it depends on argus-contracts, not argus-identity

JwtFilter verifies the token SIGNATURE locally (cheap, no I/O) and then
calls `argus.identity.v1.ValidateToken`, which is authoritative for
everything stateful: the user row and its status, the refresh-token
session, its expiry and its device binding. DeviceFilter calls
`CheckDeviceCredential` in credential mode. So the only heavy edge is
`argus::sdk-identity` — a generated contract, not a domain library.

Two consequences worth keeping in mind:

- The package carries no AI or database closure. `argus-tts` links it and
  stays ncnn-free; that is a standing gate (`nm -C | grep -c 'ncnn::'`).
- An unreachable identity service means every authenticated request 401s.
  That is deliberate — failing closed — and it is why the client resolves
  a target rather than silently falling back to a local database.

## The identity target

`filterIdentityClient()` resolves `identity.target`, and when that is
empty falls back to `identity.rpc_host` / `identity.rpc_port` (default
`127.0.0.1:7040`). The fallback is what lets the gateway — which HOSTS the
listener — need no target key of its own, while every other service names
one explicitly. The client is cached per resolved target (the
`voice-engine-seam` precedent), so a config change picks up a new client
and tests can point the chain at a dead port to prove fail-closed.

The call itself rides `BlockingTask`, which moves the blocking stub call
off the event loop — the `camera-sync-source` pattern. It costs a detached
thread per validation; the SDK channel base (step 5) is where that should
become an async stub over the CQ bridge.

## Device context: presence is not emptiness

`DeviceFilter` always inserts a `DeviceContext`; in credential mode the
hash is EMPTY when the credential is missing, oversized or unknown. The
session check must therefore key on the context being PRESENT, not on the
hash being non-empty — otherwise a credential-less request passes a
device-bound session. f7-3 shipped that bug briefly and the
device-credential suite caught it; the contract now carries presence
(proto field presence) separately from the value.

## What does NOT live here

Anything stateful. No repositories, no schemas, no `DbService`. The
identity domain (users, sessions, device credentials) is
`argus-identity/`; the RPC surface that serves this package is
`argus-identity/src/feature/rpc/`.
