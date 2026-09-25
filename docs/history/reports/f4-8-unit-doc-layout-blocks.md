# Phase 4 step 8 — every unit doc's layout block, measured

Plan row: `docs/history/plans/architecture-plan.md` "8 | Update every
`AGENTS.md`/`CONTEXT.md` layout block (several are stale today)".

## What the row asked for

The row was written in Phase 2, when several unit docs still described the
pre-migration tree. By the time this step ran, the staleness had moved: the
fenced trees were the smallest part of it, and the bulk was quantitative prose
that had drifted as the tree changed under it — include counts, link-line
enumerations, source-line counts and cited line numbers.

The scope of the audit is every unit doc in the tree: **70 files** (54
`AGENTS.md` and 16 `CONTEXT.md` under `packages/` and `services/`), 16 of
which carry a fenced block.

## The four staleness classes, and the instrument that measures each

Reading 70 documents and judging each sentence would have been an unbounded
review. The step was made checkable by splitting the docs' assertions into
four classes and building one measurement per class.

| Class | Claim shape | Instrument |
|---|---|---|
| 1 | a path inside a fenced layout tree | resolve every token in the fence against the filesystem |
| 2 | "N files include it" | `#include <root/name.hxx>` over every first-party `.cc`/`.hxx`, the include spelling being the doc token's path minus `src/` |
| 3 | link-line and file-count enumerations | `git grep -n -E "argus::clients::X([^-A-Za-z0-9_]\|$)" -- '*CMakeLists.txt'`, with `NOT TARGET` lines counted as path-add **guards**, not links |
| 4 | cited `path:NN` | resolve the path, then require the line to exist inside the file and to be non-blank |

Two calibration results made the instruments trustworthy. First, the include
instrument agreed exactly with ~25 claims that were already known correct, and
every client doc's include count proved accurate — so a disagreement is a real
drift, not an instrument artefact. Second, the link instrument had to be
repaired twice before it was usable: a `\b`-anchored search falsely matched
`argus::clients::camera-actions` when searching for `argus::clients::camera`,
and counting `if(NOT TARGET …)` lines as links made `services/camera` look
like an `argus::clients::identity` consumer when it only guards that package
by path. Both corrections changed findings, which is the point of calibrating
before judging.

## Class 1 — fenced layout trees

Two fences named directories that do not exist; both fixed.

- `services/tunnel/AGENTS.md` listed `src/controllers/` (there is no
  controller under `src/`) and described `src/server/` as "per-binary config
  resolution" alone, while the directory also holds the `/health` extras.
- `services/voice/AGENTS.md` listed `src/controllers/` and `src/server/`
  neither of which exists, and omitted the `src/shared/services/` (VAD, noise
  suppression, reaction engine), `src/shared/wrapper/` (the sample ring) and
  `src/test-support/` (the fake voice sink) it does have. Its `main.cc` line
  was rewritten to what that entry point actually does.

`services/{auth,camera,guard}/AGENTS.md`'s layout blocks were measured against
their trees and are accurate; they were left untouched.

## Class 2 — include counts

20 stale counts, every one of them in `packages/contracts/*/AGENTS.md`. The
drift direction was always the same: the header gained consumers as the
migration moved code, and the count stayed where it was written.

- `contracts/auth`: `user-role.hxx` 34 → 36 files, `auth-errors.hxx` 9 → 10.
- `contracts/camera`: `identity-state.hxx` 2 → 3, `camera-errors.hxx` 6 → 8.
- `contracts/identity`: `identity-errors.hxx` 10 → 11.
- `contracts/productivity`: `productivity-errors.hxx` 6 → 12.
- `contracts/sync`, 13 counts in one file: `sync-operation.hxx` 14 → 16,
  `table-name.hxx` 27 → 35, `user-action.hxx` 6 → 7, `sync-filter.hxx` 7 → 9,
  `sync-limits.hxx` 3 → 4, `syncable.hxx` 18 → 19, `sync-errors.hxx` 8 → 9,
  `socket-emit-dto.hxx` 17 → 19, `identity-change-sink.hxx` 7 → 8,
  `user-audit-event.hxx` 5 → 7, `module-audit-event.hxx` 9 → 11,
  `user-action-event.hxx` 2 → 4, `sync-control-sink.hxx` 3 → 5.

