# Phase 1 step 9 — the llm tool runtime moves into its service, and the vocabulary waits

Scope: the row is "Move the llm tool framework out of `packages/clients/llm-client`
(`tool-registry`, `tool-executor`, `tool-validator`, `tool-contracts`) into `services/llm` as a
feature — it has one consumer and is not a client (D16)". The row's list is four files; the
measurement below says they are two different things, that only one of them can move now, and
that the reason is the same constraint an earlier commit recorded when it put them there. Base
`b24ae92`.

## What the tree says about the row's premise

The row's reason is a count — "it has one consumer". Counted against the tree, the four files
are not one unit:

| file | production consumers |
|---|---|
| `tool-registry.{hxx,cc}` | `services/llm` — `src/main.cc:173` registers, `src/controllers/llm-controller.cc:38-42` lists the names, `src/shared/services/llm/lfm-adapter.{hxx:68,cc:442}` reads the singleton |
| `tool-validator.{hxx,cc}` | `services/llm` — only through the executor (`tool-executor.cc:18`) |
| `tool-executor.{hxx,cc}` | `services/llm` — the adapter holds one (`lfm-adapter.hxx:100`) |
| `tool-contracts.hxx` | **two** — `services/llm` (the three headers above, `lfm-adapter.hxx:4`) **and `packages/memory`** (`memory-formation.hxx:5`, `memory-tool-descriptors.hxx:3`, `memory-service.hxx:10`) |

So the machinery behind D16's own sentence — "a single-consumer framework shipped inside a
client (the llm tool registry, executor and validator) is a feature of its service" — is exactly
that: three files, one production consumer. The vocabulary is not: `packages/memory` declares
its descriptors in `tools::ToolDescriptor` and takes a `tools::ToolCall` in
`MemoryFormation::form(...)` (`memory-formation.hxx:72`), and it is a **package**.

That word is the whole blocker. §2.4's table puts services at the top, tier 5, and §1's fourth
principle is the one that meets here: "no reaching into another service's `src/`", alongside rule
1's "a tier may never point back up, and the graph has no cycles". A package may not include a
service's header, so `tool-contracts.hxx` cannot live in `services/llm` while memory is a
package — and the plan itself schedules memory's exit: Phase 4 step 7, "`memory` and `intent`
leave `packages/` and become features of `services/llm`". That is the step after which the
vocabulary can follow, and it is why §9.3's four-file list cannot be executed as written today.

**The history is the same constraint read backwards.** `b05c7f3` (f8-b2) moved
`tool-registry`, `tool-validator` and `tool-executor` *into* the client, and its message says why:
"lfm-adapter.hxx (argus-llm) included tool-executor.hxx (argus-memory), so the tool loop could
not compile inside llm-core no matter what: argus-llm cannot see argus-memory's source root."
The client was then the only tier both sides could reach; it still is for the vocabulary, and the
machinery no longer needs it because its consumer is one service.

**And the reason f8-b2 gave for the placement does not hold.** Its message — repeated in this
package's `CMakeLists.txt` until this step — says "the descriptors travel with the chat
contract". Measured: they do not travel anywhere. `ChatRequest` carries one tool-shaped field,
`bool toolsEnabled` (`llm-service.hxx:29-30`); `llm-remote.cc:306-307` writes `body["tools"] =
false` and nothing else tool-shaped; the declarations the model reads are built **server-side**
from the registry (`lfm-adapter.cc` `buildToolDeclarations`, fed by
`llm-controller.cc:38-42`). Nothing in `llm-service.hxx`, `llm-remote.cc` or `llm-remote.hxx`
names a `tools::` type. The vocabulary's home is therefore decided by the tier table alone, and
this step corrects the comment rather than repeating the claim.

## The decision

1. **The machinery moves**: `tool-registry.{hxx,cc}`, `tool-validator.{hxx,cc}`,
   `tool-executor.{hxx,cc}` → `services/llm/src/shared/services/tools/`. D16 names precisely
   these three, their one consumer is that service, and the plan's destination for the framework
   is `llm`.
2. **The vocabulary stays**, in `packages/clients/llm-client/src/shared/contracts/tool-contracts.hxx`
   — the tier-3 package both sides already link — and the reason is written where a reader will
   meet it: the header's own comment, the package's `CMakeLists.txt`, its `AGENTS.md`, and an
   annotation on §9.3. It follows the machinery when Phase 4 step 7 folds memory into that
   service.
3. **Memory's seam is re-cut**, because the move forces it: `memory-service.hxx` named the moved
   type in its public API (`void registerTools(ToolRegistry& registry)`), which is the *other*
   half of the row's premise. `MemoryService::toolDescriptors()` now returns what the service
   offers with its handlers bound, and the registry's owner registers them — `src/main.cc:172-175`.
   That is also what makes D16's "one consumer" true of the machinery instead of nearly true:
   after this step the registry is named by one service and nobody else.

