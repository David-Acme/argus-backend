# 3a-1b — the twelve cross-domain repositories go home

Phase 3 step 3a-1, sub-step **b**: `packages/sync`'s twelve cross-domain repositories
and their schemas move to the services that write the rows, and the package sheds them.
Sub-steps **c** (`services/sync` created, the gateway's sync surface removed,
`packages/{socket,room,audit}` deleted) and **d** (`packages/sync` deleted) follow.

## Pre-state

Measured at `b8b2bdc`, working tree clean.

`packages/sync/src/shared/repositories/` holds **twelve** directories —
`calendar-event`, `calendar-event-share`, `camera`, `camera-stream`, `event`,
`notification`, `project`, `project-member`, `project-task`, `reminder`,
`reminder-detail`, `zone` — and `src/shared/schemas/` holds **thirteen**: the same
twelve plus `person-event`. `packages/sync/CMakeLists.txt:131-155` compiles all
twenty-five `.cc` into `argus_sync`.

### Who reads each one

Readership was measured header by header, over `services/` and `packages/`:

| repository | readers | destination |
|---|---|---|
| `camera`, `camera-stream`, `zone` | `services/camera` — `feature/sync/camera-sync-rpc-service.hxx:5-7`, `main.cc`, `controllers/camera-media-service.hxx:99`, `feature/actions/camera-action-rpc-service.hxx:117`, `feature/api/camera-control/services/camera-control-feature-service.hxx:40`, `feature/api/camera/services/camera-feature-service.hxx:24`, `feature/api/zone/services/zone-feature-service.hxx:25-26` — and one **gateway test** | `services/camera` |
| the seven productivity ones | `services/productivity` only — `feature/sync/productivity-sync-rpc-service.hxx:5-11` and its five feature services | `services/productivity` |
| `notification` | `services/notification` (`feature/rpc/notification-rpc-service.hxx:9`, `feature/api/notification/services/notification-feature-service.hxx:5`) **and** `packages/sync/src/shared/services/notification/notification-service.hxx:6` | `services/notification` |
| `event` | the engine only — `packages/sync/src/feature/socket/sync/services/synchronized-service.hxx` and `tests/unit/event-sync-empty-test.cc` | stays with the engine |

Every repository uses the interface the sync package already defines: the four
`Syncable` virtuals plus `sync_query::buildSyncQuery`. Nothing in the engine reads a
domain repository — `SynchronizedService` pages camera, productivity and notification
rows through `packages/sync/src/shared/contracts/{camera,productivity,notification}-sync-source.hxx`,
whose implementations live in the gateway (`services/gateway/src/sync/*-sync-source.cc`)
and call the owner services over gRPC (`argus::camera::v1::PullTableRequest`,
`argus::notification::v1::PullNotificationsRequest`). The repositories are their
owners' write paths, in the wrong tree.

### The include interior is already the services'

`argus_sync`'s include root is `packages/sync/src` (`CMakeLists.txt:165-169`), so a
consumer writes `<shared/repositories/camera/camera-repository.hxx>`. Each owner
service's root is its own `src/` — `services/camera/CMakeLists.txt:164` `INCLUDES src`,
`services/productivity/CMakeLists.txt:15,137-138` `${PRODUCTIVITY_SRC_ROOT}`,
`services/notification/CMakeLists.txt:137-138` `${NOTIFICATION_SRC_ROOT}` — and the
moved files keep `<shared/repositories/X/…>` / `<shared/schemas/X/…>` for each other.

So the move is path-for-path and **no include line changes in any of the three
services**, in their tests, or in the moved files themselves. The spellings that would
have had to be rewritten — a flat `<repositories/…>` root, a per-service prefix — do
not exist in this tree.

### What the owners link today

Each of the three reaches the repositories through `argus_sync` and nothing else of it:

