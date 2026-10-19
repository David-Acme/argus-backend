# Parallel conversations: one conversation per person, on one server

Status: design note, 2026-10-14, step 1 of the parallel-conversations programme. It is taken from
`docs/history/plans/parallel-conversations-plan.md` (the owner's ruling of 2026-10-12 is section 8 of
that plan). Every decision below is stated as a decision and says which plan section it comes from.
Open questions are in section 8, numbered. Revision 2 (2026-10-14) applies the rulings of backend-c6
on the note: every question is now ruled, Q3, Q10 and Q14 changed from the proposal, and section 8 records
each ruling. Nothing in this note is built by the note itself; the code units that follow it are listed
in section 3.

## 0. Scope, sources and what is out

In scope: the phase-1 order of plan section 8 (P1 and P2 measurements, the conversation store and the
switch note, parallel LLM sequences, fair STT and TTS, memory scopes). The messenger (plan section 6, F8)
is out of this note except for the shared points named in section 6, which are owned by the messenger
note on `track/b4-messenger`.

Out: the recorder (on hold by the owner); Laya and GLiNER (the end of the programme); co-located
handling and self-echo of plan section 4 beyond the decisions recorded here (it is taken with the
camera watchman); personal VAD and target-speaker extraction (no deployable open model, plan section 4).

Sources read at master `cc798138`, branch `track/b4-conversations`:

| area | what was read |
|---|---|
| plan | `docs/history/plans/parallel-conversations-plan.md`, sections 0 to 9 in full |
| LLM engine | `packages/clients/llm/src/llm/llm-service.hxx` (`LlmService`, held by value in the controller), `services/llm/src/feature/llm/services/llm-service.cc` (context setup, prefix checkpoints, the generate lock), `services/llm/src/feature/llm/services/lfm-adapter.hxx`, `services/llm/src/feature/llm/controllers/llm-controller.{hxx,cc}` (caller binding, `boundToCaller`) |
| LLM wire | `packages/contracts/llm/llm.proto` (`ChatRequest`, field 12 `session_id`, reserved 14 to 16) |
| turn, tools, pendings | `services/llm/src/feature/llm/services/turn/` (slots, turn-flow, deciders), `services/llm/src/feature/llm/services/tools/` (registry, executor, access, core and remote tools), `services/llm/src/feature/intent/services/intent-router.hxx`, `services/llm/src/feature/pending-intent/`, `services/llm/database/schema.sql` |
| memory | `services/llm/src/feature/memory/services/memory/` (memory-service, graph-recall, memory-formation, memory-vec), `services/llm/src/feature/memory/repositories/memory-graph/` (query and repository), `services/llm/database/schema.sql` (scope columns) |
| voice | `services/voice/src/feature/voice/voice-session-service.{hxx,cc}`, `call-history.hxx`, `services/voice/CMakeLists.txt` (client dependencies) |
| STT, TTS | `services/stt/src/feature/stt/services/stt-service.{hxx,cc}`, `services/tts/src/feature/synthesis/services/tts-service.{hxx,cc}` |
| access | `packages/lib/auth/src/auth/role-access.hxx`, `packages/lib/auth/src/auth/capability.hxx` |
| runtime | `packages/lib/runtime/src/runtime/thread-budget.hxx` (the declared functions), `blocking-task.hxx` |
| model runtime | `third_party/llama.cpp/include/llama.h` in the main checkout (tag `b10305`, the submodule is empty in this worktree): `n_seq_max` (line 354), `kv_unified` (line 396), the `llama_state_seq_*_ext` functions (lines 898 to 918). `third_party/sherpa-onnx/c-api/c-api.h` (line 1448, `SherpaOnnxDecodeMultipleOfflineStreams`) |
| rules | `AGENTS.md` (root), `services/llm/AGENTS.md` (rules 1 to 10 read in full: the brain process, internal wire only, the caller trust of the `voice` caller, the controller owning `LlmService` by value), `services/llm/CONTEXT.md` (section headings only; its turn, memory and pending sections were not read), the reference note `docs/design/push-calls.md` on `design/push-calls` (style only) |

Drift in the plan's line references, verified today: `llm-service.hxx:182` is the header
`packages/clients/llm/src/llm/llm-service.hxx` (its engine mutex is at line 189, the chat template at
182); `llm-service.cc:351` is now the batch build at lines 351 to 352, and the generate lock is at line
451; `voice-session-service.hxx:289` is now `observeSpeaker`, and `CallHistory` is the member at line 202
(the session map `sessions_` is at line 300, keyed by `VoiceSessionSink*`). The plan's claims hold; only
the citations moved.

