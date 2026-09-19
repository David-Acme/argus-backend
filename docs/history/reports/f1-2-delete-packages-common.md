# Phase 1 step 2 — deleting the `packages/common` shim

Scope: `packages/common` removed and every consumer repointed at the real
owner (`packages/response`); 41 `.cc`/`.hxx`, 33 `CMakeLists.txt`, 7 docs,
`scripts/build-all.sh` and the root `AGENTS.md` touched. Base `e9e725c`. This
is the step `packages/common/CMakeLists.txt` itself announced: "Deleting this
package is the last step once no consumer links it."

## What `packages/common` had become

Not a module — a **transition shim**, and its own CMakeLists said so:

- `src/shared/exceptions/response-exception.hxx` was `#include
  <response-exception.hxx>` and `src/shared/wrapper/api-response/api-response.hxx`
  was `#include <http/api-response.hxx>`: two redirects into
  `packages/response`, which had owned the real definitions since the earlier
  extraction.
- `api-response.cc` compiled nothing but its own redirect header, yet the
  target carried `LINKER_LANGUAGE CXX` only because the leaf moves had left it
  header-only.
- Its `DEPENDS` still listed `access validation json hash text threading config
  storage nats`, and its PUBLIC block still carried `pkg::response-http`,
  `Drogon::Drogon` and `cnats::nats_static`. Any consumer that linked bare
  `argus_common` inherited that whole closure plus, in the pre-split model, a
  whole-tree include root — the two invariants `f7-1b-argus-common-extraction`
  records.
- `packages/response/src/http/CMakeLists.txt` existed only to give the shim's
  include root a `pkg::response-http` target; it goes with the shim.

So the step was not `rm -rf`: deleting the shim is only correct once every
consumer names what it really uses.

## What the unit did

1. **Deleted the package**: 10 tracked files (`.gitignore`, `AGENTS.md`,
   `CMakeLists.txt`, `CMakePresets.json`, `conanfile.txt`, the three shim
   sources, the suite) plus its local build output. `CONTEXT.md` survives as
   the `f7-1b` report — the git rename is part of this change, not a new file.
2. **Repointed the includes** in 41 sources: `<shared/wrapper/api-response/api-response.hxx>`
   → `<http/api-response.hxx>` and `<shared/exceptions/response-exception.hxx>`
   → `<response-exception.hxx>` — the paths the real owner exposes.
3. **Rewrote the dependency declarations** in 33 `CMakeLists.txt`: every target
   that said `argus_common` now names the leaves it actually uses, and every
   `add_subdirectory` the removal introduced is guarded by
   `if(NOT TARGET <alias>)`. The rule the tree now follows: **the provider adds
   its subtree unguarded, every consumer guards** — which is what makes a
   standalone configure and a consumed configure agree.
4. **Moved the envelope's suite** to `packages/response/tests/unit/api-response-test.cc`
   (the package that owns the code): same 10 cases, same names and order. One
   assertion was rebased — the definition-backed code assertion now uses the
   response package's own `ErrorDefinition` and points at `services/tts` for
   the wire contracts that assert their own definitions where they live.
5. **Dropped it from the build**: `scripts/build-all.sh` `PROJECTS` is 18
   projects, `packages/common` gone; the root `AGENTS.md` (19 dead paths
   repointed) and five other docs follow the files to their real owners.

## The test-registration defect this step uncovered

Deleting the shim exposed that **tests in packages added under a project were
being compiled but never run**. `enable_testing()` only propagates into
subtrees configured *after* it runs; the projects called it mid-file, just
before their own test block, so every package added earlier got no
`CTestTestfile.cmake`, and ctest's `subdirs()` chain dead-ends silently at the
first directory that lacks one.

Proven, not guessed: a minimal two-directory repro (`root` calls
`enable_testing()` after `add_subdirectory(b)`) leaves `b/c`'s tests
unreachable while `b/c/CTestTestfile.cmake` exists on disk. In this tree the
chain broke exactly there — `packages/cert/build/dev/CTestTestfile.cmake` says
`subdirs("config")`, and `config/CTestTestfile.cmake` was absent, orphaning
`api-response-test` and everything else beneath.

