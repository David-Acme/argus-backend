# Phase 4 step 5 — argus-llm gets the reference shape

`services/llm` is the third and last of the legacy horizontal services step 5
names — `stt`, `vlm`, `llm` — and the heaviest: it was the only one of the
three with no `feature/` at all, it carried the three spellings `services/vlm`
had just shed (`src/controllers/`, `src/llm/`, `src/shared/services/`),
the only one with a second capability beside its engine (the encounter-closed
consumer, which the build already kept in an archive of its own), and the only
one whose engine facade has **no header of its own** — `LlmService`'s header is
the tier-3 client's `packages/clients/llm/src/llm/llm-service.hxx`, so
`services/llm` implements an interface it does not declare. It ends the step on
the shape `services/stt` settled and `services/vlm` repeated: `src/app/` for
composition, `feature/<feature>/` for the vertical slice, and a build in which
the folder that owns a translation unit declares itself (rule 25).

## What existed before

- `src/main.cc` (292 lines): config load, `ListenerConfig::resolve(7032)`, the
  health and `LlmController` `registerController` calls,
  `llama_backend_init()`, the engine boot gate (`llm->initEngine()`, then
  `LOG_FATAL` + `llama_backend_free()` + `return 1` when `isEngineLoaded()` is
  false), the memory stack boot gate (`MemoryService::init({.deferStore =
  true})`, same abort shape), memory tool-registry registration, the optional
  `NatsBus` + `CatalogReplica` pair, the optional `EncounterClosedConsumer`
  wiring (owner resolution through `IdentityClient::listNotifiableUsers`, the
  `EncounterCaptureInput` → `observeSystemEvent` capture lambda), the
  beginning-advice that starts the consumer and seeds the catalog snapshot
  through `BlockingTask<CatalogReplica::Snapshot>`, `app().run()`, then the
  teardown in the order the service depends on (replica, consumer, memory
  shutdown, engine shutdown, `llama_backend_free()`).
- `src/controllers/`: `llm-controller.{hxx,cc}` (40 + 279 lines) — a
  `drogon::HttpController<LlmController, false>` with the three `/llm/v1/*`
  legs — and `llm-errors.hxx` (16 lines) with the LLM catalog entries.
- `src/llm/chat-dto.{hxx,cc}` (30 + 96 lines): the chat request DTO on the
  validation DSL.
- `src/shared/services/llm/`: `llm-service.cc` (573 lines) — the engine, owned
  BY VALUE by the controller — `lfm-adapter.{hxx,cc}` (88 + 616 lines, the
  tool-calling loop) and `intent-gate.{hxx,cc}` (21 + 44 lines, the fast
  classifier tier). Five files, no `llm-service.hxx`.
- `src/shared/services/tools/`: `tool-registry.{hxx,cc}`, `tool-validator.{hxx,
  cc}` and `tool-executor.{hxx,cc}` — the tool runtime the adapter drives.
- `src/shared/services/encounter-closed/encounter-closed-consumer.{hxx,cc}`
  (90 + 269 lines): the durable JetStream consumer, already a target of its own
  (`encounter-consumer`) because the executable and the two consumer suites
  link it.
- the root build: two hand-written archives — `llm-core` over eight sources
  with `third_party` as a SYSTEM PUBLIC include root, `src` as the PUBLIC
  include root, fourteen PUBLIC link libraries and `-Wall -Wextra`, and
  `encounter-consumer` over one source with three link libraries — plus a
  hand-written `add_executable(argus-llm src/main.cc)` linking `llm-core
  memory-catalog encounter-consumer argus::clients::identity
  argus::clients::camera`, hand-written `$ORIGIN` rpath properties, a
  POST_BUILD `models` symlink and a `doctest` SYSTEM-include fixup. Six
  targets (bench + five tests) each repeated `-Wall -Wextra` and
  `EXCLUDE_FROM_ALL FALSE`; `llm-wire-test` re-listed all eight production
  sources and carried thirteen link entries, one of them
  `${ARGUS_LLAMA_TARGETS}`; the two consumer tests re-declared the memory
  schema path.
- `find_package(doctest REQUIRED)` twice, `set(ARGUS_LLAMA_TARGETS llama)` once
  (read once, by that same wire test), and two bootstrap blocks — llama.cpp
  (`:25-58`) outside the `if(NOT TARGET Drogon::Drogon)` guard (`:60-117`) that
  held the dependency graph.
