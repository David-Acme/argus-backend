# Sub-step 3a-1d — `packages/sync` retired into `services/sync`

The fourth and last sub-step of Phase 3a row 1. Its clause, as the row's own
report records it (`f3-1a-sync-contract-vocabulary.md:214`): **"`packages/sync`
deleted; `memory` repointed; the doc sweep; the gate; the review"**. Sub-steps
a1 (`5687dac`), a2 (`b8b2bdc`), b (`ef67014`) and c (`990691f`) are committed,
and c left the engine deliberately where it was: the new service *composes*
`argus_sync` rather than owning it, so the commit that changed who serves
`/sync` did not also move the protocol code — two independent risk surfaces,
two commits.

## Pre-state, measured

`packages/sync` holds **42 files** (16 `.cc`, 24 `.hxx`, `CMakeLists.txt`,
`.gitignore`), 2928 lines of source and 180 more in the two build files, one
target `argus_sync`, and **no suite of its own**
— c moved all five to `services/sync/tests/`, and its build tree (3.2 G `dev` +
249 M `prod`) was deleted with them because its `compile_commands.json` named
files that no longer exist.

It is a package with no project: the orchestrator's array does not build it
(`scripts/build-all.sh` composes `services/sync`, which adds it by path), so
nothing would fail if it drifted. What keeps it honest today is one
`add_subdirectory` in one CMakeLists.

**What it contains, and who reads each piece** — the measurement every home
below is decided by:

| Piece | Files | Read by |
|---|---|---|
| `feature/socket/sync/socket/sync-forwarder.{hxx,cc}` | 2 | the engine, `services/sync` (3 files), `services/camera` (2), `services/gateway` (2) |
| `feature/socket/sync/socket/sync-socket.{hxx,cc}` | 2 | the engine, `services/sync/src/app/main.cc` via the registrar |
| `feature/socket/sync/services/sync-service.{hxx,cc}` | 2 | the engine (socket), the registrar |
| `feature/socket/sync/services/synchronized-service.{hxx,cc}` | 2 | the engine (sync-service), `services/sync/tests/unit/audit-sync-read-test.cc` |
| `feature/socket/sync/dtos/synchronized-dto.hxx` | 1 | synchronized-service, `audit-sync-read-test.cc` |
| `shared/contracts/{camera,notification,productivity}-sync-source.hxx` | 3 | the engine's socket/service pair, the three gateways in `feature/transport/infra/`, `audit-sync-read-test.cc` |
| `shared/repositories/{audit-log,user-audit-log,user-action-log}/` | 9 | `synchronized-service` (the audit pages) and `fanout`'s `AuditFanOut` (the writes) |
| `shared/schemas/{audit-log,user-audit-log,user-action-log}/` | 6 | their own repositories |
| `shared/services/{audit-log,user-audit-log}/` | 4 | `fanout`'s `AuditFanOut` |
| `shared/services/room/room-manager.{hxx,cc}` | 2 | the transport's `sync-service`, `fanout`'s `sync-fan-out`, `app/main.cc` (the lifecycle object), two suites |
| `shared/repositories/event/` + `shared/schemas/{event,person-event}/` | 7 | `synchronized-service` alone; `event-sync-empty-test.cc` exercises it |

`argus_sync` is linked at **eight sites in six CMakeLists**: `camera-core`
(`services/camera/CMakeLists.txt:244`), `gateway-core` (PUBLIC,
`services/gateway/CMakeLists.txt:143`), the service's three modules
(`feature/transport/CMakeLists.txt:10`, `feature/fanout/CMakeLists.txt:9`,
`app/rpc/CMakeLists.txt:6`) and three of its seven suites
(`sync-surface-test`, `audit-sync-read-test` and `event-sync-empty-test`, at
`services/sync/tests/CMakeLists.txt:13/28/68`;
`notification-delivery-inbox-test` and `voice-leg-test` never needed it).

