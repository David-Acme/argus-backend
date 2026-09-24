# Phase 4 step 5 — argus-stt gets the reference shape

`services/stt` is the first of the three legacy horizontal services step 5
names — `stt`, `vlm`, `llm` — to take the shape every other service already
carries: `src/app/` for composition, `feature/<feature>/` for the vertical
slice, and a build in which the folder that owns a translation unit declares
itself (rule 25). It is the smallest of the three: six source files, one
controller, one engine facade, no database and no gRPC leg. The move is three
directories and one module declaration, and what it settles is the pattern the
other two follow.

## What existed before

- `src/main.cc` (58 lines): config load, `ListenerConfig::resolve(7030)`, the
  health and domain `registerController` calls, the engine boot gate
  (`LOG_FATAL` + `return 1` when sherpa-onnx fails to load), `app().run()`, then
  `SttService::instance().shutdown()`.
- `src/controllers/`: `stt-controller.{hxx,cc}` — a
  `drogon::HttpController<SttController, false>` with two routes,
  `/stt/v1/transcribe` (POST) and `/stt/v1/config` (GET) — and `stt-errors.hxx`
  with three catalog entries.
- `src/shared/services/stt/stt-service.{hxx,cc}` (64 + 257 lines): the
  sherpa-onnx facade, a singleton reached as `SttService::instance()` from both
  the controller and `main.cc`.
- the root build: `add_library(stt-core STATIC
  ${STT_SRC_ROOT}/controllers/stt-controller.cc
  ${SHARED_ROOT}/services/stt/stt-service.cc)` with `third_party` and
  `third_party/sherpa-onnx` as SYSTEM PUBLIC include directories, `src` as the
  PUBLIC include root, eight PUBLIC link libraries (config, runtime, http,
  clients::stt, Drogon, onnxruntime, nlohmann_json, sherpa-onnx-c-api) and
  `-Wall -Wextra`; a hand-written `add_executable(argus-stt src/main.cc)`
  linking that archive, with hand-written `$ORIGIN` rpath properties and a
  POST_BUILD `models` symlink; and `stt-wire-test` compiling the controller and
  the engine a second time by listing their `.cc` files itself, with an include
  path (`packages/clients/stt/tests/support`) it never used.
- No `src/server/`. The service's `AGENTS.md` layout block named one;
  `git log --all -- services/stt/src/server` is empty, so the line described a
  directory that never existed in this service's history.

## The move

Six renames, one new file, and five include lines rewritten into five:

```
rename services/stt/src/{ => app}/main.cc (93%)
rename services/stt/src/{ => feature/stt}/controllers/stt-controller.cc (98%)
rename services/stt/src/{ => feature/stt}/controllers/stt-controller.hxx (100%)
rename services/stt/src/{ => feature/stt}/controllers/stt-errors.hxx (100%)
rename services/stt/src/{shared/services/stt => feature/stt/services}/stt-service.cc (100%)
rename services/stt/src/{shared/services/stt => feature/stt/services}/stt-service.hxx (100%)
```

`<controllers/stt-controller.hxx>` became
`<feature/stt/controllers/stt-controller.hxx>` twice (`src/app/main.cc` and the
wire test) and `<shared/services/stt/stt-service.hxx>` became
`<feature/stt/services/stt-service.hxx>` three times (`src/app/main.cc`,
`stt-controller.cc`, the wire test). Those rewrites are the entire content
change. `src/app/main.cc` had its two include lines rewritten in place, in the
order `HEAD` already had them (`<drogon/drogon.h>` first,
`<config/config-service.hxx>` last — the grouping the neighbouring services'
`main.cc` carry), which is its 93% similarity; `stt-controller.cc`'s single
rewritten line is its 98%. The wire test is the only file whose include block
was re-sorted — three lines, so the block reads `config/`, `drogon/`,
`feature/`, `http/`. `stt-service.hxx`, `stt-service.cc` and `stt-errors.hxx`
carry no first-party include at all, so they moved byte for byte.