- `src/server/` in the service's `AGENTS.md` layout block, as in `stt` and
  `vlm`: the listener is `ListenerConfig::resolve(7032)` from `argus::lib::http`
  and `git log --all -- services/llm/src/server` is empty.

## The move

Nineteen renames: sixteen into the `llm` feature, two into
`encounter-closed`, and `main.cc` into `src/app/`:

```
rename services/llm/src/{ => app}/main.cc (98%)
rename services/llm/src/{ => feature/llm}/controllers/llm-controller.cc (98%)
rename services/llm/src/{ => feature/llm}/controllers/llm-controller.hxx (92%)
rename services/llm/src/{ => feature/llm}/controllers/llm-errors.hxx (100%)
rename services/llm/src/{llm => feature/llm/dtos}/chat-dto.cc (100%)
rename services/llm/src/{llm => feature/llm/dtos}/chat-dto.hxx (100%)
rename services/llm/src/{shared/services/llm => feature/llm/services}/intent-gate.cc (100%)
rename services/llm/src/{shared/services/llm => feature/llm/services}/intent-gate.hxx (100%)
rename services/llm/src/{shared/services/llm => feature/llm/services}/lfm-adapter.cc (100%)
rename services/llm/src/{shared/services/llm => feature/llm/services}/lfm-adapter.hxx (97%)
rename services/llm/src/{shared/services/llm => feature/llm/services}/llm-service.cc (100%)
rename services/llm/src/{shared => feature/llm}/services/tools/tool-executor.cc (93%)
rename services/llm/src/{shared => feature/llm}/services/tools/tool-executor.hxx (85%)
rename services/llm/src/{shared => feature/llm}/services/tools/tool-registry.cc (100%)
rename services/llm/src/{shared => feature/llm}/services/tools/tool-registry.hxx (100%)
rename services/llm/src/{shared => feature/llm}/services/tools/tool-validator.cc (100%)
rename services/llm/src/{shared => feature/llm}/services/tools/tool-validator.hxx (100%)
rename services/llm/src/{shared/services/encounter-closed => feature/encounter-closed/services}/encounter-closed-consumer.cc (100%)
rename services/llm/src/{shared/services/encounter-closed => feature/encounter-closed/services}/encounter-closed-consumer.hxx (100%)
```

The tool runtime stays inside the `llm` feature rather than becoming a third
one: rule 23's 2+ rule is about a second *feature* reading a unit, and nothing
but the brain reads `ToolRegistry`/`ToolExecutor`/`validateArguments` — the
service's own `AGENTS.md` rule 4 says exactly that ("Tool loop stays here").
`src/shared/` therefore does not exist in this service now: the 2+ rule never
earned it, and `encounter-closed` — the one folder that could have argued for
it — became a feature of its own, which is what its status as a separate target
already said.

Twenty include lines in twelve files carried a service-local path and all
twenty were rewritten: `<controllers/llm-controller.hxx>` became
`<feature/llm/controllers/llm-controller.hxx>` (twice: `src/app/main.cc` and
the wire test), `<llm/chat-dto.hxx>` became `<feature/llm/dtos/chat-dto.hxx>`,
`<shared/services/llm/{lfm-adapter,intent-gate}.hxx>` became
`<feature/llm/services/…>` (five times), `<shared/services/tools/tool-{registry,
executor,validator}.hxx>` became `<feature/llm/services/tools/…>` (nine
times), and `<shared/services/encounter-closed/encounter-closed-consumer.hxx>`
became `<feature/encounter-closed/services/encounter-closed-consumer.hxx>`
(three times). Measured after the rewrite, on the include form itself:
`grep -rn '<controllers/\|<shared/services/llm/\|<shared/services/tools/\|
<shared/services/encounter-closed/\|<llm/chat-dto'` under `services/llm`
returns nothing, where `HEAD` matched twenty lines across those twelve files.

Ten include lines were **not** rewritten because they are not this service's,
and every one of them was verified to be a name another unit owns:

- `<llm/llm-service.hxx>` and `<llm/tool-contracts.hxx>` (ten lines across
  nine files) resolve through `argus::clients::llm`'s include root
  (`packages/clients/llm/src`) — the tier-3 client that owns the engine
  interface and the tool vocabulary. Plan §9.3 keeps `tool-contracts.hxx`
  there until Phase 4 step 7 relocates the memory host, and the module names
  `argus::clients::llm` in its `DEPENDS` so the spelling keeps resolving.