`table-name.hxx`'s bullet also carried a superlative ("the
second-most-included header of the ten"): measured tree-wide it is 8th overall
and 2nd among the contract headers, so the qualifier now says "of the contract
packages" and names the first (`<auth/user-role.hxx>`, 36).

## Class 3 — link lines, file counts, source-line counts

- `clients/camera`: `argus-llm`'s link is `services/llm/CMakeLists.txt:168`,
  not `:183`; `argus-llm`'s path-add is `:101`, not `:124`.
- `clients/identity`: `services/notification:355` (not `:389`) and
  `services/llm:167` (not `:183`); `argus-camera` is a **linker** the doc did
  not list (`src/feature/operator/CMakeLists.txt:21`, the known-person
  matcher), which makes ten owner trees; and the parenthetical claiming
  `argus-camera` reaches the identity RPC "without a link line of its own" was
  false — it has one, so the sentence now names `argus-notification` as the
  example that actually fits.
- `clients/notification`: the enumeration now reads five link lines in five
  CMakeLists, with the two in `services/notification` at
  `services/notification/CMakeLists.txt:131` and its `notification-rpc` module
  at `src/app/rpc/CMakeLists.txt:7`.
- `clients/productivity`: `services/productivity/CMakeLists.txt:113` and
  `:258`, `src/feature/sync:12`, `services/sync/src/feature/transport:22`.
- `clients/voice`: three link lines —
  `services/sync/src/feature/transport:23`,
  `services/sync/tests/CMakeLists.txt:83` and
  `services/voice/CMakeLists.txt:86`.
- `clients/camera-actions`: `services/camera`'s path-add is `:115`, not
  `:114`.
- `contracts/productivity`: the opening claimed `services/sync`'s adapter
  "reads it since". Measured, no source of the sync leg names `ShareAccess`,
  `ReminderDetailStatus` or `MembershipError` at all — `services/sync` only
  adds the package to its standalone tree by path. The opening was rewritten
  to say exactly that: nine CMakeLists in `services/productivity` link it, and
  the sync leg's path-add has no reader.
- `contracts/sync`: the fan-in claim said 30; measured, 46 CMakeLists name
  `argus::contracts::sync` — 45 consumers plus the package's own test links, 7
  of the 45 being argus-sync's own.
- `contracts/response`: the consumer list said four boundaries but named
  three modules; the Layout count is 13 files, enumerated.
- `contracts/auth`: its "twenty-five CMakeLists link it" was re-measured
  under the naming convention (link lines ∪ path-add guards: 24 link files ∪
  `argus-notification`'s guard-only file) and is correct as written.
- `contracts/camera`: "the executable's own `MODULES` list
  (`CMakeLists.txt:217`), two suites" was wrong twice — line 217 is
  `camera-controller-test`, and the executable's `MODULES` list does not link
  the contract at all (it reaches it through its modules). The three link
  lines in `services/camera/CMakeLists.txt` are three suites (`:217`, `:251`,
  `:273`), which keeps the sentence's own count of nine files in that service
  (its one CMakeLists plus the eight modules) exact.

## Class 4 — cited line numbers

The tree-wide check resolved 89 citations across the 70 docs (plus the ones
written relative to a directory named earlier in the sentence, hand-checked)
and found no citation pointing past the end of its file or at a blank line.
The stale ones were:

- `clients/camera`: `services/sync/src/config/sync-config.cc:54` → `:39`;
  `services/sync/config.toml.example:42-43` → `:43-44` and
  `argus-deploy/config.sync.toml.example:42-43` → `:43-44`.
  `argus-deploy/config.llm.toml.example:74-75` was measured correct.
- `clients/productivity`: `sync-config.cc:55` → `:40`; both examples'
  `:45-46` → `:50-51`.
- `clients/voice`: both examples' `:48-49` → `:53-54`.
- `services/notification/CONTEXT.md`: the `database/schema.sql:428-450`
  citation pointed into the deleted monolith file; the landed file is 101
  lines and the two legacy tables are at `:10-38`. Its neighbouring sentence,
  "No indexes exist on the two legacy tables, so the schema file carries none
  for them", was false in the other direction — the file carries two (`:22`,
  `:37-38`) — and was replaced with what the file holds.
