# argus-auth — context

## Origin

Extracted in f7-4 from the filters and the JWT service. A pure move: 13
files, zero content edits — the include prefixes of the day (`filter/…`,
`shared/services/jwt/…`) were preserved inside `src/`, so not one of the
`#include` lines in the fleet changed then. Phase 2 step 2 dropped those
prefixes: the package is flat (`src/auth/<file>`, the private
`identity-access.hxx` under `src/auth/details/`) and every consumer's include
was repointed (`<auth/jwt-filter.hxx>`).

The extraction had to wait for f7-3. Until the auth RPC landed, the
filters read identity's repositories directly, so the package would have
depended on `argus-identity` while `argus-identity` depends on this
package for its own routes — a genuine cycle. It is last in the module
sequence for that reason.

## Why it depends on two wires and on no service

JwtFilter verifies the token SIGNATURE locally (cheap, no I/O) and then asks
`argus.auth.v1.ValidateToken`, which is authoritative for everything stateful:
the user row and its status, the refresh-token session, its expiry and its
device binding. DeviceFilter calls `CheckDeviceCredential` on the same leg in
credential mode. The user directory is the second, read-only edge
(`argus.identity.v1.GetUser`, behind `filterIdentityClient()`). So the
package's heavy edges are `argus::clients::auth` and
`argus::clients::identity` — generated contracts, not domain libraries, and
neither of them a database.

The verdict moved off the identity listener in 3b: before that, both filters
rode the identity client and `services/auth` did not exist. It owns the
session tables and answers the verdict now.

Two consequences worth keeping in mind:

- The package carries no AI or database closure. `argus-tts` links it and
  stays ncnn-free; that is a standing gate (`nm -C | grep -c 'ncnn::'`).
- An unreachable `argus-auth` fails closed: no authenticated request is
  admitted, and the client resolves a target rather than silently falling
  back to a local database. The refusal is 503 `AuthUnavailable`, not 401: a
  401 tells the app its token is bad, and the app answers it by refreshing
  and then clearing the session and its local projection, so restarting the
  auth container used to sign every user out. Only a verdict that says
  invalid is a 401. A missing verdict covers every RPC failure, a wrong fleet
  secret included, because none of them is the user's fault.
- In `credential` identity mode a failed `CheckDeviceCredential` RPC is the
  same outage: `AuthClient::checkDeviceCredential` answers `nullopt` for a
  transport failure (and `false` only for a credential auth says is not
  active), and `DeviceFilter` refuses with 503 `AuthUnavailable` instead of
  continuing with an empty device hash that ended in a device-mismatch 401.
- With `trust_forwarded_for`, the client address is the right-most
  `X-Forwarded-For` hop that is not itself a trusted proxy - the address the
  nearest trusted proxy actually saw. The left-most entry is whatever the
  client chose to send, and in `ip` mode the device hash is built from it,
  so a stolen token could be replayed with the victim's address claimed.
- The header is read at all only when the immediate peer is in
  `device.trusted_proxy_ips`, a comma-separated list of exact addresses or
  CIDRs (`details/proxy-allowlist`, IPv4, IPv6 and IPv4-mapped peers), and
  the list is empty by default. Loopback used to be trusted implicitly, which
  let anything that reached a service through a local port (the tunnel client
  dials the remote listener on loopback) choose its own address. No
  first-party component writes the header, so the deploy templates leave the
  list empty and `trust_forwarded_for` false.
- `JwtContext.sessionId` is the session (refresh-token family) the verdict
  names; argus-sync tags each socket with it, and argus-auth uses it as the
  caller's current session. `role_access::kSessionAccess` opens
  `GET /auth/sessions`, `DELETE /auth/sessions` and
  `DELETE /auth/sessions/{id}` to every role, route by route like
  `kGuardAccess`, without widening `kAuthAccess`: the service scopes every
  query to the caller's own sessions.

## The two targets

`filterAuthClient()` resolves `auth.target` (`auth.rpc_host` /
`auth.rpc_port` as the fallback, default `127.0.0.1:7043`) and
`filterIdentityClient()` resolves `identity.target` the same way
(`identity.rpc_host` / `identity.rpc_port`, default `127.0.0.1:7040`). Each
client is cached per resolved target (the `voice-engine-seam` precedent), so a
config change picks up a new client and tests can point the chain at a dead
port to prove fail-closed.

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

## The user directory

`IdentityUserDirectory` (rule 27) exposes the identity domain's read-only
user row over the same cached client: `findById` calls
`argus.identity.v1.GetUser` and returns the DB-free `DirectoryUser`
(`auth/user-directory.hxx` in this package). Consumers install
it where they own a seam — argus-sync builds one at boot
(`services/sync/src/app/main.cc:82`) and hands it to its socket and to the
voice relay, while argus-productivity holds it as a private member in its two
share-carrying feature services — so no service opens
another domain's database for a user row.

## What does NOT live here

Anything stateful. No repositories, no schemas, no `DbService`. The
identity domain (users, persons, invitations, portraits) is
`services/identity/`; the RPC surface that serves this package is
`services/identity/src/app/rpc/`. The sessions and the device credentials
behind a verdict are `services/auth/`'s.
