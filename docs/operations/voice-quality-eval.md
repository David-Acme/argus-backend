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
    [--sealed services/llm/tests/fixtures/eval/sealed.jsonl --sealed2 services/llm/tests/fixtures/eval/sealed2.jsonl --sealed3 services/llm/tests/fixtures/eval/sealed3.jsonl --final]
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

A number that is reported goes through `scripts/measure-guard.sh`, which refuses to start the run
when the tree that built it has uncommitted changes or when the build predates `HEAD`, and prints
the key the number is filed under — `commit`, `build_type`, `profile` and `config`. `--profile`
defaults to `prod`, so a dev build is reachable only by passing `--profile dev` and the key says so.
A measurement is therefore always a commit, a build type and a configuration together, and two
numbers taken under different keys are never compared:

```
scripts/measure-guard.sh --build services/llm/build/dev --profile dev \
    --config 'gates.json --temperature 0.3 --seed 42 --render-acts' \
    -- services/llm/build/dev/tests/eval/call-faithfulness-eval <args>
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
| `fixtures/eval/sealed3.jsonl` | 2,689 | SEALED-3: 2,399 near-miss negatives (calendar 899, task, project and modules 1,000, reminders, memory, app and camera 500) and 290 positives by three fresh-context authors who never saw the training data, SEALED-2, the selection set or each other; sized so the pooled near-miss pool tolerates one wrong ACT at the 0.1% ceiling |
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
(a correct ACT, or an ASK that names the right tool, over the clear commands) such that the wrong-ACT
rate on the negatives, the point estimate and never an upper bound, is at or below
`decider.fitWrongActMax` (0.05%, half the gate) pooled, per family and on the authored near-miss
stratum, at most `askRateClearMax` (10%) of the clear commands are asked about and at most 1% are acted
on with the wrong tool. The gate, `decider.wrongActMax` (0.1%), is judged at the final read; the factor
of two between them is the room the sealed sets need for a policy that was chosen at the edge of what
the selection set allowed (a test keeps the fit ceiling at or below half the gate). A stratum with fewer
than `decider.minStratum` (600) negatives cannot pass by having no errors, it is unmeasured and no
policy is feasible while one exists: 600 is 3 / (10 x 0.05%), the size at which zero errors rule out, at
95% (the rule of three), a true wrong-ACT rate ten times the fit ceiling; `decider-eval-test.py` ties the
number to the ceiling. Every stratum of the round-1 selection set is at least 2,522 deep, so the floor
does not bind there.

The search walks every ACT from 0.50 to 0.99 in steps of 0.01 (and 0.995), and under each ACT every
ASK threshold of a short list below it plus the empty band (ASK equal to ACT), every margin and every
`nowMin` of 0, 0.5, 0.7, 0.8 and 0.9. The first version walked eight hand-picked ACT values
(0.7, 0.8, 0.9, 0.93 and up) and ASK values that were never equal to an ACT below 0.93; the calibrated
confidence of round 1 tops out at 0.92, so every ACT from 0.93 up fired nothing, 0.9 asked about too many
clear commands through its band, and the one feasible policy the grid held was the one that does nothing
(coverage 0.000 at every ceiling). The grid, not the ceilings, was the constraint that bound
(`--report` carries a `binding` table, and every round report prints it: the best policy with each
constraint lifted in turn, and with the second signal off). Ambiguous cases (`expect.ambiguous.tools`) are scored apart: asking is
right, acting is wrong. `--final` then reads the sealed set and SEALED-2, each pinned by sha256, at that
policy and prints each sealed sweep as information only; `--errors` writes the selection-set errors
(never the sealed ones) for `intent-training/scripts/error_analysis.py`.

What is counted, per family (calendar, task, project, modules, reminder list, memory, app, camera):
*coverage*, *precision* of the ACTs and the *wrong-ACT rate*: a negative, or a positive of another family,
acted on with that family's tools. A pooled rate over easy real utterances is diluted, which is why the
authored near-miss stratum is gated on its own: a router that sends 2.3% of the near-misses to a
calendar tool still shows 0.49% pooled.

Fitting is not certifying. The policy fit judges point rates on the selection set, so its answer does not
depend on how many near-misses the set holds. The Wilson upper bound is taken once, at the final read, over
the near-miss stratum pooled across the sealed set and SEALED-2 (`decider.certification` in `gates.json`:
`z` 1.96 and the two sets to pool), and `--final` fails when the pooled bound is above the ceiling or when a
set of the pool was not read. 0.1% cannot be certified on 363 near-misses (zero errors still leave an upper
bound of about 1%); 3,311 of SEALED-2 give 0.12%; the 3,674 near-miss negatives of both give 0.1045%, still
above 0.1%: at `z` 1.96 the ceiling needs 3,838 error-free near-misses (2,704 at a one-sided 95%, `z` 1.645),
and one error anywhere in the pool fails it. The harness pools the stratum it counts, which also holds the
authored positives of the families outside the modules, and prints the count it needs beside the count it
has. The pool is sized for one error: 6,000 pooled near-misses give an upper bound of 0.064% at zero
errors and 0.094% at one, 6,500 give 0.087% at one and 0.112% at two (`decider-eval-test.py` pins these
figures), which is why a third sealed set of 2,399 further near-misses, by authors who never saw
the training data, SEALED-2 or each other, joins the pool: 363 + 3,311 + 2,399 = 6,073 negatives give 0.063% at zero errors,
0.093% at one (passes) and 0.120% at two (fails).

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

## The call prompt (call-faithfulness)

`call-faithfulness-eval` measures what the spoken call prompt does on the shipped path, not on
a rehearsal of it: every case runs through `LlmController::chatSync`, so `LfmAdapter` decides the
clock note itself (`LfmAdapter::clockNote`, the one place `asksAboutTime` is consulted) and
inserts it as its own system note before the last user message, exactly as a live call does. The
same adapter path carries the tool-less turn (a role that holds no tool, a call without tools),
so the clock note and the offer strip are measured there too. The first request is assembled the
way `CallHistory::rebuildPrompt` assembles it (the prompt, then the framed known-header, then
each note on its own line), and the corpus has grown with it
(`services/llm/tests/fixtures/eval/call-faithfulness.jsonl`, 20 cases es+en, 22 turns).

Thirteen deterministic dimensions, no model as judge: `claims` (the product's own claim gate,
`claim-check`/`reply-claims`, applied to the returned text — a nonzero count means the shipped
pipeline let a claim through), `rawClaims` (the same gate applied to the model's text BEFORE
the gate substitutes its honest line — read from the adapter's own `ToolChatOutput::rawReply` /
`LlmChatOutcome::rawReply`, i.e. the exact generation this turn shipped, not a second one; this
is the dimension that measures the prompt itself),
`clockRestraint` (no weekday, month or clock reading on a turn that did not ask for one),
`recital` (no verbatim note text), `language`, `roleConfusion`, `genericOffer`, `parrot` and
`nameAskRepeated` (all pinned at zero), plus the soft bounds `sentences` (at most two),
`missedNameAsk`, `relevance` and `unpromptedGreeting`. `genericOffer` is the drifted measure —
it fires on a generic offer anywhere in the reply — while the strip only ever drops a sentence
that *is* the offer (`reply_claims::standaloneOffer`: after one leading interjection the sentence
must begin with an offer phrase and everything after it must be a conversational particle —
"más", "hoy", "ahora" / "else", "you", "today", "now", "further" — the offer phrases themselves
untouched), so a sentence that carries an offer behind a comma, or follows it with a content
word, is reported and never cut.

The original A/B that chose the rewrite, measured 2026-10-07 at 9 cases / 10 turns,
temperature 0.0, seed 42, debug build with `--force` — before the controller-side clock gate
existed:

| dimension | old prompt (HEAD) | shipped prompt |
|---|---|---|
| `clockRestraint` | 1 | 0 |
| `casePass` | 5/9 | 6/9 |
| `claims`, `rawClaims`, `recital`, `language`, `roleConfusion` | 0 | 0 |
| `sentences` | 3 | 3 |

The one violation the old prompt produced was the failure the rewrite was about: on an
unrelated turn it volunteered "Hoy es miércoles 7 de octubre de 2026 y son las 17:30" before
answering.

Re-run on the final 20-case corpus at the shipping temperature (0.3, five seeds), the
comparison inverts: the clock gate now protects any prompt — the old one's violation cannot
fire any more — and the pre-rewrite prompt still scores higher:

| seed | old prompt | shipped prompt |
|---|---|---|
| 42 | 17 | 16 |
| 7 | 17 | 17 |
| 1234 | 18 | 15 |
| 11 | 17 | 17 |
| 99 | 18 | 15 |
| **mean** | **17.40** | 16.00 |
| **min** | **17** | 15 |

Hard dimensions scored 0 on every run for both prompts; the old prompt's remaining misses are
sentence counts (two to three runs) and, on some seeds, the name ask, while the shipped one's
are the name ask and the greeting opener — the prompt items tracked as U19. The prompt question
was settled by this table: the body reverted to the old one (the eval, the strip and the clock
gate are unaffected and were re-pinned against the reverted prompt), and U19 starts from it.
A Spanish-body variant stays a candidate that needs its own five seeds.

The gates live under `callFaithfulness` in `gates.json`, pinned against the shipped
configuration at the shipping temperature with a fixed seed (`--temperature 0.3 --seed 42`,
reproducible across builds): `cases >= 20` and `turns >= 22` keep a shrunk corpus from passing
vacuously, every hard dimension is pinned at 0 (`claims`, `rawClaims`, `recital`,
`roleConfusion`, `clockRestraint`, `language`, `genericOffer`, `parrot`, `nameAskRepeated`),
and the soft dimensions are pinned at the pinned run's measured values (`missedNameAsk <= 0`,
`relevance <= 0`, `sentences <= 2`, `unpromptedGreeting <= 1` — the reverted prompt's cell).
The pin carries its own args (`callFaithfulness.pinnedArgs`: `temperature 0.3`, `seed 42`), so
the pin pins itself: a run whose `--temperature`/`--seed` differ from the recorded pair stops
with `[SKIPPED] gates pinned for different args` and exit 77 before the model is touched, and a
metrics pin without that pair is an error (exit 1). `call-faithfulness-pin-eval-test` checks all
three model-free. The ctest smoke uses its own small pin (`gates-smoke.json`, three cases, its
own `pinnedArgs`) so it stays a plumbing check: it carries only the zero-invariant dimensions
and the corpus floor for its own subset. The four soft dimensions are budgets of the measured
20-case corpus, and a three-case subset cannot meaningfully budget them — they are deliberately
omitted from the smoke rather than guessed from three cases.

Running it:

```
services/llm/build/dev/tests/eval/call-faithfulness-eval \
  --cases services/llm/tests/fixtures/eval/call-faithfulness.jsonl \
  --gates services/llm/tests/eval/gates.json \
  --llm-model models/llm/LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf \
  --prompt-es services/voice/tests/fixtures/call-prompt-es.txt \
  --prompt-en services/voice/tests/fixtures/call-prompt-en.txt \
  --report <path>
