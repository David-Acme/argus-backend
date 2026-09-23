# AGENTS.md — argus-identity AI Agent Instructions

> This file is read by AI coding assistants before any code generation task.
> It defines this folder's purpose, conventions, and constraints.

## Project Identity

- **argus-identity** — the identity service: authentication features
  (login, register, refresh, device login), users, persons, invitations,
  pairing, device credentials, refresh tokens, face embeddings and the
  portrait/private-portrait storage. Declared as the `argus_identity`
  module (`argus::identity` alias).
- A package folder of the Argus monorepo (rooted at `backend/`), not an
  independent repository or process. It compiles into `argus-gateway` (which
  serves its RPC surface) and into `argus-sync` (whose engine pages its user
  and person tables), and also configures standalone for validation, against
  the root graph.
- Authentication IDENTIFIES here; it does not authorize. Role-based
  authorization is the `argus-auth` filter package declared on routes by
  name. Biometrics (faces, embeddings) stay in this service.
- English only: file contents, commit messages.

## Layout

```
argus-identity/
  CMakeLists.txt          argus_module(NAME identity ...) + unit suites
  database/schema.sql     the DDL truth for every identity table
  src/feature/api/{auth,invitation,pairing,user}/   moved files, relative
  src/shared/{repositories,schemas,services}/...    paths preserved
  src/shared/repositories/change-outbox/            the identity feed's durable
                                                    outbox (its own module)
  tests/unit/             identity migration, device credential, change outbox
                          and change sink suites
  tools/migrate-identity/ identity migration CLI and library
```

The `src/feature/...` and `src/shared/...` path prefixes inside this
service are intentional, not legacy: every `#include <feature/...>` /
`<shared/...>` across the consumer files keeps resolving unchanged. See
CONTEXT.md for why.

## Binding rules

Root `AGENTS.md` MUST-FOLLOW rules 19-25 apply in full (modern C++20,
no comments in code, efficiency, DB tuning, feature layout, shared SDK,
monolith structure, build-by-module-name). In particular:

- Rule 20 (no comments in code) applies to every file here regardless of
  how long it predates this folder: a file's age or its having been moved
  is never a reason to keep a comment.
- Rule 25 (the folder IS the module): this service is declared once,
  through `argus_module(NAME identity ...)` in this folder's
  `CMakeLists.txt`. Never list `packages/identity/src/...` files by raw path
  in a consumer's CMakeLists — link `argus_identity` (or `argus::identity`)
  instead.
- Rule 12 (thin controllers): controllers parse and delegate; the feature
  services own the domain logic.

## Tests

The four unit suites register in this package's standalone CTest graph. Test
file naming stays hyphenated (`identity-migration-test.cc`).

`identity-change-outbox-sink-test` skips its live block unless
`ARGUS_NATS_URL` names a broker; run it with one to exercise the publish,
the self-ensured stream and the journal's two identical reads:

```bash
ARGUS_NATS_URL=nats://127.0.0.1:4222 \
  packages/identity/build/dev/identity-change-outbox-sink-test
```

```bash
# From the monorepo root
./scripts/build-all.sh dev --only identity
```

Driving CMake by hand inside this folder means installing the root graph once
(`./scripts/build-all.sh dev --install-only`) and passing its toolchain; the
exact flag set is in `docs/operations/build-and-test.md` under "Working inside
one project".

## What does NOT live here

- `argus-auth` — the sibling filter package (`DeviceFilter`, `JwtFilter`,
  `RoleFilter`, `ValidJsonFilter`, `JwtService`).
- Audit, socket, cert, sqlite and room — sibling cross-domain packages.
- The `identity.db` file — live data, opened from `database/` at runtime;
  only the schema (`database/schema.sql` here) is code.
