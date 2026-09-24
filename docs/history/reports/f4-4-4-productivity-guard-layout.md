# Phase 4 step 4 — argus-productivity and argus-guard get the reference shape

`services/productivity` and `services/guard` were the last two services whose
HTTP surface still lived under the pre-migration `feature/api/<resource>/`
level, and they landed in one unit because the shape they converge on is the one
`services/camera` had just taken: `src/app/` for composition, `feature/<feature>/`
for the vertical slice, and a build in which every folder that owns a
translation unit declares itself (rule 25).

For productivity the size was in the build: the root file compiled a 16-source
`productivity-core` archive beside a `PRODUCTIVITY_FEATURE_SOURCES` variable
holding 20 `.cc` paths, and **three** targets consumed that variable — the
executable and two suites that re-listed it inside their own `add_executable`.
For guard the size was in the merge: two modules that had never carried
`-Wall -Wextra` became one, which is what surfaced the 27 warnings recorded
below.

## What existed before

`services/productivity`:

- `src/feature/api/{calendar-event,calendar-event-share,project,project-member,project-task}/`,
  each with `controllers/`, `dtos/` and `services/` — the `api/` level rule 23
  names as a spelling to delete, sitting beside `src/feature/sync/`, which the
  earlier steps had already migrated.
- `src/main.cc` at the service root, building the gRPC server inline:
  `grpc::ServerBuilder`, `AddListeningPort(GrpcListenerConfig::resolve(7037))`,
  one `RegisterService` call, `BuildAndStart()`, a `LOG_FATAL` + `return 1`
  failure path and `grpcServer->Shutdown()` after `app().run()`.
- `src/productivity/` (the typed config and the `productivity_change` NATS sink)
  and `src/shared/{repositories,schemas}/` — domain code beside `feature/`.
- the root build: `add_library(productivity-core STATIC …)` with **16** sources
  (the two in `src/productivity/`, seven repositories, seven schemas) and
  `set(PRODUCTIVITY_FEATURE_SOURCES …)` with **20**, consumed by
  `argus-productivity` and re-listed by `productivity-controller-test` and
  `productivity-change-transaction-test`.
- five controllers declared `drogon::HttpController<T>` — Drogon's
  `AutoCreation` static registration, the shape step 2's notification module
  needed a `WHOLE_ARCHIVE` link for.

`services/guard`:

- `src/feature/api/guard/{controllers,dtos,services}/` beside
  `src/feature/guard/` (the domain), so one feature was split across two folders
  and two modules.
- `src/main.cc` at the service root.
- `argus_module(NAME guard …)` with 9 domain sources and
  `INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}` — the feature root itself, which is why
  every include in that folder was a bare `<guard-service.hxx>` — plus
  `argus_module(NAME guard-api …)` with 9 surface sources. Neither carried
  `-Wall -Wextra`: `argus_module` does not add it and the root file carried it
  only on the executable and the test targets.
- `add_executable(argus-guard ${GUARD_SRC_ROOT}/main.cc)` with a
  `target_link_libraries` line — no `$ORIGIN` rpath and no `ARGUS_PORTS`
  property.

## The move

| Service | From | To | entries |
|---|---|---|---|
| productivity | `src/main.cc` | `src/app/main.cc` | 1 |
| productivity | `src/feature/api/calendar-event/` | `src/feature/calendar-event/` | 8 |
| productivity | `src/feature/api/calendar-event-share/` | `src/feature/calendar-event-share/` | 8 |
| productivity | `src/feature/api/project/` | `src/feature/project/` | 8 |
| productivity | `src/feature/api/project-member/` | `src/feature/project-member/` | 8 |
| productivity | `src/feature/api/project-task/` | `src/feature/project-task/` | 8 |
| productivity | `src/shared/repositories/reminder{,-detail}/` | `src/feature/sync/repositories/reminder{,-detail}/` | 6 |
| productivity | `src/shared/schemas/reminder{,-detail}/` | `src/feature/sync/schemas/reminder{,-detail}/` | 4 |
| guard | `src/main.cc` | `src/app/main.cc` | 1 |
| guard | `src/feature/api/guard/{controllers,dtos,services}/` | `src/feature/guard/{controllers,dtos,services}/` | 19 |

