# 3a-1 — the sync service, and the vocabulary that leaves `packages/socket`

Phase 3 step 3a-1, sub-steps **a1** and **a2**: the vocabulary `packages/socket` owns
becomes `packages/contracts/sync` (a1), then the control surface §3.5 asks for gets its
`.proto` and `packages/clients/sync` is created (a2). The row's other half — extracting
`services/sync` and deleting `packages/{sync,socket,room,audit}` — is sub-steps b–d, and
the design they follow is recorded here because this is the row's first artefact.

## Pre-state

Measured on `e1716df` (the commit that closed Phase 2 step 6), working tree clean.
The gate's own ledger is authoritative: the `build/dev` trees beside the packages are
stale leftovers from before the Phase 2 re-homes, so `ctest -N` against one of them
gives a count that is wrong and reads as a defect.

`./scripts/build-all.sh dev` — exit 0 in 612 s, 18/18 projects, **405 tests**:
cert 2, socket 13, sqlite 2, identity 22, sync 29, memory 22, intent 4, gateway 41,
camera 50, productivity 34, notification 39, guard 49, tts 18, stt 6, vlm 7, llm 32,
voice 23, tunnel 12. No `***Failed`, no `Not Run`, 0 errors and 0 compiler warnings
(the 21 `Warning:` lines are third-party configure notices). `check-tidy`: 480 TUs,
3156 findings over 45 checks, baseline 3157, no check risen, no unread TU — and one
check already **below** its baseline (`modernize-use-scoped-lock`), a fall row 6 left
recorded in the baseline at its pre-row value; the row's own log states it
(`check-tidy: 480 TUs, 3156 findings over 45 checks, baseline 3157; 1 checks below it`).
`check-deps`: 54 declarations, 424 edges, 0 forbidden, 0 cycles, 0 unresolved,
110 edges deferred to phase 3.

The four packages this row retires, by `git ls-files`:

| Package | Tracked files | Of which source | Plan's count (§3.6, §9.1) |
|---|---|---|---|
| `packages/socket` | 10 | 8 | 8 |
| `packages/room` | 3 | 2 | 2 |
| `packages/audit` | 26 | — | 25 |
| `packages/sync` | 98 | 77 src + 19 tests | 89 |

§3.6's "three jobs in 8 files" holds exactly for socket's eight source files
(two contracts, two DTO files, three transport files, one test); the other three
counts are off by one to nine and are recorded rather than quietly reconciled.

## What the row's clauses rest on, measured

Six facts decide the design, and each was verified against the tree rather than read
from a report.

1. **`SocketEmitDto` is the shared payload vocabulary, not a socket detail.** **36
   consuming files outside `packages/socket`** name at least one of the six symbols that
   move — audit (4), identity (4), memory (1), `packages/sync` (3), camera (5), gateway
   (7), notification (4), productivity (8) — and **27** of them name `SocketEmitDto`
   itself; measured at `HEAD` with `git grep -lE`, source files only. This is §3.6's
   "payload vocabulary → `contracts/sync`" measured as a consumer set.
2. **The change feed has four subjects, and only one producer is the socket.**
   Producers publish on their own subject — `nats-camera-change-sink.cc:20` on
   `kCameraChange`, `nats-productivity-change-sink.cc:23` on `kProductivityChange`,
   `nats-notification-change-sink.cc:23` on `kNotificationChange` — and the gateway's
   fan-out subscribes `kSyncChangeWildcard` = `argus.*.v1.change`, which matches all
   three. The only real publisher on `kSyncChange` itself is `packages/identity`
   (`identity-rpc.cc:156` and `:577`, both through a `bus_`); the socket's own
   `socket-service.cc:27` publishes on it too but **its bus is never installed** —
   `SocketService::setEventBus` has no caller anywhere in the tree, so `publishChange`
   is a no-op and the gateway cannot publish its own fan-out subject. The control paths
   (`disconnectUser`, `replaceRoleRooms`, `emitUsers`) are therefore **in-process only**
   today, which is why extracting `/sync` into its own process breaks them unless the
   control RPC lands in the same step. This is the row's load-bearing defect.
