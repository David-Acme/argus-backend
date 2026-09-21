# Phase 1 step 11 — the docs that still describe the old tree

Scope: the row is "Refresh the stale docs that describe the old tree: `packages/common/AGENTS.md`,
the root key-files table (`SampleRing` is in `voice`, not in `audio`; the TTS service is not in
`tts/src/shared/services`), the `packages/client` (singular) references in `README.md` and
`AGENTS.md`. The `docs/architecture/*` layout and subscriber columns are Phase 3d step 4, because
they change meaning only when the gateway actually goes". Base `31fd41c`.

Measured against the tree, **two of the row's three items need nothing at all** — recorded as
measured rather than skipped — and the row's real subject, the docs that describe a tree that no
longer exists, is four classes of defect plus two handoffs earlier steps of this phase filed here
by name, plus twelve findings an adversarial review turned back. 42 tracked files change, 183
insertions and 155 deletions; no source, CMake, Dockerfile, compose, or script *logic* is touched — the only
non-`.md` edits are one comment in a Python tool and one in a TOML example, both named below.

The four classes, and where each is proved: the **command** class (22 un-runnable
`--only argus-<name>` lines), the **path** class (64 retired path spellings in 23 files — the sweep
now returns **zero**), the **count** class (`Twenty`/`20 projects` in four docs, where the tree
has eighteen) and the **claim** class (four sentences whose subject does not exist or does not
behave as described: `argus::tts-grpc-client`, `AppConfig`, `ToolValidator`, `<enum>ToString`).

## What the row asked for, measured

| the row says | the tree says |
|---|---|
| `packages/common/AGENTS.md` | **the file does not exist**: `packages/common` was deleted in step 2 (`7cc5564`, "delete the packages/common transition shim"), and `git grep packages/common` over every tracked file outside `docs/history/` is empty — there is no live reference left to refresh either |
| the key-files table puts `SampleRing` in `audio` | stale, and both halves were wrong: `SampleRing` is `services/voice/src/shared/wrapper/audio/sample-ring.hxx`, while `packages/audio/src/shared/wrapper/audio/` holds `AudioResampler` and `EndpointDetector`. The table now has one row per home, and the `audio` row names what it actually holds |
| the table puts the TTS service in `tts/src/shared/services` | stale: `TtsService` is `services/tts/src/feature/synthesis/domain/` and the engine set (`TtsEngine`, `Style`, `UnicodeProcessor`, onnx loading) is `.../infra/supertonic/`. The row is now the feature root, in the shape row 5 gave the other services |
| `packages/client` (singular) in `README.md` and `AGENTS.md` | **no occurrence**: `git grep 'packages/client\b'` answers nothing in either file. Nothing to fix |

Two more items arrived here by name from step 10's report and are discharged: the root `AGENTS.md`
named `packages/access` in rule 1 and in the file table (the package was deleted in step 4), and
`docs/architecture/camera-guardian-deep-analysis.md:942` named `AppConfig` as the thing controllers
answer with. Rule 1 now names the four contract headers that own the CHECK-constrained enums and
their `<enum>ToString`/`<enum>FromString` pairs, the table row names
`packages/contracts/{auth,camera,productivity,sync}-contract/`, and the analysis says
`ResponseException` (`packages/errors/src/errors/response-exception.hxx:33`) — which is what
`packages/http/src/http/error-handler.hxx:10-14` describes as "the single place a refusal becomes
an envelope". Every path introduced there was checked to exist, and the check corrected this line:
the three camera enums and the productivity one sit at their contract's root, not under a
`src/shared/contracts/` leaf — `packages/contracts/auth-contract/user-role.hxx`,
`packages/contracts/camera-contract/{event-severity,camera-record-mode,zone-type}.hxx`,
`packages/contracts/productivity-contract/reminder-detail-status.hxx`,
`packages/contracts/sync-contract/src/shared/contracts/user-action.hxx` and
`packages/auth/src/shared/access/role-access.hxx`.

## The class nobody had swept: `--only argus-<name>`

22 live occurrences of a command that cannot run. `scripts/build-all.sh:69` matches
`basename "$dir"` against `--only`'s value, so the prefixed form is refused before any build work:

```
$ ./scripts/build-all.sh dev --no-tests --only argus-camera
[error] unknown project for --only: argus-camera   (exit 1)
$ ./scripts/build-all.sh dev --no-tests --only camera
…configure, build, ctest: 35/35 pass               (exit 0)
```

