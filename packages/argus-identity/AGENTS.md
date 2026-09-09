# AGENTS.md — argus-identity AI Agent Instructions

> This file is read by AI coding assistants before any code generation task.
> It defines this folder's purpose, conventions, and constraints.

## Project Identity

- **argus-identity** — the identity service: authentication features
  (login, register, refresh, device login), users, persons, invitations,
  pairing, device credentials, refresh tokens, face embeddings and the
  portrait/private-portrait storage. Declared as the `argus_identity`
  module (`argus::identity` alias).
- A service folder of the Argus monorepo (rooted at `backend/`), not an
  independent repo. It is compiled INTO the argus-gateway binary today —
  no new process, no new port — so it carries no `CMakePresets.json` /
  `conanfile.txt` of its own yet; the later standalone extraction is a
  CMake/deploy change, not a second file move.
- Authentication IDENTIFIES here; it does not authorize. Role-based
  authorization is the `argus-auth` filter package declared on routes by
  name. Biometrics (faces, embeddings) stay in this service.
- English only: file contents, comments, commit messages.

## Layout

```
argus-identity/
  CMakeLists.txt          argus_module(NAME identity ...) + the two unit suites
  database/schema.sql     the DDL truth for every identity table
  src/feature/api/{auth,invitation,pairing,user}/   moved files, relative
  src/shared/{repositories,schemas,services}/...    paths preserved
  tests/unit/             identity-migration-test, device-credential-test
```

The `src/feature/...` and `src/shared/...` path prefixes inside this
service are intentional, not legacy: every `#include <feature/...>` /
`<shared/...>` across the consumer files keeps resolving unchanged. See
CONTEXT.md for why.

## Binding rules

Root `AGENTS.md` MUST-FOLLOW rules 19-25 apply in full (modern C++20,
comment discipline, efficiency, DB tuning, feature layout, shared SDK,
monolith structure, build-by-module-name). In particular:

- Rule 20 (comment discipline) applies to every file here regardless of
  how long it predates this folder — a file's age or its having been moved
  is never a reason to leave a stale or misplaced comment.
- Rule 25 (the folder IS the module): this service is declared once,
  through `argus_module(NAME identity ...)` in this folder's
  `CMakeLists.txt`. Never list `argus-identity/src/...` files by raw path
  in a consumer's CMakeLists — link `argus_identity` (or `argus::identity`)
  instead.
- Rule 12 (thin controllers): controllers parse and delegate; the feature
  services own the domain logic.

## Tests

The two unit suites are root-project tests: they register with the root
ctest run only (guarded by `ARGUS_ROOT_PROJECT`), because the
identity-migration library they ride is a root-only target. Test file
naming stays hyphenated (`user-feature-service-test.cc`).

## What does NOT live here

- `argus-auth` — the filter package (`DeviceFilter`, `JwtFilter`,
  `RoleFilter`, `ValidJsonFilter`, `JwtService`), still `src/filter/`
  until its package extraction.
- The audit / socket / cert / sqlite / mdns / room modules — cross-domain
  or gateway-owned, still in their temporary `src/` module folders.
- The `identity.db` file — live data, opened from `database/` at runtime;
  only the schema (`database/schema.sql` here) is code.
- The migration CLI (`tools/migrate-identity`) — still in `tools/` until
  the root `tools/` dissolution; its library is what
  `identity-migration-test` verifies.
