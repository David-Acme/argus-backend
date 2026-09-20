# Phase 1 step 7 — `threading` + `hardware` into `runtime`

Scope: the row is one line — "Merge `threading` + `hardware` into `runtime`" — and it is the
first row of the phase that is a real code move rather than a measurement or a deletion. Two
packages become one; the interesting part is not the move but the two questions the row does
not answer: **where the merged package sits in the tree** (flat `packages/runtime` or Phase 2's
`packages/lib/runtime`) and **what happens to the one target the second package declared in a
folder of its own**. Base `82e26d8`.

## What the merge is

`packages/threading` was five headers, one source and one test:

| file | role |
|---|---|
| `src/shared/wrapper/thread-budget/thread-budget.{cc,hxx}` | the adaptive thread counts |
| `src/shared/wrapper/ai-init/ai-init.hxx` | the shared AI-engine mutex |
| `src/shared/wrapper/blocking-task/blocking-task.hxx` | the off-loop coroutine awaiter |
| `src/shared/wrapper/cancellation/cancellation-token.hxx` | streaming cancellation |
| `tests/unit/thread-budget-test.cc` | its suite |

`packages/hardware` was one wrapper folder holding two targets over one source pair —
`hardware-profile` (compiled with `ARGUS_NO_NCNN_GPU=1`) and `hardware-profile-gpu` (the Vulkan
probe, ncnn-linked when the graph has ncnn). Both declare the same include root, `src`, and the
same source pair, `hardware-profile.{cc,hxx}`.

The merge is therefore a move plus one CMakeLists: `git mv packages/threading packages/runtime`,
`git mv` of the two profile files into `packages/runtime/src/shared/wrapper/hardware-profile/`,
`git rm` of `packages/hardware/src/shared/wrapper/hardware-profile/CMakeLists.txt`, and a
rewritten `packages/runtime/CMakeLists.txt` that declares all three targets.

**0 `.cc`/`.hxx` changed.** `git diff --cached -M --raw` pairs all nine moved files at `R100` —
byte-identical renames, no source touched. The proof that no consumer source *could* need a
change is the include root: `argus_module` resolves `SOURCES` and `INCLUDES` with
`cmake_path(ABSOLUTE_PATH … BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})`
(`cmake/argus-module.cmake:22-33`), so the old `INCLUDES ../../..` under
`packages/hardware/src/shared/wrapper/hardware-profile/` and the new `INCLUDES src` under
`packages/runtime/` both name the package's own `src/` — the root moved with the sources. All 19
includers spell the header package-relative (`#include
<shared/wrapper/hardware-profile/hardware-profile.hxx>`), so nothing in the tree ever named the
old path.

## The decision the row does not make

The row says `runtime`; §2.2's target tree says `packages/lib/<unit>` and §2.5 says the
namespace mirrors the folder (`argus_lib_<name>` → `argus::lib::<name>`). The tree is at Phase 1,
where steps 3 and 4 already answered this: both merged or redistributed packages kept the flat
`packages/<name>` spelling and the `argus_<name>`/`argus::<name>` aliases, and both recorded
`lib/X` as "Phase 2's rename". Two more facts settle it:

- §2.6 lists `argus_lib` (with `argus_contracts` and `argus_clients`) as a helper that "needs
  work before Phase 2 can land", and Phase 2 step 1 is the move into `lib/`, `contracts/`,
  `clients/`. **`argus_lib` does not exist** in `cmake/argus-module.cmake`, which declares
  `argus_module`, `argus_client_module`, `argus_service` and the substrate helpers only.
- The asymmetry: staying flat costs one rename in Phase 2 — a mechanical `add_subdirectory`
  path and alias sweep over the same ten consumers this step already swept once. Using
  `packages/lib/runtime` now costs a new build helper, an alias scheme (`argus::lib::runtime`)
  that ~10 consumers must be repointed to, and a tree where exactly one package is nested — all
  of it landing in Phase 1, whose whole point is that it does not change meaning.

So this step produces `packages/runtime`, target `argus_runtime`, alias `argus::runtime`, and
the Phase 2 move renames it along with every other package.

## Three targets, one CMakeLists

The merged file declares `runtime`, `hardware-profile` and `hardware-profile-gpu`. Rule 25 says
"one folder = one module = one `CMakeLists.txt`", and the pre-merge tree satisfied it for
`hardware` by giving the profile its own folder and CMakeLists — at
`packages/hardware/src/shared/wrapper/hardware-profile/CMakeLists.txt`, **depth 5, the deepest
CMakeLists anywhere under `packages/`** (everything else is depth 1 for a package, 2 for the
`clients/`/`contracts/` groupings, or a `tools/` helper executable). The merge keeps the
two-target split, which it must: the CPU variant exists so that consumers stay ncnn-free, and
one target would make every consumer of the thread budget link ncnn. What it does not keep is
the nested file — the three targets are declared where `threading` declared its own sources.

