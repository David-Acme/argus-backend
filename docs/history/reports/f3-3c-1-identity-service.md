# Phase 3c step 1 — `services/identity`, the people domain as its own process

Phase 3c is two steps. This unit is the first: `packages/identity` becomes
`services/identity` — the people domain gets a process, a listener and a
database of its own — and the package dies. The gate keeps its eighteen
projects and changes one slot's kind: `packages/identity` was one of the five
package entries, and `services/identity` takes its place among the fourteen
services. It is not a file move. A package has no `main`, no listener and no
database of its own — the gateway hosted it in its own process, applied its
schema, mounted its data directory and answered its four routes, and the sync
engine read its three tables in process. Extracting the service means deciding
who owns each of those, and three of the four answers are new wire.

- **3c-1 (this unit)** — the service, its listener, its database, its own
  filter chain, and the three things that used to reach inside it: the
  gateway's hosting, the sync engine's repositories, the deploy stack.
- **3c-2** — the split of `identity.db`: the audit half (`audit_log`,
  `user_audit_log`, `user_action_log`) moves to `sync.db` with row-count and
  checksum verification and the `user_action_log.msg_id` re-key.

## What existed before

`packages/identity` had no `src/app/` and no `main.cc`. Its shape was
`src/feature/api/{auth,enrollment,invitation,pairing,user}/` (39 files),
`src/feature/rpc/identity-rpc.{hxx,cc}` (the `argus.identity.v1` surface),
`src/shared/{repositories,schemas,services,vocabulary}/`, `database/schema.sql`
(eleven tables), `scripts/provision.sh`, `tools/migrate-identity/` and four
suites under `tests/unit/`. 3b had already taken the session half out of its
schema and its `/auth` folder, so what remained was the people: users, persons,
faces, invitations, portraits and the identification RPC.

The gateway hosted it. `services/gateway/src/identity/identity-config.{hxx,cc}`
resolved the identity database, its schema path and the RPC listener out of the
gateway's `[identity]` section, and `identity-registrar.{hxx,cc}` composed the
package inside the gateway process: the identity listener (7042, TLS), the
filter chain, the controllers behind `/user`, `/invitation`, `/pairing` and
`/portrait-preview`, the face engine and the object storage for portraits.
Those four paths were in `gatewayNativePaths()`, so the reverse proxy never saw
them. The compose mounted `${ARGUS_DATA_DIR}/identity` as the gateway's
`/opt/argus/database`, `packages/identity/database/schema.sql` as its
`database/schema.sql`, and `${ARGUS_MODELS_DIR}` read-only for the face models.

The sync engine read the package's tables directly.
`SynchronizedService` held `UserRepository`, `UserInvitationRepository` and
`PersonRepository` beside its camera, productivity and notification sources,
`repoFor(TableName)` returned them for `User`, `UserInvitation` and `Person`,
and the rule-7b scope was applied in that method:

```cpp
if (table == TableName::User && ctx.role != UserRole::Owner &&
    ctx.role != UserRole::Guard)
  base.userId = ctx.sub;
```

The service's link to `packages/identity` existed for exactly that read.

## The decision, as built

### The package becomes a service of its own

`services/identity/` is the owner shape of rule 23: `src/app/main.cc` and
`src/app/rpc/`, `src/config/`, `src/feature/{enrollment,invitation,pairing,user}/`,
`src/shared/{repositories,schemas,services,vocabulary}/`, `database/schema.sql`,
`tests/unit/`, `tools/migrate-identity/`, `scripts/provision.sh`, its own
`Dockerfile` and `config.toml.example`. Of the package's 109 tracked files, 106
are renames: `src/feature/api/<feature>/` loses the retired `api/` segment,
`src/feature/rpc/identity-rpc.{hxx,cc}` becomes
`src/app/rpc/identity-rpc-service.{hxx,cc}`, and the four suites keep their
names in the service's `tests/unit/`. The three that are not renames are the
package's own `AGENTS.md`, `CONTEXT.md` and `CMakeLists.txt`, which the
service's own replace. Nothing was dropped: 0 of the 109 have no target.