**The two outside consumers use one header, and only one header.** Measured:
`services/camera/src/controllers/camera-media-service.hxx` and
`camera-media-socket.cc` take `SyncFrameInput` and `sendSocketFrameError` for
the camera's own `/media` socket, and
`services/gateway/src/sync/camera-stream-relay.{hxx,cc}` implements
`SyncForwarder` with `camera-stream-socket.cc` doing the same. Nothing else
outside `services/sync` names the engine at all. That single fact decides the
engine's whole destination: **everything else in `packages/sync` has exactly
one owning feature and moves into the service; `sync-forwarder.hxx` has two
outside consumers and cannot, because rule 27 forbids a service including
another service's `src/`.**

`memory`'s clause is **already discharged**, by c and not by d: `catalog-replica.cc`
includes `<sync/module-audit-event.hxx>` and `<sync/sync-operation.hxx>`, both
of which are tier-2 vocabulary in `packages/contracts/sync` — never the engine.
What memory did link was `argus::audit`, for the `<nats/...>` include roots
that reached it through `argus::audit`'s PUBLIC chain to `argus::socket` and
from there to `argus::lib::nats`; c repointed it to `argus::lib::nats` directly
in the same commit that deleted those targets (`packages/memory/CMakeLists.txt`,
`+ argus::lib::nats` / `- argus::audit`). d's job is to record that, not to
redo it — and the measurement above is how it is checked: no file under
`packages/memory/` names the engine or any of its headers today.

## Design

### `sync-forwarder.hxx` moves to `packages/contracts/sync`

The header declares three things: `SyncFrameInput` (a connection, the parsed
frame, the raw text), `SocketFrameError` + `sendSocketFrameError` (the
`{type:"<type>_error", status, error}` envelope), and the `SyncForwarder`
interface (`onConnect`, `forwardText`, `forwardBinary`, `onClose`) — the
side-channel seam of the `/sync` socket, which is exactly what
`VoiceGrpcRelay` and the camera/gateway media relays implement.

Its home is the sync contract, not a service:

- **Rule 27 is the binding constraint.** `services/camera` and
  `services/gateway` both compile this header today, and neither may include
  anything under `services/sync/src/`. A package is the only home that keeps
  both compiling.
- **`contracts/sync` is the right package.** The header is the sync domain's
  own WS vocabulary — the sibling of `SocketEmitDto` (`{operation, option,
  info}`, one direction of the same socket) which already lives there. Rule 25
  puts "the C++ types that cross the wire" in the contract that owns them, and
  this is the type a frame handler crosses with.
- **No new edge, no new tier question.** Tier 2 may depend on third-party
  explicitly, and `contracts/sync` already links `Drogon::Drogon`
  (`CMakeLists.txt`, in `DEPENDS`), four of its headers already
  `#include <drogon/...>` (`camera-change-sink.hxx`, `identity-change-sink.hxx`,
  `syncable.hxx`, `user-change-sink.hxx`), and every consumer of the forwarder
  header already links Drogon.

