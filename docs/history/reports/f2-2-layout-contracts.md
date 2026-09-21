# Phase 2 step 2b — §2.3's layout applied to the ten contract packages

Scope: the row is "Apply the package layout of §2.3 to every package"
(`docs/history/plans/architecture-plan.md:817`). That row covers thirty-six packages, so it runs as
four sub-steps: this report closes the second, **2b, the ten `packages/contracts/` packages**. 2c
(ten clients) and 2d (eleven services) are not in this pass and the row stays open until they land.
Base `062b020`.

## What the layout change is

§2.3 gives every package the same three-part interior: `src/<name>/` is the public surface and the
include root, `src/<name>/details/` is private by convention, `tests/unit/` holds the suite, and
`AGENTS.md` says what the package is for. Step 1 gave the ten contracts their folder
(`packages/contracts/<domain>/`, rule 25's D2 name); 2a did the same for the fifteen libs. What was
still wrong here was the **interior**, in two flavours.

The first is the pre-split contract flavour: the headers sat at the package root, so the include root
was the package itself and a consumer wrote `<user-role.hxx>` — a bare basename with no domain in it,
which is why the sweep below is the largest of the four sub-steps. The second is the pre-split shared
flavour, which `sync` alone kept: `src/shared/contracts/`, the eight-folder spelling 2a removed from
the libs. `sync` carried both at once — seven files under `src/shared/contracts/` and
`sync-errors.hxx` at the package root.

Under §2.3 none of that survives: a file belongs to the contract, so it sits in `src/<domain>/`, and
the include root is `src/`, so the consumer writes `<domain>/<header>.hxx>`. The move is 27 files:

| package | from | to | files |
|---|---|---|---|
| `auth` | `<pkg>/` | `src/auth/` | 3 |
| `camera` | `<pkg>/` | `src/camera/` | 5 |
| `gateway` | `<pkg>/` | `src/gateway/` | 1 |
| `identity` | `<pkg>/` | `src/identity/` | 1 |
| `notification` | `<pkg>/` | `src/notification/` | 1 |
| `productivity` | `<pkg>/` | `src/productivity/` | 4 |
| `response` | `<pkg>/` | `src/response/` | 2 |
| `sync` | `<pkg>/src/shared/contracts/` | `src/sync/` | 7 |
| `sync` | `<pkg>/` (`sync-errors.hxx`) | `src/sync/` | 1 |
| `tts` | `<pkg>/` | `src/tts/` | 1 |
| `voice` | `<pkg>/` | `src/voice/` | 1 |

Twenty of the 27 came from a package root — nineteen in the nine other contracts and `sync`'s own
`sync-errors.hxx` — and seven from `sync`'s shared spelling. Not one file went into a `details/`
folder: a contract's headers *are* its public surface, and the ten packages have no private half at
all — no `.hxx` here is included only by its own package. That is the one structural difference from
2a, where twelve files had a private home to move to.

## The sweep the move forces

The include root changed, so every consumer's include line changed with it, and the sweep is the
largest of the four sub-steps: **125 files outside `packages/contracts/`** had a contract include
rewritten, plus the **five vocabulary suites inside** it that included by bare basename — **191
include lines in total, 179 of them in the files outside `packages/contracts/`** and twelve inside
the group. The 179 by domain:

```
auth        user-role 25, request-context 21, auth-errors 4
camera      camera-errors 4, event-severity 3, zone-type 2,
            camera-record-mode 2, camera-driver 2
gateway     gateway-errors 7
identity    identity-errors 9
notification notification-delivery-status 2
productivity membership-error 6, productivity-errors 5, share-access 2,
            reminder-detail-status 2
response    response-rpc 3
sync        table-name 23, syncable 17, sync-operation 12, sync-filter 7,
            audit-log-priority 6, sync-limits 3, user-action 2, sync-errors 2
tts         tts-errors 6
voice       voice-lang 2
```

Two checks made the sweep safe to trust. First, every one of the 125 files was verified to have
**include-line changes only**: the diff of each file, with `#include` lines removed, is empty. The
only files in the whole 2b working tree with a non-include change are the four docs edited by hand
(below) and the ten CMakeLists. Second, the old spelling was grepped for after the sweep and occurs
nowhere in the live tree — the only remaining `src/shared/contracts` hits are audit, `clients/llm`,
socket and sync's **own** folders, which are named that way legitimately and are not contracts.

`sync` is the one package where the include spelling did not change for its consumers:
`<sync/table-name.hxx>` was already the spelling under `src/shared/contracts/`, because that folder
sat under an include root of `src/`. Its seven files moved without a single consumer edit; only the
declaration in its own CMakeLists moved.

## The nine suites the ten contracts were missing

Five contracts had a suite — auth, camera, productivity, sync and voice, the round-trip vocabulary
tests their enums had always had, which this step updated to the grouped include. The other five had
none at all, and **not one of the seven `*Errors` catalogs the contracts declare was pinned by
anything**: 74 entries (4 + 8 + 9 + 30 + 8 + 6 + 9) whose only assertion was the service that
happened to throw them, in a suite that would keep passing after an entry's status or message
changed. Six more catalogs live outside `packages/contracts/` — `HttpErrors` in `lib/http`,
`StorageErrors` in `lib/storage`, and `GuardErrors`, `LlmErrors`, `SttErrors`, `VlmErrors` in four
services — and they are other sub-steps' units; this step pinned the seven it moved.

Nine suites close it, 31 cases:

| suite | pins |
|---|---|
| `auth-contract-catalog-test` | the 4 `AuthErrors` refusals |
| `camera-contract-catalog-test` | the 8 `CameraErrors` refusals |
| `gateway-contract-catalog-test` | the 9 `GatewayErrors` refusals |
| `identity-contract-catalog-test` | the 30 `IdentityErrors` refusals |
| `productivity-contract-catalog-test` | the 8 `ProductivityErrors` refusals |
| `sync-contract-catalog-test` | the 6 `SyncErrors` refusals |
| `tts-contract-catalog-test` | the 9 `TtsErrors` refusals |
| `notification-contract-vocabulary-test` | the two delivery spellings and the fallback |
| `response-contract-rpc-test` | the envelope's two directions and its limits |

The seven catalog suites share one shape, generated rather than retyped: a `kCatalog` table of
`{name, definition, code, status, message}` rows matching the header entry for entry, then three
cases — the table matches the header, every entry is legal on the wire, and no two entries say the
same thing. The `kCatalog.size() == N` check is the tripwire: adding a refusal to a header without
pinning it here fails the suite. The legality case mirrors `response-rpc.cc`'s `validRecord` clause
for clause — the status range, and for a record: non-empty, ≤1024 bytes, no NUL in the message;
non-empty, ≤128 bytes, printable `0x21`–`0x7e` in the code — so the pinned limits are the
serializer's own and not a second opinion. The review pass below found that mirror to be inexact in
one clause and fixed it.

A generator wrote them and a second, independently written parser re-derived the same ordered
`(name, code, status, message)` tuples from each header and each `.cc` and diffed them; all seven
agree. The catalogue sizes are the header's own — 4, 8, 9, 30, 8, 6, 9 — and `identity`'s 30 is the
one the tree cannot check by eye.

The identity suite carries a fourth case the others have no reason to: the **two entries whose status
contradicts their code**. `LoginChallengeGenerationFailed` and `DeviceCredentialIssuanceFailed`
answer 500 while their code says `SERVICE_UNAVAILABLE`; every other `SERVICE_UNAVAILABLE` in the
tree answers 503. Both call sites pass 500 explicitly
(`packages/identity/src/feature/api/auth/services/auth-service.cc:330` and `:574`), so the pair is
harmless in effect and is **flagged, not fixed** — the suite names the two so a third cannot arrive
unnoticed.

## The ten `AGENTS.md` files

None of the ten existed. Each was written from the tree, not from a summary, and each says what the
contract is for, its layout with a measured include count per header, the rules a reviewer checks,
and its suites. The measured consumer graph is the part worth recording, because it is what the
counts in the plan's §2.3 rows will be read against:

| package | CMakeLists that link it (outside itself) |
|---|---|
| `sync` | 13 — the largest fan-in of the ten: `audit`, `clients/llm`, `identity`, `lib/auth`, `memory`, `room`, `socket`, `sync`, and the camera, gateway, llm, notification and productivity services |
| `auth` | 13 — eight packages, five services |
| `camera` | 2 — `sync`, `audit` |
| `productivity` | 2 — `sync`, `services/productivity` |
| `voice` | 2 — `identity`, `services/voice` |
| `gateway` | 1 — `services/gateway` |
| `identity` | 1 — `packages/identity` |
| `notification` | 1 — `packages/sync` |
| `tts` | vocabulary 2 (`clients/tts`, `services/tts`), plus `tts-wire` for the two gRPC ends |
| `response` | no vocabulary target at all; the wire is linked by `clients/tts` and `services/tts` |

Three of these correct a claim the plan makes. `gateway` and `identity` are each consumed by exactly
one unit, so neither is a shared contract in the sense the folder suggests — they are a service's own
refusal list that happens to live in the contract group. `response` declares no vocabulary target:
its whole surface is one `.proto` and two functions, and `argus_contracts` would have produced an
empty INTERFACE target.

## Docs

Four live files carried stale spellings and are fixed — six of them in the root `AGENTS.md`: the
contract row's four pre-layout paths (`packages/contracts/auth/user-role.hxx`, `.../camera/`,
`.../productivity/`, `packages/contracts/sync/src/shared/contracts/user-action.hxx`),
`auth/request-context.hxx` at :157 and `sync-operation.hxx` at :514. The Tier 2 table now names
`packages/contracts/<domain>/src/<domain>/`. `docs/architecture/wire-sync-tables.md` cited
`sync-operation.hxx`, `table-name.hxx` and `sync-limits.hxx` by their pre-split paths — two of them
with a `backend/src/` prefix, the monolith-era spelling the docs sweep had already retired
elsewhere. `packages/contracts/{AGENTS,CONTEXT}.md` both claimed the golden /sync fixtures live under
`sync/`; they live with the engine that replays them, `packages/sync/tests/fixtures/sync/`, and both
files now say so and describe the new interior.

