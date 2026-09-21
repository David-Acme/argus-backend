# Phase 1 step 10 — the refusal vocabulary gets its own package, the envelope gets a home, and mdns comes back

Scope: the row is "Create `lib/errors` (from `response`) and `lib/mdns` (extracted from the
gateway); `lib/http` gathers `api-response`, `error-handler`, `cors` and the health controller —
the `config/app-config.{hxx,cc}` split of §2.3, renaming the class out of `AppConfig` and
collapsing the `getNNNResponse` builders into the advice". The unit is three packages plus the
config split that feeds one of them, and the class removal that the split forces. Base
`2b08c3f`.

Measured against the tree, the row's three destinations are four — `errors`, `http`, `config`
(which keeps the TOML reader and loses two files) and `mdns` — and its "5 files → `errors`,
4 → `http`" split (the plan's D-table, `architecture-plan.md:961`) counts only `response`'s own
files, because the health controller and the listener config were never inside it. And the row
cannot be executed without executing row 13: deleting `AppConfig` deletes the last declaration
of the error codes and of the sync page size, so their single home has to exist first.

## What the tree says about the row's premise

`packages/response` at `HEAD` is **11 files, 724 lines**, and every one of them has a
destination:

| `packages/response` @ `HEAD` | lines | destination |
|---|---|---|
| `src/error-code.hxx` | 49 | `packages/errors/src/errors/error-code.hxx` |
| `src/error-definition.hxx` | 11 | `packages/errors/src/errors/error-definition.hxx` |
| `src/response-exception.{hxx,cc}` | 40 + 57 | `packages/errors/src/errors/` |
| `src/validation-exception.hxx` | 26 | `packages/errors/src/errors/` |
| `src/http/api-response.{hxx,cc}` | 37 + 80 | `packages/http/src/http/` |
| `src/config/app-config.{hxx,cc}` | 69 + 134 | deleted: `cors` and the advice → `packages/http/src/http/{cors,error-handler}`, the class and its builders → nothing |
| `tests/unit/api-response-test.cc` | 187 | `packages/http/tests/unit/api-response-test.cc` (252 lines: re-pointed and extended) |
| `CMakeLists.txt` | 34 | deleted, `packages/errors` and `packages/http` carry their own |

D17 is the reason the split is two packages and not one: "the error substrate is **not**
HTTP-only (contracts, `storage`, and several services consume it)" — so the vocabulary
(`ErrorCode`, `ErrorDefinition`, the typed exceptions) is a leaf with no Drogon in it, and the
envelope, the advice, CORS and the health route sit above it in the package named after the
transport, `http`, the counterpart of the existing `lib/grpc`.

**Two of §2.3's four claims do not survive measurement.** It says "All four live today inside
`config/app-config.{hxx,cc}`": `cors` and `error-handler` do (as `AppConfig::applyCors`,
`handleOptions` and `handleException`), but `api-response` is its own file in `response`, and the
health controller is `packages/config/src/controllers/health-controller.{hxx,cc}` — a file in
`config`'s tree, not a member of the class. The listener config is the same:
`packages/config/src/server/listener-config.{hxx,cc}`. That is why this step touches `config`'s
tree as well as `response`'s, and why the row's "and the health controller" is incomplete — the
listener config moves with it (D-table `architecture-plan.md:950` says so for `config`).
§2.3's counts are also low by one each: it predicts the include-path change in "18 files" and
"14"; at `HEAD`, 19 code files include `<controllers/health-controller.hxx>` (the 20th match is
`packages/config/CMakeLists.txt` listing its own sources) and 15 include
`<server/listener-config.hxx>` (the 16th is the same `CMakeLists`). After this step: 21 and 16 —
the three extra direct includes are the transitive ones the checker below flushed out.

Nothing else in the row's list is missing, but §2.2's `lib/http` also mentions "route
registration"; the row does not, and nothing in this tree registers routes from a shared place
(every service calls `drogon::app().registerController(...)` itself, and the gateway adds the
proxy plugin). Recorded as a finding, not invented.

## Where every member of `AppConfig` went