- `services/camera/CMakeLists.txt:159-169` — a comment saying the repositories "belong
  to `argus_sync` (the gateway's /sync reads the same rows)", then
  `argus_module(NAME camera-rpc … DEPENDS argus_sync …)`. `camera-core` links
  `argus_sync` too (`:252`), and `camera-talk-cutover-test` (`:398`) and one more suite
  (`:452`) name it.
- `services/productivity/CMakeLists.txt:150` — "the repositories and schemas ride
  `argus_sync`", then `productivity-core` PUBLIC-links it (`:140`);
  `productivity-controller-test` (`:239`) and `productivity-sync-rpc-test` (`:273`)
  link it for the same reason.
- `services/notification/CMakeLists.txt:148` — the same sentence, seven `argus_sync`
  links across `notification-core` and six test targets.

## The decisions this sub-step makes

**D1 — the eleven cross-domain repositories and their eleven schemas move; `event` and
`person_event` stay.** Their two names have no owner: neither has a `CREATE TABLE`
anywhere in the tree, and the engine prepares statements against a table that has never
existed (`synchronized-service.hxx`). No service's schema declares them, so no service
can receive them; they are the engine's own business, and sub-step c rewrites that
package. Flagged below as a measured defect, not fixed here — deleting a repository
whose only exercise is its own empty-table suite is c's call, and its `Event` table name
is frozen wire vocabulary the contract suite pins.

**D2 — `notification-service.{hxx,cc}` moves with the notification repository.** Its
only consumers are `services/notification`'s RPC service, its feature service and five
suites; nothing in `packages/sync` and nothing in the gateway names it. It is the
notification table's writer, so it goes where the table's owner is. This is what makes
the table single-owner: after this sub-step `services/notification` is the only unit
that reads or writes `notification`.

**D3 — the delivery-sink vocabulary moves to `packages/contracts/notification`.** After
D2 the last consumer of `packages/sync`'s one remaining header,
`shared/contracts/notification-delivery-sink.hxx`, is gone from the engine: what is left
is `services/notification` (9 files) and `services/gateway` (4). It carries the
notification domain's own wire — id, row ids, title, body, `data`, the two virtuals and
the JetStream message id — over `json/value.h` alone, with no sync type in it, which is
tier 2's definition under rule 25. It becomes
`packages/contracts/notification/src/notification/notification-delivery-sink.hxx` and
the thirteen include sites change spelling from `<shared/contracts/notification-delivery-sink.hxx>`
to `<notification/notification-delivery-sink.hxx>`, the form the domain's other header
already uses. Without it `notification-core` and its tests cannot shed `argus_sync`,
and a service that still links the engine's package cannot survive sub-step d.

**D4 — the gateway test loses its camera phase, and the camera DB client loses its only
setter.** `services/gateway/tests/audit-sync-read-test.cc:540-564` seeds a camera table,
installs it through `DbService::setCameraClient` and reads it back through
`CameraRepository::findById` — a gateway process opening the camera domain's database,
which is the rule-27 read this phase exists to delete, and the repository it names is
leaving this tree. The phase, its three helpers, its `TempDb` and its two
`setCameraClient` calls go; its subject is a routing mechanism the gateway no longer
has. The mechanism's other half is measured and recorded, not touched: after this
sub-step `DbService::setCameraClient` has **no caller in the tree** while
`DbService::cameraClient()` still has the camera repositories', and the two fall back to
the host's own database. The named-client family in `packages/lib/sqlite`
(`identityClient`, `cameraClient`, `productivityClient`, `gatewayClient`) is the
gateway-era arrangement, installed by the host at boot; its disposal belongs with the
gateway's death in sub-step 3d, where `setGatewayClient` loses its last caller too.

**D5 — the camera repositories land beside `camera-core`, and the one suite that refuses
`camera-core` links them.** `camera-core` is where the service's own sources
already live and where four of its suites reach them; `argus_camera-rpc` links it
instead of `argus_sync`. `camera-talk-cutover-test` compiles the control feature and the
driver stack directly *because* linking `camera-core` would drag ncnn into a suite that
needs none of it (`CMakeLists.txt:389-391`), and both of those sources include a
camera repository. **Superseded by what the build forced** (see "As executed"): the row
code became a module of its own, `argus::camera-repositories`, carrying the three
repositories and the three schemas, and the suite links that module rather than the six
sources.

**D6 — the two tests that link `argus_sync` for the repositories link their service's
core library instead.** `productivity-controller-test` and `productivity-sync-rpc-test`
link `productivity-core`; `notification-controller-test`, `notification-rpc-test`,
`notification-no-nats-test`, `notification-delivery-test`, `notification-ack-test` and
`push-intent-test` link `notification-core` (which the live suite already does). Each
keeps every other link it had.

## Open items this sub-step records

- **`event` and `person_event` have no DDL anywhere** (D1). Two frozen table names, one
  repository, one empty-table suite, no `CREATE TABLE` in any of the eight owners'
  schemas. They stay with the engine and are sub-step c's to delete or keep.
- **Two repository methods are measured dead** and move with their owners rather than
  being deleted in a relayout: `CameraStreamRepository::create` (its only member —
  `camera-sync-rpc-service.hxx:26` — calls `find` alone; the camera's own create path
  goes through `CameraRepository::create`, `camera-feature-service.cc:50`) and
  `EventRepository::linkPerson` (no caller). Neither service's suite covers them, so an
  accidental deletion would be silent; the owner's cleanup is the right change.
- **`setCameraClient` has no caller after D4** while `cameraClient()` keeps nine
  call sites in the camera repositories, all of which now resolve to the service's own
  database (D4).
- **The gateway's delivery consumer** (`src/sync/notification-delivery-consumer.hxx` and
  `src/shared/repositories/delivery-inbox/`) keeps its `notification_delivery_inbox`
  table, whose DDL still sits in identity's schema. The destination of both is still
  open from a2's list and is not decided here. What it no longer keeps is the
  notification **schema**, resolved below (D7).

