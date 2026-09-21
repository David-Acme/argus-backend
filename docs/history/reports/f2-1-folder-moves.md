# Phase 2 step 1 — the packages move into `lib/`, `contracts/`, `clients/`

Scope: the row is "Move packages into `lib/`, `contracts/`, `clients/` with the naming rule of D2"
(`docs/history/plans/architecture-plan.md:816`). This is stage 4 of that step and the only one that
touches the tree: stages 1–3 landed the helper calls (`argus_lib`, `argus_contracts`,
`argus_clients`) and the D2 names while every folder still sat flat, so the tree has been green
since. What remains is the move itself — 29 folders, 209 files following them through git, and every
path in the repository that named a moved folder recomputed. Base `dc07d5f`.

## What the tree looked like, and what it looks like now

Before the sweep the three groups existed only as names in `cmake/argus-module.cmake`: fifteen
packages sat directly under `packages/` (`packages/errors`, `packages/http`, …), ten contract folders
carried the `-contract` suffix (`packages/contracts/sync-contract`), and four clients carried the
`-client` suffix (`packages/clients/llm-client`). The plan's §2.5 rule — "the folder never repeats
the group" — could not be satisfied while the folders were flat, because there was no group for them
to sit under.

The change is exactly three move kinds:

| from | to | count | rule |
|---|---|---|---|
| `packages/<lib>` | `packages/lib/<lib>` | 15 | §2.2's D1 table |
| `packages/contracts/<d>-contract` | `packages/contracts/<d>` | 10 | D2: the suffix dies with the flat folder |
| `packages/clients/<d>-client` | `packages/clients/<d>` | 4 | D2, same rule |

The four client renames are `llm-client→llm`, `stt-client→stt`, `tts-client→tts`, `vlm-client→vlm`.
The other six client folders (`camera`, `camera-actions`, `identity`, `notification`,
`productivity`, `voice`) already read correctly and are not in the move map at all — an identity
mapping in the map is what crashed the first run of the sweep (see *What went wrong*, below).

After the move the three group folders hold what §2.5 says they hold, and nothing repeats its group:

```
packages/lib/       audio auth cert config errors grpc http mdns nats phrase runtime sqlite storage text validation
packages/contracts/ AGENTS.md CONTEXT.md buf.gen.yaml buf.yaml camera gateway identity manifests
                    notification productivity proto response sync tts voice
packages/clients/   camera camera-actions identity llm notification productivity stt tts vlm voice
```

The helper inventory is unchanged by the move and matches the plan's measured table exactly: 14
`argus_lib` calls over 15 `lib/` folders (`grpc` declares no folder target of its own — it declares
`argus_lib_grpc-health` and the two `argus_grpc_*` helpers instead), 9 `argus_contracts` plus the
hand-written `response` package, 10 `argus_clients` (6 with `PROTO`, 4 without: `llm`, `stt`, `tts`,
`vlm`), 3 grouped `argus_client_module` (`grpc-health` in `lib`, `response-wire` and `tts-wire` in
`contracts`), and 16 `argus_module` calls — of which five declare the ungrouped packages the plan
sends to a later phase (`audit`, `identity`, `intent`, `room`, `socket`) and the rest declare
service-local modules (`camera-rpc`, `voice-core`, `tts-synthesis`, `guard`, …). The count is a
measurement of *calls*, not of packages: **two packages and two modules are not helper-declared at
all** — `packages/sync` hand-writes `add_library(argus_sync STATIC)` with no alias,
`packages/memory` hand-writes `memory-core` and `memory-catalog`, and `packages/lib/runtime` declares
`hardware-profile` and `hardware-profile-gpu` through the ungrouped helper, so their targets carry no
group prefix. All four are named in *Deviations*, below; none is new debt this step created.

## Two rewrites, not one

`git mv` moves the files; it does not move the paths that name them. Two distinct classes of path
had to be recomputed, and conflating them is what damaged the vendored submodules (below).