**71 renames** (51 productivity, 20 guard), 47 of them byte-identical and the
rest 83–99% similar; `src/feature/api/` is gone from both services and no empty
directory is left under either `src/`. The rewrite behind them is measured off
the diff: **130 include lines rewritten into 136** — guard 86 for 86, symmetric
because its rewrite only changed the prefix, and productivity 44 for 50, the six
extra being `main.cc`'s new includes (the five feature controllers and the
listener). No include line in either service names a retired prefix any more,
and the include roots did not move (`INCLUDES ../..`, i.e.
`services/<name>/src`), so no other project is affected: no build file or source
outside these two services names one of their source paths.

Guard's includes were normalized to the house convention while they moved — a
same-directory sibling is a quoted include, anything else first-party is an
angle include with the full path from `src/`. That is why the feature root is no
longer on that module's include path: the **71** bare include lines at `HEAD`
across 30 files — `<guard-schema.hxx>` 11, `<guard-service.hxx>` 10,
`<guard-repository.hxx>` 9 and 28 more over eight `vocabulary/` headers — became
qualified `<feature/guard/…>` spellings, of which **0** remain. The tree's
`feature/guard/…` include lines went 6 → 90 over 32 files. Productivity's moved
repositories did the same
(`<feature/sync/repositories/reminder/reminder-repository.hxx>` from the sync
service, `<feature/sync/schemas/reminder/reminder-schema.hxx>` from the
repository), while the five families that stayed under `src/shared/` keep their
`<shared/repositories/…>` spelling, which is correct — they did not move.

## The build, converted

Eleven modules declare themselves now, one per folder that owns a translation
unit, replacing one archive and one path variable:

| Module | Folder | Sources |
|---|---|---|
| `productivity-core` | `src/productivity/` | the typed config and the `productivity_change` NATS sink |
| `productivity-repositories` | `src/shared/repositories/` | the five repository families whose readers are 2+ features, plus their schemas |
| `productivity-change-outbox` | `src/shared/repositories/change-outbox/` | the change outbox repository |
| `productivity-calendar-event` | `src/feature/calendar-event/` | controller, two DTOs, feature service |
| `productivity-calendar-event-share` | `src/feature/calendar-event-share/` | controller, two DTOs, feature service |
| `productivity-project` | `src/feature/project/` | controller, two DTOs, feature service |
| `productivity-project-member` | `src/feature/project-member/` | controller, two DTOs, feature service |
| `productivity-project-task` | `src/feature/project-task/` | controller, two DTOs, feature service |
| `productivity-sync` | `src/feature/sync/` | the sync RPC service, the reminder and reminder-detail repositories and their schemas |
| `productivity-rpc-server` | `src/app/rpc/` | the gRPC listener |
| `guard` | `src/feature/guard/` | the domain (9 sources) and the surface (controller, 7 DTOs, feature service) |

`40` `.cc` files live under `services/productivity/src` and `39` of them are
declared in a module — `src/app/main.cc` is the executable's `MAIN`. Guard is
`19` and `18`, the same way. Every `.cc` is compiled by exactly one module.

The root file globs `feature/*/CMakeLists.txt` through the four-line
`CONFIGURE_DEPENDS` loop the tree uses elsewhere, adds `src/productivity`,
`src/shared/repositories`, its `change-outbox` and `src/app/rpc` explicitly, and
hands the executable to `argus_service(NAME argus-productivity … PORTS 7027
7037)`, which is where the `$ORIGIN` rpath and the `ARGUS_PORTS` property come
from. Guard's root is the same shape one module wide
(`argus_service(NAME argus-guard … PORTS 7039)`), and the two pre-build guards
it already had — `find_package`/`add_subdirectory` under
`PROJECT_IS_TOP_LEVEL`, and the `if(NOT TARGET …)` blocks for the packages it
consumes — are untouched.

