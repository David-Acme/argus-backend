# Phase 4 step 7 — `memory` and `intent` become features of `services/llm`

Step 7 of the architecture plan (row 1007) is the last one that empties
`packages/` of a project of its own: `packages/memory` and `packages/intent`
leave and become features of the service that was already their only host and
their only consumer. It also discharges the deviation Phase 1 step 9 recorded
(§9.3): `tool-contracts.hxx`, which that step deliberately left in the tier-3
client because a package may not include a service's source, follows memory out
of `packages/` now that both of its consumers are features of one service.

## What existed before

Two packages that were services in all but name:

- `packages/memory` — 54 tracked files, its own `CMakeLists.txt` declaring
  `project(argus-memory)`, its own `AGENTS.md` and
  `CONTEXT.md`, its own `config.toml.example`, its own
  `scripts/provision.sh`, its own `database/schema.sql`, and five test suites.
  It carried `memory-core` (the graph, recall, formation, extraction and
  embedding sources), `memory-catalog` (a one-source archive over
  `catalog-replica.cc`, linked by the executable and by the replica test) and
  `src/memory/memory-dto.{hxx,cc}`.
- `packages/intent` — the fastText classifier and the intent router, its own
  project file, its model pin under `models/` and two test suites.

Neither had a second consumer. Both were hosted by `services/llm`: the service
file added them by path inside its `if(PROJECT_IS_TOP_LEVEL)` group, linked
`memory-core`/`argus::intent` from two of its own feature modules, and its
config carried the memory blocks as a copy of the package's example. So every
edge check rule 23 and 25 care about was already inside one service; what the
step removes is the second project file, the second `--only` name and the
second schema owner.

## The move

58 paths moved as git renames (24 of them R100):

| From | To |
|---|---|
| `packages/memory/src/shared/services/{memory,embedding,extract}/` | `services/llm/src/feature/memory/services/{memory,embedding,extract}/` |
| `packages/memory/src/shared/repositories/memory-graph/` | `services/llm/src/feature/memory/repositories/memory-graph/` |
| `packages/memory/src/shared/vocabulary/encounter-closed-receipt.hxx` | `services/llm/src/feature/memory/vocabulary/` |
| `packages/memory/src/memory/catalog-replica.{hxx,cc}` | `services/llm/src/feature/memory/infra/` |
| `packages/memory/database/schema.sql` | `services/llm/database/schema.sql` |
| `packages/memory/tests/unit/*` (5 suites) | `services/llm/tests/unit/` |
| `packages/memory/tests/fixtures/eval-usage.tsv` | `services/llm/tests/fixtures/memory/` |
| `packages/intent/src/shared/services/intent/*` | `services/llm/src/feature/intent/services/*` |
| `packages/intent/{NOTICE,models/intent.bin.sha256}` | `services/llm/src/feature/intent/models/` |
| `packages/intent/tests/unit/*` (2 suites) | `services/llm/tests/unit/` |
| `packages/intent/tests/fixtures/*.tsv` (2) | `services/llm/tests/fixtures/intent/` |
| `packages/clients/llm/src/llm/tool-contracts.hxx` | `services/llm/src/shared/vocabulary/` |

The include sweep is 92 rewritten include lines: 70 inside the 58 renames (each
moved file's own cross-references, as
`<feature/memory/services/memory/memory-service.hxx>` and
`<feature/intent/services/intent-router.hxx>`) and 22 across the 36 further
files whose includes pointed at the old spellings. Nothing outside
`services/llm` included any of them — measured before the move, not assumed —
so no other service's source changed.

Two conventions decide the spellings, both already in the tree:

- Every module declares `INCLUDES ../..`, which is `services/llm/src`, so a
  feature reads its siblings as `<feature/<name>/...>` and the shared
  vocabulary as `<shared/vocabulary/tool-contracts.hxx>`. The paths are the
  same shape `services/llm`'s own modules already used, so the move rewrote
  prefixes rather than introducing a second convention.