**1. CMake paths that resolve into a moved folder.** Every
`${CMAKE_CURRENT_SOURCE_DIR}/<rel>`, `${CMAKE_CURRENT_FUNCTION_LIST_DIR}/<rel>` and
`${CMAKE_CURRENT_LIST_DIR}/<rel>` in the tree was resolved to a repo path, mapped through the move,
and recomputed *relative to the file that names it*. The recomputation is not optional even for a
path whose target did not move: a folder that changed depth breaks its own escape paths.
`packages/lib/errors/CMakeLists.txt` needs `../../../cmake/argus-module.cmake` where the flat
`packages/errors/CMakeLists.txt` needed `../../cmake/argus-module.cmake`, and the same file's
`add_subdirectory` into `../../lib/runtime` from `packages/lib/errors` resolves differently than its
`../../runtime` did from `packages/errors`. 70 files were repointed this way.

The three variables are not interchangeable and the sweep had to respect that:
`${CMAKE_CURRENT_SOURCE_DIR}` is the CMakeLists **currently being processed** — not the file that
*defined* a macro that is running; `${CMAKE_CURRENT_LIST_DIR}` is the containing file's directory;
`${CMAKE_CURRENT_FUNCTION_LIST_DIR}` is the file that defined the function. A rewrite that computes a
caller-relative path from a function body's own directory produces a path that is correct at the
definition site and wrong at every call site. Every `${CMAKE_CURRENT_*}` rewrite in our own tree was
audited against that distinction after the fact and all of them are correct; the vendored files that
were damaged were damaged by exactly this confusion (see below).

**2. Textual references to a package path.** A live file that spells `packages/errors` — a
`conanfile.txt` include path, a `CMakePresets.json` `binaryDir`, a Dockerfile `COPY`, a doc's
prose, a comment naming where a module lives — has to spell it as `packages/lib/errors` now. The
token rewriter used lookarounds (`(?<![\w-])` / `(?![\w-])`) so that `backend/packages/errors` is
repointed (a real spelling in the Dockerfiles) while a longer folder name that merely starts the
same way is refused. `docs/history/` and `.git`, `build/` and `.superpowers/` are skipped by
construction: the history records what the tree looked like when it was written, and rewriting it
would falsify the record.

The two rewrites touch different file sets, which is why the sweep measured before it acted: the
only live files naming a moved package at the time were `.md` (17), `.txt` (16), `.sh`, `.proto` and
`.cmake` (1 each). `.cc` and `.hxx` are in the suffix list because their only matches were comments
and path strings — a schema path default in a header, an `(packages/errors)` aside in a `.proto`
comment — and none of those matched a code identifier.

## The third stale class: build-tree labels

The review found 49 sites the sweep could not have caught, because neither rewrite matches them:
the *second argument* of `add_subdirectory` — the binary-directory label — in 20 `CMakeLists.txt`.
Twelve spelled a retired client folder (`${CMAKE_BINARY_DIR}/tts-client` in `services/voice`,
`/llm-client` in `packages/memory`, …) and 37 spelled a retired contract folder
(`${CMAKE_CURRENT_BINARY_DIR}/auth-contract` in `packages/identity`, `/sync-contract` in
`packages/sync`, …). The token rewriter never saw them: it repoints `packages/<old>` spellings, and a
label carries no `packages/` prefix.

They cannot break a build — a label is an arbitrary name for a build tree, and the review confirmed
that no label was claimed by two different source directories, which is the only way one can fail —
but they are exactly what this step exists to remove: a folder name that no longer exists, left in
the file that names it. All 49 were rewritten to the canonical spelling the tree already uses for its
client labels (17 sites, `${CMAKE_BINARY_DIR}/clients/<domain>`) and for its contract labels
(`${CMAKE_BINARY_DIR}/contracts/<domain>`, which is symmetric with the client form and makes a
collision impossible by construction — one label per domain, and the guard stops a second
`add_subdirectory` of the same contract). Verified after the rewrite: 0 stale labels remain, and the
47 explicit labels in the tree resolve to 0 pairs of distinct source directories, so no
`add_subdirectory` can collide. The change is 49 lines, one per site.

The orphaned build directories the old labels left behind (`build/dev/auth-contract`,
`build/dev/llm-client`, …) were removed after the gate ran, as row 8 did with its own — they are
untracked build artifacts, and deleting them does not invalidate the run.

## What went wrong, and what was reverted

Two failures, both caught before anything was staged, and both worth recording because the second is
the one that matters.