`contracts/sync` is `INTERFACE` by construction, so the one compiled function
in it — `sendSocketFrameError`, four statements that build a `Json::Value` and
call `conn->sendJson` — becomes **`inline`** in the header. That is the change
the move forces, and it is smaller than making the contract carry a compiled
object (which would break the header-only property nine of the ten contracts
share, and the plan's own count of them).

### The pull-source contracts move into the feature, not into `shared/`

Rule 23's 2+ rule decides this one, and the measurement is unambiguous: the
three `*-sync-source.hxx` contracts are read by `sync-socket.hxx`,
`sync-service.hxx`, `synchronized-service.hxx` and their three implementations
— **the transport feature, all of them**. No other feature of this service
names them, so `shared/contracts/` (today's spelling, which was correct in a
package that had no features) would be structure ahead of its consumer.

They land in `feature/transport/infra/` beside their implementations, which is
the folder rule 23 gives an adapter and its interface. That puts
`camera-sync-source.hxx` (the port, declaring `CameraSyncSource`) and
`camera-sync-source.hxx` (the adapter, declaring `CameraSyncGateway`) in one
directory, so **the adapters are renamed after the classes they declare**:
`infra/{camera,notification,productivity}-sync-gateway.{hxx,cc}`. The old name
disagreed with its own class — the file said *source* and declared a
*Gateway* — so the rename fixes a naming defect rather than trading one for
another. The ports keep the canonical `*-sync-source.hxx` names their classes
answer to.

### Everything else lands where its reader is

```
services/sync/src/
├── app/main.cc  app/rpc/                      unchanged
├── config/sync-config.{hxx,cc}                unchanged
├── feature/transport/
│   ├── controllers/sync-socket.{hxx,cc}       was feature/socket/sync/socket/
│   ├── dtos/synchronized-dto.hxx              was feature/socket/sync/dtos/
│   ├── services/{sync-service,synchronized-service}.{hxx,cc}
│   │                                          was feature/socket/sync/services/
│   ├── infra/{camera,notification,productivity}-sync-source.hxx     the ports
│   ├── infra/{camera,notification,productivity}-sync-gateway.{hxx,cc}
│   │                                          the adapters, renamed
│   ├── infra/{sync-socket-registrar,voice-grpc-relay}.{hxx,cc}      already here
│   ├── repositories/event/                    single reader: synchronized-service
│   └── schemas/{event,person-event}/          single reader: the event repository
├── feature/fanout/
│   ├── repositories/delivery-inbox/           already here
│   └── services/                              already here, plus the audit set's
│                                              {audit-log,user-audit-log}-service.{hxx,cc}
└── shared/
    ├── infra/notification-row-json.hxx        stays: two features read it
    ├── repositories/{audit-log,user-audit-log,user-action-log}/
    ├── schemas/{audit-log,user-audit-log,user-action-log}/
    └── services/room/
```

- The **audit repositories and their schemas** are read by the transport's
  `SynchronizedService` (all three) and the fan-out's `AuditFanOut` (the
  action journal) — two features, so `shared/` is earned. The **two writers
  over them** are not: measured, `audit-log-service.hxx` and
  `user-audit-log-service.hxx` have exactly one reader, `AuditFanOut`, so rule
  23 puts them in `feature/fanout/services/`, beside the class that holds them.
  (This is the review's finding, applied in the execution and recorded below.)
- **`RoomManager`** is read by the transport, the fan-out and `app/main.cc`'s
  lifecycle object: `shared/services/room/`, same spelling as today.
- **`notification-row-json.hxx`** stays in `shared/infra/`: measured, its two
  readers are `transport/infra/notification-sync-source.cc` and
  `fanout/services/notification-delivery-consumer.cc` — one per feature, which
  is what c's placement assumed and this sub-step re-measures rather than
  trusts.
- The **event repository and its two schemas** have one reader each
  (`SynchronizedService`, and the repository itself), so they follow it into
  the feature. `event-sync-empty-test.cc` and `audit-sync-read-test.cc` reach
  them through the feature's own include root, which is legal for a suite: the
  tests are the service's own, not a second feature.

### Module composition: `argus_sync` dies, three modules take its place

Rule 25's "the folder IS the module" and rule 24's "no directory without a
consumer" settle the targets. `sync-transport` grows from five sources to
eleven: the three `infra/` adapters it already compiled keep their place under
their new names, and the six it gains are the socket controller
(`controllers/sync-socket.cc`), the two services
(`services/{sync,synchronized}-service.cc`) and the three translation units no
module compiled any more once the package died
(`repositories/event/event-repository.cc`, `schemas/event/event-schema.cc`,
`schemas/person-event/person-event-schema.cc`). The DTO and the three ports are
headers and add no source. Two new modules are
declared in their own homes:

- `src/shared/repositories/CMakeLists.txt` → `argus_module(NAME
  sync-repositories …)` over the three repositories **and their three
  schema `.cc`** — the shape `services/camera/src/shared/repositories/CMakeLists.txt`
  already uses, and for the same reason: the schemas' translation units have no
  other compiler, and the three are one readership.
- `src/shared/services/CMakeLists.txt` → `argus_module(NAME sync-services …)`
  over `room` alone once the review moved the two audit writers into `fanout`
  (see the execution below); the module keeps the folder's name, the way
  `camera-repositories` does.

The service root adds those two directories, drops the `argus_sync` block, and
`argus_service` (NAME `argus-sync`, ports 7025/7041) keeps its shape. The
standalone configure loses the `packages/sync` subtree and keeps identity and
ncnn — which it needs directly now that the transport compiles
`SynchronizedService`, whose `UserRepository` include comes from identity's own
tree.

### The two outside callers repoint to the contract

`gateway-core` and `camera-core` stop linking `argus_sync` and their guards go
with it — `if(NOT TARGET argus_sync)` around an `add_subdirectory` of the
package, at `services/gateway/CMakeLists.txt:94-96` and
`services/camera/CMakeLists.txt:105-107` as HEAD had them. Both already linked
`argus::contracts::sync` in the same list that carried the engine's target
(HEAD `services/gateway/CMakeLists.txt:145`,
`services/camera/CMakeLists.txt:256`), so the repoint costs one line each: those
lists now carry it at `:141` and `:251`. Two comments that described the old
edge say the new one instead — `camera-core`'s `/media` note (`:236-237`) and
the executable's "arrives transitively through camera-core's `argus_sync` pull"
(HEAD `:274-276`), which named a target that no longer exists.

## Open items carried into the execution

1. **The transitory database does not change.** `[sync] db =
   "database/identity.db"` and the four tables stay where sub-step 3c-2 will
   split them; this sub-step moves C++ only and touches no DDL.
2. **`event` and `person_event` still have no `CREATE TABLE` anywhere in the
   tree** while `EventRepository` and both schemas compile and serve the paging
   legs. Carried, not invented here.
3. **`SyncService` and `SyncSocket` both declare the same four setters**
   (`setForwarder`, `setCameraSource`, `setProductivitySource`,
   `setNotificationSource`) and the socket forwards each to its service. It is
   duplication, it is pre-existing, and the review decides whether this
   sub-step is where it goes: the move is mechanical, the fix is behavioural
   surface, and rule 24's "dead code, unused folders and duplicated copies are
   removed in the same change that introduces their replacement" applies to what
   this change introduces, not to what it relocates.
4. **The camera's own `/media` socket keeps the frame-error vocabulary** from
   `contracts/sync`. That is a deliberate cross-domain read of a tier-2 header
   and not a rule-27 breach: packages travel, service sources do not.

## As executed

### The package's 42 files

36 left under `git mv` — 35 into `services/sync/`, and `sync-forwarder.hxx`
alone into `packages/contracts/sync/` — 3 moved by hand onto the path their own
adapter had occupied (the three pull-source ports: the adapter was renamed to
`{camera,notification,productivity}-sync-gateway.{hxx,cc}` first, so git reads
the port's arrival as an edit of a path that already existed rather than as a
rename), and 3 deleted: `CMakeLists.txt`, `.gitignore` — the package's whole
build existence —
and `sync-forwarder.cc`, whose single function is now `inline` in the header it
belonged to. `packages/sync/` itself was removed once its directories were
empty, and it no longer appears in `ls packages/`.

The three adapters were renamed because the port and the adapter shared a
filename and a single destination path: `camera-sync-source.hxx` held the
interface (`class CameraSyncSource`) and `camera-sync-source.cc` the
implementation of a differently named class (`CameraSyncGateway`). The rename
fixes a name/class mismatch that predates this sub-step.

### The include rewrite

41 files include at least one of the moved headers — resolved against the
tree's include roots, not grepped by basename, because `event-repository.hxx`
and `event-schema.hxx` have namesakes in `services/productivity` — and 4 of the
41 are outside `services/sync`: `services/camera`'s `/media` controller and
service, and `services/gateway`'s camera-stream relay and socket, all four for
`sync-forwarder.hxx` alone. Inside the service the spellings are
`<feature/transport/{controllers,dtos,services,infra,repositories,schemas}/…>`
for the feature's own code and `<shared/{repositories,schemas,services}/…>` for
what two features read. A tree-wide grep for the old spellings —
`feature/socket/…`, the three `shared/contracts/*-sync-source.hxx`,
`<shared/repositories/event/…>`, `<shared/schemas/{event,person-event}/…>` and
the three audit log homes — returns no hit in live code. Two prose mentions
of `packages/sync` survive by design, each describing it in the past tense
(`services/sync/CONTEXT.md:15`, `packages/contracts/sync/AGENTS.md:104`), and
`docs/history/` keeps every
record of the state before, because that is what those files are.

### Three fixes the build found, not the design

1. **The three adapter `.cc` files included their own header by its old name.**
   Line 1 of each said `#include "…-sync-source.hxx"`, which after the rename
   resolves to the port — a header that declares the interface, not the class
   the `.cc` defines. The result was 491 lines of `ProductivitySyncGateway::`
   definitions with no declaration: four errors at the tail and a set of
   `-Wunused-function` warnings for the anonymous-namespace helpers above them.
   Each now includes its own `-sync-gateway.hxx`.
2. **`app/main.cc` constructs the three gateways, so it includes the gateways.**
   The mechanical rewrite sent it to the ports; `make_shared<CameraSyncGateway>`
   then failed to compile. This is the one site where the mechanical rewrite was
   wrong rather than incomplete.
3. **`argus::sync-services` needs `argus::lib::auth`.** `room-manager.cc`
   includes `<auth/role-access.hxx>`, which lives in `packages/lib/auth/src` and
   not in the `contracts/auth` header set the module already named.

The transport module also gained the three translation units no module compiled
any more once the package died: `repositories/event/event-repository.cc`,
`schemas/event/event-schema.cc` and `schemas/person-event/person-event-schema.cc`.
Without them the link fails on `vtable for EventRepository` — the header
declares it and nothing emitted it. They went into `argus::sync-transport`
rather than a module of their own: one reader each, and rule 24 forbids a
directory without a consumer.

### The standalone configure is the service's own business now

`services/sync/CMakeLists.txt` had been reaching `contracts/camera`,
`contracts/notification` and `contracts/productivity` through
`packages/sync`'s standalone block. That block is gone, so the service's own
`if(PROJECT_IS_TOP_LEVEL)` adds the three contract packages beside the
`contracts/voice` guard it already had; in the root build each arrives through
its owner, as before. Its `argus_service` line lists the two new modules
(`argus::sync-repositories`, `argus::sync-services`) and the `argus_sync`
subdirectory block is gone. The stale comment above the standalone block
("the engine package (argus_sync, still a package until sub-step d)") was
rewritten with it.

### The consumers and the two other spellings of `argus_sync`

`gateway-core` and `camera-core` lost the link and the `if(NOT TARGET
argus_sync)` guard that added the package by path; both already named
`argus::contracts::sync`, which is what the forwarder header needs. Three
comments elsewhere named the target as history and were rewritten, because a
comment naming a deleted target is a lie a reader cannot check: the executable
link comments in `services/productivity/CMakeLists.txt:185` and
`services/notification/CMakeLists.txt:170`, and
`services/camera/src/shared/repositories/CMakeLists.txt:5`. The camera's own
executable comment (`services/camera/CMakeLists.txt:269`) said `argus_identity`
arrived "through camera-core's argus_sync pull"; with the pull gone it says
what the edges are — `argus::clients::identity` riding `argus::lib::auth`.

`scripts/lib/check-deps.py` drops `packages/sync` from `IN_TRANSIT`, which is
the point of the sub-step for that gate: the service's edges are no longer
deferred to a later phase, they are classified as tier 5 and checked. The
docstring's list of unmoved packages (which still named `audit`, `room` and
`socket`, all deleted in earlier sub-steps) was corrected to the three that
remain: `identity`, `intent`, `memory`.

