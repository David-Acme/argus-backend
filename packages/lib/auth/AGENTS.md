# argus-auth

The authentication filter package: the Drogon filter chain every service
runs in front of its routes, plus the JWT service that mints and verifies
the tokens.

Authentication IDENTIFIES the caller here; it does not authorize. Route
permissions are the owning service's business (RoleFilter only enforces
the static `role_access` table it is handed).

## What this is

A PACKAGE, not a service: no database, no listener, no process, no
`main`. It is compiled into every binary that serves authenticated
routes — auth, camera, guard, identity, llm, notification, productivity,
sync and tts.

## Layout

- `src/auth/device-filter.{cc,hxx}` — DeviceFilter: the per-request device
  fingerprint (HMAC over user-agent and either the source IP or the device
  credential, per `device.identity_mode`). In credential mode the
  `X-Argus-Device-Credential` header is looked up as a SHA-256 through
  `CheckDeviceCredential`; the plaintext never leaves the request.
- `src/auth/jwt-filter.{cc,hxx}` — JwtFilter: extracts the bearer token,
  verifies its signature locally, then asks `argus-auth` for the session
  verdict (`ValidateToken`) and stores the `JwtContext` the handlers read.
- `src/auth/role-filter.{cc,hxx}`, `src/auth/role-access.hxx` — RoleFilter:
  the static role/route table (`TableName` → permission set) and the
  route-prefix → module map (`kModuleRoutes`). It refuses a route of a
  disabled module with 403 `MODULE_DISABLED` before the role check. No I/O.
- `src/auth/module-gate.{cc,hxx}` — `ModuleGate` (`moduleGate()`, one per
  process): the enabled set of the selectable modules, read by `RoleFilter`
  through `role_access::moduleOfPath` and by a service's background loops;
  the enabled-set parser and the last-known-state file.
- `src/auth/module-feed.{cc,hxx}`, `src/auth/module-settings-read.cc` —
  `ModuleFeed` and `module_gate::install`: the boot read over
  `argus.settings.v1.Modules` and the per-service durable on
  `argus.settings.v1.module` that keep the gate current. CONTEXT.md, "Module
  gating".
- `src/auth/valid-json-filter.{cc,hxx}` — ValidJsonFilter: request body shape.
  No I/O.
- `src/auth/remote-config.{cc,hxx}` — `RemoteConfig` (`tunnelPort`,
  `enabled`, `resolve()`) over the `[remote]` keys, with `requestIsRemote`,
  `appendRemoteListener` and `requireDistinctTunnelPort`: the tunnel listener
  a service appends to its own, and the predicate that says whether a request
  arrived through it.
- `src/auth/remote-gate.{cc,hxx}` — `RemoteGate::check(req, remote)`: the
  pre-routing advice that answers `AuthErrors::RemoteNotAllowed` (403, CORS
  applied) when a tunnel-originated request reaches `/pairing` or
  `/auth/register` while `remote.enabled` is off.
- `src/auth/user-directory.hxx`, `src/auth/user-directory-identity.{cc,hxx}`
  — `IUserDirectory` and its RPC-backed implementation: the read-only
  `DirectoryUser` a service may hold without opening the identity database
  (rule 27).
- `src/auth/jwt-service.{cc,hxx}` — JwtService: HS256 mint and verify.
  `JwtRole::Issuer` (argus-auth) loads both secrets and refuses equal ones;
  `JwtRole::Verifier` (every `JwtFilter`) loads `jwt.secret` alone.
- `src/auth/auth-access.{cc,hxx}` — `filterAuthClient()`: the package's
  single auth RPC client, cached per resolved target, so the filters and a
  service's composition root share one connection to `argus-auth`;
  `installLocalAuthClient()` replaces it inside argus-auth itself with the
  in-process authority, so the session owner never dials its own listener.
- `src/auth/details/identity-access.{cc,hxx}` — the shared identity RPC
  client, cached per resolved target. Private by convention.

## Rules

- Rule 25: the folder IS the module. One `argus_lib(NAME auth ...)`
  declaration in `CMakeLists.txt`; explicit source lists, never
  `file(GLOB)`.
- Filters are declared on controllers BY NAME (Drogon `METHOD_ADD`
  strings), resolved at runtime through `DrClassMap`. A consumer links
  this package; it does not reference the filters by symbol. That also
  means a binary can link the package and still drop the objects — check
  presence with `nm -C`, never by reading CMake.
- No database. This package must never gain a repository, a schema or a
  `DbService` call: it asks `argus.auth.v1` for the session and the device
  credential and `argus.identity.v1` for the user directory. A dependency on
  `argus-identity` or `argus-auth` would recreate the cycles f7-3 and 3b-1
  dissolved.
- Include prefixes are load-bearing: consumers include
  `<auth/jwt-filter.hxx>` and `<auth/jwt-service.hxx>`,
  so the paths under `src/` must keep that shape.

## Tests

Seven suites of its own. `tests/unit/module-gate-test.cc` pins the module
map (every gated prefix, core never gated), `RoleFilter`'s
`MODULE_DISABLED` for every role, the `/modules` rows, the cache defaults,
the enabled-set parser, the persisted last known state, the versioned live
update and the boot read's retry and fallback. `tests/unit/role-access-test.cc` pins the
`kTableAccess` map role by role (`readableTables`, `permissionForMethod`,
`tableFromPath`, `hasHttpAccess`, the Owner-only guard administration, the
case and trailing-slash normalization, deny-by-default for unlisted `/auth`
paths and unknown methods, and `hasAppAction`); `jwt-service-test.cc` pins
the issuer/verifier split and the `typ` claim; `device-origin-test.cc` and
`proxy-allowlist-test.cc` pin the origin classes, the network prefixes and
the forwarded-for allowlist;
`tests/unit/remote-gate-test.cc` pins the `[remote]` keys, the tunnel
listener `appendRemoteListener` adds to a service's own, the refusal when
`tunnel_port` collides with a service listener, and the 403
`REMOTE_NOT_ALLOWED` a tunnel-originated `/pairing` or `/auth/register`
receives, CORS applied, while `remote.enabled` is off.

The filter chain is exercised against a real in-process auth RPC by
`services/auth/tests/unit/device-login-test.cc` (DeviceFilter in both
identity modes, JwtFilter's verdict refusals, and the device-login
handshake that mints the credential) and by every service's route suites.
