# CONTEXT.md — why this folder exists

## Origin

The Argus backend (C++20/Drogon monolith) is being migrated to microservices;
`src/` disappears entirely over that migration and every repo-root folder
becomes a service or a package. This folder was `packages/identity` (step
f7-2d) and is the identity service's home; Phase 3c step 1 turned it into a
service — a process with its own boot, listeners and database, instead of a
module the gateway linked — and the gate's project list swapped the package for
it, so the tree still counts eighteen projects.

What the package was: everything `src/identity/CMakeLists.txt` compiled as
`argus_identity` beyond the cross-domain modules — the invitation, pairing and
user features (the `auth` one became `enrollment` in Phase 3b-2, when the
session surface moved to `argus-auth`), the identity repositories and schemas,
faces and the private-portrait storage — moved here unchanged, relative paths
preserved. Step 1 then moved each of those files a second time, from
`src/feature/api/<resource>/` to `src/feature/<resource>/` and from
`src/feature/rpc/` to `src/app/rpc/`, which is rule 23's shape for a service.

## The standalone process

The extraction is a process, not a folder: `src/app/main.cc` loads
`config.toml`, opens `database/identity.db`, applies
`database/schema.sql` from its beginning advice, installs the four filters,
registers the four controllers, connects the NATS bus and installs the change
sink, builds the gRPC server on `[server] grpc_port` (7040), and runs Drogon on
`[identity] port` (7044) with the instance certificate.

Before step 1 the gateway HOSTED both surfaces: it constructed the RPC service
and bound `identity.rpc_host:rpc_port`, and it proxied the HTTP route. That was
the strangler-pattern state the plan calls "contract now, binary later" — the
gRPC contract was designed for the standalone shape from the start, so the
extraction changed CMake, boot and deploy, not file locations. The gateway's
own `src/identity/identity-config.*` and `src/identity/identity-registrar.*`
died with it: the gateway keeps the public API on 7024 and reaches this
service through `identity.proxy_url` (HTTP) and `identity.target` (gRPC), and
reads the notifiable roster through `argus::clients::identity` instead of the
tables it used to query.

## The port the pairing and invitation answers publish

`PairingController` and `InvitationFeatureService::resolve` both answer the
port a client dials next, and both read it from `mdns.port`. Before step 1 that
read happened inside the gateway process, whose config carries the key (the
port it advertises and serves, 7024); after it, the read happens here, so this
service's own config carries the same key and value. The number is the
installation's public port, not this service's — the app pairs against it,
stores it as the paired instance's port and puts it in every later invitation
QR, and it rejects an answer whose port is not positive or does not match the
port mDNS advertised. Identity runs no advertiser; the key is here because the
answer must equal what the gateway advertises, and Phase 3d's discovery step is
where that changes.

## What moved and what didn't

Moved: 107 source files (the four features' `.cc` + headers, the ten
`src/shared/repositories/` triplets, the seven `src/shared/schemas/` pairs,
`src/shared/services/face/{face-service,face-db}.{hxx,cc}`,
`src/shared/services/storage/private-portrait-service.{hxx,cc}`),
`database/schema.sql` (the DDL truth for every identity table), the identity
unit suites and the migration tool (now `tools/migrate-identity/`, where the
root `tools/` dissolution landed it).

Not moved, on purpose:

- The auth filter package (`src/filter/`) — `argus-auth` landed as its own
  service in Phase 3b-1 and Phase 3b-2 moved the `/auth` surface, the three
  session tables and the rate limiter onto it; what stays here is the
  `enrollment` feature that answers `RegisterUser`.
- The audit / sqlite / cert / socket / mdns / room modules — cross-domain or
  gateway-owned; they dissolve into their owner services later. `socket`,
  `room` and `audit` did, into `services/sync` in Phase 3a-1c; `sqlite` and
  `cert` are the `lib` packages this service still links. `mdns` is not one of
  them: this service runs no advertiser, and the only mdns surface it has is
  the `mdns.port` key its pairing and invitation answers publish (below).
- `src/auth/identity-change-sink.hxx` and the other sink contracts — consumed
  through the module links' include roots. The contracts now live in
  `argus::contracts::sync` and the identity sink is this service's own
  (`src/feature/user/services/nats-identity-change-sink.{hxx,cc}`).