`docs/history/` was left alone: those files are historical records of what a step found, and a report
that still spells `src/shared/contracts/` was right when it was written.

## The review pass over the ten packages

The unit closed with a review, and it found seven things worth recording — three in the new files,
four pre-existing.

- **The legality case asserted a rule the wire does not have.** It required a message to be printable
  ASCII, and `validRecord` requires no such thing: non-empty, ≤1024 bytes and no NUL, which leaves
  UTF-8 text legal — deliberately, since a refusal is text a human reads. A pinned suite that
  forbids what the serializer accepts is a false constraint: the day someone writes a Spanish
  refusal with an accent, the suite fails and reads as a contract violation when nothing is wrong.
  The clause was **removed** in all seven suites and the mirror is now exact, with the reason in the
  case's own comment. This is the review's most valuable find, and it came from reading the clause
  against `response-rpc.cc` instead of trusting the generator's comment.
- **Seven of the ten `AGENTS.md` files carried a stale include count, and one carried a false
  superlative.** The counts were measured while the files were being written and *before* the nine
  suites existed, so each of the seven that a new suite includes was one short: `auth-errors` 4 → 5,
  `camera-errors` 4 → 5, `gateway-errors` 7 → 8, `identity-errors` 9 → 10,
  `notification-delivery-status` 2 → 3, `productivity-errors` 5 → 6, `response-rpc` 3 → 4. `sync`'s
  `table-name.hxx` was called "the most-included header of the ten" and is not: `<auth/user-role.hxx>`
  has **26** includers to its 24. All **26** counts in the ten files were then re-derived by a parser
  that reads each claim out of the prose and re-measures the tree, and all 26 now agree. This is 2a's
  lesson repeating itself in a new place — a number from a measurement taken at the wrong time is
  still a number from memory — and the parser's *first* run reported 23 mismatches that were its own
  bug (it looked for `<domain/header.hxx>` only, and several files write the path instead), which is
  the same trap as the catalog verifier's regex below.