No consumer lists a `.cc` file any more. `productivity-controller-test` and
`productivity-change-transaction-test` each named the 20 feature sources at
`HEAD`; they now link the five feature modules and `argus::productivity-core`,
and `productivity-sync-rpc-test` links `argus::productivity-sync` instead of
compiling the RPC service itself. Every module carries its own
`-Wall -Wextra` line — `argus_module` does not add it and `argus_service` does,
which is why all eleven have it written out; the change-outbox module, which had
never had it, gained it here.

The merge gave guard's sources their first `-Wall -Wextra` compile, and the
build that followed is the reason that line mattered: **27 warnings, all of them
in `guard-service.cc`** — 25 `-Wmissing-field-initializers` (22 across
`GuardService::EffectInput`'s `rule`/`cameraName`/`text`/`lang` and 3 across
`QueueEntry`'s `ack`/`nak`/`term`) and 2 `-Wunused-variable` (`sequence`, `now`).
Each was fixed in this unit rather than suppressed, the designated initializers
getting the members rule 2 asks them to list: the rebuild that followed the fix
logs **0** `warning:` lines, which is the number the tree commits.

## The controller question, settled by measurement

The plan row predicted this step would need step 2's `WHOLE_ARCHIVE` link,
because productivity's five controllers were `AutoCreation` controllers like
notification's two. The newest reference settles it the other way: camera's
controllers are `drogon::HttpController<T, false>` registered by hand in
`main.cc`, and its modules link plainly with every route present. Both services
in this unit took that shape.

- `HttpController<…, false>` in all six controllers (productivity's five and
  guard's one), each registered with `drogon::app().registerController(std::make_shared<T>())`
  in `main.cc` — one line per controller.
- `WHOLE_ARCHIVE` appears **nowhere** in either root file (the only occurrence
  in the tree remains `services/notification/CMakeLists.txt`, which needs it
  because its two controllers are still `AutoCreation`).
- The route set is unchanged: productivity's controllers declare **15**
  `ADD_METHOD_TO` lines at `HEAD` and **15** now (3 per controller), guard's
  **10** at `HEAD` and **10** now.
- The routes are in the linked binaries, measured rather than assumed:
  `strings services/productivity/build/dev/argus-productivity` carries every
  registered path family (`/calendar-event`, `/calendar-event/{…}`,
  `/calendar-event-share`, `/project`, `/project-member`, `/project-task` and
  their `/{…}` forms) and `nm -C` counts **230** symbols per controller class;
  `services/guard/build/dev/argus-guard` carries all seven `/guard…` routes,
  **360** `GuardController::` symbols, **1356** `GuardService::` and **87**
  `GuardFeatureService::`. A module that were silently dropped would show zero
  of these — the failure mode step 2 measured (122 controller symbols in the
  suite that compiled them, 0 in the binary that linked the archive).

## The gRPC listener, out of `main.cc`

`src/app/rpc/productivity-rpc-server.{hxx,cc}` is the listener: it takes a
`ProductivityRpcInput` (`{std::vector<grpc::Service*> services}`, the shape the
`tts` and `camera` listeners use), resolves `GrpcListenerConfig` — the
`server.grpc_port` key, 7037 by default, the same default the old `main.cc`
passed as a literal — validates the service list with `std::ranges::any_of`,
binds `host:port`, registers what it is handed and owns `shutdown()`. `main.cc`
no longer includes `grpcpp`, names a port or calls the builder; it constructs
`ProductivitySyncRpcService` where it always was, hands its address to the
listener, keeps its `LOG_FATAL` + `return 1` when the listen fails and calls
`rpc.shutdown()` exactly where `grpcServer->Shutdown()` was. The RPC service
itself stays in `feature/sync/`: rule 23 says `app/` is composition only, and
the listener takes `grpc::Service*`, so the feature module stays independent of
the listener.

`argus::lib::grpc-health` is a dependency of the listener module alone (it is
what provides `<grpc/grpcpp.h>` through `argus_client_grpc_base`), with the
`add_subdirectory(packages/lib/grpc … EXCLUDE_FROM_ALL)` guard the camera root
carries — a standalone configure of `services/productivity` has to find it.

## The 2+ rule, measured

Rule 23 says `shared/` holds "exactly the repositories 2+ features read", so
every repository family under `src/shared/repositories/` was measured for its
feature readers (`app/` and the tests do not count; the service has six
features):

| Family | Feature readers | Evidence |
|---|---|---|
| `calendar-event` + schema | 3 | calendar-event, calendar-event-share, sync |
| `calendar-event-share` + schema | 3 | calendar-event, calendar-event-share, sync |
| `project` + schema | 4 | project, project-member, project-task, sync |
| `project-member` + schema | 4 | project, project-member, project-task, sync |
| `project-task` + schema | 2 | project-task, sync |
| `reminder` + schema | 1 | sync — **moved** into `feature/sync/` |
| `reminder-detail` + schema | 1 | sync — **moved** into `feature/sync/` |
| `change-outbox` | 0 features, 1 domain reader | no feature includes it: the publishing features reach it through the `UserChangeSink` contract (`contracts/sync`), whose concrete implementation is `NatsProductivityChangeSink` in `src/productivity/`, installed by `user_change::setProductivitySink(...)` in `main.cc` — the same indirect shape step 2 documented for notification's outbox, and it keeps its own module |

Two families had exactly one feature reader and moved into it. The five that
stay are the ones a second feature reads, and `src/productivity/` (the typed
config) stays beside `feature/` until step 9, which is where the root rules file
already accounts for it.

## Review

Two adversarial, read-only reviewers ran over the change — one over the
documents and their citations, one over the behaviour. The behaviour reviewer
came back clean on all four sections it was given, and its three "not defects"
claims were reproduced here independently rather than accepted. The document
reviewer raised five findings; re-measuring each one turned up four more wrong
citations in the same file, and every one of them is fixed. One reviewer count
did not survive re-measurement and is recorded below.

### The citations: six documents and the plan row

- **`services/productivity/CONTEXT.md` claimed the repositories and schemas
  compile into `productivity-core`.** False since this unit: the five families
  2+ features read declare `argus::productivity-repositories`, and the reminder
  pair compiles inside `argus::productivity-sync`. The `f7-7b` paragraph now
  says what step 4 did to the archive, and the `/health` bullet's
  parenthetical, which still said the repositories and schemas "compile here",
  was rewritten to name the sync feature and the families' own modules.
- **`packages/clients/identity/AGENTS.md` named `guard-api` as a consumer
  module.** The module is gone — merged into `guard` — so the paragraph now
  counts one guard module (`services/guard:108`,
  `src/feature/guard/CMakeLists.txt:25`) and its productivity line moved to
  `:196`, both re-measured.
- **`packages/clients/notification/AGENTS.md` carried two stale numbers in one
  sentence** — the guard link `:104` → `:109` and the module path `:22` → `:27`.
- **`packages/contracts/camera/AGENTS.md` cited the guard module at `:13`**,
  which is a blank line now; the declaration is at `:29`.
- **Three documents cited a path that no longer exists**, `services/guard/src/main.cc`
  (it is `src/app/main.cc`): `packages/clients/camera-actions/AGENTS.md:76`,
  `packages/clients/vlm/AGENTS.md:74` and
  `packages/clients/notification/AGENTS.md:70`. The line ranges are the same
  numbers after the move — verified, not assumed, against the content at `:131`,
  `:137`, `:149-156` and `:158-164` — so these were path-only repairs.
- **`packages/clients/vlm/AGENTS.md` had four citations that were already wrong
  at `HEAD`**, which no gate measures: `:136` → `:112` (the `argus-guard` link
  line), `:23` → `:28` (the feature module), `:225` → `:199`
  (`vlm-client-live-test`) and `:249` → `:222` (`guard-assessment-live-test`),
  plus the by-path line `:112` → `:83`. Found while re-measuring the guard
  citations above; pre-existing, and fixed here because the same sentences were
  already being edited.
- **The plan row predicted whole-archive.** `architecture-plan.md:1004` said
  `productivity` "is the only one left, so its feature module needs step 2's
  whole-archive link for the routes to register" — refuted by measurement (the
  controller section above), so the row is Done with the shape that landed.

### The warning gate: one reviewer count that did not survive

The document reviewer reported both projects' translation units as carrying
`-Wall -Wextra`. Measured against each project's `compile_commands.json`, that
is not what the tree says: **15 of 128** productivity TUs and **18 of 130**
guard TUs compile without the pair, and not one of them is this unit's code.
Both lists are the same three families: `packages/clients/*` sources, which
`argus_clients` declares without the flag (4 and 5), protoc-generated `.pb.cc`
under each project's own `build/` (10 and 12), and
`third_party/sqlite-vec/sqlite-vec.c`, one per project. Measured identically in
camera's and notification's compile databases (67 and 16 TUs without the flags,
the same families), so the gap belongs to the package helper and the generated
sources and pre-dates this unit. The one first-party exception the census
turned up in the tree is notification's
`src/shared/repositories/change-outbox/change-outbox-repository.cc`, whose
module step 2 declared without the line — also outside this unit, and left
recorded rather than silently fixed.

What is this unit's is the other half of the claim: **every one of the eleven
declarations it created carries its own `-Wall -Wextra` line**, measured by
reading them. `argus_module` does not add it and `argus_clients` does not
either; `argus_service` (`cmake/argus-module.cmake:419`) is the only helper that
does, which is why the executable's flag is the one line this unit did not have
to write. `guard`'s merged module is the one that never had the line before.

### The behaviour: clean, and the three "not defects" reproduced

- **`sqlite-vec` linkage survives the module split.** It arrives through
  `packages/lib/sqlite`, which links the vendored extension **PUBLIC**
  (`:49`) and references `sqlite3_vec_init` in `db-service.cc:191` and
  `vec-db.cc:88`, so `sqlite3_auto_extension` registers vec0 without either
  service naming the extension: measured, **96** vec0 symbols in
  `argus-productivity` and **96** in `argus-guard`.
- **guard's merged `DEPENDS` is the honest union.** Measured against `HEAD`:
  the feature module named 13 aliases, `guard-api` named 7 of which 3 were new
  (`lib::validation`, `lib::http`, `lib::auth`) and one was the `argus::guard`
  self-reference the merge makes meaningless, and the merged list is exactly
  those **16**, plus `Drogon::Drogon` — which neither `HEAD` list named, because
  the controller headers' `<drogon/drogon.h>` used to arrive transitively and the
  merged module states it.
- **guard's `main.cc` moved without being rewritten.** Rename-aware,
  `git diff -M HEAD` reports **1 insertion, 1 deletion** on the pair: the
  feature controller's include line, `<feature/api/guard/controllers/guard-controller.hxx>`
  → `<feature/guard/controllers/guard-controller.hxx>`. Nothing else in 352
  lines changed.

## Verification

- `./scripts/build-all.sh dev --only productivity` — **35/35 tests, 0 failures,
  exit 0**, and not one `warning:` line in the log.
  `--only guard` — **54/54 tests, 0 failures, exit 0** (22.54 s), 0 warnings.
  Both run after the last review fix, so these are the numbers the tree
  commits. The two pre-build gates ran with them: `check-comments` **1355**
  files checked, 0 comments, and `check-deps` **105** declarations, **810**
  edges, 0 forbidden, 0 cycles, 0 unresolved, 23 deferred (294 third-party
  mentions over 22 roots).
- Both gate deltas are exactly this unit, and the file count proves it:
  `git status` shows **11** files added — the three `src/app/rpc/` sources and
  eight module declarations — and **1** deleted (guard's nested
  `feature/api/guard/CMakeLists.txt`), so `check-comments` moved 1345 → 1355 by
  precisely 11 − 1. Declarations moved 95 → 105, and the +10 is `productivity`'s
  alone: it went from **1** declaration site at `HEAD` (the change-outbox
  module) to **11** (the root `argus_service` plus ten modules), while `guard`
  went from 2 (`guard` + `guard-api`) to 2 (`guard` + the root `argus_service`)
  — the deleted nested declaration is the one it traded for the executable's,
  so its own count did not move. Measured at `HEAD` in a scratch worktree
  (`git worktree add --detach`), not inferred: 95 declarations, 743 edges there.
- The controllers are in the binary, measured rather than assumed:
  `nm -C services/productivity/build/dev/argus-productivity` counts **230**
  symbols per controller class (all five) and `argus-guard` **360**
  `GuardController::`, **1356** `GuardService::` and **87**
  `GuardFeatureService::`. Every route is registered by hand and none depends on
  a static initializer: 15 `ADD_METHOD_TO` lines across the five productivity
  controllers, registered at `main.cc:78-85` (the fifth beside the health
  controller), and guard's 10 in one controller, registered at `main.cc:314`.
- The rename count, measured with `git diff -M --summary HEAD`: **71** renames,
  **47** of them at 100% similarity and the rest between 88% and 99%.
- `scripts/check-tidy.sh` over the whole tree — the gate a `--only` run skips
  deliberately: **539 TUs, 2901 findings over 45 checks**, exit 0. The TU count
  is two above the baseline's floor of 537, as it has been since camera's
  listener landed, and the check census was run to find out which single check
  the tree now sits below: `modernize-use-auto`, **54 → 53**, the only
  difference between the census and the baseline. Rule 19 says a baseline count
  comes down in the same change that fixes what stands behind it, so
  `scripts/lib/tidy-baseline.txt` records 53 and the gate's total is now 2901 =
  the tree's own count. No check rose, so the gate passed before the correction
  too — this is the ratchet being taken up, not a failure being cleared. The
  worst file is `services/guard/src/feature/guard/guard-repository.cc`, at 104
  findings.
- The module ownership was audited mechanically: every `.cc` under both `src/`
  trees is named by exactly one module, none twice, none orphaned, and only
  each `src/app/main.cc` (the executable's `MAIN`) sits outside a `SOURCES`
  list. The include rewrite is measured off the diff — 44 lines replaced by 50
  on productivity, 86 by 86 on guard — and first-party translation units are
  128 and 130.
- The move's completeness, measured directly: no `feature/api/` segment survives
  in either service (`git grep` finds one, in a document's prose, and no code
  include), no `shared/repositories/reminder*` copy is left beside the moved
  pair, no consumer outside `feature/sync` names one, and every source path the
  CMake files name exists.

## Files

The unit touches 131 paths (`git status --porcelain -uall`), 122 of them under
the two services: `services/productivity` 69 and `services/guard` 53. Under
productivity: the root `CMakeLists.txt` (rewritten — `productivity-core`,
`PRODUCTIVITY_FEATURE_SOURCES` and the hand-listed executable replaced by
`argus_service` plus the feature glob), `src/main.cc` → `src/app/main.cc`, the
three new `src/app/rpc/productivity-rpc-server.{hxx,cc}` and their declaration,
eight new module declarations, the five `feature/api/<resource>/` trees
flattened into `feature/<feature>/`, `reminder` and `reminder-detail`
repositories and schemas moved into `feature/sync/`, and the three suites
repointed at modules. Under guard: the root `CMakeLists.txt`, `src/main.cc` →
`src/app/main.cc`, `feature/api/guard/{controllers,dtos,services}` merged into
`feature/guard/` with the nested declaration deleted, and the eight unit suites
plus two live tests repointed. Both services' `AGENTS.md` layout blocks changed
with them — they described the pre-migration shape (`src/controllers/`,
`src/server/`, a `guard-api` module) and now describe the module graph, a piece
of step 8's work landing where the layout it documents changed — and so did
productivity's `CONTEXT.md` (the split of the archive and the reminder pair's
new home). Outside the two services: the five package documents above, the
root `AGENTS.md` (rule 23's "Today, against that target" — the
`feature/api/<resource>/` spelling is now gone from the tree, and the paragraph
says so), the plan row, `scripts/lib/tidy-baseline.txt` and this report.
