# Argus — Detailed plan to reduce tool-calling dependency

**Recipient:** Claude, implementation agent  
**Project:** Argus backend  
**Path:** `/home/acme/Desktop/argus/backend`  
**Date:** 2026-08-20  
**Status:** work plan; validate the current tree before continuing

## 1. Objective

Improve Argus so that the conversational model does not need to emit tool
calls to answer, save memory, retrieve memory or trigger simple actions.
The tools infrastructure must not be removed entirely: it must remain
available for tests, administration and explicit actions that really need
a structured contract.

The final design must work with small models, without native tool-calling
capability and on computers with few resources. The evaluation must measure
not only flexibility, but also precision, speed, RAM, ease of debugging
and behavior when the model changes.

The initial proposal is a hypothesis, not an irreversible decision:

    deterministic rules
      + fastText as a cheap hint
      + background extraction
      + deterministic recall
      + one normal LLM generation
      + tools only on explicit routes

Claude must compare this proposal with rules only, a separate LLM router,
a dedicated structured extractor and tool-calling from the main LLM.

## 2. Mandatory instructions for Claude

### 2.1 Read instructions before generating code

Before creating or editing any file, Claude must read completely:

1. `/home/acme/Desktop/argus/backend/AGENTS.md`.
2. `CONTEXT.md`.
3. This file, `CLAUDE_PLAN.md`.
4. The frontend `AGENTS.md` and `CONTEXT.md` if a change affects HTTP,
   WebSocket, DTO, permissions or synchronization contracts.

It is not enough to read the instruction block received in the prompt: it
must confirm the contents of the local file before modifying code.

### 2.2 Do not fill the code with comments

Do not add long, narrative comments that repeat what the code already
expresses. Prefer clear names, small functions and explicit structures.

A new comment is only justified if it documents a non-obvious invariant, a
limitation of an external library, a concurrency condition or a security
decision. It must be short, specific and close to the relevant line.

Do not turn every change into a block of explanation inside the `.cc` or
`.hxx`. Extensive explanations go in `CONTEXT.md` or in this plan.

### 2.3 Protect existing changes

The tree may contain legitimate user modifications. Before touching it:

    git status --short
    git diff --stat

Do not run:

- `git reset --hard`;
- `git checkout --`;
- broad recursive deletions;
- cleaning of models, databases or logs without authorization;
- massive reformatting of unrelated files.

Use `apply_patch`, review the diff after each group of changes and separate
the changes of this plan from pre-existing changes.

### 2.4 Take care of the computer's resources

Before a full compilation, a voice test or an inference:

    pgrep -af 'ollama|argus-llm-bench|voice-test|llama|cmake --build' || true

If there is other heavy work active, do not start another load without
warning. Searches, inspections and small tests can still be run.

## 3. Known project state

### 3.1 Reference model

The current reference model is LFM2.5 1.2B Instruct. In the last Ollama
battery it obtained approximately:

| Case | tok/s |
|---|---|
| short | 48.17 |
| medium | 46.22 |
| long | 43.40 |
| very long | 40.09 |
| extreme | 35.08 |
| memory battery | 48.45 |

The memory battery obtained recall 8/8, attribution 8/8, no invention 2/2
and 0/10 responses with visible thinking.

Recent results of alternatives:

| Model | Memory speed | Recall | Attribution | No invention |
|---|---:|---:|---:|---:|
| LFM2.5 1.2B | 48.45 tok/s | 8/8 | 8/8 | 2/2 |
| Granite 3.3 2B | 21.61 tok/s | 8/8 | 8/8 | 2/2 |
| Granite 4.0 H-1B | 31.32 tok/s | 8/8 | 8/8 | 2/2 |
| Granite 4.0 1B | 33.83 tok/s | 7/8 | 8/8 | 2/2 |
| Granite 4.0 Micro 3B | 17.79 tok/s | 6/8 | 8/8 | 2/2 |

Granite H-Micro 3B was downloaded, but it was not evaluated. Do not reopen
the model search in this plan unless the user explicitly requests it.

### 3.2 Execution paths

