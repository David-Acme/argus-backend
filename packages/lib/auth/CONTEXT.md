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
  `kGuardAccess`: the service scopes every query to the caller's own
  sessions. Four more rows are `kOwnerOnly`: `GET /auth/users/sessions`,
  `GET` and `DELETE /auth/users/{id}/sessions` and
  `DELETE /auth/users/{id}/sessions/{id}`, the owner's view of every user's
  sessions. `routeMatches` compares a pattern segment by segment, one `{id}`
  per segment, so a placeholder never spans a `/` and an empty segment never
  matches.

## Route access is denied unless a table names it (2026-10 audit)

The role check used to read the path as it came and to fall back on a generic
`kAuthAccess` rule (any `/auth` path, decided by method alone, `GET` open to
every role). Drogon routes case-insensitively, so `GET /auth/Users/sessions`
reached the owner-only handler while `RoleFilter` saw an unknown `/auth` path
and let a guest through. Since the audit:

- `hasHttpAccess` lowercases the path and strips trailing slashes before any
  table is consulted (`normalizedPath`). A trailing slash is not a no-op for
  Drogon: `/x/` reaches the handler of `/x/{1}` with an empty id. So a path
  that had a trailing slash must be allowed both as `/x` and as its `{id}`
  child (`/x/0`); a role that holds only one of the two is refused.
- `kAuthAccess` is gone. Every `/auth` route behind `RoleFilter` is listed in
  `kSessionAccess`, and an `/auth` path that no row names is refused to every
  role but the Owner. The `/auth` routes without `RoleFilter` (status, me,
  logout, the QR approval and its details) never consult the table.
- Dispatch is by whole first segment (`firstSegment`): `auth`, `rtc`,
  `privacy`, `visitor`/`visitor-crop`, `sync` and `guard` each answer from
  their route table; then `kRouteOverrides` (rows that narrow a table, today
  `GET /notification/delivery-summary`, owner-only); then the camera actions;
  then `tableFromPath`, which also matches a whole first segment, so `/user`
  no longer answers for `/users-x`.
- `permissionForMethod` returns `std::nullopt` for every method it does not
  name, `PUT` included, and a table route with no permission is refused. A
  `PUT` reaches a non-owner only through an explicit row (`/privacy/me`,
  `/guard/safety/pin`).
- `role_access::hasAppAction(role, AppAction)` is the one answer for the
  actions the assistant can ask the app to perform (`ShowCamera`,
  `OpenScreen`, `SetGuardMode`). `SetGuardMode` is decided by the same
  `POST /guard/mode` row the HTTP route uses, so the tool descriptor and the
  route cannot drift; argus-llm and argus-voice are meant to ask it instead of
  carrying their own table/permission pair.

## The filters after the audit

- `JwtFilter` refuses (500 `DeviceContextMissing`) when no `DeviceContext` is
  on the request: a route that forgot `DeviceFilter` would otherwise lose the
  device binding silently. `scripts/check-routes.sh` enforces the same rule
  at build time (`JwtFilter` implies `DeviceFilter`) and keeps an explicit
  allowlist of the routes that run without `JwtFilter`.
- The token is read from `Authorization: Bearer` only. `?token=` is accepted
  on a WebSocket upgrade (`Upgrade: websocket`) and nowhere else, and the
  `authorization` cookie is no longer read: query strings end up in proxy and
  access logs, and no first-party client uses a cookie.
- `JwtFilter` holds a `JwtService{JwtRole::Verifier}`, which loads
  `jwt.secret` alone; the refresh secret stays in `argus-auth`, the only
  issuer. An issuer refuses to start when `jwt.secret` equals
  `jwt.refresh_secret`, every minted token carries `typ` (`access` or
  `refresh`), `verifyAccess` requires `typ = access`, and `verifyRefresh`
  refuses `typ = access` (a refresh token minted before the claim existed,
  with no `typ`, still verifies, so nobody is signed out by the upgrade; an
  access token without `typ` is refused and the app refreshes once).
- `device.fingerprint_secret` is mandatory (32 characters or more) and no
  longer falls back to `jwt.secret`. `DeviceFilter::requireFingerprintSecret()`
  is the boot check; argus-auth calls it before it listens, and a service that
  skips it refuses every request that needs a fingerprint instead.
- `device.identity_mode` is `credential` unless the key says `ip` exactly:
  a missing or unknown value is the stronger mode.
- The default LAN list no longer carries `172.16.0.0/12`: those are Docker's
  bridges, and a tunnelled request that reaches a service through a published
  port arrives from one. Installations behind the userland proxy name their
  bridge gateway in `device.lan_networks` themselves.
- `DeviceFilter::networkPrefix` (an address cut to a given prefix, IPv4 and
  IPv6, IPv4-mapped folded to IPv4) and `networkFingerprint` (HMAC of the
  origin class and the /24 or /64 network, `tunnel` for the tunnel listener)
  are what argus-auth binds an `ip`-mode refresh to and what its rate gate
  groups IPv6 clients by.
- `RemoteConfig` also reads `[remote] allow_qr_login` and
  `[remote] tunnel_profile`. `requireTunnelListener` refuses a configuration
  that expects the tunnel (`tunnel_profile`, `enabled` or `allow_qr_login`)
  while `tunnel_port` is 0, because then every tunnelled request is taken
  for a LAN one.

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