### The doc sweep

12 files, all live: the root `AGENTS.md` (the WS/sync DTO clause in rule 10,
`RoomManager`'s path in rule 18, the Tier-2 contract row, and five Key Files
rows that said "becomes `services/sync` (Phase 3a)"), `services/sync`'s
`AGENTS.md` (rule 9 rewritten from "the engine is a package until sub-step d"
to what is true now, the layout tree and the module list) and `CONTEXT.md`,
`services/gateway/CONTEXT.md`, `docs/architecture/wire-camera-media.md`, and
the package docs: `packages/contracts/{camera,notification,productivity,sync}/AGENTS.md`
and `packages/clients/{camera,notification,productivity}/AGENTS.md`.
`packages/contracts/sync/AGENTS.md` gained the `sync-forwarder.hxx` row, and its
three client-side neighbours had their includer paths and line citations
repointed from the pre-rename adapter names (`…-sync-source.hxx`/`.cc`) to the
landed ones — including a fan-in count corrected from 17 CMakeLists to the
measured 18.

### The review's findings, applied

The row's clause ends in "the review", and it earned its place: two of its
findings were structural, and both are fixed above rather than carried.

1. **A contract edge was declared by the wrong module.** `event-query.hxx:3`
   and `event-schema.hxx:6` — both under `feature/transport/` — include
   `<camera/event-severity.hxx>`, but `argus_sync-transport` never named
   `argus::contracts::camera`; it compiled because the sibling
   `argus::sync-repositories` carried the edge and a module propagates its
   include directories. The other direction was dead weight: no TU under
   `src/shared/repositories/` or `src/shared/schemas/` includes a `<camera/…>`
   header at all. The edge moved to the module that compiles those two TUs
   (`feature/transport/CMakeLists.txt:24`) and left the repositories module —
   which is precisely the fragility rule 25's per-module declaration exists to
   prevent, and it would have taken a silent compile break to surface it.