- `<shared/services/{memory,extract,intent}/…>` and
  `<shared/repositories/memory-graph/…>` (sixteen lines) resolve through
  `memory-core`'s and `argus::intent`'s own include roots — `packages/memory`
  and `packages/intent`, both hosted by this binary and both moving into
  `services/llm` at step 7, not now. The rewrite was scripted with exact
  replacements and asserted per pair, so no line that merely *looked* local
  (`shared/…` under a package root) was touched.

The include root did not move: both modules publish `src/` exactly as the two
archives did (`INCLUDES ../..`), so nothing outside `services/llm` changed —
the three `/llm/v1/*` legs, the config keys and the port are untouched, and
the client package's four headers are the same files they were.

## The build, converted

`src/feature/llm/CMakeLists.txt` and `src/feature/encounter-closed/
CMakeLists.txt` are new and declare one module each:

```cmake
argus_module(NAME llm
    SOURCES controllers/llm-controller.cc dtos/chat-dto.cc
            services/intent-gate.cc services/lfm-adapter.cc
            services/llm-service.cc
            services/tools/tool-executor.cc services/tools/tool-registry.cc
            services/tools/tool-validator.cc
    INCLUDES ../..
    DEPENDS argus::lib::config argus::lib::auth argus::hardware-profile
            argus::lib::phrase argus::lib::runtime argus::lib::validation
            argus::lib::http argus::clients::llm argus::intent memory-core
            Drogon::Drogon nlohmann_json::nlohmann_json
            argus::contracts::auth llama)

argus_module(NAME encounter-closed
    SOURCES services/encounter-closed-consumer.cc
    INCLUDES ../..
    DEPENDS argus::lib::text argus::lib::nats memory-core)
```

The two archives' fourteen and three link libraries are carried over
one-to-one (`llm` names `llama` itself, which is where
`${ARGUS_LLAMA_TARGETS}` used to deliver it), each with the `-Wall -Wextra`
gate the archives already had, and `llm` keeps the SYSTEM `third_party`
include root the archive republished. The executable goes through
`argus_service`:

```cmake
argus_service(NAME argus-llm
    MAIN ${LLM_SRC_ROOT}/app/main.cc
    MODULES argus::llm argus::encounter-closed argus::lib::config
            argus::lib::runtime argus::lib::text argus::lib::nats
            argus::lib::sqlite argus::lib::http argus::clients::identity
            argus::clients::camera memory-core memory-catalog Drogon::Drogon
    PORTS 7032)
```

`PORTS 7032` is the number `config.toml.example` and the Dockerfile's `EXPOSE`
already carried, and the property is the one stt and vlm set — nothing reads it
yet (plan `:876`), which is exactly why the three services now agree on where
it belongs. The `$ORIGIN` rpath pair the hand-written target set is what
`argus_service` sets, the POST_BUILD `models` symlink stays after it, and both
tests keep their `EXCLUDE_FROM_ALL FALSE`.

The six test and bench targets now link the module instead of re-listing
production sources — `llm-wire-test`'s eight-source list and its thirteen link
entries collapse to `argus::llm doctest::doctest`, which is where
`${ARGUS_LLAMA_TARGETS}` and its `set()` both disappear: no build file sets or
reads the variable now (the name survives only in the documents, this report
included). Two things went with their last reader for
the same reason: the second `find_package(doctest REQUIRED)` (the file had
one before the test block and one inside it) and the `if(NOT TARGET
Drogon::Drogon)` wrapper, whose job the top-level guard now does. The llama.cpp
bootstrap and the whole dependency graph sit inside one
`if(PROJECT_IS_TOP_LEVEL)`, on the same measured ground as vlm: the bootstrap
sets cache variables, probes CUDA/Vulkan and `add_subdirectory`s a standalone
project that needs only `find_package(Threads)`, consuming nothing the guarded
`find_package` calls provide.

## The wire, unchanged

The three legs are `/llm/v1/chat` (POST), `/llm/v1/chat-stream` (POST, chunked
with the `{done:true,…}` sentinel) and `/llm/v1/config` (GET). In
`services/llm/build/dev/argus-llm` — the dev binary, 983 MB, unstripped —
`nm -C` counts 371 `LlmController`, 170 `LlmService`, 55 `LfmAdapter`, 17
`ToolRegistry`, 4 `ToolExecutor`, 6 `IntentGate` and 165
`EncounterClosedConsumer` symbols, and the five tests below drive the surface
itself: `llm-wire-test` speaks HTTP over a loopback socket to the real engine.

## What the gates measured

