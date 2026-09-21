# Phase 2 step 2d — section 2.3 applied to the eleven services

Base: `41d410a` (2c, the clients). Unit of work: the eleven `services/`.
Gate: `./scripts/build-all.sh dev`.

## What the sub-step turned out to be

The row's fourth sub-step is the eleven services, and the first thing measuring them settles is that
the plan has already assigned almost all of their layout work elsewhere. §9.2's own "Layout work"
column, read against Phase 4, is a complete map: `tts` is flattened by Phase 4 step 1, `notification`'s
`feature/api/<r>/` becomes `feature/<r>/` and its `feature/rpc/` becomes `app/rpc/` in step 2,
`camera` is step 3 ("the largest"), `productivity` and `guard` are step 4, and `llm`, `stt` and `vlm`
are step 5. `voice` is the one cell no Phase 4 step claims — §9.2 gives it `app/` and the gRPC move
and the phase never names it, which Phase 1 step 12's row already recorded as a gap rather than
something this step silently absorbs. Two services are not in the work at all, by decision: `tunnel`'s
own shape is accepted (**D19**) and `gateway` dies (**D5**). Doing 2d as "apply §2.3 to the eleven"
would therefore mean doing Phase 4's rows first, which is why this step separates the three classes
instead — already true, owned by a later step, and unowned — and changes only the third.

## The eleven measured against §2.3

`src/` file counts and test files are measured on the tree this report ships with; the owner column
is §9.2 and Phase 4, not a judgement made here.

| Service | `src/` shape today | src files | test `.cc` | `tests/` | Remaining §2.3 layout work, and its owner |
|---|---|---|---|---|---|
| `tts` | `app/{main.cc,rpc/}` + `feature/synthesis/{api/http/{controller,dto},domain,infra/supertonic}` — and no `shared/` at all | 19 | 2 | `unit/` | Flatten the feature (Phase 4 step 1) — the reference for the service-level split |
| `guard` | `feature/api/guard/{controllers,dtos,services}` + `feature/guard/vocabulary`, no `app/` | 49 | 21 | `support/` + `unit/` | Add `app/`, move the gRPC (step 4) |
| `voice` | `feature/{health,voice}` + `shared/{services/{noise,reaction,vad},wrapper/audio}` + `test-support/` (one header), no `app/` | 17 | 4 | `unit/` | Add `app/`, move the gRPC — §9.2's cell, and **no Phase 4 step names `voice`** (Phase 1 step 12's recorded gap) |
| `notification` | `feature/api/notification/*` + `feature/rpc/` + `src/notification/` + `shared/` | 32 | 9 | `unit/` | `feature/api/<r>/` → `feature/<r>/`, `feature/rpc/` → `app/rpc/` (step 2) |
| `productivity` | `feature/api/<five resources>/*` + `feature/sync` + `src/productivity/` | 47 | 4 | `unit/` | Same, plus `app/` (step 4) |
| `camera` | `feature/api/<three>/*` + `feature/{actions,health,sync}` + `src/{camera,controllers,monitor,objects,operator}` + `shared/` | 123 | 14 | `unit/` | Domain dirs → features, residual `src/controllers/` → features, gRPC → `app/rpc/` (step 3) |
| `llm` | `src/controllers/` + `src/llm/` + `shared/services/{encounter-closed,llm,tools}` | 19 | 6 | `bench/` + `fixtures/` + `unit/` | Legacy horizontal → features (step 5) |
| `stt` | `src/controllers/` + `shared/services/stt/` | 6 | 1 | `unit/` | Legacy horizontal (step 5) |
| `vlm` | `src/controllers/` + `src/vlm/` + `shared/services/vision/{,remote}` | 13 | 2 | `support/` + `unit/` | Legacy horizontal (step 5) |
| `tunnel` | `client/` + `core/` + `net/` + `protocol/` + `relay/` + `server/` + two mains at the root (`main-client.cc`, `main-relay.cc`) | 22 | 7 | suites at the `tests/` root | None — **D19** accepts this shape, no phase touches it |
| `gateway` | `identity/` + `proxy/` + `server/` + `sync/{,fallback}` + `shared/` | 49 | 5 | suites at the `tests/` root | None — the service **dies** (D5) |

## What was already true, measured rather than assumed

- **The file set §2.3 names.** All eleven carry `AGENTS.md`, `CONTEXT.md`, `CMakeLists.txt`,
  `config.toml.example` (the local `config.toml` is the ignored one every service's `.gitignore`
  lists) and `Dockerfile`. `scripts/` is present in six (`camera`, `llm`, `stt`, `tts`, `vlm`,
  `voice`) and `tools/` in five (`camera`, `gateway`, `notification`, `productivity`, `tts`); the two
  services with neither are `guard` and `tunnel`, which is rule 24 doing its job rather than a gap.
- **Rule 26's single schema.** Measured per service: exactly one `database/schema.sql` in each of the
  five services that own a database — `camera`, `gateway`, `guard`, `notification`, `productivity` —
  and none in the six that own no data (`llm`, `stt`, `tts`, `tunnel`, `vlm`, `voice`). The two
  `database/schema.sql` files that live in `packages/` (`packages/identity`,
  `packages/memory`) are Phase 3c's and Phase 4 step 7's to move, since §2.4 keeps data access out of
  packages.
- **Ports are config, not build.** §2.3's `CMakeLists.txt` line writes `PORT 7026`, and the ports in
  the tree are `config.toml` values the service reads (`voice` 7034/7035,
  `services/voice/config.toml.example:6-7`; `tts` 7029, `services/tts/config.toml.example:5`).
