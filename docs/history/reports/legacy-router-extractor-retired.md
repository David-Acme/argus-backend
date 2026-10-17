# fastText and NuExtract retired: what they were, their numbers, and the swap to Laya and GLiNER

Date: 2026-10-17. Owner rulings: 2026-10-12 (Laya decides and GLiNER extracts; the router and the
off-turn extractor leave the turn) and 2026-10-16 (the released INT8 bundles ship now as the initial
MVP models, the quality merge gate is dropped, a retrain is a swap by replacing the bundle's files,
and RSS and decide/extract p95 are reported rather than gated).

## What each engine was

### fastText — the intent router's fast tier (2026-09-09 → 2026-10-17)

F9 of 2026-09-09 made fastText the router's fast tier in front of tool calling: the
`third_party/fastText` submodule (`1142dc4`, inference only, built as a static library by
`services/llm`), the classifier artifact `models/intent/intent.bin` (12.9 MB) carried in-repo with a
configure-time SHA-256 pin under `services/llm/src/feature/intent/models/`, a model card, the `[intent]`
config block, and its tests (`intent-router-test`, `intent-accuracy-test`, `fast-tier-eval`). The
training lived outside this repository, in the sibling `intent-training/` project; only the artifact,
its card and the frozen eval fixtures crossed over.

The classifier answered into six classes (`memory_save`, `memory_recall`, `reminder_set`,
`memory_forget`, `camera`, `none`) at an operating point of 0.90 / 0.10 — `none` meaning "no tool" —
and the families variant added fourteen module-family classes on top of those six (twenty in all,
trained through `intent-training`'s corpus tooling, scored in the harness through the adapter
`scripts/decider_fasttext.py`, margin 0.10). `IntentRouter` arbitrated its proposal against the rules:
an explicit trigger decided alone, a confident model outranked a rule, and a statement or a recall
marker stood only when the model agreed. `IntentGate` gated the decision and `RouterDecider` handed it
to the turn. Over 629 judge utterances the rules alone had decided with a precision of 0.44
(statements) and 0.31 (recall markers) against the model's 0.96, which is why the arbitration existed.
The explicit-trigger tier outlived the router as `TriggerDecider`.