3. **`user_action_log` has exactly six writers, all in `packages/identity`** —
   `auth-service.cc:309,528,633`, `invitation-feature-service.cc:151`,
   `portrait-preview-service.cc:137`, `user-feature-service.cc:134` — each holding a
   `UserActionLogService` by value, and **no caller outside that package**. §3.5's
   "its write path is the additive subject" is a statement about these six sites and
   nobody else.
4. **Audit rows have two writers today.** The gateway's fan-out writes them
   (`camera-fan-out.cc:31` → `auditLogService.create`, `user-change-fan-out.cc:32`),
   and so does identity through `packages/audit`'s `SyncAuditService`
   (`sync-audit-service.cc:16,38` → `AuditLogService::createAndEmit` /
   `UserAuditLogService::createAndEmit`), which is a *local* write plus a dead emit
   (`audit-log-service.cc:69-75`). "`sync` becomes the single audit writer" (3a-2) is
   the removal of that second writer.
5. **The payload builders have real consumers, not only their own test**:
   `emitPayload` (identity ×2, the socket, the gateway suite), `userEmitPayload`
   (notification ×2, productivity ×2, the gateway's delivery consumer, the gateway
   suite), `disconnectPayload` and `roleRoomsPayload` (the socket's transport and the
   gateway suite). A signature change to any of them reaches tests that pin the wire,
   so the moves below preserve every assertion count.
6. **`RoomManager` has two kinds of consumer.** Real fan-out users: the socket's
   `SocketService`, `packages/sync`'s `SyncSocket` (`sync-service.cc:49` joinMany,
   `:119` leaveAll) and the gateway's `sync-fan-out.cc:84-99`. Lifecycle-only users:
   `services/camera/src/main.cc:258-259,327` (construct, `init()`, `shutdown()` — no
   join, leave or emit anywhere in that service) and the gateway's
   `main.cc:438-439` (construct and `init()`; its `shutdown()` is never called, so the
   prune timers of the only process that has rooms are never stopped). Camera's use is
   vestigial and goes with this row.

## Corrections the measurements force

