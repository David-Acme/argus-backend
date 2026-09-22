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

**C8 — the frame does change spelling in transit, and this report said twice that it did
not.** Both the design paragraph above and the first draft of "as executed" asserted that
`info` stays the JSON string the WS envelope carries and that "no frame changes shape in
transit". Measured, the two spellings differ on two of the three fields. The envelope,
`SocketEmitDto::toJson()` (`contracts/sync/src/sync/socket-emit-dto.hxx:22-31`, emitted via
`json_util::toString` at `socket-service.cc:33,40,52,70`), writes
`{operation: <int>, option: <table name>, info: <object>}`; the gRPC frame
`SyncFrame{operation, table, info}` carries the *number* where the envelope carries the
name and the *text* where the envelope carries the object. Same triple, different
spelling — which is exactly why the service that answers these calls rebuilds the payload
at the far end rather than handing a `SocketEmitDto` back by assignment, and why the
mapping's job is a conversion and not a copy. Three artefacts carried the old claim and
were corrected in place: `argus/sync/v1/sync.proto`'s preamble, `sync-client.hxx`'s class
comment and `sync-client.cc`'s `toFrame` comment. The service is named
**`SyncControlService`**, not `SyncControl`: buf lint's STANDARD rules require a service
name to end in `Service`, every one of the nineteen sibling services under
`packages/contracts/proto/` obeys it, and the plan never spells this one — so the name
follows the tree's convention rather than a plan silence. Roles are the one field that
does *not* need a conversion at the far end: they already cross as names (C3).

## The sub-steps this row runs as

The row covers four packages and 613 reference sites across nine units, so it runs as
four sub-steps, each building on the previous and each keeping the tree green — the
shape Phase 2 step 2 used for its forty-six packages. Sub-step **a** turned out to hold
two independent units of work — a pure move of frozen vocabulary, and the creation of a
new control wire with its client — so it runs as **a1** and **a2** rather than as one.

| Sub-step | Scope | Commit |
|---|---|---|
| **a1** | vocabulary → `contracts/sync`: the emit DTO, the change builders, the two sinks, the two audit events, the change suite; every consumer repointed; `packages/socket` reduced to its transport | this report |
| **a2** | the control wire: `argus/sync/v1/sync.proto` and `packages/clients/sync` | `build: add the sync control wire and its client` |
| **b** | the twelve cross-domain repositories and their schemas to their owner services; `packages/sync` sheds them | `build: move the twelve cross-domain repositories to their owner services` |
| **c** | `services/sync` created (WS, rooms, fan-out, audit persistence, journal, control RPC server and client); gateway's sync surface removed; identity's audit and journal writes onto the wire; `packages/{socket,room,audit}` deleted; the gate's array 18 → 17 | `build: serve /sync from argus-sync and retire socket, room and audit` |
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
holds the enum, the wire holds the name — and `info` crosses as the row's JSON text. The
sentence that stood here claimed that spelling *was* the WS envelope's and that "no frame
changes shape in transit": measurement refuted both halves (C8), and the frame's typed
spelling is converted back at the far end instead.

`packages/clients/sync` follows `packages/clients/identity` file for file (rule 23): the
`argus_clients(NAME sync PROTO_ROOT … PROTO argus/sync/v1/sync.proto …)` macro, a thin
wrapper over `argus::sync::v1::SyncControlService::StubInterface` built on `lib/grpc`'s
`makeChannel` + `setDeadline` + `addFleetSecret`, `virtual` methods that return `bool`
(false when unreachable — the house spelling for an ack with no payload), and a doctest
suite that opts back into the default build because the folder arrives through
`EXCLUDE_FROM_ALL`. The file-for-file claim does not survive in one place — the
constructor's parameter shape, which the tidy ratchet forced into a config struct; the
measurement that chose it is in "as executed" below.

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

## The review of sub-step a1

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

## Sub-step a2, as executed