- **A count in one comment was written from memory.** identity's convention case said "the convention
  in all four catalogs" — there are seven, and five carry a `SERVICE_UNAVAILABLE` entry. Measured and
  rewritten: five carry one, four answer 503, and these two answer 500. Smaller than the two above
  and the same defect class as both: a number from a folder listing rather than from a measurement.
- **The catalog suites were nearly asymmetric.** The plan's five missing suites left auth, camera,
  productivity and sync with unpinned catalogs, which a reviewer would flag as an inconsistency
  rather than a gap. The generator was extended to all seven, so all ten contracts get the same
  treatment; this is a deliberate scope change and the extra work is four suites.
- **Two files were missing from their own SOURCES lists**, found by diffing `src/<domain>/*` against
  each CMakeLists: `sync-errors.hxx` (absent before the move too) and `response-rpc.hxx`. Both are
  inert — `argus_contracts` passes SOURCES to `target_sources(INTERFACE ...)` and never checks that
  a listed file exists — so this is consistency, not a fix, and it is disclosed as such.
- **`sync` declares a dependency no header of its uses.** `argus::contracts::sync` carries
  `argus::lib::text`; no source in the package includes `<text/...>`. It is not inert like a SOURCES
  entry: an INTERFACE target's DEPENDS become transitive link edges, so a consumer may be reaching
  `text` through this declaration, and dropping it is a link change this step cannot verify from the
  declaration alone. Flagged, not changed, and `sync`'s `AGENTS.md` says so rather than repeating the
  claim that the package depends on `text`.