The composition is new because it never existed: `src/app/main.cc` loads
`config.toml`, applies the identity schema, back-fills the `person.status`
column, installs the four filters, registers the five controllers, connects
NATS (the change-feed sink and its drain), serves `/health` with the NATS and
face extras, owns the sync-control leg to `argus-sync`, initialises the cert
service and the face engine, and runs the `argus.identity.v1` gRPC server on
`[server] grpc_port` (7040) beside the TLS HTTP listener on `[identity] port`
(7044). `argus_service(NAME argus-identity … PORTS 7044 7040)` declares both,
and the `[identity]` section is now the service's own `host`/`port`/`plain`/
`db`/`schema`/`rpc_secret`.

Two listeners, one process is the same shape `argus-auth` took in 3b, including
its refusal to start: a listener reachable beyond loopback with an empty
`rpc_secret` is `LOG_FATAL` and `_exit(1)`, because that secret is what tells
the fleet's other services that this one is the identity authority.

### The gateway keeps the edge and stops hosting

The four native paths leave `gatewayNativePaths()` and become a proxy route in
the gateway's route table — `/invitation`, `/pairing`, `/portrait-preview` and
`/user`, `max_segments = 4`, `validate_cert = false`, backend
`identity.proxy_url` (`https://127.0.0.1:7044`). The gateway's `[identity]`
section goes from `db`/`schema`/`rpc_host`/`rpc_port` to `proxy_url` + `target`
+ `rpc_secret`, `[face] enabled` leaves its template with the face engine, and
`services/gateway/src/identity/` (four files) is deleted along with the
identity block in its CMakeLists. The compose block loses the identity
database mount, the identity schema mount and the models mount; the gateway
keeps the TLS termination, the LAN gate, the reverse proxy and its own
schema — the edge, not the domain.

This is deliberately the same move 3b-2 made for `/auth`, so the gateway's
route table now has one entry per owning service and no native domain handler
outside `camera-stream` and `health`.

### The pull leg moves onto the wire

The sync engine's in-process read of identity's repositories cannot survive the
extraction under rule 27: sync may not open the identity database. The leg is
now the same shape the camera, productivity and notification legs already had.

- `packages/contracts/proto/argus/identity/v1/sync.proto` — a second proto in
  the identity contract, `SyncService.PullTable` with a `TablePull` body per
  table (`user`, `user_invitation`, `person`), the same
  `required_create`/`required_deleted`/`find_last_created`/`find_last_deleted`
  flags and the same `SyncRange` the sibling legs speak.
- `packages/clients/identity/src/identity/identity-sync-client.{hxx,cc}` — a
  second stub beside `IdentityClient` (two stubs, two clients, neither wrapping
  the other), one method `pullTable`, `kPullTimeoutMs = 5000`, the fleet secret
  on every call.
- `services/identity/src/app/rpc/identity-sync-rpc-service.{hxx,cc}` — the
  server side, registered on the same gRPC listener as `IdentityService`.
- `services/sync/src/feature/transport/infra/identity-sync-source.hxx` and
  `identity-sync-gateway.{hxx,cc}` — the fourth pull-source port and its
  adapter, wired by the socket registrar; `SynchronizedService` loses the three
  repositories, the `repoFor` branches and the scope, and pages the three
  tables through `syncWithRepo` with an empty base filter.
  `SyncErrors::IdentitySyncUnavailable` (503) joins the sync contract's catalog
  for the case where the leg is unconfigured.

What the leg must not lose is the rule-7b scope, and it does not: it moves to
the side that owns the rows.

```cpp
if (!role_access::hasAccess({.role = role, .table = *table,
                             .perm = RolePermission::Read}))
  → PERMISSION_DENIED

SyncFilter scope;
if (*table == TableName::User && role != UserRole::Owner &&
    role != UserRole::Guard)
  scope.userId = *caller;
```

The four roles resolve the way `role_access` says they should: Owner and Guard
receive the whole directory (four rows), Resident and Guest receive only the
row whose id is the caller, Guard/Resident/Guest are refused `UserInvitation`
(no `kTableAccess` entry) and Guest is refused `Person`. The scope rides into
the repository's own `SyncFilter`, so `find`, `findDeleted`, `findLast` and
`findLastDeleted` are all narrowed by it — `FIND_LAST_FOR_USER` and
`FIND_LAST_DELETED_FOR_USER` exist for exactly the `findLast` case, which a
naive `AND id = ?` insertion could not express.