Fix: `enable_testing()` hoisted to immediately after `project()` in all 18
project files, with the reason in a comment. Measured effect on clean
configures: cert 7 → 11 tests reachable, socket 4 → 5, sqlite 1 → 2. The
reductions elsewhere are the legitimate shrink of the old `argus_common`
closure (the shim's build used to aggregate `sha256-test`,
`nats-wrapper-test`, `thread-budget-test` and `validation-dsl-test` into every
project) — each of those suites still runs, in the projects that own their
packages.

**Gotcha for the next steps**: declaring a dependency is only half of it. When
a module's `DEPENDS` gains an alias the project never used before, the project's
standalone block must also own that package's subtree
(`if(NOT TARGET <alias>) add_subdirectory(...) endif()`), or the standalone
configure fails at generate time with "Target ... links to ... but the target
was not found" — which is exactly what declaring `argus::validation` in
`tts-synthesis-http` first did, until tts's preamble added the guarded
`packages/validation` subtree too. The mirror case bites the same way: a
subtree the project already owns is not a declared dependency — tunnel's
standalone block had carried `packages/nats` all along while the target that
includes the nats headers linked none of it.

## Verification

- **Static**: 282 resolvable `add_subdirectory` paths across the tree exist on
  disk (0 missing; the 4 skipped are build-directory or loop-variable paths,
  checked by hand); no `CMakeLists.txt` anywhere still names `argus_common` or
  the deleted `pkg::response-http` alias, and no target links either.
- **Dependency audit**: a script walked every target that had linked
  `argus_common`, collected the owning package of every header its sources
  include (plus one transitive level), and compared that against the target's
  transitive link closure. One real defect: `argus_tts-synthesis-http`
  compiles validation DSL through its DTOs and did not declare
  `argus::validation` — confirmed by the compiler
  (`shared/validation/validation_dsl.hxx: No such file or directory`), fixed by
  declaring it. The audit's remaining hits were false positives, each
  explained: the `argus_client_module` alias convention (`argus::client-*`)
  which the checker had not modelled, headers with the same name in two
  packages (`health-rpc-service.hxx` in both `voice` and `camera`), and
  headers reached through a generated wire target. **Its blind spot** is worth
  recording: it walked `argus_module` targets only, so the plain
  `add_executable` targets declared directly in a project file were outside
  it — which is exactly where the second real defect lived (below).
- **Build**: `./scripts/build-all.sh dev` — 18/18 projects, all test suites
  green, no first-party compiler warnings. Tests per project: cert 11,
  socket 5, sqlite 2, identity 11, sync 14, memory 13, intent 4, gateway 20,
  camera 29, productivity 18, notification 23, guard 32, tts 7, stt 3,
  vlm 5, llm 21, voice 8, tunnel 9.
- **Nothing was lost in the shrink**: the suites the shim's build used to
  aggregate into every project still run where their packages belong —
  `sha256-test` in sync, gateway, camera, productivity, notification and guard;
  `nats-wrapper-test` in ten projects; `thread-budget-test` and
  `validation-dsl-test` wherever `threading` and `validation` are added. tts
  drops from 9 reached tests to 7 for exactly that reason: it no longer pulls
  `hash` and `nats`, which it never used.
- **The moved suite now actually runs**: `api-response-test` is reachable in
  17 of the 18 projects — the 11 that name `packages/response` themselves and
  the 6 that reach it through another package's guarded `add_subdirectory`;
  only `packages/intent` does not. Counted in the final full run: 17 runs.
  Before the hoist it was
  registered and orphaned — its CTestTestfile existed but no chain reached it,
  and the only place it ever ran was the shim's own build.
- **Suite moved, not rewritten**: the 10 `TEST_CASE` names of
  `api-response-test` are identical to the deleted file's, in the same order.
