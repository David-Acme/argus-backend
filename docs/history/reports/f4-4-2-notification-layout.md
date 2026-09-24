# Phase 4 step 2 — argus-notification gets the reference shape

`services/notification` now carries rule 23's layout: `src/main.cc` became
`src/app/main.cc`, the gRPC owner left `feature/rpc/` for `app/rpc/`, and the
HTTP feature left `feature/api/notification/` for `feature/notification/`.
Nineteen files moved as git renames, so the change itself is fourteen include
lines, twelve CMake source paths and the documents that named the old paths —
and then the two homes the step creates became rule-25 modules, which is where
the interesting part of this unit turned out to be: the first green build of
that conversion linked a binary with **no HTTP controllers in it at all**.

## What existed before

- `feature/api/notification/{controllers,dtos,services}` — the pre-migration
  `api/` level rule 23 names as a spelling to delete rather than carry.
- `feature/rpc/` — a capability-shaped home for transport infrastructure;
  rule 23 puts gRPC in `app/rpc/`, "infrastructure rather than a capability".
- `src/main.cc` at the service root — the other pre-migration spelling rule 23
  names, alongside `src/server/`.
- `src/notification/` (the typed config and the two NATS sinks) stays where it
  is: Phase 4 step 9 owns the "config resolution into `src/config/`" move, and
  the root rules file already accounts for a service that "still keeps code
  beside" `feature/` until then.

## The move

| From | To | git |
|---|---|---|
| `src/main.cc` | `src/app/main.cc` | R099 |
| `src/feature/rpc/notification-rpc-service.{hxx,cc}` | `src/app/rpc/` | R100 / R100 |
| `src/feature/api/notification/controllers/*.{hxx,cc}` (4) | `src/feature/notification/controllers/` | R085–R093 |
| `src/feature/api/notification/dtos/*.{hxx,cc}` (8) | `src/feature/notification/dtos/` | R100 |
| `src/feature/api/notification/services/*.{hxx,cc}` (4) | `src/feature/notification/services/` | R100 |

The four controller files below 100% carry the include rewrite; the other
fifteen are pure moves. `src/feature/api/` and `src/feature/rpc/` are gone.

References updated with them:

- **Includes, fourteen lines across eight files** — `src/app/main.cc:3`,
  `feature/notification/controllers/notification-controller.{hxx:7,cc:3,4,5}`,
  `feature/notification/controllers/notification-token-controller.{hxx:7,cc:3}`,
  `tests/unit/notification-controller-test.cc:6-10`,
  `tests/unit/notification-rpc-test.cc:8` and
  `tests/unit/notification-no-nats-test.cc:8`. The include roots did not move
  (`target_include_directories` names the source root), so a consumer outside
  the service is unaffected, and there is none: the write-side feature sources
  compile into this service's own module and reach the executable and the
  controller suite from there, and nowhere else.
- **`services/notification/CMakeLists.txt`** — twelve source paths: the eight
  `NOTIFICATION_FEATURE_SOURCES` lines, the executable's `main.cc`, and the
  three places `notification-rpc-service.cc` is compiled (the executable,
  `notification-rpc-test`, `notification-no-nats-test`). No target name and no
  link line changed, so the build graph is the same graph. The section below
  then replaced those twelve paths with two modules.
- **`services/notification/AGENTS.md`** — the `## Layout` block, plus three
  corrections it needed: the `src/server/` line described a directory that
  does not exist (measured: `ls services/notification/src/server` fails, and no
  `src/server/` path appears anywhere in the service), the `src/shared/` tree
  was absent from the block although it holds seven module folders, and the
  sentence below it said the feature sources "compile from the shared tree"
  where they now compile from `src/feature/notification/`.
- **`services/notification/CONTEXT.md:60`** — the RPC-owner line's path.
- **the root `AGENTS.md`** rule 23 paragraph, "Today, against that target",
  which named `notification`'s `feature/rpc/` as the thing step 2 moves. Its
  counts were also wrong and are corrected: the tree has thirteen services, of
  which five now carry `src/app/` (`auth`, `identity`, `notification`, `tts`,
  `sync`), seven keep a single `main.cc` at their `src/` root and `tunnel` has
  two entry points there — the paragraph said "the other nine services keep a
  single `main.cc`", which added up to no tree.