`labs/voice-test/voice-test.cc` is the main laboratory for voice, memory,
camera, STT, LLM and TTS. Its current flow is:

    input
      -> optional IntentService
      -> optional camera
      -> MemoryService::captureExplicit
      -> MemoryService::captureImplicit if fastText authorizes it
      -> GraphRecall
      -> LlmService::chatStream
      -> TTS in the voice modes
      -> history and compaction

`src/feature/socket/sync/services/voice-session-service.cc` is the real voice
WebSocket path. It currently uses VAD, STT, `voiceLlm()` and TTS, but does not
use `MemoryService` or `ConversationService`. Do not assume that a lab change
has already modified the production backend.

`src/shared/services/conversation/` contains the general conversation
abstraction. It originally enumerated `ToolRegistry`, built descriptors and
called `LfmAdapter::chatWithTools()`. The normal path must move to a single
generation of `LlmService::chat()` or `chatStream()`.

### 3.3 Memory

`MemoryService` coordinates `SqliteGraph`, `MemoryFormation`, `RuleParser`,
`TieredExtractor`, `EmbeddingService`, `GraphRecall`, the worker and
compaction. The relevant boundaries are:

- `captureExplicit`: trigger or statement recognized by rules;
- `captureImplicit`: candidate approved by a lightweight caller;
- `captureToolCall`: compatibility with explicit tools;
- `enqueueSummary` and `enqueueCompaction`: background episodes.

`RuleParser` already recognizes triggers, statements, interrogatives, recall
markers, fillers and confirmations in the static Spanish/English vocabulary.

### 3.4 Intent and data

`IntentService` uses fastText for `camera` and `memory_save`. The corpus has
approximately 1406 camera examples, 351 memory examples and 5161 `none`
examples.

`labs/intent-data/usage.tsv` is historical telemetry. It is not ground truth.
It already contains false positives such as questions, recalls and
cancellations labeled as `memory_save`. Never merge it automatically with
`train.tsv` again.

### 3.5 Existing tools

`ToolRegistry`, `ToolExecutor`, `LfmAdapter`, `ToolParser` and
`MemoryService::registerTools()` must be preserved for `labs/tool-bench`,
`labs/memory-probe`, explicit automations and compatibility tests. The fact
that normal dialogue does not use tools does not justify deleting those
components.

## 4. Cause of false saves

The problem of an input such as “Argus, cuándo es 2 x 2” has several layers:

1. fastText can trigger `memory_save` due to an imperfect classification.
2. `captureImplicit()` historically queued any text received.
3. `voice-test` printed “queued by intent” even though later formation could
   reject the text.
4. `usage.tsv` recorded predictions and could later be used for training as
   if they were manual labels.
5. `enqueueSummary()` could receive questions and rely only on another LLM
   omitting them.

The fix must separate:

    prediction -> policy -> persistence -> telemetry -> training

A prediction is not a policy and an accepted policy is not automatically a
perfect training example.

## 5. Comparison of alternatives

### 5.1 Rules only

Flow:

    RuleParser -> save or reject -> normal LLM

It is the fastest and most explainable option. It has high precision on
“recuerda que”, “anota que” and phrases covered by the vocabulary, but lower
recall for unforeseen natural statements.

It must always exist as a fallback when fastText or an extractor are not
available.

### 5.2 Rules + fastText + idle extractor

Flow:

    RuleParser asks
      -> fastText as a hint
      -> MemoryService decides
      -> lexicon/model extractor in worker
      -> conversational LLM without tools

It is the provisionally recommended option because it preserves low latency
and allows implicit statements. It requires a clean corpus, a deterministic
barrier against questions and a queue that does not block voice.

### 5.3 Small LLM router

A separate model can classify `conversation`, `memory_save`, `camera` or
`recall`. It can improve ambiguous cases, but it introduces another inference
per turn, another RAM/CPU dependency and another source of errors. Only test
it if fastText + rules do not reach acceptable precision with real data.

### 5.4 Tool-calling from the main LLM

It is flexible for complex actions, but it adds descriptors, tokens, parsing,
variability and possibly several generations. It must not be a requirement of
normal dialogue. Keep it isolated for routes that really need it.

### 5.5 Dedicated extractor off-turn

It allows separating memory formation and conversation. It can be very
precise, but it increases consumption and makes memory appear with delay. It
must be optional, cancelable and lower priority than the voice response.

### 5.6 Provisional decision