The correct form was already in the same repository — `README.md:34` and `AGENTS.md:667` both say
`--only camera`, and every Dockerfile passes a bare basename — so the 22 sites contradicted files
sitting beside them. They are the copy-paste blocks each service's own `AGENTS.md` and `CONTEXT.md`
carry, which is exactly why the class is 22 sites wide:

| where | count |
|---|---|
| the eleven services' `AGENTS.md` (`camera`, `gateway`, `guard`, `llm`, `notification`, `productivity`, `stt`, `tts`, `tunnel`, `vlm`, `voice`) | 11 |
| the six services' `CONTEXT.md` (`camera`, `gateway`, `guard`, `notification`, `productivity`, `voice`) | 6 |
| the two packages' `AGENTS.md` (`identity`, `memory`) | 2 |
| `docs/architecture/system-overview.md:57`, `docs/operations/build-and-test.md:9` | 2 |
| `docs/operations/deployment-docker.md:34` (the placeholder form, `--only argus-<name>`) | 1 |

All 22 now carry the basename. The check is one `git grep` and it is in Verification.

## The retired path spellings

The class is bigger than the command class and was swept whole: every `argus-`-prefixed package or
service *path* in the live tree. Counted at base with

```
git grep -nE "(packages|services|lib|src)/argus-[a-z]+|argus-[a-z]+/(database|src|tests|scripts|tools)" \
    -- . ':!docs/history' ':!*/build/*'
```

**64 occurrences in 23 files**; after this step the same grep answers **nothing at all** — the
first sweep of this step to close a class completely rather than to a deferred line. What it
corrected, by kind:

- **the per-unit docs that name their own siblings** — `services/gateway/CONTEXT.md` (five: the
  fan-out and rate-limiter sources, `identity-rpc.cc`, the gateway schema, the identity schema),
  `services/camera/CONTEXT.md` (three, including the `argus-camera/database/schema.sql` /
  `argus-identity/database/schema.sql` pair in the ownership paragraph that now reads
  `services/camera/…` and `packages/identity/…`), `packages/identity/CONTEXT.md` (four),
  `packages/auth/{AGENTS,CONTEXT}.md` (three), `packages/{identity,memory}/AGENTS.md` (two: the
  `# From packages/…` comment above each direct-build block), `services/{notification,productivity}`
  (three), and the root `AGENTS.md`.
- **the docs that name where a thing lives** — `docs/architecture/{events-and-contracts,
  services-and-packages,system-overview,wire-camera-media,wire-nats-subjects,
  wire-sync-golden-frames,build-model}.md` and all of `docs/operations/` (the eight
  `scripts/provision.sh` rows, the fixtures path, the `cd services/…` blocks, the two runnable
  binary paths in `system-overview.md:104-106`).