**The wire is new, and it is additive.** `argus/sync/v1/sync.proto` declares one service,
`SyncControlService`, over four messages: the three requests and `ControlAck{ok, reason}`. It
imports `contracts.proto` and carries `SyncFrame` for the two row-carrying calls, which is
the whole reason the row exists in this order — a2 could not be written before a1 had put
the frozen triple in a package the client can reach. §1.8 is satisfied by construction:
nothing in the new file names an existing subject, frame key or enum value, and
`contracts.proto` is not touched. Roles cross as `string` (C3's rule: the caller holds the
enum, the wire holds the name), and `info` crosses as the row's JSON *text*, which the
service converts back into the envelope's object at the far end (C8 — the frame's two
spellings are recorded there rather than conflated). The reply is a message rather
than a bare `bool` because gRPC has no other way to say it; `reason` is written by the
server for diagnosability on the wire, and the client's surface is the bool — recorded
below as the one piece of this leg that has no reader until c's server exists.

**`packages/clients/sync` follows `clients/identity` and `clients/voice` file for file.**
`argus_clients(NAME sync …)` with **two** protos listed, not one: the macro generates per
file, so an imported schema has to be compiled in the same call or `SyncFrame`,
`SyncOperation` and `TableName` are missing at compile time — and this package is now the
**only** CMakeLists in the tree that compiles `contracts.proto` (measured:
`/bin/grep -rn "sync/v1/contracts.proto\|sync/v1/sync.proto" --include=CMakeLists.txt`
returns these two lines and nothing else). The surface is three `[[nodiscard]] virtual
bool` methods, all `const`, and no local DTO: the two frame-carrying calls take the
`SocketEmitDto` the caller already holds and the role call takes
`sync_change::RoleRoomChange`, both from `contracts/sync`. The `.cc` holds the channel, the
one deadline (`kCallTimeoutMs` = 5000), the fleet secret and `toFrame`, the single mapping
this leg needs. Refusals stay local where they can be proved: a non-positive `userId` never
opens a socket, and role names are deliberately **not** validated here — the names arrive
from `userRoleToString`, and `userRoleFromString` is the authority on what they mean.

**Two of a2's own design decisions changed while writing it, both measured.**

- The design table above spells the role call `replaceRoleRooms(id, oldRole, newRole)`.
  Written as **`replaceRoleRooms(const sync_change::RoleRoomChange&)`** instead: the struct
  is the contract's own spelling of exactly this wire message — the one C3 created in a1 —
  and `clients/identity` sets the house precedent that a multi-field input is a struct
  (`UpdateUserNameInput`, `PromotePersonInput`, …), not loose arguments. The fields map 1:1
  onto the request either way; using the frozen type keeps a second spelling of the same
  three fields out of the tree.
- The three methods are `[[nodiscard]]`, which the design did not say.
  `modernize-use-nodiscard` is in `.clang-tidy`'s set and `scripts/lib/tidy-baseline.txt`
  holds it at **790**; `check-tidy` fails on any count that rises, so three new unmarked
  ack-returning declarations would have failed the gate by themselves. The alternative —
  writing a raised ceiling into the baseline — is what the ratchet exists to prevent, and
  rule 19's own reading settles it: an ack nobody reads is a refusal nobody sees.

**One behaviour difference is carried by the mapping, and it is unreachable from the call
sites that will use it.** Today the transport hands the room the text of
`SocketEmitDto::toJson()`, whose `info` is the row object as-is; over gRPC the row is
serialised with `json_util::toString` and parsed back at the far end. For every object row
the two are byte-identical, but a **null** `obj` would cross as `"info": {}` where
`toJson()` renders `"info": null`. Measured, the corner is unreachable: both identity sites
that build a disconnect frame assign an object (`updated.toJson()` at
`user-feature-service.cc:95`, a `Json::objectValue` with two assigned keys at
`auth-service.cc:522-525`), and a1's `sync-change-test` pins the builders' own shapes. It is
recorded here rather than papered over because c's server rebuilds the payload from the
wire, and that is where the equality will finally be checkable end to end.

