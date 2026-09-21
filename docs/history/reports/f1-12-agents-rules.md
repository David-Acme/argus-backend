# F1-12 — the rules file rewritten to the target architecture

Phase 1 step 12: *"Rewrite AGENTS.md rules 23–27 and the Key Files table to this
architecture: `src/server/` → `app/`, `feature/api/<resource>/` →
`feature/<resource>/{controllers,services,dtos}`, the 2+ rule for `shared/`, the
package taxonomy and the tier table of §2.4. The rules file is the contract every
future change is written against, so it lands with the layout, not after it"* — and,
discharged from step 11's handoff, the `argus-contracts` taxonomy row in
`docs/architecture/services-and-packages.md`.

Two tracked files change, both prose: `AGENTS.md` (+268/−57, 733 → 944 lines) and
`docs/architecture/services-and-packages.md` (+11/−3). No source, no CMakeLists, no
config.

## The row's four items, measured

| Item | Measured before | After |
|---|---|---|
| `src/server/` → `app/` | two units have `src/server/` (`services/gateway`, `services/tunnel`); `services/tts` is the only one with `src/app/`; the other nine keep `main.cc` at their `src/` root, and `tunnel` has two entry points there | rule 23 states `app/ = main.cc + rpc/`, names `src/server/` and a root `main.cc` as the pre-migration spellings, and lists what each service does today |
| `feature/api/<resource>/` → `feature/<resource>/{controllers,services,dtos}` | `notification`, `productivity` and `guard` already have the `{controllers,services,dtos}` interior — one level deeper, under `feature/api/<resource>/`; `notification` also has `feature/rpc/`; `tts` has `feature/synthesis/{api/http/{controller,dto},domain,infra/supertonic}` | rule 23 gives the target shape and says the `api/` segment and `feature/rpc/` are what goes; rule 10's two DTO paths follow |
| the 2+ rule for `shared/` | `shared/` exists in six services, each enumerated (`voice` `{services/{noise,reaction,vad},wrapper/audio}`, `camera` `{utils/geometry,repositories/{action-command,object-event-outbox},services/{tapo,evidence,stream,camera-driver}}`, `notification` `{repositories,services,schemas}/notification-token`, `llm` `{services/{llm,encounter-closed,tools}}`, `stt` `{services/stt}`, `vlm` `{services/vision}`); `guard` and `productivity` have none | it is a rule with a test, not a preference: rule 23 defines it, rule 3 carries the repository half, rule 24 makes "no directory without a consumer" the enforcement |
| the package taxonomy and §2.4's tier table | AGENTS.md had no taxonomy and no tiers; the plan carried both, in a file that gets archived when the migration ends | rule 25 gained two subsections: the three groups with their target names, and the five tiers with their eight rules |

## The rules are prescriptive, so they state the target — and say where the tree is not there yet

The row's closing sentence decides the hard part: the rules file is what a future
change is written against, which is why it lands *with* the layout rather than after
it. That makes it legitimate for a rule to describe the target while the tree is
still mid-migration — but only if no reader can mistake a target for a fact. Every
rewritten rule therefore ends with a bolded, measured "today" paragraph naming the
phase that closes the gap:

- Rule 23: which service has `src/app/` (one), which three already have the
  `{controllers,services,dtos}` interior, that the other nine keep a single
  `main.cc` at their `src/` root while `tunnel` has two entry points there,
  which five services have no `feature/` at all (`gateway`, `llm`, `stt`,
  `tunnel`, `vlm`) and which four of the six that do keep code beside it
  (`camera`, `notification`, `productivity`, `voice`), that no service has
  `tests/e2e/` and the only first-party one is `packages/sync/tests/e2e/`, that
  `tunnel` is exempt by design (D19) and that `gateway` is deleted in Phase 3d.
- Rule 23 also had to name where the per-service typed config lives **today**,
  because the target puts it in `src/config/` (D20) and it exists already: three
  services carry one under their domain folder (`camera-config.{hxx,cc}`,
  `notification-config.{hxx,cc}`, `productivity-config.{hxx,cc}`), plus a second in
  `camera` (`operator-config.{hxx,cc}`), three in the gateway and tunnel's
  `service-config.hxx`. "No service has `src/config/` yet" is true; "no service has
  a typed config" would not have been.