- **Drogon is the HTTP substrate of all eleven** (`grep -r drogon` per service, no exceptions), which
  is what makes the upload path below every service's business rather than five services'.
- **`tests/` exists in all eleven**, with `tests/unit/` in nine of them.
- **§2.3's "`src/server/` disappears" already holds wherever the service survives.** Only two
  services carry `src/server/` — `gateway`, which **D5** deletes, and `tunnel`, whose shape **D19**
  accepts — so no surviving service has one to remove. `src/app/` exists in exactly one service
  (`tts`), which is the shape §2.3 draws and Phase 4 step 1 flattens from, and `src/config/` exists
  in none, which is Phase 4 step 9's work (D20).

## What this step changed: the upload trees, and the ignore that keeps them out

Five services carried drogon's default upload path inside the repository — `gateway`, `guard`, `llm`,
`notification` and `tts`, each with **258 directories and 0 files**, dated 2026-09-12 to 2026-09-17,
created when those services were run by hand from their own directory. Nothing ignored them:
`git check-ignore` returns nothing for them, and `git status` was silent only because git tracks
files and these directories hold none — 1,290 empty directories that no reader of the tree would
guess were runtime debris.

- The five trees are **removed**. They are drogon's `uploads/{tmp,00..FF}` scaffolding, recreated by
  any run of the service from its own directory, and nothing in the tree reads them.
- `uploads/` is **ignored by all eleven services** (`services/<name>/.gitignore`, one line each, after
  the existing `gen/`), so a manual run cannot quietly re-litter the tree. Probe-verified: a created
  `services/tts/uploads/tmp/00` and `services/voice/uploads` are both reported by
  `git check-ignore -v` as `services/<name>/.gitignore:12:uploads/`, `git status` stays silent, and the
  probes were removed.
- The mechanism is the one 2c's report already recorded for the `vlm` suite: drogon's upload path
  defaults to the process's working directory, so a test or service started from a tree root creates
  the 256 subdirectories there. 2c fixed that suite by pointing `setUploadPath` at a temporary
  directory; the service trees are what this step fixes, at the ignore level rather than by
  redirecting a path nothing in the tree configures.

## Recorded, not fixed

Four items came out of the measurement and none is changed here, because each belongs to a step that
has not run or to no step at all:

- **`argus_service` is called by two of the eleven.** `services/tts/CMakeLists.txt:72` and
  `services/voice/CMakeLists.txt:131` use the helper
  (`cmake/argus-module.cmake:513-529`); the other nine hand-roll `project()` + `add_executable` +
  their own link and rpath lines. §2.6 says the helper needs no work ("unchanged except the alias it
  links") and no phase converts the rest, so the divergence is recorded rather than swept.
- **§2.3 writes the helper's call wrong.** Its line reads `argus_service(NAME camera PORT 7026 ...)`,
  and the helper's contract is `NAME;MAIN` plus `MODULES;DEPENDS;PORTS` — there is no `PORT`, and
  `ARGUS_PORTS` is consumed by nothing: the only occurrence in the tree is the line that sets it
  (`cmake/argus-module.cmake:527`). Feeding ports from the build into the services is config
  resolution, which is Phase 4 step 9's ground (D20).
- **`tests/e2e/` exists nowhere**, and two services (`gateway`, `tunnel`) keep their suites at the
  `tests/` root rather than under `unit/`. The plan's Phase 4 steps name the `src/` shape and not the
  test shape, so this is recorded here with the measurement above and left where Phase 4's own rows
  will find it.
- **Every service still carries its own `conanfile.txt` and its two preset files.** Measured: all
  eleven hold `conanfile.txt`, `CMakePresets.json` and `CMakeUserPresets.json` (18 of each repo-wide,
  the other seven being one per package gate project). Deleting them is the next row's stated work —
  row 3 is "one root `conanfile.txt`; delete per-package `conanfile.txt` and presets" — so nothing
  here touches them, and the count is recorded so that row starts from a measurement.

One stale record, measured while building the table: **§9.2's `Files` column does not reproduce
against the tree.** Today, `src/` file counts run 6 (`stt`) to 123 (`camera`), and whole-service
counts excluding `build/` run 19 (`stt`) to 262 (`gateway` — 195 of that is
`tools/probe-captures/`, the proxy probes' tracked `.txt` captures, which `gateway`'s death by
**D5** takes with it). Against the column's numbers for the eleven — `stt` 632, `tts` 39, `guard`
68, `voice` 51, `notification` 58, `productivity` 64, `camera` 158, `llm` 21, `vlm` 15, `tunnel`
30, `gateway` 62 — four land within a few of the whole-service count (`camera` 158 vs 157,
`notification` 58 vs 57, `tts` 39 vs 36, `productivity` 64 vs 67), which says the column meant
"files in the service", and the rest have drifted as the tree changed. `stt`'s 632 matches no
reading at all — its `src/` holds 6 files and its whole tree 19, so the vendored model code the
column's own row names is no longer in the service. A docs item, not a defect: the numbers are used
for ordering work, not for verification.

## Verified

`./scripts/build-all.sh dev` on the tree this report ships with: exit 0, closing line
`[setup] All selected projects built and tested (profile: dev).`, 18 of 18 projects at
`100% tests passed, 0 tests failed`, no `Not Run`, no `***Failed` and no `warning:` line, and the
ledger unchanged at **428** — this step changes no compiled file, so the count moving would have been
the finding. The five removed trees were inspected before removal (0 files each) and the ignore was
probe-verified in a service that had the tree (`tts`) and one that never did (`voice`).

Report: `docs/history/reports/f2-2-layout-services.md`.
