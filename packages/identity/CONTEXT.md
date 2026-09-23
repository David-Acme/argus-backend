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

The package also configures from its own folder: the top-level configure adds
the sibling packages the module links, the vendored `sqlite-vec` and `ncnn`,
and the migration tool the unit suites ride. There is no per-package Conan
manifest: the root `conanfile.txt` is the tree's only one, `build-all.sh`
resolves it once, and a configure inside this folder takes the toolchain that
install produced (`docs/operations/build-and-test.md`, "Working inside one
project"). The cq bridge is no longer skipped here, because the root graph
carries a Conan abseil through onnxruntime's protobuf, so this tree has the
same two abseil flavors as every other and `argus_grpc_absl_bridge()` joins
them (see `packages/contracts/CONTEXT.md`).

## What moved and what didn't

Moved: 107 source files (26 feature `.cc` + headers, 10 repository
triplets, 10 schema pairs,
`src/shared/services/face/{face-service,face-db}.{hxx,cc}`,
`src/shared/services/storage/private-portrait-service.{hxx,cc}`),
`database/schema.sql` (the DDL
truth for every identity table), and the two identity unit suites
(`identity-migration-test`, `device-credential-test`).

Not moved, on purpose:

- The auth filter package (`src/filter/`) — `argus-auth` extraction is a
  later step, and it must land after the auth RPC.
- The audit / sqlite / cert / socket / mdns / room modules — cross-domain
  or gateway-owned; they dissolve into their owner services later. `socket`,
  `room` and `audit` did, into `services/sync` in Phase 3a-1c; `sqlite`, `cert`
  and `mdns` are the `lib` packages this service still links.
- `src/auth/identity-change-sink.hxx` and the other sink
  contracts — consumed through the module links' include roots; they get
  their true home when the socket module does. The socket module did not
  survive to take them: the contracts now live in `argus::contracts::sync` and
  the identity sink is this service's own
  (`src/feature/api/user/services/nats-identity-change-sink.{hxx,cc}`).
- The migration tool (`tools/migrate-identity`) — the root `tools/`
  dissolution owns it.
- `database/identity.db` — live data. Runtime still opens it from
  `database/` by default (`[identity] db`), so the move touched only the
  schema path default (`[identity] schema` now defaults to
  `packages/identity/database/schema.sql`) and the deploy bind that ships
  the schema into the container.

## The include-prefix invariant

Every moved file kept its `feature/...` / `shared/...` relative path, so
not one `#include` line changed. The module's include root became
`packages/identity/src` (it no longer exports the old `src/` tree at all):
each include of a file that stayed in `src/` was audited to resolve
through a declared module edge — `argus::lib::cert`, `argus::lib::sqlite`
(db-service, vec-db), `argus::lib::auth`
(jwt-service, the filters), `argus::lib::config`, `argus::lib::validation`,
`argus::lib::runtime` (the cancellation and
threading wrappers), `argus::lib::storage` and `argus::contracts::sync` (config,
validation, wrapper, s3-storage, sync contracts). Two edges of that audit are
gone since Phase 3a-1c: `argus::socket` and `argus::audit` were deleted with
their packages, the sink contract moved into `argus::contracts::sync` and the
sink that implements it into this service's own feature tree, so identity now
publishes onto the feed instead of writing the audit tables.

## The auth⇄identity cycle is gone (f7-3)

`argus::lib::auth`'s filters used to read this service's repositories (device
credential by secret hash; user and refresh-token by the JWT chain), which
made the dependency mutual — this service's AuthService calls JwtService
and DeviceFilter statics in the other direction. f7-3 deleted the reading
half: the filters now call `argus.identity.v1` (ValidateToken,
CheckDeviceCredential) through `argus::clients::identity`, so `argus::lib::auth`
depends on the identity wire — contract and client — never on this folder. What remains is one
direction only — argus_identity → argus::lib::auth — and no consumer's link
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
by row verification), `device-credential-test` (the DeviceFilter gate,
the credential repository, the auth-service issuance flow),
`identity-change-outbox-test` (the outbox's own key rules and
dispositions) and `identity-change-outbox-sink-test` (every leg of the
sink, plus the live round trip when `ARGUS_NATS_URL` names a broker)
register in the package's standalone CTest graph. No e2e suite exists yet;
the folder gains `tests/e2e/` when the package has one.