- Rule 25: the pre-migration names of all three groups — `argus_<name>` /
  `argus::<name>`, `contract-<domain>` / `contract::<domain>`,
  `argus_client_<domain>` / `argus::client-<domain>` — with the note that a change
  writes the spelling that exists until Phase 2 lands the helpers, and that the
  suffix survives in every contract folder but only four of the ten client ones.
- Rule 25's tier table carries the same note once, and rule 27's client spelling is
  given both ways.

**The tier table could not become rule 28.** Rules 23, 25 and 27 are cited by number
outside this file — 10, 13 and 13 files respectively (measured case-insensitively:
`cmake/argus-module.cmake:1`, `argus-deploy/AGENTS.md`, ten `packages/*/AGENTS.md`,
six client headers, `packages/contracts/CONTEXT.md`,
`services/gateway/tests/audit-sync-read-test.cc` and more). A new rule number would
have left every one of those citations pointing at a rule that is no longer the one
they mean, so the tiers went inside rule 25, whose subject they belong to anyway
(which group may link which).

## Two rules outside 23–27 that had to move with them

Both are cases where leaving them alone would have made the file contradict itself,
which is worse than a rule the row did not name:

- **Rule 3** declared `<owner>/src/shared/repositories/{entity}/` as *the* home of
  every repository. Under the 2+ rule a repository with one reader belongs in its
  feature, so rule 3 now carries the home rule and points at rule 23; its shape
  block is unchanged, because the file layout inside a repository is the same either
  way.
- **Rule 10** gave the DTO path twice as
  `<owner>/src/feature/api/{resource}/dtos/` — the exact shape this row retires. It
  is `<owner>/src/feature/{feature}/dtos/` now, and its WS/sync DTO sentence names
  `packages/sync/src/feature/socket/sync/dtos/` as today's location with its
  Phase 3a destination.

## The Key Files table, regrouped by the taxonomy

48 rows → 51 (the SDK/clients tier had no row at all; the review added two more),
under seven heads: any owner (the two generic rows, with the 2+ rule folded into the
repositories row), tier 1 `lib/`, tier 2 `contracts/`, tier 3 `clients/`, tier 4
`lib/auth`, tier 5 services, plus docs and templates. A preamble states that the
paths are today's spellings, that the section head names each unit's target group,
and that Phase 2 moves a path for two different reasons.

Every row that points at a unit which is scheduled to **die or leave `packages/`**
now says so in its own cell — `packages/memory/*` (feature of `services/llm`,
Phase 4 step 7), `packages/identity/*` (`services/identity`, Phase 3c),
`packages/sync/*` (Phase 3a), `packages/socket/*` and `packages/room/*` (die in
Phase 3a; vocabulary to `contracts/sync`, transport to `services/sync`), the three
`packages/audit/*` rows (`services/sync` owns the three tables and is the only
writer), and `services/tts`'s `domain/` → `services/` rename. That is the half of
the table an agent is most likely to act on wrongly, because those paths exist and
look permanent.

Nothing was dropped: the four auth-filter rows, `role-access.hxx`, the JWT service
and the runtime/text/audio/storage/sqlite/phrase/config rows all survive, and the
new tier-3 row names the ten SDK clients and the alias callers link.

## The `argus-contracts` row — step 11's handoff

`docs/architecture/services-and-packages.md` listed `argus-contracts` as a standalone
package that "owns a Conan/CMake graph and builds on its own". Measured:
`packages/contracts/` has no `CMakeLists.txt`, no `conanfile.txt` and no
`CMakePresets.json` at all — the folder is a directory of ten `*-contract`
subfolders, and each of those has a `CMakeLists.txt` and nothing else. The row was
false in both directions: not a project, and not a member of the list it sat in.

Now: the standalone table is the seven folders that really carry the graph
(`cert`, `identity`, `intent`, `memory`, `socket`, `sqlite`, `sync` — measured as the
folders holding `CMakeLists.txt` + `conanfile.txt` + `CMakePresets.json`, the same
seven that declare `project(argus-<name>)`), with one sentence stating what makes the
claim a build fact. The direct-import list, which named 9 of the 35 such folders,
now names all 35: 15 `packages/*`, the ten `*-contract` packages and the ten SDK
clients.