Removed from the tree on 2026-10-12 (owner's ruling) with its classifier: the submodule, the in-repo
artifact and card with their pin, `[intent]`, `intent-router-test`, `intent-accuracy-test`,
`fast-tier-eval` and `tests/fixtures/intent/eval-check.tsv`. `tests/fixtures/intent/eval-production.tsv`
(real traffic) survived; `decider-eval.py --traffic` reads it.

### NuExtract — the off-turn memory extractor (→ 2026-10-12)

`models/extract/NuExtract-1.5-tiny-Q4_K_M.gguf`, run through llama.cpp by `ExtractionService` behind
`MemoryService::refineLater`: a note the lexicon tier could not structure was queued as an off-turn
job, the job waited for the chat engine to be idle (`memory.extract_wait_ms`), ran the extractor on
the utterance and replaced the note with a triple when the model found one. Inside a call's turn it
had measured 23 s of `tool_ms` on a loaded machine, and its idle-wait meant it, e5 and the main model
never ran at once on the same cores.

Removed 2026-10-12 together with the job and `memory.extract_wait_ms` (`ExtractionService::extractAsync`
had already been removed as dead, #111). The note is now final: the memory feature's capture is the
lexicon tier with its coverage floor (`CoveredLexiconExtractor`, `extract.lexicon_min_coverage`), and
the memory behaves as it always did on an install where the off-turn extractor is absent.

## The numbers (moved out of `docs/operations/voice-quality-eval.md`)

Baseline of the decider campaign, for the record: the fastText classifier with twenty classes, adapter
`scripts/decider_fasttext.py`, margin 0.10. Two readings on the judge corpus and the sealed set:

| reading | model sha256 | threshold | module-family coverage | precision | pooled false-route | near-miss stratum |
|---|---|---|---|---|---|---|
| first (trained on a corpus with 1,247 containment near-duplicates of the sealed set) | d8c93ec5... | 0.93 | 0.456 | 0.852 | 0.665% | 2.875% |
| clean (0 near-duplicates; same hyperparameters) | 44bd49cf... | 0.93 | 0.456 | 0.874 | 0.488% | 2.259% |

The clean reading was the bar the cutover had to clear: per family (coverage / precision / false-route)
calendar 0.500 / 0.756 / 0.456% (near-miss 1.700%), task 0.423 / 0.957 / 0.041%, project 0.630 / 0.944 /
0.041%, modules 0.403 / 0.931 / 0.083%, reminder list 0.286 / 1.000 / 0.000%; memory coverage 0.556,
precision 0.900, false action 0.167%; the real stratum had no false route at all. On the sealed sweep
the near-miss stratum was at or under 0.5% only at confidence 0.99 and above, where coverage was 0.180.
The model failed the decider gate and was not published as the decider: the false routes were near-miss
sentences that use the family nouns in the past tense or as plain nouns, and calendar is where nearly
all of them landed. The sealed set was read twice for fastText; any later number of it on the sealed
set is a tuned number and is labelled so.

The figure the gate-obeying router reached for comparison in every report was 0.154 coverage on the
selection set at the 0.1% ceiling (calibrated, `ACT >= 0.91 ASK >= 0.85`, 8,807 non-positives, upper
95% bound 0.10%) and 0.101 on the sealed set (the first sealed reading, at confidence >= 0.995 under
the 0.5% false-route gate that held then, so a tuned number, not the same ceiling as the selection
figure). Both sat in `gates.json` under `decider.gateObeyingFastText` and were printed beside the
coverage by `decider-eval.py` and `round-report.py`; the key and the prints were removed on 2026-10-17,
and these figures live here now.

On real traffic (turns that came from the product or lab sessions) at the selected threshold the clean
model sent 44.3% of the 61 production turns to a tool and left 55.7% to plain conversation (memory
coverage 43.1%); 44.6% of the 92 lab-session turns and 61.2% of the 103 check turns reached a tool.

The classifier's own split (`intent-training`, 5,649 test rows): the Wilson 95% lower bounds of
precision were 0.873 for `memory_save`, 0.840 for `memory_recall`, 0.908 for `reminder_set`, 0.839 for
`memory_forget` and 0.831 for `camera`, and the pooled precision of the fourteen family classes was
0.956 (lower bound 0.936); held-out `none` rows fired a module family 0.170% of the time (ceiling 0.5%)
and `memory_save` 0.089% (ceiling 0.25%). Three of its gates missed on that build (`memory_recall` by
0.010 and `camera` by 0.019 on the lower bound, `none` recall on the case judge 0.968 against 0.97),
which is why the decision to publish was taken on the sealed reading and not on them.

### Round 0 of the fine-tuned decider, on the selection sets only

Laya (`jhu-clsp/mmBERT-base` fine-tuned on 5,978 folded rows, 2 epochs) against fastText families-v1.1
on the same selection set (the judge corpus, the fresh holdout and 2,500 real negatives; the sealed
sets untouched), module families pooled, ACT only:

| ACT at | decider | coverage | precision | wrong tool | wrong ACT | near-miss stratum |
|---|---|---|---|---|---|---|
| 0.90 | Laya | 0.849 | 0.869 | 1.59% | 1.005% | 3.09% |
| 0.90 | fastText | 0.669 | 0.966 | 1.99% | 0.100% | 0.617% |
| 0.93 | Laya | 0.653 | 0.906 | 0.80% | 0.502% | 2.06% |
| 0.93 | fastText | 0.622 | 0.969 | 1.59% | 0.067% | 0.412% |
| 0.94 | Laya | 0.167 | 0.933 | 0.40% | 0.067% | 0.206% |

Laya's confidence was compressed (it stops near 0.945 and falls off a cliff above it), so thresholds
had to be fitted by calibration before two deciders were compared at equal wrong-ACT; no non-trivial
ACT / ASK policy met the 0.1% ceiling on that selection set for either decider. The production router
of that day (`appCommandFor` + the six-class fastText memory router, behind
`tests/eval/production-decider`) routed no module family and sent 3.98% of module-family requests to a
memory tool.

### The fast tier's own cost

| decider | p50 / p95 ms | CPU ms per call | resident / peak MB | threads | artifact MB |
|---|---|---|---|---|---|
| fastText router, C++ dev build (removed 2026-10-12) | 0.28 / 0.39 | 0.27 | 54.8 / 54.8 | 1 | 12.3 |
| fastText 20 classes, Python adapter | 0.06 / 0.14 | 0.03 | 52.3 / 52.3 | 1 | 12.5 |
| Laya round 0, PyTorch fp32, CPU | 91.7 / 101.4 | 740.6 | 1,733 / 2,324 | 39 | 1,294 (615 weights) |

The fp32 form of either engine is not shippable under the 1 GB-per-model ceiling; the INT8 bundle is
the shipped form today.

Two more readings the swap retired: the fastText router before the arbitration wave sent 27.7% of the
negatives to a memory tool (productivity requests routed to a memory tool: 52.6%), which the
arbitration and the retrained classifier took to 1.4% (0.9%); and the tool handlers' off-turn
extractor had measured 23 s of `tool_ms` inside a call's turn on a loaded machine, the reason the
lexicon tier is the only extractor the handlers use.

### Superseded merge-gate bars (2026-10-12)

The gate as it stood on 2026-10-12, kept here because it is what the two engines measured on
`llm-tier-eval` in a prod build (789 cases) and what the cutover branch had to beat beside the
pilots; the owner's 2026-10-16 ruling dropped it — no comparison against master, no
`selection.accuracy` floor, no sealed-set re-scoring. Master `ef85ad81` was the tree whose decider
was the router and whose extractor was the off-turn one:

| metric | gate | master (the bar) | branch, pilots |
|---|---|---|---|
| `selection.accuracy` | ≥ 0.55 | 0.643 | 0.544 |
| `selection.memory.accuracy` | ≥ 0.60 | 0.241 | 0.008 |
| `tool.memory.remember.recall` | ≥ 0.50 | 0.228 | 0.025 |
| `tool.memory.recall.recall` | ≥ 0.45 | 0.520 | 0 |
| `tool.memory.forget.recall` | ≥ 0.60 | 0.156 | 0 |
| `tool.memory.remind.recall` | ≥ 0.75 | 0.013 | 0 |
| `tool.memory.remember.precision` | ≥ 0.85 | 0.947 | 1.0 |
| `tool.memory.recall.precision` | ≥ 0.93 | 1.0 | 0 |
| `tool.memory.forget.precision` | ≥ 0.90 | 1.0 | 0 |
| `tool.memory.remind.precision` | ≥ 0.90 | 1.0 | 0 |
| `falseCompletionRate` | ≤ 0 | 0.046 | 0.051 |
| app / camera / modules / productivity / none accuracy | | 0.68 / 1.0 / 0.92 / 0.96 / 0.99 | same (none 1.0) |

## Why they were replaced

Both engines were replaced by Laya and GLiNER on the owner's rulings, and the swap is
replacement-only: the bundles (`models/decide/bundle-laya-*` onnxruntime, `models/extract/bundle-gliner-*`
llama.cpp/ONNX) are installed into their slots by `services/llm/scripts/install-bundle.sh` (sha256 and
manifest verified, the pilot kept as `<slot>.previous`, `--rollback` restoring it), `installTurnEngines`
composes them into the turn, and a retrain is dropping new files into the slot — no code change and no
configuration picks another engine.

- **Laya decides**: a fine-tuned multilingual encoder behind `RuleDecider` in a `FirstOf`, with
  `TriggerDecider` ahead of it; when its bundle does not load, its model cannot run (`the Laya decider
  is off: ...`), the turn runs on the rules and explicit triggers. Its thresholds come from the
  bundle's own `decision.json`.
- **GLiNER extracts**: the slot layer's text engine behind `slots::TextSlots`; a bundle that does not
  load, a model that cannot run, or a span below the bundle's threshold falls back to the rule tier
  (`slots::RuleText`).
- **Degradation is a contract**: no model is a log line and the pipeline still answers, never a
  crashed turn.

## The MVP ratchet is the baseline

The owner's 2026-10-16 ruling dropped the quality gate the cutover had been parked on (no
`llm-tier-eval` comparison against the pre-cutover master, no `selection.accuracy` floor, no sealed-set
re-scoring of the INT8 exports). What stands instead is integrity plus a ratchet: the bundles in the
installer layout with their golden vectors passing, the degradation contract, the engines above gone
from build and provisioning, the llm suites green on a prod boot, and RSS and decide/extract p95
reported rather than gated.

The ratchet landed as the gates of `services/llm/tests/eval/gates.json` carrying the shipped MVP's own
readings on the integration pass (prod build, the pinned cell, an idle machine) as floors a later
bundle may not go under and ceilings it may not go over — `llm-tier-eval` and `call-faithfulness-eval`
both re-run green under the new bounds. The decider coverage bar (0.456) and the wrong-ACT ceilings
kept their values; what was removed with this record is the fastText-era comparison bar — the
`decider.gateObeyingFastText` key and the two lines `decider-eval.py` and `round-report.py` printed
from it — so no live gate and no report reads a retired engine's number. The shipped MVP budget the
gates were ratcheted against (p95 193.5 ms, 328.9 CPU ms per call, 972 MB resident, 3.9 s to load,
inside the owner's 1 GB-per-model ceiling) is recorded in `services/llm/CONTEXT.md`, "The budget the
MVP ships with".

## The retirement, in the tree

- The cutover unit removed the engines from the build and the provisioning: the `services/llm` CMake
  targets, the `[intent]` config, the submodule entry, the deploy mounts (`models/decide` where
  `models/intent` was), and the config gate that refuses a config still carrying `[intent]` or an
  `engine =` selection.
- This unit (2026-10-17) removed the last live mentions: the `gateObeyingFastText` key and its prints,
  the fastText-era tables and sealed-set notes in `docs/operations/voice-quality-eval.md` (their home
  is this record), the history paragraphs in `services/llm/CONTEXT.md` and `argus-deploy/CONTEXT.md`
  (replaced by one sentence each, pointing here), the superseded merge-gate bar table from
  `services/llm/CONTEXT.md` (the "Superseded merge-gate bars" section above), and the `RouterDecider`
  reference in the 2026-10-16 working-pass note.
- The models cleanup and the submodule's local git data stay with the owner, as the steps after he
  redeploys the new build; the git data under `.git/modules/` is shared by every worktree, so removing
  it while a tree still has the submodule checked out breaks that tree. The commands are in the unit's
  report.
