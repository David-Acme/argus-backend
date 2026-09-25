# Phase 4 step 9 — every service's config resolution into `src/config/`

Plan row: `docs/history/plans/architecture-plan.md` "9 | Move every service's
config resolution into `src/config/`, leaving `app/` with `main.cc` and `rpc/`
only, and register the shared advice with one line in `main.cc` (D20)".

## What the row asked for

D20 is one paragraph of the plan's decision list: "**Service config lives in
`src/config/`; `app/` stays composition-only.** A service's typed
configuration resolution (`<svc>-config.{hxx,cc}`: db path, schema path,
ports, feature flags, read through `lib/config`) lives in `src/config/` as a
sibling of `feature/` and `shared/`, so it is not buried inside the process
entry point. `app/` holds `main.cc` and `rpc/` and nothing else, and every
service registers the one shared exception advice with a single line. **There
is no per-service `AppConfig` subclass.**" Two neighbouring lines bound it:
"A feature never reads the environment or a TOML file on its own (D20)"
(line 709) and rule 23's "`app/` is process composition only — `main.cc` and
`rpc/`. No domain logic, no controller, no repository, no config resolution"
(root `AGENTS.md`).

## The tree before the step, measured

Four shapes had to become one:

| Shape at HEAD | Services |
|---|---|
| `src/config/` already, resolution there | `auth`, `identity`, `sync` |
| a domain folder beside `feature/` holding the config **and** the NATS sinks | `camera` (`src/camera/`), `notification` (`src/notification/`), `productivity` (`src/productivity/`) |
| the whole typed config resolved inline in `main.cc` | `guard` (58-field service config + the 25-field belief config), `stt`, `tts`, `vlm`, `llm`, `voice` |
| exempt by design (D19) | `tunnel` — two entry points at its `src/` root |

`tunnel` keeps its `src/server/service-config.{hxx,cc}` where it is: D19 says
the service "stays exactly as it is — no layout work, no contract work, no
phase touches it", so the row's "every service" stops at its boundary.

`app/` was already only `main.cc` and `rpc/` everywhere it existed; what the
row actually moved was the resolution *inside* `main.cc`, plus the three
domain folders that D20 has no place for.

## Wave 1 — camera, notification, productivity: the domain folders go

Each of the three folders held exactly two things the rules do have places
for, and nothing else, so each folder disappeared:

| Service | config → | sinks → | modules |
|---|---|---|---|
| camera | `src/config/{camera-config,operator-config}.{hxx,cc}` | `src/shared/services/change-sink/` | `argus::camera-config`, `argus::camera-change-sink` (was `argus::camera-core`) |
| notification | `src/config/notification-config.{hxx,cc}` | `src/shared/services/{change-sink,delivery-sink}/` | `argus::notification-config`, `argus::notification-change-sink`, `argus::notification-delivery-sink` (was `argus::notification-core`) |
| productivity | `src/config/productivity-config.{hxx,cc}` | `src/shared/services/change-sink/` | `argus::productivity-config`, `argus::productivity-change-sink` (was `argus::productivity-core`) |

`camera`'s `operator-config.{hxx,cc}` came from `feature/operator/`, where it
was the operator feature's resolver for three *other* features' configs
(objects, operator, identity), which is why it belongs to the service rather
than to the feature that happened to spell it.

These three had a config module already, and it was partial: the rest of the
resolution sat inline in their `main.cc`, and that is what grew the files —
`camera`'s gained `resolveListener`, `resolveHealth` and
`resolveGuardCallerSecret` (fifteen lines of `resolveDb` at `HEAD`, the rest
lifted from `main.cc` lines 91, 97 and 215–235), `notification`'s
`resolveListener`, `resolveRpcListener` and `resolveIdentity` with its own
struct, `productivity`'s `resolveListener`. That growth is also why those
pairs score 30–79% similarity in the commit's rename detection, while the nine
files that only moved score 100%.

Two types a feature had declared privately became shared vocabulary in the
same move, because the config module names them: `HealthThresholds` (out of
`feature/monitor/health-event.hxx`) and `OperatorZone` (out of
`feature/operator/event-intelligence.hxx`) are
`src/shared/vocabulary/{health-thresholds,operator-zone}.hxx` and both sides
now include them.