- **Three `toString` implementations reach their fallback through `default:`** rather than naming the
  enumerator: `user-action.hxx` (`Create`), and, in packages this step did not move,
  `camera/zone-type.hxx` (`Monitor`) and `productivity/reminder-detail-status.hxx` (`Pending`). The
  errors vocabulary's exhaustive-switch rule is the stricter convention. Flagged, not fixed: the
  behaviour is right and changing a fallback is a contract decision, not a layout one.

`notification`'s header comment overclaims and is worth a line of its own: it says the column's
`CHECK` constraint "mirrors" the enum, while the constraint pins the two *spellings*, not the
ordinals, which are never stored. The suite asserts the real relationship — both spellings
round-trip, and anything the constraint would reject reads back as `Pending`.

On formatting, one disclosure a reviewer of a mechanical sweep should have: the local
`clang-format` is **22.1.8**, and the tree is not clean under it — measured whole rather than sampled,
**843 of the 1530 first-party `.cc`/`.hxx` files** would be reformatted (55%). The formatter is
therefore not the repo's authority on its own,
and this step used it the narrow way: **the files it created were run through it** (the nine new
suites are v22-clean and none has a line over 80 columns) and **no pre-existing file was
reformatted**, even the sixteen in `packages/contracts/` that are not v22-clean. Two pre-existing
lines in the contract suites are over the 80-column limit for the same reason —
`productivity-contract-vocabulary-test.cc:39` (83) and `sync-contract-vocabulary-test.cc:50` (84) —
and are left as they were rather than half-reformatted by hand.

The review was also asked what the move broke, and the answer is one thing: **`argus_response_rpc_contract()`
now runs from its own package.** Before 2b the wire module was created by the first consumer,
`clients/tts`, which calls it after establishing the substrate. `packages/contracts/response` now
calls it in its own CMakeLists — the package that owns the two functions also builds them — and the
call chain was checked for the ordering this could break (`clients/tts` adds the substrate, then the
tts contract, then calls `argus_tts_rpc_contract()`, which calls `argus_response_rpc_contract()`
first). The guards make it idempotent either way, and the module now exists earlier and from its
owner.