**The sweep refused to run on its own move map.** The first invocation died with
`fatal: can not move directory into itself, source=packages/clients/camera,
destination=packages/clients/camera/camera`. The client half of the map was built by iterating
`packages/clients/` and registering every folder into `CLIENT_RENAMES.get(name, name)` — which
registers the six folders that keep their name as identity moves (`camera → camera`). The lib and
contract moves had already run by then (25 folders moved, 187 renames staged, no rewrites yet). The
fix registers only real renames; the rerun completed: **29 folders moved, 70 files repointed**.

**The path rewrite reached into vendored submodules.** `fix_paths` walks the whole tree, and
`third_party/` was not excluded. It rewrote six files across three submodules:
`third_party/ncnn/cmake/ncnn_add_shader.cmake`, `ncnn_add_param.cmake` and `ncnn_add_layer.cmake`,
plus files under `third_party/llama.cpp` and the nested `third_party/ncnn/glslang`. Most were
harmless trailing-slash normalizations, but six lines were not: the
`"${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ncnn_generate_…"` script paths ncnn passes to
`${CMAKE_COMMAND} -P` became `"${CMAKE_CURRENT_SOURCE_DIR}/ncnn_generate_…"`. That spelling is right
only if the variable means "the directory of the file I am rewriting", which it does not: these run
inside `ncnn_add_*` macros invoked from a parent directory, so the rewrite pointed the generated
shader/param/arch scripts at a path that does not exist and would have broken the next configure that
built a layer. All six files were restored
with a scoped `git -C <submodule> restore -- <paths>` and all four submodules verify clean.

The lesson is the one the CMake variable semantics above already state: `${CMAKE_CURRENT_SOURCE_DIR}`
means "the CMakeLists being processed *right now*" — inside a vendored `include()`d helper it is
whatever project included it, so a "relative to the file I am rewriting" assumption is simply not
what the variable says. The sweep now skips `third_party/`, and no upstream file is in this change.

## The docs pass

The move invalidated prose, not just paths, and the sweep's token rewriter cannot judge prose. A
punch list of the classes that needed a human was built and worked through:

- **Class 1 — folder paths that moved.** `docs/architecture/contracts-overview.md` (its title, two
  `sdk/…` paths, a "top-level folder" claim), `system-overview.md`'s Contracts row,
  `packages/contracts/CONTEXT.md` (five sites), `services/camera/CONTEXT.md`,
  `packages/lib/mdns/AGENTS.md`.
- **Class 2 — helper names that no longer exist.** `events-and-contracts.md` still taught
  `argus_sdk_module()`; seven sites in all spelled a retired helper or a retired
  `argus-contracts`/`argus::sdk` token.
- **Class 3 — alias claims.** `packages/identity/CONTEXT.md` said `argus::threading` (the target is
  `argus::lib::runtime`); `packages/identity/CMakeLists.txt`, `packages/sync/CMakeLists.txt`,
  `services/camera/CMakeLists.txt`, `services/gateway/CONTEXT.md`,
  `services/productivity/CONTEXT.md` and `packages/memory/AGENTS.md` carried similar claims.
- **Class 4 — rules that described the old shape.** `AGENTS.md`'s rule 25 example named a real call
  with real paths; its service-local example named `argus::vad` and a
  `services/voice/src/shared/services/vad/CMakeLists.txt` that do not exist (voice declares only
  `argus_module(NAME voice-core)` inline), so it became a `<module>` placeholder; the "Today the other
  half…" paragraph became "The target-name half of that rule is structural… The dependency half is
  still prose — 20 helper calls…"; and the tier heads lost their "(today …)" asides.

`docs/architecture/build-model.md`'s 11-line project grid had to be realigned: the longer paths
broke the column widths, so the table was rebuilt programmatically (columns of 27 and 26).

A **second pass** over the docs — the claim-by-claim review below — found 25 more sites in the same
four classes, all fixed the same way: six `lib/*/AGENTS.md` still teaching `argus_module` for
packages that now call `argus_lib` (the helper really was `argus_module` at HEAD, so the rename is
what made the line wrong), three client `AGENTS.md` doing the same for packages that now call
`argus_clients`, the four `-client` titles in `packages/clients/{llm,stt,tts,vlm}` (the four renamed
clients are the only four clients that carry docs at all), the `README.md` layout block, three
sentences in `services-and-packages.md` (its two `*-contract` mentions and the `argus-vlm-client`
name), the tree block in `contracts-overview.md`, and four `sdk/…` citations in
`packages/contracts/CONTEXT.md` and `events-and-contracts.md`.

## Deviations and flags