**The suite reaches the gate through `packages/identity`, which is the only place it
can.** A client package that no project adds is configured by nobody, so its `add_test`
registers in no project and the gate cannot see it — and `services/sync`, the consumer that
will serve this schema, does not exist until c. The wiring added to
`packages/identity/CMakeLists.txt` is therefore two lines of the same shape as the block
above them: a guarded `add_subdirectory(../clients/sync … EXCLUDE_FROM_ALL)` at `:133-136`
and `argus::clients::sync` in the link list at `:228`. It is justified by measurement, not
by hope — identity is the sole owner of the four imperative call sites, three of them
carrying a frame (this report's table) — and c turns the link from a registration vehicle
into the real dependency, while the folder's `EXCLUDE_FROM_ALL` and the suite's opt-back-in
stay exactly as `clients/identity` has them.

**Measured effects.** `packages/clients/sync` is five files (this report's `Layout` claim is
checkable with `find`). Two CMakeLists name `argus::clients::sync` (identity's guard and
link; its own suite). **15** CMakeLists now name `argus::contracts::sync`, up from 14, the
addition being this client's `DEPENDS`. In the identity project, whose tree registers the
suite first, the ledger moved **22 → 23** tests with `sync-client-test` passing in 0.03 s.

**The gate measured the unit and rejected it, and the corrections below come out of that
measurement.** `./scripts/build-all.sh dev` over the tree as first written built all 18
projects and passed every suite — identity's ledger at 23, the suite's own cases among them —
and then failed at `check-tidy` with exactly three risen counts:

| check | with a2 | baseline | delta |
|---|---|---|---|
| `modernize-use-scoped-lock` | 302 | 295 | +7 |
| `bugprone-easily-swappable-parameters` | 53 | 52 | +1 |
| `performance-unnecessary-value-param` | 50 | 49 | +1 |

Nine findings, and the totals reconcile arithmetically: 3165 reported against 3156
baselined, the difference being exactly these nine. The seven are `std::lock_guard` in the
new suite, which `std::scoped_lock` replaces one for one. The other two sit on
`SyncClient`'s constructor — the `(std::string target, std::string fleetSecret = {})` shape
copied from `IdentityClient`, which carries both findings in the baseline already and
therefore may not be copied into a new file. Seven candidate shapes were measured with the
gate's own check set before one was chosen:

| shape | `...swappable-parameters` | `...unnecessary-value-param` |
|---|---|---|
| `(std::string, std::string)`, target not moved | fires | fires |
| `(const std::string&, std::string)` | fires | silent |
| `(std::string, std::string)`, both moved | silent | silent |
| `(const std::string&, std::string_view)` | silent | silent |
| `(std::string, std::string_view)` | silent | fires |
| inline definition carrying the default argument | fires | fires |
| `(SyncClientConfig config)` | silent | silent |

Two rows corrected an inference rather than confirming it: the default argument excuses
nothing (the sixth row is what an inline definition in the header would have been), and the
one escape needing no type change is moving both parameters — which here would mean
`makeChannel(std::move(target))` into a `const std::string&` that moves nothing, a cast
telling the reader the callee consumes a string it only borrows. The chosen shape is the
last row, and it is not an invention: `NotificationClientConfig{target, credential}` is the
same pair of strings declared the same way, and
`services/gateway/src/sync/notification-sync-source.cc:58-61` shows the call site it
produces — `.target = …`, `.credential = …`, a swap that cannot be written. `SyncClient`
therefore takes `SyncClientConfig`, with an omitted secret still meaning no secret.

**The measurement after the fixes.** clang-tidy over the two new translation units reports
three findings each — `modernize-use-nodiscard` at `socket-emit-dto.hxx:19` and
`modernize-return-braced-init-list` at `json-util.hxx:37` and `:43` — every one of them in a
pre-existing header whose location the baseline already counts, so the unit's own
contribution is zero, which is what rule 19 asks of a change. `clang-format -i` was applied
to the three sources (it had wanted different wrapping in three places) and
`clang-format --dry-run -Werror` is clean over all five files.

**The reviewer's documentation findings were folded in**, each verified before it was
accepted: the null-`obj` caveat now stands in `packages/clients/sync/AGENTS.md`'s frame rule
and not only here; "missing at link time" reads "at compile time"; the measured count is
four call sites carrying three frames, in the identity comment and in the client's
AGENTS.md; and `docs/architecture/services-and-packages.md:57` counted ten SDK clients where
`ls packages/clients` returns eleven.

## The review of sub-step a2

An independent reviewer read the whole diff against the row and the package conventions: the
wire, the client, the suite and the wiring. Its verdict was that the unit is structurally
sound, with one finding that would have failed the gate, one design risk, three
documentation errors and one informational note. Every one was verified against the tree
before anything was done with it, and two of those verifications moved what this report
believed:

- **The tidy rise, which would have failed the gate.** Reproduced independently and then by
  the gate itself — the table above is that measurement — and fixed, with the fix measured
  rather than reasoned because both of the first two guesses about what a check excuses were
  wrong: copying `IdentityClient`'s shape raises the ratchet, and the default argument (which
  the header carries) excuses nothing.