```

Add `--force` on a debug build and `--temperature 0.3 --seed 42` for the pinned invocation
(any other temperature or seed is a measurement, not the gate: the eval refuses it); it is a
heavy job (big lane, cap 3), and it skips 77 without the model or while the gates are unpinned.
The prompt text's
single source of truth stays `services/voice/src/feature/voice/call-history.cc`:
`call-prompt-*.txt` and `call-known-*.txt` are generated copies pinned byte-for-byte by the
voice test, and the eval derives the known fixture as a sibling of the prompt file, so a
missing fixture is an error, not a silent pass.
Two caveats: the sentence counter is punctuation-based, and the corpus' first request carries
the harness's own `person`/`roles` slots beside the prompt/known/notes skeleton. Three fidelity
deltas are known and accepted: the eval asks for
160 max tokens where the live call asks 256, it drives the non-streaming `chatSync` while the
live call streams, and it asserts nothing about whether the tool loop was taken.

## The call prompt's temperature (measured 2026-10-07)

The call prompt's temperature is a measured choice, not a feel: the shipped prompt (reply-claim
strip and clock gate included) was run through `call-faithfulness-eval` in the prod build at
0.3, 0.45 and 0.6, five seeds each (42, 7, 1234, 11, 99), 15 runs, on the 20-case / 22-turn
corpus. Every run passed the pinned gates, and the hard dimensions (`claims`, `rawClaims`,
`recital`, `roleConfusion`, `clockRestraint`, `language`, `parrot`, `genericOffer`) scored 0 on
every run at every temperature; the whole discrimination is `casePass`, paired by seed:

| seed | 0.3 | 0.45 | 0.6 |
|---|---|---|---|
| 42 | 16 | 13 | 14 |
| 7 | 17 | 15 | 16 |
| 1234 | 15 | 16 | 16 |
| 11 | 17 | 16 | 16 |
| 99 | 15 | 14 | 12 |
| **mean** | **16.00** | **14.80** | **14.80** |
| **min** | **15** | **13** | **12** |

0.3 takes the best mean and the best worst case and wins four of the five paired seeds, so the
default is 0.3, and the CI gate pins this loop's cell — `--temperature 0.3 --seed 42` in
`gates.json` — which reproduces exactly across rebuilds. Temperature 0 is not a gate: greedy
decoding produces text the strip does not meet (a mid-reply generic offer, deterministic on
the es-greeting cases), so its numbers are a diagnostic, not a product measure. The soft
misses that survive at every temperature are
not sampling noise: the same one name-ask case and two-to-three greeting cases miss at 0.3,
0.45 and 0.6 alike — prompt or corpus work, not a temperature, and tracked as its own item.

## The call context, gated per turn (measured 2026-10-08)

The call used to carry every app-state fact (the camera list, the guard mode, the agenda)
in the static prefix on every turn. The per-turn context selector (`argus-llm`'s
`turn::ContextSelector`) now keeps only the facts the turn's own decision names — the
decided tool's family, unioned with the facet words and phrases the utterance itself says
— and composes them into one framed tail note, marked as data and never instructions. The
call-faithfulness corpus gained the facet tagging and five cases (needed-fact-only and
misleading-block); the two arms are the same binary, `--legacy-context` putting the facts
back in the prefix, so the comparison is paired. All runs prod, `taskset -c 8-15`
(8 effective CPUs), `--temperature 0.3`, 25 cases / 27 turns.

| seed | gated | legacy |
|---|---|---|
| 42 | 24 | 22 |
| 7 | 23 | 21 |
| 1234 | 22 | 23 |
| 11 | 22 | 21 |
| 99 | 23 | 21 |
| **mean** | **22.80** | **21.60** |
| × 20/25 | **18.24** | **17.28** |

The legacy arm on the 20-case corpus reproduces the reverted pin exactly (17/20, all hard
dims 0). On the grown corpus the gated arm keeps every hard dim at 0 except `missedNameAsk`
on `es-name-unknown` (4 seeds gated, 3 legacy — the pre-existing U19 name-ask item) and
scores 0 for `claims`, `rawClaims`, `recital`, `language`, `roleConfusion`, `parrot`,
`missedFact` and `leakedFact` on every run. Prefill falls 7.4 % of the model's own
`decodedTokens` (10,089 vs 10,895 over 27 turns) and 8.6 % of the block sum, not the 30 %
target: the corpus's context blocks are only about 8 % of its prompt, and the production
context the target presumes (situation blob, recall block, tool results) is not in it.
Time to first token is flat (p50 727 vs 734 ms, one run). Ablation confirms each family is
used: dropping the camera facts costs a case, the agenda facts three, the guard facts one.
Details and the per-seed table: `docs/history/project-log.md` and the unit's report.

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
