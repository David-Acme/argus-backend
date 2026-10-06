# Voice quality evaluation

What Argus understands when somebody speaks to it is measured, not reviewed. Three things are
measured, each against numbers kept in the repository: the fast tier (the fastText classifier and
the rules that decide a turn without the LLM), the LLM tier (which tool the model picks, with
which arguments, and what it answers when a module is off) and the speech recogniser. A change
that makes a gated number worse fails the test that carries it.

Measured on 2026-10-06 on the development machine (Ryzen 7 5725U, 16 threads, 32 GB, CPU only).

## How to run it

```
bash scripts/build-all.sh dev --only llm
ctest --test-dir services/llm/build/dev -L eval --output-on-failure
ctest --test-dir services/llm/build/prod -L eval-llm --output-on-failure

python3 scripts/stt-eval-data.py
ctest --test-dir services/stt/build/dev -R stt-wer-eval --output-on-failure
```

A test that has nothing to run (no intent model, no LLM weights, no speech clips) exits with code
77 and ctest reports it as skipped, so a missing model cannot pass for a good number. Reports
(JSON, every metric) are written beside each runner in its build directory. The classifier is
retrained and republished from the sibling `intent-training/` project (`CONTEXT.md` there).

## The judge corpus

`services/llm/tests/fixtures/eval/cases.jsonl`: 789 cases, 629 distinct first utterances. It is
authored in `intent-training/data/eval/` (training must exclude it, rows near it included) and
copied here by `intent-training/scripts/publish.py`; do not edit it in place.

| group | cases | what it holds |
|---|---|---|
| memory | 261 | a fact to keep, a question about what was told, a reminder, a retraction naming a fact |
| camera | 40 | looking at a camera or asking what is at the door |
| none | 129 | chit-chat, world knowledge, device control, a bare "olvídalo" |
| productivity | 123 | agenda, calendar, task and project requests and their cancellations |
| modules | 50 | list, explain, enable, request, disable, open the purge screen |
| app | 34 | open a screen, set the guard mode, list reminders |
| inactive | 152 | the same requests asked with their module off, as Owner and as Resident |

Language variants: neutral Spanish 320, English 239, Peruvian-register Spanish 131 and STT-style
Spanish 99 (lower case, no punctuation, fillers, "agenda me"). The Peruvian rows are written by the
assistant in a Peruvian register (gasfitero, cochera, chibolo, "ya pues", yapear, Sedapal); no
Peruvian speaker has reviewed them, so they stress vocabulary, not usage. Every case has a fast-tier
route (`memory_save`, `memory_recall`, `reminder_set`, `memory_forget`, `camera` or `none`) and the
tools the LLM must call, may call and must not call, with argument matchers that read any string
value (so a renamed argument does not break a case). A case may also expect a module-off answer,
a confirmation before a destructive tool, or the spoken yes after an offer. `eval-corpus-test`
checks the structure, including that every agenda, calendar, task and project request exists
labelled `none` in all four variants and again with productivity off.

Existing judges stay: `tests/fixtures/intent/eval-production.tsv` (61 real production utterances)
and `eval-check.tsv`, `tests/fixtures/memory/eval-usage.tsv` (real lab sessions).

## Fast tier

`fast-tier-eval` drives the production router (`IntentGate`: rules, fastText, the arbitration in
`services/llm/CONTEXT.md`) over the distinct utterances and checks `tests/eval/gates.json`
(section `fast`). Definitions: a turn is *routed* when the router is confident of one of the four
memory classes; every other turn *falls through* to the LLM. A *false action* is a routed turn whose
gold route is `none` or `camera`.

| | shipped model, old router | shipped model, new router | retrained model, new router |
|---|---|---|---|
| false-action rate, all negatives | 27.7% | 18.5% | 1.4% |
| productivity requests routed to a memory tool | 52.6% | 46.5% | 0.9% |
| camera asks routed to a memory tool | 17.5% | 2.5% | 2.5% |
| turns that fall through to the LLM | 56.9% | 66.1% | 72.7% |
| `memory_save` precision / recall | 0.54 / 0.49 | 0.82 / 0.51 | 0.90 / 0.57 |
| `memory_recall` precision / recall | 0.38 / 0.40 | 0.48 / 0.37 | 0.98 / 0.52 |
| `reminder_set` precision / recall | 0.49 / 0.63 | 0.64 / 0.76 | 0.98 / 0.81 |
| `memory_forget` precision / recall | 0.72 / 0.56 | 1.00 / 0.53 | 1.00 / 0.63 |