- **two references outside the docs**: `models/intent/MODEL-CARD.md:5` ("`services/llm` as the fast
  tier of the intent router") and one comment in `scripts/export-yolo26-ncnn-e2e.py:7` pointing at
  `services/camera/src/objects/ncnn-object-detector.cc`. Comment-only; no behaviour.
- **the two blocks the first pass had written off as deferred**, and did not survive the review's
  evidence: `build-model.md`'s project tree named `packages/argus-common` (deleted) and
  `packages/argus-contracts` (never a project), and `data-storage.md`'s table gave an operator six
  schema paths that resolve to nothing. Both are today's facts, not post-gateway architecture, and
  both are fixed; the count class lives in the same paragraph as the tree and is fixed with it.

**The audit tool matters, and the first one was wrong.** The inventories were first taken with
`grep -rn … | grep -v '^./docs/history/'`, whose exclusion silently did nothing — recursive `grep`
through this shell does not prefix `.` — so the "live" set was never actually filtered, and one
later pass missed `docs/architecture/system-overview.md:104-106` entirely until `git grep` over
tracked files with a `':!docs/history'` pathspec exclusion found it. Every count in this report is
from `git grep` for that reason: the pathspec filter is exact and cannot be defeated by an output
prefix. The same sweep produced two false positives that were measured and discarded rather than
"fixed": `scripts/setup.sh camera` is a valid argument (`scripts/setup.sh:42`,
`camera) CAMERA_ONLY=1`), and the `packages/contracts/AGENTS.md` proto references are
folder-relative by design.

## The counts, and the project set they describe

`build-all.sh` declares **18** projects (`scripts/build-all.sh:9-27`), and the same 18 is what the
docs' own definition yields — every directory carrying `CMakeLists.txt` + `conanfile.txt` +
`CMakePresets.json`, which is the sentence sitting directly under the tree. Four live docs said
`Twenty`: `build-model.md:5`, `system-overview.md:44`, `build-and-test.md:6` and `:53`. All four
say eighteen now, and `build-model.md`'s tree lists the eighteen by their real paths — its two
extra entries were exactly the padding that made twenty: `packages/argus-common` (deleted in step
2) and `packages/argus-contracts` (a buf container with no `CMakeLists.txt`, no `conanfile.txt` and
no `CMakePresets.json` of its own, so it builds no project).

## The claims whose subject does not exist

Four sentences asserted behaviour that is not in the tree. Each was measured before it was
rewritten, and the first two were reported to this step by name (step 10's report, the review):

- **`services/tts/AGENTS.md`** said the RPC target "links the shared `argus::tts-grpc-client`
  module (`packages/clients/tts-grpc-client`)" — there is no such directory and no such target
  anywhere in the tree (the string `tts-grpc-client` occurs in exactly that one line).
  `services/tts/src/app/rpc/CMakeLists.txt:2` links `argus::tts-client`, so the sentence names
  `packages/clients/tts-client` now, and the second stale spelling in the same paragraph
  (`packages/tts-client`, missing the `clients/` level) is corrected with it.
- **`camera-guardian-deep-analysis.md:942`** named `AppConfig` as the thing controllers answer
  with; the boundary answers with `ResponseException` (above).
- **`services/llm/CONTEXT.md`** named `ToolValidator` beside `ToolRegistry` and `ToolExecutor`.
  Two of those are classes; the third is not — `tool-validator.hxx:12` declares
  `namespace tools { std::optional<std::string> validateArguments(...) }` and no class at all. The
  bullet names the directory and the three real symbols now.
- **the root `AGENTS.md`, in two places**, told the reader each contract header carries
  "`<enum>ToString`/`<enum>FromString`" — with the enums spelled PascalCase in the sentence above,
  the substitution a reader makes (`UserRoleToString`) does not compile and appears nowhere in the
  tree. The helpers are lowerCamelCase derived from the enum's own name: `userRoleToString` /
  `userRoleFromString` (`user-role.hxx:14,29`), `eventSeverityToString`,
  `cameraRecordModeToString`, `zoneTypeToString`, `reminderDetailStatusToString`,
  `userActionToString` and their `FromString` twins. This is the worst of the four — it is a rule
  an agent follows literally — and the adversarial review found it.

## The two handoffs rows 5 and 9 left for this step

**Row 5 filed `argus-deploy/CONTEXT.md:180-186,408-409`** as describing "per-database named volumes
and gateway `[productivity] db`/`[notifications] db` keys that no longer exist". Measured:

- the compose declares exactly two named volumes, `argus-cutover-camera-stream` and
  `argus-cutover-nats-data` (`argus-deploy/docker-compose.yml:894-898`); no `argus-cutover-*-db`
  is declared anywhere, and `productivity.db`, `notification.db` and `memory.db` live in
  bind-mounted subdirectories of `${ARGUS_DATA_DIR:-./data}` (`:199`, `:252`, `:484`). The
  `argus-cutover-*-db` names survive in `scripts/provision-host.sh` alone: as the *source*
  list of the one-time `--migrate-volumes` copy (`:231-235`), with the leftover-legacy-volume
  detector that exists to find them on an old installation beside it (`:82-88`,
  `grep -q '^argus-cutover-.*-db$'`). That is what the doc now says.
- the gateway's `[productivity]` and `[notifications]` blocks carry `proxy_url` and `grpc_target`
  and no `db` key (`argus-deploy/config.gateway.toml.example:74-77,102-109`); the `db` keys belong
  to the owning services (`config.productivity.toml.example:35`,
  `config.notification.toml.example:35`). The doc's sentence is now about the owners.
- the same section's init-profile table still called those targets "the camera.db volume", "the
  productivity.db volume" and "the notification.db volume" — the one place the file contradicted
  the paragraph this step rewrote. They are data directories now, and the same word is fixed in
  `services-and-packages.md:30` ("each database directory is mounted by its owner only").

**The same bullet's f8-b4 tense was stale in the same way**, so it is corrected with them rather
than left contradicting its neighbours: the memory bullet said the `argus-cutover-memory-db` volume
"stays DECLARED" and that argus-llm "takes the rw mount over at f8-b4", and the Fase 4 paragraph
said argus-llm had "no bus consumer" and "no database". f8-b4 has landed: the compose's llm block
mounts `${ARGUS_DATA_DIR:-./data}/memory`, `packages/memory/database/schema.sql`, `models/memory`
and `models/extract` (`docker-compose.yml:482-499`), the config example's own header says "Since
f8-b4 this is the brain" and carries the `[memory]` block, and `services/llm/src/main.cc:175-243`
builds a `NatsBus` and starts a durable consumer on the guard encounter-closed stream
(`argus-llm-encounters`). The doc now states exactly that. One comment in
`argus-deploy/config.llm.toml.example:34` ("the memory-db volume mounts at …") was corrected for
the same reason, comment-only.

**Row 9 filed `services/llm/CONTEXT.md:91-92`** ("No tool loop: `LfmAdapter`/`ToolRegistry`/
`chatWithTools` stay legacy-side (Ruling BV) — this service exposes chat/chat-stream only"), noting
it was already stale when f8-b4 landed the loop. It now carries a supersession marker in the house
style (`services/camera/CONTEXT.md:34`, `services/gateway/CONTEXT.md:230` are the precedents) and
names where the runtime is: `src/shared/services/tools/`, driven by the controller through
`chatWithTools`/`chatWithToolsStream` — which is step 9's own move, so the bullet was stale in two
directions at once.

## The review

An adversarial read-only agent was given the changed lines, the tree and no summary of intent, and
told to check every claim against the code. It returned twelve findings; each was verified here
before anything was believed, and **six were fixed, four discarded with evidence, one deferred by
plan and one recorded for its owner**. Its audit ran against a tree this session kept editing: it
recorded the snapshot it finally judged (`git diff | sha256sum` =
`86d309a066013a75214588259d4a84ec2d24f7a35181e09d231387bc955c9d50`) and dropped one finding of its
own because a concurrent edit had already fixed it.

| # | finding | measured here | disposition |
|---|---|---|---|
| 1 | `services-and-packages.md:37` heads its "Standalone packages" table with `argus-common`, a deleted package | `git ls-files packages/common` is empty | **fixed**: the row is gone |
| 2 | `Twenty`/`20 projects` in four docs where the build has 18 | `build-all.sh:9-27` = 18; the docs' own presets+conanfile definition also yields 18 | **fixed** in all four, and the tree lists the real 18 |
| 3 | `<enum>ToString`/`<enum>FromString` is a pattern that appears nowhere | zero `UserRoleToString`-style hits; helpers are lowerCamelCase | **fixed** (the severest: a rule an agent follows literally) |
| 4 | `ToolValidator` is not a symbol | `tool-validator.hxx` declares `validateArguments` only | **fixed** — found here independently, the review confirms it |
| 5 | twelve `## Layout` trees still root at `argus-<name>/` | `git grep -nE "^argus-[a-z-]+/$"` → exactly the twelve AGENTS.md sites the review names | **deferred**: Phase 4 step 8 owns layout blocks (plan line 872); now enumerated in Findings below, which the previous draft of this report did not do |
| 6 | `data-storage.md`'s schema paths and the memory row's package are retired spellings | all six paths resolve under `packages/<name>`/`services/<name>`; the compose binds `packages/memory/database/schema.sql` | **fixed** (paths and package names); the *ownership* column stays step 4's |
| 7 | `tool-calling-eval-set.md:4` says "with the classifier retired" while `MODEL-CARD.md:5` says the classifier is loaded in-process | both halves are load-bearing: `IntentGate` is constructed in `LlmController` (`llm-controller.hxx:21,40`) and loads `models/intent/intent.bin` (`intent-gate.cc:33-44`), but the compose mounts no `models/intent` and `services/llm/scripts/provision.sh` fetches none, so a deployed gate falls through | **recorded, not fixed**: the fix needs a decision about what "retired" means (code or deployment) that belongs to the intent feature's owner, and the sentence's own history is the docs consolidation `c416c65`, before `IntentGate` moved here |
| 8 | `AGENTS.md:710` puts `supertonic/` beside `domain/` | `synthesis/` holds `api/`, `domain/`, `infra/supertonic/` | **fixed**: the row names `domain/` and `infra/supertonic/` |
| 9 | `system-overview.md:24` lists `argus-contracts` as a service owner | the name is the house convention, not a path | **discarded** — see the two below |
| 10 | `src/config/application.cc` does not exist | true of *this* tree; both sentences say "the legacy publishes"/"unlike the legacy" and the path is the legacy monolith's | **discarded** |
| 11 | twelve bare `argus-<name>` spellings in prose and titles | 18 CMake projects are literally `project(argus-<name>)` (`packages/identity/CMakeLists.txt:2`, all eleven services); the units without one title their own docs `# argus-<name>` (`packages/auth/AGENTS.md:1`, `# argus-http`, `# argus-errors`, `# argus-audio`, `# argus-phrase`, `# argus-mdns`, `# AGENTS.md — argus-contracts …`) | **discarded**: a deliberate naming convention (`argus-<name>` is the deploy's container name too), not a broken reference — unlike a *path*, a *command* or a *file*, which this step does fix |
| 12 | `argus-deploy/CONTEXT.md:143-145` still says "volume" | the init rows contradicted the paragraph rewritten at `:180` | **fixed** (above) |

Two of the review's discarded findings are worth the sentence: findings 9 and 11 are the same
question — whether a bare `argus-<name>` is a stale spelling — and the tree answers it twice, with
eighteen CMake projects named exactly that and with the units' own document titles. The report
would be wrong to "fix" them; the classes this step does fix are the ones a reader *acts* on.

The review's clean list is not repeated here, but two of its verifications are load-bearing for
this report's own claims: the `--only` values the sweep wrote are all accepted by
`basename "$dir"` matching, and the deploy claims (two named volumes, three data directories, the
gateway's absent `db` keys, the llm block's five memory mounts) match the compose and the config
templates rather than the prose they replace.

## Verification

- **The phase gate: `./scripts/build-all.sh dev` exits 0** — 18/18 projects, every suite
  `100% tests passed, 0 tests failed`, the reached-test ledger **302 — unchanged**, which is what a
  step that touches no source must look like. Measured, not forecast: the run's own log line is
  `GATE_EXIT=0` and the per-project sums are 16+8+1+16+20+16+4+26+35+24+29+37+10+4+6+25+14+11. No
  first-party warning: every `Warning:` line is the third-party set the phase has recorded since
  step 4 (llama.cpp's `CMAKE_CXX_STANDARD` notice, conan's absent-ccache note,
  ncnn/glslang/openfst's own).
- **The command class is closed**: `git grep -- "--only argus-" -- . ':!docs/history' ':!*/build/*'`
  answers nothing, at base it answered 22 lines in 22 files. Every replacement value is a project
  basename that `build-all.sh` accepts, and two of them were exercised directly before the sweep
  (`--only camera` 35/35, `--only gateway` 26/26 at step 10's gate).
- **The path class is closed outright**: the sweep grep above returns **zero lines** over the
  live tree, from 64 occurrences in 23 files at base. The invariant is checkable rather than
  asserted — extract every `packages/<name>`/`services/<name>` token from every tracked file
  outside `docs/history/` and test the directory: nothing is left unresolved. Applied to the 80
  distinct path tokens this step *introduced*, the same check resolves all 80 — 73 at the repo
  root and 7 against the unit that owns them (`src/shared/services/tools/` under `services/llm`,
  the identity `src/shared/services/…` inventory under `packages/identity`).
- **The counts are checked against the build, not against each other**: `build-all.sh`'s PROJECTS
  has 18 entries, and the 18 directories carrying `CMakeLists.txt` + `conanfile.txt` +
  `CMakePresets.json` are the same 18. The stale form is gone too:
  `git grep -nE "Twenty|all 20 projects|\b20 projects\b" -- . ':!docs/history' ':!*/build/*'`
  answers nothing.
- **The deploy claims are checked against the compose and the config templates**, not against the
  prose they replace: the two named volumes, the three bind-mounted data dirs, the gateway's absent
  `db` keys and the owning services' present ones, and the llm block's five memory-related mounts.
- **Line hygiene**: `git diff -U0 -- . ':!docs/history' | grep '^+' | awk 'length>78'` returns 15,
  and the split is the point: 14 are Markdown table rows (every one begins with `|`; a table row
  cannot be wrapped, and the root `AGENTS.md` carries 99 such lines at base, 98 now), and one is
  prose — 79 columns, the unbreakable path in
  `` `<shared/services/tts/tts-wire.hxx>` from `packages/clients/tts-client` ``. Every other
  rewrapped line is back inside the width the surrounding files use.
- **No unintended change**: `git diff --stat` is 42 files (the 41 content files plus this step's
  plan row), and the only non-`.md` edits are
  `scripts/export-yolo26-ncnn-e2e.py` (one comment) and `argus-deploy/config.llm.toml.example` (one
  comment). `git grep` for `AppConfig`, `get400Response`, `pkg::response`, `packages/common`,
  `packages/access`, `packages/response`, `packages/client`, `tts/src/shared/services`,
  `ToolValidator` and `tts-grpc-client` over the live tree now returns **nothing**, and `SampleRing`
  returns only the code plus the intended new table row.

## Findings outside this unit

- **The per-unit layout blocks are Phase 4 step 8's** (plan line 872, "Update every
  `AGENTS.md`/`CONTEXT.md` layout block (several are stale today)"). This step corrected rules,
  commands, paths and claims inside those files; it did not redraw their trees, and the review's
  finding 5 is exactly that deferral — enumerated here so step 8 starts with the list:
  `packages/{identity,memory}/AGENTS.md`, `services/{camera,gateway,llm,notification,productivity,
  stt,tts,tunnel,vlm,voice}/AGENTS.md`, twelve in all, each rooting its tree at `argus-<name>/`
  while the build block a dozen lines below says `--only <name>`. The two classes are
  distinguishable in this diff: every edit here is a rule, a command, a path or a claim, never a
  diagram.
- **The package taxonomy is Phase 1 step 12's, and it inherits one row this step could not
  classify**: `services-and-packages.md`'s "Standalone packages" table says each entry "owns a
  Conan/CMake graph and builds on its own", and `packages/contracts` owns none: the container
  directory has no `CMakeLists.txt`, and each of its eleven contract folders has one but no
  `conanfile.txt` and no `CMakePresets.json` — a fragment with `cmake_minimum_required` and no
  `project()`, pulled in by consumers (`packages/auth/CMakeLists.txt:17,22`) in the
  empty-when-standalone pattern `packages/identity/CONTEXT.md:28-33` records. Deleting the
  deleted package's row was unambiguous; re-classifying a live one is step 12's, which owns "the
  package taxonomy and the tier table of §2.4".
- **`docs/operations/tool-calling-eval-set.md:4` and `models/intent/MODEL-CARD.md:5` disagree about
  whether the fastText classifier is live** (review finding 7). Measured: `IntentGate` is
  constructed in `LlmController`'s initialiser and its router is handed to `LfmAdapter`
  (`llm-controller.hxx:21,40`, `lfm-adapter.hxx:67`), `intent-gate.cc:33-44` loads
  `models/intent/intent.bin`, and `services/llm/CMakeLists.txt` links `argus::intent` — but
  `argus-deploy/docker-compose.yml` mounts no `models/intent` and
  `services/llm/scripts/provision.sh` fetches none, so on a deployed stack the gate logs
  "no intent model" and falls through. Which sentence is wrong depends on whether "retired" means
  code or deployment, so this step changes neither, and the intent feature's owner should.
- **`docs/architecture/*`'s gateway-as-subscriber prose is Phase 3d step 4's**, per the plan's own
  words (line 1061: "Payloads and subjects stay frozen; the subscriber columns and the layout prose
  are rewritten in Phase 3d step 4"), which names the 26 `Consumer` cells in
  `wire-nats-subjects.md`'s subject table and the layout mentions in nine further files. This step
  deliberately did *not* extend its deferral to the retired spellings in those same files: a
  deleted package, a wrong count and a dead schema path are today's facts, and the gate's
  departure changes none of them into truths.
- **The three `mdns/1.4.3` conanfile declarations, `services/gateway/tools/probe-captures/`,
  `argus-deploy/data/`'s residue, the nine Dockerfiles' `mkdir` lists and the four service-local
  `*-errors.hxx`** are the deferrals earlier reports recorded; none of them is a doc, so this step
  neither touches nor re-flags them.
- **`docs/README.md:34`, `packages/contracts/CONTEXT.md:26` and the three service `AGENTS.md`
  files still spell `SYNC_LIMIT`** — deliberately, per step 10's finding: that is the frozen wire
  name (the protobuf message and the wire invariant), not the C++ symbol `SyncLimits::kMaxRows`,
  and those documents point at `contracts.proto` and `wire-sync-tables.md`, which name the symbol.
  Verified again this step: no change is wanted there.