That is the shape's one cost and it is recorded rather than hidden: under a strict reading of
rule 25 the profile deserves its own module folder, and Phase 2 step 2 ("apply the package
layout of §2.3 to every package") is where the tree decides whether the profile becomes
`packages/runtime/hardware-profile/` with its own CMakeLists or stays a second and third target
of the runtime package. The upside is real and immediate: **no `packages/` CMakeLists now sits
inside a package's `src/` tree** — the deepest one left under `packages/` is
`packages/identity/tools/migrate-identity/` (depth 3, a `tools/` executable of the kind the
folder architecture names beside `src/`), where the profile's was depth 5 — and the one file
that had to be found by path, `packages/hardware/src/shared/wrapper/hardware-profile`, is gone.

## The consumer repointing

13 files in 10 projects. Nine guarded `packages/threading` (six of them *also* guarded the
profile folder, which declared no tests), so the threading guard became the runtime guard and
the profile guard was deleted, not repointed: one folder now creates all three aliases, so the
`if(NOT TARGET argus::hardware-profile)` block cannot be false while `argus::runtime` exists.

| consumer | file(s) | what changed |
|---|---|---|
| `packages/auth` | `CMakeLists.txt` | guard + 1 link line |
| `packages/identity` | `CMakeLists.txt` | profile guard replaced by the runtime guard, threading guard dropped as its duplicate, 1 link line |
| `packages/memory` | `CMakeLists.txt` | same shape as identity, `CMAKE_CURRENT_BINARY_DIR` form |
| `services/gateway` | `CMakeLists.txt` | guard, no link line (it guards a subtree its sources reach through other packages — the step-3 shape) |
| `services/stt` | `CMakeLists.txt` | guard + 2 link lines |
| `services/tts` | `CMakeLists.txt`, `src/feature/synthesis/CMakeLists.txt` | guard (comment rewritten), profile guard deleted, 1 link line in each file |
| `services/vlm` | `CMakeLists.txt` | guard, profile guard deleted, 3 link lines |
| `services/llm` | `CMakeLists.txt` | guard, profile guard deleted, 2 link lines |
| `services/camera` | `CMakeLists.txt` | guard, profile guard deleted, 4 link lines |
| `services/guard` | `CMakeLists.txt`, `src/feature/guard/CMakeLists.txt`, `src/feature/api/guard/CMakeLists.txt` | 2 link lines in the project file and 1 in each feature file; no guard — the project reaches the alias through `packages/identity`, exactly as it reached `argus::threading` before |

Every alias that survives is spelled `argus::runtime`; no target links `argus::threading` or
`argus::hardware-profile*` through a name that no longer exists anywhere. The `hardware-profile`
links themselves are unchanged — `packages/memory:154`, `services/camera:270,275,333`,
`services/vlm:104,161`, `services/llm:165,259`, `packages/identity:211,227` still name the two
profile aliases, which the same subtree now provides.

**The ncnn ordering was checked, not assumed.** `argus_hardware-profile-gpu` links ncnn only
`if(ARGUS_NCNN_TARGET AND TARGET ${ARGUS_NCNN_TARGET})`, so what matters is whether the subtree
is added before or after ncnn in each consumer. It is added at the same position in both
consumers that link the GPU variant, because the deleted profile CMakeLists sat exactly there:
`services/camera` adds it after ncnn (`:51-53` then `:81`), `packages/identity` before ncnn
(`:32` then `:122`) — and identity's `argus_hardware-profile-gpu` was unlinked there before the
merge too. Nothing about the ncnn edge changes; the gate's link of `libargus_hardware-profile-gpu.a`
is what confirms it.

Documentation follows the files: `AGENTS.md` rule 13b (`:309`), rule 13c (`:320`), the Key Files
rows at `:701`, `:714`, `:715`, a new Key Files row for the profile folder at `:716`, and
`docs/operations/hardware-tiers.md:15`. `packages/hardware/` is gone from disk — the `git mv`
emptied it and the empty chain was removed, so no orphan directory is left to look like a
package.

## Verification

- **The phase gate is green**: `./scripts/build-all.sh dev` exits 0, "[setup] All selected
  projects built and tested (profile: dev)". The run is the second one — the first was
  interrupted mid-project-6 (memory) when the session ended, and the artifacts it left made this
  one incremental. It is the run reported here.
- **The reached-test ledger is unchanged at 288**, project by project and in `build-all.sh`'s
  order: cert 15, socket 8, sqlite 2, identity 15, sync 19, memory 16, intent 4, gateway 25,
  camera 34, productivity 23, notification 28, guard 36, tts 9, stt 3, vlm 5, llm 23, voice 13,
  tunnel 10 — the same numbers step 4 recorded. That is arithmetic, not luck: nine files guarded
  `packages/threading`, the six that also guarded the profile folder sat beside a threading
  guard, and the profile declared no tests, so no project gained or lost a suite and
  `thread-budget-test` still runs wherever it ran before.
- **No first-party warning**, and no warning class this step introduced: 23 `Warning:` lines in
  the whole log, every one third-party — ncnn and glslang's `CMAKE_CXX_STANDARD` notices (16),
  llama.cpp/ggml's (3), the `GGML_CCACHE` "ccache not found" note (3) and openfst's (1, from
  `services/stt/build/dev/_deps`). The same classes step 5's run recorded. (A case-sensitive
  `grep 'warning:'` reports zero and is wrong: the prefix is `-- Warning:`, capital W.)