Where the wrong routes came from: with the old router the rule layer decided alone and was right
44% of the time for statements and 31% for recall markers, against 96% for the model; 47 of the 49
false actions of the first measurement were rules ("remind me to call my mom" was a recall,
"who's at the door right now" a saved fact). The share that falls through is the right number to
report next to the false-action rate: the fast tier now handles the clear cases and leaves 73% of
this deliberately hard set (51% of its utterances are module commands, chit-chat and camera asks that no memory tool should take) to the LLM. On the 61
production utterances 39% fall through and `memory_save` is routed with precision 0.97 and recall
0.81. Per variant, precision and recall: Spanish 0.95 / 0.59, English 0.92 / 0.67, Peruvian 1.00 /
0.58, STT-style 1.00 / 0.77.

The classifier itself (`intent-training`, 4,921 test rows, 22,813 held-out negatives): valid
precision@1 0.920; test Wilson 95% lower bound per class 0.865-0.908 (`memory_forget` lowest); `none`
recall 0.991; held-out `none` rows fire `memory_save` 0.123% of the time (gate 0.25%) and any tool
0.697% (gate 2%). The card that said 19.100% for the first of those printed a fraction as a percent
twice: the shipped model measured 19 of 9,934 = 0.191% and passed.

## LLM tier

`llm-tier-eval` (ctest labels `eval` and `eval-llm`, Release builds; a debug build decodes about
twenty times slower and exits 77) drives the production `LlmController::chatSync` with the
LFM2.5-1.2B QAD Q4_0 model, the real tool loop and executor, the router with the published intent
model, the production module catalog (`services/settings/modules.json`) and stub tools. The tool
specs are the committed ones: the memory and `app.open` descriptors from the code, the provider
tools mirrored by hand from their MCP servers in `eval-tools.cc` (they live in other services and
cannot be linked; a rename there must be copied here). Handlers record what ran and call nothing;
destructive tools answer with the same `ConfirmationLedger` two-phase protocol the providers use.
Temperature 0, one session per case, text-only history between the turns of a case as the voice wire
carries it. What it scores:

- per tool: precision and recall of the call, with argument matchers; false action (a successful call
  the case does not allow) and false write; selection accuracy by group and by variant;
- **false completion**: a reply that says something was created, saved, enabled, scheduled,
  cancelled, opened, or just "listo", "confirmado", "done", without a successful tool result of that
  kind in the turn. Offers, questions and negations are not claims, a claim after a successful tool is
  not counted, a failed or merely previewed tool is not a success, and a write claim after only
  `app.open` succeeded still counts. Gated at 0, overall and per variant;
- module off: the model attempted the inactive tool, called nothing else, answered in prose with the
  offer (turn it on for the Owner, ask the owner for anyone else) and never answered from
  `memory.recall`;
- destructive tools: nothing executed before a spoken yes, preview then execution after it;
- an offer that is accepted (`modules.enable` / `modules.request`) or declined.

Runs are saved as JSONL (`--runs-out`) and can be merged and rescored without a model (`--score a,b`),
so the full corpus runs in chunks (`--skip`, `--limit`) of at most fifteen minutes under
`heavy.sh 6`; `--no-fast-tier` removes the intent model and `--offer calendar.,memory.` narrows the
offered tools for experiments. Measured at the committed tool loop (4dcc876a), 789 cases, 809 turns,
Release, 4.7 s per turn on average and 13.9 s at the 95th percentile on a busy machine:

| tool | precision | recall | cases | note |
|---|---|---|---|---|
| memory.recall | 0.975 | 0.52 | 75 | |
| memory.remember | 0.90 | 0.57 | 79 | |
| memory.remind | 0.97 | 0.81 | 75 | |
| memory.forget | 1.00 | 0.66 | 32 | |
| app.open | 0.80 | 0.86 | 14 | |
| app.set_guard_mode | 1.00 | 0.85 | 13 | |
| modules.list | 0.50 | 0.10 | 10 | one call in the whole corpus |
| calendar.create_event, calendar.list_events | 0 | 0 | 32, 21 | never called |
| task.create, task.list, task.complete | 0 | 0 | 15, 11, 10 | never called |
| project.create, project.list | 0 | 0 | 9, 7 | never called |
| modules.explain, modules.enable, modules.request, modules.open_purge_screen | 0 | 0 | 8, 7, 4, 6 | never called |
| reminder.list | 0 | 0 | 7 | never called |