## The change feed's durable outbox (3a-2e)

This feed publishes on two subjects — the change subject the memory catalog
replicas follow and the action subject the journal subscriber reads — where
camera, notification and productivity each publish on one. The copied module
therefore carries a `subject` column: a row states where it goes instead of the
drain inferring it from a payload that does not always name its own kind (a
journal row has no `kind` field at all).

The two legs are addressed differently, and the difference is the whole reason
the row's own `id` is what the drain publishes under. A change leg names a
*transition*: its `event_id` is the hash of the table, the record and the
payload, so a redelivered transition is a replay and the same record cannot be
published twice for one move. A journal leg names an *action*: a portrait view
changes no row, so two views of one portrait carry byte-identical payloads and a
content-derived id would collapse them into one audit row — the kind of loss
nobody would notice. Those rows leave `event_id` NULL and travel as
`identity-action:<id>`, the row's own position, which the broker's duplicate
window still protects against a redelivery without merging two distinct
actions. Settlement is a status-guarded CAS over that same id, because a NULL
`event_id` cannot guard anything.

`identity.db` is also the one database two owners write — this package and
`services/sync`, which applies its own four tables into the same file until
Phase 3c-2. Both would be free to call their table `change_outbox`, and
`CREATE TABLE IF NOT EXISTS` would silently let whoever boots second adopt the
first one's shape; a `sync` outbox must take a different name until the files
split.

The sink registers with `shutdown_signal` at the boot that installs it — the
gateway's, which hosts this package until Phase 3c; the package gains its own
boot when it gains its process — so a
SIGTERM stops the drain before Drogon's `quit()` destroys the database client
manager the worker reaches through `DbService::client()`. The drain's worker
is a thread of the sink's own, so nothing in Drogon stops it, and the hook
waits for it to report drained rather than quitting underneath it. The
registration comes before `drogon::app().run()` — the hook's handlers are what
`run()` installs Drogon's own `sigaction` over, and a signal that arrived
before the first registration would run Drogon's default quit with no drain
wait — and therefore before the `reconcile()` that starts the worker, because
a drain registered after the stop was requested is only stopped at once,
never waited for.

## Camera guard surface (camera-guard phase 2)

`IdentifyPerson`, `EnrollPerson`, `TouchPerson`, `TagPerson` and
`ListNotifiableUsers` are fleet-secret gated RPCs. Enrollment creates a person
without user (empty name), persists the embedding and its `face_vec` row,
optionally stores the JPEG crop in `person_snapshot`, and emits the `person`
sync add. `person_tag` holds LLM tags. The face engine stays inside this
process; argus-camera only ships crops.

`PromotePerson` (candidate → known) is the one mutating call with a human
gate: the owner bearer token plus the device fingerprint travel in the call,
the service verifies the access token, requires an Owner actor with an
active bound session on that device (`hasActiveSession`), promotes, and
publishes the `person` module audit. `person.status` (`candidate`/`known`)
defaults to `known` for legacy rows; `IdentifyPerson` reports
`trusted = (status == known)` and attaches the linked user only for active
users.

The schema used to carry `notification_delivery_inbox` and the three audit
tables — the gateway's durable delivery receipts and its audit trail. Phase
3a-1c moved all four into `services/sync/database/schema.sql`, whose owner
applies and writes them; this file is identity's tables and nothing else. The
four rows still *live* in `database/identity.db` until Phase 3c-2 splits them
out, which is the transitory state `services/sync` declares in `[sync] db`.