2. **Two single-reader services were in `shared/`.** Measured, the only
   includer of `audit-log-service.hxx` and `user-audit-log-service.hxx` is
   `feature/fanout/services/audit-fan-out.hxx`; the transport pages the audit
   tables through the *repositories*, never through the writers. Rule 23 gives
   a service one reader the feature, so both moved to
   `feature/fanout/services/` (flattened, since that folder is flat) and
   `argus_sync-fanout` compiles them. `argus_sync-services` is now
   `room/room-manager.cc` alone and lost its `argus::sync-repositories` edge,
   which nothing in it used: the room registry needs only
   `contracts/auth` (`UserRole`), `contracts/sync` (`TableName`) and
   `lib/auth` (`role_access`). The repositories and their schemas stay in
   `shared/`, where the two-feature measurement does hold.

Both fixes are `--only sync`-verified; the module list, the tests' include
spellings and the seven suites are unchanged by either.

Its doc findings were right too, and are corrected in the working tree: root
`AGENTS.md`'s two audit-writer Key Files rows repointed to `fanout` (they had
been written before the move above), its pull-source row's "one reader:
`SynchronizedService`" replaced with the three parties that actually hold the
ports (the socket, the engine, the registrar) — a fan-in count reads as a rule
23 claim it was never making — `services/sync/AGENTS.md`'s rule 9 and layout
paragraph, `services/gateway/CONTEXT.md`'s "composes the engine `packages/sync`
used to hold" (the gateway links the contract, not the engine), and the line
citations in `packages/clients/{camera,productivity}/AGENTS.md`, read off the
two files' diffs: camera's `camera-rpc` `:162`→`:158`, its transport citation
`:11`→`:19` and its add-by-path trio `llm :143`, `camera :132`→`:129`, `sync
:116`→`:125`; productivity's transport citation `:13`→`:21` and
`services/productivity` `:191`,`:279`→`:190`,`:278`. Every one of them had been
shifted by this sub-step's own later edits,
plus `packages/contracts/camera/AGENTS.md`'s "three CMakeLists link it", which
is four — `camera-core`, camera's shared repositories, `guard`'s module and
the sync transport.