- `database/identity.db` — live data. The service opens it from `database/` by
  default (`[identity] db`), and the schema path default is
  `services/identity/database/schema.sql`; the deploy binds the schema into the
  container.

## The include-prefix invariant

Every moved file kept its `feature/...` / `shared/...` relative path, so the
include root stayed `src/`, exactly as the package's was
`packages/identity/src`; the only include edits the move made are the retired
`api/` segment's removal in thirteen files (`<feature/api/x/...>` →
`<feature/x/...>` — eleven feature sources and the two unit tests), and the
same segment dropped by the header that became
`src/app/rpc/identity-rpc-service.hxx` in its rename. Each include of a file
that stayed
in `src/` was audited to resolve through a declared module edge —
`argus::lib::cert`, `argus::lib::sqlite` (db-service, vec-db),
`argus::lib::auth` (jwt-service, the filters), `argus::lib::config`,
`argus::lib::validation`, `argus::lib::runtime` (the cancellation and threading
wrappers), `argus::lib::storage`, `argus::contracts::sync`,
`argus::contracts::voice` (the `VoiceLang` a new user's language is carried in)
and `argus::contracts::identity` (the generated stubs the RPC services
implement). Two edges of that audit are gone since Phase 3a-1c: `argus::socket`
and `argus::audit` were deleted with their packages, the sink contract moved
into `argus::contracts::sync` and the sink that implements it into this
service's own feature tree, so identity now publishes onto the feed instead of
writing the audit tables.

## The auth⇄identity cycle is gone (f7-3)

`argus::lib::auth`'s filters used to read this service's repositories (device
credential by secret hash; user and refresh-token by the JWT chain), which made
the dependency mutual — this service's controllers declare those same filters.
f7-3 deleted the reading half: the filters called `argus.identity.v1`
(ValidateToken, CheckDeviceCredential) through `argus::clients::identity`
instead. Phase 3b-3 moved that call a second time, onto the verdict argus-auth
serves (`argus.auth.v1`, through `argus::clients::auth`), so nothing in the
filter path names this service at all. What remains is one direction only —
the identity service → `argus::lib::auth`, the filter declarations its own
controllers carry — and no consumer's link order matters anymore.

## The RPC surface

`src/app/rpc/identity-rpc-service.cc` serves `argus.identity.v1`:
UpdateUser (the F6-3 spoken-name write), RegisterUser (the `enrollment`
feature), GetUser and the person/face calls (ListPersons, IdentifyPerson,
EnrollPerson, TouchPerson, TagPerson, ListNotifiableUsers, GetPersonTags,
GetPerson, PromotePerson). `IdentitySyncRpcService` serves the same contract's
`SyncService.PullTable` leg for `user`, `user_invitation` and `person` — the
rows `services/sync` pages when a client bootstraps, alongside the change feed
this service publishes.

The f7-3 pair ValidateToken and CheckDeviceCredential left with the session
surface in Phase 3b-1/3b-2: argus-auth serves them now, and it is the single
authoritative validation — JWT signature, the live user row (status always
fresh, no cached verdicts) and, when the caller's device filter ran, the
refresh-token row with its expiry and device binding. It answers OK with
`valid=false` and the caller's 401 body rather than a gRPC error, so a rejected
token and a broken service stay distinguishable — an unreachable authority
makes the filter fail closed. `IdentityRpcService` holds `filterAuthClient()`
for the one thing that still needs a live verdict here: PromotePerson's Owner
actor check.

## The unit suites

`identity-config-test` (the config module's defaults, its section overrides and
the non-loopback RPC listener's secret gate — the cases the gateway's suite
carried while it hosted this surface), `identity-migration-test` (schema apply
+ the argus.db → identity.db row by row verification),
`identity-change-outbox-test` (the outbox's own key rules and dispositions),
`identity-change-transaction-test` (a feature write and its outbox row commit
together), `identity-change-outbox-sink-test` (every leg of the sink, plus the
live round trip when `ARGUS_NATS_URL` names a broker) and
`identity-sync-rpc-test` (the `SyncService` pull leg: the role gate, the
rule-7b user scope, the tombstones and both sides of the fleet-secret gate)
register in this service's standalone CTest graph. `device-credential-test`
left with the credential flow it drives (Phase 3b-2), where it is
`services/auth`'s `device-login-test`. No e2e suite exists yet; the folder
gains `tests/e2e/` when the service has one.

## The change feed's durable outbox (3a-2e)

This feed publishes on two subjects — the change subject the memory catalog
replicas follow and the action subject the journal subscriber reads — where
camera, notification and productivity each publish on one. The module
therefore carries a `subject` column: a row states where it goes instead of the
drain inferring it from a payload that does not always name its own kind (a
journal row has no `kind` field at all).

The two legs are addressed differently. A change leg names a *transition*: its
`event_id` is the hash of the table, the record and the payload, so a
redelivered transition is a replay and the same record cannot be published
twice for one move. A journal leg names an *action*: a portrait view changes no
row, so two views of one portrait carry byte-identical payloads and a
content-derived id would collapse them into one audit row — the kind of loss
nobody would notice. A journal row therefore carries a **minted** `event_id`,
`identity-action:` followed by 32 lowercase hex digits drawn from
`RAND_bytes` at enqueue, and travels under that id as its `Nats-Msg-Id`. The id
must be opaque rather than positional: once Phase 3c-2 split the journal into
`sync.db` its row ids come from a table the identity outbox does not own, so an
id derived from them would collide with whatever the sibling owner's own rows
happen to be numbered. Settlement stays a status-guarded CAS over the row id
(the minted id guards the broker's duplicate window, not the flush), and rows
enqueued before the minting still flush under the row-derived
`identity-action:<id>` they were published with.

The sink registers with `shutdown_signal` at this service's boot — it used to
register with the gateway's, which hosted the package — so a SIGTERM stops the
drain before Drogon's `quit()` destroys the database client manager the worker
reaches through `DbService::client()`. The drain's worker is a thread of the
sink's own, so nothing in Drogon stops it, and the hook waits for it to report
drained rather than quitting underneath it. The registration comes before
`drogon::app().run()` — the hook's handlers are what `run()` installs Drogon's
own `sigaction` over, and a signal that arrived before the first registration
would run Drogon's default quit with no drain wait — and therefore before the
`reconcile()` that starts the worker, because a drain registered after the stop
was requested is only stopped at once, never waited for.

`identity.db` used to be the one database two owners wrote — this service and
`services/sync`, which applied its five tables into the same file between Phase
3c-1 and 3c-2. Both were free to call their table `change_outbox`, and
`CREATE TABLE IF NOT EXISTS` would have silently let whoever booted second
adopt the first one's shape; Phase 3c-2 removed the question by giving the sync
owner a file of its own, and this service's outbox is the only `change_outbox`
left here.

## Camera guard surface (camera-guard phase 2)

`IdentifyPerson`, `EnrollPerson`, `TouchPerson`, `TagPerson` and
`ListNotifiableUsers` are fleet-secret gated RPCs. Enrollment creates a person
without user (empty name), persists the embedding and its `face_vec` row,
optionally stores the JPEG crop in `person_snapshot`, and emits the `person`
sync add. `person_tag` holds LLM tags. The face engine stays inside this
process; argus-camera only ships crops.

`PromotePerson` (candidate → known) is the one mutating call with a human gate:
the owner bearer token plus the device fingerprint travel in the call, the
service verifies the access token, requires an Owner actor with an active bound
session on that device (`hasActiveSession`), promotes, and publishes the
`person` module audit. `person.status` (`candidate`/`known`) defaults to
`known` for legacy rows; `IdentifyPerson` reports
`trusted = (status == known)` and attaches the linked user only for active
users.

The schema used to carry `notification_delivery_inbox` and the three audit
tables — the gateway's durable delivery receipts and its audit trail. Phase
3a-1c moved all four into `services/sync/database/schema.sql`, whose owner
applies and writes them; this file is identity's tables and nothing else.

## The file split (Phase 3c-2)

This service and `services/sync` wrote one SQLite file until Phase 3c-2 split
the five sync tables into argus-sync's own `sync.db`. `argus-migrate-sync`
copies them (row-count and checksum verification over exactly the keys the run
copied; the deploy runs it as `--profile sync-init` forward and `--profile
sync-rollback` with the paths swapped), and
the journal's redelivery key was re-minted at the same time so
`user_action_log.msg_id` no longer borrows a row id from a table this owner
writes. Nothing here owns, renames or drops those five tables any more.