## What measurement corrected in the plan

Three plan statements did not survive the tree, and the rule text avoids repeating
them:

1. **§2.6 says `argus_module`'s hard-coded `STATIC` blocks `validation`, `text` and
   `phrase` from being header-only.** `text` and `phrase` are not header-only
   candidates at all: `packages/text/src` holds 3 `.cc` and 6 `.hxx`, and
   `packages/phrase/src` holds 3 `.cc` and 10 `.hxx`. Only `validation` has no
   compiled source (0 `.cc`, 3 `.hxx`) — and it is declared `STATIC` over those
   headers with a `LINKER_LANGUAGE CXX` workaround
   (`packages/validation/CMakeLists.txt:20`). Rule 25 states exactly that, so the
   `HEADER_ONLY` option is Phase 2 work for one package, not three.
2. **Phase 4 never names `services/voice`.** §9.2's layout column gives `voice`
   `app/`, gRPC — but Phase 4's steps cover `tts` (1), `notification` (2), `camera`
   (3), `productivity` and `guard` (4), `stt`/`vlm`/`llm` (5–6), `memory`/`intent`
   (7), the layout blocks (8) and config (9). `voice` is neither legacy-horizontal
   like step 5's three nor named in step 4, so as written the phase leaves it with
   `main.cc` at the root and gRPC outside `app/rpc/` while the rules say otherwise.
   Flagged for the phase's owner below.
3. **§9.2 and §2.3 both give `services/tts` an empty `src/shared/`** — §9.2 as
   "`app` + `feature` + `shared` (empty)", §2.3 as "its empty
   `src/shared/services/` disappears". There is no `services/tts/src/shared` at
   all: the folder holds `app/` and `feature/` only, so Phase 4 step 1's "empty
   `src/shared/services/` goes" is a no-op and the file count in §9.2's row (`39`)
   counts a directory that is not there. Harmless to the phase, but it is the third
   claim in the plan about this service's shape that the tree does not support.

Rule 26 also needed one correction of its own, found while writing it: the gateway's
`database/schema.sql` is **not** a copy of the identity schema. It is a 32-line file
of its own — `Gateway schema (gateway.db)`, "degraded-fallback record only … this
file must never gain their tables" — and the gateway mounts both it
(`/opt/argus/gateway/schema.sql`) and the identity one it hosts
(`/opt/argus/database/schema.sql`). The first draft of the rule said the copy *was*
the identity schema; the md5 comparison (`401aceb2…` against `c21e7d77…`) and the
file's own header disproved it, and the rule now says what the tree does.

## What the self-review caught before the review returned

While the full gate ran, every "today" claim the new text makes was re-measured
against the tree. Four were wrong, all of them the same way — a claim about what
a folder *is* written from a partial listing — and all four are fixed:

1. **"every service except `tts` keeps `main.cc` at the root"** — `tunnel` does
   not: it has **two** entry points at its `src/` root, `main-client.cc` and
   `main-relay.cc`. The sentence now names the nine that keep a single `main.cc`
   and gives tunnel's two, with D19 beside them.
2. **"`camera`, `llm`, `stt` and `vlm` also keep code outside `feature/`"** —
   `llm`, `stt`, `vlm`, `gateway` and `tunnel` have **no `src/feature/` at all**
   (measured: one `ls -d services/*/src/feature`), so "also … outside" was
   backwards for three of the four named. The paragraph now splits the services
   into the five without a `feature/` and the four of the six with one that keep
   code beside it, and says `guard` and `tts` keep everything inside.
3. **"the ten contracts and `cert` are `INTERFACE`"** — both halves were wrong.
   Nine contracts declare `add_library(contract-<domain> INTERFACE)`; the tenth,
   `response-contract`, is a *function* that defines `argus_client_response-wire`
   through `argus_client_module` and compiles `response-rpc.cc`
   (`packages/contracts/response-contract/CMakeLists.txt:16-22`). And `cert` is
   `argus_module(NAME cert ...)` — `STATIC`, one `.cc`
   (`packages/cert/CMakeLists.txt:92`, `src/shared/services/cert/cert-service.cc`).
   The bullet now names the nine, says what the tenth is, and stops calling
   `cert` header-only.