## Rule 25, applied to the two homes this step creates

The move alone left the service in the pre-module build shape: the root
`CMakeLists.txt` held the feature's eleven `.cc` files in a
`NOTIFICATION_FEATURE_SOURCES` variable and handed that list to the
executable and the controller suite, while the RPC source was named again for
each of the two RPC suites. That is the pattern rule 25 forbids — a consumer
listing `.cc` files — and `services/tts`, after step 1, is the reference that
does the opposite. Both new homes became modules:

- **`src/feature/notification/CMakeLists.txt`** — `argus_module(NAME
  notification-feature SOURCES … INCLUDES ../.. DEPENDS …)`. The dependency
  list is measured from the sources' own includes, not guessed:
  `notification-core` (the delivery service and the notification repository),
  `argus::lib::auth` (`device-filter.hxx`, `jwt-filter.hxx`,
  `request-context.hxx`), `argus::lib::config` (`config-service.hxx`),
  `argus::lib::errors` (`validation-exception.hxx`), `argus::lib::http`
  (`api-response.hxx`), `argus::lib::sqlite` (`db-service.hxx`),
  `argus::lib::validation` (`validation_dsl.hxx`), `Drogon::Drogon` and
  `nlohmann_json::nlohmann_json`.
- **`src/app/rpc/CMakeLists.txt`** — `argus_module(NAME notification-rpc …)`
  with its one source and the same exercise: `notification-core`,
  `argus::clients::notification` (the generated stub and the channel base),
  `argus::lib::config`, `argus::lib::nats` (`push-intent-sink.hxx`),
  `argus::lib::text` (`json-util.hxx`) and `Drogon::Drogon`.
- **the root file** discovers feature modules —
  `file(GLOB NOTIFICATION_FEATURE_MODULES CONFIGURE_DEPENDS
  "${NOTIFICATION_SRC_ROOT}/feature/*/CMakeLists.txt")` then
  `add_subdirectory` over it, the same four lines tts uses — adds
  `src/app/rpc`, drops the variable, compiles `app/main.cc` alone into
  `argus-notification` and links `argus::notification-feature`,
  `argus::notification-rpc` and `argus::notification-camera-notification` by
  name. The three suites link their module and `notification-core`; the
  `target_include_directories(... ${NOTIFICATION_SRC_ROOT})` lines they each
  carried are gone, because the module publishes that include root.
- **`-Wall -Wextra` is not the helper's job.** `argus_module` does not add it
  and `argus_service` does (`cmake/argus-module.cmake:419`), so both new
  modules carry the line explicitly, as `tts-synthesis` does. The pre-existing
  `src/feature/camera-notification/CMakeLists.txt` did not — a module
  compiled outside this project's own rule 15 gate — and now does, with no
  warning raised by the change.
- **the feature module links whole-archive, and that is not decoration.** Both
  controllers are Drogon `AutoCreation` controllers: `methodRegistrator`
  registers their routes from a static initializer
  (`drogon/HttpController.h`), so their object files have to reach the
  executable, and Drogon `static_assert`s against registering them by hand
  (`drogon/HttpAppFramework.h`). A plain static-library link therefore drops
  every route **silently**: measured on the first green build of this
  conversion, `nm -C services/notification/build/dev/argus-notification |
  grep -c NotificationController` was **0**, and all 41 suites still passed,
  because a suite that instantiates a controller pulls its object itself
  (`nm -C …/notification-controller-test` → 122 controller symbols). The
  executable links
  `$<LINK_LIBRARY:WHOLE_ARCHIVE,argus::notification-feature>`, the same
  measurement then shows the symbols back, and `scripts/lib/check-deps.py`
  understands the genex as an edge (`resolve()` unwraps
  `$<LINK_LIBRARY:…,item>`, and `tokens_of` keeps the comma-bearing token
  whole), so the tier check still sees it. The alternative — tts's explicit
  `registerController` — is not open here: `services/tts` declares
  `HttpController<TtsController, false>`, and these controllers predate the
  unit as auto-created ones.