**One deliberate deviation from the row's letter**, recorded rather than glossed: `tool-contracts`
did not move, for the tier reason above. §9.3's bullet is corrected in place, in the style §9.1's
`room` row was corrected in Phase 1 step 6.

## The move costs no include line

`argus_client_module`/`argus_module` make `${src}` the include root, and both trees are rooted the
same way — llm-client's `INCLUDES src` and `llm-core`'s `target_include_directories(... PUBLIC
${LLM_SRC_ROOT})` — so `<shared/services/tools/tool-registry.hxx>` resolves to the moved file
without a single `#include` changing anywhere. The same property step 8 leaned on for the
generated health header.

What did change:

| file | change |
|---|---|
| `packages/clients/llm-client/CMakeLists.txt` | the three tool sources leave `argus_llm-client`; the comment paragraph now explains what stays and why |
| `packages/clients/llm-client/AGENTS.md` | layout line and a new rule: no tool machinery here, the vocabulary only |
| `tool-contracts.hxx` | header comment: who consumes it, why it waits here, and the correction that nothing here crosses the wire |
| `services/llm/CMakeLists.txt` | three `.cc` into `llm-core`; three into `llm-wire-test`; the new suite's target |
| `services/llm/src/main.cc` | the registration loop over `memory.toolDescriptors()` |
| `packages/memory/.../memory-service.{hxx,cc}` | the seam |
| `packages/memory/tests/unit/memory-reminder-test.cc` | drives the handlers directly |

`llm-wire-test` is the one target whose source list had to grow: it does not link `llm-core`, it
builds its own copies of `llm-controller.cc`, `lfm-adapter.cc`, `intent-gate.cc` and the engine
(the PORTED pattern this file already follows) and links `argus::llm-client` + `memory-core` —
neither of which carries the runtime any more. Adding the same three sources is what the file
already does for the loop it recompiles. `argus-tool-bench` and `llm-tool-parse-test` link
`llm-core` and needed nothing.

## The suite that used to be the pipeline's only cover

Measured before touching anything: `ToolExecutor` was constructed in exactly one test in the
whole tree — `packages/memory/tests/unit/memory-reminder-test.cc:87-89` (at `HEAD`), which built a
local `ToolRegistry`, asked the service to fill it through `registerTools`, and drove its calls
through resolve → validate → `role_access::hasAccess` → handler. That test is a package's, and a
package may not link a service's target, so after the move the pipeline is unreachable from it.

The pipeline is now covered where it lives: `services/llm/tests/unit/llm-tool-runtime-test.cc`,
registered as `llm-tool-runtime-test`, with the cases memory's suite never had —

- an unknown name is refused and a **near miss of a registered name** is too, so resolution is
  proven to be a lookup rather than a prefix (`probe.forgotten` against `probe.forget`);
- the schema's three rejections, each with the message it produces: a missing required argument,
  an enum value the schema does not list, a number argument that arrives as text — plus arguments
  that are not an object at all;
- the role gate refuses a **guest** (whose table carries no `Memory` row — `Guard`'s is the same,
  `role-access.hxx:54-71`) and the identical call is granted to a resident, which is what makes the
  refusal the gate's and not the schema's;
- a dispatch comes back under the tool that was called even when the handler stamped something
  else, and its `data` survives;
- the declarations are read from `memoryToolDescriptors()` — the real ones, not a copy — for the
  access pair of every tool and for each required argument, `bareCall` included.

The last two are the part that moves rather than grows: the descriptors' own declarations were
only ever exercised as a by-product of memory driving them. The **deny** path had no coverage at
all before this step — memory's suite executed every call as `UserRole::Resident`, whose `Memory`
row is `kFull` (`role-access.hxx:53`), so the gate could only ever say yes.

Memory's reminder test keeps every domain assertion it had (the reminder forms for the speaking
user, another user's recall never sees it, the routed `decided` path) and now calls
`tool("memory.remind")->handler(...)` directly, with its comment saying where the pipeline went.

## Verification

- **The phase gate: `./scripts/build-all.sh dev` exits 0 on all 18 projects**, every suite green
  (cert 15, socket 8, sqlite 2, identity 15, sync 19, memory 16, intent 4, gateway 25, camera 34,
  productivity 23, notification 28, guard 36, tts 9, stt 3, vlm 5, **llm 24**, voice 13,
  tunnel 10). The reached-test ledger is **289**, exactly the 288 the phase has carried since
  step 4 plus the one new suite — every other project's count is unchanged, which is what a move
  that adds no test to any other project should look like. No first-party warning in the run:
  every `Warning:` line is llama.cpp's `CMAKE_CXX_STANDARD` notice, conan noting ccache is
  absent, or ncnn/glslang/openfst's own, the same classes the phase has recorded since step 4.