- `services/productivity/CONTEXT.md`: `database/schema.sql:159-280` → the
  landed `:10-131`, and the "6 indexes" of the seven tables is 11 today.

## The adversarial review, and what it found

Two read-only reviewers were dispatched against the edited docs: one
re-measuring every quantitative claim about code, one re-checking every path,
layout tree and internal count. Reports are treated as untrusted input, so
every finding below was re-measured here before it was acted on, and each is
recorded with the measurement that decided it.

**Real, and fixed in this step** (15 findings, every one reproduced):

- `contracts/sync` called the error catalog "the eight refusals" in three
  places; the header declares **nine** (`IdentitySyncUnavailable` was added
  for the identity leg and the doc was never updated), and five of them are
  the `*Unavailable` 503 answers, not four.
- `contracts/auth` called its catalog "twenty-three definitions"; measured,
  24, the missing one being `RemoteNotAllowed` (the remote gate's refusal).
- Seven `packages/lib/*/AGENTS.md` consumer lists named units that do not
  exist any more (`gateway` was deleted in Phase 3d step 1c; `audit`,
  `memory`, `intent` and `socket` are not units — `memory` and `intent` are
  features of `services/llm`) and carried counts that no longer matched:
  `nats` 11 → 10 (and `auth` was missing from the list), `sqlite` 10 → 8,
  `text` 14 → 11, `runtime` 9 → 11, `config` 16 → 17, `errors`' "eight
  contracts … the `tts` client and five other libs" → eleven contracts, four
  wire clients and four other libs, `validation`'s composition (its count of
  10 was right, its named units were not).
- `contracts/{response,tts,stt,vlm}` each called themselves one "of the four"
  non-header-only contracts; measured, five packages under
  `packages/contracts` declare a compiled wire module (`llm`'s own doc already
  said five).
- `contracts/auth`'s parenthetical said `clients/voice` "only guards against
  the contract standing alone". It also links it (`CMakeLists.txt:21-22`, for
  the `<auth/user-role.hxx>` its header includes); the only file in the set
  that guards without linking is `services/notification/CMakeLists.txt`. The
  sentence's own "twenty-five" was right, and now says why: 24 link files plus
  that one guard.
- `contracts/notification` still listed the deleted `gateway` as a consumer;
  the delivery consumer, its inbox and the two suites live in argus-sync's
  `fanout` module now.
- Four cited line numbers had drifted under refactors:
  `services/llm/src/app/main.cc:65` → `:70-71` (the camera-target read),
  `voice-client.cc:140-142` → `:138-139` (the queue drop),
  `voice-client.cc:113-117` → `:119-121` (the identity metadata, inside
  `begin()`), and `services/guard/src/app/main.cc:149-156` → `:145-152` (the
  two action keys, whose cited range had picked up the VLM block below it).
- `lib/http`'s "the three responses built by hand" was loose in a way that
  mattered: the unmatched-route 404 and 405 are built through the same
  envelope factory as every other answer (`ErrorHandler`, which has no handler
  of ours to throw from), and only the CORS OPTIONS answer is constructed
  directly. The sentence now says which is which.

**The finding behind two of those numbers, which was not a document defect at
all:** the two catalogs are pinned by tests, and both tests pin a hand-written
table rather than the header — so when `IdentitySyncUnavailable` and
`RemoteNotAllowed` were added to their headers, the tables stayed at 8 and 23
and the tests kept passing while claiming to check "the catalog". Both tables
gained the missing entry and both pins were raised (8 → 9, 23 → 24), the
values copied from the definitions. That is a code change, so this step was
built rather than declared documentation-only — see Verification.

**Re-measured and found accurate as written** (not changed): the whole of
`packages/clients/*`'s include counts and link enumerations;
`contracts/sync`'s 46-CMakeLists fan-in; `contracts/camera`'s eleven
CMakeLists and its three suites; `contracts/productivity`'s nine;
`contracts/voice` two; the other contract catalogs' sizes against their own
pins (camera 9, identity 31, llm 10, notification 1, productivity 9, stt 11,
tts 9, vlm 11); `lib/auth`'s nine binaries, `lib/storage`'s three,
`lib/cert`'s one; the config citations across the twelve client docs; both
drogon header citations in `clients/vlm` (they resolve against the Conan
source package).

## The second review round