Implement alternative 5.2 first, measure it and compare it against 5.1. Do
not add an LLM router until it is demonstrated that fastText and the rules
are not sufficient.

## 6. Target architecture

### 6.1 Normal flow

    STT or text
      -> normalization and language
      -> captureExplicit
      -> isQuestion for the implicit route
      -> fastText as a hint
      -> formation queue if it is a valid candidate
      -> deterministic recall
      -> one normal LLM generation
      -> TTS or HTTP/WS response

### 6.2 Invariants

1. A question is not saved as implicit memory.
2. An intent does not write directly to SQLite.
3. `Rejected` means that nothing was queued or persisted.
4. `Deferred` means that valid work exists in the queue.
5. `Stored` means that a fact is persisted.
6. The conversational LLM does not receive tool descriptors on the normal
   route.
7. The LLM must not produce JSON, tags or blocks to save memory.
8. A memory is only confirmed if the backend confirms the capture.
9. Background extraction must not block the first voice response.
10. Sensitive actions validate permissions outside the LLM.
11. `ToolRegistry` keeps its `role_access` validations.
12. Probes can still exercise isolated tool-calling.

## 7. Partial changes present in the tree

There is a partial patch that Claude must review and compile before extending
it.

### 7.1 `MemoryService::captureImplicit`

A user check, empty text check and `RuleParser::isQuestion()` were added before
queuing. Verify that it:

- rejects “¿cuánto es 2 x 2?”;
- rejects “Argus, cuando es 2 x 2” without a question mark;
- rejects “¿cuándo viene mi hermana?”;
- allows `captureExplicit()` to handle explicit triggers;
- does not break legitimate implicit statements;
- does not report `Deferred` if the input was rejected.

### 7.2 `voice-test`

Active `ToolParser` processing was removed from the text, camera and voice
loops. Tokens are handled as a normal response. `ToolParser` must still
compile for the probes that need it.

The `memory_save` log must occur after `captureImplicit()` accepts, not
immediately after the fastText prediction.

### 7.3 `ConversationService`

The normal path was changed to explicit capture, recall, profile and a normal
call to `LlmService`. `ConversationTurnInput` was introduced to avoid keeping
tool-calling parameters that are no longer necessary on that route.

Claude must search for all call sites before changing the signature again.

## 8. Implementation phases

### Phase 0 — Baseline

1. Read `AGENTS.md`, `CONTEXT.md` and this plan.
2. Review status and diff.
3. Search for call sites of `processTurn()` and `chatWithTools()`.
4. Confirm that there are no active inferences.
5. Compile or do a focused check to detect errors from the partial patch.

Output: route map and list of errors without mixing unrelated changes.

### Phase 1 — Capture contract

1. Keep `captureExplicit()` as the first route.
2. Reject in `captureImplicit()` invalid user, empty text, questions, recall
   markers, cancellations and forgettings.
3. Keep formation as a second validation, never as the only filter.
4. Adjust `voice-test` messages so they reflect the real `CaptureOutcome`.
5. Add tests for Spanish and English.

Minimum negative cases:

    ¿cuánto es 2 x 2?
    Argus, cuando es 2 x 2
    ¿cuándo viene mi hermana?
    ¿qué me gusta tomar?
    qué no le gusta a Rodrigo
    no, olvídalo

Minimum positive cases:

    recuerda que mi hermana viene los domingos
    anota que llegó el paquete
    ten en cuenta que soy alérgico a los frutos secos
    mi perro se llama Toby

### Phase 2 — Safe compaction

1. Filter questions from `user:` lines before `enqueueSummary()` and
   `enqueueCompaction()`.
2. Remove the immediately associated `assistant:` response when the whole
   turn is a transient query.
3. Do not enqueue if no useful content remains.
4. Keep the compaction prompt as a secondary defense.
5. Test that a session with only greetings, questions and calculations does
   not create an episode.
6. Test that a durable statement does appear in the summary when appropriate.

### Phase 3 — fastText data

1. Keep `usage.tsv` as telemetry.
2. Do not merge it automatically with `train.tsv`.
3. Create a curated data flow, for example `usage-curated.tsv`, or a flag
   that requires explicit review.
4. If historical samples are kept, discard questions, recalls, cancellations
   and ambiguous phrases.