- `tool-contracts.hxx` goes to `src/shared/vocabulary/` and not into either
  reader, because two features read it (rule 23's 2+ rule): `feature/llm`'s
  tool runtime executes the descriptors `feature/memory` declares. The folder
  is a module of its own (`argus::llm-shared`, declared `HEADER_ONLY` like
  `packages/lib/validation`), linked by both readers and by the executable, so
  nothing is declared without a consumer and nothing is linked without being
  read.

## The build

`services/llm/CMakeLists.txt` lost the two `add_subdirectory` guards and gained
what the packages used to bring with them:

- `find_package(onnxruntime CONFIG REQUIRED)` and
  `find_package(cnats CONFIG REQUIRED)` join the top-level group, which is where
  every project in this tree resolves a dependency it configures against
  (the tts/stt/identity precedent): the memory stack uses both.
- The fastText static library is bootstrapped in that same group over
  `third_party/fastText`'s 13 sources with `-w` and SYSTEM includes, so only
  the target name travels to the feature that links it.
- Seven test targets replace the ones the packages registered: the five memory
  suites and the two intent suites, each linking `argus::memory` or
  `argus::intent`, with their `ARGUS_TEST_MEMORY_SCHEMA`,
  `ARGUS_TEST_MEMORY_MODELS_DIR`, `ARGUS_TEST_MEMORY_FIXTURES_DIR`,
  `ARGUS_TEST_INTENT_MODELS_DIR` and `ARGUS_TEST_INTENT_FIXTURES_DIR`
  definitions repointed at `services/llm`'s own paths.
- `argus::memory` replaced `memory-core` in the two feature modules that link
  it (`feature/llm` and `feature/encounter-closed`); `argus_service`'s module
  list gained `argus::llm-shared`, `argus::memory` and `argus::lib::sqlite`.

`--only llm` is 44/44 before and after: the seven suites used to arrive through
the two `add_subdirectory` calls, so they were already in the same ctest run —
what changed is which project declares them. The `--only memory` and
`--only intent` names are gone from `scripts/build-all.sh`, whose project list
is 15: `packages/lib/cert`, `packages/lib/sqlite` and the thirteen services.

Two declarations were written where the old package had one:

- `services/llm/src/feature/memory/CMakeLists.txt` — one `argus_module(NAME
  memory)` over the 15 sources that compile, `DEPENDS` listing only what the
  sources use. `argus::lib::validation`, `argus::lib::errors` and
  `cnats::nats_static` were dropped from the package's list after measuring
  zero remaining references: they existed for the deleted
  `memory-dto.{hxx,cc}`. `argus::lib::nats` stays, because
  `catalog-replica.hxx` includes `<nats/nats-bus.hxx>` and its `.cc`
  `<nats/nats-subject.hxx>`, and that package already links cnats PUBLIC.
- `services/llm/src/feature/intent/CMakeLists.txt` — the model SHA-256 pin
  (five directories up to the shared `models/intent/intent.bin`, compared
  against the moved `models/intent.bin.sha256` and a `FATAL_ERROR` naming the
  intent-training project on a mismatch), then `argus_module(NAME intent)`.

Both keep the `-Wall -Wextra` line their packages had.

## Configuration and provisioning

`services/llm/config.toml.example` had a three-key stub `[memory]` block; it
now carries the package's whole `[memory]`, `[extract]` and `[nats]` blocks
verbatim, including `schema_file = "database/schema.sql"`, which is the line
that makes the moved schema the service's own. The deploy configs
(`argus-deploy/config.llm.toml.example` and the host-local
`config.llm.toml`) already carried these blocks as the host's, so nothing there
moved.

`services/llm/scripts/provision.sh` absorbed `setup_memory_model` (the e5-small
ONNX pair) and `setup_extract_model` (NuExtract Q4_K_M, byte-verified at
491400416) with their notices, so one script provisions the three artifacts the
brain reads. `scripts/setup.sh` no longer calls the package's provisioner and
no longer generates a config for it; `scripts/provision-host.sh` no longer
lists it among the units whose models it downloads.

## Deletions

- `packages/memory/` and `packages/intent/` in full, including their build
  trees, their `.gitignore` files, their `AGENTS.md`/`CONTEXT.md`, their
  `config.toml.example`, their `CMakeLists.txt` and the intent model pin's
  old home.
- `packages/memory/src/memory/memory-dto.{hxx,cc}` — 73 + 161 lines of the
  retired `/memory/v1` request bodies, with zero includers at `HEAD` and the
  only user of `lib/validation` and `lib/errors` inside the package. The
  service answers that wire from `feature/llm`'s own DTOs and always did.
- `packages/contracts/proto/argus/memory/v1/memory.proto` — the other half of
  that retired surface: `MemoryService` with `PutMemory`/`DeleteMemory`/`Recall`
  over `argus.common.v1.Envelope`. Measured dead before removal — no `CMakeLists`
  names it (the helpers compile only the protos they list), no `.proto` imports
  it, no generated `argus.memory.v1` type appears in any source file, and the
  only remaining mention of its path is inside a gitignored capture of an older
  phase. Deleting a proto is a wire change and is declared as one here, the same
  declaration steps 6a, 6b and 6c made for the four `argus/ai/v1/*.proto`
  skeletons; no CI gate enforces `buf breaking FILE` in this tree.
- The `memory-catalog` archive, whose one source,
  `catalog-replica.cc`, is one of the 15 the single `argus::memory` module
  compiles now, so the target that existed only to keep cnats out of an
  unwilling consumer has no reason left.

## The tool contract leaves the client

`packages/clients/llm/AGENTS.md` recorded the deviation §9.3 named: the
vocabulary sat in the tier-3 client because memory (a package) declared its
descriptors in it while the service executed them, and neither could include
the other's source. With both sides features of `services/llm`, the reason is
gone, so the header is `services/llm/src/shared/vocabulary/tool-contracts.hxx`
and the client keeps no tool machinery — which is what its own document says a
client is: the stub, the URL, the envelope and the retry policy.

## Scripts, deploy and the checker

- `scripts/build-all.sh` — the two package lines removed from `PROJECTS`.
- `scripts/setup.sh`, `scripts/provision-host.sh` — as above.
- `argus-deploy/docker-compose.yml` — the schema bind is
  `../services/llm/database/schema.sql`, the same container path as before.
- `scripts/lib/check-deps.py` — the `IN_TRANSIT` machinery (a name, a
  predicate, a clause feeding the "packages with no tier" note) is deleted
  with its last user, and the note is reworded to what it now means: a
  package outside the three groups. The measurement below shows why the
  machinery had nothing left to mark.
- `scripts/build-all-test.sh` — the synthetic fixture that used to create
  `packages/memory` (a tier-less package, to check that a cycle through one is
  still caught) is renamed `packages/stray`: memory is no longer a package, and
  a fixture naming a deleted folder is a lie in a file whose subject is truth
  about the tree.

## Documents

Updated in this unit: root `AGENTS.md` (the MemoryService paragraph, the
fastText bullet, rule 25's dependency-half paragraph — re-measured to 23
packages naming an alias by hand over 13 contracts / 12 clients / 15 libs, with
the two ungrouped packages now gone — rule 26's schema list, the phrase row and
the three Key Files rows), `services/llm/AGENTS.md`, `services/llm/CONTEXT.md`
(both new features documented, including what the merge retired),
`docs/architecture/build-model.md` (Fifteen owner projects),
`docs/architecture/data-storage.md`, `docs/architecture/wire-nats-subjects.md`,
`docs/architecture/services-and-packages.md` (two standalone packages, thirteen
contracts), `docs/architecture/camera-guardian-deep-analysis.md`,
`docs/operations/provisioning-and-models.md`,
`docs/operations/configuration-keys.md`, `argus-deploy/CONTEXT.md`,
`packages/clients/llm/AGENTS.md`, `packages/contracts/llm/AGENTS.md` and
`packages/lib/phrase/AGENTS.md`. The review pass corrected six of these and
seven more of the same kind; the section below is the record of what each one
had wrong.

`docs/history/` is a historical record and was not rewritten: its mentions of
`packages/memory` describe what was true when they were written, including the
plan rows that assign this move to this step. The plan itself is the exception
it always is: §9.3's forward reference ("step 6b/6c owe `vlm` and `llm` theirs")
was still waiting for work that had already landed, so it now records 6b, 6c and
the deleted memory skeleton.

## The review pass

Two adversarial read-only passes ran against the staged unit, one over the code
and one over the documents, and every finding was re-measured before it was
accepted. What the re-measurement changed:

- **`scripts/setup.sh` did not parse.** Removing the `packages/memory` line from
  the `for dir in` list took the loop's `do` with it, so the script died at line
  198 with `syntax error near unexpected token '||'` — every invocation, the one
  that generates the per-installation configs and calls `build-all.sh` included.
  No gate in this tree checks shell syntax, so the unit's own verification
  (`--only llm`, the two pre-build gates) could not have caught it. Fixed to
  `services/tunnel; do` and verified the way it was found: `bash -n` over
  `scripts/*.sh`, `scripts/lib/*.sh` and every `services/*/scripts/*.sh` parses.
- `services/llm/src/shared/CMakeLists.txt` declared no `DEPENDS` although the
  header it exports includes `<sync/role-permission.hxx>` and
  `<sync/table-name.hxx>`. It compiled only because both of its readers happen to
  link the contract themselves, and `check-deps` could not see the edge because
  there was no declaration to read. `DEPENDS argus::contracts::sync` added; the
  edge count below moves 858 → 859 precisely because that edge now exists to be
  checked.
- `scripts/lib/check-deps.py` carried four consecutive blank lines where the
  `IN_TRANSIT` machinery was removed.
- `services/llm/CONTEXT.md`'s config paragraph still read "No database, no NATS,
  no JWT/device keys — nothing here persists anything" while the service owns
  `database/schema.sql`, declares `[memory] db_file`/`schema_file` and `[nats]`,
  and the deploy stack binds `${ARGUS_DATA_DIR}/memory`. Rewritten to what the
  config now is, with `[intent]` named beside it.
- Eleven live documents still counted the retired projects as projects or named
  them as packages: root `AGENTS.md` (the `src/shared/` census, the clients
  paragraph — twelve clients wrap a stub, not ten, and six wire modules call
  `argus_client_module`, not four — the header-only contract count, rule 26's
  stale parenthetical and the project count), `docs/README.md`,
  `docs/operations/build-and-test.md`, `docs/architecture/build-model.md` (the
  third-party "Compiled by" column inside the file this unit had already
  touched), `docs/architecture/contracts-overview.md` (whose ASCII layout listed
  a deleted `ai` directory and the `memory` one, and omitted `auth`),
  `docs/operations/configuration.md`, `docs/operations/configuration-keys.md`,
  `docs/operations/tool-calling-eval-set.md` (the classifier is live inside
  argus-llm, not retired), `argus-deploy/CONTEXT.md`,
  `packages/clients/llm/AGENTS.md` (which contradicted itself: the move is
  recorded at the bottom of the same file), `packages/contracts/auth/AGENTS.md`
  (eighteen link lines at `HEAD`, twenty-five measured now) and
  `packages/contracts/sync/AGENTS.md`.

Recorded rather than changed:

- `scripts/provision-host.sh`'s `memory` entry is a data-directory loop, not a
  project list: it creates `${ARGUS_DATA_DIR}/memory`, which compose binds at
  `/opt/argus/memory`. The data belongs to the domain and argus-llm is the
  service that owns it, and rule 17b forbids moving a user's database as a side
  effect of a feature, so the path stays.
- The deployed argus-llm image carries no intent model. `services/llm/Dockerfile`
  copies the binary and its two library sets, compose mounts `models/llm` (and
  the memory and extract artifacts) but no `models/intent`, and
  `argus-deploy/config.llm.toml.example` declares no `[intent]` — so the
  container's router abstains and every turn stays on the LLM's own tool calling,
  the degradation the artifact was designed for, and `IntentGate` says so at
  boot: `IntentGate: no intent model at <path>; every turn stays on the LLM's
  tool calling`. This is not a regression from the move (no image ever carried the
  artifact, and the artifact is in-repo rather than provisioned), and closing it
  is a deploy-surface decision — a `COPY` in the runtime stage, or a host
  directory provisioned like the other models — that belongs to the deploy unit
  rather than to a move between two build units.

## Verified

- `./scripts/build-all.sh dev --only llm` — exit 0, 0 errors, 0 warnings,
  44/44 tests passed (the same 44 the project ran before: the seven moved
  suites were already reached through the two `add_subdirectory` calls).
- `./scripts/check-comments.sh` — 1373 files, 0 comments (1374 before the
  review pass deleted the dead memory proto).
- `./scripts/build-all.sh dev` — the whole tree, exit 0: the two pre-build
  gates, fifteen projects and the terminal clang-tidy gate, **458 tests, 0
  failures** (llm's 44 among them), **0 errors and 0 warnings** in first-party
  code — the eighteen `warning:` lines the log carries all point into vendored
  `third_party/ncnn`, which is what SYSTEM includes are for. The tidy gate's
  own line: `547 TUs, 2872 findings over 45 checks, baseline 2901; 11 checks
  below it`, exit 0, nothing above the baseline. 547 is one fewer than the 548
  the 6c full run measured, and the one translation unit this unit removed is
  `memory-dto.cc` — the moves change a file's project, not the scan's count of
  it. A per-project run cannot see that gate (`--only` skips it by design),
  which is why the run that closes the unit is the full one.
- `./scripts/check-deps.sh` — 123 declarations, 859 edges, 0 forbidden, 0
  cycles, 0 unresolved, **0 edges deferred** where step 6c recorded 22. The
  22 were memory's and intent's own edges: a package outside the three groups
  has no tier, so every edge it declared was deferred rather than checked. With
  the two packages gone the tier graph has no untiered node left, which is also
  what let the checker's `IN_TRANSIT` machinery go. The edge count read 858
  before the review pass added the `llm-shared` → `contracts/sync` declaration:
  the one edge the missing `DEPENDS` had been hiding is now a checked one.
