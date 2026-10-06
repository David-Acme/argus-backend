# Voice quality evaluation

What Argus understands when somebody speaks to it is measured, not reviewed. Since the owner's
decision of 2026-10-06 the LLM no longer chooses tools: a decider (rules, a learned classifier, a
fine-tuned model) picks the tool, a slot layer fills its arguments, the executor runs it through the
capability filter and the spoken confirmation, and the LLM only speaks about the result. So four
things are measured, each against numbers kept in the repository, and a change that makes a gated
number worse fails the test that carries it: the **decider** (which tool, how often it is wrong),
the **slots** (the arguments, and that a missing one is asked for, never guessed), the
**conversation** (what the LLM says about a result) and the **speech recogniser**.

Measured on 2026-10-06 on the development machine (Ryzen 7 5725U, 16 threads, 32 GB, CPU only).

## How to run it

```
python3 -I services/llm/tests/eval/decider-eval.py --decider '<command>' \
    --gates services/llm/tests/eval/gates.json \
    --select services/llm/tests/fixtures/eval/cases.jsonl <holdout.jsonl> \
    --select-negatives <real-negatives.txt> \
    --traffic services/llm/tests/fixtures/intent/eval-production.tsv \
    [--cache <file>] [--errors <file>] \
    [--sealed services/llm/tests/fixtures/eval/sealed.jsonl --sealed2 services/llm/tests/fixtures/eval/sealed2.jsonl --final]
python3 -I services/llm/tests/eval/perf-eval.py --decider '<command>' --artifact <model files> \
    --gates services/llm/tests/eval/gates.json
python3 -I services/llm/tests/eval/slot-eval.py --cases services/llm/tests/fixtures/eval/slots.jsonl \
    --gates services/llm/tests/eval/gates.json --filler '<command>'
python3 -I services/llm/tests/eval/conversation-eval.py --cases services/llm/tests/fixtures/eval/conversation.jsonl \
    --gates services/llm/tests/eval/gates.json --speaker '<command>'
ctest --test-dir services/llm/build/dev -L eval --output-on-failure

python3 scripts/stt-eval-data.py
ctest --test-dir services/stt/build/dev -R stt-wer-eval --output-on-failure
```

A decider, a filler and a speaker are processes that read one JSON request per line on stdin and
write one JSON answer per line on stdout, so the rule tier, the fastText families, a fine-tuned
model and the pipeline's own stages are scored by the same code and the same numbers. A command that
cannot start is a visible skip (exit 77), never a pass. The classifier is retrained and republished
from the sibling `intent-training/` project (`CONTEXT.md` there).

