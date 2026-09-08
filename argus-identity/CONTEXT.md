# CONTEXT.md — why this folder exists

## Origin

The Argus backend (C++20/Drogon monolith) is being migrated to
microservices; `src/` disappears entirely over that migration and every
repo-root folder becomes a service or a package. argus-identity (step
f7-2d) is the identity service's folder: what `src/identity/CMakeLists.txt`
compiled as `argus_identity` beyond the cross-domain modules — the
auth/invitation/pairing/user features, the identity repositories and
schemas, faces and the private-portrait storage — moved here unchanged,
relative paths preserved.

## Compiled into the gateway, on purpose

The service owns its code and its schema now, but NOT its process yet:
`argus_identity` still links into the argus-gateway binary (the
strangler-pattern state the migration plan calls "contract now, binary
later"). No new port, no new deployment unit. The gRPC contract
(`argus.identity.v1`) is designed for the standalone shape from the
start, so the later extraction changes CMake and deploy, not file
locations. That is why this folder has no `CMakePresets.json` or
`conanfile.txt` today.

## What moved and what didn't

Moved: 107 source files (26 feature `.cc` + headers, 10 repository
triplets, 10 schema pairs, `services/face`, `private-portrait-service`),
`database/identity-schema.sql` (now `database/schema.sql` here — the DDL
truth for every identity table), and the two identity unit suites
(`identity-migration-test`, `device-credential-test`).

Not moved, on purpose:

- The auth filter package (`src/filter/`) — `argus-auth` extraction is a
  later step, and it must land after the auth RPC.
- The audit / sqlite / cert / socket / mdns / room modules — cross-domain
  or gateway-owned; they dissolve into their owner services later.
- `src/shared/contracts/identity-change-sink.hxx` and the other sink
  contracts — consumed through the module links' include roots; they get
  their true home when the socket module does.
- The migration tool (`tools/migrate-identity`) — the root `tools/`
  dissolution owns it.
- `database/identity.db` — live data. Runtime still opens it from
  `database/` by default (`[identity] db`), so the move touched only the
  schema path default (`[identity] schema` now defaults to
  `argus-identity/database/schema.sql`) and the deploy bind that ships
  the schema into the container.

## The include-prefix invariant

Every moved file kept its `feature/...` / `shared/...` relative path, so
not one `#include` line changed. The module's include root became
`argus-identity/src` (it no longer exports the old `src/` tree at all):
each include of a file that stayed in `src/` was audited to resolve
through a declared module edge — `argus::socket` (socket-service,
identity-change-sink), `argus::cert`, `argus::audit` (sync-audit,
user-action-log), `argus::sqlite` (db-service, vec-db), `argus::auth`
(jwt-service, the filters), `argus_common` (config, validation, wrapper,
contracts, s3-storage).

## The auth⇄identity cycle is declared, not hidden

`argus::auth`'s filters read this service's repositories (device
credential by secret hash; user and refresh-token by the JWT chain), and
this service's AuthService calls JwtService and DeviceFilter statics.
Both edges are now declared PUBLIC to CMake — the sanctioned mechanism:
CMake repeats the archives at link time, so no consumer's link order
matters. The auth-RPC step (f7-3) deletes auth's database reads in favor
of one SDK call through argus-contracts, and the cycle dissolves with it.

## The unit suites

`identity-migration-test` (schema apply + the argus.db → identity.db row
by row verification) and `device-credential-test` (the DeviceFilter gate,
the credential repository, the auth-service issuance flow) are
root-project tests: the migration library is a root-only target
(`tools/` is EXCLUDE_FROM_ALL there), so the suites register only under
`ARGUS_ROOT_PROJECT`. Standalone consumer builds (gateway, camera,
productivity, notification) never had them. No e2e suite exists yet; the
folder gains `tests/e2e/` when the service has one.