- **The generation order inside `argus_client_module` — claimed missing, measured present.**
  The finding was that `:419-435` runs one `add_custom_command` per proto inside a `foreach`,
  so the importer's generated files carry no edge ordering them after the imported proto's.
  The first half is true of the macro; the conclusion is false of the build. CMake gives
  every object of a target an order-only dependency on
  `cmake_object_order_depends_target_<target>`, a phony that lists **all** of the target's
  generated files. Measured in the tree this unit's gate built:
  `packages/identity/build/dev/build.ninja:14194` is that phony for `argus_clients_sync` and
  it names all eight generated files — both protos' `.pb.{cc,h}` and `.grpc.pb.{cc,h}` — and
  the five object rules that follow carry it after `||`: `contracts.pb.cc.o`,
  `contracts.grpc.pb.cc.o`, `sync.pb.cc.o`, `sync.grpc.pb.cc.o` and `sync-client.cc.o`. So
  there is no clean-tree race to fix: every object of this library waits for both protos to
  be generated, and the two `protoc` runs are unordered relative to each other only in a way
  that cannot matter, because `protoc` reads the imported `.proto` from the source tree and
  not its generated output. **Both this bullet's original claim and the Open item that
  recorded it are withdrawn**, and the `cmake/` change the finding proposed is not made —
  the absence it described does not exist, so patching the macro would have been a fix for
  nothing. The repeated-regeneration attempt recorded in the superseded bullet is what a
  finding looks like before it is read down to the ninja file it is about.
- **The three documentation errors**, each verified before being accepted: "unchanged by
  having travelled over gRPC" was too strong for the null-`obj` corner this report records a
  few paragraphs above; "missing at link time" described a failure that happens at compile
  time; and the call sites are four, three of them carrying a frame. **Fixed in place**, in
  the package's AGENTS.md, in identity's comment and in `docs/architecture/`. The first fix
  was a sweep and the sweep was not total: re-reading the tree for the phrase afterwards
  found it in two places the fix had walked past — the comment above `argus_clients` in this
  client's own `CMakeLists.txt` and the paragraph above — both corrected, so
  `/bin/grep -rn "link time" packages/` is empty and the only remaining mentions are the
  three quotes on this page that name the error while describing the fix.
- **The informational note** — the client is unused by construction, the identity link is a
  registration vehicle until c, and `ControlAck.reason` has no reader — is disclosed above
  rather than acted on, because c is the sub-step that gives all three a consumer.
  Re-measured rather than believed: `nm` over the **13** production executives the gate
  builds finds zero `SyncClientConfig` symbols, and the near-misses are worth the sentence
  because they are how the claim was almost got wrong twice — `SyncClient` as a substring
  matches `CameraSyncClient` and `ProductivitySyncClient` (in the gateway and in llm), and
  `replaceRoleRooms` matches the pre-existing `SocketService` and `RoomManager` methods that
  share the name with this client's and move to `services/sync` in step c.

## The second review of sub-step a2

A second independent reviewer read the corrected unit — the tree as it stood after the first
review's fixes, with the gate green. Its verdict was that the unit is sound and that **the
suite could not fail on two of the defects it exists to prevent**, plus five smaller
findings. Each one was reproduced or refuted against the tree before anything was changed
with it:

- **Two mutants the suite let through.** Deleting `frame.set_table(...)` from `toFrame`, and
  deleting `*request.mutable_frame() = toFrame(frame)` from `disconnectUser`, each leave the
  suite green. Established by reading the assertions rather than by running the mutants:
  the only frame a case inspected was the one `emitToUser` carried, and every assertion on
  it compared against a value the proto3 default already equals (`TableName::User` is 0 and
  `TABLE_NAME_USER` is 0), so the table assertion held whether or not the field was ever
  set — and no case read the `DisconnectUser` frame at all. **Fixed in the suite**, and the
  fix was designed to fail under exactly those two mutants: the two row-carrying calls now
  send *different* rows, the `emitToUser` leg the per-user audit row (`Log`, `UserAuditLog`
  — operation 6, table 17) so both of its fields are non-default, the `disconnectUser` leg's
  frame asserted separately against its own text, and the landing proof moved off the single
  shared header map and onto per-RPC call counters, so "the calls landed" covers all three
  calls instead of the last one. **Both mutants were then executed against the fixed suite**
  rather than only reasoned about: dropping `set_table` fails one assertion
  (`CHECK(0 == 17)` — the non-default table), dropping `mutable_frame()` in
  `disconnectUser` fails two (the frame's JSON text and its `resync` flag), and the suite is
  green again at 5 cases and 71 assertions with the mutated file's md5 identical to the hash
  recorded before the mutation.
- **`ControlAck` was written across threads with no lock.** The scripted service assigned
  `ack` from the RPC's thread while the test thread read it. Benign in practice — the write
  happens before the call is issued — and still a data race under the standard, so it was
  removed rather than argued about: `setAck(ok, reason)` locks, `answer()` copies under that
  same lock, and the fields it records were already guarded.
- **The service name.** `SyncControl` breaks buf lint's STANDARD `SERVICE_SUFFIX` and would
  have been the only one of the twenty services under `packages/contracts/proto/` not ending
  in `Service`. Renamed to `SyncControlService` in the proto, in the client's stub type and
  in both comments; the plan never names this service, so the tree's own convention decides
  (C8).
- **The frame spelling, which this report and the package's `AGENTS.md` both got wrong.**
  C8 records the measurement and the three source artefacts it corrected; the AGENTS.md
  paragraph that called the typed spelling "the spelling the WS envelope already uses" was
  rewritten to state the conversion instead.
- **`AGENTS.md` overclaimed the suite.** It said the suite pins the enum mirroring, which the
  old assertions did not. The suite now does: a new case compares all 24 `TableName` and all
  8 `SyncOperation` enumerators against their proto constants value for value, and pins
  `kLastTableName`, so a renumbering on either side fails and a table added beyond the
  mirrored set has to be listed. The same section records what the suite does **not** reach —
  the 5000 ms deadline and the empty-secret branch of `addFleetSecret` — because a coverage
  claim is worth only what it excludes.
- **Three smaller corrections.** The suite's target is now guarded like the client's, so a
  second consumer pulling this folder in cannot register the executable twice; identity's
  comment no longer reads "Identity is its only consumer" in the present tense, since the
  four call sites move onto the client in sub-step c and not in this one; and three citations
  in this report were off by a line or two (`user-feature-service.cc:93` → `:95`,
  `auth-service.cc:518-522` → `:522-525`, `packages/identity/CMakeLists.txt:133-139` →
  `:133-136`), each re-measured here rather than corrected by arithmetic.

Every one of those is a change to this unit, so the gate was run again over the corrected
tree — the section below is that run, and the numbers it prints are the corrected suite's.

## The gate of sub-step a2

**The run that stands is the second one — after both review passes — and it is green, with a
ledger identical to the first green run's, project for project.**
`./scripts/build-all.sh dev` **exit 0** on 18/18 projects with **416** reached tests — cert 2,
socket 13, sqlite 2, identity 23, sync 30, memory 22, intent 4, gateway 42, camera 51,
productivity 35, notification 40, guard 50, tts 20, stt 6, vlm 7, llm 32, voice 25,
tunnel 12 — no `***Failed` and no `Not Run`. The seven-test difference from the previous
gate's 409 is the new suite and nothing else: `sync-client-test` registers in exactly the
seven trees that add `packages/identity` (identity 22 → 23, sync 29 → 30, gateway 41 → 42,
camera 50 → 51, productivity 34 → 35, notification 39 → 40, guard 49 → 50), and the other
eleven counts are that run's line for line — `llm` included, whose own pre-existing
`camera-sync-client-test` shares the substring without being this suite. Running the tree
twice is what makes that checkable: `diff` over the two runs' per-project ledgers prints
nothing. The suite was also run by hand, outside ctest:
`packages/identity/build/dev/clients/sync/sync-client-test`
prints **5 test cases, 71 assertions**, all passing. `check-tidy: 481 TUs, 3156 findings
over 45 checks, baseline 3156` — the two translation units the unit adds — with no check
risen and none below it either, so the ratchet stands where measurement put it. The run
*before* the corrections is the one that failed at that step, which is what the table above
records; the two runs after them differ only in the suite's own case count (4 cases and 26
assertions → 5 and 71), because a case is not a ctest test.