**The fleet secret.** The pull leg shares its listener with
`IdentityService`, whose eleven methods all verify `x-argus-fleet`, and
`IdentitySyncClient` sends it from sync's `[identity] rpc_secret` — so the
service must verify it too, or a caller that can set three metadata headers
would page the people directory of a listener the rest of the fleet gates. The
port now takes `Dependencies{.fleetSecret}`, `main.cc` passes `rpc.secret`, and
`fleetAuthorized` runs before the caller and role checks. It was written from
the start; the first draft checked only `callerUserId`, and
`services/identity/AGENTS.md`'s rule 3 already claimed the listener was
"fleet-gated", which is what surfaced it. The gate is pinned from both sides by
the suite below.

### The six projects that pulled the package stop pulling it

`packages/identity` was pulled into six service trees that no longer need a
single header of it: camera, gateway, guard, notification, productivity and
sync. Their own code reaches identity through `argus::clients::identity` (the
lib/auth filters and the guard's directory were already doing so), so the
`add_subdirectory` blocks go — and with them five dead `cert` pulls that had
been riding the same guard.

Removing the pull exposed more than a dead include path. Four of those trees
(notification, productivity, guard, sync) had been compiling their own copy of
everything the package's closure dragged in — an eleven-line ncnn configuration
block with `add_subdirectory(third_party/ncnn)`, and an unused
`find_package(OpenCV CONFIG REQUIRED)`. Both were dead the moment the package
left, and both are removed: the measured closure of each of the four now
contains zero ncnn and zero OpenCV steps (`ninja -n all`, then the object list),
neither binary carries an AI symbol, and the sentences in
`services/{notification,productivity}/CONTEXT.md` that justified the block are
rewritten to say what is now true. `docs/architecture/build-model.md`'s
third-party table follows: `ncnn` is compiled by camera and identity only (it
listed seven), `stb` by identity alone (it listed three).

### Deploy, scripts and docs

`argus-deploy/config.identity.toml.example` is new, and the compose gained an
`argus-identity` block (both ports published on `127.0.0.1` only, the identity
data directory, the identity schema, the certs read-only, the models
read-only, the NATS and `rustfs-init` dependencies, a `/health` healthcheck).
`scripts/lib/common.sh` adopts the new template, `scripts/setup.sh` writes it,
`scripts/provision-host.sh` provisions the service, `scripts/build-all.sh`
lists `services/identity` and builds `argus-migrate-identity` as its extra
target, and `scripts/lib/check-deps.py` follows the moved edges. The docs that
name the package (`packages/identity`'s rows in the root `AGENTS.md`,
`docs/architecture/*`, `docs/operations/configuration-keys.md`, the two
`CONTEXT.md` files that describe the gateway's hosting) follow the service.

The service's own templates carry the keys the service reads, which two review
findings proved they did not at first: `jwt.refresh_secret` (empty makes
`JwtService` throw at construction, so the container would not have booted) and
`[mdns] port` (the pairing and invitation answers publish it; without the key
they advertised port 0).

## The review

Two adversarial reviews ran against the unit's tree: one on the extraction
itself (the layout, the moves, the wiring, the deploy and what the six removed
pulls did to the test graph) and one on the documentation sweep (every claim
the unit's prose makes about paths, ports, counts and owners, checked against
the tree). Both were treated as claims: each finding was reproduced against the
source before it was acted on. Eight findings came back, all eight real — six
fixed, one recorded below, one a caveat about the tree the review had run
against, which the full run under Verification closes. Two of the eight were
then surfaced by the unit's own machinery rather than read off the page: the
gate's first run stopped on the phantom target below before it built anything,
and the identity project's first test run failed on a fixture bug the reviews
had not seen.

Fixed after reproduction:

- **The templates omitted `jwt.refresh_secret`** — the gateway's identity
  section had carried it for the package; the service's own template did not,
  and `JwtService`'s constructor throws on an empty refresh secret. A container
  deployed from that template would have died at boot.
- **The pairing and invitation answers advertised port 0** — `[mdns] port` was
  read and never present in a template, so the QR answers carried the 0 the
  reader defaults to. Both keys now ship in `config.toml.example` with the
  deploy value.
- **Docs claimed no `#include` changed** — 14 files did, all of them on the
  retired `api/` segment. The prose now says so.
- **`services/identity/CONTEXT.md` claimed an mdns advertiser** the service
  does not run. Corrected to what it does own: the `mdns.port` key its two
  answers publish.
- **A false ncnn justification, and four orphaned ncnn pulls** — see above;
  this one changed four other services' build files, not just prose.
- **`argus::lib::grpc` is not a target this tree declares** — the RPC module's
  `DEPENDS` named it and `check-deps.sh` refuses an unresolved declaration, so
  the whole gate stopped before it built anything. The real name is
  `argus::lib::grpc-health` (the health stubs are the only target the package
  exports, and it is what auth, sync, voice and camera already link).
- **`<errors/response-exception.hxx>` without `argus::lib::errors`** — the
  second half of the review's rule-25 finding; both halves are now satisfied,
  the include's package declared in the same list.
- **`identity-config-test` read an empty TOML** — the fixture parsed the file
  while its own `std::ofstream` was still open, so the two cases that assert
  non-default values read the defaults instead. The house fixture
  (`writeConfig()` in a scope, then `ConfigService::load`) replaced it.

Recorded, not fixed:

- **The boot-time route-surface assertion did not come across.** The package's
  registrar asserted that its four paths were registered before the process
  served; a service's `main.cc` registers five controllers and asserts nothing.
  That is the convention `argus-auth` set in 3b, and the assertion's value — a
  typo'd route failing loudly at boot — is worth having back, but as a shared
  check rather than one service's private one. It is on the flag list below.

The last of the eight was a caveat rather than a defect: the review had run
against a tree whose libraries were rebuilt incrementally, and it asked for a
full run on the frozen tree. That run is the one under Verification.

## What proves it

**The pull leg's own suite.** `services/identity/tests/unit/identity-sync-rpc-test.cc`
is new — the identity leg had no test at all, while camera and productivity
each ship one for theirs. It boots the real Drogon loop, applies
`database/schema.sql` to a throwaway SQLite file, seeds an owner, a resident, a
guard, a guest, a soft-deleted user, one invitation and one person, stands up
an in-process `IdentitySyncRpcService` on `127.0.0.1:0`, and then drives it
through `IdentitySyncClient` (the SDK, not a raw stub) and asserts:

- the user directory per role — Owner and Guard see all four live rows,
  Resident and Guest see exactly their own row;
- the deleted path is scoped too — a Guest's tombstone pull is empty while the
  Owner's carries the deleted user with its `deleted_at`;
- `UserInvitation` is Owner-only, refused for Guard, Resident and Guest;
- `Person` is served to a Resident and refused to a Guest;
- `find_last_created` answers the newest live row for the Owner and *the
  caller's own row* for a Resident — the scope on the paging leg, which is the
  one a repository that ignored `filter.userId` in `findLast` would fail;
- an empty request (no table branch) is `INVALID_ARGUMENT`;
- the fleet-secret gate both ways: a second harness configured with a secret
  serves a client that sends it, refuses one that sends a different value, and
  refuses a raw stub that sends no metadata at all (`UNAUTHENTICATED`).

**The six pulled-out trees lost no coverage — measured, not assumed.** The
whole-gate test collection was compared before and after the unit (the last
full run of 3b-2, 18 projects and 490 test executables, against this unit's
full run):

| Project | Suites lost | Which |
|---|---|---|
| gateway | 8 | the five identity suites, `storage-vocabulary-test`, `validation-dsl-test`, `voice-contract-vocabulary-test` |
| sync | 7 | the five identity suites, `cert-san-test`, `storage-vocabulary-test` |
| camera | 8 | the five identity suites, `cert-san-test`, `sync-client-test`, `voice-contract-vocabulary-test` |
| productivity | 9 | the five identity suites, `cert-san-test`, `storage-vocabulary-test`, `sync-client-test`, `voice-contract-vocabulary-test` |
| notification | 9 | the same nine as productivity |
| guard | 8 | the five identity suites, `cert-san-test`, `sync-client-test`, `voice-contract-vocabulary-test` |

49 collections fewer, and **not one of the 49 names left the repository**: every
one of them still runs in this tree, the five identity suites in the `identity`
project (whose executables grew from 30 to 32 with this unit's two new suites),
and the rest in the projects the table's own rows name — `storage-vocabulary-test`
in three, `validation-dsl-test` in eleven, `voice-contract-vocabulary-test` in
three, `cert-san-test` in three, `sync-client-test` in four. What the six trees
lost was the *second* copy of a suite that the removed pull had been dragging
in.

**The move's fidelity.** Of the 109 tracked files under `packages/identity` at
HEAD, 89 are byte-identical at their new relative path and 14 differ only in
the include lines that spelled the retired `api/` segment. Three changed
content for reasons other than their path — `identity-rpc.cc` (the service's
own entry point), and the migration tool's `CMakeLists.txt` and
`identity-migration.hxx` — and three are the package's `AGENTS.md`,
`CONTEXT.md` and `CMakeLists.txt`, which the service's own documents replace.
Nothing else was touched, which is what the include root promises: every
`#include` was already relative to `src/`.

## Verification

`./scripts/build-all.sh dev --only identity` builds the service and runs 32
suites, all green, with no first-party compiler warning:

```
100% tests passed, 0 tests failed out of 32
```

The whole-tree gate — `./scripts/build-all.sh dev`, all eighteen projects, both
rule gates before the build and the clang-tidy scan after it — was run on the
finished tree:

```
check-comments: 1330 files checked, 0 comments
check-deps: 78 declarations, 658 edges, 0 forbidden, 0 cycles, 0 unresolved,
            23 edges deferred to phase 3 (264 third-party mentions over 22 roots)
```

**The first full run built and tested everything and failed the tidy ratchet.**
Its build phase was green on all eighteen projects — 443 test executions, 0
failures, no compiler warning anywhere — and this service's own `--only
identity` run had compiled its sources from scratch under `-Wall -Wextra` with
nothing to report, which is what matters here because the unit's four changed
build files *reconfigure* their projects rather than recompile them. What
exited 1 was the run's last step:

```
risen: bugprone-easily-swappable-parameters  53 findings, baseline 52
risen: bugprone-optional-value-conversion  31 findings, baseline 30
risen: bugprone-unchecked-optional-access  233 findings, baseline 209
risen: modernize-avoid-c-style-cast  380 findings, baseline 359
risen: modernize-return-braced-init-list  50 findings, baseline 49
risen: modernize-use-designated-initializers  249 findings, baseline 247
risen: modernize-use-nodiscard  733 findings, baseline 730
risen: performance-enum-size  27 findings, baseline 25
risen: performance-unnecessary-value-param  50 findings, baseline 49
```

Nine checks, **56 findings over the floor**, and every one of them this unit's
own: the rises were resolved to lines rather than counted — a scan of the
finished tree dumped all 3012 findings, classified each by file provenance
against the move and the diff, and kept the ones sitting on lines this unit
created or rewrote. Every cause landed in eight files of this change, five of
them new. Each was fixed where it stood, never absorbed into the floor:

| check | raised by | closed with |
|---|---|---|
| `bugprone-unchecked-optional-access` | +24 | 23 sites in the new `identity-sync-rpc-test`: `CHECK_FALSE(x.has_value())` leaves the access unchecked, so every pull binds its answer first and bails out of the case (`if (!owner.has_value()) { FAIL("the owner pull answered nothing"); return; }`); the 24th is `synchronized-service.cc`'s fan-out `*field`, which needed the optional bound to a local — `if (!(body.*member).has_value())` on a member-pointer expression does not satisfy the check, and `const auto& field = body.*member;` removed all five findings of that loop, three of them older than this unit |
| `modernize-avoid-c-style-cast` | +21 | the new `identity-sync-gateway`'s row serializers: `Json::Int64(x)` → `static_cast<Json::Int64>(x)` |
| `modernize-use-nodiscard` | +3 | `[[nodiscard]]` on `IdentitySyncClient::pullTable`, the suite harness's `listening()` and `target()`, and `RecordingIdentityClient::listNotifiableUsers` |
| `modernize-use-designated-initializers` | +2 | the identity suite's `Sqlite3Config` and the four `CameraNotificationPolicy::Config` lists the gateway suite wrote positionally |
| `performance-enum-size` | +2 | `IdentitySyncTable` and the gateway's private `Mode` pinned to `std::uint8_t` |
| `bugprone-easily-swappable-parameters` | +1 | `IdentitySyncClient` took `(std::string target, std::string fleetSecret)` — two adjacent strings of one type; replaced by `IdentitySyncClientConfig`, the struct shape the other clients already use |
| `performance-unnecessary-value-param` | +1 | the same struct, moved from in the constructor, which is what the check asks for |
| `bugprone-optional-value-conversion` | +1 | `scope.userId = *caller;` → `= caller;` |
| `modernize-return-braced-init-list` | +1 | `return {503, SyncErrors::IdentitySyncUnavailable};` |

The three projects whose sources these fixes touch were rebuilt green — identity
32/32, sync 44/44, gateway 27/27, 0 errors and 0 warnings — and the scan re-run
over the whole tree:

```
check-tidy: 534 TUs, 2945 findings over 45 checks, baseline 2957; 4 checks below it
```

exit 0, nothing risen, and the tree reads **12 findings below the floor it was
measured against**.

**The floor is re-recorded on the verified tree, and it moved down.** Seven of
the nine risen checks returned exactly to their recorded floor —
`modernize-avoid-c-style-cast` 359, `modernize-use-scoped-lock` 279,
`performance-enum-size` 25, `bugprone-easily-swappable-parameters` 52,
`modernize-return-braced-init-list` 49,
`performance-unnecessary-value-param` 49,
`bugprone-optional-value-conversion` 30 — which is what makes the 56 above a
measurement rather than a count. Four fell below it, because the fixes removed
older findings of the same checks along with the risen ones — the fan-out
loop's local binding took three that predate this unit, and the positional
`Config`/`Sqlite3Config` lists were already charged to the record — and
because one check lost a finding with the files the unit deleted:

```
bugprone-unchecked-optional-access  209 -> 205
modernize-use-designated-initializers  247 -> 241
modernize-use-nodiscard  730 -> 729
modernize-use-starts-ends-with  45 -> 44
```

The tree reads **2945 findings over 45 checks** where the floor recorded 2957,
the difference being exactly those four deltas (−12), and
`./scripts/check-tidy.sh --write-baseline` re-recorded the floor at `534 TUs`
(was 529: the unit adds translation units, and the gate only fails when it sees
fewer).

## Flagged, not fixed

- **The camera and productivity pull legs verify no fleet secret.** Their
  `PullTable` checks `callerUserId` — three metadata headers a caller sets —
  and nothing else, while identity's now verifies `x-argus-fleet` because it
  shares a listener with a fleet-gated surface. Whether the other two should is
  a decision about their listeners' posture, not this unit's extraction; it is
  recorded here because the asymmetry is now visible and because identity's own
  leg would have shipped without a gate had the sibling files not been read.
- **The boot-time route-surface assertion** the package's registrar carried is
  not part of any service's `main.cc` today. The right home is a shared check
  over a service's declared routes, not one private assertion; until there is
  one, a mistyped route fails at request time rather than at boot.
- **The gateway's documents still describe a gateway that hosts identity.** Its
  `AGENTS.md` layout block says `src/controllers/` holds "health today; identity
  domain in F1-3" and that the template is "minimal `[gateway]` + `[nats]`",
  and its `CONTEXT.md`'s "What it owns" still lists an "Identity domain (F1-3)"
  bullet (the surface "served from the `argus_identity` static library", the
  registrar at `src/identity/identity-registrar.cc`), an `identity.db` bullet
  and a device-identity bullet that says the mode is "shared with the legacy
  through `argus_identity`". The unit updated the parts it made false in the
  file's history sections (the change sink's drain leaving with the owner, the
  RPC service's home, the `argus.identity.v1` leg) but not those three bullets,
  whose staleness partly predates it. Left alone deliberately: the service is
  deleted in Phase 3d step 1, and rewriting a 600-line legacy document that
  dies next is not this unit's work.
- **`argus-deploy/CONTEXT.md`'s memory-catalog paragraphs** still describe the
  catalog replica as a future arrival ("when argus-llm hosts the memory
  catalog"); that staleness belongs to the memory package's phase, not this
  one, and this unit's edits to the file are the ordering, service-table and
  schema-mount paragraphs that named identity's old hosting.
- **The mint-time SAN gap.** `mdns.name` and `remote.hostname` are read for the
  pairing answers but no certificate is minted with them as SANs; the identity
  service inherits the gateway's behaviour unchanged. Recorded because a
  service that answers over TLS with a name it does not certify is the shape a
  later phase will want to fix once, not per service.
- **Stale build-tree artifacts naming `packages/identity`.** An incremental
  build directory still holds `CTestTestfile.cmake` fragments and object
  directories from the retired package; a clean configure does not. Not worth a
  deletion pass — the next fresh build tree is the fix.
- **`services/voice`'s unused `third_party/stb` include path** is now the only
  stb consumer that does not need it; the include path is harmless and the
  service does not compile the header.
- **The new pull-source enum carries an underlying type its two siblings do
  not.** `IdentitySyncTable` is pinned to `std::uint8_t`, while
  `CameraSyncTable` and `ProductivitySyncTable` beside it declare none — all
  three carry the same `performance-enum-size` finding, the older two inside
  the recorded floor, so the ratchet is green either way. Pinning them is two
  lines in two headers this unit does not otherwise touch; left for whoever
  edits those files next, recorded here because the unit wrote the third one
  and chose to differ deliberately.
- **Standing from earlier units** and unchanged: the frontend's
  `/auth/has-admin` assumption, the untracked `go2rtc.yaml` at the repository
  root, the gRPC 4 MB receive limit against the 10 MB DTO cap, the
  `sync control_secret` provisioning filler, and the `{Feature}FeatureService`
  naming that a later step renames.

## Files

217 paths in the working tree, of which 28 are new files and 7 are deletions;
89 are renames and 17 are renames with edits. The untracked `go2rtc.yaml` at
the repository root, which carries RTSP credentials, is not part of this unit:
it is excluded from that count and stays unstaged.

- **`services/identity` (20 new, 106 renamed from the package)** — the new
  service: `src/app/{main.cc,rpc/}`, `src/config/`, the four feature modules
  with their `CMakeLists.txt`, `src/shared/`, `database/schema.sql`,
  `config.toml.example`, `Dockerfile`, `AGENTS.md`, `CONTEXT.md`,
  `scripts/provision.sh`, `tools/migrate-identity/`, the six suites — of which
  `identity-config-test.cc` and `identity-sync-rpc-test.cc` are new, as are
  `identity-sync-rpc-service.{hxx,cc}`.
- **`services/sync` (21)** — the identity pull source, gateway, and the
  registrar wiring; `SynchronizedService` without the three repositories;
  `sync-config` with the identity leg's target and secret; `main.cc`, the
  template, `CONTEXT.md`, `AGENTS.md` (four pull sources now).
- **`services/gateway` (15)** — the identity proxy route and its config keys,
  the four deleted hosting files, the trimmed `CMakeLists.txt` and `main.cc`,
  the template, `CONTEXT.md`, and the camera-notifier edits the extraction
  forced.
- **`packages` (14)** — six paths under `clients/` (the two new
  `identity-sync-client` files, identity's `CMakeLists.txt`, three
  `AGENTS.md`s), four under `contracts/` (the sync proto, `sync-errors.hxx`'s
  new catalog row, two `AGENTS.md`s), three under `lib/` (two auth documents
  and cert's `AGENTS.md`) and `packages/memory/config.toml.example`.
  `packages/identity`'s own 109 paths are the 106 renames and the 3 deletions,
  and they are counted under `services/identity` above.
- **`argus-deploy` (13)** — the new identity template, the compose service
  block and the gateway's unmounted paths, and the nine other templates and
  documents that name identity.
- **`scripts` (5)** and **`docs` (9) plus the root `AGENTS.md`** — the project
  list, the dependency checker, the provisioning and setup passes, five
  architecture documents, three operations documents, and this report.
- **The four services with a build-file path each** — `productivity` (3),
  `notification` (3), `guard` (1) and `sync` (in the 21 above): the removed
  pulls, and with them the dead ncnn block and the unused OpenCV lookup.
  `camera` (2), `auth` (1) and the root `AGENTS.md` carry the remaining
  follow-on edits.
