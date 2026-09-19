# Phase 1 step 4 — distributing the `access` vocabulary and deleting the package

Scope: `packages/access/src/shared/enums.hxx` dissolved — 32 enums into the unit
that owns each one, 2 deleted as dead — plus `role-access.hxx` and its suite
moved into `packages/auth` (the plan's `lib/auth`, renamed in Phase 2), and the
package itself gone. Base `1a00595`.

## What the package was

One header, `src/shared/enums.hxx`, holding **32** `enum class` declarations with
their `toString`/`fromString` helpers (64 inline helpers in total), and one
header of role gating, `src/shared/access/role-access.hxx` — the single
`UserRole → TableName → permission` table that both the HTTP `RoleFilter` and the
sync engine read. Header-only, a `Drogon::Drogon` link for `drogon::HttpMethod`,
two suites (`enums-test`, `role-access-test`).

It was the classic shared-vocabulary package: everything that needed an enum —
a camera record mode, a table name, a voice language — reached into it, which is
exactly what §3.1 rule 6 and D12 forbid ("enums live where they are used… no
shared enum package, and no service keeps a copy of a wire enum").

## Where the vocabulary went

| Unit | Header | Enums |
|---|---|---|
| `contracts/auth` | `user-role.hxx` | `UserRole` |
| `contracts/sync` | `src/shared/contracts/*.hxx` | `TableName`, `AuditLogPriority`, `UserAction` |
| `contracts/camera` | `camera-driver.hxx`, `camera-record-mode.hxx`, `event-severity.hxx`, `zone-type.hxx` | `CameraDriver`, `CameraRecordMode`, `EventSeverity`, `ZoneType` |
| `contracts/productivity` | `membership-error.hxx`, `reminder-detail-status.hxx`, `share-access.hxx` | `MembershipError`, `ReminderDetailStatus`, `ShareAccess` |
| `contracts/voice` | `voice-lang.hxx` | `VoiceLang` |
| `contracts/notification` | `notification-delivery-status.hxx` | `NotificationDeliveryStatus` |
| `packages/storage` | `src/shared/vocabulary/stored-file-category.hxx` | `StoredFileCategory` |
| `packages/phrase` | `src/shared/vocabulary/*.hxx` | `MemoryType`, `PhraseKind`, `LexiconKind` |
| `packages/identity` | `src/shared/vocabulary/person-status.hxx` | `PersonStatus` |
| `packages/memory` | `src/shared/vocabulary/encounter-closed-receipt.hxx` | `EncounterClosedReceipt` |
| `services/camera` | `src/objects/object-event-status.hxx` | `ObjectEventStatus` |
| `services/gateway` | `src/shared/vocabulary/*.hxx` | `FallbackDropReason`, `NotificationDeliveryReceipt` |
| `services/guard` | `src/feature/guard/vocabulary/*.hxx` | `GuardMode`, `GuardDanger`, `GuardActionKind`, `ObservationStatus`, `EncounterState`, `DecisionSuppression`, `GuardIntentStatus`, `FeedbackLabel` |

Deleted as dead: `MemoryScope` and `MemorySource`, with their four helpers and
their `enums-test` cases — no file in the tree names either the type or a helper.
`FeedbackLabel` was deleted with them and **restored** in the review (below): the
only spelling of it in the tree is its helper, `feedbackLabelFromString`, so the
grep that cleared the three looked for a name nothing writes.

**Deviation from the row's letter, and why.** The row sends `CameraRecordMode` and
`ZoneType` to `services/camera`, `ReminderDetailStatus` to `services/productivity`,
`EventSeverity` to its consumers and `StoredFileCategory` to `lib/storage`. The
first three cannot live inside a service: `packages/sync`'s repositories and
schemas for camera, zone, event, reminder-detail, notification, project-member and
calendar-event-share name them, and rule 3 forbids a package reaching into a
service's source. They go to the contract of the domain that owns them instead —
D12's "the wire vocabulary is declared once in the contract that owns it", which
is also what makes a client able to speak it. `StoredFileCategory` did go to
storage's own vocabulary (the `lib/storage` of the row is Phase 2's rename), and
`EventSeverity` went to `contracts/camera` because `packages/sync`'s event
repository names it too.

## The two halves of the work

**The split** (headers): every enum moved with its helpers into the header named
in the table above, include roots following each owner's existing convention —
bare name for a contract (`<user-role.hxx>`), `<shared/contracts/table-name.hxx>`
for `contracts/sync`, `<shared/vocabulary/…>` for a package, `<vocabulary/…>` for
guard's feature folder. The four contract packages that did not exist
(`contracts/{auth,voice,productivity,notification}`) were created on the
`camera-contract` model — an `INTERFACE` target, `contract::<domain>` alias, the
package directory as its include root — and given a vocabulary round-trip suite;
`camera-contract` itself had none at `HEAD`, so the suite it now carries is new
too, as are the ones in `sync-contract`, `storage`, `phrase` and `packages/auth`
(the moved `role-access-test`).

**The include rewrite**: 80 files in the diff carry the vocabulary change, and
almost all of them are one line — `<shared/enums.hxx>` out, the owner's header in
(the include is written the way each owner's own files write theirs). In 73 that
is literally what the diff shows. Seven only dropped the line: none of them names
a vocabulary type, so the include was noise rather than a dependency, and the
compiler accepts its removal. Thirteen had no line to swap — they named the
vocabulary and included nothing, because the old header reached them transitively
through a service header. Twelve of those thirteen are the audit below; the
thirteenth is guard's feature service, which the review added for `FeedbackLabel`.

**The CMake rewire**: `argus::access` disappears, so every target that linked it
had to link instead the aliases its own sources need. Rather than guess, a script
analysed the tree: for each `CMakeLists`, each target's declared sources were
resolved to the identifiers they name (enum names *and* the inline helpers beside
them, because a consumer can name only `tableNameFromString`), which maps to the
owning alias. It then edited the link lists, and added an
`if(NOT TARGET <alias>)` guard per newly needed subtree, in the house form (path
relative, `${CMAKE_CURRENT_BINARY_DIR}/<name>` for the binary dir).

## Verification

- **No vocabulary lost or changed**: 30 of the 32 enums are present exactly once
  in the tree — same values, same order, verified by extracting each enum body
  from `HEAD:packages/access/src/shared/enums.hxx` and from the new headers and
  comparing. Of the 64 helpers in the old header, the only four missing are the
  two dead enums' pairs.
- **No live vocabulary was buried and no dead vocabulary was kept**: each of the
  32 names, and each of its helpers' names, was searched across the tree — 30
  have at least one consumer outside their own header, and the two that have none
  (`MemoryScope`, `MemorySource`) are exactly the two deleted. This is the audit
  that `FeedbackLabel` failed on the first pass (§ review).
- **No stragglers in the build or the code**: no `CMakeLists` names
  `argus::access`, and no `.cc`/`.hxx` in the tree names `packages/access`,
  `<shared/enums.hxx>` or either header's path. `packages/access/` is gone from
  disk as well as from the index. The surviving mentions are all in prose that
  another step owns: `AGENTS.md`'s four (deferred, below), the citation
  `wire-sync-tables.md` carries of the old header it freezes values from (step
  13 moves that citation; the values do not change), and the historical plans and
  reports, which record what the tree looked like and are not rewritten.
- **Every consumer owns its subtree**: the 31 guards this step inserted follow
  the house rule the tree already uses — the provider adds its subtree
  unguarded, each consumer wraps it in `if(NOT TARGET <alias>)` and names a
  binary dir under `${CMAKE_CURRENT_BINARY_DIR}` (34 files already used that
  form; the 19 older absolute ones are untouched). One file, `services/tts`, has
  a duplicate guard of its own, pre-existing and harmless.
- **Build**: `./scripts/build-all.sh dev` is green on 18 of 18 projects with no
  first-party warnings — the per-project ledger is the section below.

## The gate

The count below is what each project *reaches* — the suites of every guarded
subtree its configure walks, not only the ones it owns — so a project that gains
a contract subtree also gains that contract's vocabulary suite. At `HEAD` (build
of `1a00595`) against the tree as committed:

| project | at `HEAD` | now |  | project | at `HEAD` | now |
|---|--:|--:|---|---|--:|--:|
| `cert` | 12 | 15 |  | `productivity` | 18 | 23 |
| `socket` | 6 | 8 |  | `notification` | 23 | 28 |
| `sqlite` | 2 | 2 |  | `guard` | 32 | 36 |
| `identity` | 12 | 15 |  | `tts` | 7 | 9 |
| `sync` | 14 | 19 |  | `stt` | 3 | 3 |
| `memory` | 14 | 16 |  | `vlm` | 5 | 5 |
| `intent` | 5 | 4 |  | `llm` | 21 | 23 |
| `gateway` | 20 | 25 |  | `voice` | 8 | 13 |
| `camera` | 29 | 34 |  | `tunnel` | 10 | 10 |
|  |  |  |  | **total** | **241** | **288** |

Every project that used to reach `enums-test` loses exactly that one suite, and
`intent` — the only project that goes down, 5 → 4 — loses `role-access-test` as
well, because it no longer needs `packages/auth` to name a phrase enum: the
dependency D12 exists to remove, gone where it was never a dependency of the
domain. The four unchanged projects (`sqlite`, `stt`, `vlm`, `tunnel`) reach no
new vocabulary; `voice` gains the most (5) because `clients/voice` and
`voice-core` now link `contract::auth` and `contract::voice`.

The ledger is one gate run over the code tree as committed — the prose in this
report and in the plan was written after it and touches no source. An earlier run
was discarded rather than cherry-picked: `services/gateway/CMakeLists.txt` was
reformatted after the gateway had already configured in that run, so its result
came from a file the tree no longer had, and the only honest fix was to run the
whole gate again. Both runs agree project by project.

## Code review of the change

Reviewed: the 23 `CMakeLists` in the diff (every guard path, every link list), the
moved files, the 30 vocabulary headers against the old bodies, the four
`conanfile.txt` that gained a requirement, and the whole tree for identifiers left
unreachable. Findings and their disposition:

- **A module's links can live in two places** — the script's first version edited
  only the `argus_module(... DEPENDS ...)` list of a target, so
  `target_link_libraries(argus_room PUBLIC argus::access Drogon::Drogon)` kept the
  dead alias while the `DEPENDS` list looked right. `argus_audit`, `argus_room`,
  `argus_phrase` and `sync-change-test` were affected; the fix edits the module
  span *and* every `target_link_libraries` call, and the re-run removed the
  remaining tokens without adding duplicate aliases (a duplicate is harmless in
  CMake, which is exactly why it would have gone unnoticed).
- **A token glued to a closing paren is still that token**: `argus_module(NAME
  phrase … DEPENDS\n argus::access)` left `        argus::access)` behind because
  the check looked at whitespace-separated tokens and the trailing `)` made
  `argus::access)` a different token. Found by grepping for the alias after the
  run, not by the run's own report. The empty `DEPENDS` line is gone too.
- **Files that named vocabulary they never included** — the reason the first two
  build gates failed. The old `shared/enums.hxx` arrived transitively through
  service headers, so a source could use `VoiceLang` or `MembershipError` without
  including anything. Twelve files broke; rather than iterate one compile error
  per gate run, an audit walked every file's first-party include closure and
  reported every identifier whose owner header was unreachable: 12 gaps in 12
  files (identity's `auth-service.cc` and `voice-client.hxx`; sync's
  notification repository; memory's graph repository; guard's repository; the
  gateway's delivery-inbox test; six files in productivity). All 12 got the
  include, each placed where its neighbours put theirs, and the audit now reports
  zero. The compiler is the only real auditor, but it stops at the first error.
- **`packages/auth` in a new closure means its third-party closure too**: making
  `packages/room` (and `packages/clients/llm-client`) link `argus::auth` pulled
  `find_package(jwt-cpp REQUIRED)` into four projects that did not declare it —
  `socket`, `memory`, `llm`, `voice`. Their `conanfile.txt` now requires
  `jwt-cpp/0.7.2`, the same version the other nine already carry. Checked across
  all 18 projects that every conan-provided `find_package` their reachable
  `CMakeLists` call is declared; those four were the only gaps.
- **MembershipError has no external consumer today**: unlike `ShareAccess` and
  `ReminderDetailStatus`, which `packages/sync`'s repositories name, it is used
  only inside `services/productivity`. It stays in `contracts/productivity`
  because it is that domain's error vocabulary and the sibling of the two enums
  that do cross — splitting one domain's vocabulary across a contract and a
  service folder would cost more than it buys. Flagged, no action taken.
- **Style pass on what the surgery appended**: three files had aliases appended
  inline (`argus::nats contract::auth contract::sync`) or at the wrong indent
  (`packages/identity`'s `DEPENDS` items landed at four spaces because the line
  they replaced was itself mis-indented). All rewritten one-alias-per-line to
  match the surrounding lists. `packages/audit` and `packages/clients/voice` kept
  a `DEPENDS` block whose items sat at the keyword's own indent; both now follow
  the house order (`… SOURCES INCLUDES DEPENDS`, 17 of the 24 module calls).
- **A "dead" enum was live, and the build gate is what said so.** `FeedbackLabel`
  was deleted with the genuinely dead pair, and the first full gate stopped at
  `services/camera` — which builds `guard-regression` — on
  `guard-repository.cc:974: error: 'feedbackLabelFromString' was not declared in
  this scope`. The enum is alive in two call sites (`guard-repository.cc`'s
  `setDecisionFeedback` and `guard-feature-service.cc:285`, both validating a
  label before it reaches the journal) plus the guard suites that assert the
  spellings. Why the clearing grep missed it: nothing in the tree writes the
  *type* — the only spelling a consumer needs is `feedbackLabelFromString`, whose
  first letter is lowercase, so a search for `FeedbackLabel` found nothing. The
  same reasoning that put live enums with their helpers (a consumer can name only
  the helper) was not applied to the deadness test, which is the one place it
  matters most. Restored byte-identical as
  `services/guard/src/feature/guard/vocabulary/feedback-label.hxx` — guard is its
  only consumer, so guard's feature vocabulary is where it belongs — with the
  include added at both call sites. The re-audit now searches every enum *and*
  every helper name: 30 of 32 live, the two dead ones dead.
- **`packages/clients/llm-client` now points one tier up** — the move of
  `role-access.hxx` into `packages/auth` is what makes `argus_llm-client` link
  `argus::auth`, and §2.4 puts `lib/auth` in tier 4 while `clients/*` is tier 3,
  whose row allows tiers 1–2 only (rule 1: *a tier may never point back up*). The
  edge is narrow and named: `tool-executor.cc` is the only file that reads
  `role_access`, and the tool framework is precisely what step 9 moves into
  `services/llm` as a feature (D16, plan line 803) — a service may depend on
  anything above it, so that move removes the edge by construction. Doing it here
  would mean performing step 9 inside this unit; flagged and deferred, not fixed.
  Every other target that gained `argus::auth` in this step is a service
  (`gateway`, `guard`, `notification`, `productivity`, `camera`, `tts`) or a
  legacy package that Phase 2/3a deletes or promotes (`room`, `sync`,
  `identity`), where tiering does not bite yet.

## Deferred, and findings outside this unit

- **`AGENTS.md` still names the deleted package** in four places (rule text at
  lines 37 and 148, the Key Files rows at 673 and 677). Step 11 — "refresh the
  stale docs that describe the old tree" — and step 12 — "rewrite AGENTS.md rules
  23–27 and the Key Files table" — own that file; this step did not touch it.
- Fifteen of the vocabularies have no round-trip suite and had none before:
  the split moved the suites that existed (`enums-test`'s cases were distributed
  into the new `*-vocabulary-test` targets — 15 of its 17 round-trip cases
  survive, the two that went with the dead enums aside) and created a
  round-trip suite only where the old one covered the enum. `SyncOperation` has
  `syncOperationToString`/`FromString` and no case, exactly as before, and
  `AuditLogPriority` has no helpers to round-trip. Writing missing coverage is
  not a distribution step.
- `packages/sync`'s cross-domain repositories and schemas still hold
  service-owned vocabulary (`CameraRecordMode`, `ShareAccess`, …). That they now
  reach it through contracts is this step; that they should not own them at all
  is Phase 3a, when `services/sync` is extracted.
- **`llm-client`'s tier-4 edge is step 9's to remove** — see the review bullet
  above. The plan already schedules the move that removes it; nothing here
  pre-empts that step.
- `services/tts/CMakeLists.txt` carries two `if(NOT TARGET argus::tts-client)`
  guards (one inside `PROJECT_IS_TOP_LEVEL`, one after it). Benign — the second
  condition is false — and pre-existing: this unit did not touch that file.