4. **"contract and client folders still carry the suffix"** — true of all ten
   contract folders and of only **four** of the ten client ones
   (`llm-client`, `stt-client`, `tts-client`, `vlm-client`; the rest are already
   `camera`, `identity`, `notification`, `productivity`, `voice`,
   `camera-actions`). The note now says exactly that.

Three of the four came from generalising a folder listing into a rule sentence
without listing the folder. The measurement that catches the whole class is
cheap and is now part of this step's checks: `ls -d` each shape the paragraph
claims and diff its output against the prose.

Also checked and clean: the Key Files table's 51 rows and 7 subheads, with every
one of its path tokens resolving to the tree or explicitly marked as a target;
the markdown tables (0 blocks with mismatched pipe counts); every `rule <n>`
reference inside `AGENTS.md` resolving to a heading that exists; and no added
prose line over 78 columns.

## The review

A read-only adversarial review of the changed lines ran against the tree and
returned **fourteen numbered findings plus two side notes**. Every one was
re-measured here before being acted on; all fourteen were real, and all fourteen
are fixed. It also re-raised two findings against the revision it started from
(`cert` claimed `INTERFACE`, "the ten contracts are `INTERFACE`") that the
self-review above had already fixed mid-read — it verified the fixed text.

The findings fell into three classes, and the first class is the one worth
naming: **five were symbols attributed to the wrong owner**, which is the same
failure the step-11 review caught in the audio row, arriving again from the
regrouping:

| # | Finding | Measured | Fix |
|---|---|---|---|
| 1 | Rule 25 said a client is `argus_client_<domain>` / `argus::client-<domain>` | Four of the ten are `argus_module(NAME <x>-client)` (`packages/clients/{llm,stt,tts,vlm}-client/CMakeLists.txt:39,15,32,14`), so their alias is `argus::llm-client` — `argus::client-llm` fails to configure. Plan §2.5 records this drift | the today-note names the four and says write the spelling that exists; the tier-3 row and rule 27 follow |
| 2 | Rule 25's "a target name is never spelled by hand in a `DEPENDS` list — the helper derives it" | No derivation exists: `cmake/argus-module.cmake:35-36` passes `DEPENDS` verbatim, and **three** `CMakeLists.txt` spell `argus::` names in a `DEPENDS` list (`packages/validation/CMakeLists.txt:17`, `packages/contracts/response-contract/CMakeLists.txt:21`, `packages/contracts/tts-contract/CMakeLists.txt:28`) | kept normative, with a today-marker giving the pass-through and the three sites — a rule the helper does not yet hold up is marked, not dropped |
| 3 | The Key Files preamble said "nothing else about these rows moves" | Phase 2 step 2 is "Apply the package layout of §2.3 to every package", which replaces today's `src/shared/...` interior with `src/<name>/` | the preamble now lists both moves and says a path can move for two reasons |
| 4 | `PrivatePortraitService` listed under `packages/storage/` | declared in `packages/identity/src/shared/services/storage/private-portrait-service.hxx:14`; greps of storage's folder find nothing | removed from the storage row (which keeps `S3StorageService`) and given its own identity row |
| 5 | `jsonToString`/`jsonFromString` under `packages/text/.../json-util/` | `json-util.hxx` declares `namespace json_util` with `toString`, `isValid`, `fromString`; the only `jsonToString` in the tree is a file-local helper in a sync test | row now names the three real functions |
| 6 | `RuleParser`/`PhraseCatalog` listed as `packages/memory` contents | both are declared in `packages/phrase/src/shared/services/memory/`, and are read by `memory`, `intent`, `services/llm` and `services/voice` | removed from memory's row; a new tier-1 phrase row carries them with their four readers |
| 7 | Tier 2's "the enums every CHECK-constrained column uses" | `notification-delivery-status.hxx` (a contract) and `packages/identity/src/shared/vocabulary/person-status.hxx` each mirror their own schema's CHECK | "every" is gone; the row names the four domains' wire enums and where the other two live |
| 8 | Tier 3's path template `packages/clients/<domain>/src/<domain>/` | true of 6 of 10: the four wire clients hold `src/shared/`, and `camera-actions` holds `src/camera/` | the exceptions are inline in the row's own cell, since the section header calls these paths today's |
| 9 | Rule 26's "every config points at `database/schema.sql`" | false for the gateway, the one owner the same sentence then discusses: `argus-deploy/docker-compose.yml:51-52` mounts its own file at `/opt/argus/gateway/schema.sql` and `argus-deploy/config.gateway.toml:10` is `schema = "gateway/schema.sql"` (its comment at `:7` states the exception) | the sentence now names the gateway as the exception and its two mounts |
| 10 | Rule 24's anti-speculation example "an `infra/` folder whose adapter no second feature touches" | contradicts rule 23 two sections earlier, which gives each feature its own `infra/` adapters, and plan §2.3 | example replaced with "a module declared but linked by nothing" |
| 11 | Rule 24's "a `details/` folder with one type" | contradicts plan §2.3:176-177, whose own errors package has `details/code-table.hxx`, one type | example dropped |
| 12 | Rule 24's "a package is `src/<group>/...` under its group" | no package has `src/lib`, `src/contracts` or `src/clients`; §2.3 gives `src/<name>/` (`packages/lib/errors/src/errors/`) | "a package is `src/<name>/`, its own name as the include root" |
| 13 | Rule 23's tree gave no `scripts/` or `tools/` | rule 24 names `tools/`, plan §2.3 has both, and six services have `scripts/` while five have `tools/` | both added to the box |
| 14 | "the plan lists `validation`/`text`/`phrase` as header-only; AGENTS.md says `text` and `phrase` compile" | the tree supports the new text — **the plan is stale, not the change** | no change; it is correction 1 of the section above, and the review's `OperatorConfig` note confirms correction 2's shape |

