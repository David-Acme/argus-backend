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

- `src/filter/device/` — DeviceFilter: the per-request device fingerprint
  (HMAC over user-agent and either the source IP or the device
  credential, per `device.identity_mode`).
- `src/filter/jwt/` — JwtFilter: extracts the bearer token, verifies its
  signature locally, then asks the identity service to validate it.
- `src/filter/role/` — RoleFilter: the static role/route table. No I/O.
- `src/filter/valid-json/` — ValidJsonFilter: request body shape. No I/O.
- `src/filter/identity-access.*` — the shared identity RPC client, cached
  per resolved target.
- `src/shared/services/jwt/` — JwtService: HS256 mint and verify.

## Rules

- Rule 25: the folder IS the module. One `argus_module(NAME auth ...)`
  declaration in `CMakeLists.txt`; explicit source lists, never
  `file(GLOB)`.
- Filters are declared on controllers BY NAME (Drogon `METHOD_ADD`
  strings), resolved at runtime through `DrClassMap`. A consumer links
  this package; it does not reference the filters by symbol. That also
  means a binary can link the package and still drop the objects — check
  presence with `nm -C`, never by reading CMake.
- No database. This package must never gain a repository, a schema or a
  `DbService` call: it asks `argus.identity.v1` instead. A dependency on
  `argus-identity` would recreate the cycle f7-3 dissolved.
- Include prefixes are load-bearing: consumers include
  `<filter/jwt/jwt-filter.hxx>` and `<shared/services/jwt/jwt-service.hxx>`,
  so the paths under `src/` must keep that shape.

## Tests

No suite of its own yet. The filter chain is exercised by
`argus-identity/tests/unit/device-credential-test.cc` (both filters
against a real in-process identity RPC) and by every service's route
suites.