- **The two projects the step touches, run first**: `./scripts/build-all.sh dev --only llm`
  exits 0 with `100% tests passed, 0 tests failed out of 24` — 23 before the step, the new suite
  being the 24th — and `--only memory` exits 0 with `100% tests passed, 0 tests failed out of 16`,
  unchanged: the reminder test still runs, with the seam re-cut.
- The moved machinery has no consumer left outside `services/llm`: a grep of every `*.cc`/`*.hxx`
  for `<shared/services/tools/` returns nine lines in seven files, all inside `services/llm` —
  three service sources (`main.cc:17`, `llm-controller.cc:5`, `lfm-adapter.hxx:7`), the machinery's
  own two internal includes (`tool-executor.hxx:4` of the registry, `tool-executor.cc:4` of the
  validator), the bench (`tool-calling-bench.cc:10`) and the new suite (three lines). No line under
  `packages/` names that path.
- The vocabulary's two-sided use is the finding that keeps it where it is, and the grep shows it:
  four files under `packages/memory`, five under `services/llm`.
- No first-party warning in either run: the only `Warning:` lines are llama.cpp's
  `CMAKE_CXX_STANDARD` notice and conan's `ccache not found` note, the same classes the phase has
  recorded since step 4.

## Findings outside this unit

- **Row 4's flag for this step is only half discharged.** It said the framework this row moves
  "is the only thing that reads `role_access`, so that move removes the edge by construction".
  The machinery left; the tier-3 → tier-4 edge did not. `tool-contracts.hxx:5` includes
  `<shared/access/role-access.hxx>`, both to name `RolePermission` on `ToolDescriptor`
  (`:58-59` — `TableName` arrives through the same header), and `llm-client/CMakeLists.txt:45`
  still declares `argus::auth`. What this step narrowed is the *reason*: one header, two fields,
  no source, no logic. Phase 2 step 4's mechanical DAG check will still find the edge; the
  candidate fix is visible from here — `RolePermission` is gate vocabulary, and D12 says the
  vocabulary that crosses the wire is declared once in the contract that owns it, so a
  `contracts/auth` declaration paired with the `TableName` `sync-contract` already freezes would
  leave this client tier-2-only. Recorded, not decided: moving a type between packages is not
  this step's diff.
- **Memory's `argus::auth` dependency is transitive.** `memory-service.hxx` includes
  `tool-contracts.hxx`, which includes `<shared/access/role-access.hxx>` — memory's public header
  therefore needs auth's include root, and it reaches it through `argus::llm-client` →
  `argus::auth` rather than by declaring it. Rule 4 ("interface dependencies are `PUBLIC`") is
  what makes this a finding; Phase 2 step 4's mechanical DAG check is where it should surface.
  Recorded, not fixed: it is not this step's diff.
- **The bench mirrors memory's descriptors, and its mirror has drifted.**
  `tool-calling-bench.cc:129-143` rebuilds `memory.remember` by hand, with a comment saying it
  mirrors `memoryToolDescriptors()`, and it omits the `confidence` argument the real descriptor
  declares (`memory-tool-descriptors.cc:38-42`). Left alone deliberately: the bench measures the
  schema cost on first-token latency and its numbers are f8-b1's recorded accuracy baseline, so
  changing the schema it feeds the model would invalidate the comparison. It links `llm-core` and
  keeps compiling; whoever re-baselines should read the descriptors instead of copying them.
- **`services/llm/CONTEXT.md:91-92` was already stale.** Its "What it did NOT change" section says
  "No tool loop: `LfmAdapter`/`ToolRegistry`/`chatWithTools` stay legacy-side (Ruling BV)" — the
  loop landed inside this service in f8-b4, before this step. Phase 1 step 11 is the row that
  gathers stale docs, so it is recorded here rather than edited.
- **`docs/history/plans/tool-calling-reduction-plan.md:173`** still names
  `MemoryService::registerTools()` and two `labs/` binaries. It is a dated historical plan
  (2026-08-20) superseded by the v4 architecture plan; this step honours its intent — the
  machinery is preserved, only its address changed — so the line stays as history.
- **llm-client keeps one header that is not the wire.** That is the interim §9.3 records: the
  folder's "two things" become the wire client plus one vocabulary that memory and llm both
  need, until Phase 4 step 7. `services/llm/AGENTS.md:20-21` ("the `LfmAdapter`, `ToolRegistry`
  and the fast intent gate are part of the brain") needed no edit: this step is what makes it
  true.