Recorded rather than fixed; none of them is a regression this step introduced.

- **`packages/lib/grpc` declares no folder target.** Its content is two helper-defined clients
  (`argus_grpc_absl_bridge`, `argus_grpc_client_base`) and the health stubs
  (`argus_lib_grpc-health` / `argus::lib::grpc-health`). §2.2:131 implies a `lib/grpc` module;
  what exists is a folder of helpers, which is what its six files actually are. Row 12 recorded the
  same fact against §9.1's direct-import list; it stays a Phase 2 decision.
- **`packages/sync` and `packages/memory` are not helper-declared at all.** `packages/sync:170`
  hand-writes `add_library(argus_sync STATIC)` — the name happens to match the ungrouped convention
  but there is no `argus::sync` alias anywhere in the tree, so its consumers in `notification` (10
  spellings), `camera` (7), `productivity` (5) and `gateway` (2) name the raw target in guards, link
  lines and comments. `packages/memory:129,:176` hand-writes `memory-core` and `memory-catalog` for
  the same reason, named raw by `services/llm` at nine sites. The other five ungrouped packages
  (`audit`, `identity`, `intent`, `room`, `socket`) do it through `argus_module` and carry their
  alias. Left alone deliberately: converting them renames targets across 33 consumer lines, which is
  a naming decision with consumers, not a move.
- **`packages/lib/runtime` declares `hardware-profile` and `hardware-profile-gpu` through the
  ungrouped helper**, so two targets live under `packages/lib/` without the `argus_lib_*` prefix that
  §2.5 gives the group. The helper reserves an empty `GROUP` for service-local modules, and a module
  *inside* a lib package is a case the rule does not name — recorded as a gap in the rule rather than
  a violation of it, since the spelling in every consumer is correct today.
- **`argus_clients`' PROTO branch merges `SYSTEM_DEPENDS` into `DEPENDS`**, so a generated client
  cannot separate "needs this at configure time" from "needs this at link time". Left as-is; it is
  the helper's contract and changing it belongs to a step that needs the separation.
- **`ci.yml:40`'s glob is latently fragile.** `packages/*/config.toml.example` matches today, but it
  is written against the flat tree: if any owner nested a config example one level deeper the job
  would silently stop finding it. No change owed now.
- **Five gitignored local `config.toml` files point at paths that do not exist** —
  `packages/memory/config.toml:7` names `packages/argus-memory/database/schema.sql` and
  `services/gateway/config.toml:56` names `packages/argus-identity/database/schema.sql`, with three
  more service configs in the same state. They are untracked, so no commit can fix them, and the
  tracked `.example` counterparts are already correct; a developer running with those files would
  read a schema path from a folder that has never existed. Noted here so the discrepancy is not
  mistaken for repo content.
- **Plan §2.2's `health/`-without-`proto/` tree and §2.3/§2.6's header-only claim** hold for
  `validation` only — nine contracts are header-only `INTERFACE` packages by construction, `cert` is
  `STATIC`, and the `response` package is the hand-written exception.
- **`packages/contracts/CONTEXT.md`'s `sync/` bullet** still describes the cross-domain repository
  layout of the pre-split tree; the sentence is true of the module, not of the folder.
- **Three contract CMakeLists (`gateway`, `identity`, `notification`) fall below git's rename
  threshold** and appear in `git diff` as delete+add pairs. Verified with `-M20%` to be the intended
  transformation; a detection artifact, not three lost files.
- **Rule 25's service-local example is now a placeholder.** It was already illustrative before this
  step; naming `<module>` makes that honest instead of naming a target that does not exist.
- **`packages/clients/llm` still links `argus::lib::auth`** — the tier-3 → tier-4 edge the plan
  already records for Phase 2 step 4. The spelling is correct; the edge is the flag.

## Verification

Every claim below was measured on the tree this commit contains, by the step's own greps rather than
by reading the sweep's output.

**No retired spelling survives.** Over the 1474 tracked live files (`docs/history/` and
`third_party/` excluded), each of `argus_sdk_module`, `argus::sdk`, `argus-contracts`,
`argus_contracts_http`, `argus_client_health`, `argus::threading` and the four `*-client` folder
spellings answers **0 files**. `find` for a directory named `*-contract` or `*-client` outside the
build trees and the history returns **nothing**, and no folder repeats its group: `packages/lib/`,
`packages/contracts/` and `packages/clients/` hold no `lib`, `contracts` or `clients` subdirectory.