5. Show counts of accepted and rejected lines.
6. Add negative cases for calculation, time, date, weather, recall and small
   talk.
7. Measure precision, recall and false positives over questions.
8. Do not lower the threshold just to make a small corpus pass.

Precision must weigh more than recall for `memory_save`: a false positive
contaminates future facts and a false negative can be corrected with an
explicit trigger.

### Phase 4 — Prompt without tools

Add to the prompt, in Spanish and English, brief instructions equivalent to:

    Questions, calculations, dates, times, definitions, jokes and queries
    are ephemeral conversation; answer them and do not treat them as memory.

    Memory is managed outside the model. Do not emit tool calls, JSON,
    special tags or save blocks.

    Only confirm that something was saved if the system added an accepted
    capture note.

Do not fill the prompt with repeated instructions. Measure the prefix size
and preserve KV cache reuse.

### Phase 5 — ConversationService

The order must be:

    validate text
    captureExplicit
    recallBlock
    profileFor
    build the user message
    LlmService::chat or chatStream
    update history
    trimHistory

It must not enumerate tools, build `ToolDescriptor`, call `chatWithTools`,
run `ToolExecutor` by model decision or use `maxToolHops`.

External tools remain available for probes and explicit routes.

### Phase 6 — Worker priority

Audit `deferCapture()`, `enqueueJob()` and `processExtract()`. Implicit
capture that requires an LLM extractor must be markable as idle work.

Objective:

- answer first;
- form memory later;
- re-enqueue if `LlmService::isBusy()`;
- prevent the extractor from becoming a second blocking generation;
- keep deterministic explicit memory on the highest-priority route.

Do not change the policy without measuring latency and reviewing the
concurrency of the shared `LlmService` context.

### Phase 7 — Production VoiceSessionService

Do not copy lab globals. Before integrating:

1. determine how to obtain `MemoryService` by injection;
2. add `WorkingMemory` per session if appropriate;
3. use the authenticated `userId`;
4. resolve language per session;
5. define the accepted-capture notification;
6. define cancellation on WebSocket close;
7. ensure that STT, LLM, TTS and memory respect their mutexes/slots;
8. add WebSocket-specific tests.

## 9. Prompt and expected behavior

For the input “Argus, cuando es 2 x 2” the correct result is:

1. `captureExplicit()` does not find an explicit memory.
2. `captureImplicit()` rejects because it is a question.
3. No extraction is queued.
4. No fact or episode is written.
5. The LLM answers the question normally.
6. TTS only receives the spoken response.

For “recuerda que mi hermana viene los domingos”:

1. `captureExplicit()` saves or queues a valid capture.
2. The message contains a capture note only if the result allows it.
3. The LLM briefly confirms without generating a tool call.
4. The subsequent recall retrieves the fact.

For “mi perro se llama Toby”:

1. fastText can act as a hint if the model is available.
2. The deterministic question does not trigger.
3. `MemoryService` decides whether to queue extraction.
4. The user response does not wait for another tool-calling cycle.

For “no, olvídalo”:

1. it is not saved as a fact;
2. it is not used as an automatic positive sample;
3. if an explicit forget operation exists, it must have a separate and
   validated route.

## 10. Tests

### 10.1 Static searches

Run:

    rg -n "chatWithTools|ToolParser|captureToolCall" \
      src/shared/services/conversation labs/voice-test

Expected:

- no `chatWithTools` on the normal path;
- no `ToolParser` in the active `voice-test` loops;
- `captureToolCall` only in compatibility, probes or tests.

### 10.2 Compilation

Run the focused target first if it exists. Then:

    cmake --preset dev
    cmake --build --preset dev -j 8

Requirement: 0 errors and 0 new warnings. Do not continue with heavy tests if
the compilation fails.

### 10.3 Intent

Run the existing `intent-probe` and review especially the cases with:

- `2 x 2`;
- `cuándo viene`;
- `qué me gusta`;
- `qué no le gusta`;
- `olvídalo`;
- `what time`;
- `tell me`.

None of them must trigger `memory_save` at the production threshold.

### 10.4 Memory

Test:

1. explicit statement stored;
2. question rejected before the queue;
3. implicit statement approved;
4. subsequent recall;
5. cancellation rejected;
6. compaction without questions as an episode;
7. compaction with a durable statement;
8. no invention when no recall exists.

### 10.5 Voice

Only run `voice-test` when the user authorizes it and the computer is not
busy. Verify that tags, JSON or internal text are not spoken.

### 10.6 Metrics

Compare under equivalent conditions:

| Metric | Before | After |
|---|---:|---:|
| TTFT without memory | measure | measure |
| TTFT with recall | measure | measure |
| generations per turn | measure | goal 1 |
| tool schema tokens | measure | goal 0 |
| false `memory_save` on questions | measure | goal 0 |
| implicit memory availability | measure | measure |

Do not compare a busy machine against an idle one and attribute the
difference to the model.

## 11. Documentation in `CONTEXT.md`

Add a dated section with:

1. LFM2.5 vs Granite table;
2. clarification that H-Micro was downloaded, not evaluated;
3. decision not to require tool-calling from normal dialogue;
4. separation between `RuleParser`, `IntentService`, `MemoryService`,
   `GraphRecall`, `LlmService` and `ToolRegistry`;
5. problem of `usage.tsv` as uncurated telemetry;
6. difference between `labs/voice-test` and the production
   `VoiceSessionService`;
7. before/after metrics;
8. any pending limitation.

## 12. Risks

### Loss of implicit statements

Mitigate with vocabulary, real examples, fastText as a hint and an explicit
trigger as a fallback.

### Voice blocked by the extractor

Mark implicit work as idle, re-enqueue if the LLM is busy and measure
first-response latency.

### Model output with tags

Do not include schemas, reinforce the prompt and use a sanitizer only to hide
artifacts; never interpret that sanitizer as executing an action.

### Public signature change

Search for call sites, adapt real consumers and compile immediately.

### Confusing episode with fact

Test tables and recall routes separately. Do not call a provisional summary
“saved”.

### Corpus contamination

Separate telemetry, curated samples and training corpus. Do not use
predictions as labels without review.

## 13. Acceptance criteria

- [ ] Claude read the local `AGENTS.md` before modifying code.
- [ ] No long comment blocks were added to the code.
- [ ] Normal dialogue does not call `LfmAdapter::chatWithTools()`.
- [ ] Normal dialogue does not announce tools to the LLM.
- [ ] `ToolRegistry` still works in probes and explicit routes.
- [ ] Questions do not enter `captureImplicit()`.
- [ ] “Argus, cuándo es 2 x 2” does not appear as a fact or episode.
- [ ] Recall questions do not trigger `memory_save`.
- [ ] “olvídalo” is not saved.
- [ ] `usage.tsv` does not receive raw predictions as training labels.
- [ ] Compaction filters questions.
- [ ] The prompt states that memory is managed outside the model.
- [ ] The normal route uses one generation.
- [ ] TTS does not speak internal protocols.
- [ ] False positives and latency were measured.
- [ ] `CONTEXT.md` documents the decision.
- [ ] The dev build finishes with 0 errors and 0 warnings.

## 14. Execution order

    1. Read AGENTS.md, CONTEXT.md and CLAUDE_PLAN.md.
    2. Review status and diff.
    3. Confirm heavy processes.
    4. Search for call sites and fix errors from the partial patch.
    5. Finish the captureImplicit contract.
    6. Finish the compaction filter.
    7. Separate usage.tsv from the curated corpus.
    8. Add reviewed negative and positive cases.
    9. Update prompts without tool-calling.
    10. Compile dev.
    11. Run focused tests.
    12. Run voice only with authorization.
    13. Measure latency and precision.
    14. Document in CONTEXT.md.
    15. Review the final diff and deliver results.

## 15. Claude's delivery

The final summary must include:

1. modified files;
2. files deliberately not modified;
3. chosen alternative and discarded alternatives;
4. tests run and results;
5. memory false positives and recall;
6. before/after latency;
7. pending limitations;
8. heavy tests that were not run and why;
9. confirmation that no models, databases or user changes were deleted;
10. confirmation that normal dialogue does not depend on tool-calling.

Do not declare the work finished just because it compiles. The behavior must
be demonstrated with questions, calculations, explicit statements, implicit
statements, recalls, cancellations and compaction.