- **The repointing is include-only**: every one of the 41 edited sources has
  added and removed `#include` lines and nothing else — no blank-line churn at
  the seam, no reordering, no reformatting. The only source diff with other
  content in it is the moved suite (155 lines), which is the suite itself.
- **One build warning, chased down**: the full pass printed
  `ninja: warning: premature end of file; recovering` in `packages/memory`. It
  reproduced on a memory-only rebuild and disappeared after deleting a
  truncated `.ninja_deps`/`.ninja_log` from that project's build directory —
  corrupt untracked build state, not a source or CMake defect; memory rebuilds
  clean at 13 tests. No first-party compiler warning appeared anywhere in the
  18 projects.

## Code review of the change

Reviewed: the 33 `CMakeLists` (for every rewritten target, its declared
`DEPENDS` against what its sources include; every `add_subdirectory` the change
introduced, guarded), the 18 project files (`enable_testing()` placement, the
standalone blocks, no dangling alias), the 41 source edits (include lines only —
no reordering, no reformatting, no double blanks at the seam), the moved suite
(case-for-case identical to the deleted one), and the six docs. Findings and
their disposition:

- `tts-synthesis-http` did not declare `argus::validation` — found by audit,
  fixed.
- tts's standalone block then lacked the `packages/validation` subtree — found
  by the tts-only configure, fixed.
- `argus-tunnel-relay` did not link `argus::nats` — found by the compiler in
  the full build, after every other project had gone green:
  `services/tunnel/src/main-relay.cc:9: fatal error:
  shared/wrapper/nats/nats-bus.hxx: No such file or directory`. The relay's
  `main` builds a `NatsBus` and subscribes to the push-intent subject, but its
  target linked only `tunnel-core`; the include root had arrived through the
  shim's PUBLIC closure before. tunnel's standalone block already owned the
  guarded `packages/nats` subtree, so the missing half was the link itself.
  Fixed by linking `argus::nats`; tunnel rebuilds green at 9 tests.
- A duplicated nested `if(NOT TARGET argus::tts-client)` in
  `services/tts/CMakeLists.txt` — an artifact of this change, removed.
- Stale `argus-common_SOURCE_DIR` entries in the project `CMakeCache.txt`
  files: gone — every project reconfigured during this build.
- Mixed standalone sentinels (above) — safe, deferred as hygiene.
- tts's module-folder `file(GLOB)` — checked against the rule, allowed.

## Deferred, and findings outside this unit

- 12 comment lines still say a dependency "used to arrive through
  argus-common" (the guarded standalone blocks). They explain why those blocks
  exist; step 11's doc refresh owns rewording them.
- `docs/architecture/build-model.md:9` and
  `docs/architecture/services-and-packages.md:37` still list `argus-common` in
  the layout tables. Step 11 / Phase 3d step 4 own those tables, which change
  meaning only when the gateway goes.
- `packages/lib/errors/` is an untracked skeleton (AGENTS.md, CMakeLists
  declaring `src/errors/*` sources that do not exist yet, and a test wired to
  nothing). It belongs to step 10 (`lib/errors` + `lib/http`), so it is left
  untracked and out of this change.
- The standalone blocks guard themselves with three different sentinels —
  `PROJECT_IS_TOP_LEVEL` (camera, productivity, notification, guard, voice),
  `NOT TARGET Drogon::Drogon` (tts, stt, vlm, tunnel) and the third-party probe
  the split originally used (`NOT TARGET argus_identity` in gateway,
  `NOT TARGET fasttext` in intent, `NOT TARGET llama` in llm). Every inner add
  is `if(NOT TARGET <alias>)`-guarded, so all three are safe, but they are not
  the same question. Unifying them is a hygiene step of its own, left out of
  this change so the verified build stays the verified build.
- `services/tts/CMakeLists.txt:67` globs `${TTS_SRC_ROOT}/feature/*/CMakeLists.txt`
  to auto-discover its feature modules. Checked and cleared: that is the one
  allowed glob (module folders), not a source glob.