Three of this report's own claims did not survive its review either, and are
corrected above: the rename accounting (35 into the service, not 36; one went
to `contracts`), `argus_sync`'s link sites (eight in **six** CMakeLists, with
three of **seven** suites), and the claim that a grep for the old spellings
returns nothing — it returns nothing in live code, two past-tense prose
mentions and everything under `docs/history/`, which is what those files are.

A fourth claim, "three prose mentions of `packages/sync`", was a miscount on
this sub-step's own later reading, caught in the closing review: there are two,
because `services/gateway/CONTEXT.md` no longer contains the string at all —
this change rewrote that sentence. The paragraph above lists the two. That same
review also falsified the before-values in this report's earlier parenthetical
about the client docs' citations (`:16`, `:18`, `:109` appear in neither file's
diff); the paragraph above now carries the values read off the diffs.

A fourth, found while the closing gate ran: the transport module "grows from
five sources to fifteen" was never fifteen — it is eleven (its five, minus
nothing, plus the socket controller, the two services and the three units the
dead package had been compiling). The DTO and the three ports are headers and
add no source. The paragraph above says eleven.

### The closing pass, after the review

The closing review found six things, all applied here rather than carried. Three
were prose corrections in this report and three were module declarations that
did not name what their own translation units include — the identical defect
class the earlier review had found for `argus::contracts::camera`, which is why
they were not left standing:

