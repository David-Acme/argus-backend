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
routes — gateway, camera, productivity, notification, tts.

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
  the static role/route table (`TableName` → permission set). No I/O.
- `src/auth/valid-json-filter.{cc,hxx}` — ValidJsonFilter: request body shape.
  No I/O.
- `src/auth/identity-change-sink.hxx` — the sink interface that republishes
  identity-domain writes for the memory catalog replicas (the gateway
  installs the NATS-backed one).
- `src/auth/user-directory.hxx`, `src/auth/user-directory-identity.{cc,hxx}`
  — `IUserDirectory` and its RPC-backed implementation: the read-only
  `DirectoryUser` a service may hold without opening the identity database
  (rule 27).
- `src/auth/jwt-service.{cc,hxx}` — JwtService: HS256 mint and verify.
- `src/auth/auth-access.{cc,hxx}` — `filterAuthClient()`: the package's
  single auth RPC client, cached per resolved target, so the filters and a
  service's composition root share one connection to `argus-auth`.
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

No suite of its own. The filter chain is exercised against a real in-process
auth RPC by `services/auth/tests/unit/device-login-test.cc` (DeviceFilter in
both identity modes, JwtFilter's verdict refusals, and the device-login
handshake that mints the credential) and by every service's route suites.