| Gate | Result |
|------|--------|
| `./scripts/build-all.sh dev --only llm` | exit 0, **0 errors, 0 warnings** (full log captured: zero `warning:` lines in 27 compile/link steps) |
| `ctest` (in that run) | **41/41 passed**; `llm-wire-test` 44.30 s against the real engine, `llm-tool-parse-test` 0.03 s, `llm-tool-runtime-test` 0.02 s, `encounter-closed-consumer-test` 0.03 s, `encounter-closed-live-test` 0.01 s |
| `scripts/check-comments.sh` | 1359 files checked, **0 comments** (+2 files: the two new module `CMakeLists.txt`) |
| `scripts/check-deps.sh` | 112 declarations, 816 edges, **0 forbidden, 0 cycles, 0 unresolved**, 22 edges deferred to phase 3 (290 third-party mentions over 20 roots) |
| `scripts/check-tidy.sh` (whole tree) | 539 TUs, 2901 findings over 45 checks, **at the recorded baseline** (2901), exit 0 |

The check-deps delta against `HEAD` was accounted by running the scanner's own
`scan()` over both trees:

- **declarations 109 → 112 (+3)**: `services/llm` had **zero** `argus_*`
  declarations before (nine literal `add_library`/`add_executable` calls, six
  of which are still literal for the test and bench targets) and
  has three now — the two `argus_module` and the one `argus_service`, each
  attributed to its own file. The literal calls the helper replaces are not
  counted, which is why the count moves by the number of helpers, not by the
  number of targets.
- **edges 813 → 816 (+3)**: per unit, `services/llm` 34 → 23, plus 11 in
  `feature/llm` and 3 in `feature/encounter-closed` (37 total). The +3 is the
  two archives' link lists moving into module `DEPENDS` plus the service's
  `MODULES` list, which is one edge set where `HEAD` had the executable's link
  line plus the same dependencies repeated through the archives.
- **third-party mentions 290 → 290 over 21 → 20 roots**: exactly two names
  moved — `${ARGUS_LLAMA_TARGETS}` 1 → 0 (the variable's last spelling, in the
  wire test's link line) and `Drogon` 89 → 90 (the service's `MODULES` list
  names it where `HEAD` reached it through `llm-core`). One name out, one name
  in, so the mention count holds and the root count loses the variable.
- **deferred 23 → 22 records over 17 → 19 triples**, all of it `services/llm`:
  `HEAD` declared `services/llm → argus::intent` twice and `→ memory-core`
  five times (the archive, the consumer target and the three consumer tests),
  which the module split breaks up — `feature/llm → argus::intent` once,
  `feature/llm → memory-core` once, `feature/encounter-closed → memory-core`
  once, `services/llm → memory-core` three times (service + two consumers).
  Same edges, one fewer duplicate record, two more declaring units.

## Review

Two adversarial reviewers read the change: one on build semantics, one on the
documents. Their reports are claims, not evidence — every finding below was
re-measured here before it was acted on.

### Build semantics (one finding acted on, one recorded)

The reviewer derived every link set from `HEAD`'s `CMakeLists.txt` against the
generated `build.ninja` and reported no edge, include root, flag, rpath
property or compile definition lost or narrowed: the two archives' fourteen
and three link libraries carried one-to-one into the modules' `DEPENDS`;
`llm-wire-test`'s thirteen link entries and its `${ARGUS_LLAMA_TARGETS}` now
arrive through `argus::llm` (its link line carries `libargus_llm.a`,
`libllama.so`, `libmemory-core.a`, `libdrogon.a`), and
`llm-tool-runtime-test`'s `lib::auth` the same way; the SYSTEM `third_party`
root is republished literally and visible as `-isystem` on every production
TU; `-Wall -Wextra` is on both modules and all seven non-module targets; the
three compile definitions, the `$ORIGIN` RUNPATH, the `models` symlink and the
five ctest registrations are unchanged; and nothing outside the project
configures it (there is no repo-root `CMakeLists.txt`, and no
`add_subdirectory` in the tree names `services/llm`). It reproduced both
scanners' figures exactly.

**Acted on — the client package's consumer list named a deleted target.**
`packages/clients/llm/AGENTS.md:10-13` still read "`services/llm` (`llm-core`,
`llm-wire-test`): six link lines in five CMakeLists". Re-measured: the link
lines naming `argus::clients::llm` are five now — guard's executable, guard's
feature module, `services/llm`'s own feature module, voice and memory — over
five CMakeLists, and the four by-path `add_subdirectory` lines are unchanged,
so the count moved by exactly the wire test's entry. Corrected to
"(`argus::llm`) … five link lines in five CMakeLists".