- **`argus_sync-transport` compiled `<sqlite/db-service.hxx>` and declared no
  sqlite.** `event-repository.cc:4` includes it; the module reached the include
  root through `argus::sync-repositories`'s `PUBLIC` chain. This one *is* this
  sub-step's: at HEAD the same translation unit was compiled by `argus_sync`,
  which declared `argus::lib::sqlite` itself. The edge is now declared at
  `feature/transport/CMakeLists.txt:30`.
- **`argus_sync-control-rpc` compiled `<grpc/grpc-server-identity.hxx>` and
  declared no gRPC runtime.** `sync-control-rpc-service.cc:5` includes it,
  reached today through `argus::clients::sync`. Pre-existing from 3a-1c, fixed
  here: `argus::lib::grpc-health` joins its `DEPENDS`, which is what
  `camera-rpc` and voice's module declare for the same include root.
- **`argus_sync-transport` declared `argus::contracts::voice` and nothing used
  it.** Measured: the transport's two `voice/…` includes
  (`voice-grpc-relay.{hxx,cc}`) resolve to `packages/clients/voice/src`, and
  the contract package holds one header, `voice-lang.hxx`, whose only three
  includers are its own suite, `packages/identity`'s auth service and
  `services/voice`. `clients/voice` does not link the contract either, so the
  edge was the sole provider of an include root nobody read. Dropped.

The three prose corrections:

- `services/sync/src/shared/services/CMakeLists.txt` and its `repositories`
  sibling were written with `${CMAKE_CURRENT_SOURCE_DIR}/../..` as their
  `INCLUDES` argument while the three modules at that same depth in the same
  service (`app/rpc` and the two features) say `../..`; all five now say
  `../..`. `services/camera`'s shared repositories is the only other place in
  the tree that spells it long, and a service reading its own depth two ways is
  noise.
