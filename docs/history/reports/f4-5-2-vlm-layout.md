# Phase 4 step 5 — argus-vlm gets the reference shape

`services/vlm` is the second of the three legacy horizontal services step 5
names — `stt`, `vlm`, `llm`. It takes the same shape `services/stt` settled one
unit earlier: `src/app/` for composition, `feature/<feature>/` for the vertical
slice, and a build in which the folder that owns a translation unit declares
itself (rule 25). What it adds over stt is weight — an engine bootstrap of its
own (llama.cpp + libmtmd, seventeen cache variables and a CUDA/Vulkan probe;
stt's sherpa-onnx is a vendored submodule too, so that is not the difference),
a DTO folder, two test targets instead of one, and a remote adapter that only a
test consumes today.

## What existed before

- `src/main.cc` (65 lines): config load, `ListenerConfig::resolve(7031)`, the
  health and `VlmController` `registerController` calls, `llama_backend_init()`,
  the engine boot gate (`vlm->initEngine()`, then `LOG_FATAL` +
  `llama_backend_free()` + `return 1` when `isEngineLoaded()` is false),
  `app().run()`, then `vlm->shutdownEngine()` and `llama_backend_free()`.
- `src/controllers/`: `vlm-controller.{hxx,cc}` (28 + 80 lines) — a
  `drogon::HttpController<VlmController, false>` with two routes,
  `/vlm/v1/describe` (POST) and `/vlm/v1/config` (GET) — and `vlm-errors.hxx`
  (16 lines) with the vision catalog entries.
- `src/vlm/describe-dto.{hxx,cc}` (14 + 27 lines): the base64/JSON describe
  request DTO on the validation DSL.
- `src/shared/services/vision/`: `vision-service.{hxx,cc}` (90 + 362 lines) —
  the LFM2.5-VL engine, owned BY VALUE by the controller — `vision-hash.hxx`
  (39 lines), and `remote/` (`vlm-remote.{hxx,cc}`, 48 + 263 lines, the wire
  client, and `remote-vision-adapter.{hxx,cc}`, 55 + 116 lines, the adapter
  whose only in-repo consumer is `vision-remote-adapter-test`).
- the root build: `add_library(vlm-core STATIC)` over three sources
  (`controllers/vlm-controller.cc`, `vlm/describe-dto.cc`,
  `shared/services/vision/vision-service.cc`) with `third_party` as a SYSTEM
  PUBLIC include root, `src` as the PUBLIC include root, ten PUBLIC link
  libraries and `-Wall -Wextra`; a hand-written `add_executable(argus-vlm
  src/main.cc)` linking that archive with hand-written `$ORIGIN` rpath
  properties and a POST_BUILD `models` symlink; `vlm-wire-test` re-listing
  three of the production sources and carrying eight link entries (the sixth
  being `${ARGUS_LLAMA_TARGETS}`);
  `vision-remote-adapter-test` listing the two remote `.cc` files and carrying
  four.
- No `src/server/`. The service's `AGENTS.md` layout block named one;
  `git log --all -- services/vlm/src/server` is empty, so the line described a
  directory that never existed in this service's history.

## The move

Thirteen renames and nine include lines rewritten:

```
rename services/vlm/src/{ => app}/main.cc (96%)
rename services/vlm/src/{ => feature/vlm}/controllers/vlm-controller.cc (98%)
rename services/vlm/src/{ => feature/vlm}/controllers/vlm-controller.hxx (93%)
rename services/vlm/src/{ => feature/vlm}/controllers/vlm-errors.hxx (100%)
rename services/vlm/src/{vlm => feature/vlm/dtos}/describe-dto.cc (100%)
rename services/vlm/src/{vlm => feature/vlm/dtos}/describe-dto.hxx (100%)
rename services/vlm/src/{shared/services/vision => feature/vlm/services}/remote/remote-vision-adapter.cc (96%)
rename services/vlm/src/{shared/services/vision => feature/vlm/services}/remote/remote-vision-adapter.hxx (95%)
rename services/vlm/src/{shared/services/vision => feature/vlm/services}/remote/vlm-remote.cc (100%)
rename services/vlm/src/{shared/services/vision => feature/vlm/services}/remote/vlm-remote.hxx (100%)
rename services/vlm/src/{shared/services/vision => feature/vlm/services}/vision-hash.hxx (100%)
rename services/vlm/src/{shared/services/vision => feature/vlm/services}/vision-service.cc (99%)
rename services/vlm/src/{shared/services/vision => feature/vlm/services}/vision-service.hxx (100%)
```

Nine include lines in eight files carried a service-local path and all nine
were rewritten — `<controllers/vlm-controller.hxx>` became
`<feature/vlm/controllers/vlm-controller.hxx>` (twice: `src/app/main.cc` and
the wire test), `<vlm/describe-dto.hxx>` became
`<feature/vlm/dtos/describe-dto.hxx>`,
`<shared/services/vision/vision-service.hxx>` became
`<feature/vlm/services/vision-service.hxx>`, `<shared/services/vision/
vision-hash.hxx>` became `<feature/vlm/services/vision-hash.hxx>` (twice),
and `<shared/services/vision/remote/{vlm-remote,remote-vision-adapter}.hxx>`
became `<feature/vlm/services/remote/…>` (three times). Measured after the
rewrite, on the include form itself: `git grep` for `<controllers/vlm`,
`<vlm/describe-dto` and `<shared/services/vision` under `services/vlm` returns
nothing, where `HEAD` matched nine lines across eight files. The bare words
survive on their own — `controllers/vlm` in 3 files and `vlm/` in 12 — because
the new spellings contain them (`feature/vlm/controllers/…`), which is why the
test is run on the angle-bracketed form; `shared/services/vision` is the one
that is gone outright, 0 files.

Two of the nine rewrites re-sorted the block they sit in, both where the new
spelling sorts earlier than the old one: `remote-vision-adapter.cc` (its
`config/`, `feature/`, `runtime/` block is now sorted, `HEAD` had
`vision-hash.hxx` before `remote/vlm-remote.hxx`) and the wire test (now
`config/`, `drogon/`, `feature/`, `http/`, `http/`). `src/app/main.cc`,
`vlm-controller.hxx`, `vlm-controller.cc`, `vision-service.cc` and
`remote-vision-adapter.hxx` each had their single line rewritten in place, in
the order `HEAD` already had it — that in-place line is what their 93–99%
similarity measures.

The include root did not move: the module publishes `src/` exactly as the
archive did (`INCLUDES ../..`), so the new spellings are the only ones that
changed and nothing outside `services/vlm` was touched by them.

## The build, converted

`src/feature/vlm/CMakeLists.txt` is new and declares the one module:

```cmake
argus_module(NAME vlm
    SOURCES
        controllers/vlm-controller.cc
        dtos/describe-dto.cc
        services/remote/remote-vision-adapter.cc
        services/remote/vlm-remote.cc
        services/vision-service.cc
    INCLUDES
        ../..
    DEPENDS
        argus::lib::config
        argus::hardware-profile
        argus::lib::runtime
        argus::lib::validation
        argus::lib::http
        Drogon::Drogon
        opencv::opencv
        nlohmann_json::nlohmann_json
        llama
        mtmd)
```

plus the archive's SYSTEM PUBLIC `third_party` include root and
`-Wall -Wextra`, which `argus_module` does not add. The ten `DEPENDS` are the
archive's ten PUBLIC links, target for target.

The root file discovers it (`file(GLOB VLM_FEATURE_MODULES CONFIGURE_DEPENDS
"${VLM_SRC_ROOT}/feature/*/CMakeLists.txt")`, then `add_subdirectory` per
directory) and composes the executable through the helper:

```cmake
argus_service(NAME argus-vlm
    MAIN ${VLM_SRC_ROOT}/app/main.cc
    MODULES
        argus::vlm
        argus::lib::config
        argus::hardware-profile
        argus::lib::runtime
        argus::lib::validation
        argus::lib::http
        Drogon::Drogon
    PORTS 7031)
```

`argus_service` (`cmake/argus-module.cmake:411-427`) is what makes the
hand-written properties redundant: it applies `-Wall -Wextra`, sets
`BUILD_RPATH`/`INSTALL_RPATH` to `$ORIGIN`, calls `argus_runtime_rpath` and
records `ARGUS_PORTS` (which nothing in the tree reads — pre-existing, recorded
in the plan at `architecture-plan.md:876`). What no helper supplies stays by
hand and is unchanged — the llama.cpp bootstrap's seventeen cache variables
plus `include(CheckLanguage)`/`check_language(CUDA)` and the Vulkan/
SPIRV-Headers probe, the doctest `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES`
treatment, and the POST_BUILD `models` symlink next to the binary.

Three things were dropped, each because this change removed its last reader:

- `set(SHARED_ROOT ${VLM_SRC_ROOT}/shared)` — `src/shared/` no longer exists.
- `find_package(doctest REQUIRED)` — the file called it twice; the first call
  (before the doctest system-include treatment) stands.
- `set(ARGUS_LLAMA_TARGETS llama mtmd)` — its only reader in this project was
  the wire test's link line, which now links `argus::vlm`. `llama` and `mtmd`
  travel to every consumer through the module's PUBLIC `DEPENDS`.

The guard became the reference one. `HEAD` opened with
`if(NOT TARGET Drogon::Drogon)` around the four `find_package` calls and the
four `add_subdirectory` calls, and the llama.cpp bootstrap sat outside it at
top level. It is now `if(PROJECT_IS_TOP_LEVEL)` (`services/guard`'s spelling)
with the bootstrap inside it, which is what the bootstrap's own contents
permit: it sets cache variables, probes CUDA and Vulkan, and
`add_subdirectory`s `third_party/llama.cpp` — a standalone project that
consumes nothing the guarded `find_package` calls provide, unlike stt's
sherpa-onnx block, which reads `onnxruntime::onnxruntime`. The guard's
direction is the same as stt's: a nested configure must now supply everything
the module's `DEPENDS` names, and the standalone configure — the only
reachable one — is unaffected.

Both tests now link the module instead of re-listing it:

```cmake
target_link_libraries(vlm-wire-test PRIVATE argus::vlm doctest::doctest)
target_link_libraries(vision-remote-adapter-test PRIVATE argus::vlm doctest::doctest)
```

`vlm-wire-test` compiled the controller, the DTO and the engine a second time
in `HEAD` and carried eight link entries; `vision-remote-adapter-test`
compiled the two remote sources and carried four. Both keep their
`EXCLUDE_FROM_ALL FALSE`,
their `tests/support` include path (for `fake-vlm-server.hxx`),
`ARGUS_TEST_VLM_MODELS_DIR` on the wire test, and `-Wall -Wextra`; the include
root, the third-party root, Drogon, OpenCV, nlohmann_json, llama and mtmd now
travel with the module. The tests' `${VLM_SRC_ROOT}` include directory is gone
for the same reason the module publishes it.

Ownership, measured: `find services/vlm/src services/vlm/tests -name '*.cc'`
is eight first-party translation units; each of the five production sources is
named by exactly one module, `src/app/main.cc` is named by no `SOURCES` list —
it is the service's `MAIN` — and the two test sources are named by their
`add_executable`s.

## The wire, unchanged

Rule 1 of the service's own `AGENTS.md` is that the engine is THE capacity, and
rule 3 that the envelope is frozen. Measured on the dev binary the change
produced: both route strings are still registered (`/vlm/v1/config`,
`/vlm/v1/describe`, read off the binary), `nm -C` finds 411 `VisionService` /
`VlmController` symbols in `argus-vlm` (32 of them the engine's `init` and
`describeMat`), and `vlm-wire-test` — which drives the real LFM2.5-VL engine
over a loopback socket — passes in 35.90 s, with
`vision-remote-adapter-test` beside it at 0.03 s.

## What the gates measured

- `check-comments`: 1357 files checked, 0 comments (1356 before: the new module
  declaration is one more file to scan).
- `check-deps`: 109 declarations, 813 edges, 0 forbidden, 0 cycles,
  0 unresolved, 23 edges deferred, 290 third-party mentions over 21 roots
  (107/812/292 over 21 before). The delta is accounted for exactly, by running
  the scanner's own `scan()` against `HEAD` in a temporary worktree and
  against the working tree: declarations +2 (`argus_module(NAME vlm)` and
  `argus_service(NAME argus-vlm)`, neither of which the file had — it used a
  bare `add_library`/`add_executable`); edges +1, which is `services/vlm`'s own
  list going from 12 entries to 13 (the five `argus::` deps the module
  declares, the three `argus::vlm` mentions the service and its two tests now
  make, and the five package-level deps the service's `MODULES` list restates)
  against the twelve the three old link lines carried; and third-party mentions
  292 → 290, the sum of `${ARGUS_LLAMA_TARGETS}` 2 → 1, `Drogon` 88 → 89 (the
  service's `MODULES` list names it) and `opencv` 7 → 5 (named once in the
  module instead of twice in the two link lines that are gone).
- `./scripts/build-all.sh dev --only vlm`: 0 errors, 0 warnings, ctest 12/12.
- whole-tree `scripts/check-tidy.sh`: 539 TUs, 2901 findings over 45 checks,
  baseline 2901 — not one check above the baseline and none below it, exit 0.
  The numbers are the ones step 4 recorded (`argus-productivity`/`argus-guard`),
  unchanged by this unit: the scan is keyed by source path and `services/vlm`
  has the same eight first-party `.cc` files it had before the move.

## Review

Two adversarial reviewers read the change: one on build semantics, one on the
documents. Their reports are claims, not evidence — every finding below was
re-measured here before it was acted on, and the ones that did not survive are
recorded as such.

### Build semantics (one finding acted on, four recorded)

**Acted on — a now-false gloss in the root agent contract.** The reviewer
found `AGENTS.md`'s llama.cpp bullet still claiming
`Targets: ${ARGUS_LLAMA_TARGETS} (= llama mtmd)`. Re-measured: the only live
setter left in the tree is `services/llm/CMakeLists.txt:57`
(`set(ARGUS_LLAMA_TARGETS llama)`), read once at `:243`; the
`llama mtmd` value this change deleted from `services/vlm/CMakeLists.txt` was
its last definer. The bullet now names the two consumers instead of a variable
whose value no longer matches the gloss.

**Recorded, no change — the adapter test gained `DT_NEEDED` on the engine.**
`readelf -d` on `services/vlm/build/dev/vision-remote-adapter-test` lists
`libllama.so.0`, `libmtmd.so.0` and the four `libggml*.so.0`; `HEAD`'s target
linked `argus::lib::config argus::lib::runtime opencv::opencv doctest::doctest`
and compiled the two remote sources itself, so it carried none of them. It is
a real change in the binary, not in its behaviour: the test exercises the
remote adapter over a loopback socket, runs under `ctest` with the build tree's
rpath covering those libraries, and passes in 0.03 s. The only alternative —
keeping the test's own `.cc` re-list — is what rule 25 removes. Not a defect.

**Recorded, no change — the `third_party` SYSTEM include root has no
first-party includer.** The module republishes `HEAD`'s
`target_include_directories(vlm-core SYSTEM PUBLIC …/third_party)`. Measured:
the only third-party headers the service includes are `<llama.h>`,
`<mtmd.h>` and `<mtmd-helper.h>`, which the `llama` and `mtmd` targets carry
their own include directories for (visible in `compile_commands.json`), and
`<json/value.h>` from `nlohmann_json`. The reviewer's own recommendation was to
record it rather than remove it, and the line is part of the interface `HEAD`
declared — unlike `services/stt`'s two roots, whose sherpa-onnx header is a
first-party include. Kept as carried; a future tidy pass should not read it as
load-bearing.

**Recorded, no change — the guard makes a nested configure of this project
unconfigurable.** `if(PROJECT_IS_TOP_LEVEL)` now wraps the `find_package`
calls and the llama bootstrap. Measured: no `add_subdirectory` of any service
exists in the tree, `scripts/build-all.sh` configures each project top-level,
and the Dockerfile goes through the orchestrator, so the mode the guard closes
is unreachable — and `HEAD` was already broken in it unless the parent had
pre-found Drogon. The reviewer's expected counter-finding — that the relocated
bootstrap consumes something the guarded block provides — did not survive its
own check and does not survive this one: the bootstrap sets cache variables,
probes CUDA/Vulkan and `add_subdirectory`s a standalone project that needs only
`find_package(Threads)`.

**Noted — `HEAD`-build leftovers in the gitignored build tree.** `libvlm-core.a`
and `CMakeFiles/argus-vlm.dir/src/main.cc.o` still sit in
`services/vlm/build/dev` beside the current outputs; `build.ninja` references
neither. The reviewer's point is that a `find`-based audit can misread them as
double compilation; the compile database settles it — eight first-party
translation units, one entry each, and the five production sources are compiled
once where `HEAD` compiled them twice.

### Documents (eight findings: five acted on, three recorded)

**Acted on — the "old prefixes" sentence was false as written.** It claimed
none of `controllers/vlm`, `vlm/` and `shared/services/vision` appears in any
tracked file under `services/vlm`. Re-measured: the first two appear in 3 and
12 files — as substrings of the *new* spellings (`feature/vlm/controllers/…`),
so a bare prefix test cannot support the sentence — while
`shared/services/vision` is at 0. The paragraph now measures the include form
(`<controllers/vlm`, `<vlm/describe-dto`, `<shared/services/vision`): no match
in the worktree where `HEAD` had nine lines across eight files.

**Acted on — "a submodule rather than a package" is not an addition over
stt.** Re-measured: `third_party/sherpa-onnx` is a gitlink in
`git ls-tree HEAD third_party/` (`160000`), declared in `.gitmodules`, and
`services/stt/CMakeLists.txt:58` adds it as a subdirectory — stt's engine is
vendored the same way. The sentence now names what actually differs: the
seventeen cache variables and the CUDA/Vulkan probe.

**Acted on — the rewritten root line over-claimed for `identity` and
`sync`.** It said those seven services "keep everything inside `feature/`
beside their `app/`". Re-measured per service: `guard`, `stt`, `tts` and `vlm`
hold `app/` and `feature/` only; `auth` adds `src/config/`; `identity` and
`sync` add `src/config/` and `src/shared/`. The sentence now says exactly that.
The loose phrasing was `HEAD`'s ("…keep everything inside `feature/` (plus
`app/` in …)"), so this change inherited it rather than introduced it.

**Acted on — the Files block was short one row and one description.** The
staged set is 22 paths and the block listed 21; the missing one is
`docs/history/plans/architecture-plan.md`, whose step-5 row records 5b. The
`AGENTS.md` row also named three hunks of seven — the llama.cpp bullet this
change had to correct was not among them. Both fixed.

**Acted on — the verification preamble's "index equal to it".** The reviewer
is right that it stopped holding while this report was still being edited
(`AM` in `git status --porcelain`). It is made true by the final `git add`
and re-measured immediately before the commit, not by rewording it away.

**Recorded, hand-off to step 8 — four stale layout lines in three other
services.** `services/llm/AGENTS.md:55` and `services/voice/AGENTS.md:57` list
a `src/server/`, `services/voice/AGENTS.md:56` and
`services/tunnel/AGENTS.md:57` a `src/controllers/`. Re-measured: three of the
four directories do not exist and `git log --all` is empty for them; tunnel's
`src/server/` does exist, so its `src/controllers/` line is the stale one. The
same class of line this unit removed from vlm's own block, but none of them is
this change's file — step 8 is the row that owns them, and `services/llm`'s
block is rewritten by step 5c anyway.

**Recorded, no change — the carried config-inventory lines.** Both
`services/vlm/AGENTS.md:53` and `services/vlm/CONTEXT.md` say `[vision]` plus
`[server]` "only", while `config.toml.example` also carries `[drogon.app]`.
The "no other domains" reading survives (Drogon tuning is not a domain); the
enumeration reading does not. Pre-existing lines this change did not touch.

**Acted on — the file's longest line was the one this change rewrote.**
`services/vlm/CONTEXT.md`'s engine bullet is rewrapped; the longest line in
the file is now 77 characters.

The reviewer also re-ran both pre-build gates on the current tree
(1357/0 and 109/813/0/0/0/23/290, byte-identical to the numbers above),
matched the rename list against `git diff --cached -M --summary` entry for
entry, and reconstructed the `check-deps` delta from the scanner source
instead of a worktree — its figures agree with mine. Four suspicions it
carried did not survive its own checks and are recorded as such: the
`HttpController<…, false>` static-init claim in vlm's `AGENTS.md` (it read the
vendored Drogon header), the remote adapter's absence from the production
binary, the `packages/clients/vlm` spellings (that package's own include
root), and the two findings that this change fixed while the review was
reading it.

## Verification

Everything below was run against the working tree this report describes, with
the index equal to it (`git diff --stat` empty, 22 staged paths, no untracked
file but the ignored ones). Where a number is quoted from a reviewer, it was
re-measured first; the two reviewers ran no build and no gate.

| What | How | Result |
|---|---|---|
| Build, dev profile + tests | `./scripts/build-all.sh dev --only vlm` | exit 0, 0 errors, 0 warnings |
| The suite | `ctest` in `services/vlm/build/dev` | 12/12 passed; `vlm-wire-test` 35.90 s driving the real LFM2.5-VL engine over a loopback socket, `vision-remote-adapter-test` 0.03 s |
| Comments (rule 20) | `./scripts/check-comments.sh` | 1357 files checked, 0 comments (1356 before this unit: the new module declaration) |
| Dependency tiers (rule 25) | `./scripts/check-deps.sh` | 109 declarations, 813 edges, 0 forbidden, 0 cycles, 0 unresolved, 23 edges deferred, 290 third-party mentions over 21 roots — the delta against `HEAD` accounted for exactly by scanning both trees (declarations +2, `services/vlm`'s own edge list 12 → 13, foreign 292 → 290) |
| Modern C++ (rules 16, 19) | `./scripts/check-tidy.sh`, whole tree | 539 TUs, 2901 findings over 45 checks, baseline 2901 — nothing risen, nothing below it, exit 0 |
| Old spellings gone | `git grep 'shared/services/vision\|<controllers/vlm\|<vlm/describe-dto' -- services/vlm` | no match |
| One entry per translation unit | parse of `build/dev/compile_commands.json` | eight first-party `services/vlm` entries, each naming a distinct source, one entry each |
| Wire unchanged | strings and symbols on `build/dev/argus-vlm` | both `/vlm/v1/*` route strings present; 411 `VisionService`/`VlmController` symbols |
| Deploy unchanged | `services/vlm/config.toml.example`, `services/vlm/Dockerfile` | `port = 7031` and `EXPOSE 7031`, matching the module's `PORTS 7031` |
| The gates measured the committed tree | `git diff --stat` before and after them | identical; nothing was written while they ran |

Not verified here: the release profile (`prod`) was not rebuilt — this unit
changed no conditional, but "prod builds clean" is not backed by a run of it.
The tidy scan reads the tree's compile databases, of which `services/vlm`'s was
produced by this unit and every other project's was untouched by it.

## Files

```
M  AGENTS.md                                             the llama.cpp bullet, the two "today" paragraphs, the vlm key-file row (seven hunks)
M  services/vlm/AGENTS.md                                the layout block, the module paragraph
M  services/vlm/CMakeLists.txt                           module graph + argus_service + the two test links
M  services/vlm/CONTEXT.md                               the engine ownership line and the llama targets line
M  services/vlm/tests/unit/vision-remote-adapter-test.cc one include
M  services/vlm/tests/unit/vlm-wire-test.cc              two includes, re-sorted block
A  services/vlm/src/feature/vlm/CMakeLists.txt           argus_module(NAME vlm)
R  services/vlm/src/main.cc                              -> src/app/main.cc
R  services/vlm/src/controllers/vlm-controller.cc        -> src/feature/vlm/controllers/vlm-controller.cc
R  services/vlm/src/controllers/vlm-controller.hxx       -> src/feature/vlm/controllers/vlm-controller.hxx
R  services/vlm/src/controllers/vlm-errors.hxx           -> src/feature/vlm/controllers/vlm-errors.hxx
R  services/vlm/src/vlm/describe-dto.cc                  -> src/feature/vlm/dtos/describe-dto.cc
R  services/vlm/src/vlm/describe-dto.hxx                 -> src/feature/vlm/dtos/describe-dto.hxx
R  services/vlm/src/shared/services/vision/remote/remote-vision-adapter.cc  -> src/feature/vlm/services/remote/remote-vision-adapter.cc
R  services/vlm/src/shared/services/vision/remote/remote-vision-adapter.hxx -> src/feature/vlm/services/remote/remote-vision-adapter.hxx
R  services/vlm/src/shared/services/vision/remote/vlm-remote.cc             -> src/feature/vlm/services/remote/vlm-remote.cc
R  services/vlm/src/shared/services/vision/remote/vlm-remote.hxx            -> src/feature/vlm/services/remote/vlm-remote.hxx
R  services/vlm/src/shared/services/vision/vision-hash.hxx     -> src/feature/vlm/services/vision-hash.hxx
R  services/vlm/src/shared/services/vision/vision-service.cc   -> src/feature/vlm/services/vision-service.cc
R  services/vlm/src/shared/services/vision/vision-service.hxx  -> src/feature/vlm/services/vision-service.hxx
A  docs/history/reports/f4-5-2-vlm-layout.md             this report
M  docs/history/plans/architecture-plan.md              the step 5 row, now two of three services
```