**The dependency checker moved by exactly the edges the unit adds, and the deferred set by
exactly one.** Measured with the scanner itself over a `git worktree` of `HEAD` (the a1
tree): that tree reads `54 declarations, 424 edges, 0 forbidden, 0 cycles, 0 unresolved, 107
edges deferred to phase 3 (207 third-party mentions over 22 roots)`, this one reads `55,
427, 0, 0, 0, 108 (208 over 22 roots)`. The three new edges are the client's two —
`argus::contracts::sync` (3 → 2) and `argus::lib::text` (3 → 1), both legal and therefore
classified, which is why they raise the total and not the deferred count — plus identity's
link to `argus::clients::sync`, deferred because identity still has no tier.
`--list-deferred` diffed between the two trees adds exactly that one triple, printed at
`packages/identity/CMakeLists.txt:156`, and the third-party roots are unchanged.

**The contract package's own pre-commit validation was run, including the file
`git ls-files` cannot see.** `buf` is absent from this machine, so the fallback the package's
`AGENTS.md` documents is the one that stands: `protoc --descriptor_set_out=/dev/null -I
proto $(git ls-files 'proto/*.proto')` exits 0 over the nineteen tracked protos — and does
**not** see the new file, because it is still untracked and the command is written for a
tree whose protos are already staged. Run with `proto/argus/sync/v1/sync.proto` named
explicitly it exits 0 as well, and `--include_imports` over that one file emits a descriptor
set holding both `argus/sync/v1/contracts.proto` and `argus/sync/v1/sync.proto`, the
`SyncControlService` service among them — so the import resolves and the new schema is validated,
not merely imported by something that was.

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
page sources → `services/sync`; eleven of the twelve domain repositories and twelve of the
thirteen schemas → their owners (C1), the exception being the `event`/`person_event` pair no
owner can be found for (measured below); the three identity-owned reads it serves locally
(`user`, `user_invitation`, `person`) stay with it and keep reading through
`packages/identity` — the §3.4 debt row 4 recorded as "left to 3a/3c", discharged by 3c
when identity becomes a service and the read becomes a `clients/identity` call.

## Open, recorded rather than guessed

- Which sub-step owns the `notification_delivery_inbox` table, whose DDL sits in
  `packages/identity/database/schema.sql:219` while a **gateway** repository writes it
  (`services/gateway/src/shared/repositories/delivery-inbox/`). §3.4 gives it to
  notification; no row in Phase 3 names it.
- Where `event` and `person_event` go, since **no owner exists for them**: neither name has
  a `CREATE TABLE` anywhere in the tree (measured: `/bin/grep -rlE "CREATE TABLE (IF NOT
  EXISTS )?(event|person_event)\b" --include=*.sql --include=*.cc` over the working tree
  returns nothing outside `build/`, and `.superpowers/sdd/progress/` keeps the historical
  diff in which the old repo-root `database/schema.sql` was deleted — the pair went with
  it), `packages/identity/database/schema.sql` declares neither, and the census of every
  owner's schema (`camera`, `productivity`, `notification`, `guard`, `gateway`, `identity`,
  `memory`) finds no table either name could belong to. Both names are still frozen wire
  vocabulary (`table-name.hxx:19-20`, pinned by the contract suite), `Event` is granted to
  Owner in `role-access.hxx:38`, and the engine still holds `EventRepository eventRepository_`
  at `synchronized-service.hxx:62`, preparing statements against a table that has never
  existed. Sub-step b must therefore decide between keeping the pair with the engine (they
  are nobody else's data) and deleting a repository whose only exercise is
  `event-sync-empty-test.cc`; the earlier sentence in this report that grouped `event` with
  identity's reads was wrong and is corrected above — its three reads are `user`,
  `user_invitation` and `person`.
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