Argument accuracy is 190 of 190 when a tool is called. Selection accuracy is 59.1% (memory 63.6%,
camera 97.5%, none 98.5%, app 67.7%, modules 10.5%, productivity 0%); Spanish 56%, English 62%,
Peruvian 58%, STT-style 63%. False actions are rare (2.0% of cases, 1.1% writes), which makes the
fast-tier arbitration and the existing guards the reason the system does not write what it should
not; the failure is the opposite one.

- **The LLM alone almost never calls a tool.** With the fast tier removed (`--no-fast-tier`: no intent
  model, only explicit triggers and the deterministic app-command rules left) the 261 memory cases
  give `memory.remember` 2 calls of 79 (precision 1.0, recall 0.025), `memory.recall` 0 of 75,
  `memory.remind` 0 of 75 and `memory.forget` 0 of 32: selection accuracy 0.8%. Every memory
  tool call in the table above is the router's, and the app tools that work are the rule layer's
  (`appCommandFor`), not the model's. So all four memory classes deserve their fast route, none can be
  handed back to the LLM today, and the sentence in the project notes that the LLM keeps every turn the
  router abstains on is true of the architecture and untrue of the measured tool calling for the
  trigger-less utterances this corpus is made of.
- **False completion: 13.2% of turns** (neutral Spanish 17.8%, Peruvian 19.5%, STT-style 12.1%,
  English 4.1%); by group app 23.5%, productivity 22.7%, modules 21.8%, module-off cases 21.5%, none
  9.3%, camera 5.0%, memory 3.5%. Examples: "He agendado la cita..." with `app.open` as the only
  successful call; "Confirmado: reunión con Andrea el jueves" with no call; "He registrado que el
  colegio termina a las 4:10" with no `memory.remember`. The gate is red by design until it is zero.
- **Module off: 0 of 146** cases attempt the tool, offer to turn it on or ask the owner; the model
  never answers them from `memory.recall` (99.3%) and calls nothing else (95.9%), and it makes the
  same prose claims it makes with the module on.
- **Why the new tools are never called** was checked, not guessed: offering only `calendar.*` (and
  `memory.*`) the model still answers "Creo una reunión con Andrea para el jueves a las 3:00 PM.
  Confirmado." in prose, with no call and no dropped call in the adapter log, so it is neither tool
  overload nor the harness. The tool policy sentence in `llm-controller.cc` names only `memory.*` and
  `app.*`.
- Destructive confirmation: nothing is ever executed without a spoken yes (16 of 16), but the two-turn
  execution cannot be judged because `calendar.cancel_event` and `modules.disable` are never called.
  Whether the confirmation code reaches turn 2 through text-only history is open; the case is in the
  corpus and will be measured the day the model calls them.

Before this wave the whole system (shipped router, shipped model, seven tools, old rule layer) gave, on
the same utterances: `memory.remember` P 0.57 R 0.49, `memory.remind` P 0.52 R 0.63, `memory.recall` P
0.40 R 0.40, `memory.forget` P 0.72 R 0.56, a false action on 20.8% of the cases and an unwanted memory
write on 12.9% (31.6% of the agenda, calendar, task and project requests were saved as something they
were not). Those are 620 distinct utterances measured with the old release bench; the router and
classifier work removed that failure and the new tools reveal the next one.

The gates in `tests/eval/gates.json` (section `llm`) hold the measured numbers above for everything the
model does today and zero for false completion; the module tools are gated the day they are called.

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
- `memory_recall` has the thinnest data (about 1,200 training rows after calendar questions moved
  to `none`); its real-traffic recall on the production judge is 0 of 14, every one falling to the
  LLM.
- The held-out negatives come from datasets built for other assistants. The labels were audited
  against the taxonomy after a model fired on them, so the held-out rates are slightly optimistic.

## Next steps

1. Record a few hours of household speech through the real microphone path, transcribe it with the
   deployed STT and label it; it replaces every proxy above.
2. Have Peruvian speakers review and extend the `pe` rows.
3. More `memory_recall` data: questions about what was told, in the three variants.
4. Run the LLM tier on every model or prompt change; gates tighten when a number improves.