The two side notes: "the tree's only `tests/e2e/`" was over-strict — a vendored
`third_party/llama.cpp/tools/ui/tests/e2e` exists — so the sentence says
"first-party" now; and `docs/architecture/services-and-packages.md` naming
`argus-grpc` in the direct-import list while `packages/grpc/CMakeLists.txt`
declares no target for the folder itself is **left as it is**, because the plan
already carries it as an open item ("`packages/grpc` still declares no target
for the folder itself — both are Phase 2 decisions", plan row 8) and the list
inventories folders, not targets.

Two claims the review listed as unverified were checked here and hold: the
repo root's "no source tree of its own since f7-8" is confirmed by
`docs/history/project-log.md:2905` ("`src/` was deleted at f7-8"), and the
`docs/architecture/services-and-packages.md` claims it did check — the
"Seven" build fact, the ten `*-contract` folders, the 15-name direct-import
list, and `argus-vlm-client` being linked only by `argus-guard`
(`services/guard/CMakeLists.txt:144`) — all measured true.

## Verification

The step touches no source, no `CMakeLists.txt` and no config, so the gate is
there to prove exactly that — the phase gate, `./scripts/build-all.sh dev`, run
over the tree at `2ff3ec5` plus these edits:

- **`GATE_EXIT=0`**, 18/18 projects, and every one of the 18 suites reports
  `100% tests passed, 0 tests failed`.
- **302 reached tests** — the same ledger as step 11's 302, which is the check
  that matters here: a documentation step that moved a test count would mean it
  had touched something it should not have.
- **0 first-party warnings**, 0 errors. The log's 23 `warning:` lines are all
  third-party: eight `CMAKE_CXX_STANDARD` notes from `third_party/ncnn`, eight
  from its `glslang`, three from `third_party/llama.cpp/ggml`, one from
  `_deps/openfst-src`, and three `ccache not found`.

The document checks, all re-run after the last edit:

| Check | Result |
|---|---|
| Key Files rows / subheads | 51 / 7 |
| Path tokens in those rows | every one resolves to the tree or is explicitly a target/future path; 14 relative fragments (e.g. `s3-signing.hxx`, `domain/`) verified against their own row's unit |
| Markdown tables, both files | 0 blocks with mismatched pipe counts |
| `rule <n>` references inside `AGENTS.md` | all resolve to a heading that exists |
| Added prose lines over 78 columns | 0 (table rows exempt, as elsewhere) |
| The four rule citations outside the file | still 10 / 13 / 13 files for rules 23 / 25 / 27, unchanged, so no citation was silently repointed |
| Change set | `AGENTS.md`, `docs/architecture/services-and-packages.md`, this report — nothing else (`git status --porcelain` reports ` M` twice and one untracked file) |