The change sinks are the one class of module in this tree whose reader is
composition: `main.cc` constructs them and installs them into the contract's
sink slot (`camera_change::setSink`, `user_change::setNotificationSink`,
`push_intent::setSink`), and the outbox suites drive them directly. That is
the same indirect-reader shape the notification service had already
documented for its own outbox, and it is why the sinks are
`src/shared/services/` modules and not a feature's.

## Wave 2 — guard: the largest single resolution, and a defect

`main.cc` resolved the whole service config field by field, and
`guard_belief`'s belief weights with it. Both are
`src/config/guard-config.{hxx,cc}` (`argus::guard-config`) now, nine
resolvers wide: `resolveDb`, `resolveListener`, `resolveNotifications`,
`resolveIdentity`, `resolveActions`, `resolveAssessEndpoints`,
`resolveAssessment`, `resolveService` and `resolveBelief(cameraId)` — the
`LOG_WARN` fallbacks and the
`guardModeFromString(configOr("guard.default_mode", "home"))` default
reproduced verbatim, so the service path boots unchanged.

The belief path did not, and the review caught it. `main.cc`'s helper
(`configIntOr` at HEAD line 69) guarded on positivity — `value > 0 ? value :
fallback` — while `guard-belief.cc` had its own presence-based trio
(`if (!ConfigService::hasKey(key)) return fallback;`). Merging them into one
module kept main.cc's policy, so `resolveBelief` silently replaced every
belief key an operator set to `0` or a negative number with the struct
default:
the penalty weights (`weight_identity_known`, `weight_track_jitter`,
`weight_detector_weak`, …) are negative by design, so the keys could not be
tuned at all, and `config.toml.example`'s own negatives survived only because
they equal the defaults. Nothing failed — no assertion in
`services/guard/tests/unit/guard-belief-test.cc` set a weight key. The fix
restores HEAD's semantics with three belief-local helpers (`beliefIntOr`,
`beliefInt64Or`, `beliefDoubleOr`) used by `resolveBelief` alone, and a new
case (`belief keys are honoured whatever their sign`) pins a negative
per-camera weight, a zero weight and a negative global weight, plus the
untouched default on a second camera. Once the helper names are normalised,
the fixed resolver's body — field, key and fallback, in that order — is
identical to the pre-step `resolveBeliefConfig`.

Three types moved to `src/shared/vocabulary/`: `guard-mode.hxx` (from
`feature/guard/vocabulary/`, the 2+ rule now that the config module and the
feature both read it), and `belief-gate-scope.hxx` + `belief-config.hxx`
(extracted from `guard-belief.hxx` byte for byte). Every public spelling
survives: `GuardService::Config` and `GuardAssessment::Config` are in-class
aliases of the config module's structs, and `BeliefConfig`/`BeliefGateScope`
keep their global names, so no consumer's expression changed.

**Guard was the one service whose `main.cc` never registered the shared
advice** — no `setExceptionHandler(ErrorHandler::handleException)`, no
`setCustomErrorHandler(ErrorHandler::unmatchedRoute)`, so a refusal thrown in
a guard handler was not the `{status, info, errors}` envelope. Measured
across the tree before the fix: eleven of the twelve services and both of
`tunnel`'s entry points registered it, and guard's entry point was the only
one missing. The two lines are
in `services/guard/src/app/main.cc` now. Its hand-rolled `/health` stays: the
deviation is recorded in `packages/lib/http/AGENTS.md`, and no compose
healthcheck polls guard.

## Wave 3 — the five AI services: the inline resolution leaves `main.cc`

| Service | module | resolvers | port |
|---|---|---|---|
| `stt` | `argus::stt-config` | `resolveListener()`, `resolveRpc()` | 7030 |
| `tts` | `argus::tts-config` | `resolveListener()`, `resolveRpc()` | 7029 |
| `vlm` | `argus::vlm-config` | `resolveListener()`, `resolveRpc()` | 7031 |
| `llm` | `argus::llm-config` | `resolveListener()`, `resolveRpc()`, `resolveIdentity()`, `resolveCameraTarget()`, `resolveMemory()` | 7032 |
| `voice` | `argus::voice-config` | `resolveHealthListener()`, `resolveGrpcListener()` | 7035 / 7034 |

