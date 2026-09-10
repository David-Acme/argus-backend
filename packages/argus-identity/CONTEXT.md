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
locations.

## Standalone build (f8-c2)

The package also configures from its own folder (`conanfile.txt` +
dev/prod presets): the top-level configure adds the sibling packages the
module links, the vendored `sqlite-vec` and `ncnn`, and the migration
tool the unit suites ride. Like argus-contracts standalone, the cq-bridge
stand-ins are declared empty: standalone protobuf comes from the system
(no conan onnxruntime to pull conan protobuf in), so the bridge entry
symbol would interpose libgrpc's own callbacks and recurse.

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

## The auth⇄identity cycle is gone (f7-3)

`argus::auth`'s filters used to read this service's repositories (device
credential by secret hash; user and refresh-token by the JWT chain), which
made the dependency mutual — this service's AuthService calls JwtService
and DeviceFilter statics in the other direction. f7-3 deleted the reading
half: the filters now call `argus.identity.v1` (ValidateToken,
CheckDeviceCredential) through `argus::sdk-identity`, so `argus::auth`
depends on argus-contracts, never on this folder. What remains is one
direction only — argus_identity → argus::auth — and no consumer's link
order matters anymore.

## The RPC surface

`src/feature/rpc/identity-rpc.cc` serves `argus.identity.v1`: UpdateUser
(the F6-3 spoken-name write) plus the f7-3 pair ValidateToken and
CheckDeviceCredential. It lives here because the surface belongs to this
service; the gateway only HOSTS the listener (it constructs the service
and binds `identity.rpc_host:rpc_port`), which is what makes it move with
the folder at the standalone extraction instead of being rewritten.

ValidateToken is the single authoritative validation: it verifies the JWT
signature, reads the live user row (status always fresh — no cached
verdicts) and, when the caller's device filter ran, the refresh-token row
with its expiry and device binding. It answers OK with `valid=false` and
the caller's 401 body rather than a gRPC error, so a rejected token and a
broken service stay distinguishable — an unreachable service makes the
filter fail closed.

## The unit suites

`identity-migration-test` (schema apply + the argus.db → identity.db row
by row verification) and `device-credential-test` (the DeviceFilter gate,
the credential repository, the auth-service issuance flow) register in the
package's standalone CTest graph. No e2e suite exists yet; the folder gains
`tests/e2e/` when the package has one.