## As executed

Fifty-eight files moved, path for path: 15 out of `packages/sync/src/shared/repositories/`
and `.../schemas/` into `services/camera` (9 repository, 6 schema), 35 into
`services/productivity` (21 repository, 14 schema), 7 into `services/notification` (3
repository, 2 schema, `notification-service.{hxx,cc}`), and the delivery-sink header into
`packages/contracts/notification`. No include line changed in any moved file, in the three
services or in their suites. `event` and `person-event` stayed; `ARGUS_SYNC_SOURCES` is down
to the socket, the sync service, `SynchronizedService`, the event repository and the two
schemas beside it, and the three pull-source contracts.

**D5 as executed — the camera repositories got a module of their own, not `camera-core`.**
`services/camera/src/shared/repositories/CMakeLists.txt` declares
`argus_module(NAME camera-repositories …)`, carrying the three repositories **and the three
schemas** as six `.cc`. The schemas ride it because their translation units had no other
compiler: `argus_sync` was all that compiled them, and `camera-core` is a plain
`add_library` source list, not a target that can be named before it is declared —
`camera-rpc` is declared above it, and a `::`-namespaced link to an undeclared target is a
configure-time error, so the three `add_subdirectory` lines had to move above the
`argus_camera-rpc` block. `camera-core` PUBLIC-links the module, `argus_camera-rpc`
`DEPENDS` on it, and `camera-talk-cutover-test` and `camera-action-rpc-test` link it.

**Productivity and notification keep one core library each.** `productivity-core` and
`notification-core` gained the moved sources inline rather than a new module: they are
already the owners' libraries and five and six of their suites already linked them (D6).
Both also gained what the repositories need now that `argus_sync` is gone —
`argus::contracts::{productivity,notification}`, `argus::contracts::sync`,
`argus::lib::{nats,text}`, `argus::lib::sqlite` and the vec0 target — measured from the
moved files' own includes.