Not verified in this session: whether llama.cpp `b10305` decodes LFM2.5 (a hybrid convolution and
attention model) correctly with several sequences in one batch, and whether `llama_state_seq_*` restores
a hybrid sequence exactly. That is P2 and nothing here depends on it being true; section 5 makes it a
gate. The model's default `n_seq_max` in `b10305` was not read from the source, only the header comment.

## 1. Decisions, with their source

Each row is a decision of this note. Where the owner decided, the plan section says so.

| id | decision | from |
|---|---|---|
| PC1 | One conversation per account, keyed by `userId`, owned by argus-llm (the service that already owns pendings and memory). It holds turns, a running summary of about 60 words in the user's language, open pendings, active entities and the last leg. It replaces voice's per-session `CallHistory` as the source of truth; voice keeps audio state only. | plan 2 |
| PC2 | Exactly one active leg per conversation (camera, phone or laptop). A second device of the same person takes the conversation over by itself. The old leg hears "seguimos en tu teléfono" and stops capturing for it. A camera leg is never hung up on anyone. | plan 2, owner 8.1 |
| PC3 | A camera takeover needs positive identification of the holder (the face or the voice above that holder's threshold) and no contradiction (the other signal does not match someone else). Without identification it is refused, and the turn is handled as unverified with no personal data; it is not an error. Revision 2 ruling (Q3, backend-c6): the earlier "refused only when both are below" is withdrawn. Phone and laptop legs are authenticated by device and are not subject to this rule. | plan 2, ruling Q3 |
| PC4 | The switch note is one line in the user's language in the context tail, where the memory block goes ("La conversación continúa desde el teléfono; antes estaban en la cámara de la sala."). The conversation continues; nothing restarts. | plan 2 |
| PC5 | Authoritative state is text in argus-llm's database, persisted after every turn. The accelerator is the person's own KV sequence, kept while active, dropped to text after the idle period, rebuilt with one prefill on return. A switch is a pointer change of the active leg. | plan 2 |
| PC6 | An answered call makes that user's leg active. The engine's rule of one ringing call per user is unchanged. The live hand-off of a call between devices follows `camera-watchman-plan.md` section 5b and is not part of phase 1. | plan 2 |
| PC7 | One model in memory, one llama context with several sequences, one scheduler thread in place of the request mutex, `kv_unified` false. Each step: a decode token per active sequence first, then prefill chunks capped at a few hundred tokens, round robin between sequences. The sequence count follows the hardware. | plan 3 |
| PC8 | Parallel sequences are gated. Until P1 and P2 pass on the target CPU, the engine keeps today's single-sequence path, selected by a config key (section 4, `llm.parallel_sequences`, default 1). | plan 7 and 8 order (P1, P2 first) |
| PC9 | STT: one recognizer behind a queue; utterances that become ready within a short window are decoded together with the sherpa-onnx multi-stream call; there are never two threads in one recognizer. | plan 3 |
| PC10 | TTS: chunks are served round robin per conversation through a fair queue, so a person's first audio waits at most one chunk of another person's audio. | plan 3 |
| PC11 | No product limit on conversations. When the machine is saturated a turn hears "un momento" and waits in the fair queue. A refusal (HTTP 429 or equivalent) is never produced for capacity. | plan 3, owner 8.5 |
| PC12 | Every registered person has an account and talks through it. A voice that differs on the person's own device is handled gently and never treated as an intruder; Argus never asks the person to prove who they are. | plan 4, owner 8.2 |
| PC13 | Camera legs: every turn of about 2 s or more is checked with the voice embedding (and the face, with the watchman). A short turn inherits the last verified speaker of that camera within about 10 s. Anything else is answered as unverified, without personal data. | plan 4 |
| PC14 | Self-echo and same-speech: the server keeps about 3 s of each leg's playback and drops a match on another leg's microphone (lag window about 500 ms). Echo is ignored, never subtracted. A transcript that matches text Argus just spoke on any leg is dropped. | plan 4 |
| PC15 | Memory: every extracted fact carries a scope, `personal` (the speaker's own life) or `household` (the home and shared entities). Personal is the default; household is set when the fact is about the home or shared entities, or when the speaker says it is for everyone. | plan 5, owner 8.3 |
| PC16 | Recall is scoped by the listener: a conversation recalls its own person's personal facts and the household facts, never another person's personal facts. The Owner can review and correct a fact's scope. | plan 5 |
| PC17 | Messages never ring. A relayed message is a notification, the camera that sees the recipient, or inside an open conversation. The call engine is not used to ring a person for a message. | plan 6, owner 8.4 |
| PC18 | Measure first: P1 to P5 (section 5) are tests or benchmarks with thresholds. Each measured value is a config knob with a default. | plan 7 |

## 2. What is reused, by module path

- **LLM engine.** `packages/clients/llm/src/llm/llm-service.hxx` (`LlmService`), held by value in
  `LlmController` (`services/llm/src/feature/llm/controllers/llm-controller.hxx:98`, the adapter shape that
  `services/llm/AGENTS.md` rule 6 requires). `LlmService::instance()` (`llm-service.cc:58`) exists and has
  no caller; it is not used and this note does not rely on it. Implementation in
  `services/llm/src/feature/llm/services/llm-service.cc`. Today the
  context has one sequence: the code writes `seq_id 0` at lines 352, 388 and 603, and the context
  parameters (lines 123 to 140) do not set `n_seq_max`. One `mutex_` (header line 189) serialises
  generation; `prefillOnly` requests take it with `try_lock` and return empty on contention (generate
  lock at line 451). The prefix checkpoint machinery (`checkpoint`, `rewind`, lines 265 to 306, with
  `llama_state_seq_get_data_ext` and `llama_state_seq_set_data_ext` on sequence 0 with
  `LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY`) is the precedent for the per-conversation save and restore of
  section 3. Its limits are `kMaxCheckpoints = 8` and `kCheckpointsPerPrefill = 3`.
- **Caller binding.** `services/llm/src/feature/llm/controllers/llm-controller.cc` (`boundToCaller`).
  Only the identity caller `kIdentityCaller = "voice"` (declared in `llm-controller.hxx`) may supply a
  `userId` and role; any other caller gets `userId = 0`, role `Unknown`, no tools and an empty
  `sessionId`. The conversation key is honoured only through this path (risk R4).
- **Turn, router, slots.** `services/llm/src/feature/llm/services/turn/` (`slots`, `turn-flow`,
  `pending-turn`, `deciders`, `decision-policy`), `services/llm/src/feature/intent/services/intent-router.hxx`,
  `services/llm/src/feature/llm/services/lfm-adapter.hxx` (`LfmAdapter` takes `LlmService&` and the
  router). The conversation reuses these unchanged; the LLM still speaks and never chooses a tool (the
  standing rule in the user's memory index: "LLM conversational only").
- **Tools.** `services/llm/src/feature/llm/services/tools/` (`tool-registry`, `tool-executor`,
  `tool-access`, `core-tools`, `remote-tool`). The messenger's `relay` tool is registered here.
- **Pendings.** `services/llm/src/feature/pending-intent/` and the table `pending_intent`
  (`services/llm/database/schema.sql`, which already has `session_id`). Unchanged.
- **Memory.** `services/llm/src/feature/memory/services/memory/memory-service.hxx` (the facade and
  `recall`), `graph-recall.hxx` (recall with `scope` and `refId` in its input), `memory-formation.cc`
  (writes scope `"user"` with `refId = obs.userId`, lines 389 and 390), `memory-vec.hxx`
  (`partitionFor(scope, refId)`, with `"global"` as a special partition), and
  `repositories/memory-graph/memory-graph-query.hxx` (every fact and episode query filters on
  `scope` and `ref_id`).
- **Voice.** `services/voice/src/feature/voice/voice-session-service.{hxx,cc}` (the `Session` struct and
  its `CallHistory`, the `sessions_` map, `voice-session-service.cc:1352` which sets `sessionId` to
  `callId`), `call-history.hxx` (`CallHistoryLimits`: 10 turns kept at most, 5 kept after compaction).
  `services/voice/CMakeLists.txt` already depends on `argus::clients::llm`, `argus::clients::stt`,
  `argus::clients::tts` and `argus::clients::identity`.
- **STT.** `services/stt/src/feature/stt/services/stt-service.{hxx,cc}`: `SttService::decode` takes
  `mutex_` at `stt-service.cc:236` and calls one recognizer. The vendored sherpa-onnx C API provides
  `SherpaOnnxDecodeMultipleOfflineStreams` (`third_party/sherpa-onnx/c-api/c-api.h:1448`).
- **TTS.** `services/tts/src/feature/synthesis/services/tts-service.{hxx,cc}`: `synthMutex_` is a
  `std::timed_mutex`; `acquire` and `acquireWhenIdle` (lines 304 and 317) are the per-chunk lock the
  fair queue replaces. Chunking is `text/prosodic-chunker.hxx`, unchanged.
- **Runtime budget.** `packages/lib/runtime/src/runtime/thread-budget.hxx` declares `batchThreads()`,
  `lightThreads()` and `inferenceSlots()`; `blocking-task.hxx` is the off-loop awaiter. The `heavy`
  lane variant named in `AGENTS.md` (section 13c) was not confirmed in the header read here.
- **Access.** `packages/lib/auth/src/auth/role-access.hxx`: `kTableAccess` (line 38), `AppAction` and
  `hasAppAction` (lines 671 and 678). `packages/lib/auth/src/auth/capability.hxx`: `kCapabilities`
  with ids such as `assistant.voice`, `calls.join`, `notifications.register`, `people.read`,
  `memory.manage`. The tool gate reads these through `role_access::capabilitiesFor`.
- **Name matching.** `packages/lib/text/src/text/name-match.hxx` (`text_norm::matchName`), used by the
  messenger's recipient slot (plan 6).
- **Sync contract.** `packages/contracts/sync/src/sync/` is not changed in phase 1 (section 4).

## 3. What is new, with the rule-23 layout

Every new unit below has a home that follows rule 23 (`feature/<feature>/` owns it; `shared/` only when
two features of the same service read it) and rule 24 (no folder without a consumer). Each unit carries
its own tests (`*-test.cc`) under the owner's `tests/unit`, and no comments.

argus-llm (`services/llm/`):

- `packages/contracts/llm/src/llm/conversation-leg.hxx`: `enum class ConversationLegKind` (`Camera`,
  `Phone`, `Laptop`) with `conversationLegKindToString` and `conversationLegKindFromString`. It is a
  wire vocabulary, so it lives in the contract (rule 1 and the "enums where they are used" rule); voice
  and llm both read it.
- `services/llm/src/feature/conversation/`: the conversation feature.
  - `repositories/conversation/`: `conversation-query.hxx` (SQL strings and parameter structs),
    `conversation-repository.hxx` and `.cc`. Turns and the conversation row go to the additive tables
    `conversation` and `conversation_turn` in `services/llm/database/schema.sql`.
  - `services/`: `ConversationService` (open, take over, switch note, idle drop to text, the rebuild
    request, `appendInbound` and `hasOpenConversation` for the messenger, section 6), and the summary
    job that runs off-turn through the main `LlmService`.
  - `schemas/`: the row mapping.
- `services/llm/src/feature/llm/services/sequence-scheduler/`: the engine change of PC7. One scheduler
  thread, per-sequence state, decode-first round robin, prefill chunks, and save and restore through
  the existing checkpoint primitives. It replaces `mutex_` only when `llm.parallel_sequences > 1`.
- `services/llm/src/shared/vocabulary/memory-scope.hxx`: `enum class MemoryScope` (`Personal`,
  `Household`) with its ToString and FromString pair. It is shared because the conversation feature
  (writes and recall) and the memory feature (formation and graph recall) both read it.
- `services/llm/tests/eval/parallel-bench.py` (P1, section 5): wraps `llama-batched-bench -npl 1,2,4`,
  exits 77 when the model is absent, as the existing eval harnesses do.

argus-voice (`services/voice/`):

- `services/voice/src/feature/voice/services/leg-arbiter/`: the leg decision (takeover, verified speaker
  check for a camera leg, the 10 s inheritance window, PC2 to PC4 and PC13). Read only by the voice
  session, so it stays inside the voice feature until a second reader exists.
- The same-speech and echo detector (PC14) is planned here for P4 and is not part of the first code
  units.

argus-stt (`services/stt/`):

- `services/stt/src/feature/stt/services/utterance-batcher/`: the one queue in front of the recognizer
  (PC9), collecting ready utterances within the window knob (section 8, Q8) and decoding them as one
  multi-stream call.

argus-tts (`services/tts/`):

- `services/tts/src/feature/synthesis/services/fair-chunk-queue.{hxx,cc}`: a round-robin queue of
  prosodic chunks keyed by conversation key. It replaces the per-chunk timed mutex for ordering; the
  synthesis itself still holds `synthMutex_` for one engine call at a time.

packages/lib/auth:

- One new row in `kCapabilities` for the messenger's relay (section 6). No table change, no route change
  in phase 1.

Test placement follows the existing layout: `services/llm/tests/unit/`, `services/voice/tests/unit/`,
`services/stt/tests/unit/`, `services/tts/tests/unit/`. Model-dependent tests skip with a clear message
when the model file is absent (the skip convention of the eval harnesses; whether each unit test follows
it was not checked).

## 4. Contract changes

Every contract change below is coordinated with the frontend (`frontend/`, the sibling React Native
app), per the root `AGENTS.md` rule that changes to endpoints, WS operations, DTO shapes, `SyncOperation`
values and role permissions travel with the frontend.

- **Sync (`/sync`, `SyncOperation`).** No new `SyncOperation` value in phase 1. A conversation row is not
  a syncable table, so nothing is added to `/sync`. If the app should show "we were talking on the
  camera", the way is `ResourceChanged` (`resource_changed`, domain and scope registered in
  `packages/contracts/sync/src/sync/resource-change.hxx`), not a new operation. That was open question
  Q11, ruled: no (section 8).
- **LLM wire (`packages/contracts/llm/llm.proto`).** `ChatRequest` gains two fields, numbered 17 and 18
  (14 to 16 are reserved): `ConversationLegKind conversation_leg` (as a proto enum) and `bool
  use_conversation` (default false). When `use_conversation` is false the request behaves exactly as
  today: the caller sends the full message history. When true, argus-llm loads the conversation for
  `user_id`, appends the request's newest user message and ignores the caller's history. This keeps
  compatibility during the migration (rule of the plan, section 8, "keep compatibility").
- **Voice wire (`packages/contracts/proto/argus/voice/v1/voice.proto`).** `VoiceStart` gains a
  `leg` field (a new `VoiceLegKind` proto enum with the same three values; numbered 5). The phone and
  laptop set it; the camera leg is set by the server. Frontend change required: the app sends the leg
  when it starts a voice session.
- **TTS and STT wire.** `packages/contracts/tts/tts.proto` and the STT request gain an optional
  `conversation_key` string (`<userId>:<leg>`), used only for the fair queue. Old callers omit it and
  get the default lane. No frontend change.
- **role-access.** One new capability in `kCapabilities`: `messages.relay`, module core, granted to every
  registered role (Owner, Resident, Guard, Guest), never to `Unknown`, and never to a visitor (a visitor is
  not a role; Q10 ruling). No change to `kTableAccess` in phase 1.
- **HTTP routes.** No new public route in phase 1. The conversation is internal to argus-llm and is
  reached through the existing chat wire. A read route for the Owner's summary is deferred (Q12).
- **Memory scope.** The `scope` column of `memory_fact` and `memory_episode` keeps its TEXT type. The
  values become `user` (personal, `ref_id` the speaker's `userId`, unchanged for existing rows) and
  `household` (`ref_id` 0). Because the column has no CHECK today, no migration is needed; if a CHECK is
  added later it must be the `MemoryScope` enum (rule 1). The Owner's scope correction is an audit
  change under the sync rules, not a new operation (Q7).

## 5. The measurements as tests and benchmarks

Each row gives the test or benchmark, the threshold (a default, not a measured value), and the config
key that holds the threshold, so the measured value can be set without a code change. Defaults are this
note's proposal; the owner confirms them (Q13).

| id | test or bench | threshold (default) | knob (`config.toml`) |
|---|---|---|---|
| P1 | `services/llm/tests/eval/parallel-bench.py` over `llama-batched-bench -npl 1,2,4` on the target CPU. Reports aggregate decode tokens/s and per-user first-token latency with 1, 2 and 4 active conversations. | aggregate tokens/s at 2 sequences at least 1.5 times 1 sequence; at 4 sequences at least 2.2 times; per-user first-token latency with 2 active conversations at most 1.6 times the single-user latency | `llm.parallel.p1_min_scaling_2 = 1.5`, `llm.parallel.p1_min_scaling_4 = 2.2`, `llm.parallel.p1_max_latency_ratio = 1.6` |
| P2 | `services/llm/tests/unit/llm-sequence-state-test.cc` (model-dependent, skipped without the model). Two sequences decoded interleaved, greedy, 64 tokens, against each sequence decoded alone. A save at token k and restore followed by continuation against the uninterrupted run. | zero token mismatches in both cases; the saved state of one conversation at 4096 tokens at most 48 MiB | `llm.parallel.state_max_mib = 48` |
| P2b | The same test with the prefix checkpoints of PC5 (rebuild after idle): the rebuilt sequence reproduces the uninterrupted tokens. | zero mismatches | none (a correctness gate) |
| switch | `services/llm/tests/unit/conversation-switch-test.cc` (no model; the store and the pointer change only). A takeover from one leg to another, measured from the takeover call to the new leg being ready. | at most 100 ms with the KV already resident; the rebuild cost is measured by P1 and reported separately | `conversation.switch_budget_ms = 100` |
| P3 | `services/voice/tests/eval/voice-verify-eval.py` (new harness). Equal error rate of the voice embedding by turn length (2 s, 4 s, 8 s) at 8 kHz (camera) and 16 kHz (phone), run on BOTH the Common Voice clips and the Peruvian clips that `scripts/stt-eval-data.py` downloads (Q14 ruling). If the Peruvian clips are unavailable the harness reports it and exits 77; it never substitutes the other set silently. | at 16 kHz and 4 s or longer: EER at most 5 %; at 8 kHz and 4 s or longer: EER at most 8 % | `voice.verify.max_eer_16k = 0.05`, `voice.verify.max_eer_8k = 0.08` |
| P4 | `services/voice/tests/unit/leg-correlation-test.cc` over recorded pairs and synthetic ones with jitter (lag window 500 ms) and G.711 on the camera side. Two checks: same-speech detection within ±300 ms, and self-echo match against the server's playback history. | same-speech detection at least 95 % with at most 2 % false positives on different speech; echo match correlation at least 0.9 within the 500 ms lag window | `leg.same_speech_min_rate = 0.95`, `leg.same_speech_max_false = 0.02`, `leg.echo_min_corr = 0.9`, `leg.echo_lag_ms = 500` |
| P5 | `services/llm/tests/eval/load-bench.py` (new harness): N conversations with STT and TTS running together, N = 1, 2, 4, on the target machine. Reports p95 time to first audio and the thread count against `ThreadBudget`. | p95 time to first audio at N = 4 at most 2.5 s; the process never exceeds the thread count `ThreadBudget` gives for its lanes (asserted by a unit test that reads the budget) | `conversation.p95_first_audio_ms = 2500` |

Every P-test that needs a model or audio the repository does not hold exits 77 (skipped) with the
missing item named, the convention of `services/llm/tests/eval/`. A gate fails only when a measured
value is worse than its threshold, never when the input is missing.

## 6. Interfaces with the messenger track (F8)

The messenger note is written on `track/b4-messenger` by another agent. This section names the shared
points only; the two notes must agree on them, and the reviewer checks that they do.

- **The relay tool.** The messenger registers a tool named `relay` through
  `services/llm/src/feature/llm/services/tools/` (`ToolRegistry`). The router picks it (never the LLM)
  and its slots (recipient through `text_norm::matchName`, message in the sender's words) are the
  messenger's. This note does not design the tool.
- **The capability.** `messages.relay` in `packages/lib/auth/src/auth/capability.hxx`. The tool gate
  shows the tool only to roles that hold the capability (`role_access::capabilitiesFor`). The role mask
  is ruled (Q10): every registered role, never `Unknown`, never a visitor.
- **Delivery into an open conversation.** This note provides `ConversationService::appendInbound`, which
  places a message from another person into the recipient's open conversation as a context-tail line
  (the same mechanism as the switch note, PC4). The messenger decides when to call it and what the line
  says.
- **Presence, without revealing it.** This note provides `ConversationService::hasOpenConversation`
  returning a boolean for one `userId`. No leg, no device and no time is ever returned to another
  person (plan 6, privacy). The messenger reads only that boolean.
- **Pendings.** A message that cannot be delivered now is the messenger's to store. The messenger may
  use `pending_intent`; this note changes nothing in that table.
- **Notifications and calls.** Delivery by notification goes through the notification client; the call
  engine is not used for messages (PC17). Neither is changed here.

## 7. Risks

- R1 Multi-sequence correctness on LFM2.5 (a hybrid model, plan 3) is unverified. P2 is a gate; PC8 keeps
  the single-sequence path until it passes.
- R2 Batching may not speed up decode on the target CPU (memory bandwidth bound, plan 3). P1 decides
  the default for `llm.parallel_sequences`. The memory note in the user's index records the owner's rule
  for model memory (at most 1 GB per model, from 2026-10-08); the KV state of several conversations
  must fit inside the RAM the box has after that model, which P2 and P5 measure.
- R3 Privacy leak through recall. The entity-anchored recursive recall in `graph-recall` and the FTS and
  vec tiers each take a scope and a `refId`; a missed filter would expose another person's personal
  facts. The tests for PC16 must cover every recall tier, not only the facade.
- R4 Caller trust. A conversation keyed by `userId` is safe only when `userId` comes from the identity
  caller (`boundToCaller`). A non-identity caller with `userId = 0` must never reach a conversation.
  The use-conversation path is refused for any other caller.
- R5 Migration of voice history. Removing `CallHistory` as the source of truth changes how a call
  remembers earlier turns. The compatibility key (section 4) keeps the current path until the conversation
  path passes its tests.
- R6 `sessionId` and `callId`. Today the voice session passes its call id as the LLM `sessionId`
  (`voice-session-service.cc:1352`). A conversation keyed by person means two calls of the same person
  share one conversation. That is the intent of PC1, but pendings and memory episodes keyed by
  `session_id` change meaning; the migration must say so.
- R7 TTS lock ordering. `TtsService` takes `lifecycleMutex_` and then `synthMutex_` (`tts-service.cc:100`
  and `:101`). The fair queue must not hold either across a blocking enqueue.
- R8 Thread budget. Four sequences, STT and TTS share one machine. `ThreadBudget` sizes the lanes; a
  scheduler thread added beside them must be counted, which P5 checks.

## 8. Open questions and rulings

Each question was numbered with a recommended default. backend-c6 ruled on all of them on 2026-10-14
(revision 2). Accepted as proposed unless marked CHANGED. Where a ruling leaves a value to David, the
default in code is the value stated here and the final number is David's.

Q1 (David) Summary length and language. RULED, accepted: about 60 words in the user's language,
regenerated off-turn by the main `LlmService` when the turns since the last summary exceed 20.

Q2 (David) Idle drop. RULED, accepted: a conversation's KV sequence is dropped to text after 900 s
without a turn (`conversation.idle_seconds = 900`); the text stays.

Q3 (David) Takeover threshold for a camera leg. RULED, CHANGED (backend-c6): a camera takeover needs
POSITIVE identification of the holder (face OR voice above that holder's threshold) and NO contradiction
(the other signal does not match someone else). With no identification it is refused, and the turn is
handled as unverified with no personal data. The earlier default ("refused only when both are below the
threshold") is withdrawn as too permissive.

Q4 (David) Call legs. RULED, accepted: an answered call becomes the user's active leg for its duration;
when the call ends, the leg returns to the last non-call leg and the summary records the call.

Q5 (David) Retention of turns in argus-llm's database. RULED, with a ruling on the value (backend-c6): the
30-day default is kept in code as a config value (`conversation.retention_days = 30`); the final number is
David's to set. This is personal data under Ley 29733 and needs David's decision before any production run.

Q6 (David) Parallel sequences. RULED, accepted: `llm.parallel_sequences = 1` until P1 and P2 pass; then
the count is `min(4, inferenceSlots())` from `ThreadBudget`, with no product cap (PC11).

Q7 (David) Owner correction of a fact's scope. RULED, accepted: an audited change to the fact's scope
through the Owner's memory screen, written as an audit row, never as a sync `Add`.

Q8 (backend-c6) STT batching window. RULED, accepted: 25 ms (`stt.batch_window_ms = 25`); the first
utterance is decoded alone when nothing else is ready.

Q9 (backend-c6) The LLM wire fields. RULED, accepted: `conversation_leg` is field 17 and `use_conversation`
is field 18 of `ChatRequest`; `VoiceStart.leg` is field 5. Additive; no field renumbered.

Q10 (messenger, backend-c6) The capability `messages.relay` and its role mask. RULED, CHANGED
(backend-c6): granted to every REGISTERED role (Owner, Resident, Guard, Guest). Never `Unknown`, and never
a visitor (a visitor is not a registered role, so it cannot hold the capability).

Q11 (David, backend-c6) Does the app show the conversation's leg? RULED, accepted: no in phase 1. If it
is ever wanted, it is a `ResourceChanged` domain `conversation`, not a new `SyncOperation`.

Q12 (David) The Owner's summary route. RULED, accepted: deferred.

Q13 (David) The thresholds of section 5. RULED, accepted: the numbers in that table are the defaults; each
is a config knob, so a measured value replaces it without a code change.

Q14 (David) Data for P3. RULED, CHANGED (backend-c6): P3 uses BOTH the Common Voice clips and the
Peruvian clips that `scripts/stt-eval-data.py` downloads. If the Peruvian clips are unavailable, P3 reports
it and exits 77; no silent substitution.

Q15 (backend-c6) Co-located handling. RULED, accepted: the detector and its tests (P4) are written in phase 2
as units; the wiring into the voice session waits for the watchman.

Q16 (backend-c6) The compatibility key during migration. RULED, accepted: `use_conversation = false` stays
the default in every build until the conversation path passes its tests.

## 9. What was not verified in the code

- The llama.cpp b10305 behaviour for multi-sequence hybrid decode and `llama_state_seq_*` restore (P2).
  The submodule is empty in this worktree; the header was read from the main checkout.
- The default value of `n_seq_max` in b10305. Only the header comment was read.
- The `heavy` lane of `ThreadBudget`, which AGENTS.md names (section 13c); the header read here declares
  three functions only.
- The recursive recall CTE in `graph-recall.cc` was not read line by line; R3 depends on it.
- The one-ringing-call rule in `services/notification/src/feature/call/` was taken from the plan and not
  re-read in this note.

## 10. Step 2 status (2026-10-14)

Built as unit commits on `track/b4-conversations`, each compiled-free (the phase rule), each checked by
the comment and dependency gates: the leg enum (contract), the memory scope enum, the TTS fair chunk
queue, the messages.relay capability (Q10), the leg arbiter with the Q3 rule, the STT utterance batcher
(Q8), the conversation config knobs (Q2, Q5, Q6), the conversation store tables and repository (Q5), the
switch note (PC4), the P3 harness with both clip sets (Q14), and the P4 leg correlation detector (Q15).

Not built in step 2, and why:

- The ConversationService (takeover, idle drop to text, appendInbound, hasOpenConversation). It touches
  the llm engine's context and the voice session; it waits for the sequence scheduler design.
- The sequence scheduler and the llm.parallel_sequences wiring. P2 is the gate (ruling); nothing is
  wired until P2 has a test that can pass.
- The conversation_key wire field and the TTS and STT wiring of the queue and the batcher. The field
  is a contract change coordinated with the frontend; the queue and batcher are not yet called.
- P1 (llama-batched-bench wrapper), P2 (sequence save and restore test) and P5 (the load bench). They
  need the binary, the model and the target machine; the harness output format of llama-batched-bench
  was not verified, so no parser is written.
- Memory scope classification (PC15): the formation path still writes scope "user" for every fact;
  the household rule needs its own design.
- Speaker identities in the stt-eval manifest, which P3 needs to build trials.

## 11. Phase 2 list: checks carried from phase 1

Each check states what was read, what was found, and what was done.

1. **Streaming and gRPC refusals (done, by reading, then fixed).**
   - HTTP stream: the handler opens the 200 response before the job runs, and the job
     catches every exception and only logs it. Before the fix, a conversation refusal
     raised inside the job left a 200 with a cut stream: no sentinel, no final `onToken`
     call. Fixed in 426f6dfa: `requireConversationAccess` runs in the handler, before the
     lease and the stream, so a refusal is a 503 or 400 response. The sync HTTP chat runs
     the same check before the blocking lane.
   - Open, not from this work: an engine exception in the middle of an HTTP stream still
     leaves the stream without its sentinel, as before this series. Phase 2 must define
     an error sentinel and make the voice client treat a missing sentinel as a failed
     turn. This changes the stream protocol, so it is a wire decision.
   - gRPC unary and stream: the refusal is a `ResponseException` raised in `chatSync` or
     `chatStreamSync`, caught in the rpc handler and mapped by `toRpcStatus`
     (`packages/contracts/response/src/response/response-rpc.cc`: 400 to
     INVALID_ARGUMENT, 503 to UNAVAILABLE). The stream keeps its queued status and
     returns it (`llm-rpc-server.cc`, the producer sets `done` after a catch). `UNKNOWN`
     appears only for a non-`ResponseException`, which maps to 500 INTERNAL. No change
     needed.
2. **The recorded user text is the screened text (done, fixed in a82a01d7).** Read:
   the duress screen returns its input unchanged on a pass; the controller discarded the
   screen's text except when the turn was withheld. Now the screen's text replaces the
   user message whenever it differs, so the stored user turn and the model's last user
   message are the screened text. Test: the rewriting-screen case in
   `llm-controller-conversation-test`. The rule is written in the llm CONTEXT.
3. **The real generation path (covered by the phase 2 gate, no mock).** The controller
   test drives turns through the turn-engine seam, not the model, by design. The real
   path is exercised by `memory-recall-vec-e2e-test` (2037f9a6) for the memory side and
   must be added for the conversation side in phase 2. The gate assumes that `init`
   re-embeds facts that have no vector row; if it does not, the gate fails at its first
   count check (it does not pass silently).

### 11.1 Approved phase 2 unit: the stream error sentinel (not blocking batch 4)

Approved by backend-c6 as a phase 2 unit. It closes the open item of check 1: an engine
exception in the middle of an HTTP stream currently leaves the stream without its
sentinel and without a final `onToken` call.

Spec:

- ADDITIVE. The sentinel line keeps its mark and its shape: `done` stays `true`, and the
  stats, `speech` and `act` fields keep their meaning. A failed turn adds two keys to the
  sentinel: `"failed": true` and `"error": "<stable code>"` (for example `engine` or
  `refused`). No other key changes.
- Always a final call. When the engine throws after the stream opened, the job sends the
  sentinel with `failed` and `error`, then closes the stream. No token text is sent after
  the sentinel, and the final `onToken(done=true)` call is never skipped.
- argus-voice (the only consumer) reads `failed` and reports a failed turn to the session;
  it does not read `error` aloud.
- Compatibility: an old voice client parses the line as the end of the turn, because it
  still has `done: true`. It never reads `failed` or `error` as text, because they are JSON
  keys of the sentinel, not tokens. The unit's test asserts that an old client, given the
  failed sentinel, ends the turn and produces no spoken text.

Acceptance: a test that the failed sentinel is sent on an engine exception, that no token
follows it, and that the old client's reading of it is end of turn with no text.