A slow decider (a fine-tuned model) is run in rounds by `run-round.py`, which splits the selection
set into chunks that each fit a fifteen-minute window, writes the decision cache after every batch
of 250 (a chunk that is killed loses at most one batch, and running it again continues), then scores
from the cache alone: first uncalibrated with `--calibrate-out`, then calibrated, then
`round-report.py` turns the two reports into the tables a review reads (the operating point, per
family and per variant, the price of each wrong-ACT ceiling, calibration, real traffic, performance).
Only the `perf` stage needs an idle machine and it refuses a busy one with exit 77.
`--slices <file>` scores the selection set again by a label of each case's text (a JSON object of
groupings, each mapping an utterance to a label) at the round's reference policy, which is how a round is
read by where its training data came from: `intent-training/scripts/slice_by_neighbours.py` writes the
labels (the license of a case's nearest trained-on row, and of its own row). A slice is an association with
the data a case resembles, not the effect of removing that data; that effect is measured by training without it.

```
python3 -I services/llm/tests/eval/run-round.py --name <round> --out <dir> --decider '<command>' \
    --select <cases.jsonl> <holdout.jsonl> <selection.jsonl> --negatives <real-negatives.txt> \
    --traffic services/llm/tests/fixtures/intent/eval-production.tsv --artifact <model files> \
    --runner '<prefix that bounds memory and serialises heavy jobs>' --chunks 8 [--stages fill,score,perf,report]
python3 -I services/llm/tests/eval/round-report.py --round joint=<dir> --round choice-only=<dir> --ceiling 0.01
```

The offered tools of a request are the tools the case's role holds, read from
`tests/eval/tool-visibility.json` (generated from the capability table and the providers' tool specs,
and checked against them by `tool-visibility-test.py`); a guard or a guest is offered seven tools, a
resident nineteen, an owner twenty-one.

## The corpora

| file | cases | role |
|---|---|---|
| `fixtures/eval/cases.jsonl` | 789 (629 distinct utterances) | the judge corpus the rule tier and the classifier were developed against |
| `intent-training/data/eval/holdout.jsonl` | 185 | fresh cases written without reading any rule tier's vocabulary, used to choose an operating point |
| `fixtures/eval/sealed.jsonl` | 2,482 | the sealed held-out set: judges whichever decider wins, read once per frozen decider |
| `fixtures/eval/sealed2.jsonl` | 4,156 | SEALED-2: 3,311 near-miss negatives, 700 positives, 145 ambiguous utterances by three authors who never saw the training data; certifies the 0.1% wrong-ACT ceiling that 363 near-misses cannot |
| `intent-training/build/laya/selection.jsonl` | 7,523 | the held-out selection set of the round-1 corpus (the group-disjoint test split), where the policy and the calibration are fitted |
| `fixtures/eval/slots.jsonl` | 100 | utterance, tool, reference clock; expected arguments or the slot that must be reported missing |
| `fixtures/eval/conversation.jsonl` | 127 | a turn's facts (done, listing, empty, failed, refused, module-off offer, destructive preview, clarifying question, plain chat, undecided action) and what the reply must and must not say |

`cases.jsonl` is authored in `intent-training/data/eval/` and copied here by
`intent-training/scripts/publish.py`; do not edit it in place. Its groups are memory 261, camera 40,
none 129, productivity 123, modules 50, app 34 and inactive 152 (the same requests with their module
off); the language variants are neutral Spanish 320, English 239, Peruvian-register Spanish 131 and
STT-style Spanish 99. The Peruvian rows are written by the assistant in a Peruvian register
(gasfitero, cochera, chibolo, "ya pues", yapear, Sedapal); no Peruvian speaker has reviewed them, so
they stress vocabulary, not usage.

The sealed set holds 352 positives (calendar 68, task 52, project 27, modules 67, reminder list 14,
memory 81, app 30, camera 13; Spanish, Peruvian Spanish, STT-noise Spanish and English) and 2,130
negatives: 363 authored near-misses and plain talk (`ayer tuve una reunión`, `la tarea de mi hijo`,
agenda the noun, `programa` the television show, plans in the future tense, other devices, 100 of
them rendered with STT noise) and 1,767 real utterances never trained on (MASSIVE, Multi3NLU++,
MINDS-14, FLEURS, Tatoeba; CC-BY-4.0 and CC-BY-2.0-FR, named per case in `source`) from which every
module, schedule and memory cue was filtered. Its sha256 is pinned in `gates.json` and the ctest
`eval-sealed-hash` fails the day a byte changes; `intent-training` refuses to build a corpus when it
differs and drops from training every row that is within Jaccard 0.65 or token containment 0.8 of a
sealed text (`scripts/filter_sealed.py` applies the same test to any file). It is not fully independent of the judge corpus a rule tier was tuned on: 22 of its 352 positives
and 16 of its 363 authored near-misses are within Jaccard 0.65 of a `cases.jsonl` text (89 and 37 within
0.5), the exact duplicates having been dropped when it was written. Nobody who tunes a decider
reads it: the harness reports numbers, never failing utterances, and the operating point is chosen
on the selection sets, not on it.

## The decider

Request: `{seq, text, lang, role, tools}`, where `tools` are the tool names offered to that role (the
choice options; modules.enable, modules.disable and modules.open_purge_screen only to the Owner,
modules.request only to the others). Answer: `{seq, tool|null, confidence, runnerUp:{tool, confidence},
now}`: the best option and how sure the decider is, the next best, and (optionally) the probability that
the user asks to do it now. A policy turns an answer into one of three outcomes: ACT (confidence at or
above `act` and clearly ahead of the runner-up by `margin`), ASK (the middle band, or two options close
together: the system asks "¿Lo agendo para el jueves a las tres?" or "¿Lo agendo o lo guardo como
recordatorio?" and acts on a yes) and conversation. A write or destroy tool (everything outside
calendar.list_events, task.list, project.list, modules.list/explain, reminder.list, memory.recall,
app.open and app.show_camera; app.set_guard_mode changes the house's security state and is a write)
ACTs only when the second signal `now` also passes `nowMin`, otherwise it is an ASK. The policy
(`act`, `ask`, `margin`, `nowMin`) is chosen on the selection sets as the one with the most coverage
(a correct ACT, or an ASK that names the right tool, over the clear commands) such that the wrong-ACT rate
on the negatives is at or below `decider.wrongActMax` (0.1%) pooled, per family and on the authored
near-miss stratum, at most `askRateClearMax` (10%) of the clear commands are asked about and at most 1%
are acted on with the wrong tool. Ambiguous cases (`expect.ambiguous.tools`) are scored apart: asking is
right, acting is wrong. `--final` then reads the sealed set and SEALED-2, each pinned by sha256, at that
policy and prints each sealed sweep as information only; `--errors` writes the selection-set errors
(never the sealed ones) for `intent-training/scripts/error_analysis.py`.

What is counted, per family (calendar, task, project, modules, reminder list, memory, app, camera):
*coverage*, *precision* of the ACTs and the *wrong-ACT rate*: a negative, or a positive of another family,
acted on with that family's tools. A pooled rate over easy real utterances is diluted, which is why the
authored near-miss stratum is gated on its own: a router that sends 2.3% of the near-misses to a
calendar tool still shows 0.49% pooled. 0.1% cannot be certified on 363 near-misses (zero errors still
leaves a Wilson upper bound of about 1%); 3,311 of SEALED-2 give 0.12%.

Baseline: the fastText classifier with twenty classes (the six of the memory router plus fourteen
for the module families, `intent-training` `CONTEXT.md`), adapter `scripts/decider_fasttext.py`,
margin 0.10.

| reading | model sha256 | threshold | module-family coverage | precision | pooled false-route | near-miss stratum |
|---|---|---|---|---|---|---|
| first (trained on a corpus with 1,247 containment near-duplicates of the sealed set) | d8c93ec5... | 0.93 | 0.456 | 0.852 | 0.665% | 2.875% |
| clean (0 near-duplicates; same hyperparameters) | 44bd49cf... | 0.93 | 0.456 | 0.874 | 0.488% | 2.259% |

The clean reading is the bar: per family (coverage / precision / false-route) calendar 0.500 / 0.756
/ 0.456% (near-miss 1.700%), task 0.423 / 0.957 / 0.041%, project 0.630 / 0.944 / 0.041%, modules
0.403 / 0.931 / 0.083%, reminder list 0.286 / 1.000 / 0.000%; memory coverage 0.556, precision 0.900,
false action 0.167%; the real stratum has no false route at all. On the sealed sweep the near-miss
stratum is at or under 0.5% only at confidence 0.99 and above, where coverage is 0.180. The model
fails the gate and is not published: the false routes are near-miss sentences that use the family
nouns in the past tense or as plain nouns, and calendar is where nearly all of them land. The
sealed set has been read twice for fastText; any later number of it on the sealed set is a tuned
number and is labelled so.

The coverage bar is 0.456, fastText at the sealed point where it broke the wrong-ACT gate; a pass is
under the gate AND above the bar. Beside it every report prints what a fastText that obeys the gate
reaches: 0.154 on the selection set at the 0.1% ceiling (calibrated, `ACT >= 0.91 ASK >= 0.85`, 8,807
non-positives, upper 95% bound 0.10%) and 0.101 on the sealed set (the figure of the first sealed
reading, at confidence >= 0.995 under the 0.5% false-route gate that held then, so it is a tuned number and not the same
ceiling as the selection figure). `gates.json` carries both under `decider.gateObeyingFastText`, and
`decider-eval.py` and `round-report.py` print them beside the coverage they report.

On real traffic (turns that came from the product or lab sessions) at the selected threshold the
clean model sends 44.3% of the 61 production turns to a tool and leaves 55.7% to plain conversation
(memory coverage 43.1%); 44.6% of the 92 lab-session turns and 61.2% of the 103 check turns reach a
tool. With the LLM no longer a second chance, that share is the number to move: a turn nothing
decides is conversation, never a guess.

The classifier's own split (`intent-training`, 5,649 test rows): the Wilson 95% lower bounds of
precision are 0.873 for `memory_save`, 0.840 for `memory_recall`, 0.908 for `reminder_set`, 0.839 for
`memory_forget` and 0.831 for `camera`, and the pooled precision of the fourteen family classes is
0.956 (lower bound 0.936); held-out `none` rows fire a module family 0.170% of the time (ceiling
0.5%) and `memory_save` 0.089% (ceiling 0.25%). Its gates sit within the noise of a split of that
size: three of them miss on this build (`memory_recall` by 0.010 and `camera` by 0.019 on the lower
bound, `none` recall on the case judge 0.968 against 0.97), which is why the decision to publish is
taken on the sealed reading and not on them.

### Round 0 of the fine-tuned model, on the selection sets only

Laya (`jhu-clsp/mmBERT-base` fine-tuned on 5,978 folded rows, 2 epochs) against fastText families-v1.1 on
the same selection set (the judge corpus, the fresh holdout and 2,500 real negatives; the sealed sets
untouched), module families pooled, ACT only:

| ACT at | decider | coverage | precision | wrong tool | wrong ACT | near-miss stratum |
|---|---|---|---|---|---|---|
| 0.90 | Laya | 0.849 | 0.869 | 1.59% | 1.005% | 3.09% |
| 0.90 | fastText | 0.669 | 0.966 | 1.99% | 0.100% | 0.617% |
| 0.93 | Laya | 0.653 | 0.906 | 0.80% | 0.502% | 2.06% |
| 0.93 | fastText | 0.622 | 0.969 | 1.59% | 0.067% | 0.412% |
| 0.94 | Laya | 0.167 | 0.933 | 0.40% | 0.067% | 0.206% |

Laya's confidence is compressed (it stops near 0.945 and falls off a cliff above it), so thresholds must
be fitted by calibration before two deciders are compared at equal wrong-ACT; no non-trivial ACT / ASK
policy meets the 0.1% ceiling on that selection set for either decider. The production router today
(`appCommandFor` + the six-class memory router, `tests/eval/production-decider`) routes no module family and
sends 3.98% of module-family requests to a memory tool.

## Performance and consumption

`perf-eval.py` measures latency per decision (p50, p95, max), resident and peak memory of the process
tree, threads, CPU milliseconds per call, load time and artifact size, sequentially, and refuses to
record a number when `/proc/stat` shows the machine busy (exit 77; `--allow-busy` measures anyway and
marks it not reportable). `gates.json` (`performance.metrics`) holds the budget: p95 150 ms, resident
600 MB, CPU 400 ms per call after INT8 with 4 threads.

| decider | p50 / p95 ms | CPU ms per call | resident / peak MB | threads | artifact MB |
|---|---|---|---|---|---|
| fastText router, C++ dev build (today) | 0.28 / 0.39 | 0.27 | 54.8 / 54.8 | 1 | 12.3 |
| fastText 20 classes, Python adapter | 0.06 / 0.14 | 0.03 | 52.3 / 52.3 | 1 | 12.5 |
| Laya round 0, PyTorch fp32, CPU | 91.7 / 101.4 | 740.6 | 1,733 / 2,324 | 39 | 1,294 (615 weights) |

Laya passes the latency budget and fails the memory budget by 2.9 times in the form nobody ships. The plan
for the shipped form (vocabulary pruned to the Spanish and English tokens, ONNX, INT8, 4 threads) is
measured before it is claimed.

## The slots

`slot-eval.py` scores the layer that turns an utterance into arguments, against a fixed clock
(Tuesday 2026-10-06 12:30): the start time to the minute, the title or name (it must hold the words
of the object and none of the trigger or time words), a due date, a day range, the module id, and
twelve cases where a required slot is absent. Gates: a guessed slot 0, an unneeded clarifying
question 0.05, time, title, name and range 0.90, module 0.95, missing-slot detection 0.90.

## The conversation

`conversation-eval.py` gives the LLM the turn's facts and scores what it says with no judge model:
*false completion* (it claims to have done something the turn did not do; a read-only success does
not legitimise a write claim; gated at 0 overall and per variant), the facts it must mention,
forbidden invention, digits and names that are in no fact and not in the user's words (ungrounded),
reply language, voice length, and a question when the case needs one (an offer, a preview, a
clarifying question). The undecided-action cases are the trap: the user asked for something, no
decider decided, and the reply must neither claim it nor pretend. Gates: mentions 0.90, ungrounded
0.05, language 0.98, length 0.95, question 0.90. The marker lists for a completion claim are the ones
the old tool-loop metric used (`gates.json`, `llm.scoring`).

## History: the LLM as a tool chooser

Measured before the decision above, at the committed tool loop (4dcc876a, 789 cases, Release), and
the reason for it: with the fast tier removed the model called `memory.remember` in 2 of 79 cases and
the other three memory tools in none; it never called a calendar, task, project or module tool (28 of
28 requests with a policy sentence in its prompt: 0 calls); 13.2% of its turns claimed an action
nothing had done (22.7% of the productivity requests); and with a module off it never made the offer
(0 of 146). The router before this wave sent 27.7% of the negatives to a memory tool, which the new
arbitration and the retrained classifier took to 1.4% (productivity requests routed to a memory tool
52.6% to 0.9%). `llm-tier-eval` and its stub tools still drive that loop and are retired with it.

## Speech recogniser

`services/stt/tests/eval/stt-wer-eval.cc` decodes 216 clips with the deployed engine
(`nemo_transducer`, `es`): Common Voice es test split (CC0-1.0, fourteen accent groups plus
unlabelled, 136 clips) and OpenSLR 73 Peruvian Spanish (CC-BY-SA-4.0, 80 clips). Word error rate
5.80% (4.83% with accents folded), character error rate 1.77%, 0.25 of real time. Per set: Common
Voice 5.98%, Peruvian 5.50%. `services/stt/CONTEXT.md` has the table and the caveats; the model is
unchanged.

## What these numbers do not say

- The judges are short, written utterances. Nothing here is a recording of a household through a
  far-field microphone, so the fast tier's recall on speech and the STT error rate on speech in a
  room are unmeasured.
- Peruvian coverage is lexical and written by the assistant; the speech set is read sentences.
- `memory_recall` has the thinnest memory data (about 1,200 training rows); its real-traffic recall
  on the production judge is 0 of 14, every one left to conversation.
- The module-family classes have no public data beyond MASSIVE's calendar rows; tasks, projects,
  modules and reminder list are authored by one author, who also wrote the judge cases and the sealed
  set (without reading any rule tier's vocabulary, but in one style).
- The sealed set holds 363 authored near-misses, so a false-route rate on that stratum has a Wilson
  upper bound of about 1% even at zero errors; the pooled 0.5% ceiling cannot be certified by it
  alone.
- The held-out negatives come from datasets built for other assistants. The labels were audited
  against the taxonomy after a model fired on them, so the held-out rates are slightly optimistic.

## Next steps

1. Record a few hours of household speech through the real microphone path, transcribe it with the
   deployed STT and label it; it replaces every proxy above.
2. Have Peruvian speakers review and extend the `pe` rows.
3. More `memory_recall` data: questions about what was told, in the three variants.
4. Score the pipeline's own stages (decide, slots, speak) through the three harnesses the day they
   land, and the module-command rule tier on the sealed set once it is behind the decider protocol.
5. Score a fine-tuned multilingual decider on the same sealed set; it has to beat coverage 0.456 and
   precision 0.874 with pooled and near-miss false-route rates at or under 0.5%.
6. Measure turn latency (decide, slots, execute, speak) end to end on an idle machine through the
   helpers, before and after.