The four gRPC-capable services share one resolver shape — `resolveRpc()`
returns `{address, credentials}` from `rpc.address` and
`ConfigService::getStringPairs("rpc.callers")`, with empty pairs erased — and
`main.cc` keeps the boot gate
(`if (!rpc.address.empty() && !credentials.empty())`)
that decides whether the internal leg is registered. `voice` is the one
service without an `app/` (its entry point is `src/main.cc`, D19's neighbour
case): its two listeners were two lines in `main.cc` and are two resolvers in
`src/config/` now.

Each service's root `CMakeLists.txt` gained an explicit
`add_subdirectory(src/config)` — parents auto-discover only
`feature/*/CMakeLists.txt` — and the executable names the module
(`argus_service(… MODULES argus::stt-config …)`), with the AI service's
module listed first, the way camera's `argus::camera-config` already was.

## The technique the move needed: config must not depend on a feature

A config module that returns a struct the feature declares would have to
`DEPENDS` the feature's module, and the feature's module already `DEPENDS`
the config module — a CMake cycle and a rule-25 violation. Two shapes avoid
it, and both were used:

- **move the type down**: `HealthThresholds`, `OperatorZone`, `GuardMode`,
  `BeliefGateScope`, `BeliefConfig` are `src/shared/vocabulary/` types now,
  read by both sides (rule 23's 2+ rule, applied to types rather than files);
- **alias up**: the feature keeps its spelling with `using Config =
  GuardServiceConfig;` — the type lives where the rules put it, the name
  consumers already write keeps working, and designated-initializer
  construction is untouched (inheritance would have broken it, which is why
  the alias and not a base class).

## What stays in `main.cc`, measured rather than assumed

The row says "config resolution", not "every read of a key", and three kinds
of read are not that. All three are deliberate and were measured across the
tree before being left:

- **boot itself**: `ConfigService::load("config.toml")` and
  `ConfigService::drogonConfig()` in all fourteen entry points — the twelve
  services' entry points and both of `tunnel`'s — the file has
  to be read before anything can be resolved.
- **the `nats.url` gate**: nine entry points read it — the eight services that
  build the optional `NatsBus` plus `tunnel`'s relay — to decide whether to
  build that bus. The key is the bus's own —
  `packages/lib/nats/src/nats/nats-bus.cc:82` resolves it again inside
  `connect()` — so a per-service resolver would duplicate the bus's
  resolution to re-answer the same question.
- **`identity`'s `setRuntimeString("database.file", identityDb.dbPath)`**: a
  publication of the resolved path, not a resolution.

One case is weaker and is recorded as such: `camera`'s `main.cc` reads
`operator.cooldown_ms` and `operator.outbox_max_pending` to fill
`NatsObjectEventSink::Config` where that optional sink is built, beside the
three fields it hardcodes (`retryMs = 500`, two empty strings). Moving them
into `camera-config` would split one struct's construction across two files
and would make the config module depend on the sink's header; the honest
boundary is the sink's own `Config` type, and it is left with that note.

**Features still read single keys at use time, and that was left alone.**
Measured over `src/` excluding `src/config/`, `src/app/` and the entry
points, files that call `ConfigService::get*`/`hasKey`:

| Service | files | calls |
|---|---|---|
| `llm` | 8 | 54 |
| `camera` | 5 | 22 |
| `identity` | 4 | 6 |
| `voice` | 3 | 13 |
| `notification` | 2 | 14 |
| `sync` | 2 | 2 |
| `tts` | 2 | 11 |
| `stt` | 1 | 3 |
| `vlm` | 1 | 10 |
| `auth`, `guard`, `productivity` | 0 | 0 |

Most of those are the *right* shape already — a feature-local resolver such
as `camera_notifier::resolveConfig()` or `push_intent::enabledFromConfig()`
reads its own keys in the feature that owns them, which is what D20's "a
feature never reads the environment or a TOML file on its own" asks for. What
the row scoped was the boot-time resolution `main.cc` performed, and that is
gone from every service. The remainder — in-feature single-key reads that are
not a resolver — is beyond this row and is listed under open items.

## Adversarial review

Two read-only reviewers, one over the extraction's fidelity against `HEAD`
(key by key, port by port, guard by guard) and one over the rules and the
documents, and every finding was re-measured here before anything moved.

The one that mattered was the belief guard, found independently by both: the
service path and the belief path never shared a fallback policy, and merging
them into one module applied main.cc's positivity test to weights that are
negative by design. Fixed, with a case pinning it (see Wave 2 above).

The rest were claims, and the claims were the interesting half:

- the report's "so boot behavior is unchanged" was true of the service path
  and false of the belief path — corrected where it stood;
- root rule 23 listed `guard` among the services that keep nothing at `src/`
  level but `app/`, `config/` and `feature/` **and** among those with a
  `src/shared/`, and it left `auth` — which does keep exactly those three
  — out of both groups. Measured with `ls` over the twelve trees and
  rewritten: four keep only the three, seven carry `shared/`, and `voice`
  carries `shared/` and `test-support/` without an `app/`;
- the same paragraph's "every service with a source tree has `src/config/`"
  is false for `tunnel`, which D19 exempts; the carve-out is in the sentence
  now;
- the report said "thirteen entry points" for the boot reads where the tree
  has fourteen (measured by grepping `ConfigService::drogonConfig()` over
  `services/`), and the same bullet's `nats.url` gate is nine entry points,
  not nine services — `tunnel`'s relay is one of them;
- `services/voice/AGENTS.md`'s layout block described the root
  `CMakeLists.txt` without the module the step added to it, and
  `services/voice/CMakeLists.txt`
  spelled `argus_voice-core` by hand on a line the step edited — both fixed,
  the second through the alias every sibling uses.

Recorded as deliberate rather than fixed: guard's newly registered error
advice is a wire-visible change (its refusals are the `{status, info, errors}`
envelope now, and framework errors answer `ErrorHandler::unmatchedRoute`) —
that is the row's own instruction, "register the shared advice with one line
in `main.cc`", and its absence was the defect; and `camera`'s two `operator.*`
reads in `main.cc` stay where the sink's own `Config` type is built.

## Documents updated

Every doc that named a moved path, and the root rule that summarized the
tree:

- unit layout blocks gained their `src/config/` line: `stt`, `tts`, `vlm`,
  `llm`, `guard` (+ `src/shared/vocabulary/`), `voice`, `camera`,
  `notification`, `productivity`;
- `camera`, `notification` and `productivity` `CONTEXT.md` gained a "Phase 4
  step 9" section and had their stale present-tense claims amended
  (`camera-core`, `notification-core` and their `src/<domain>/` paths are
  named as the pre-step spellings, not as the tree);
- `guard/AGENTS.md`'s "one feature and one module" became "one feature and
  two modules";
- root `AGENTS.md` rule 23's "Today, against that target" paragraph was
  rewritten against the tree as it is now: every service with a source tree
  has `src/config/`, only `voice` still keeps code beside `feature/`
  (`src/test-support/`), eight keep a `src/shared/`, and the count of
  services with `src/app/` was corrected to eleven — the old text said twelve
  and listed eleven.
- the five AI services' `CONTEXT.md` gained the section naming their
  resolvers and ports.

## Verification

The orchestrator's gates on the final tree, then the two projects whose
sources the review's fix touched, re-verified on their own.

`./scripts/build-all.sh dev` — **15 projects, all green, 0 errors, 0
warnings** (`-Wall -Wextra` over every first-party translation unit), and 458
registered test executables passing:

| Project | Tests | Project | Tests | Project | Tests |
|---|---|---|---|---|---|
| cert | 2 | notification | 41 | vlm | 17 |
| sqlite | 2 | guard | 57 | llm | 44 |
| auth | 31 | tts | 29 | voice | 32 |
| identity | 36 | stt | 15 | tunnel | 15 |
| sync | 49 | camera | 53 | productivity | 35 |

The two gates that run before anything is built, re-run on the final tree
after the last edit: `scripts/check-comments.sh` — 1401 files checked, 0
comments; `scripts/check-deps.sh` — 135 declarations, 911 edges, 0 forbidden,
0 cycles, 0 unresolved, 298 third-party mentions over 20 roots.

After the belief fix and the `argus::voice-core` alias, `guard` and `voice`
were rebuilt and re-tested on their own — each exit 0, 0 warnings, 57 and 32
tests green — and the belief suite was driven by case and in randomized order
(15 cases, 105 assertions, 0 failed), so the new case runs and its runtime
overrides do not leak into the cases around it whichever order they take.

`scripts/check-tidy.sh` was re-run over the final tree after the last edit:
553 TUs, 2873 findings over 45 checks against a 2901 baseline, 11 checks below
it, worst file `services/guard/src/feature/guard/guard-repository.cc` (104) —
the gate exits 0, and the three presence-based helpers moved no count in
either direction, so the fix is invisible to rules 16 and 19.