## The 2+ rule, measured

Rule 23 says `shared/` holds "exactly the repositories 2+ features read", so
each module under `src/shared/` was measured for its feature readers (`app/`
and tests do not count; the service's two features are `notification` and
`camera-notification`):

| Module | Feature readers | Evidence |
|---|---|---|
| `services/notification` (`NotificationService`) | 2 | `notification-feature-service.hxx:22`, `camera-object-notifier.hxx:48` |
| `repositories/notification`, `schemas/notification` | 2 | through `NotificationService`, and `app/rpc/notification-rpc-service.hxx` |
| `repositories/change-outbox` | 0 direct, 2 indirect | no feature includes it; it is the module behind the change sink, which `app/main.cc:122` installs process-wide (`user_change::setNotificationSink`), so every feature's write lands in it |
| `services/notification-token`, `repositories/notification-token`, `schemas/notification-token` | **1** | `notification-token-feature-service.hxx:5` only — moved into `src/feature/notification/` by this change |

The last row named a divergence rule 23 does not allow: one module family with
exactly one reader. It is a move, not a split — the trio compiled by path into
the executable and the controller suite, never into `notification-core` — so
the three folders went from `src/shared/{repositories,schemas,services}/` to
`src/feature/notification/{repositories,schemas,services}/notification-token/`,
their six include lines followed (`notification-token-controller.cc:8`,
`notification-token-feature-service.hxx:4,5`,
`services/notification-token/notification-token-service.hxx:4,5`,
`repositories/notification-token/notification-token-repository.hxx:5`,
`tests/unit/notification-controller-test.cc:14`), and no target name changed.
After it, `src/shared/` holds exactly the notification family and the change
outbox — four module folders, seven before.

## Review

One adversarial, read-only reviewer ran over the change and returned six
findings. Every one was re-measured here before anything moved; all six are
true, and five of them are fixed in this change.

| # | Finding | Verified how | Outcome |
|---|---|---|---|
| 1 | `packages/clients/identity/AGENTS.md:103` still pointed at `services/notification/src/main.cc:148` | `git ls-files \| grep -v '^docs/history/' \| xargs grep -n 'notification/src/main.cc'` → that one hit; the named content is at line 148 of the new path | fixed — the path spelling, and the referenced line still holds |
| 2 | `src/feature/notification/` had no `CMakeLists.txt` and the root file listed its `.cc` files for two consumers — what rule 25 forbids and tts does the opposite of | `find src/feature -maxdepth 2` → camera-notification declares itself, notification did not; `ls services/tts/src/feature/*/CMakeLists.txt` → the reference does | fixed — the module conversion above |
| 3 | the `src/shared/` line this change added to `AGENTS.md` did not hold for the notification-token family (one reader) | the report's own 2+ table, measured independently before the review | fixed — the trio moved into its single feature and the sentence now says what `shared/` holds |
| 4 | the paragraph this change rewrote still called the notification-token repository/service `notification-core`'s | `sed -n '91,98p' CMakeLists.txt` → core's six sources; the trio compiled into the executable and the controller suite | fixed — the paragraph is rewritten around the modules |
| 5 | the report the plan row cites was untracked and carried three placeholders | `git status --porcelain -- docs/history/`, `grep -ln 'REVIEW_SECTION…'` | fixed — it is staged, complete and referenced by the row |
| 6 | the report said the layout block omitted a `src/shared/` tree "of four modules"; measured seven | `find src/shared -maxdepth 2 -type d` → seven module folders, one of which declared itself in CMake | fixed — "seven module folders", and the post-move four is stated as the result of the token move |

Findings 2 and the whole-archive consequence are the same thread: the review
caught the shape, and the shape's first green build hid a real regression that
the symbol count, not the suite, exposed. Both are recorded above.