The class is the step's completeness test: it must not exist afterwards, and nothing it held may
be lost. `git show HEAD:packages/response/src/config/app-config.hxx` names nine members and the
builders; the tree says:

| `AppConfig` member | new home |
|---|---|
| `applyCors`, `handleOptions` | `Cors::apply`, `Cors::handleOptions` — `packages/http/src/http/cors.{hxx,cc}` |
| `handleException` | `ErrorHandler::handleException` — `packages/http/src/http/error-handler.{hxx,cc}` |
| the custom 404/405 error handler | `ErrorHandler::unmatchedRoute(status)`, the only place that hand-builds an envelope, because no exception exists to carry it |
| `get400/401/403/404/405/409/429/500/502/503Response`, `getRemoteNotAllowedResponse` | **deleted.** Each was a status, a code and a default message; every one of them is a catalog row now, thrown (`ErrorDefinition`) and formatted once by the advice |
| `JWT_CTX_KEY`, `DEVICE_CTX_KEY` | `AuthContext::kJwtKey`, `AuthContext::kDeviceKey` — `packages/contracts/auth-contract/request-context.hxx` |
| `REMOTE_CTX_KEY` | `RemoteGate::kRemoteContextKey` — `services/gateway/src/server/remote-gate.hxx` |
| `SYNC_LIMIT{"200"}` | `SyncLimits::kMaxRows` — `packages/contracts/sync-contract/src/shared/contracts/sync-limits.hxx` |
| the 11 `ERROR_CODE_*` strings | `ErrorCode` — `packages/errors/src/errors/error-code.hxx` |

Counted at `HEAD`, the sweep had to move **188 `AppConfig::` mentions across 50 files** and
**92 `getNNNResponse(...)` call sites across 38 files** (both counts exclude `packages/response`
itself). Verified after the sweep: `git grep "AppConfig"` over `*.cc`, `*.hxx`, `*.h` and
`CMakeLists.txt` is **empty**; `git grep "get400Response\|get500Response\|get503Response\|
getRemoteNotAllowedResponse"` is **empty**; `app-config` survives only in `docs/history/**` and
in one historical sentence of `docs/architecture/camera-guardian-deep-analysis.md:942` (flagged,
below).

The builders are gone rather than re-homed, which is §4.7's whole point: a handler that wants a
refusal **throws** it, and the code, status and message come from the catalog entry. Exactly two
answers are still built by hand, both in `packages/http`, both because the framework asks for them
with no exception to carry them: an unmatched route's 404 and 405
(`ErrorHandler::unmatchedRoute`, which goes through `ApiResponse::error` like everything else) and
the OPTIONS answer (`Cors::handleOptions`, which is not an envelope at all — an empty 200 carrying
only the CORS headers, kept line for line from `AppConfig::handleOptions` at `HEAD`, which also
built it without `ApiResponse`).

## The three packages

- **`packages/errors`** (5 sources, 234 lines with the CMakeLists). A pure leaf: `argus_module`
  with `INCLUDES src` and **no `DEPENDS` at all** — no Drogon, no JSON, no HTTP type — so a
  contract can declare its refusals without dragging a framework in. `ErrorCode` is the enum —
  and it grew by four codes that at `HEAD` existed only as string literals where the refusal was
  raised (`CAMERA_UNREACHABLE` in the camera control controller, `LLM_NOT_LOADED`,
  `STT_NOT_LOADED` and `VLM_NOT_LOADED` in the three model services), because the collapse is
  meant to leave one declaration per code; the catalog *rows* for those four are still
  service-local (last finding, below). `ErrorDefinition` is "the code a client switches on, the
  status the envelope carries and the text a human reads" plus `withMessage()` and `wireCode()`,
  `ResponseException` is the one way to refuse — five constructors, four of which delegate to the
  fifth (`ResponseExceptionInput`, the shape a refusal off the wire has): a catalog entry, a
  catalog entry with a status override, a list of wire records, a bare message, and the input
  struct itself — and `ValidationException` is the 422 case. Its suite,
  `tests/unit/error-definition-test.cc` (139 lines, new), pins every code's wire string and the
  three shapes a refusal can take; the definitions themselves are asserted in the contracts that
  declare them.