The include root did not move: the module publishes `src/` exactly as the
archive did (`INCLUDES ../..`), so the new spellings are the only ones that
changed and nothing outside `services/stt` was touched by them. Measured: the
only remaining mention of the old service-local paths anywhere in the tree
outside `docs/history/` is the sentence in `packages/clients/stt/AGENTS.md`
that records where the *client's* header came from — a different move, in a
different package, and still true.

## The build, converted

`src/feature/stt/CMakeLists.txt` is new and declares the one module:

```cmake
argus_module(NAME stt
    SOURCES
        controllers/stt-controller.cc
        services/stt-service.cc
    INCLUDES
        ../..
    DEPENDS
        argus::lib::config
        argus::lib::runtime
        argus::lib::http
        argus::clients::stt
        Drogon::Drogon
        onnxruntime::onnxruntime
        nlohmann_json::nlohmann_json
        sherpa-onnx-c-api)
```

plus the two SYSTEM PUBLIC include directories the archive carried
(`third_party`, `third_party/sherpa-onnx`) and `-Wall -Wextra`, which
`argus_module` does not add.

The root file discovers it (`file(GLOB STT_FEATURE_MODULES CONFIGURE_DEPENDS
"${STT_SRC_ROOT}/feature/*/CMakeLists.txt")`, then `add_subdirectory` per
directory) and composes the executable through the helper:

```cmake
argus_service(NAME argus-stt
    MAIN ${STT_SRC_ROOT}/app/main.cc
    MODULES
        argus::stt
        argus::lib::config
        argus::lib::runtime
        argus::lib::http
        argus::clients::stt
    PORTS 7030)
```

`argus_service` (`cmake/argus-module.cmake:411-427`) is what makes the
hand-written properties redundant: it applies `-Wall -Wextra`, sets
`BUILD_RPATH`/`INSTALL_RPATH` to `$ORIGIN`, calls `argus_runtime_rpath` and
records `ARGUS_PORTS` (which nothing in the tree reads — pre-existing, recorded
in the plan at `architecture-plan.md:876`). What no helper supplies stays by
hand and is unchanged — the ten `SHERPA_ONNX_*` cache variables plus
`json_POPULATED`/`BUILD_SHARED_LIBS` and the `onnxruntime` INTERFACE IMPORTED
shim that make sherpa-onnx build against Conan's onnxruntime, the doctest
`INTERFACE_SYSTEM_INCLUDE_DIRECTORIES` treatment, and the POST_BUILD `models`
symlink next to the binary.

That bootstrap block moved inside the top-level guard, which is the review's
one substantive repair. The archive's guard was `if(NOT TARGET Drogon::Drogon)`
— "the parent supplied the graph, use it" — and the block, consuming the
`onnxruntime::onnxruntime` that the guarded `find_package` provides, sat
outside it. Adopting guard's `PROJECT_IS_TOP_LEVEL` without moving the block
would have paired a conditional provider with an unconditional consumer: a
nested configure skips both `find_package`s and then dies inside
`get_target_property()`. The reference settles it — `services/guard` puts its
`third_party` `add_subdirectory` inside its guard — so a nested configure now
requires the parent to supply `sherpa-onnx-c-api` by the name the module's
`DEPENDS` already spells, and fails at that link instead of inside a property
read. No in-repo path nests this file (`services/stt` has no parent
`CMakeLists.txt` and no root `CMakeLists.txt` exists), and the standalone
configure is unaffected — measured by rebuilding it. The block's dead last line,
`set(ARGUS_SHERPA_ONNX_TARGET sherpa-onnx-c-api)`, is gone: its only reader was
the test link line this change replaced.

Ownership, measured: `find services/stt/src services/stt/tests -name '*.cc'`
is four first-party translation units, each of the two production sources is
named by exactly one module, and `src/app/main.cc` is named by no `SOURCES`
list — it is the service's `MAIN`.

The wire test now links the module instead of re-listing it:

```cmake
target_link_libraries(stt-wire-test PRIVATE argus::stt doctest::doctest)
```

It keeps its `ARGUS_TEST_STT_MODELS_DIR` definition and its
`EXCLUDE_FROM_ALL FALSE`; the include root, the sherpa include directory and
sherpa-onnx itself now travel with the module. Its
`packages/clients/stt/tests/support` include path is gone — the package's own
`AGENTS.md` documented it as "carries the path but includes nothing from it",
and the build proves it (the suite compiles and passes without it). That
sentence was corrected in the same change, which is where the count of CMake
files putting that path on an include path fell from three to two.