The reviewer's own passes are also worth keeping, because they are the
completeness half: no path under a `feature/api/` or `feature/rpc/` segment
survives anywhere in the working tree (including the gitignored
`config.toml`), no `src/server/`, no root `src/main.cc`, no empty directory;
every first-party include resolves; every source path the CMake files named
existed (19 `${NOTIFICATION_SRC_ROOT}/…` plus 11 test/tool paths, counted
before the module conversion moved them into the modules); the moved files are
byte-identical apart from include lines; and no comment, `typedef`, `NULL`,
`std::bind`, `printf` or C-style cast was introduced. Its two gaps are covered
here instead: the reviewer was read-only and ran no build, so the compile, the
41 suites and the tidy gate below are this change's own measurement.

## Verification

- `./scripts/build-all.sh dev --only notification` — 41/41 tests, 0 failures,
  exit 0, and not one `warning:` line. The two pre-build gates ran with it:
  `check-comments` **1329** files checked, 0 comments — two more than this same
  unit measured before the conversion (1327), which is exactly the two
  `CMakeLists.txt` it adds — and `check-deps` **81** declarations, **669**
  edges, 0 forbidden, 0 cycles, 0 unresolved, 23 deferred. (The step-1 run
  reported 1328 for the tree; Phase 4 step 1 added `go2rtc.yaml` to the root
  ignore list and the scanner enumerates `git ls-files --cached --others
  --exclude-standard`, `scripts/lib/comment_scan.py:869`, so the untracked
  native-run artifact left the scan.)
- Both dependency deltas are explained by measurement rather than assumed.
  Declarations 79 → 81 is the two `argus_module` calls. Edges 670 → 669 came
  out of the same scanner twice: re-run over the **staged** pre-conversion root
  file with the two module files in the tree, it reports 682 — that is 670 plus
  the modules' 12 (7 for the feature, 5 for the RPC owner, both measured by
  grouping the edges per package) — so the root file's literal link lists shed
  thirteen edges (52 → 39) while the modules added twelve, and the net is the
  one edge the gate reports. The checker reads both places, which is why a
  conversion from literal lists to `DEPENDS` moves its number at all.
- The `$<LINK_LIBRARY:WHOLE_ARCHIVE,…>` genex is read as an edge, not as a
  third-party mention: `check-deps` still reports 271 third-party mentions over
  22 roots (unchanged), 0 unresolved, and the feature module's edge appears as
  `services/notification → services/notification/src/feature/notification`
  (tier 5 → tier 5, same unit, allowed).
- The AutoCreation regression and its fix, measured on the binaries:
  `nm -C services/notification/build/dev/argus-notification | grep -c
  'NotificationController::'` → **0** with the plain archive link, **120** with
  whole-archive; the controller suite holds 122 in both states, which is why
  the suites never noticed. The rebuild that carries the fix is the one the
  numbers above come from: 41/41, 0 failures, 0 `warning:` lines, exit 0.
- `scripts/check-tidy.sh` over the whole tree — the gate a `--only` run skips
  deliberately: **537** translation units, **2902** findings over 45 checks,
  baseline 2902, exit 0. The count is the baseline exactly, so the conversion
  moved no check's number in either direction, and the module boundary the
  scanner sees is the same set of sources it saw before it.
- The move's completeness, measured directly: no path under a `feature/api/` or
  `feature/rpc/` segment remains in the service, no empty directory survives
  under `src/`, and every source path the CMake files name exists.

## Files

39 changed, 26 of them renames and 3 new files (`git status --porcelain`):
the root `AGENTS.md`, the plan, this report and the two service documents
(`services/notification/{AGENTS,CONTEXT}.md`, `packages/clients/identity/AGENTS.md`);
`services/notification/CMakeLists.txt`;
`services/notification/src/app/{main.cc,rpc/CMakeLists.txt,notification-rpc-service.{hxx,cc}}`;
the twenty-three moved files under `src/feature/notification/` (controllers,
dtos, the write-side services and the token family's
`{repositories,schemas,services}/notification-token/`) plus its
`CMakeLists.txt`;
`src/feature/camera-notification/CMakeLists.txt`; and the three suites in
`services/notification/tests/unit/`. Nothing was deleted outright: every file
`feature/api/`, `feature/rpc/` and the token trio held is staged as a rename,
so the diff a reviewer reads is fourteen include lines, the CMake rewrite and
the documents. The three additions are the two module declaration files and
this report.