Two further read-only reviewers were dispatched against the edited documents
once the first round's fixes had landed. Together they raised **36
disagreements** over the same 70 files — six named paths or rosters that do
not exist (S1), thirteen falsified "only / no other / every" claims (S2), nine
counts (S3), eight layout blocks that omit real files (S4) — plus five claims
they quoted as ambiguous rather than asserting. Several were already correct
on disk when the report landed: the reviewers re-read files that were changing
under them and said so. Every finding was re-measured here; 35 were real and
are fixed, and the one kept is a convention, named at the end of this section.

**S1 — named things that do not exist** (six, all fixed):

- `packages/contracts/CONTEXT.md` credited `argus_contracts_substrate()` to a
  `packages/contracts/CMakeLists.txt` that does not exist. It is defined in
  `cmake/argus-module.cmake:153`, and each consumer calls it from its own
  file.
- `packages/contracts/AGENTS.md` listed a `memory` proto domain. The proto
  root holds eight domains under `proto/argus/`, and no `memory.proto` exists
  in the tree.
- The same file attributed `SYNC_LIMIT = 200` to the sync proto. The name is
  `SyncLimits::kMaxRows` in `sync/src/sync/sync-limits.hxx`, which
  `docs/architecture/wire-sync-tables.md:30-32` already cites; the proto
  carries the two enums and nothing else.
- `services/auth/CONTEXT.md` counted two features and kept a Phase 3b-2 item
  open. The folder holds three (`auth`, `device`, `session`) and the surface
  landed with nine routes.
- `packages/contracts/CONTEXT.md` spelled `grpc-client-base` without its
  extension.
