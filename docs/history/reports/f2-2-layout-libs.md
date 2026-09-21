# Phase 2 step 2a — §2.3's layout applied to the fifteen lib packages

Scope: the row is "Apply the package layout of §2.3 to every package"
(`docs/history/plans/architecture-plan.md:817`). That row covers thirty-six packages, so it runs as
four sub-steps and this report closes the first: **2a, the fifteen `packages/lib/` packages**. 2b
(ten contracts), 2c (ten clients) and 2d (eleven services) are not in this pass and the row stays
open until they land. Base `45b6bc5`.

## What the layout change is

§2.3 gives every package the same three-part interior: `src/<name>/` is the public surface and the
include root, `src/<name>/details/` is private by convention, `tests/unit/` holds the suite, and
`AGENTS.md` says what the package is for. Step 1 already gave the folders their group
(`packages/lib/<name>/`, rule 25's D2 names); what was still wrong inside them was the **interior**:
a lib kept the pre-split `src/shared/<kind>/<unit>/file.{hxx,cc}` spelling, where `<kind>` was one of
`wrapper`, `services`, `utils`, `vocabulary`, `validation`, `repositories`, `contracts` and `access`.
Under §2.3 none of those eight folders exists: a file belongs to the package, so it sits directly in
`src/<name>/`, and the ones that are private move into `details/`.

`auth` carried a second flavour of the same rot — `src/filter/{jwt,role,device,valid-json}/` — and
`http` a third, a lone `src/http/http-errors.hxx` that no consumer outside the package includes.

**79 files moved**, grouped by the source path they came from — seven groups, and the last row is a
subset of the ones above it rather than an eighth:

| from | to | files |
|---|---|---|
| `src/shared/wrapper/<unit>/` | `src/<name>/` | 16 |
| `src/shared/services/<unit>/` | `src/<name>/` | 20 |
| `src/shared/utils/<unit>/` | `src/<name>/` | 14 |
| `src/shared/vocabulary/`, `src/shared/validation/`, `src/shared/repositories/`, `src/shared/contracts/`, `src/shared/access/` | `src/<name>/` | 18 |
| `src/filter/{jwt,role,device,valid-json}/` | `src/auth/` | 8 |
| `src/filter/identity-access.{cc,hxx}` | `src/auth/details/` | 2 |
| `src/http/http-errors.hxx` | `src/http/details/` | 1 |
| files private to their package | `src/<name>/details/` | 12 of the above |

Where a file went into `details/` rather than the package root, the test was whether anything outside
the package includes it: `auth/details/identity-access.{cc,hxx}` (the identity RPC client the two
filters share), `http/details/http-errors.hxx`, `phrase/details/{phrase-kind,vocabulary-en,
vocabulary-es,vocabulary-types}.hxx`, `sqlite/details/{vector-index-query,vector-index-repository}.*`,
`storage/details/s3-signing.hxx` and `validation/details/rules.hxx` are included only by their own
package. The rest are the public surface and stayed at `src/<name>/`.

The result, measured per package at the end of the sweep:

```
audio      audio/{audio-resampler,endpoint-detector}.{cc,hxx}
auth       auth/{device,jwt,role,valid-json}-filter.* auth/jwt-service.* auth/role-access.hxx
           auth/{user-directory,identity-change-sink}.hxx auth/user-directory-identity.*
           auth/details/identity-access.*
cert       cert/cert-service.{cc,hxx}
config     config/config-service.{cc,hxx} config/service.hxx
errors     errors/{error-code,error-definition,validation-exception}.hxx
           errors/response-exception.{cc,hxx}
grpc       grpc/grpc-client-base.* grpc/grpc-cq-bridge{,-entry,-exit}.* grpc/grpc-server-identity.hxx
http       http/{api-response,cors,error-handler,health-controller,listener-config}.{cc,hxx}
           http/details/http-errors.hxx
mdns       mdns/mdns-service.{cc,hxx}
nats       nats/nats-bus.{cc,hxx} nats/{nats-subject,nats-push-intent-sink,push-intent-sink}.hxx
phrase     phrase/{phrase-automaton,phrase-catalog,rule-parser}.{cc,hxx} phrase/vocabulary.hxx
           phrase/{lexicon-kind,memory-type}.hxx phrase/details/{phrase-kind,vocabulary-*}.hxx
runtime    runtime/{ai-init,blocking-task,cancellation-token}.hxx
           runtime/{hardware-profile,thread-budget}.{cc,hxx}
sqlite     sqlite/{db-service,vec-db,schema-runner}.{cc,hxx} sqlite/{sql-escape,sqlite-stmt}.hxx
           sqlite/details/vector-index-*
storage    storage/s3-storage-service.{cc,hxx} storage/{storage-errors,stored-file-category}.hxx
           storage/details/s3-signing.hxx
text       text/{base64,json-diff,text-norm}.{cc,hxx} text/{fnv-hash,json-util,sha256}.hxx
validation validation/validation_dsl.hxx validation/validator.hxx validation/details/rules.hxx
```

`errors` is the one package that needed no move: Phase 1 step 10 built it directly in this shape, so
its interior was already `src/errors/`.

## The sweep the move forces

`git mv` moves files; it does not move the paths that name them. Every include of a moved header had
to be recomputed — **633 include lines rewritten**, across **370 tracked files** (341 modified and 29
of the renames), out of **447 tracked paths touched in all** (368 modified plus the 79 moves). The
rename-detected diff adds 650 include lines, and the 17-line difference is exactly the includes the
three new suites bring with them, so no include was added to an existing file. Measured with rename
detection (`git diff HEAD -M`), the only non-include change
in any `.cc` or `.hxx` in the whole sweep is the three-line comment correction in
`packages/lib/config/src/config/config-service.hxx` (below); without rename detection a moved file
reads as a whole-file rewrite, which is how a sweep hides edits among its moves. The rewrite is
mechanical and total: no `packages/lib` header answers to a `shared/` include any more, and no file in
the tree reaches a header by a cross-directory relative path — `git grep '#include "\.\./'` returns
nothing, so the only relative includes left are the same-directory siblings the house style allows.
The largest groups are the ones the plan predicted (`<shared/services/config-service/config-service.hxx>`
→ `<config/config-service.hxx>`, `<shared/wrapper/nats/nats-bus.hxx>` → `<nats/nats-bus.hxx>`,
`<shared/utils/json-diff/json-diff.hxx>` → `<text/json-diff.hxx>`), and they land in every group of
the tree — `services/camera` alone carries 47 rewritten files, `packages/identity` 40,
`services/guard` 32.

What the sweep deliberately did **not** take: 161 distinct `<shared/...>` spellings still appear in
248 live files, and every one of them names a header owned by a contract, a client, a service or one
of the not-yet-moved packages (`packages/memory`, `packages/identity`, …) — none resolves into
`packages/lib` and none resolves to nothing. Those are 2b's, 2c's and 2d's interiors to move, and
rewiring them here would put another step's spelling in this step's commit.

Three files inside `packages/lib` had to change for a reason that is not an include rewrite:

- **`cmake/argus-module.cmake`, `argus_grpc_absl_bridge()`.** The ABI-bridge entry object had its own
  source directory on its private include path, which made `<grpc-cq-bridge.hxx>` resolve there while
  every other file in the package spells it `<grpc/grpc-cq-bridge.hxx>`. The sweep normalized the
  entry's spelling like any other consumer and broke the build; the fix publishes the package's
  `src/` root to that target. The helper was changed, not the file, because the package-qualified
  spelling is the one §2.3 asks for.
- **`packages/lib/grpc/CMakeLists.txt`, `config`, `mdns`** gained the suite block below.
- **`packages/lib/config/src/config/config-service.hxx`** carried a comment asserting the opposite of
  what `getStringPairs` does (`// Order is preserved as written in the file`); toml++ tables are
  key-ordered, so the comment now says so.

## The three suites the packages were missing

Twelve of the fifteen libs already carried a `tests/unit/` suite. Three did not, and each of the three
tested something whose absence a reader would not notice:

- **`config`** — `tests/unit/config-service-test.cc`, 8 cases: dotted-path resolution, the absent-key
  defaults and `hasKey` (including a path that walks through a scalar), the runtime override winning
  over the file, the malformed-file refusal leaving the loaded config alone, the overlay replacing one
  top-level table wholesale, `getStringPairs`'s key order, `drogonConfig()` answering null with no
  `[drogon]` table, and the persisting setters — which the review added after finding the family
  untested: the case asserts the file is rewritten in place, the comment lines and the key's
  neighbours survive, a key the file does not carry joins the section it names, and a flag is written
  as a flag rather than a quoted string. The service is all-static, so every case writes and loads its
  own file; the override case uses keys no other case reads, because an override outlives the case
  that set it, and the persisting case is declared last because it rewrites the file it loaded.
- **`grpc`** — `tests/unit/grpc-server-identity-test.cc`, 2 cases: `constantTimeEquals` (equal,
  empty-vs-empty, and differences in the middle, at the end and in length) and
  `callerCredentialsFromPairs` dropping half-filled pairs while preserving order. Both are pure, so
  the suite needs no channel — it links the base target for its include root only.
- **`mdns`** — `tests/unit/mdns-service-test.cc`, 4 cases: the disabled path a boot must survive
  (initialize true, not advertising, `health()["advertising"]` false, shutdown idempotent), the
  `mdns.*` keys as the health report shows them, an out-of-range port keeping the default 7024, and an
  empty-string override keeping the `Argus` / `_argus._tcp` defaults. No case opens a socket.

One of the three needed more than the house block. `packages/lib/grpc` is a folder consumers reach
with `add_subdirectory(... EXCLUDE_FROM_ALL)`, so `grpc-server-identity-test` was declared,
configured, and never built, which ctest then reported as `Not Run`. The suite now carries
`set_target_properties(... EXCLUDE_FROM_ALL FALSE)`, the same opt-back-in
`packages/clients/camera/CMakeLists.txt` already used for `client-caller-identity-test`.

The mechanism is worth stating because it is not obvious from either call site: `EXCLUDE_FROM_ALL`
propagates into a directory's *nested* `add_subdirectory` calls. Measured over every `CMakeLists.txt`
outside `build/`, 61 calls carry the flag and 19 of them name a client folder as their source —
seventeen in the seven services that add a client folder *with* it (the gateway's five among them) and
two in `packages/identity` and `packages/lib/auth`. The grpc folder itself is reached without the flag
by the six client folders that carry a PROTO runtime and by two contract packages, so the package is
almost always reached through an excluded parent; only two calls name `packages/lib/grpc` with the
flag spelled at the call site (`services/camera`, `services/voice`), and neither is the route the
gateway takes. `config` and `mdns` are reached by ordinary `add_subdirectory` calls, so they need no
opt-back-in.

## Docs

Nine packages carried no `AGENTS.md` — `cert`, `config`, `grpc`, `nats`, `runtime`, `sqlite`,
`storage`, `text`, `validation` — and now do, in the house shape (`# argus-<name>`, what it is, the
layout, the rules, the tests). Each claim in them was re-measured against the tree rather than
carried over from a summary, which corrected five: `runtime` reaches 9 units outside `packages/lib`,
not 10; `sqlite`'s `readOnlyClient` has **no** fallback while the four named clients do; `storage` has
no presigned-URL surface; `text` is not header-only (it has three `.cc`); and `grpc` declares no
`argus_lib` of its own at all. Four existing files were updated to the new interior — `audio`,
`auth`, `http`, `phrase` — and a fifth, `errors`, took the consumer-count correction the review found
(below); the root `AGENTS.md`'s Key Files Reference now says the `lib/` rows
carry §2.3's interior while the contract, client and service rows still show the pre-layout spelling
that 2b–2d replace.

Two docs named a path this sweep had to touch and were left correct rather than half-correct:
`docs/operations/hardware-tiers.md:15` (ThreadBudget's new path) and
`docs/architecture/wire-device-identity.md:5`, which f2-1 had already recorded as stale
(`backend/src/filter/device/device-filter.cc` — a prefix no tree ever had). It now reads
`packages/lib/auth/src/auth/device-filter.cc`, and its claim "gateway and legacy share the gate" was
corrected to what the tree shows: seven units outside the package use `DeviceFilter` — `identity`,
`sync`, and the `camera`, `gateway`, `guard`, `notification` and `productivity` services.

## The review pass over the fifteen files

The unit closed with a review, and its most productive half was arithmetic: every consumer count in
the fifteen `AGENTS.md` files was re-derived from a `git grep` over the CMake files rather than read
off the prose that was already there. Five counts were wrong; all five are fixed, and the pattern is
worth recording because it is the same mistake five times — **a number written from memory of a
folder listing, when the target graph is what answers it**.

| file | said | measured |
|---|---|---|
| `config` | fifteen units, twelve services | **sixteen**: `identity`, `memory`, the `llm`/`stt`/`tts` clients, all eleven services |
| `text` | sixteen units, eight services | **fourteen**: the same eight packages, six services |
| `validation` | identity, memory, sync and six services | **seven services** (the total of ten was right) |
| `sqlite` | eleven units | **ten**: `audit`, `identity`, `memory`, `sync` and six services |
| `errors` (pre-existing) | contracts, `storage`, `validation` and four services | the **eight** contracts, `audit`, `memory`, `sync`, the `tts` client and five other libs |

`errors`' sentence was already wrong at HEAD — no service links `argus::lib::errors` directly at all,
they reach it through `argus::lib::http` — and is corrected here because a path review of the libs is
where a reader would look for it. Five further claims were corrected *while* the files were being
written, the same defect caught earlier: `runtime` reaches 9 units not 10, `text` is not header-only,
`grpc` declares no `argus_lib`, `storage` has no presigned-URL surface, and `sqlite`'s
`readOnlyClient` has no fallback.

The review's other half was coverage, and it found one gap worth closing: `config`'s suite exercised
every read path and none of the **persisting** setters, the family a provisioning tool uses to change
a value on disk (`setBool`/`setString`/`setInt`/`setDouble` → `applyValue` → `patchContent`). The
eighth case pins what that machinery promises: the file is rewritten in place, the comments and the
key's neighbours survive, a key the file does not carry joins the section it names, and a flag is
written as a flag rather than a quoted string. It passes, which is the first evidence the family has
ever had.

Because that case was added after the gate's first run had begun, the gate was stopped and re-run
whole rather than reported from a stale run: **the run in the ledger below is the one that ran on the
tree this commit contains**. The stopped run's one `[error] project failed: camera` line is the
`SIGTERM`, not a defect — the camera project had no failure of its own before or after.

## Deviations, and two things this step got wrong first

The sweep is mechanical, and mechanical sweeps break things. Two of its failures were caught here and
are recorded rather than quietly repaired:

- **`packages/lib/auth/AGENTS.md` was corrupted by the sweep itself.** Three layout bullets came out
  naming `src/auth/details/{jwt,role,valid-json}/` — folders that do not exist — and two more were
  both labelled `src/auth/`. `git diff` showed the sweep had rewritten `src/filter/jwt/` (the *old*
  path) rather than replacing the bullet. The file was rewritten from the tree.
- **The grpc bridge include break** described above.

Deliberate deviations, all pre-existing and none created by this step:

- **`grpc` declares no `argus_lib(NAME grpc ...)`.** Its two OBJECT targets come from the
  `argus_grpc_client_base()`/`argus_grpc_absl_bridge()` helpers, which declare them so the ABI bridge
  is compiled once, before the first generated client; only `argus::lib::grpc-health` is declared with
  `argus_client_module`. Its `AGENTS.md` says so rather than pretending the folder is an ordinary lib.
- **`text` is not header-only.** §2.3's line "A header-only package (`validation`, `text`)" is stale
  for `text`: `base64.cc`, `json-diff.cc` and `text-norm.cc` are real sources. `validation` is the
  genuine `HEADER_ONLY` package and stays the only one.
- **§2.3's sqlite line names two files that are not in the package.** It expects
  `src/sqlite/{db-service, sqlite-stmt, vec-db, sqlite-graph}.{hxx,cc}` plus
  `src/sqlite/details/pragma-apply.{hxx,cc}`. `sqlite-graph.{cc,hxx}` lives in `packages/memory`
  (whose own step will place it) and no `pragma-apply.*` exists anywhere in the tree — the per-boot
  pragma list is `kPerBootPragmas` in `sqlite/db-service.cc`, applied by `DbService::applyPragmas`.
  The plan line was not corrected here; it is §2.3's, not a path this step moved.
- **`runtime`'s two modules keep ungrouped target names** (`hardware-profile`,
  `hardware-profile-gpu`) because they are declared through `argus_module`, which carries no group
  prefix. Renaming them is a target rename with consumers, not a layout move.
- **Contract headers reached from lib sources were left alone.** `auth` includes
  `<shared/contracts/table-name.hxx>`, `<user-role.hxx>`, `<request-context.hxx>` and
  `<auth-errors.hxx>` — all `packages/contracts/` paths, which still resolve because 2b has not run.
  Rewiring them is 2b's job; doing it here would put one step's spelling in another step's commit.

## Verification

The gate is `./scripts/build-all.sh dev` over all eighteen projects. Result: **18/18 projects built
and tested, 0 failed, no `Not Run`** — the full ledger and the per-project test counts are recorded
below, and the counts are ctest's own, taken from the run rather than from the CMake declarations.

| project | tests | of the three new suites |
|---|---|---|
| cert | 18 | `config-service-test`, `grpc-server-identity-test` |
| socket | 10 | `config-service-test`, `grpc-server-identity-test` |
| sqlite | 2 | `config-service-test` |
| identity | 18 | `config-service-test`, `grpc-server-identity-test` |
| sync | 22 | `config-service-test`, `grpc-server-identity-test` |
| memory | 18 | `config-service-test`, `grpc-server-identity-test` |
| intent | 4 | — |
| gateway | 29 | all three, `mdns-service-test` among them |
| camera | 37 | `config-service-test`, `grpc-server-identity-test` |
| productivity | 26 | `config-service-test`, `grpc-server-identity-test` |
| notification | 31 | `config-service-test`, `grpc-server-identity-test` |
| guard | 39 | `config-service-test`, `grpc-server-identity-test` |
| tts | 12 | `config-service-test`, `grpc-server-identity-test` |
| stt | 5 | `config-service-test` |
| vlm | 7 | `config-service-test` |
| llm | 27 | `config-service-test`, `grpc-server-identity-test` |
| voice | 16 | `config-service-test`, `grpc-server-identity-test` |
| tunnel | 12 | `config-service-test` |
| **reached** | **333** | **31 suite instances**, no `Not Run`, no non-zero `tests failed` |

The arithmetic is the evidence that nothing else moved: **302 + 31 = 333**. 302 is the ledger steps
11, 12 and 13 all report, 31 is every instance of the three new suites the eighteen projects reach —
`config-service-test` in 17 of them (`intent` is the one that links nothing this package serves),
`grpc-server-identity-test` in 13, and `mdns-service-test` once, in the gateway, its only consumer in
the tree. The run also produced **no `warning:` line at all**, and the three suites carry `-Wall
-Wextra` on their own targets, so they compile clean rather than merely compile.

Eleven of the twelve pre-existing lib suites were rewritten by the include sweep and still pass — the
evidence that the sweep's spelling is the one the build resolves; the twelfth, `errors`'
`error-definition-test`, needed no rewrite at all, its headers having been in §2.3's shape since step
10.

Report: `docs/history/reports/f2-2-layout-libs.md`.
