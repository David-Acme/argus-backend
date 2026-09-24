# Phase 4 step 3 — argus-camera gets the reference shape, and its build becomes modules

`services/camera` was the largest layout left in the plan and the last service
whose build shape was still pre-module: a root `CMakeLists.txt` that listed 52
`.cc` paths by hand into one `camera-core` archive and handed it to nine test
targets besides the executable, two of which re-listed feature sources of
their own on top of it. The step's three moves landed first (the `api/` level,
the four folders beside `feature/`, the gRPC listener out of `main.cc`), and
then the build was converted to rule 25, which is where the size of the unit
actually was: 18 module declarations, one per folder that owns code, and an
executable that names modules instead of paths.

## What existed before

- `src/feature/api/{camera,camera-control,zone}/` — the pre-migration `api/`
  level rule 23 names as a spelling to delete rather than carry.
- `src/{operator,objects,monitor}/` — domain code beside `feature/`, with
  `src/controllers/` holding the media socket and service.
- `src/main.cc` at the service root, building the gRPC server inline:
  `grpc::ServerBuilder`, `AddListeningPort(GrpcListenerConfig::resolve(7036))`,
  three `RegisterService` calls, `BuildAndStart`, a `LOG_FATAL` + `return 1`
  failure path and `grpcServer->Shutdown()` after `app().run()`.
- `argus_module(NAME camera-rpc …)` at the root, holding the sync and health
  RPC services' sources, plus `add_subdirectory` calls for the repositories
  that stayed under `src/shared/repositories/`.
- `add_library(camera-core STATIC …)` with 52 sources, linked by the executable
  and by nine of the sixteen suites; `camera-talk-cutover-test` and
  `camera-action-rpc-test` re-listed 17 and 22 `.cc` files of their own inside
  their `add_executable` blocks (16 and 21 beyond the test file itself) on top
  of it — the rule-25 anti-pattern in its purest form.

## The move

| From | To | entries |
|---|---|---|
| `src/main.cc` | `src/app/main.cc` | R099 |
| `src/feature/api/camera/` | `src/feature/camera/` | 8 renames |
| `src/feature/api/camera-control/` | `src/feature/camera-control/` | 10 renames |
| `src/feature/api/zone/` | `src/feature/zone/` | 8 renames |
| `src/operator/` | `src/feature/operator/` | 19 renames |
| `src/objects/` | `src/feature/objects/` | 4 renames |
| `src/monitor/` | `src/feature/monitor/` | 4 renames |
| `src/controllers/camera-media-{service,socket}.{cc,hxx}` | `src/feature/media/` | 4 renames |
| `src/shared/repositories/action-command/` | `src/feature/actions/repositories/action-command/` | 3 renames |
| `src/shared/repositories/object-event-outbox/` | `src/feature/operator/repositories/object-event-outbox/` | 3 renames |
| `src/shared/services/evidence/` | `src/feature/operator/services/evidence/` | 2 renames |
| `src/shared/repositories/camera-stream/` | `src/feature/sync/repositories/camera-stream/` | 3 renames |
| `src/shared/schemas/camera-stream/` | `src/feature/sync/schemas/camera-stream/` | 2 renames |

