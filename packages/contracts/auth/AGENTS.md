# argus_contracts_auth

The auth boundary's vocabulary: the four roles, the two request-context keys
every authenticated handler reads, the cross-device login challenge's status,
and the auth catalog the gates and the `/auth` surface throw from.

## What this is

A CONTRACT, not a service and not a library: one `argus_contracts`
declaration, an INTERFACE target with no translation unit. The include root is
`src/`, so a consumer writes `<auth/user-role.hxx>` and links
`argus::contracts::auth`. Twenty-five CMakeLists name it — 24 that link it
plus `services/notification/CMakeLists.txt`, which adds the package to its
standalone tree by path and links nothing from it — across the packages
`contracts/sync`, `lib/auth` and `clients/voice` (which links it for the
`<auth/user-role.hxx>` its header includes), plus the services that serve or
read a role:
`auth` across five files, `identity` across six, `sync` across five, `llm`
across two (its own file and the `argus::llm` feature module), and `camera`,
`notification` and
`productivity` — because the
role is the one value a JWT, a DTO and a role gate all have to spell the same
way, and the gate that enforces it lives in a different package from the
handlers that read it.

No `.proto` answers to this domain: the roles travel as JWT claims and as
Drogon attributes, not as a protobuf message, and the auth refusals travel as
the `{status, info, errors}` envelope's `errors.code`.

## Layout

- `src/auth/user-role.hxx` — `UserRole` (`Owner`, `Resident`, `Guard`,
  `Guest`) with `userRoleToString`/`userRoleFromString`; 36 files include it.
- `src/auth/request-context.hxx` — `AuthContext::kJwtKey` (`"jwt_ctx"`) and
  `AuthContext::kDeviceKey` (`"device_ctx"`): the attribute keys the filters
  write and every authenticated handler reads; 21 files include it.
- `src/auth/device-login-status.hxx` — `DeviceLoginStatus` (`Pending`,
  `Approved`, `Expired`) with
  `deviceLoginStatusToString`/`deviceLoginStatusFromString`: the QR pairing
  challenge's `status` column, which is both a `CHECK` constraint in
  `argus-auth`'s schema and the `status` string the polling device reads.
- `src/auth/auth-errors.hxx` — the auth catalog, twenty-four definitions: the
  five the gate filters and the remote gate answer with (`MissingToken`,
  `AuthenticationRequired`, `AccessDenied`, `RemoteNotAllowed`,
  `InvalidJsonBody`), the refresh-limiter refusal
  (`TooManyAttempts`), the `/auth` surface's own (multipart shape, challenge,
  device credential, refresh token, user, `ChangeNotRecorded`) and the
  enrollment outcomes `argus-identity` reports through `AuthFeatureService`,
  plus two the 2026-10 audit appended: `LoginProofRequired` (400, a QR
  challenge without its poll hash) and `DeviceContextMissing` (500,
  `JwtFilter` reached without `DeviceFilter`); 10 files include it. The
  catalog test pins thirty entries.

## Rules

- The four role strings and the two context keys are wire values. `"owner"` is
  what a JWT carries and `jwt_ctx` is what thirty-two handlers across the tree
  look up; renaming either is a contract break, not a refactor.
- The types stored under the context keys (`JwtContext`, `DeviceContext`)
  belong to `packages/lib/auth`; only the keys travel through here, so a
  consumer can read them without taking Drogon.
- A gate has no status code to pick: it throws a catalog definition and the
  shared advice formats it. A string literal for a code is the duplication
  that breaks the wire the first time the two copies disagree.
- Rule 25: the folder IS the module. One `argus_contracts(NAME auth ...)` with
  an explicit source list, never `file(GLOB)`.

## Tests

- `tests/unit/auth-contract-vocabulary-test.cc` — the role round-trip and the
  documented fallback for an unknown string.
- `tests/unit/auth-contract-catalog-test.cc` — every refusal as a pinned
  table row, each entry's wire legality, and that no two say the same thing.