## The wire, unchanged

Rule 1 of the service's own `AGENTS.md` is that the engine is the only
capacity, and rule 3 that the envelope is frozen. Measured on the dev binary
the change produced: both route strings are still registered
(`/stt/v1/config`, `/stt/v1/transcribe`), `nm -C` finds 308 `SttController` /
`SttService` symbols in `argus-stt`, and `stt-wire-test` — which drives the real
engine over a loopback socket and asserts the envelope, the 400/422/503/404/405
refusals, the language switch, the empty-body and misaligned-PCM cases and the
post-`shutdown()` 503 — passes in 17.15 s.

## What the gates measured

- `check-comments`: 1356 files checked, 0 comments (1355 before: the new module
  declaration is one more file to scan).
- `check-deps`: 107 declarations, 812 edges, 0 forbidden, 0 cycles,
  0 unresolved, 23 edges deferred (105/810 before). The delta is accounted for
  exactly: `argus_module(NAME stt)` (+1 declaration) contributes four edges
  (config, runtime, http, clients::stt); `argus_service(NAME argus-stt)`
  (+1 declaration) contributes one (`argus::stt`); the wire test's link line
  contributes one more; and the four edges the deleted `stt-core` archive
  carried are gone. Third-party mentions fell 294 → 292 over 22 → 21 roots
  because the same four third-party targets were named in three places before
  and are named in one now.
- `./scripts/build-all.sh dev --only stt`: 0 errors, 0 warnings, ctest 11/11.

## Review

Two read-only reviewers ran against the change — one on the build's semantics,
one on the documents the change writes. Every finding below was re-measured
here before anything was done about it; a reviewer's report is a claim, not
evidence.

Repaired:

1. The sherpa-onnx bootstrap sat outside the top-level guard this change
   introduced while consuming the `onnxruntime::onnxruntime` that the guarded
   `find_package` provides. The block moved inside the guard; `services/guard`
   is what settles the direction, since its `third_party` `add_subdirectory`
   lives inside its own guard (see "The build, converted").
2. `set(ARGUS_SHERPA_ONNX_TARGET sherpa-onnx-c-api)` lost its only reader in
   this change — the test's old link line — and was still being set. Removed,
   per rule 23's clause that dead code goes in the change that replaces it.
3. The service's layout block claimed `controllers/ (health + /stt/v1/* wire)`.
   Measured, the folder holds `stt-controller.{hxx,cc}` and `stt-errors.hxx`;
   the health controller is `packages/lib/http`'s, registered from
   `src/app/main.cc`. The line now names the frozen wire alone.
4. The client package's `AGENTS.md` counted "four link lines in three
   CMakeLists" with `stt-wire-test` among the linkers. Measured after the move,
   `argus::clients::stt` appears in one link line each in
   `services/stt/CMakeLists.txt:79`,
   `services/stt/src/feature/stt/CMakeLists.txt:11`,
   `services/voice/CMakeLists.txt:92` and
   `services/camera/src/feature/actions/CMakeLists.txt:15` — four lines in four
   CMakeLists, three of which also add the package by path. The wire test links
   `argus::stt` now, not the package. Both sentences corrected.
5. This report claimed `main.cc`'s include block had been re-sorted. It had
   not; corrected above.

Re-measured and rejected:

- "`PORTS 7030` decorates nothing and no deploy mechanism reads it" — true and
  pre-existing: `ARGUS_PORTS` has no reader anywhere in the tree, which the
  plan already records (`architecture-plan.md:876`), and the value itself
  matches `services/stt/config.toml.example`, `src/app/main.cc:29` and the
  deploy compose. Not a defect of this change, not repaired here.
- "The file is no longer consumable by a parent that supplies nothing" — that
  is the guard's intent, not a defect: nesting was already broken before the
  move (the archive's `if(NOT TARGET Drogon::Drogon)` skipped the
  `find_package`s and the block then read a target nobody had provided), the
  reachable configure paths are all top-level (`scripts/build-all.sh` does
  `cd` + `cmake -S . -B build/<profile>`, no root `CMakeLists.txt` exists, and
  no in-tree `CMakeLists.txt` adds `services/stt`), and the service's
  `AGENTS.md` now says "standalone buildable" rather than inheriting the
  neighbouring services' stale "add_subdirectory-compatible".
