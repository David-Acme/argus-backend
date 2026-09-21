# argus_contracts_auth

The auth boundary's vocabulary: the four roles, the two request-context keys
every authenticated handler reads, and the four refusals the gates throw.

## What this is

A CONTRACT, not a service and not a library: one `argus_contracts`
declaration, an INTERFACE target with no translation unit. The include root is
`src/`, so a consumer writes `<auth/user-role.hxx>` and links
`argus::contracts::auth`. Thirteen CMakeLists link it — eight packages
(clients/llm, clients/voice, identity, lib/auth, memory, room, socket, sync) and
five services (camera, gateway, llm, notification, productivity) — because the
role is the one value a JWT, a DTO and a role gate all have to spell the same
way, and the gate that enforces it lives in a different package from the
handlers that read it.

No `.proto` answers to this domain: the roles travel as JWT claims and as
Drogon attributes, not as a protobuf message.

## Layout

- `src/auth/user-role.hxx` — `UserRole` (`Owner`, `Resident`, `Guard`,
  `Guest`) with `userRoleToString`/`userRoleFromString`; 26 files include it.
- `src/auth/request-context.hxx` — `AuthContext::kJwtKey` (`"jwt_ctx"`) and
  `AuthContext::kDeviceKey` (`"device_ctx"`): the attribute keys the filters
  write and every authenticated handler reads; 21 files include it.
- `src/auth/auth-errors.hxx` — the four refusals (`MissingToken`,
  `AuthenticationRequired`, `AccessDenied`, `InvalidJsonBody`), thrown by the
  three gates that answer with them; 5 files include it.

## Rules

- The four role strings and the two context keys are wire values. `"owner"` is
  what a JWT carries and `jwt_ctx` is what forty handlers look up; renaming
  either is a contract break, not a refactor.
- The types stored under the context keys (`JwtContext`, `DeviceContext`)
  belong to `packages/lib/auth`; only the keys travel through here, so a
  consumer can read them without taking Drogon.
- A gate has no status code to pick: it throws one of the four definitions and
  the shared advice formats it. A string literal for a code is the duplication
  that breaks the wire the first time the two copies disagree.
- Rule 25: the folder IS the module. One `argus_contracts(NAME auth ...)` with
  an explicit source list, never `file(GLOB)`.

## Tests

- `tests/unit/auth-contract-vocabulary-test.cc` — the role round-trip and the
  documented fallback for an unknown string.
- `tests/unit/auth-contract-catalog-test.cc` — the four refusals as a pinned
  table, each entry's wire legality, and that no two say the same thing.