**Every CMake path resolves.** All 489 `${CMAKE_CURRENT_SOURCE_DIR}/…`, `${CMAKE_CURRENT_LIST_DIR}/…`
and `${CMAKE_CURRENT_FUNCTION_LIST_DIR}/…` references in the tree's 67 CMake files were resolved
against disk. Two do not resolve as written and both are by design: `cmake/argus-module.cmake:353`'s
`${proto}` is the loop variable of the `PROTO` list, whose branch is a deliberate two-location probe
(use the proto beside the `CMakeLists` if it exists, otherwise resolve against `PROTO_ROOT` — the
`PROTO_ROOT` branch is the one that fires, and it resolves), and
`services/camera/CMakeLists.txt:374`'s `../../packages/clients/${CLIENT}` expands over
`llm`, `vlm`, `notification`, all three of which exist.

**Every `packages/…` token resolves.** 118 distinct tokens across the live tree; three do not name a
path on disk and all three are intended: `packages/lib/grpc/src/grpc/grpc-client-base` is a unit
prefix in `packages/contracts/CONTEXT.md` prose (the `.hxx`/`.cc` pair exists beside it),
`packages/clients/health` is `packages/lib/grpc/CMakeLists.txt:31`'s past-tense comment recording
what the stubs were before row 8, and
`packages/identity/build/prod/tools/migrate-identity/argus-migrate-identity` is a path inside a built
tree, which the gateway's Dockerfile reads after the prod build and not before.

**No `add_subdirectory` can collide.** The tree's 47 explicit binary-directory labels were resolved
to repo paths: **0** labels are claimed by two distinct source directories. The label rewrite was
checked this way because a collision is the one failure mode a label has, and it is the reason the
49 sites were normalized to one canonical spelling per domain rather than merely de-suffixed.

**The alias rule still holds.** Across 499 invocations of the nine target commands
(`target_link_libraries`, `set_target_properties`, `target_compile_options`,
`target_include_directories`, `target_sources`, `target_compile_definitions`, `get_target_property`,
`target_link_options`, `add_dependencies`), **0** name an `argus::` alias as the target being
modified. Dependencies may be spelled as aliases; owners never are. This is the rule the whole
namespace rests on — an alias is read-only, so a mutating command naming one fails at configure
time — and it survived a change that rewrote every one of these files' neighbours.

**The integrity of the move itself.** 181 of the staged renames are `R100` (byte-identical), no moved
file is zero bytes, no file in the change is truncated, and the three `gateway`/`identity`/
`notification` contract `CMakeLists.txt` that `git diff` shows as delete+add pairs are rename
detection artifacts — both halves of each pair exist at the new path and declare
`argus_contracts(NAME gateway)` / `(NAME identity)` / `(NAME notification)`.

**What the gate does not cover.** The reference sweeps above resolve each path relative to the file
that names it, which is the *definition-site* resolution — the resolution that is correct inside a
function body. They therefore cannot catch the ncnn-class failure, where a path is right at its
definition site and wrong at every call site. That class is covered by the helper review below, which
re-read `cmake/argus-module.cmake` and each of its call sites, and by the gate, which configures
every one of the 18 projects from scratch.