**Two contract edges, measured one at a time.** `argus_sync`'s link list carried
`argus::contracts::notification` and `argus::contracts::productivity` although none of its
remaining sources uses either. Measured consumers:

- **`contracts::notification` stays** — the gateway includes
  `<notification/notification-delivery-sink.hxx>` from six files, and this edge is the only
  root that supplies it: `argus_clients(NAME notification …)`'s `DEPENDS` is empty and its
  header includes generated protobuf alone, so `argus::clients::notification` carries no
  hand-written contract root, and the gateway links no `contracts::notification` of its
  own. The link goes when its last consumer does: the gateway's sync surface in sub-step c.
- **`contracts::productivity` is removed here** — it has no consumer in the tree. Nothing
  under `packages/sync/src` includes `<productivity/…>`; `services/productivity` and the
  contract's own suites link it directly; the gateway's single `<productivity/…>` include
  is the *client's* header (`packages/clients/productivity/src/productivity/productivity-sync-client.hxx`,
  resolved by that client's own include root); and the guard never links `argus_sync` at
  all — its notification edge is `argus::clients::notification`
  (`services/guard/CMakeLists.txt:141`). `packages/contracts/productivity/AGENTS.md` now
  names one CMakeLists.

**D7 — the gateway stops compiling the notification owner's schema.** The sweep for moved
headers over the whole tree found exactly one stale include:
`services/gateway/src/sync/notification-delivery-consumer.cc:7` included
`<shared/schemas/notification/notification-schema.hxx>` to render the socket `Add` payload
of a durable delivery. That file is `notification-core`'s now, so the include resolves
against nothing, and the gateway is not its owner anyway (rule 27). The gateway already
renders the same row for its `/sync` pulls — from the wire, in `notification-sync-source.cc`'s
`rowToJson` — so the two builders became one:
`services/gateway/src/sync/notification-row-json.hxx`, a nine-field `NotificationRowJson`
with `toJson()` (`[[nodiscard]]`, because the tidy gate counts
`modernize-use-nodiscard`), filled from the proto row by `rowToJson` and from the delivery
event by `dispatchToSockets`. The payload is byte-identical: the key set, the value shapes
and `readAt` as JSON null were read off `NotificationSchema::toJson()`, and neither consumer
suite pins the payload (both inject their own `dispatch`).

**D8 — the engine's link closure, measured one target at a time.** `argus_sync` PUBLIC-links
fifteen targets (`packages/sync/CMakeLists.txt:150-166`: `argus::audit`, `argus::socket`,
`argus::room`, `argus_identity`, `argus::lib::auth`, `argus::contracts::{sync,auth,camera,notification,productivity}`,
`argus::lib::{text,nats,sqlite,validation,errors}`), so every service that linked it *for the
repositories* received that whole closure — include roots included. Shedding it surfaced three
needs the repositories' own includes do not reveal, each measured by the compiler:

- **`camera-core` keeps `argus_sync`.** `services/camera/src/controllers/camera-media-service.hxx:5`
  includes `<feature/socket/sync/socket/sync-forwarder.hxx>` for `SyncFrameInput`, the frame type
  of the service's `/media` socket. The engine's include root was all camera-core wanted from it,
  but it is a target, not a header, so the edge stays with the comment saying why; the socket
  vocabulary is sub-step c/d's to re-home. Its repositories are `argus::camera-repositories`'s
  now, and `argus-camera` pulls `argus_identity` through this edge exactly as before — restoring
  the original sentence in the CMakeLists.
- **`argus-productivity` and `argus-notification` gain `argus::lib::validation` and
  `argus::lib::http`.** Their DTOs validate through the DSL and their controllers answer
  through `ApiResponse`/`Cors`; both packages were in the closure and neither is reached by
  the repositories, `argus::lib::auth` or `argus::clients::<domain>`.