- `services/guard/CONTEXT.md`'s route roster omitted `GET
  /guard/decisions/summary` and `POST /guard/decisions/{eventId}/feedback`.
  The controller registers ten routes; eight were listed.

**S2 — exclusivity claims, falsified** (thirteen, all fixed):

- `contracts/identity` explained two 500-status entries as "both call sites
  pass 500 explicitly". No call site passes a status — the definitions carry
  it — and the same-named pair sits in the auth catalog at the same 500, so
  "every other `SERVICE_UNAVAILABLE` answers 503" was false twice over.
  `contracts/stt` and `contracts/tts` carried the same sentence; all three now
  name the auth pair as the exception.
- `contracts/routes` said nothing in tier 1 may consume it.
  `packages/lib/http` is tier 2, links it, and reads all three constants in
  `route-announcements.cc`.
- `contracts/sync` listed `contracts/identity` among its consumers; that
  package names no sync header at all.
- `contracts/notification` called the service's repository a consumer of the
  delivery sink. Measured, the sink's includers are nine files of
  `services/notification` and four of argus-sync's `fanout`; the repository
  reads the status vocabulary, which is what the bullet now says.
- `lib/validation` said "no `.cc` anywhere in it" while its own Tests section
  names one. The library ships none; the suite does.
- `lib/http` claimed `/health` is what "every service serves" — guard
  hand-rolls its own at `services/guard/src/app/main.cc:103-115` — and that it
  is "the only package allowed to know that the wire is Drogon". Ten other
  packages include Drogon headers and fourteen link `Drogon::Drogon`. The
  bullet now makes the claim that holds: the one package that owns the HTTP
  wire, with the three direct responses named (tts's and llm's streaming
  controllers and guard's `/health`) and `lib/auth`'s filters shown to answer
  through the envelope instead.
- `lib/nats` said every subject constant appears in the wire document; three
  did not. Two are published and live — `argus.auth.v1.user-action` and
  `ARGUS_AUTH_CHANGE` — and were added to
  `docs/architecture/wire-nats-subjects.md`; the third is `argus.guard.v1.>`,
  a subscription filter nobody publishes on, which the package doc now says. A
  census of all 21 string-valued constants in `nats-subject.hxx` confirms it:
  20 rows, one constant with no row, and it is the filter.
- `lib/auth/CONTEXT.md` credited argus-camera with installing the user
  directory into the sync socket. It is argus-sync's boot
  (`services/sync/src/app/main.cc:82`).
- `clients/identity` called the sync gateway the client's "one consumer". The
  boot site that constructs it (`services/sync/src/app/main.cc:80`) and the
  owner's own `identity-sync-rpc-test` are named now.
- `clients/camera-actions` said one file declares the target and no file
  declares the credential. Both templates in the tree carry both keys
  (`argus-deploy/config.guard.toml.example:115-116`,
  `services/guard/config.toml.example:119-120`) and
  `scripts/lib/common.sh:312` mints the deploy pair's real secret.
- `services/sync/CONTEXT.md` listed five change feeds and called the identity
  action feed "the fifth". `change_feed::defaults()` returns six — the sixth
  is `argus-sync-auth-action` on `ARGUS_AUTH_CHANGE` — and both action
  journals keep 256 in flight, not one of them.

**S3 — counts** (nine, all fixed): auth's schema roster "three tables and
their three indexes" → four tables and five index statements; auth's `MODULES`
enumeration missing `argus::auth-auth`; productivity's "11 indexes" → 13
statements, two of them unique; notification's cited `:37-38` → `:36-37` (the
range's tail line was blank, which the citation instrument accepts — see the
note below); `clients/camera`'s suite "lines 21-35, `add_test` at 35" → 21-32
with `add_test` at 32; `contracts/camera`'s "four enums" → five, as the same
file's Tests section already said; `contracts/sync`'s `sync-change-test` "25
assertions" → 24 (measured: 24 `CHECK`, no `REQUIRE`); `contracts/CONTEXT.md`
naming two gRPC versions (1.83.1 against the vendored 1.82 stack) → 1.82.1
once, which is what `~/.local/argus-thirdparty/grpc/lib` holds;
`contracts/voice`'s "the smallest of the ten" → one of the smallest of the
twelve `argus_contracts` packages.

**S4 — layout blocks that omit real files** (eight, all fixed):
`contracts/sync` named 21 of its 23 headers and four of its five suites, so
`auth-change-sink.hxx`, `stream-retention.hxx` and `stream-retention-test.cc`
were invisible in it — and the same gap was in code, the module's `SOURCES`
list carrying 22 of the 23 headers, which this step closed with it (the
declared set and the directory are now equal, and it is the second round's
only non-document change besides the two test rows); `lib/sqlite` omitted
`transaction.{cc,hxx}`; `lib/phrase` omitted `vocabulary.hxx`,
`lexicon-kind.hxx` and `memory-type.hxx`; `lib/audio` omitted
`endpoint-detector.{cc,hxx}`; and four config rosters — notification,
productivity, tts, identity — omitted blocks their own templates carry. The
identity one now names `[identity]`, `[sync]` and `[face]`, which are its own
features' sections.

**S5 — quoted, not asserted**: the `jwt_ctx` count ("forty") is thirty-two,
measured by mapping all 33 `AuthContext::kJwtKey` look-up sites to their
enclosing methods (the voice relay finds-then-gets, so a site is not always a
handler); the tts bullet's "four tomls" became "four templates" with values
and line numbers, because the runtime configs it had been citing are generated
and gitignored while the templates are what the tree carries; and the
`grpc-health` rule was rewritten around a measurement, below.

One S5 item is kept as written: six client docs count link lines among
*external* consumers only — one short of the figure a count that includes the
package's own test target would give — and two sibling docs spell that base
out. The six are exact under the base they name, so the convention is recorded
here rather than paid for with six sentences of arithmetic caveat.

### The five-edge experiment

`grpc-health` had every appearance of a dead dependency: no first-party code
builds a health stub, and the five `app/rpc` modules that name it — auth,
camera, identity, productivity, sync — reference no health symbol anywhere.
Removing `argus::lib::grpc-health` from those five `DEPENDS` lists was tried
rather than argued about, and the build answered: `./scripts/build-all.sh dev
--only camera` stopped at `camera-rpc-server.hxx:3:10: fatal error:
grpcpp/grpcpp.h: No such file or directory`. The edge is what hands a module
the `grpcpp/` runtime when it has no domain stub to inherit it from — for
camera's and productivity's `app/rpc` it is the only such edge, while a module
that links a client already has one, the way notification's does. All five
files were restored byte for byte (`git diff` on them is empty) and the rule
now describes the dual role: the stub two units implement, and the runtime the
five name.

### The wire-document gap

The same round found the subject document short: no row for
`argus.auth.v1.user-action` and no stream roster entry for
`ARGUS_AUTH_CHANGE`. Both are live — argus-auth publishes on the auth-owned
stream and argus-sync's durable `argus-sync-auth-action` consumes them into
`user_action_log`, keyed by `auth-action:` plus 32 hex the producer mints — so
the row and the roster entry were added, and the constant census afterwards is
the figure above: 21 constants, 20 rows, the filter as the one with none.

### What the instruments cannot see

One methodological limit was measured twice and belongs in the record for the
next step that reuses these instruments. Class 4's citation check accepts a
cited range on its first line — `schema.sql:37-38` passed while `:38` was
blank (now `:36-37`) — and no instrument here matches a count word against the
artifact it names, which is why "25 assertions" against 24, "11 indexes"
against 13 statements and "four enums" against five survived the first round
and were caught by the second. A citation check bounds where a sentence
points; the number inside the sentence is a separate reading.

### The reflow

Every line this step added was measured against the 78-column rule. The second
round's edits added 14 lines over it: twelve were rewrapped by hand and two
are markdown table rows (the wire-doc subject row and the plan row), which no
wrap can shorten. A cascade script that rewrapped whole paragraphs damaged
three regions — a bullet merged into the following one in `lib/phrase`, a
107-column line in `contracts/CONTEXT.md` and a 79-column line in
`contracts/sync` — all three repaired by hand. Four more lines crossed the
limit in the round's last edits (the `jwt_ctx` sentence, the `lib/http`
rewrite, the identity config roster and the `contracts/camera` tail), each
inside a paragraph whose earlier lines already sat at the limit; all four were
rewrapped. The close-out measurement is zero added non-table lines over 78
columns.

## Verification

The step was declared a documents-only unit, and that is how it was planned:
until the first review's last finding, `git diff --name-only | grep -v
'\.md$'` was empty. Three non-document changes closed it — the two
catalog-test rows and one `SOURCES` line in
`packages/contracts/sync/CMakeLists.txt` — so the affected projects were built
and tested, twice: once for the test rows and again after the `SOURCES` line
landed.

- `./scripts/build-all.sh dev --only auth` — **31/31 tests passed**,
  including `auth-contract-catalog-test` and `sync-contract-catalog-test` (the
  auth project adds both contract packages, so one build covers both suites).
  First run failed on the new `RemoteNotAllowed` row with `REMOTE_NOT_ALLOWED
  == FORBIDDEN` — the row had been written with `ErrorCode::Forbidden` where
  the definition carries `ErrorCode::RemoteNotAllowed`; corrected and re-run
  green. The suite caught its own fix, which is the point of the pin. Re-run
  after the `SOURCES` line: **31/31, exit 0**.
- `./scripts/build-all.sh dev --only sync` — **49/49 tests passed**, and
  re-run after the `SOURCES` line at **49/49, exit 0**.
- `./scripts/build-all.sh dev --only camera` — the experiment's build failed
  as quoted above; with the five `DEPENDS` lists restored, the project builds
  and its suite is **53/53**.
- `scripts/check-comments.sh` — 1373 files, 0 comments. Re-run after the last
  edit: 1373 files, 0 comments.
- `scripts/check-deps.sh` — 123 declarations, 859 edges, 0 forbidden, 0
  cycles, 0 unresolved, and the same figures after the `SOURCES` line, which
  adds a header to a list and no edge.
- the citation audit, re-run with two-base resolution (repository root, then
  the citing file's directory) over the 70 unit docs: **403 citations
  resolved, 0 pointing past the end of their file, 0 at a blank line**. The
  161 tokens that resolve at neither base are bare basenames inside fenced
  layout trees (`main.cc`, `voice-client.hxx`), which is the class the fence
  pass covers by construction.
- `scripts/check-tidy.sh` over the whole tree, because the two test files are
  first-party translation units and the ratchet is measured, not reviewed:
  **exit 0** — 547 TUs, 2,873 findings over 45 checks against a baseline of
  2,901, 11 checks below it and none risen. The `SOURCES` line and the prose
  came after that run; neither compiles a translation unit.
- the reflow pass over the lines this step added: no added markdown line
  exceeds 78 columns. Nine did after the first pass, fourteen after the second
  round's edits (twelve rewrapped, two table rows), and four more in the
  round's last edits; all were rewrapped, and the close-out measurement is
  zero.