- **`packages/http`** (11 sources, 520 lines with the CMakeLists and the suite). `DEPENDS
  argus::errors argus::config Drogon::Drogon`. It holds `ApiResponse` (the only place an
  `{status, info, errors}` is built), `ErrorHandler` (the one advice, D20), `Cors`,
  `HealthController` (+ `HealthStatus`), `ListenerConfig` and `http-errors.hxx` — the four
  definitions for what the substrate refuses on its own (path not found, method not allowed,
  validation failed, internal error), which nothing domain-shaped may name. Its suite is the
  moved `api-response-test`, re-pointed and grown from 187 to 252 lines.
- **`packages/mdns`** (2 sources, 669 lines). The whole move is **two include lines**:
  `#include <drogon/drogon.h>` became `#include <trantor/utils/Logger.h>` (the file uses only
  `LOG_*`, which is trantor's logger, the convention every other first-party file follows), and
  the header is byte-identical (`diff` empty). `mdns::mdns` is `PRIVATE` to the new target, so
  no consumer inherits the vendored `mdns.h` or the SYSTEM-include workaround that used to live
  in the gateway's `CMakeLists`; `argus::mdns` depends on `argus::config` because the responder
  reads `mdns.enabled`/`name`/`service_type`/`port`/`txt`.

`packages/config` keeps the TOML reader and its `<shared/services/config-service/config-service.hxx>`
include path (97 consumers), loses the two HTTP-facing files, and its `CMakeLists.txt` records
why it now depends on neither `argus::http` nor `argus::errors`.

Include convention, settled by this step: own header quoted; cross-package by package path
(`<errors/…>`, `<http/…>`, `<mdns/mdns-service.hxx>`); contract headers bare
(`<camera-errors.hxx>`, `<auth-errors.hxx>`, `<request-context.hxx>`) because those packages put
their root on the include path.

## The row 13 overlap, disclosed

Row 13 ("Remove the error vocabulary duplicated in the HTTP config") is this step's by-product,
not its goal, and it is executed here for three reasons:

1. `AppConfig::ERROR_CODE_*` cannot outlive the class, so the codes had to collapse into the one
   `ErrorCode` declaration — which is row 13's first half.
2. `AppConfig::SYNC_LIMIT` had to move to `contracts/sync-contract` — row 13's second half. It is
   `SyncLimits::kMaxRows`, and the sweep had already rewritten its **11 call sites** (the audit,
   user-audit and notification repositories) to that name, so the header had to exist for the
   tree to compile at all. The constant is a C string (`inline constexpr char kMaxRows[]{"200"}`)
   and not a `std::string_view` because `operator+(std::string, std::string_view)` does not exist
   in C++20 and every call site appends it to a `std::string`.
3. Row 13's third item — the citations in the frozen-wire documents — moved with the values:
   `docs/architecture/wire-sync-tables.md` now names
   `packages/contracts/sync-contract/src/shared/contracts/sync-limits.hxx` for `SYNC_LIMIT` and
   `…/table-name.hxx` for `TableName` (keeping the `HEAD 5970173` anchor as provenance);
   `packages/contracts/proto/argus/sync/v1/contracts.proto` names `table-name.hxx` and
   `SyncLimits::kMaxRows`; `proto/argus/common/v1/base.proto` names `packages/errors` for the
   error codes; and `packages/contracts/AGENTS.md`'s list of C++ sources the protos mirror is
   `(sync-operation.hxx, table-name.hxx, error-code.hxx)`. The frozen values are untouched: no
   number, code or table id in any of those documents changed.

## The incident this step carries, and what the recovery left behind

Mid-step, a bare `git checkout -- services packages` discarded the step's unstaged edits while
the staged renames kept the **old** file contents under the new paths. The recovery re-applied
the sweep's rewrites, and the defects it left behind are worth recording because most of them
only showed up in a build:

1. **19 files included the same header twice** (`<errors/response-exception.hxx>`): the sweep
   added the new include at the top of the block *and* rewrote the old bare
   `<response-exception.hxx>` line in place. Removed by
   `~/.cache/argus/dedup-includes.py`, which keeps the first (sorted-position) occurrence and
   deletes only later duplicates — deletions only, reviewed per file. The same defect in a
   different spelling survived that pass, because it compares whole lines: a file including its
   **own** header twice, once quoted and once by package path. `packages/http/src/http/cors.cc`
   and `error-handler.cc` both did (`"cors.hxx"` next to `<http/cors.hxx>`), found by a rule
   added to the checker for exactly this shape. Adding it also repaired a dead counter: the
   checker's `unqualified_total` — the "bare moved header" count — was incremented nowhere, so
   its summary line said zero whatever the tree held. Both rules were then tested against
   fixtures that violate them, and the summary is now `0 files with a missing include, 0 with an
   unused one, 0 with a stale one, 0 with a bare moved header, 0 including their own header
   twice` — with all five counters able to move.
2. **Six files lost an include and did not get a replacement**: the sweep's signature here is
   that it rewrote the whole include block, so anything in it that the sweep had no opinion about
   was dropped. `services/gateway/src/server/remote-config.hxx` is the smallest case — it lost
   `#include <server/listener-config.hxx>` instead of having it re-pointed, so `ListenerConfig`
   (held in `AppendRemoteListenerInput` and `requireDistinctTunnelPort`) was undeclared, and the
   first full gate run failed exactly there, `gateway-core` only, with `'ListenerConfig' does not
   name a type` in `remote-config.{hxx,cc}`. Audited rather than patched one by one: every file
   the step touched (`git diff --name-only HEAD`) compared with its `HEAD` revision by include
   **basename** found 56 files that lost a basename, of which 49 lost exactly `app-config.hxx`
   (the sweep's purpose) and six lost something else:

   | file | lost | verdict |
   |---|---|---|
   | `contracts/response-contract/response-rpc.cc` | `response.pb.h`, `<algorithm>`, `<string_view>`, `<utility>` | restored — 39 × `'v1' does not name a type`, caught at camera |
   | `clients/tts-client/tts-client.cc` | `grpc-client-base.hxx`, `tts.grpc.pb.h`, `response-rpc.hxx`, `<cmath>`, `<utility>` | restored — the stub, the base class and `fromRpcStatus` are all undeclared without them |
   | `services/voice/src/main.cc` | `drogon/drogon.h`, `<memory>`, `<string>` | restored — `drogon::app()`, `std::unique_ptr`, `std::to_string` |
   | `identity/…/auth/dtos/{login,register}-dto.cc` | `drogon/HttpTypes.h` | **not** restored: dead. The only `drogon::` name in either file is `MultiPartParser`, declared by `drogon/MultiPart.h` in the file's own header. Measured, not argued: `identity` configured, built and passed in the run that failed later, at `camera` |
   | `clients/tts-client/tts-client.hxx` | `response-exception.hxx` | **not** restored: dead at `HEAD` too — the header names no member of the vocabulary, and its body is byte-identical to `HEAD`'s |
   | `services/voice/src/main.cc` | `api-response.hxx` | **not** restored: dead at `HEAD` too — the file used `AppConfig::get405Response()`, never `ApiResponse` |

   The three restored files are the ones whose loss was invisible to every check but the
   compiler: two of them sit in projects (`tts`, `voice`) that the gate had not reached in either
   run, so they were never in an error message. This is the reason the gate is run on the
   finished content rather than once at the start, and the reason the basename audit exists: a
   `--only <project>` run proves one project, not the tree.
3. **Four files lost the blank line after their own header** (`#include "own.hxx"` immediately
   followed by the next include): `auth/filter/valid-json/valid-json-filter.cc`,
   `identity/…/{login,register}-dto.cc`, `notification/…/notification-ack-dto.cc`. Cosmetic, and
   restored from `HEAD`; found by the same audit, which reports a file whose first two lines are
   both includes.
4. **`packages/identity/tests/unit/device-credential-test.cc` aborted** (SIGABRT, 5 runs out of
   5) because §4.7 turned four of its expectations into throws: the test drives
   `JwtFilter::doFilter` through `drogon::sync_wait` directly, so the filter advice never runs
   and the `ResponseException` escaped the test body; the joinable `runner` then called
   `std::terminate` during unwinding, before doctest could report anything. Diagnosed under gdb
   (breakpoint on `std::terminate`, frame 3, `runner` the aborting object). Fixed on the test's
   side with a `refusalOf(task)` helper that catches the exception and reads status, code and
   message off it — the design is right (a filter throw **is** caught when the framework drives
   the filter, `HttpFilter.h:100-128`) and only the direct-drive test needed to change: the four
   sites now assert `401`/`UNAUTHORIZED`/`Device mismatch` and `401`/`Authentication required`.
   No other test in the tree drives a filter (`grep doFilter` over every suite).

## The review: what it found, and what it confirmed clean

The step closed with a read-only review of the whole diff, whose findings were verified one by one
against the tree before anything was changed. Five were real; all five are fixed above.

1. **`argus::http` was added to three services and never linked.** `services/{voice,tts,tunnel}`/
   `CMakeLists.txt` each guard `add_subdirectory(packages/http)`, and **no target in any of the
   three named `argus::http`**: `add_subdirectory` defines a target, while the include root travels
   with a link edge (`argus_module` links its `DEPENDS` `PUBLIC`, `cmake/argus-module.cmake:33-35`).
   At `HEAD` those files reached the root transitively, through `argus::config`'s `DEPENDS
   pkg::response` (`git show HEAD:packages/config/CMakeLists.txt:32`), which this step deleted —
   the accident D17 exists to remove, and the one the three services still leaned on. Nine
   translation units compile against `<http/…>` there: `services/voice/src/main.cc`,
   `services/tts/src/app/main.cc`, `services/tts/…/tts-controller.cc`,
   `services/tunnel/src/{main-client,main-relay}.cc` and
   `services/tunnel/src/server/health-extras.{hxx,cc}` — the last four projects of the gate's fixed
   order, which the failing run never reached because it stopped at `camera`. Fixed by declaring
   the edge where each file is compiled: `argus_voice-core` (`argus::http`), `tts-synthesis-http`
   (`argus::http`, which `argus-tts`'s `MODULES` then pass on) and `tunnel-core` (`PUBLIC
   argus::http`, beside `argus::config`). The review's sweep named three services; auditing every
   project that compiles an `<http/…>` include against its own `CMakeLists` found one more of the
   same shape on the other side of the line: `gateway-core` reached the root **transitively**
   through `argus_identity` (which links `argus::http` `PUBLIC`), so its six files compiled while
   its link list never named the package. It now does, and the invariant is one line to check:
   every one of the eleven services and two packages that include `<http/…>` names `argus::http`.
2. **Two refusals changed the message a client reads.** The envelope carries
   `errors.message` on the wire, and two catalog rows were written with wording that differs from
   the builder call they replace: `IdentityErrors::InvalidMultipartForm` said "Body must be a
   multipart form" where both call sites passed `"Invalid multipart form"`
   (`HEAD:packages/identity/src/feature/api/auth/controllers/auth-controller.cc:20,37`), and
   `IdentityErrors::ServerAlreadyPaired` said "Server is already paired" where
   `pairing-controller.cc` passed `"Server already paired"`. Status and code were never in
   question; the text was, and it is restored to `HEAD`'s. The frozen probe recordings
   (`services/gateway/tools/probe-captures/`) hold `HEAD`'s text in ten files, so the captures and
   a fresh probe now agree on that line again; nothing else in those captures was compared here.
3. **Two include edges were undeclared** — the same accident, smaller. `packages/mdns`'s public
   header carries a `Json::Value` and its `.cc` logs through trantor, both reachable only because
   `argus::config` links `Drogon::Drogon`; and `packages/memory/src/memory/memory-dto.cc` names
   `ValidationException` with `argus::errors` reachable only through `argus::validation`. Both now
   declare what they use (`Drogon::Drogon` on `argus::mdns`, `argus::errors` on `memory-core`).
4. **`AGENTS.md` had three leftovers**: the file table still listed `packages/response/src/http/`
   as "Standardized API response builder" (a row the new `packages/errors`/`packages/http` rows
   already cover, so it is deleted, not re-pointed) and two lines still spelled the attribute keys
   `DEVICE_CTX_KEY` / `JWT_CTX_KEY`, which no longer exist — `AGENTS.md:280,287` now name
   `AuthContext::kDeviceKey` / `AuthContext::kJwtKey`, matching the code they document.

What the review confirmed clean, independently of the author: every moved file is byte-identical to
its `HEAD` original except the intended edits (`health-controller.{hxx,cc}`,
`listener-config.{hxx,cc}`, `validation-exception.hxx` and `mdns-service.hxx` all `diff` empty);
`cors.cc` and `error-handler.cc` reproduce `AppConfig::applyCors`/`handleOptions`/`handleException`
line for line, including the untyped tail that routes through
`HttpErrors::InternalError.withMessage(e.what())`; comparing all **92** deleted-builder call sites
against the rows they now throw, only the two messages above differ — no status and no code
changed, and where a call site passed no argument the row carries the builder's default verbatim;
`ErrorHandler::unmatchedRoute` branches exactly as the 15 deleted lambdas did, and no call site
ever passed an argument, so the two defaults are the whole contract; `packages/errors` is a
genuine framework-free leaf (its sources include only std and its own headers); and no occurrence
of `AppConfig`, `getNNNResponse`, `pkg::response`, `packages/response`, `JWT_CTX_KEY`,
`DEVICE_CTX_KEY` or `REMOTE_CTX_KEY` survives anywhere in code, CMake, scripts, Dockerfiles or CI
— the single remaining spelling is the deliberate prose comment in `error-definition.hxx:10`.

## Verification

- **The phase gate: `./scripts/build-all.sh dev` exits 0 on all 18 projects**, every suite green
  (`~/.cache/argus/gate-r10-final3.log`, run once on the finished content). No first-party warning
  in the run: the only `Warning:` lines are llama.cpp's and ncnn/glslang's `CMAKE_CXX_STANDARD`
  notices, conan's `ccache not found` note and openfst's, the same classes the phase has recorded
  since step 4.
- **The reached-test ledger moves 289 → 302, and the whole of the difference is the split itself**,
  which is what a move that invents no test should look like. `error-definition-test` — the new
  `packages/errors` suite — joins the **sixteen** projects that build the vocabulary, and
  `api-response-test` leaves the **three** that build the vocabulary but no longer the envelope:
  `socket`, `sqlite` and `memory` built `packages/response` at `HEAD` for its error codes and got
  the envelope suite with it. 16 − 3 = +13, and no other project's set of tests changed:
  cert 16, socket 8, sqlite 1, identity 16, sync 20, memory 16, intent 4, gateway 26, camera 35,
  productivity 24, notification 29, guard 37, tts 10, stt 4, vlm 6, llm 25, voice 14, tunnel 11.
  (`intent` is the one project that gained nothing — it builds neither suite.) The three
  subtractive projects are the receipt for D17: what they needed was the vocabulary, and they no
  longer drag the transport in behind it.
- The projects the step touches, run before the gate: `--only gateway` exit 0 (26/26) after the
  mdns re-point; `--only camera` exit 0 (35/35) and `--only productivity` exit 0 (24/24) after the
  two suites that aborted under SIGABRT were fixed; `--only tts` (10/10), `--only tunnel` (11/11)
  and `--only voice` (14/14) exit 0 after `argus::http` was declared — the three that had been
  compiling `<http/…>` against a package they never linked, and whose suites had therefore never
  run at all.
- The `argus::http` invariant, one line to check and now true across the tree: every one of the
  eleven services and two packages that compiles an `<http/…>` include names `argus::http` in one
  of its `CMakeLists`.
- `~/.cache/argus/check-includes.py` (extended with `ListenerConfig`, `HealthController`,
  `HealthStatus`, `MdnsService` and `RemoteGate`, and with its symbol matching turned from
  substring to token — `cameraHealthStatus` is not `HealthStatus`): **0 files with a missing
  include, 0 with an unused one, 0 with a stale one, 0 with a bare moved header, 0 including their
  own header twice**. It found the three files that got a symbol transitively instead of by name
  (`remote-config.cc`, `tunnel/src/server/health-extras.cc`,
  `tunnel/tests/tunnel-health-test.cc`), each of which now names the header it uses.
- The two restored messages are the whole of the wire difference: a grep of every `*.cc`/`*.hxx`
  outside `docs/history` and the probe captures for `multipart form` and `already paired` returns
  the two catalog rows and nothing else — no test and no script asserts either text.

## Findings outside this unit

- **`docs/architecture/camera-guardian-deep-analysis.md:942` still names `AppConfig`** as the
  thing controllers use for responses ("use the standard filters, DTO validation and `AppConfig`
  responses"). It is a live architecture document, not history, and the class is gone. The
  sentence is one of a list of coding rules and the replacement is `ResponseException` +
  `ErrorHandler`; left for the documentation sweep because the whole file is a dated analysis
  whose other bullets have their own drift.
- **The root `AGENTS.md` names a package that no longer exists**: `AGENTS.md:37` ("Every DB column
  with a CHECK constraint … MUST use its `enum class` from `packages/access/src/shared/enums.hxx`")
  and `AGENTS.md:677` (the same path in the file table). `packages/access` was deleted in step 4
  (`62b841c`, "distribute the access vocabulary and delete the package") and the vocabulary now
  lives in the packages and contracts that own each domain. Pre-existing, not this step's, and
  left because the replacement list is per-domain and belongs to whoever sweeps the docs.
- **Three services declare `mdns/1.4.3` and never use it**: `services/{camera,notification,
  productivity}/conanfile.txt` carry the dependency while the only code that names `MdnsService`
  is the gateway's `main.cc:441,489`. A conanfile is its own project in this build, so removing
  the line is a per-project change with its own gate; disclosed, not done.
- **`MdnsService` ships with the coverage it had in the gateway, which is none**: `health()` and
  `isAdvertising()` have **no caller in the tree** (the health route's extras come from
  service-local providers), `initialize()` returns `true` on every path — including "no interface
  and no socket", which is deliberate (a home appliance must boot without a LAN) and makes the
  gateway's `if (!mdnsService->initialize())` branch dead — and the class has no move
  constructor. The move is honest about it: the package's `AGENTS.md` states the best-effort
  contract, and no test was invented for code that has none.
- **`SyncLimits::kMaxRows` is a C string, not a `std::string_view`**, because
  `operator+(std::string, std::string_view)` does not exist in C++20 and all 11 call sites append
  it to a `std::string`. The header says so; a future reader who "fixes" the type breaks
  eleven repositories.
- **The remaining `SYNC_LIMIT` spellings in live docs are the frozen wire name, not the C++
  symbol** (`docs/README.md:34`, `packages/contracts/CONTEXT.md:26`, the three service
  `AGENTS.md` files): the protobuf message and the wire invariant are named `SYNC_LIMIT`, and
  those documents point at `contracts.proto` and `wire-sync-tables.md`, which now name the C++
  symbol. No change is wanted there.
- **Four service-local error headers were created rather than contract entries**:
  `services/{guard,llm,stt,vlm}/src/…/{guard,llm,stt,vlm}-errors.hxx` declare those services'
  refusals beside their controllers, because their codes (`LLM_NOT_LOADED`, `STT_NOT_LOADED`,
  `VLM_NOT_LOADED`, guard's) have no contract home yet — the vocabulary that *is* frozen lives in
  `packages/contracts/*/[a-z]*-errors.hxx` (`auth`, `camera`, `identity`, `productivity`, `sync`,
  `tts`, `gateway`). Where the four belong is the contracts' decision, not this step's.