**The gate itself.** `./scripts/build-all.sh dev` ran on the frozen tree: exit 0, 18 project headers,
every suite `100% tests passed, 0 tests failed`, and the reached-test ledger **302** — unchanged from
steps 11, 12 and 13. The log's 24 warning lines are 23 third-party (8 glslang, 8 ncnn, 3 llama.cpp
`CMAKE_CXX_STANDARD` notices, 3 ccache, 1 openfst) plus one
`CMake Warning: Manually-specified variables were not used by the project: CMAKE_TOOLCHAIN_FILE`
raised while configuring `packages/lib/sqlite`. That one was investigated rather than filed: it is a
reconfigure artifact, not a property of the tree. Emptied and rebuilt, sqlite configures and tests
clean — `./scripts/build-all.sh dev --only sqlite` exits 0 with **0 warnings** and 1/1 test — and the
toolchain is applied in all 18 projects either way: the conan toolchain's own
`Using Conan toolchain: <path>` line appears **once per project** in the gate log (18 times),
including the nine whose `CMakeCache.txt` holds the entry as
`CMAKE_TOOLCHAIN_FILE:UNINITIALIZED=<relative path>` rather than `:FILEPATH=<absolute>`. `cmake
--preset dev` passes the preset's relative spelling and CMake resolves and applies it; only the cache
entry's *type* differs between runs, and a project carrying the relative form reconfigures today with
0 warnings (measured on `packages/socket`, whose entry stays `UNINITIALIZED`). The stale cache the
warning came from was left by the interrupted run of an earlier gate, and a fresh configure removes
it. The orchestrator's own harness test, `./scripts/build-all-test.sh`, passes (`build-all tests
passed`, exit 0).

The 126 orphaned build directories the old labels left behind — 89 `*-contract`, 37 `*-client`,
1.4 GB, every one under a per-project `build/` gitignore and none of them tracked — were then
removed, which is the removal described above; nothing tracked was deleted (`git status` reports 0
deletions) and the gate's evidence stands on the tree this commit contains.

## The reviews

Four reviews ran against the tree — the helper file and its call sites, every CMake path in the tree,
the helper keyword contract, and the docs checked claim by claim. Every finding below was re-checked
here against the tree before it was believed, which is why the numbers that follow are the measured
ones and not the reported ones.

**The helper file and its 54 call sites.** The verdict on `cmake/argus-module.cmake` was clean: 14
`argus_lib` over 15 `lib/` folders, 9 `argus_contracts` plus the hand-written `response`, 10
`argus_clients`, 3 grouped `argus_client_module`, 16 `argus_module`, 2 `argus_service` — and the alias
rule intact across all 499 target commands. Its one actionable class was the 49 stale
binary-directory labels the section above describes; all 49 are fixed. Its other findings are the
flags recorded in the previous section. It counted 17 `argus_module` calls where a bare grep over the
tree's `CMakeLists.txt` counts 16 — the 16 the inventory above accounts for, five of them the
ungrouped packages; the review's verdicts come from the per-site check, not from the total.

**Every CMake path.** The independent scanner — 66 CMake files, 2644 paths, 418 of them
binary-directory paths — reported 4 unresolved paths and 0 label collisions. It was reproduced here
with the reviewer's own artifact, and **its positive control re-run**: pointed at a copy of the tree
with a planted stale label it does flag one. That is what turns "0 collisions" from an untested
silence into a tested negative. All 4 unresolved paths are the by-design cases the Verification
section lists.

**The helper keyword contract.** Five findings, four of them flags above. The fifth is latent and
worth the fix it got. `cmake_parse_arguments` does not report an argument the helper does not
declare, and in the realistic shape it fails *silently*: measured here on CMake 3.31,
`NAME n DEPENDS d SYSTEM_DEPEND Foo` parses to `ARG_DEPENDS='d;SYSTEM_DEPEND;Foo'` with an empty
`ARG_UNPARSED_ARGUMENTS`, so the misspelling travels into `target_link_libraries` as a name it cannot
resolve and surfaces at link time, far from the call that made it. No caller hits it today — the
near-miss is a `SYSTEM_DEPENDS` an author could plausibly write on `argus_clients`, which does
declare it — but it is a trap in the one file every package configures through, so
`argus_reject_unknown_args` now runs in all seven keyword helpers and rejects both a non-empty
unparsed list and any keyword-shaped token inside a value list. Verified by probe: all four typo
shapes fail at the call (stray token before any keyword, after an option, after an open multi-value
keyword, and inside `INCLUDES`), three clean calls pass untouched, and the guard's two rejections
that looked like false positives were the probe's own bad arguments — `argus_lib` declares no `GROUP`
and `argus_clients` no `MODULES`. Its limit is stated in the helper rather than hidden: a lowercase
misspelling is indistinguishable from a library name. The gate is the real proof — all 18 projects
configure with the guard in place.

**The docs, claim by claim.** 35 claims were checked against the tree. The 25 sites this step
invalidated are fixed and listed in the docs-pass addendum above. Two of the review's checks came
back clean and matter more than its findings: **no live file names an undeclared CMake target or
alias**, and **no live file names an old flat or suffixed folder path**. It also disproved two claims
a background agent had made about 12 stale `-client` and 37 stale `-contract` labels — both greps
return 0, because the label rewrite had already normalized them.

**What the doc review found that this step does not own.** Pre-existing rot, recorded so that a docs
row can sweep it in one pass instead of rediscovering it. Every line was re-verified here; every one
is prose, and none of it is a build fact.

- **Counts and lists that outlived their subject.** `docs/operations/deployment-docker.md:40` says
  three images carry extra tools and then names four (gateway, camera, productivity and notification
  — exactly the four Dockerfiles that carry them); `:61-63`'s container list omits `argus-guard`,
  which is a Compose service with its own image; `:79-83` lists six data-dir owners where
  `scripts/provision-host.sh:67` creates seven and Compose mounts `${ARGUS_DATA_DIR}/gateway` for the
  gateway process. `docs/operations/configuration.md:14` gives the gateway a `[sync]` table that no
  config has, and `:15-17` give camera, productivity and notification a `[database]` table that only
  guard has. `docs/README.md:20` says nineteen standalone projects where the tree has eighteen, and
  `:55-56` says every project folder carries an `AGENTS.md` and a `CONTEXT.md` where five of the
  eighteen (`packages/socket`, `packages/sync`, `packages/intent`, `packages/lib/cert`,
  `packages/lib/sqlite`) carry neither. `docs/architecture/data-storage.md:28` says "each owner …
  owns a migration CLI" and then names four.
- **Paths from an older tree.** `docs/architecture/wire-nats-subjects.md:56`
  (`src/shared/contracts/sync-operation.hxx` — its analogue in `wire-sync-tables.md` was fixed in
  this pass) and `:250` (`src/config/application.cc`; no `application.*` exists anywhere, the sink is
  installed in `services/gateway/src/main.cc`). `docs/architecture/wire-device-identity.md:5`
  (`backend/src/filter/device/device-filter.cc`). `services/gateway/CONTEXT.md:380,396`
  (`src/shared/services/cert/cert-service.cc`, `src/test/unit/cert-san-test.cc`).
  `docs/architecture/data-storage.md:60` ("every query lives in `src/shared/repositories/`").
- **Layout blocks naming folders that do not exist.** `src/server/` in seven services' `AGENTS.md`
  (camera, llm, stt, notification, productivity, voice, vlm) and `src/controllers/` in four
  (gateway, productivity, tunnel, voice). All of them are absent at HEAD too, so this step neither
  made them nor could have.
- **Claims a later phase falsified.** `services/guard/AGENTS.md:27` defers camera hardware to "a later
  phase" that has shipped — guard links `argus::clients::camera-actions` and `argus::clients::vlm`.
  `docs/architecture/camera-guardian-deep-analysis.md:3` says "no implementation included" while its
  own §15 records Phase 0 complete. `docs/operations/tool-calling-eval-set.md:4` calls the classifier
  retired while `packages/intent` is still one of the eighteen projects and `services/llm` links
  `argus::intent`. `packages/identity/CONTEXT.md:46,53` still call the auth filter package and the
  migration tool "not moved" — `packages/lib/auth` and `packages/identity/tools/migrate-identity`
  both exist. `services/camera/CONTEXT.md:230` warns about the `../src` include path and
  `services/camera/CMakeLists.txt:437` about suites running from `src/test/unit`; neither path
  exists in that service.
- **Citations to files that do not exist.** `docs/operations/provisioning-and-models.md:48`
  (`setup_certs()`; the function is `ensure_instance_certs()`, `scripts/lib/pki.sh:5`), and
  `models/intent/MODEL-CARD.md:36` (`scripts/evaluate.py`).

## The `.superpowers` SDD notes

Disclosed here because this step's audit of what the tree still holds surfaced it. Five notes under
`.superpowers/sdd/` are 357-byte tombstones dated 2026-09-21 instead of their original content. The
cause is a scripted `git show HEAD:<path> > <path>` restore aimed at a path that
`.superpowers/sdd/.gitignore` excludes with `*` — the restore wrote the *empty* blob that git had for
a file it was not tracking, over the file that held the real text. The content is unrecoverable: the
subtree was never in git and there is no other copy.

What that costs is bounded and worth stating precisely: no code, no commit and no step in
`docs/history/plans/architecture-plan.md` depends on those notes. The surviving records of the steps
they described are `docs/history/reports/f7-1b-argus-common-extraction.md`, `task-f7-brief.md`,
`HANDOFF.md` and `task-f8-status.md`, which are intact. The loss is of working notes, not of
decisions.