**C1 — the twelve cross-domain repositories go to their owners, not to
`services/sync`.** §9.1's `sync` row reads "services/sync (79 files, 12 cross-domain
repositories)", and that cannot be literal: §2.4 rule 3 forbids a tier-5 → tier-5 edge,
§3.4 says cross-domain reads travel through `clients/*`, and D18 forbids a package that
owns data or queries a domain. The measured tree says the same thing from the other
side: the camera, productivity and notification repositories have **30+ call sites in
their own services** (`camera-sync-rpc-service.hxx:25-27`, `main.cc:294`,
`camera-media-service.hxx:99`, `camera-action-rpc-service.hxx:117`,
`camera-control-feature-service.hxx:40`, `camera-feature-service.hxx:24`,
`zone-feature-service.hxx:25-26`; productivity's five feature services and its
`productivity-sync-rpc-service.hxx:24-30`; `notification-rpc-service.hxx:49`). They are
those services' own write paths, sitting in `packages/sync`'s tree only because the
gateway's `/sync` reads the same rows. They move to their owner services in sub-step b;
`services/sync` keeps the engine, the transport, the fan-out and the audit and journal
persistence. The stale justification for the same coupling is still in the tree —
`services/camera/CMakeLists.txt:159` comments that the repositories "belong to
`argus_sync` (the gateway's /sync reads the same rows)" — and it goes with sub-step b.

**C2 — `packages/audit`'s death splits, and the journal's write path moves in 3a-1,
not 3a-2.** Row 3a-1 deletes `packages/audit`; row 3a-2 says identity's six journal
writes move onto the additive subject. Both cannot hold: identity is a package linked
into the gateway process, so once the journal's code is a service's, the six sites
cannot call it, and 3a-1's own deletion clause removes the code they call. The move
therefore happens in 3a-1 (sub-step c) and 3a-2 keeps its outbox-and-fingerprint clause,
which is a producer-side durability mechanism that applies to all five producers.
Two further measurements bound it: the audit **tables** do not move in this row at all —
row 3c-2 owns that ("the audit tables (`audit_log`, `user_audit_log`,
`user_action_log`) → `sync.db`, with row-count and checksum verification and a
documented rollback") — so `services/sync` writes those three tables in identity.db
until 3c-2 relocates them, which is the transitory state the plan's ordering already
implies. And §3.5's own sentence places the new subject row in this phase: the
`user-action` row is declared "when Phase 3a creates it".

**C3 — the contract cannot hold the role conversion, but not for the reason first
recorded.** This correction replaces an earlier paragraph that grounded the split in the
tier table: it claimed `UserRole` lives in `lib/auth`, which is alone in tier 4, so a
tier-2 contract could not reach it. **That is false.** `UserRole` and `userRoleToString`
live in `packages/contracts/auth/src/auth/user-role.hxx` — tier 2 (`check-deps.py`'s
`ALLOWED = {1:{1}, 2:{1,2}, 3:{1,2}, 4:{1,2,3}, 5:{1,2,3,4}}` bars 2 from tier 4, but a
2 → 2 edge is legal) — and `contracts/auth` was already reachable from the old
`sync-change-test`, which linked `argus::contracts::auth`. Tier 4 is `packages/lib/auth`,
a different package that holds the JWT/session layer, not the role enum.

The split stands on the two real grounds instead. First, the old builder took
`RoleRoomReplaceInput`, which belongs to `packages/room` — a package this row deletes, so
the contract cannot name it. Second, the wire carries role *names*: `sync-fan-out.cc:44-48`
parses `old_role`/`new_role` as strings, so the contract's builder takes strings. The
conversion therefore belongs to whoever holds the enum, and that is the transport: today
`SocketService::replaceRoleRooms` (socket-service.cc:55-66), tomorrow `services/sync`. Its
signature stays `(const RoleRoomReplaceInput&)` so its callers are untouched; only the
`publishChange` argument changes. `contracts/sync` keeps a tier-2-clean include set, which
is the test that the split is in the right place — and it is now true by construction, not
by luck.

**C4 — the gate's project list is a hand-maintained array of 18.**
`scripts/build-all.sh:9-28` builds one project at a time from each project directory.
`socket` and `sync` are two of the 18; `room` and `audit` are not (they are built
transitively), and neither are `packages/contracts/*` or `packages/clients/*`. So this
row takes the array 18 → **17** when sub-step c lands, not 18 → 16, and the new
`services/sync` is the entry that replaces the two.

**C5 — `person_event` is a frozen-wire table with no storage.** `TableName::Event = 4`
and `TableName::PersonEvent` are in the catalog, `EventRepository` queries
`person_event` (`event-query.hxx:22,26`), and **no DDL for either table exists anywhere
in the tree** — not in `packages/identity/database/schema.sql`, not in any service's
schema. `event-sync-empty-test.cc:13` passes because a query against a missing table
yields nothing. This is a pre-existing defect, it is not this row's to fix, and it is
recorded because the extraction must not be blamed for the empty page it inherits.

**C6 — the golden-frame end-to-end test does not run in the gate.**
`golden-sync-test.cc` returns 0 unless `ARGUS_TEST_REFRESH_TOKEN` is set and
`packages/sync/CMakeLists.txt:215` registers it with no environment, so wire invariance
cannot be proved by it. The row's own instrument for "payloads untouched" is therefore
the assertion counts of the suites that pin the shapes
(`sync-change-test`, `gateway-test`'s fan-out cases, the three suites that drive the
emitted frames), compared one by one against this pre-state.

**C7 — `packages/socket`'s `SOURCES` omitted two headers.** `CMakeLists.txt:52-57` listed
`socket-service.cc`, both `socket-emit-dto` files and the two sink contracts, but not
`socket-service.hxx` nor `sync-change.hxx`, both of which consumers include and the
package's own test included. Nothing broke — the include root made them reachable — but
the list was incomplete. **Closed by sub-step a1**: `socket-service.hxx` is now listed
beside its `.cc`, and `sync-change.hxx` left the package entirely (the new
`contracts/sync/CMakeLists.txt` lists every header it owns, `sync-change.hxx` included).

## The sub-steps this row runs as

The row covers four packages and 613 reference sites across nine units, so it runs as
four sub-steps, each building on the previous and each keeping the tree green — the
shape Phase 2 step 2 used for its forty-six packages. Sub-step **a** turned out to hold
two independent units of work — a pure move of frozen vocabulary, and the creation of a
new control wire with its client — so it runs as **a1** and **a2** rather than as one.

| Sub-step | Scope | Commit |
|---|---|---|
| **a1** | vocabulary → `contracts/sync`: the emit DTO, the change builders, the two sinks, the two audit events, the change suite; every consumer repointed; `packages/socket` reduced to its transport | this report |
| **a2** | the control wire: `argus/sync/v1/sync.proto` and `packages/clients/sync` | |
| **b** | the twelve cross-domain repositories and their schemas to their owner services; `packages/sync` sheds them | |
| **c** | `services/sync` created (WS, rooms, fan-out, audit persistence, journal, control RPC server and client); gateway's sync surface removed; identity's audit and journal writes onto the wire; `packages/{socket,room,audit}` deleted; the gate's array 18 → 17 | |
| **d** | `packages/sync` deleted; `memory` repointed; the doc sweep; the gate; the review | |

## Sub-step a2, designed against what the imperative leg actually is

§3.5 names four imperative operations — `AuthContextChanged`, disconnect, role-room
replacement, directed emit to a user room — and D8 routes them over `clients/sync` gRPC.
Measured, they are **four call sites carrying three frames**, all in `packages/identity`,
and two of §3.5's names are one frame: `AuthContextChanged` is the operation that both of
the others carry.

| Call site | Frame today | Client method |
|---|---|---|
| `user-feature-service.cc:56` `replaceRoleRooms({id, oldRole, newRole})` | action `replace_role_rooms`, operation 7 | `replaceRoleRooms(id, oldRole, newRole)` |
| `user-feature-service.cc:97` `disconnectUser(id, context)` | operation 7, `resync:false` | `disconnectUser(id, frame)` |
| `auth-service.cc:526` `disconnectUser(id, context)` | operation 7, `resync:false` | `disconnectUser(id, frame)` |
| `user-feature-service.cc:128` `emitUser(id, body)` | operation 7, `resync:true` | `emitToUser(id, frame)` |

Everything else that emits — camera's, productivity's and notification's sinks, the
fan-out's own room writes — belongs to the *change* leg, which stays on NATS and becomes
`services/sync`'s fan-out in sub-step c. So the client carries no `emitToUsers` and no
`emitToModule`: identity's sites are the only cross-process imperative traffic there is,
and three methods carry them. The gateway's `sync-fan-out.cc:88,91` calls are the server
side of the same leg (the fan-out applying rooms locally); they move into `services/sync`
with the fan-out and never become client calls.

The wire reuses the frozen vocabulary instead of inventing a second one:
`argus/sync/v1/sync.proto` imports `argus/sync/v1/contracts.proto` and carries
`SyncFrame{operation, table, info}` as the frame, `string`/`int64` elsewhere, and answers
with `ControlAck`. Role names cross as strings for the reason C3 records — the caller
holds the enum, the wire holds the name — and `info` stays the JSON string the WS envelope
already carries, so no frame changes shape in transit.

`packages/clients/sync` follows `packages/clients/identity` file for file (rule 23): the
`argus_clients(NAME sync PROTO_ROOT … PROTO argus/sync/v1/sync.proto …)` macro, a thin
wrapper over `argus::sync::v1::SyncControl::StubInterface` built on `lib/grpc`'s
`makeChannel` + `setDeadline` + `addFleetSecret`, `virtual` methods that return `bool`
(false when unreachable — the house spelling for an ack with no payload), and a doctest
suite that opts back into the default build because the folder arrives through
`EXCLUDE_FROM_ALL`.

## Sub-step a1, as executed

**Moved.** The emit DTO became a header-only contract (`toJson()` inlined, because
`argus_contracts` is an INTERFACE target and a contract cannot ship a `.cc`); the change
builders became `sync-change.hxx`, rewritten so the role case takes `RoleRoomChange`
(C3); the two change sinks and the two audit events moved byte for byte with only their
include lines repointed; and the change suite moved with them. `packages/socket` lost its
three duplicates — `socket-emit-dto.{hxx,cc}` and `sync-change.hxx`, whose content now
lives in the contract — so what remains is `socket-service.{hxx,cc}`: the transport, and
the only place the enum-to-name conversion happens.

**Repointed, and verified rather than assumed.** **38 include sites in 33 files** across
nine units were swept onto `<sync/…>` (identity 3 files/4 sites, memory 1, `packages/sync`
3/3, camera 4/4, gateway 7/9, notification 6/7, productivity 9/10). Five further files had
include lines changed by hand: the two sink contracts, `socket-service.{hxx,cc}` (which
gains `<auth/user-role.hxx>`) and the suite. Measured by classification of the diff: the
swap is one line out and one line in in each of those 33 files, the three deleted files take
their ten include lines out of the tree with them (`socket-emit-dto.{hxx,cc}` and the old
`sync-change.hxx`, counted as `3 + 1 + 6` at `HEAD`), and the two new headers declare the
three and four includes they use — so no translation unit gained an include it did not need
or lost one it did. And the load-bearing check: **outside `CMakeLists.txt` the only changed
lines in the whole tree that are not `#include` lines are the two role-conversion sites**,
`socket-service.cc:55` and `gateway-test.cc`'s two inputs. `sync-change-test` keeps all
**25** assertions in its 5 cases and `gateway-test` keeps every one of its fan-out
assertions, so C6's instrument — assertion counts compared against this pre-state — is
intact.

**Fixed in passing, each measured first.** `packages/audit/CMakeLists.txt` still listed the
two audit-event headers that had already left it, which would have failed its configure;
`packages/socket/CMakeLists.txt` is rewritten around the two remaining files and drops the
moved suite's target, which closed C7. The empty `packages/audit/src/shared/contracts/`
directory went with them.

**The ledger grew by four, and each addition has a name.** The gate's reached-test count is
**405 → 409**, and it grew because two suites register in two more projects, not because
any instrument was loosened: `sync-change-test` (moved into `contracts/sync`, whose tree
tts and voice already reach for the two `sync-contract-*` suites) and `nats-wrapper-test`
(the suite's subject case pins `nats_subject::kSyncChange`, so the test target links
`argus::lib::nats` and the house's guard pattern brings that package's own suite along —
the *contract* itself still depends on nothing but `lib/errors`, `lib/text` and Drogon).
Proven by asking each project's `CTestTestfile.cmake` tree who registers what instead of
inferring it: in tts, `nats-wrapper-test` is registered from `contracts/sync/nats/` — a
binary directory that exists only because of this sub-step's guard — while `sha256-test`
(`contracts/sync/text/`) and the two `auth-contract-*` (`contracts/auth/`) were already
registered through pre-existing paths, and tts and voice are the only two projects whose
lists moved.

**The tidy ratchet took one deliberate edit, and only half of it is this row's.** Re-measuring
the tree wrote `tus 480 → 479` plus one inherited fall:
`modernize-use-scoped-lock 296 → 295`. The TU is this row's and its cause is named: the
deleted `socket-emit-dto.cc` was a translation unit whose single function is now an inline
header function — still scanned through every TU that includes the contract, because
findings are deduplicated by location, so no code became invisible. The fall is **not**
this row's: row 6's own gate log states `check-tidy: 480 TUs, 3156 findings over 45
checks, baseline 3157; 1 checks below it`, and `scripts/lib/tidy-baseline.txt` had kept the
pre-row number because a fall never fails the gate. `--write-baseline` recorded both, and
the file's diff is exactly those two lines.

**The gate ran twice, and the second run is the one that stands.** Run 1 was the sub-step's
own: 18/18 projects built and executed with **409** reached tests — the pre-state's 405 plus
the four named above — and it failed on `check-tidy` alone, with `risen: the scan saw 479 TUs,
baseline 480 -- build the whole tree before the gate`, which is the deliberate TU removal and
was answered by writing the measured baseline rather than by restoring a translation unit.
Run 2, after that edit, is clean: `./scripts/build-all.sh dev` **exit 0**, 18/18 projects,
**409 tests** — cert 2, socket 13, sqlite 2, identity 22, sync 29, memory 22, intent 4,
gateway 41, camera 50, productivity 34, notification 39, guard 49, tts 20, stt 6, vlm 7,
llm 32, voice 25, tunnel 12 — no `***Failed` and no `Not Run`, and
`check-tidy: 479 TUs, 3156 findings over 45 checks, baseline 3156`, with no check risen and
none below it either, so the ratchet stands exactly where measurement put it. The two projects
whose test counts moved are exactly the two the ledger paragraph above predicts (tts 18 → 20,
voice 23 → 25); socket's own 13 stay 13, which is the check that moving a suite between
packages did not quietly drop it from the ledger. A third run, after the review's findings
below were folded in (three `<cstdint>` includes and one CMakeLists comment), reproduced that
run line for line — 18/18, 409, `479 TUs, 3156 findings over 45 checks, baseline 3156`, exit
0 — so the verdict covers the tree as committed, and the review's fixes changed nothing the
gate can measure.

**The dependency checker's deferred count fell by three, and the same three edges are why
the total did not move.** Pre-state reads `110 edges deferred`, this sub-step's tree reads
`107`, with `424 edges` both times. Measured, not inferred: `--list-deferred` run against a
`git worktree` of `e1716df` and against the working tree yields an **identical set of
deferred triples** (`diff` on the listings after stripping the `CMakeLists.txt:line`
suffixes: no output), so the shift is multiplicity, not membership. The moved suite's link
line is the whole of it — before, `packages/socket`'s `sync-change-test` named
`argus::lib::text`, `argus::lib::nats` and `argus::contracts::auth`, three *deferred* edges
because `packages/socket` has no tier until §9.1 moves it; after, the same three names are
declared by tier-2 `packages/contracts/sync`, where the checker **classifies** them and
they are legal (2 → 1, 2 → 1, 2 → 2). Three departures offset three arrivals, and the five
edges the transport itself keeps (`room`, `nats`, `contracts::sync`, `contracts::auth`,
`lib::text`) keep `packages/socket` deferred wholesale.

## The review of this sub-step

An independent reviewer read the whole diff against the row and the package conventions
(read-only, and told not to build because the gate was using the same `build/dev` trees).
Its verdict: **no defect that would break a build, a test or the frozen wire** — it compared
all six headers against their `HEAD` originals (the two audit events byte-identical, the two
sinks one changed line each, `SocketEmitDto::toJson()`'s statements character-identical as
they moved into the class body, the eight `sync-change` constants identical in spelling) and
then walked every changed line in the diff that is not an `#include` or a comment, finding no
touched JSON key, enum spelling or subject. It confirmed `argus_contracts` compiles no `.cc`,
that the two new edges sit on the test target only and never in the contract's `DEPENDS`
(where they would leak into 13 consumers), and that socket's PUBLIC link set lost nothing.

Its three findings, and what each became:

- **The `argus::audit` link in `packages/memory` is load-bearing while its stated reason had
  gone stale.** `memory-core`'s comment justified the link with `CameraAuditEvent`, which now
  arrives from `argus::contracts::sync`; but the link is what supplies the `<nats/…>` include
  roots `catalog-replica.{hxx,cc}` compile against, through the PUBLIC chain
  `audit → socket → lib::nats`, and the target links no `argus::lib::nats` of its own.
  Verified against the three CMakeLists, **fixed**: the comment now says what the link is for
  and what must be linked first if it ever goes.
- **Three of the moved headers used `int64_t` without `<cstdint>`** — `sync-change.hxx`,
  `user-change-sink.hxx`, `camera-change-sink.hxx`; the two audit events already had it.
  Pre-existing in the two sinks, but mine in `sync-change.hxx`'s new `RoleRoomChange`.
  **Fixed**: one include line each, because a contract's header has no translation unit of
  its own to catch it and this package's rule is that each header includes what it uses.
- **A figure in this report did not reconcile** — the vocabulary's consumer set was stated as
  35 with a per-unit breakdown summing to 30. Re-measured at `HEAD` and rewritten above
  (36 files naming any of the six symbols, 27 of them naming `SocketEmitDto`).

Its clean findings were also checked rather than believed: the stale-reference sweep found
**zero live references** to the old paths anywhere in the tree (the one hit is a historical
mention inside `docs/history/project-log.md`'s legacy-architecture narrative, which
describes the pre-cutover layout and stays), the socket's PUBLIC link set still names every
target its two files use, and `packages/socket` keeps `enable_testing()` so the subprojects
it adds still register their suites.

## Destinations, file by file

`packages/socket` (10 files): `dtos/socket-emit/socket-emit-dto.{hxx,cc}` and
`services/socket/sync-change.hxx` → `contracts/sync` (`toJson()` becomes inline, since
a contract is an INTERFACE target and may not carry a `.cc`); `contracts/camera-change-sink.hxx`
and `contracts/user-change-sink.hxx` → `contracts/sync` (they speak `TableName` and
`Json`, and a tier-5 producer reaching a tier-2 contract is legal); their
`Nats*ChangeSink` implementations stay with camera, productivity and notification, which
is §3.6's "publish path → `lib/nats` + each producer's own outbox" already true in the
tree; `services/socket/socket-service.{hxx,cc}` → `services/sync` (sub-step c);
`tests/unit/sync-change-test.cc` → `contracts/sync/tests/unit/`.

`packages/room` (3 files): `room-manager.{hxx,cc}` → `services/sync` (sub-step c), where
its four external includers become the service's own code or the control client's.

`packages/audit` (26 files): the three repositories, three schemas and four services →
`services/sync`; the two event contracts (`camera-audit-event.hxx`,
`user-audit-event.hxx`) → `contracts/sync` (their consumers are camera, gateway,
notification, productivity **and `packages/memory`**, which is the one consumer that
cannot follow them into a service).

`packages/sync` (98 files): the engine, the `/sync` socket, the forwarder and the three
page sources → `services/sync`; the twelve domain repositories and thirteen schemas →
their owners (C1); the four identity-owned reads it serves locally (`user`,
`user_invitation`, `person`, `event`) stay with it and keep reading through
`packages/identity` — the §3.4 debt row 4 recorded as "left to 3a/3c", discharged by 3c
when identity becomes a service and the read becomes a `clients/identity` call.

## Open, recorded rather than guessed

- Which sub-step owns the `notification_delivery_inbox` table, whose DDL sits in
  `packages/identity/database/schema.sql:219` while a **gateway** repository writes it
  (`services/gateway/src/shared/repositories/delivery-inbox/`). §3.4 gives it to
  notification; no row in Phase 3 names it.
- Whether the gateway's `notification-delivery-consumer` (JetStream durable
  `argus-gateway-delivery`) moves to `services/sync` with the rest of the fan-out, or to
  `services/notification`, which owns delivery. It sits under `src/sync/` and consumes a
  directed-emit path, which is the sync service's; the plan does not say.
- The seven-consumer set the plan names at `:804` and `room`'s four includers must both
  be repointed inside sub-step c, and the census that produced the 613-site count is a
  subagent's claim, not a measurement of mine: every site this report cites was checked
  against the tree, and sub-step c re-derives its own list before editing.
- Two surplus declarations were found and left alone, because deleting either is a change
  with a consumer risk and no gate to catch it: `packages/socket` still
  `find_package(nlohmann_json REQUIRED)` and PUBLIC-links `nlohmann_json::nlohmann_json`
  with no textual use in its two remaining files (pre-existing — `HEAD` is the same), so 13
  consumers inherit it; and the same package's `argus::lib::text` link is now surplus for
  its own sources. Both belong to sub-step c, which rewrites this file anyway.
- Stale `build/dev` trees still name the deleted `socket-emit-dto.cc` in their ninja logs,
  which is why `check-tidy` prints its "entries skipped in …; delete that build tree" notice
  for them. Generated residue, advisory only, and no gate fails on it.