- `argus_service` *adds* rpath entries the hand-written executable never had —
  reproduced with `readelf -d services/stt/build/dev/argus-stt` (`RUNPATH`
  beginning `$ORIGIN`, then the Conan library directories), and
  `argus_runtime_rpath` appended nothing because `ARGUS_RPATH_DIRS` is empty.
  Behaviour-neutral, recorded rather than fixed.

Reproduced as clean, each re-run here rather than taken on trust: the rename
percentages; the module carrying the archive's eight dependencies, both SYSTEM
include roots, `INCLUDES ../..` and `-Wall -Wextra`; the test's compiled
include closure (the module's include root, both third-party roots,
onnxruntime) in the regenerated `compile_commands.json`; the four
`fake-stt-server.hxx` includers and the two CMakeLists that keep the client's
`tests/support` on an include path; the root `AGENTS.md` paragraphs' counts (9
services with `src/app/`, 3 with a `main.cc` at `src/`, 3 without `feature/`,
10 with it, 4 keeping code beside it, 6 keeping everything inside, no
`feature/api` anywhere); `git log --all -- services/stt/src/server` empty; no
`stt-core` or old-path reference left in any tracked file; and one remaining
mention of an old service-local path, in the client's `AGENTS.md`, which
records the *client* header's own earlier move and is still true.

## Verification

- `./scripts/build-all.sh dev --only stt` after the review repairs: exit 0,
  0 errors, 0 warnings, `100% tests passed, 0 tests failed out of 11`,
  `stt-wire-test` 20.84 s.
- `check-comments`: 1356 files checked, 0 comments.
- `check-deps`: 107 declarations, 812 edges, 0 forbidden, 0 cycles,
  0 unresolved, 23 edges deferred (292 third-party mentions over 21 roots).
- whole-tree `scripts/check-tidy.sh` (run by the orchestrator's full pass for
  the previous unit, re-run here): `539 TUs, 2901 findings over 45 checks,
  baseline 2901`, exit 0, worst file
  `services/guard/src/feature/guard/guard-repository.cc` (104 findings). No
  baseline line moved. The TU count is a floor of 537 and it stays at 539: the
  scan keys translation units by source path, so the test's second compilation
  of the two production sources never counted twice and linking the module
  instead of re-listing them changes nothing. The CMake repair that followed
  the tidy run touches no source and no translation unit, so the scan's result
  still stands for it.
- the wire itself, re-measured on the repaired binary: both route strings
  (`/stt/v1/config`, `/stt/v1/transcribe`) and 308 `SttController`/`SttService`
  symbols in `services/stt/build/dev/argus-stt`.

## Files

```
M  AGENTS.md                                             the two "today" paragraphs and the stt key-file row
M  packages/clients/stt/AGENTS.md                        the include-path sentence and the consumer list
M  services/stt/AGENTS.md                                the layout block, corrected
M  services/stt/CMakeLists.txt                           module graph + argus_service + the test link
M  services/stt/CONTEXT.md                               "the stt feature's own facade"
A  services/stt/src/feature/stt/CMakeLists.txt           argus_module(NAME stt)
R  services/stt/src/main.cc                     ->  src/app/main.cc
R  services/stt/src/controllers/stt-controller.cc -> src/feature/stt/controllers/stt-controller.cc
R  services/stt/src/controllers/stt-controller.hxx -> src/feature/stt/controllers/stt-controller.hxx
R  services/stt/src/controllers/stt-errors.hxx -> src/feature/stt/controllers/stt-errors.hxx
R  services/stt/src/shared/services/stt/stt-service.cc -> src/feature/stt/services/stt-service.cc
R  services/stt/src/shared/services/stt/stt-service.hxx -> src/feature/stt/services/stt-service.hxx
M  services/stt/tests/unit/stt-wire-test.cc              two includes, re-sorted block
A  docs/history/reports/f4-5-1-stt-layout.md             this report
```