## Deviations, and what this step got wrong first

Two of the step's own failures are recorded rather than quietly repaired:

- **The first spot run failed with a real compile error.** `identity-contract-catalog-test.cc`
  compared `ErrorCode` values with `CHECK(a == b)`, and doctest's `stringifyBinaryExpr` resolves
  `toString` by ADL: the errors vocabulary's global-namespace `toString(ErrorCode)` returns
  `std::string_view`, so the stringification does not compile (`no match for 'operator+'`). The
  generator now compares **wire strings** — `std::string(definition->wireCode()) ==
  std::string(toString(code))` — which is what the suite meant to assert anyway, and identity's
  convention case filters on the wire code instead of the enum. `grpc::StatusCode` comparisons were
  checked and are safe: namespace `grpc` declares no `toString`.
- **The verifier had a bug of its own, and it nearly read as a content error.** After clang-format
  wrapped the long table rows, the first verifier's row regex required `", &` on one line and single
  spaces, and reported 23 of 30 identity rows as mismatched. The tuples were fine; the regex was
  wrong. It was rewritten with `\s*` between every field and re-verified 30/30, and the pre-format
  and post-format tables both diff clean. A verifier's own bug must not be mistaken for a content
  error, which is the reason the verification was done twice with two independent parses.

Deliberate deviations, disclosed rather than fixed:

- **`argus_contracts_substrate()` moved into `contracts/response`'s CMakeLists** along with the
  `find_package(gRPC)` guard and the `CMAKE_FIND_PACKAGE_TARGETS_GLOBAL` set, because the package
  that owns the wire now declares it. Same reasoning as the call-chain change above.
- **§2.3's proto root is not what this step implements, and the question is still open.** §2.3 gives
  each contract its own `proto/` root; the tree keeps **one shared root**, `packages/contracts/proto/`,
  with the two exceptions that §2.3's own text implies — `response.proto` and `tts.proto` sit beside
  the packages that own them, because they are the only protos a helper compiles from inside a
  contract. The measured facts, for whoever decides: 19 protos live under the shared root, and
  **seven of them name a domain no contract folder owns** — `argus/ai/v1/{llm,stt,tts,vlm}.proto`,
  `argus/common/v1/base.proto`, `argus/memory/v1/memory.proto` and the vendored
  `grpc/health/v1/health.proto`; **twelve of the 19 are schema with no generator at all** (those
  seven, minus health, plus camera's `camera.proto`/`stream.proto`/`zone.proto`, productivity's
  `calendar.proto`/`project.proto` and `argus/sync/v1/contracts.proto`), the seven protos that do
  have one being built by the six client packages and the vendored health; and
  `argus_client_module` accepts exactly **one** `--proto_path`, which the nine cross-domain
  `import "argus/common/v1/base.proto"` users depend on. A per-contract split is a proto-tree
  restructure, not a folder move, and it is not something a layout sub-step should decide by
  itself. The report and the plan row both leave it flagged.
- **`packages/identity/src/shared/vocabulary/` still holds its own enums.** The root `AGENTS.md` says
  the enums that mirror a `CHECK` constraint are not all in `contracts/`; that is still true, and
  moving them is `identity`'s own step, not this one.

## Verification