**Recorded, no change — a nested configure of this project is now
unconfigurable.** The guard is `if(PROJECT_IS_TOP_LEVEL)` where `HEAD` had
`if(NOT TARGET Drogon::Drogon)`, so a hypothetical subproject configure would
add neither the dependency graph nor the llama.cpp bootstrap, and `argus::llm`'s
`DEPENDS … llama` would fail at generate time. Measured: nothing reaches that
mode — `build-all.sh:113` runs `cmake -S . -B build/$PROFILE` from inside each
project, so `project(argus-llm …)` is always top-level, and the two reference
services already guard the same way. Same decision and same evidence as stt's
and vlm's.

### Documents (eleven findings: two stale paths, eight figures, one wording)

**Acted on — two stale references this unit created.** No document outside
the service other than the root contract and the plan row was expected to
move; the review found three that did. `packages/clients/camera/AGENTS.md:35`
and `:68` still said `services/llm/src/main.cc` for the file this unit moved
to `src/app/main.cc` — the cited `:65` was re-read in the new file and still
lands on the `cameraTarget` read, so only the path moved. Fixed, then swept:
`grep -rn 'services/llm/src/' --include=*.md` over the tree returns nothing
stale outside `docs/history/`'s dated records. The third was the client
package above.

**Acted on — eight figures, each re-measured before the correction.** The
rename split is 16 into `feature/llm`, 1 into `src/app/` and 2 into
`feature/encounter-closed` (re-derived from `git diff --cached -M
--name-status`), not 17 and 2; the two rewritten include families are five and
nine lines, not six and eight (`grep -F` per prefix over `HEAD`'s 25 tracked
llm files: 5 + 9 + 3 + 2 + 1 = 20); the untouched client includes are ten
lines across **nine** files, not seven; `HEAD` carried **nine** literal
`add_library`/`add_executable` calls, six of which are still literal for the
test targets; the three-spellings superlative is false — `services/vlm` at
`HEAD^` held `controllers/`, `vlm/` and `shared/services/vision/` beside a
root `main.cc`, so the sentence now says llm "carried the three spellings
`services/vlm` had just shed" (the `feature/`-less superlative survives:
`git ls-tree HEAD services/{stt,vlm}/src` shows both already feature-shaped);
`encounter-consumer` was linked by the executable **and** the two consumer
suites; the root contract's diff is three hunks, not four; and "the variable
no longer exists anywhere in the tree" was literally false — no build file
sets or reads it, but the documents do, this report included, and the plan row
carried the same overstatement and was corrected with it.

**Acted on — one wording.** The scanners were said to run "on the staged
tree"; `scripts/lib/comment_scan.py:869` lists index + untracked paths with
`git ls-files --cached --others --exclude-standard` and reads them from disk,
so the Verification row now says the working tree. No number changes.

**Recorded for step 8, not fixed — a line citation outside this unit.**
`docs/history/plans/architecture-plan.md`'s row 2 cites
`cmake/argus-module.cmake:527` for the `ARGUS_PORTS` setter; the line is
`:425` now. The claim it supports (nothing consumes the property) still holds
— `grep -rn ARGUS_PORTS cmake/ scripts/ argus-deploy/` finds only that line —
so this is a stale citation in another row's text, handed to the step that
owns the documents rather than edited here.

## Verification

| Claim | How it was measured |
|-------|--------------------|
| 19 renames, the layout above | `git diff --cached --summary -M services/llm` (16 into `feature/llm`, 1 into `src/app/`, 2 into `feature/encounter-closed`; similarities 85–100%) |
| 20 include lines rewritten in 12 files | scripted exact replacements, one assertion per pair; then `grep` for the five old prefixes over `services/llm` (0 hits vs 20 at `HEAD`) |
| 10 client/package include lines untouched | the same `grep` counts them by name and the client's four headers are unchanged in `git status` |
| no `src/{controllers,llm,shared}/` left | `find services/llm/src -type d` |
| build green | `./scripts/build-all.sh dev --only llm` exit 0 with the full log in `/tmp/llm-build-full.log`: 0 `warning:` and 0 `error:` lines, 41/41 tests |
| the gates | the two scanner scripts, run over the working tree (each lists index + untracked paths and reads the files from disk); the check-deps deltas re-derived by calling `scripts/lib/check-deps.py`'s `scan()` on a detached worktree at `HEAD` |
| the wire | `nm -C` symbol counts on the dev binary + the five tests, `llm-wire-test` driving HTTP against the real engine |