- **The notification tree gains `packages/contracts/notification`.** Its configure failed
  outright — `notification-core` links `argus::contracts::notification`, a target that did
  not exist in that tree — because the contract *this sub-step created* had always arrived
  through the engine's own `add_subdirectory` block. Its two siblings (`contracts/auth`,
  `contracts/sync`) were already added by path.

## The review of sub-step b

One independent reviewer read the whole change set against the row's text and the house
rules (3, 19-27): the fifty-eight moved files, the three services' CMakeLists, the two
contract packages, the gateway's row builder and this report's own claims. The structural
claims held — the move is path for path with content identity on every file,
`ARGUS_SYNC_SOURCES` is down to the engine and the two schemas beside it, the sink header
carries no sync type, no consumer names a target before the `add_subdirectory` that
declares it, no source is compiled twice, and the gateway's `Add` payload is the same nine
keys with the same expressions. Ten findings survived verification — F3 and F7 are the two
halves of one entry — and each was measured against the tree before anything was done with
it.

**F1 — the new module would have run without the warnings gate (confirmed; the only
build-affecting finding).** `services/camera/src/shared/repositories/CMakeLists.txt` called
`argus_module` and stopped. `argus_module` adds no compile options
(`cmake/argus-module.cmake:145-156`; only `argus_service`, `argus_grpc_absl_bridge` and
`argus_grpc_client_base` add `-Wall -Wextra`), so the six translation units that had been
compiled inside `argus_sync` — which does carry the flags — would have lost the gate
silently, and rule 17's "0 warnings" would have become a promise about flags no longer
passed. Fixed with the line the house convention carries after every `argus_module` call
(`packages/room`, `packages/identity`, this sub-step's own two core libraries), and
re-measured: camera rebuilt with the gate on, 51/51, no warning line.

**F2 — this report's D3 undercounted the include sites (confirmed).** "eleven" and "8 + 4"
were both wrong; the measured split is 9 in `services/notification` and 4 in
`services/gateway`, thirteen. Corrected in place.

**F3/F7 — the two surplus contract edges were not the same case (confirmed, with a
correction).** The reviewer's claim that neither edge had a consumer behind `argus_sync`
was true for `productivity` and false for `notification`: the gateway includes the sink
header from six files and that edge is the only root supplying it
(`argus_clients(NAME notification …)`'s `DEPENDS` is empty, so the client carries no
hand-written contract root). Measured one at a time, `productivity`'s edge had no consumer
at all: nothing under `packages/sync/src` includes `<productivity/…>`, the contract's own
suites and `services/productivity` link it directly, the gateway's single `<productivity/…>`
include resolves against `packages/clients/productivity`'s own include root, and the guard
never links `argus_sync` at all — its notification edge is `argus::clients::notification`
(`services/guard/CMakeLists.txt:141`), which is where the report's earlier sentence about
it went wrong. `productivity`'s edge is removed here; `notification`'s stays until the
gateway's sync surface leaves in sub-step c, and both bullets now name their consumer.

**F4 — `packages/contracts/notification/AGENTS.md` said 14 include sites (confirmed).**
Thirteen, the count D3 measures. Corrected.

**F5 — the root `AGENTS.md` still pointed the notification row at `packages/sync`
(confirmed).** The row now reads `services/notification/src/shared/services/notification/`
and names the sub-step that moved it.

**F6 — `services/notification/CONTEXT.md` contradicted itself (confirmed).** The paragraph
above it says `notification-core` compiles the table's repository, schema and delivery
service in this folder since 3a-1b, while the closing sentence still said only the
notification-token side was this service's. Rewritten to match.

**F8 — a comment sat inside a link list (confirmed; rule 20).**
`services/camera/CMakeLists.txt`'s `target_link_libraries(camera-core PUBLIC …)` carried its
rationale inside the argument list. Moved above the call, the shape the productivity and
notification edits already use.

**F9 — D5 contradicted its own "As executed" (confirmed).** D5 planned the repositories into
`camera-core`; the build forced a module of their own, recorded below it. D5 now says so
instead of leaving two answers standing.

**F10 — `packages/clients/camera/AGENTS.md`'s line references (measured while checking
F3's).** Its six `CMakeLists.txt:<line>` claims were all stale at HEAD: the gateway's
`:183`/`:128` read 176/121, argus-llm's `:210`/`:136` read 218/143, camera's `:152` read
145, and the `DEPENDS` entry it pointed at as `:177` was 170 there. This sub-step edits that
file anyway, and its own inserts push the camera `DEPENDS` line to 174, so all six now carry
the measured values.

Nothing in the review asked for a change to the move itself, and nothing it confirmed moved
a path, a link or a payload beyond those three fixes.

## The gate of sub-step b

`./scripts/build-all.sh dev` — **exit 0**, 18/18 projects, **405 tests, none failed and none
skipped**: cert 2, socket 13, sqlite 2, identity 23, sync 30, memory 22, intent 4, gateway
42, camera 51, productivity 30, notification 34, guard 50, tts 20, stt 6, vlm 7, llm 32,
voice 25, tunnel 12. The run total moves 416 → 405 for one reason, measured: the removed
`add_subdirectory(packages/sync)` blocks had been registering five productivity suites and
six notification suites a **second** time in those two trees. All eleven names are still
reached — from `packages/sync`'s own tree, the gateway's, camera's and the guard's — so no
suite lost coverage; the eleven registrations were duplicates.

The three fixes above landed after that run had configured those trees, so each was rebuilt
on its own: `--only sync` 30/30, `--only camera` 51/51, `--only gateway` 42/42, every run
rc=0 with no `error:` and no `warning:` line.

`check-tidy: clang-tidy 22.1.8` — `481 TUs, 3154 findings over 45 checks, baseline 3156; 1
checks below it`. The ratchet holds with two findings below the baseline.

`check-deps`, measured on both endpoints with the scanner's own functions (`scan()` of
`scripts/lib/check-deps.py` imported directly) and stable on each: HEAD's tree reads `427
edges, 108 deferred`, the final tree `431 edges, 93 deferred`, and the two agree exactly on
the third-party tally, 208 mentions over 22 roots. The +4 net edges decompose with nothing
left over: **+4** `services/camera -> argus::camera-repositories` (`camera-core`,
`argus_camera-rpc`'s `DEPENDS` and the two suites), **+1** for each of the module's three
`DEPENDS`, **+1** `services/notification -> argus::contracts::notification` (that tree gained
the contract's `add_subdirectory`), **+11** for the one link each core library gained in five
(notification) and six (productivity) triples, against **−12** for the removed links
(notification 8 sites, productivity 3, `packages/sync`'s productivity edge 1) and **−3** for
camera's `argus_sync` sites (4 → 1). The deferred −15 is the same set: the twelve removed
sites plus camera's three. `--list-deferred` lost exactly three triples — the three edges
this sub-step removed — and gained none; the deferred counter counts sites, the listing
deduplicates per triple, which is why 3 triples read as 15 sites.

The orchestrator's **opening** line reads `56 declarations, 432 edges, 0 forbidden, 0
cycles, 0 unresolved, 94 edges deferred to phase 3 (227 third-party mentions over 39
roots)`. Its 432/94 is the final 431/93 **plus the productivity edge this sub-step removed
last**, so that reading is a snapshot of the tree mid-edit; its third-party tally is not
reproducible on either endpoint (both read 208 over 22), which makes it a property of that
instant's partly-written CMakeLists rather than of the result — the scanner expands no
variables, so a `${…}` spelling in a file read while it was being written lands in that
tally verbatim. Recorded, not chased: every gate-relevant counter — 0 forbidden, 0 cycles, 0
unresolved — agrees on all three readings.
