# Phase 2 step 4 — the section 2.4 DAG, measured, and its three violated edges

Scope: the row is "Verify the DAG of §2.4 and make the violated edges compile the other way"
(`docs/history/plans/architecture-plan.md:819`), read against §2.4's tier table and the rules that
come with it — rule 1 (a tier never points back up, and the graph has no cycles), rule 3 (a service
depends on packages and clients, never on another service's source) and rule 6 (the vocabulary that
crosses the wire is declared once, in the contract that owns it). Base `cf58606`. No phase is done
until `./scripts/build-all.sh dev` is clean; the full gate ran at the end and the ledger is in
**Verified** below.

The row has two halves and both are measurements: *verify* the DAG (there was no inventory of it, and
§2.4 rule 7's mechanical check is row 5's, not this one's) and *repair* it by re-homing what causes
each violated edge — never by adding an exception to the table.

## Measured first: the whole graph, from both places an edge can be written

The inventory is a scratch script (`/tmp/argus/dag2.py`, deliberately not committed — row 5 turns
this measurement into `scripts/check-deps.sh`). It reads every tracked `CMakeLists.txt` under
`packages/` and `services/` and collects edges from **both** places one can be written, because a
scan that reads only `target_link_libraries` sees almost nothing in this tree: the `argus_*` helpers'
keyword lists (`DEPENDS`, `SYSTEM_DEPENDS`, `MODULES`) are where most edges live. `add_library` and
`add_executable` declarations are collected too, so a raw target name used as a dependency resolves
back to the package that declares it. Tiers are §2.4's table exactly, including its two special
cases (`lib/http` is tier 2 although it is a `lib/`; `lib/auth` is tier 4), tier 5's same-service
exemption (a service's own nested `CMakeLists.txt` sub-packages are not "another service"), and
alias self-edges are skipped.

At `HEAD` that is **54 helper declarations, 426 edges, 208 third-party mentions over 23 distinct
roots** (doctest ×98 and Drogon ×35 lead; the tail is llama.cpp, sherpa-onnx, rnnoise and the two
vendored targets). One correction for row 5's script falls out of this: §2.4 rule 7 describes the
mechanical check as reading `target_link_libraries`, and measured, that alone would see almost none of
the graph — the helpers' keyword lists (`DEPENDS`, `SYSTEM_DEPENDS`, `MODULES`) are where a first-party
edge is written, `target_link_libraries` is where it is repeated for the targets that do not go through
a helper.

Three of the first run's findings were the tool's own errors, and they are worth recording because
row 5 will hit them: `packages/lib/http` classified as tier 1 (16 false forbidden edges); `ALLOWED`
sets that let tier 3 reach other clients and tier 5 reach any service; and alias edges counted when
the dependency package *is* the linker's own package, which made every test executable linking its
own package's alias look like a T3→T3/T4→T4/T5→T5 edge. Fixed in the tool before any repair was
attempted.

What the corrected inventory says about the tree at `cf58606` (measured there in a scratch worktree, so
the `HEAD` figures are measured rather than remembered):

| finding | count |
|---|---|
| edges forbidden with both ends classified | **2** — `packages/clients/llm` → `argus::lib::auth` (tier 3 → tier 4, in the `argus_clients(NAME llm … DEPENDS …)` list at `packages/clients/llm/CMakeLists.txt:39`) and `services/camera` → `argus::guard` (tier 5 → tier 5, `services/camera/CMakeLists.txt:375`) |
| cycles in the unit graph | **2** — `packages/lib/cert ↔ packages/identity` and `services/sync ↔ services/identity` |
| edges touching one of the seven packages §9.1 has not moved yet (`audit`, `identity`, `intent`, `memory`, `room`, `socket`, `sync`) | **111** |
| third-party mentions | 208 over 23 roots |

So the tree had exactly **two** live violations of the table — the client pointing up at tier 4 and one
service compiling another service's source, the second of which arrived with a third defect, the duplicated
wire enum — plus **111** edges the table cannot judge at all, because one end of each is a package §9.1 has
not moved yet. The row fixes the two violations and the duplicate, measures the third category and hands
each of its 43 illegal-once-moved edges to the step that owns the move.

## Fix 1 — the T3→T4 edge: `RolePermission` moves down to the contract

**The edge.** `packages/clients/llm` (tier 3) linked `argus::lib::auth` (tier 4) and
`argus::contracts::auth`. §2.4 rule 1 forbids a tier-3 unit pointing at tier 4. Phase 1 step 9 had
already forecast this in its own record: the machinery that needed auth moved into `services/llm`,
"but `tool-contracts.hxx:5` still includes `role-access.hxx` for `RolePermission` and
`llm-client/CMakeLists.txt:45` still links `argus::auth`, so the tier-3 → tier-4 edge survives,
narrowed to one header and two `ToolDescriptor` fields, with the candidate fix (the gate vocabulary
declared in `contracts/auth` beside the `TableName` `sync-contract` freezes, D12) belonging to
Phase 2 step 4" (`architecture-plan.md:806`). That is this row, and the forecast was right about the
shape: **one enum** was the whole surviving dependency.

**The repair.** The row's instruction is to make the edge compile the other way, which means moving
the thing depended on, not the dependent. `RolePermission` went to
`packages/contracts/sync/src/sync/role-permission.hxx` — tier 2, beside `TableName`, which is where
it belongs by rule 6's own logic: a permission names no table of its own and is only meaningful as
the third element of the gate's `(role, table, permission)` triple. The measured consumers agree:
`lib/auth` (the `kTableAccess` map and `permissionForMethod`), the tier-3 llm client (two
`ToolDescriptor` fields), `packages/memory` (six descriptors) and four hand-gating call sites in
three services (`sync`, `camera`, `productivity`). It has no `toString`/`fromString` pair, unlike
every other vocabulary in that folder, because it never crosses the wire — measured: no schema, no
query and no serializer names it; the gate compares the enum directly.

`packages/clients/llm` then shed both links (its header now reads `TableName` and `RolePermission`
from `contracts/sync`) and its `CMakeLists` comment was rewritten to say so. `services/llm` — which
really does read `role_access` in `tool-executor.cc` — declares `argus::lib::auth` itself, in the
house guard form. That last change exposed a latent defect the transitive include had been hiding:
`tool-executor.hxx:13` and `lfm-adapter.hxx:16` both name `UserRole` and neither included
`<auth/user-role.hxx>`; `--only llm` failed with 13 errors in those two headers the first time the
edge came out, and both now include what they use.

Measured after: `--only memory` exit 0 (22/22), `--only llm` exit 0 (32/32).

## Fix 2 — the dead link at tier 1: cert's suite never used identity

**The edge.** `packages/lib/cert`'s suite linked `argus_identity`, and because of it the whole
`PROJECT_IS_TOP_LEVEL` block in that package's `CMakeLists` existed — 47 lines (`CMakeLists.txt:37-83`
at `HEAD`, 54 with the comment that explained it) adding room, sqlite-vec, sqlite, socket, audit,
`lib/auth`, ncnn and identity's migration tool. The package's own
`AGENTS.md` stated the belief it rested on: the listener the cert suite stands up "is an identity
route, so the test links `argus_identity` and the configure has to be able to bring up identity's
own closure".

**Measured: that belief is false.** The 356-line suite names no identity token at all — it stands
Drogon's own listener up (`drogon::app().setSSLFiles(...)` plus `addListener("127.0.0.1", port,
true)`) over the rotated pair and reads only the cert service and the TOML reader. The link was dead
weight, and with it went the closure it dragged in.

This is also a **rule 1** repair, which is why the row owns it rather than deferring it: rule 1 says
"the graph has no cycles. Two units that need each other are one unit", and cert and identity linked
each other — identity's own `CMakeLists` said so in as many words ("this module and argus_lib_cert
link each other"). After the removal the edge is one-way (identity → cert), and the comment was
rewritten to the truth the tree now carries.

Measured after: `--only cert` exit 0, `cert-san-test` passed in 5.01 s, "0 tests failed out of 2".

## Fix 3 — the T5→T5 edge: camera's suite compiled another service's policy, and a wire enum had two homes

**The edge.** `services/camera`'s `camera-operator-test` compiled
`services/guard/src/feature/guard` and `tests/unit/guard-consumer-check.cc` into itself, and pulled
three clients (`llm`, `vlm`, `notification`) into its configure to feed that build. §2.4's tier 5
"may never depend on another service's `src/`" — the one edge the inventory found that the table
forbids today.

**Measured before removing it.** The cross-check asserted two things, and guard's own suite already
asserted both: parse-level identity reading (`guard-policy-test.cc`'s "an agreeing known claim keeps
the known reading", "a contradictory known claim never grants the known reading", "an unknown
identityState string fails closed to unrecognized") and the alert-zone escalation of an unknown
person ("an unknown person in an alert zone is critical", "a known companion never shields an
unknown primary"). Camera's own suite already pins the serialised triple the cross-check was built
on (`camera-operator-test.cc`: `identity == "known"`, `personId == 7`, `identityState == "known"`).
The only assertion no other case held was `hasKnown + inAlertZone → GuardDanger::None`.

**The second half of the same defect, and the reason the enum moved.** `IdentityState` was declared
**twice**, byte for byte, in `services/camera/src/operator/known-person-matcher.hxx` and
`services/guard/src/feature/guard/guard-policy.hxx` — with the two unreachable default returns
disagreeing ("unobservable" vs "unrecognized", both dead because the switch is exhaustive). That is
exactly the copy §2.4 rule 6 forbids: camera's matcher writes the spelling onto every track-bound
person object (`event-intelligence.cc:184`), guard's policy parses it back
(`guard-policy.cc:86-90`), the gateway's camera-notifier compares the string
(`camera-notifier.cc:159`), and the frozen wire is documented in
`docs/architecture/wire-nats-subjects.md:164`.

**The repair.** `IdentityState` and its pair are declared once, in
`packages/contracts/camera/src/camera/identity-state.hxx` (tier 2), shipped with the payload they
belong to. Both services' copies are gone; guard's parse is now
`identityStateFromString(stateName)` and camera's serialiser the contract's
`identityStateToString`. The semantics are preserved input by input: the legacy `identity` field's
`stateName.empty()` clause stays where it was (it is a statement about a legacy producer's payload,
not about the enum), and the fail-closed default is the one guard already had. The contract's suite
gains the round-trip and the fail-closed cases; guard's suite gains the one uniquely-held assertion
plus its contradictory twin.

Then the cross-service check itself went: the file (`services/camera/tests/unit/guard-consumer-check.cc`),
its forward declaration and its test case in `camera-operator-test.cc`, and in camera's `CMakeLists`
the `argus::guard` link, the `if(NOT TARGET argus::guard)` block and the three-client `foreach` that
existed only for that block.

## What is deferred, measured, and to whom

The table's own scope today cannot classify the seven packages §9.1 has not moved yet, and two of
the repairs above were only possible because their links were dead. The inventory therefore projects
the destinations onto the graph, and the projection is what Phase 3 inherits:

| deferred | sites | owner |
|---|---|---|
| `packages/sync` → `packages/identity` (live: `synchronized-service.hxx:16,18,19` include identity's `person`, `user` and `user-invitation` repositories) | 1 link | 3a + 3c — §3.4 settles it: "cross-domain reads travel through `clients/*`" |
| producers → `services/sync` (`argus::audit`, `argus::socket`, `argus_sync`): camera 8, notification 13, productivity 6, identity 2, gateway 2, llm 1 (through memory's `argus::audit`) | 32 | 3a step 2 (the row names camera, guard, notification, productivity and identity) |
| → `services/identity`: gateway 1, productivity 1, sync 1 | 3 | 3c |
| `argus::room` consumers (camera 2, gateway 1, sync 1, socket 1) | 5 | 3a — §9.1's room row already lists these consumers |
| `packages/room`'s own links (`lib/auth`, `contracts/auth`, `contracts/sync`) — they disappear with the package rather than become illegal | 3 | 3a |
| **cycle** `services/sync → services/identity → services/sync` | 1 cycle | 3a and 3c, in that order — the plan's own ordering |

**43** edges are what the projection reports in total — 35 that become cross-service, 5 that lose their
peer to room's death and 3 that are room's own links. Exactly one cycle appears; both are the moves' work,
not this row's. What this row leaves behind is the guarantee that **nothing the table can classify today
violates it**, so row 5's script can be written against the table as it stands and Phase 3's repointing is
what will keep it true.

## A pre-existing flake the gate surfaced, measured and fixed in the suite

The guard project's re-run after the report's comment edits failed one test: `cert-san-test`, at
`cert-san-test.cc:349` — after the hot reload the listener presented the certificate it had already
been presenting. It is **not** this row's doing (neither the suite nor `cert-service.cc` was among the
three fixes' files; the row only removed a link), but the phase gate is only worth what its verdict is
worth, so it was traced to the root cause before anything was changed — and the fix is now this row's
last change.

The mechanism, read out of the vendored sources rather than guessed: `rotateServerCertificate()` calls
`drogon::app().reloadSSLFiles()` from whatever thread rotates
(`cert-service.cc:468`), and `trantor::TcpServer::reloadSSL()` applies the new context **only
immediately when the caller is already on the listener's loop thread** — otherwise it queues a lambda
(`TcpServer.cc:238-256`). The test's listener runs on its own thread (`runner`), so the reload is queued,
and a handshake that starts before the loop drains its pending functors is still served by the previous
leaf. That is what the failure shows: the rotation did happen (the leaf on disk changed — the missing
`INFO` line is only because the test lowered the log level to `kWarn` just before) while the served
fingerprint did not.

The fix is in the test, not in the service: the assertion's claim is "the hot reload reaches the
listener", and a single read cannot distinguish "not yet" from "never". `servedPeerAfterReload(port,
previous)` polls until the served fingerprint differs, bounded at 5 s, in the idiom the file already uses
in `waitForBoot` and in `servedPeer`'s own connect loop; the same assertion still fails if the reload
never lands. The production path stays as it is: the rotation loop's reload arriving one loop iteration
later is milliseconds, and the queued branch is Drogon's own contract for callers off the loop.

## The documentation sweep

Eight files, each one falsified by one of the three fixes:

- `packages/lib/cert/AGENTS.md` — the standalone claim about an identity route and identity's
  closure.
- `packages/identity/CMakeLists.txt` — "this module and argus_lib_cert link each other". The six
  in-transit siblings stay behind `PROJECT_IS_TOP_LEVEL`, but that is no longer the reason;
  unwrapping them is recorded as the moving phase's call rather than done here.
- `AGENTS.md` (root, §7) — the role table's prose now names where `RolePermission` itself is
  declared.
- `packages/contracts/sync/AGENTS.md` — a layout entry for `role-permission.hxx`, including why it
  has no round-trip pair.
- `packages/contracts/camera/AGENTS.md` — a layout entry for `identity-state.hxx` and the vocabulary
  suite's count corrected from four enums to five.
- `packages/clients/llm/CMakeLists.txt` — the header's comment, rewritten for the two halves it now
  reads from `contracts/sync`.
- `packages/lib/cert/CMakeLists.txt`, `services/llm/CMakeLists.txt`, `services/guard/CMakeLists.txt`,
  `services/camera/CMakeLists.txt` — the guards' own comments.

Deliberately **not** touched: `docs/history/**` (a Done row and a report from Phase 1 describe what
those steps did, including building `guard-regression`), and
`docs/architecture/wire-nats-subjects.md`, whose `identityState` semantics this row preserves to the
letter — the contract header is now the code that implements what that document froze.

## Verified

`./scripts/build-all.sh dev` was run in full twice, and the two runs are one measurement: the first
supplies the ledger below (it is the run whose log can be compared to row 3's), and the second — after
the cert-suite fix recorded above — is the row's verdict.

**The first run**: exit 0, "All selected projects built and tested (profile: dev)", 18 of 18 projects,
and no compiler warning at all — the 21 `Warning` lines in the log are third-party configure notes only
(ncnn and glslang lowering `CMAKE_CXX_STANDARD` in their own trees, llama.cpp's ccache notice and openfst
under stt's build). A later `--only guard` re-run then failed, and the failure was not this row's: it was
the pre-existing `cert-san-test` race, root-caused in the vendored sources, fixed in the suite and
described in its own section above. `--only cert` came back 2/2, and the full gate was re-run.

The reached-test ledger went **428 → 405**, and every one of the 23 suites that left or arrived is named.
Both gate logs segment by project and print each test as it starts, so the two runs were compared test name
by test name (row 3's log against this one): four projects moved and fourteen are identical.

| project | before | after | what changed |
|---|---|---|---|
| cert | 22 | 2 | −20: identity's whole closure — `identity-client-test`, `identity-migration-test`, `grpc-server-identity-test`, `device-credential-test`, `nats-wrapper-test`, `sha256-test`, `storage-vocabulary-test`, `validation-dsl-test`, `error-definition-test`, `api-response-test`, `sync-change-test`, `sync-contract-{catalog,vocabulary}-test`, `auth-contract-{catalog,vocabulary}-test`, `identity-contract-catalog-test`, `identity-grpc-client-test`, `role-access-test`, `thread-budget-test`, `voice-contract-vocabulary-test` |
| camera | 53 | 50 | −3: the three clients' suites the removed guard block configured — `llm-client-test`, `vlm-client-test`, `notification-client-test` |
| guard | 47 | 49 | +2: `contracts/camera`'s suites, `camera-contract-{catalog,vocabulary}-test` |
| voice | 25 | 23 | −2: `role-access-test` and `thread-budget-test` — the second owned by `packages/lib/runtime` and pulled in by `packages/lib/auth/CMakeLists.txt:31`, both reached from voice only through the llm client's removed guards |

The two suites voice lost are the sharpest illustration of what a link that only exists in a consumer's
configure scope costs: one edge in `clients/llm` was making voice's project build and run lib/auth's and
lib/runtime's suites. Neither is lost from the tree — `role-access-test` runs in eleven projects and
`thread-budget-test` in thirteen. The same is true of everything cert dropped: the 20 suites left cert's
*project*, not the repository, and every one of them still runs where its package actually lives.
`contracts/camera`'s suites, meanwhile, gained a project rather than losing one.

Per-fix, before the full gate, each with its own configure and ctest run (all exit 0): `--only memory`
22/22, `--only llm` 32/32, `--only cert` 2/2 (`cert-san-test` 5.01 s), `--only guard` 49/49, `--only camera`
50/50.

**The second run, and the verdict**: exit 0, `[setup] All selected projects built and tested (profile:
dev)`, 18 of 18 — and the same ledger to the suite: **405** in total and the same eighteen per-project
counts (cert 2, socket 13, sqlite 2, identity 22, sync 29, memory 22, intent 4, gateway 41, camera 50,
productivity 34, notification 39, guard 49, tts 18, stt 6, vlm 7, llm 32, voice 23, tunnel 12), with
`cert-san-test` green in each of the eight projects whose configure reaches it at the same 5.01 s as
before the fix — the poll costs nothing while the reload has landed, and its 5 s bound is reached only in
the failure it exists to detect. The failing run and this one are the same test with one difference: it
now waits for the reload instead of reading once and assuming it.

The inventory re-run at the end of the row: `declarations: 54  edges: 424  forbidden: 0`, 110 edges
touching a package §9.1 has not moved yet, 43 of which the projection calls illegal once the moves land
(35 that become cross-service, 5 whose peer dies with `room`, 3 that are `room`'s own links), and the
single cycle `services/sync → services/identity → services/sync`. Nothing the tier table can classify
today is violating it, which is the state row 5's `scripts/check-deps.sh` will be written against.