- **The tree names neither old package**: `grep -rn -E 'packages/(threading|hardware)(/|[^a-z-])'`
  returns nothing outside `build/`, `third_party/` and `docs/history/` — the plan's own rows and
  the reports are historical records — and the same grep over `docs/` excluding `docs/history/`
  returns nothing either.
- **Every `add_subdirectory` path in the tree resolves**: 291 calls, none pointing at a
  directory that does not exist, four variable-shaped ones skipped. The first version of that
  check reported 278 "missing" paths and every one was its own defect — it resolved
  `${CMAKE_CURRENT_SOURCE_DIR}/../auth` by stripping the variable, leaving `/../auth`, and then
  looked for it at the filesystem root. The check is worth its answer only once the variable is
  substituted rather than stripped.
- **The merge leaves no residue**: the 27 `build/dev/threading` and `build/dev/hardware-profile`
  trees the old `add_subdirectory` calls created are gone; what still carries either name under
  `build/` is the current target's object-file mirror
  (`build/dev/runtime/CMakeFiles/argus_hardware-profile{,-gpu}.dir/src/shared/wrapper/hardware-profile`,
  15 project trees each), which is what the merged package is supposed to produce.

## Findings outside this unit

- **51 orphaned build directories from steps 2 and 3 were removed** — `build/dev/argus-common`,
  `build/prod/argus-common` (the step-2 shim) and `build/dev/common` (the step-3 layout) across
  16–18 projects, **8.3 GB in total**. They are untracked, unreferenced by any current configure
  (the source directories they mirror are gone) and inert — unlike the stale `CMakeCache.txt`
  files step 1 warned about, nothing regenerates them and ctest's `subdirs()` chain no longer
  names them — but 8.3 GB of caches that bear the name of a package deleted two steps ago is
  exactly the trap step 1's gotcha describes, and step 3 removed its own `build/dev/json` and
  `build/dev/hash` trees for the same reason. Deleted **after** the gate ran: no tracked file
  changed with them, so the green run above still describes the tree being committed.
- **`packages/lib/errors/` is an untracked draft of step 10** — a `CMakeLists.txt` calling a
  non-existent `argus_lib(NAME errors …)`, an `AGENTS.md`, and `tests/unit/error-definition-test.cc`,
  with **no `src/` at all**. Its include of the helper is also one level short
  (`../../cmake/argus-module.cmake` resolves to `packages/cmake/` from `packages/lib/errors/`),
  so it would not configure as written. Step 10 creates `lib/errors` from `packages/response`
  and owns the reconciliation; the recommendation recorded here is the flat `packages/errors`
  for Phase 1 uniformity, under the same reasoning as the decision above, with the `lib/`
  grouping left to Phase 2 step 1 — and the draft's `argus_lib` dependency dropped with it,
  since §2.6 assigns that helper to Phase 2.
- **The two dead `hardware-profile` guards that were deleted here are not the dead guards f1-6
  flagged.** `services/guard` and `packages/cert` guard `packages/socket` and `packages/room`
  while naming neither in any source; every `hardware-profile` guard removed by this step
  belonged to a project whose sources include `hardware-profile.hxx` or whose sibling guard was
  live, and its subtree moved rather than vanished. The socket/room pair stays deferred to the
  step that unifies the standalone-block sentinels (row 2's entry).
- **§9.1's `threading` and `hardware` rows are left as they stand** (`lib/runtime` / → `lib/runtime`).
  §9.1 states of itself that it is "measured on the working tree on 2026-09-19 … the baseline
  the migration is checked against", and the phase's Done rows are where the deltas live —
  steps 3 and 4 left the `json`, `hash` and `access` rows untouched for the same reason. Step 6
  corrected the `room` row because its *measurement* was wrong, not because the tree had moved.
- **`services/guard` still has no guard of its own for the runtime subtree** — it links
  `argus::runtime` in three files and relies on `packages/identity`'s guard to create the target
  in a standalone configure. That is exactly how it reached `argus::threading` before, so this
  step changes nothing; it is recorded because Phase 2's DAG verification (step 4) is where
  "which project owns which guarded subtree" gets decided once, rather than per consumer.