76 renames, `src/controllers/` and `src/feature/api/` gone, no empty directory
left under `src/`. The rewrite behind them replaced **99** include lines with
**107** (the extra eight are the listener's own new includes), across the
eighteen camera folder prefixes the move retired plus `main.cc`'s gRPC block;
the include roots did not move
(`target_include_directories` and every module's `INCLUDES` name
`services/camera/src`), so no other project is affected — and none names a
camera source path or target outside prose (`git grep` over the tracked tree
finds camera paths only in `docs/` and the two `AGENTS.md` files, never in a
build file or a source).

## The build, converted

Eighteen folders declare themselves now, one per folder that owns a
translation unit:

| Module | Folder | Sources |
|---|---|---|
| `camera-core` | `src/camera/` | the typed config and the camera_change NATS sink |
| `camera-rpc-server` | `src/app/rpc/` | the gRPC listener |
| `camera-actions` | `src/feature/actions/` | the action RPC service, audio capture, the STT transcriber, the action-command repository |
| `camera-feature` | `src/feature/camera/` | `/camera*` controller, DTOs, service |
| `camera-control` | `src/feature/camera-control/` | PTZ/preset/settings/talk controller, DTOs, service |
| `camera-zone` | `src/feature/zone/` | `/zone*` controller, DTOs, service |
| `camera-media` | `src/feature/media/` | the media socket and its service |
| `camera-health` | `src/feature/health/` | the `grpc.health.v1` service |
| `camera-sync` | `src/feature/sync/` | the camera sync RPC service and the camera_stream repository + schema |
| `camera-objects` | `src/feature/objects/` | the ncnn object detector (ncnn SYSTEM includes, `${ARGUS_NCNN_TARGET}`, opencv) |
| `camera-monitor` | `src/feature/monitor/` | the health monitor and its NATS sink |
| `camera-operator` | `src/feature/operator/` | the operator loop, EventIntelligence, zone provider/source, the object-event outbox and the evidence uploader |
| `camera-stream` | `src/shared/services/stream/` | go2rtc manager, StreamHub, source registrar, snapshots, fMP4 |
| `camera-driver` | `src/shared/services/camera-driver/` | the driver registry and the Tapo driver |
| `camera-tapo` | `src/shared/services/tapo/` | the Tapo protocol stack (control, talk channel, MPEG-TS muxer) |
| `camera-event-stream` | `src/shared/services/event-stream/` | the `argus.camera.events` stream helper |
| `camera-repositories` | `src/shared/repositories/` | camera, zone repositories and their schemas |
| `camera-change-outbox` | `src/shared/repositories/change-outbox/` | the change outbox repository |

The root file discovers `feature/*/CMakeLists.txt` through the four-line glob
`services/identity`, `services/notification` and `services/tts` use, adds
`src/camera`, `src/shared/{repositories,services/*}` and `src/app/rpc`
explicitly, and hands the executable to `argus_service(NAME argus-camera …)`,
which is where the `$ORIGIN` rpath and the `ARGUS_PORTS` property come from —
the hand-rolled `add_executable` block this replaces set neither. (`-Wall
-Wextra` is not new: the old root file carried it on its own line for the
executable, for `camera-core`, for `camera-rpc` and for every test target, and
what `argus_service` changes is who spells it.)
Every test target names targets: no consumer lists a `.cc` file
any more, and the two suites that listed 17 and 22 feature sources now link
`argus::camera-control` + `argus::camera-driver` and `argus::camera-actions` +
`argus::camera-stream`.

Two module declarations were **deleted** rather than carried:
`src/feature/actions/repositories/action-command/CMakeLists.txt` and
`src/feature/operator/repositories/object-event-outbox/CMakeLists.txt`. A
parent discovers modules through the glob it actually runs, which is
`feature/*/CMakeLists.txt`, so a module nested one level deeper would have
been a folder no configure ever reads. The tree keeps no such declaration
anywhere else — a feature compiles its own repositories inside its own module,
which is also the shape the notification-token family took in step 2.

`-Wall -Wextra` is not the helper's job: `argus_module` does not add it and
`argus_service` does, so all 18 modules carry the line explicitly.
`src/shared/repositories/change-outbox/CMakeLists.txt` never had it and now
does.

## gRPC out of `main.cc`

`src/app/rpc/camera-rpc-server.{hxx,cc}` is the listener: it resolves
`GrpcListenerConfig` (the `server.grpc_port` key, 7036 by default — the same
default the old `main.cc` passed as a literal), binds `host:port`, registers
the services it is handed and owns `shutdown()`. `main.cc` no longer includes
`grpcpp`, names a port or calls the builder; it constructs the three RPC
services where they always were, hands their addresses to the listener, keeps
its `LOG_FATAL` + `return 1` when the listen fails and calls `rpc.shutdown()`
exactly where `grpcServer->Shutdown()` was — before `requestStop()` on the
operator and health monitor and before `StreamHub`/`Go2rtcManager` shutdown,
so the ordering is unchanged.

The three gRPC services stay in their features, not in `app/rpc/`:
`argus.camera.v1` in `feature/sync`, `CameraActionService` in
`feature/actions` and `grpc.health.v1` in `feature/health`. Rule 23 says
`app/` is process composition only — "no domain logic, no controller, no
repository, no config resolution" — and `camera-action-rpc-service.cc` is 999
lines holding a repository, the guard-credential policy and the lease
sweeper. The plan's clause is literal and satisfied: the gRPC owner left
`main.cc` for `app/rpc/`. The listener takes `grpc::Service*`, so the three
feature modules stay independent of each other.

## The 2+ rule, measured

Rule 23 says `shared/` holds "exactly the repositories 2+ features read", so
every module family under `src/shared/` was measured for its feature readers
(`app/` and tests do not count; the service has ten features):

| Module | Feature readers | Evidence |
|---|---|---|
| `repositories/camera` + `schemas/camera` | 6 | actions, camera, camera-control, media, sync, zone |
| `repositories/zone` + `schemas/zone` | 2 | sync, zone |
| `services/stream` | 4 | actions, camera, media, operator |
| `services/camera-driver` | 2 | actions, camera-control |
| `utils/in-flight` | 2 | monitor, operator |
| `repositories/change-outbox` | 0 direct, 2 indirect | no feature includes it: the two publishing features reach it through `camera_change::getSink()` (`contracts/sync`), whose concrete implementation is `src/camera/nats-camera-change-sink.cc` and whose install is `main.cc` — the same indirect-2 shape step 2 documented for notification's outbox |
| `services/tapo` | 0 features, 1 shared reader | `services/tapo` is the protocol stack `services/camera-driver` is built on — a shared module reading a shared module, which rule 23's sentence (it names repositories, schemas and services) does not cover |
| `services/event-stream` | 1 feature + the domain folder | `feature/operator`, `src/camera/` and `main.cc` |
| `utils/geometry` | 1 | zone's two DTOs; header-only, so not a repository, schema or service |

Four families had exactly one feature reader and **moved** into it:
`action-command` → `feature/actions`, `object-event-outbox` and `evidence` →
`feature/operator`, and the `camera_stream` repository + schema →
`feature/sync` (whose RPC service is their only reader). The three rows below
the line stay, each with the reason recorded here and in `CONTEXT.md`.

## Review

Two adversarial, read-only reviewers ran over the change (one on the module
graph, one on the move's completeness); between them they raised fourteen
findings, every one re-measured here before anything moved. Three were true
and are fixed in this unit; three are recorded below as measured-but-not-defects
with the reason they stay; and eight of the fourteen are the move's own figures
and paths, all of them fixed. Two reviewer counts did not survive re-measurement
and are recorded in the second section, both of them the plan row's numbers
rather than this report's.

### The module graph: two findings fixed, three measured and left alone

- **The executable's module list in `services/camera/AGENTS.md` was wrong.** The
  doc listed `camera-{actions,camera,camera-control,zone,media,health,sync,monitor,operator,objects}`.
  The `argus_service` block names `camera-core` (not `camera`), `camera-feature`
  and `camera-rpc-server`, and does **not** name `camera-objects`, which arrives
  transitively through `camera-operator`, its only reader. The sentence now
  states the measured list and says why `camera-objects` is absent; the same
  sentence in `CONTEXT.md` got the same correction.
- **`src/app/rpc/CMakeLists.txt` declared three dependencies the module never
  uses** — `argus::camera-actions`, `argus::camera-health`, `argus::camera-sync`.
  Its two translation units include only `<grpcpp/grpcpp.h>` and
  `<http/listener-config.hxx>`, and `CameraRpcServer` holds `grpc::Service*`
  without naming a camera RPC class (those are constructed in `main.cc`).
  Replaced with the two the code reaches, `argus::lib::http` (the
  `GrpcListenerConfig` owner) and `argus::lib::grpc-health` — the same pair
  `services/{auth,identity,sync}/src/app/rpc/CMakeLists.txt` declares. Measured
  effect: **745 → 744 edges**, the three false edges becoming two true ones, and
  confirmed by a full rebuild rather than by reading the file. The second
  reviewer found one more of the same kind — `feature/monitor` declared
  `argus::camera-event-stream`, which nothing in that folder names; removing it
  took the count to **743**.
- **The schemas folder has no declaration of its own** (its two `.cc` are
  compiled by `src/shared/repositories/CMakeLists.txt` from a sibling path).
  True, and not a defect: `services/sync/src/shared/repositories/CMakeLists.txt`
  does exactly the same with the same folder names, so the tree's convention is
  that a repository-and-schema pair is one unit whose module is the repository
  folder. Each translation unit is compiled exactly once either way, and
  declaring the schemas separately would be a module with one reader (rule 24).
- **`add_subdirectory` ordering** (`src/camera` before `event-stream`; the
  feature glob before the modules that reference its targets). CMake resolves
  target names at generate time inside one configure run, the tree configures
  and links, and `services/identity/CMakeLists.txt` has the same order today.
- **`CameraRpcServer`'s constructor throws outside any `try`** in `main.cc`.
  Unreachable here — the call site passes three non-null service addresses — and
  the `services/tts/src/app/rpc/` listener is shaped the same way.

The same reviewer independently re-derived the parts that had to hold: 65 `.cc`
under `src/`, 64 declared exactly once, none declared twice, no declared path
missing, `app/main.cc` the only undeclared file and correctly the executable's
`MAIN`; no consumer listing a `.cc`; the `if(NOT TARGET)` guard set and the
`file(GLOB … CONFIGURE_DEPENDS)` discovery idiom matching `tts`, `auth`, `sync`,
`notification` and `identity`; all four controllers `HttpController<…, false>`
and registered by hand, so `WHOLE_ARCHIVE` appears nowhere; the filter chain
order unchanged; rules 2/16/19/20 clean; and every retired module name
(`camera-action-command`, `camera-object-event-outbox`) gone from the tree
including the docs.

**What the reading could not see, the gate did.** Both reviewers were read-only
and did not rebuild, and said so. The whole-tree tidy gate — which a `--only`
run skips deliberately — found the one thing that reading missed:
`performance-unnecessary-value-param` on the new constructor's `CameraRpcInput`
parameter, taking that check to 50 against the baseline's 49. Located by direct
invocation (`clang-tidy -p services/camera/build/dev
src/app/rpc/camera-rpc-server.cc --checks='-*,performance-unnecessary-value-param'`)
and fixed by taking `const CameraRpcInput&`; the parameter is *not* moved into a
member, which is what makes the `tts` precedent's by-value shape inapplicable.

### The move's completeness: nine findings, all of them fixed

The second reviewer worked from the diff itself rather than the build: rename
integrity, includes, deletions, comment and modern-C++ sweeps, and every figure
this report and the plan row claim. It confirmed the report's figures by fresh
measurement — 76 renames, the 52-source archive, 18 declarations, 124 paths,
18 added / 2 deleted — and falsified five things. Two of its own counts did not
survive re-measurement here, both in the plan row's favour rather than the
report's: it read `camera-core` as having seven test consumers where the
measurement is **nine** of the sixteen suites plus the executable, and it read
the two re-listed `add_executable` blocks as 19 and 24 `.cc` files where the
blocks themselves list **17 and 22**. The plan row is corrected on both counts
(and the review section of this report with it).

- **`feature/monitor` declared `argus::camera-event-stream`** — the only
  occurrence of that name in the folder is the line itself; the module's two
  translation units include `feature/operator/frame-source.hxx`, sqlite,
  runtime, trantor, opencv, json and nats, and nothing from the event stream.
  Removed, and the tier gate re-measured: **744 → 743 edges**, 0 forbidden,
  0 cycles, 0 unresolved.
- **`services/camera/AGENTS.md` claimed the tapo and event-stream modules are
  read by 2+ features.** Measured: `services/tapo` has **0** feature readers
  (its reader is `shared/services/camera-driver/tapo-driver.*`) and
  `services/event-stream` has **1** (`feature/operator`); the 2+ rows are
  `stream` (4) and `camera-driver` (2). The layout line now says what the
  report's own table says instead of contradicting it.
- **`CONTEXT.md` still had the driver stack compiling into `camera-core`** —
  true before this unit and false after it: `camera-core` is now the typed
  config and the camera_change sink, and the Tapo stack is the
  `src/shared/services/{camera-driver,tapo}` modules. Rewritten in the present
  tense of the new layout.
- **The report, the plan row and this section were unfinished.** The plan row
  still read `745 edges` after the `app/rpc` fix took the tree to 744 (this
  report had been corrected, the row had not), and the report carried two
  literal placeholders — `REVIEW_SECTION` and `TIDY_RESULT` — under a heading
  that already claimed the reviews were done. The row now reads **743** (both
  fixes applied) and this section is the text that replaces the placeholder;
  the report is also staged with the change, so the plan row's pointer to it
  resolves in the same commit.
- **The report and the plan row claimed "103 include lines"** and the plan row
  `-Wall -Wextra` among the things `argus_service` newly supplies. Both are
  wrong and both are corrected here and in the row: the measured diff replaces
  **99** include lines with **107**, and `-Wall -Wextra` was already on the old
  root file's own line for the executable, `camera-core`, `camera-rpc` and
  every test target — what was actually missing at `HEAD` is the `$ORIGIN`
  rpath and the `ARGUS_PORTS` property, and that is what the report says now.
- **`CONTEXT.md` described `media-relay.{cc,hxx}` in the present tense** — the
  file was deleted by `d2756952` (f8-a2) with the rest of the `labs/` surface.
  The sentence is now past tense and names the commit.

**One finding outside the unit, and it was real: the local configs could not
boot.** `services/{camera,notification,productivity}/config.toml` each spelled
their schema path `services/argus-<name>/database/schema.sql`, a directory that
has not existed since the folder rename; the tracked `.example` templates spell
it `services/<name>/database/schema.sql`, and `main.cc` feeds the value to
`DbService::runScriptFile`, whose `false` is a `LOG_FATAL` + `_exit(1)`. Dates
put the files before this unit, so it is pre-existing rather than caused here,
but a developer running any of the three services natively would have been
stopped by it. All three paths are corrected in place. They are gitignored
instance files, so nothing of the sort is committed — and per rule 17b the
secrets they carry were neither read into this conversation nor printed.

**What the second reviewer could not settle, it said so**: the 52/52 tests, the
`nm` symbol counts and the tidy result rest on this report alone, because it
built nothing. It did run both pre-build gates itself and got the numbers the
tree stood at while it read (1345 files / 0 comments; 95 declarations / 744
edges — one more than the final figure, because the `feature/monitor` edge came
out after its sweep). Everything it *could* measure it did: 19 non-include
changed lines among the 76 renames, all in `main.cc`; every other renamed file
include-line-only; exactly two deletions, each proven redundant because the
feature module's `SOURCES` already lists the file; no empty directory; no
orphan `.cc`; every module carrying its own `-Wall -Wextra` line (18 of 18);
and no living document naming a retired path.

## Verification

- `./scripts/build-all.sh dev --only camera` — **52/52 tests, 0 failures,
  exit 0**, and not one `warning:` line in the log. Re-run last, after the two
  review fixes (the `app/rpc` dependency set and the `feature/monitor` edge),
  so these are the numbers the tree commits: the two pre-build gates ran with
  it: `check-comments` **1345** files checked, 0 comments, and
  `check-deps` **95** declarations, **743** edges, 0 forbidden, 0 cycles,
  0 unresolved, 23 deferred (286 third-party mentions over 22 roots).
- Both dependency deltas are exactly this unit, and the file count confirms
  it: `git status` shows 18 files added (16 new module declarations plus the
  two listener sources) and 2 deleted (the nested declarations), so
  `check-comments` moved 1329 → 1345 by precisely 18 − 2. Declarations moved
  81 → 95 because camera went from **5** declaration sites at `HEAD` (the root
  `camera-rpc` module and the four repository modules) to **19** (the root
  `argus_service` call, the ten feature folders, the four shared-service
  folders, `src/camera`, `app/rpc` and the two surviving repository modules) —
  5 → 19 is the +14 the checker reports, with the two deleted nested
  declarations inside the old 5. Edges rose 669 → 743, and measured directly
  the service's own `argus::` mentions went 92 → 167 over 7 → 21 `CMakeLists.txt`
  files; no other project's CMake changes in this unit, so the tree's edge delta
  is camera's conversion from a root file that listed targets to modules that
  declare their own.
- The controllers are in the binary, measured rather than assumed:
  `nm -C services/camera/build/dev/argus-camera` counts **230**
  `CameraController::` symbols, **230** `ZoneController::`, **135**
  `CameraControlController::` and **25** `CameraMediaSocket::` — the
  whole-archive question step 2 hit does not arise here, because every camera
  controller is `HttpController<…, false>` and `main.cc` registers each one by
  hand, so no route depends on a static initializer reaching the binary.
- `scripts/check-tidy.sh` over the whole tree — the gate a `--only` run skips
  deliberately: **538 TUs, 2902 findings over 45 checks, baseline 2902,
  exit 0**. The new listener is the 538th translation unit (the baseline
  records 537), and the per-check total is exactly the baseline, so the module
  boundary changed no check's number. The gate's first run was **not** clean,
  and that is the point of it: `performance-unnecessary-value-param` stood at
  50 against the baseline's 49, on this unit's own new file. Re-run after the
  fix: the same three lines, the count back at 2902.
- The move's completeness, measured directly: no source path under a
  `feature/api/` segment survives in the service, no `src/{controllers,objects,
  monitor,operator}/` survives, no empty directory survives under `src/`, every
  source path the CMake files name exists, every `.cc` under `src/` is compiled
  by exactly one module (only `app/main.cc`, the executable's `MAIN`, is
  outside `SOURCES`), and no include line still names an old prefix. The only
  trace of the old shape left on disk is the stale object directories under the
  ignored `build/` trees, which the next configure rewrites.

## Files

The unit touches 124 paths under `services/camera` and outside it
(`git status --porcelain`): the root `CMakeLists.txt` (rewritten),
`src/app/main.cc` (thinned), `src/app/rpc/camera-rpc-server.{hxx,cc}` plus its
declaration, the 16 new module declarations (ten features, four shared
services, `src/camera`, `app/rpc`), the two surviving repository declarations
(`CMakeLists.txt` with the camera-stream pair dropped, `change-outbox` with
`-Wall -Wextra` added), the two deleted nested declarations, the ten test files
repointed at modules, and the two service documents. Outside the service: the
root `AGENTS.md` (rule 23's "Today, against that target"), the four package
documents whose consumer lines named `camera-core` or `camera-rpc`
(`packages/clients/{camera,camera-actions,stt,tts}/AGENTS.md`) and the
`packages/contracts/camera/AGENTS.md` line that counted four consumers where
there are eleven, the two living documents that carried media paths under the
retired `src/controllers/` (`docs/architecture/wire-camera-media.md`,
`argus-deploy/CONTEXT.md`), the plan row, and this report.