The gate is `./scripts/build-all.sh dev` over all eighteen projects, and it was run **twice** on
the tree this commit contains. The first run stopped at project 16 of 18 with one test failing, and
the whole gate was re-run rather than reported from a run that has a failure in it; the ledger below
is the second run's. That run is clean on **18/18**: no `warning:` line, no `Not Run`, no suite under
100% and the run's last line is `All selected projects built and tested (profile: dev).` The four
single-project spot runs (`--only gateway | identity | sync | tts`, the projects the new suites are
reached from) were clean before it. One difference between the spot run and the gate is recorded
rather than smoothed over: the spot run's `gateway` section listed 34 tests where the gate lists 36,
missing `sync-contract-catalog-test` and `productivity-contract-catalog-test`. Nothing went unrun —
the same
spot run's `sync` section lists both and both passed — the difference is which tests that build
directory's `CTestTestfile.cmake` had collected when it was generated, and the gate configures all
eighteen projects from scratch, so its counts are the ones that stand.

| project | tests | project | tests | project | tests |
|---|---|---|---|---|---|
| `cert` | 21 | `intent` | 4 | `tts` | 16 |
| `socket` | 12 | `gateway` | 36 | `stt` | 5 |
| `sqlite` | 2 | `camera` | 45 | `vlm` | 7 |
| `identity` | 21 | `productivity` | 32 | `llm` | 29 |
| `sync` | 28 | `notification` | 37 | `voice` | 20 |
| `memory` | 20 | `guard` | 42 | `tunnel` | 12 |
| **ledger** | **389** | | | | |

The reached-test ledger after 2a was **333** and is **389** here: **+56**, which is exactly
the number of (project, new suite) instances the nine suites register. A suite counts once per
project whose ctest collects it, because the ledger counts tests as ctest reached them and not as
files; the nine suites' own 31 cases are the source, and the increment is the sum the eighteen build
directories collected.

**The first run's one failure is a flake in a test this step does not own, and it is recorded rather
than retried until it went away.** `packages/lib/sqlite/tests/unit/identity-client-test.cc:9` — the
case "installed identity client serves its own database", 20 of llm's 29 ctest entries — aborted
0.25 s in with an uncaught `std::system_error: Resource deadlock avoided` (EDEADLK) thrown from a
non-main thread, which doctest reports as `SIGABRT` and ctest as `Subprocess aborted`, with zero
assertions reached. Four measurements say it is not this step's: the test's source is not in 2b's
diff at all (2a rewrote one line of it, its include, and nothing else), the same test **passed in the
other fifteen projects' ctest runs inside that same gate run**, the re-run reached it again in `llm`
and passed it, and 100 consecutive re-runs of the binary afterwards — 40 idle and 60 with fourteen
busy loops loading the machine — all passed. The
mechanism is drogon's rather than the test's: `Sqlite3Connection` guards a statement with
`std::shared_lock`/`std::unique_lock` over a `std::shared_mutex`
(`orm_lib/src/DbConnection.h:34`), the statement runs on the connection's own loop thread
(`Sqlite3Connection.cc:149`, which queues the work with `queueInLoop`), and glibc's
`pthread_rwlock_wrlock` returns EDEADLK when the calling thread already holds a read lock — which is
what a synchronous statement issued from inside that thread does. The case creates, destroys and
swaps whole clients, and that is where the overlap comes from. It belongs to `lib/sqlite`'s own step,
not this one; it is flagged here because it can fail the phase gate. Under-load flakes are
recorded in this tree before: the plan's step 1 row names `cert-san-test` and `memory-backpressure-test` as
flaking once under four parallel builds and passing isolated, which is the same shape as this one.

The step's three central numbers were re-measured after the review's edits, and where the
re-measurement disagreed with prose written earlier it is the prose that changed: the include-line
total was written as 177 and is **191** (179 in the 125 files outside `packages/contracts/`, 12 in
the five vocabulary suites inside), the root-versus-shared split of the 27 renames was written as
nineteen/eight and is **twenty** from a package root (nineteen in the nine other contracts plus
`sync`'s `sync-errors.hxx`) and **seven** from `sync`'s shared spelling, and the non-include change
list was written as "the two docs edited by hand" when it is **four docs and ten CMakeLists** — the
14 files `git diff -M` reports with any non-`#include` line, against 130 files that are
include-only. The formatter figure was re-measured whole rather than sampled for the same reason.