- The repositories module's header comment called itself "the four tables' row
  code" while it compiles three — the delivery inbox is the fourth table and
  its repository is compiled by `sync-fanout`, its only reader. The comment now
  says three and says where the fourth lives, which is the rule-23 reason.
- The paragraph on the two outside callers in this report mixed HEAD's line
  numbers with the landed tree's; it now gives both frames (`gateway:145` and
  `camera:256` at HEAD, `:141` and `:251` landed) and names the two comments
  that described the dead target.

The declaration fixes change include paths only, so the tidy measurement cannot
move; the closing gate below was re-run after them anyway rather than reasoned
about. That review was an independent read-only pass over six things — module
declaration integrity, rule 23's 2+ rule measured by readers, rule 27 and the
tier table, the frozen `/sync` surface against HEAD, a `--only sync` build and
a sample of sixteen numeric claims from this report. It found nothing in four of
the six: the 2+ placements hold file by file, no service reaches another's
source, the wire surface is frozen (`sync-socket.cc`, `sync-service.cc` and
`synchronized-dto.hxx` byte-identical to HEAD — only the controller's header
differs, and only in its include spellings — and the four wire-vocabulary
headers with an empty `--numstat`), and the suites are green.

## Verified

Full `./scripts/build-all.sh dev` on the frozen tree — the run that closed this
sub-step, made after the closing pass's declaration fixes, exit 0:

- **17/17 projects** — `All selected projects built and tested (profile: dev)`.
- **380 suites, 0 failed**, in the orchestrator's order: cert 2, sqlite 2,
  identity 23, memory 19, intent 4, gateway 28, **sync 40**, camera 46,
  productivity 30, notification 34, guard 50, tts 20, stt 6, vlm 7, llm 32,
  voice 25, tunnel 12.
- **0 errors and 0 warnings** in the whole log: `grep -c 'warning:'` returns 0,
  and the only `[error]` line this sub-step's tree produced was the previous
  run's ratchet refusal, now gone.
- `check-deps`: `60 declarations, 460 edges, 0 forbidden, 0 cycles, 0
  unresolved, 45 edges deferred to phase 3`. The count moved one up from the
  pre-fix run's 459 because the closing pass added two declarations and dropped
  one: `argus::lib::sqlite` for the transport and `argus::lib::grpc-health` for
  the control RPC, minus the `argus::contracts::voice` edge nothing reached.
- `check-tidy`: `481 TUs, 3141 findings over 45 checks, baseline 3141` — no
  check count rose, no translation unit failed to load, and the `tus` floor is
  satisfied because **the baseline's `tus` line came down from 482 to 481 in
  this same change**, which is what rule 19 asks for when the change retires a
  translation unit (`sync-forwarder.cc`, whose one function is now `inline` in
  its header). The count is 481 and not 482 in both the pre-fix and post-fix
  scans, so the move of the two audit writers between modules changed no
  finding: they are deduplicated per file and line.

Post-review, `./scripts/build-all.sh dev --only sync` ran twice during the
repairs and once more under the closing reviewer: `100% tests passed, 0 tests
failed out of 40` each time.

### The doc reconciliation

12 live files were corrected in this change and each was re-measured before
being signed: root `AGENTS.md` (rule 10's WS/sync DTO clause, rule 18's
`RoomManager` path, the tier-2 contract row and five Key Files rows),
`services/sync/AGENTS.md` (rule 9 and the layout tree),
`services/sync/CONTEXT.md`, `services/gateway/CONTEXT.md`,
`docs/architecture/wire-camera-media.md`,
`packages/contracts/{camera,notification,productivity,sync}/AGENTS.md` and
`packages/clients/{camera,notification,productivity}/AGENTS.md`. The line
citations that moved under this sub-step's own edits (`packages/clients/camera`
`:158`/`:129`, `packages/clients/productivity` `:21`,
`packages/clients/notification`'s fan-in count, `packages/contracts/camera`'s
"four CMakeLists") were read off the landed tree, not carried.