Not verified here: `prod`. The tidy scan uses the compile databases the dev
builds leave behind, so it measures the same translation units the dev gate
compiled.

## Files

```
M  AGENTS.md                                             the llama.cpp bullet, the two "today" paragraphs, the llm key-file row (three hunks)
M  packages/clients/camera/AGENTS.md                     two `services/llm/src/main.cc` spellings → `src/app/main.cc`
M  packages/clients/llm/AGENTS.md                        the consumer list and its link-line count
M  services/llm/AGENTS.md                                the layout block and the two-feature paragraph
M  services/llm/CMakeLists.txt                           module graph + argus_service + the six repointed test/bench links
M  services/llm/CONTEXT.md                               three path lines (engine, tool runtime, consumer)
M  services/llm/tests/bench/tool-calling-bench.cc        two includes
M  services/llm/tests/unit/encounter-closed-consumer-test.cc one include
M  services/llm/tests/unit/encounter-closed-live-test.cc one include
M  services/llm/tests/unit/llm-tool-parse-test.cc        one include
M  services/llm/tests/unit/llm-tool-runtime-test.cc      three includes
M  services/llm/tests/unit/llm-wire-test.cc              one include
A  services/llm/src/feature/llm/CMakeLists.txt           argus_module(NAME llm)
A  services/llm/src/feature/encounter-closed/CMakeLists.txt  argus_module(NAME encounter-closed)
R  services/llm/src/main.cc                              -> src/app/main.cc
R  services/llm/src/controllers/llm-controller.cc        -> src/feature/llm/controllers/llm-controller.cc
R  services/llm/src/controllers/llm-controller.hxx       -> src/feature/llm/controllers/llm-controller.hxx
R  services/llm/src/controllers/llm-errors.hxx           -> src/feature/llm/controllers/llm-errors.hxx
R  services/llm/src/llm/chat-dto.cc                      -> src/feature/llm/dtos/chat-dto.cc
R  services/llm/src/llm/chat-dto.hxx                     -> src/feature/llm/dtos/chat-dto.hxx
R  services/llm/src/shared/services/llm/intent-gate.cc   -> src/feature/llm/services/intent-gate.cc
R  services/llm/src/shared/services/llm/intent-gate.hxx  -> src/feature/llm/services/intent-gate.hxx
R  services/llm/src/shared/services/llm/lfm-adapter.cc   -> src/feature/llm/services/lfm-adapter.cc
R  services/llm/src/shared/services/llm/lfm-adapter.hxx  -> src/feature/llm/services/lfm-adapter.hxx
R  services/llm/src/shared/services/llm/llm-service.cc   -> src/feature/llm/services/llm-service.cc
R  services/llm/src/shared/services/tools/tool-executor.cc  -> src/feature/llm/services/tools/tool-executor.cc
R  services/llm/src/shared/services/tools/tool-executor.hxx -> src/feature/llm/services/tools/tool-executor.hxx
R  services/llm/src/shared/services/tools/tool-registry.cc  -> src/feature/llm/services/tools/tool-registry.cc
R  services/llm/src/shared/services/tools/tool-registry.hxx -> src/feature/llm/services/tools/tool-registry.hxx
R  services/llm/src/shared/services/tools/tool-validator.cc -> src/feature/llm/services/tools/tool-validator.cc
R  services/llm/src/shared/services/tools/tool-validator.hxx -> src/feature/llm/services/tools/tool-validator.hxx
R  services/llm/src/shared/services/encounter-closed/encounter-closed-consumer.cc  -> src/feature/encounter-closed/services/encounter-closed-consumer.cc
R  services/llm/src/shared/services/encounter-closed/encounter-closed-consumer.hxx -> src/feature/encounter-closed/services/encounter-closed-consumer.hxx
A  docs/history/reports/f4-5-3-llm-layout.md             this report
M  docs/history/plans/architecture-plan.md              the step 5 row, now three of three services
```

No file outside the service changed except the root contract, the plan row and
the two client packages' `AGENTS.md` — both swept for the dead target name and
the dead path this unit created, both re-measured after the fix.
`services/llm/config.toml.example`, the `Dockerfile` (`EXPOSE 7032`) and the
`scripts/provision.sh` model download are untouched: the port, the model path
and the fixture set are the same three constants `HEAD` had.
