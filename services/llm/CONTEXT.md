# argus-llm — CONTEXT

## Why the llm service exists

F4-5 of the `migracion-microservicios` plan extracts the LLM engine out of
the legacy monolith (Rulings BS-BV). `argus-llm` is a sibling service with
its own binary, own CMake preset and zero HTTP exposure. Unlike the VLM
(F4-4), this is NOT a capacity-only move: the voice session is a real
consumer, so the cutover re-points the voice session's `IVoiceLlm` seam to
this service over the internal wire while the legacy keeps its in-process
`LlmService` boot-initialized for MemoryService (see the dual state below).
It mirrors the argus-tts (F4-2), argus-stt (F4-3) and argus-vlm (F4-4)
scaffolds.

## What it owns

- **The chat engine** (`LlmService`: its interface is the tier-3 client's
  `packages/clients/llm/src/llm/llm-service.hxx`, its implementation this
  feature's `services/llm-service.cc`): LiquidAI LFM2.5-1.2B-Instruct
  (`models/llm/LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf`) through llama.cpp
  (vendored `third_party/llama.cpp`, tag b10305; the same commit the legacy
  links — NO mtmd: chat only). Boot aborts if the engine fails to load.
  `LlmService` is owned BY VALUE by the controller (the adapter shape — no
  singleton) and takes `ai_init::llamaMutex()` exactly as the legacy
  llm-service.cc does; `argus-llm`'s main owns its own
  `llama_backend_init/free` with the legacy teardown order (tear the engine
  down BEFORE freeing the backend — skipping the ordered teardown segfaults
  in `llama_backend_free`).
- **The KV-prefix cache (`cachedTokens_`) stays IN the service**: one llama
  context, one sequence, per-process warm state. Concurrent sessions with
  different prompts still evict each other, as two voice sessions did
  in-process. A new prompt reuses the longest prefix it shares with what the
  cache holds, which is the prompt and reply of the last generation. A
  pure-attention model drops the tail past the shared prefix
  (`llama_memory_seq_rm`). LFM2 cannot: ten of its sixteen layers are short
  convolutions whose recurrent state has no history to roll back to, so its
  tail removal fails. Before 2026-10-03 the cache was therefore only ever
  reused when the new prompt extended the old one exactly. Any divergence,
  including the one every tool turn produces (the call and tool result are in
  the cache, but the voice history keeps only the spoken answer), threw away
  the whole prefix.
- **Prefix checkpoints (`PrefixCheckpoint`).** The recurrent state is small:
  the two-token convolution window of ten layers, about 160 KB. While
  prefilling, `LlmService` splits the batch at the last three message starts
  (`<|im_start|>`) of the newly decoded range and copies that state with
  `llama_state_seq_get_data_ext(…, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY)`. It
  keeps up to eight checkpoints and drops those past any rewind. A divergent
  prompt restores the newest checkpoint at or before the divergence and
  removes the attention tail past it. Only the messages after that point are
  decoded again. This is the llama-server "context checkpoint" technique
  (ggml-org/llama.cpp server, `n_ctx_checkpoints`) placed at message
  boundaries. A full hit (the same prompt again) restores the checkpoint
  before the generation prompt instead of failing.
- **Priming (`prefill_only`).** A chat request with `prefill_only` builds the
  exact prompt the next turn starts from: the caller's history, through the same
  speak path. It prefills that prompt and answers an empty completion, without
  deciding and without running a tool. The voice
  session sends one after the greeting and after it rebuilds history[0], so
  the first turn decodes only the user's message. A prefill-only call takes
  the engine with `try_lock`: when a real generation holds the engine, the
  priming is skipped rather than queued.
- **The last-prefill counters have a provenance limit.** `lastStats_` is the
  engine's MOST RECENT prefill (`std::atomic`, one store per `prefill()`), so
  while one generation runs every other reader sees its numbers, and the tool
  loop's non-streamed terminal emit happens after the engine releases its
  mutex — a second generation in flight can therefore report the first one's
  counters. Both legs read the same member at the same point (the HTTP
  sentinel line and the gRPC closing frame), the suites all run one inference
  slot while `main.cc` sizes `ThreadBudget::inferenceSlots()`, and the full
  fix — carrying the stats in the generation's own outcome — would change
  `LlmService`'s callback contract, shared by four consumers (Phase 4 step 6c
  review, recorded there).
- **The internal wire (Ruling BT)**:
  - `POST /llm/v1/chat` — JSON `{messages, max_tokens?, temperature?,
    reset_context?, tools?}` → frozen app-envelope with `info.text` = the full
    completion. Inference runs off the event loop (`chatAsync`). `tools:false`
    keeps the request on the direct engine path even when the process has
    tools registered — the DTO carries it into `ChatRequest`, and the gRPC
    leg resolves an absent `tools` to true, so both legs honour it; raw JSON
    callers (argus-guard) use it to avoid the memory-tool preamble (~400
    tokens of prefill on every call).
  - `POST /llm/v1/chat-stream` — same body; `Transfer-Encoding: chunked`
    text stream: every token callback flushed AS PRODUCED (arrival order
    preserved — the voice session's sentence chunker depends on it),
    terminated by the sentinel `\x1e{done:true, prompt_tokens,
    reused_tokens, decoded_tokens}\n`. The ASCII record separator (0x1E) is
    out of band: the server strips it from every token before writing it,
    so only the sentinel carries it and a model that writes
    `{"done":true}` cannot end the stream (it used to, with the old
    `\n{…}` sentinel). The sentinel is the client's exact end-of-stream
    marker; token framing is "chunks until the 0x1E mark". Generation runs
    on a worker of the Heavy blocking lane, admitted by `StreamSlots`
    (`packages/lib/runtime`; at most the Heavy lane's thread cap minus one
    streams at a time, so one Heavy worker stays free for the plain chat;
    one more is 429 `TOO_MANY_REQUESTS`, and once the stop began it is 503
    `SERVICE_UNAVAILABLE`), and the engine mutex serializes concurrent
    generations. It used to be one detached `std::thread` per request. A client
    that disconnects ends the generation at the next token: the token
    callback throws, as `ChatStream`'s does on a cancelled call, so a dropped
    request does not hold the engine for up to 4096 tokens.
  - `GET /llm/v1/config` — `{loaded, defaultMaxTokens, defaultTemperature,
    contextSize, lastPromptTokens, lastReusedTokens, lastDecodedTokens}`
    (additive, internal-only).
  - Errors, all in the frozen envelope: 400 `BAD_REQUEST` (non-JSON body),
    422 VALIDATION_ERROR (empty/oversized messages, out-of-range
    max_tokens/temperature — `fields` keyed by the C++ member names, the
    documented DSL quirk), 503 `LLM_NOT_LOADED`, frozen 404/405. Latency
    logged per request.
- **The gRPC leg (Phase 4 step 6c, Ruling BT's typed face)** —
  `argus.llm.v1`'s `Chat` service, served by `src/app/rpc/llm-rpc-server.cc`
  (`argus::llm-rpc`) on `[rpc] address`, dormant unless that address and at
  least one non-empty `[rpc.callers]` pair are set (both are empty in
  `config.toml.example`, no deploy config sets them). Three RPCs: unary
  `Capabilities` (loaded, the engine's defaults, the context size, the three
  last-prefill counters), unary `Chat` (the full completion in
  `ChatResponse.text`) and server-streaming `ChatStream`, whose token frames
  carry each token's ordinal in `sequence` and whose final frame is the `done`
  token carrying the token count and the three counters — the client's
  end-of-stream marker, the role the HTTP sentinel line plays.
  Requests carry the same fields as the HTTP body (`messages`, `max_tokens`,
  `temperature`, `reset_context`, `tools`, `user_id`, `grammar`,
  `grammar_required`, the caller's role and `lang`), with `temperature` and `tools` declared proto3
  `optional` so an undeclared one takes the engine's default exactly as an
  omitted HTTP key does; the server re-checks every bound the client checks
  (the wire is a boundary, not everyone is the client) and refuses a
  caller-declared deadline more than a second past
  `argus::llm::kMaxTimeout` — gRPC rounds the relative `grpc-timeout` header,
  so the flat two-minute ceiling the tts and stt servers carry would refuse
  the client's own maximum. A caller arriving while every
  `ThreadBudget::inferenceSlots()` slot is held waits for one, until its own
  deadline or cancellation (504 / 499), instead of a 429 `Busy`: with fewer
  than sixteen hardware threads there is one slot, and the generations are
  serialized by the engine mutex anyway, so refusing only turned a second
  speaker's turn into silence. A caller without a deadline waits at most
  the longest deadline the server accepts and is then refused 429 `Busy`,
  so no gRPC thread is parked forever. The engine
  call is synchronous on the gRPC server's own thread, never on the Drogon
  loop. `ChatStream` runs the engine on a `std::jthread` producer into a
  64-token bounded queue (the TTS stream pattern, polling its condition
  variable so a cancelled or expired call is noticed while the queue is
  full).
- **The two legs do not share an error type**, deliberately: the gRPC leg
  throws `ResponseException` (the contract's ten refusals, or what
  `argus::response::fromRpcStatus` makes of a bare transport status), while
  the HTTP leg keeps its older `std::runtime_error` spelling
  (`argus-llm <code>: <message>`), which is why every in-tree caller catches
  `std::exception`. `packages/clients/llm/src/llm/llm-remote.{hxx,cc}` holds
  both: `LlmHttpClient` and the façade `LlmClient`, which picks the leg per
  call — `llm.grpc_target` set means gRPC, empty means HTTP, and the knobs
  are `llm.grpc_target`/`llm.grpc_credential` beside the HTTP
  `llm.remote_url`/`llm.remote_timeout_ms`. The façade caches its gRPC client
  in a `std::atomic<std::shared_ptr<RpcCache>>` whose entry carries both the
  target and the credential it was built for, so a runtime change of either
  rebuilds before the next call. The gRPC client's timeout is the configured
  budget clamped to `argus::llm::kMaxTimeout`, and a non-positive budget takes
  that ceiling — the HTTP leg accepts a budget the typed leg's own gate would
  refuse, so the façade clamps instead of failing every call.
- **Call session**: `session_id` (proto field 12, HTTP `session_id`,
  `ChatRequest::sessionId`, at most 128 bytes on both legs and refused above
  that) names the conversation a request belongs to. The voice session fills
  it with its per-call id. It becomes `ToolContext::sessionId`, the turn
  reference memory formation stores on the fact's source, so a fact saved
  mid-call points back to the call that produced it.
- **Caller role and language**: a chat request names the role of the user it
  speaks for (`caller_role` on the gRPC wire, `role` in the HTTP body) and the
  turn's language (`lang`, `es`/`en`). Both are additive: an absent or
  unknown wire role is `UserRole::Unknown`, which holds nothing (it used to be
  Guest), and an absent `lang` keeps the tool runtime's Spanish default; a
  `lang` outside `es`/`en` is refused. The turn used to run every turn
  as a Resident in Spanish. Now the controller offers the deciders only the
  tools whose capability the role holds (`ToolExecutor::offered`, over
  `role_access::hasCapability`; "Tools over MCP"), and the executor still
  checks every call, the routed ones included. A role with no permitted tool
  gets the direct engine path.
- **Config**: `[llm]` (engine knobs, mirroring the legacy block) +
  `[decide]` (the Laya bundle, the fallback policy and the ONNX session
  options; "Laya and GLiNER: the turn's engines") +
  `[server]` (loopback listener, default 7032) + `[rpc]`/`[rpc.callers]`
  (the gRPC face). The `[server]`
  listener is internal-network only: the wire is never announced or
  published; so is the gRPC one, whose plaintext leg is gated by the caller
  credential alone. The same Chat service also listens on `[internal_tls]
  address` (`rpc.address` plus 200, 7332 in the deploy stack) with mutual TLS
  from the internal certificate in `[internal_tls] dir`; `LlmRpcServer` opens
  both through `argus::client::startListening`, caps a message at 8 MiB on
  both, and throws when a port cannot be bound. The plaintext leg stays until
  the owner's deploy check. The database is `[memory]` (`db_file =
  "database/memory.db"`, `schema_file = "database/schema.sql"`) with
  `[extract]` (the GLiNER bundle, its session options and the memory
  lexicon's coverage floor) beside it, and `[nats]` is the bus the catalog
  replica and the guard feed read; no JWT/device key lives here.

## Tier note (the F4-2 lesson, applied)

The service compiles `hardware-profile.cc` with `ARGUS_NO_NCNN_GPU=1`, so
its capability tier is derived WITHOUT the Vulkan probe and
`HardwareProbe::llmGpuLayers()` resolves CPU-only. When the legacy offloads
to GPU, pin the parity value with `[llm] gpu_layers` (999) in this
service's config — `LlmService::init` reads the same override the legacy
reads.

## The dual state (Ruling BU/BF) — legacy side

`llm.remote_url` in the legacy `[llm]` block re-points ONLY the voice
session (`RemoteVoiceLlm` behind the F4-1 `IVoiceLlm` seam). The legacy
`LlmService` STAYS boot-initialized either way: `MemoryService`'s
`isBusy()` polling and `chat()` are same-process calls against the
in-process singleton (the back-pressure signal), and that redesign is
argus-memory's (F4-6), not this task's. The boot log shows both lines:
"LLM delegated to <url> for the voice session" AND "LLM loaded: …". There
is no in-process fallback once remote is configured — a down argus-llm
surfaces as an exception inside the voice turn's existing error path (the
turn degrades, the worker survives), while memory workers keep running
untouched.

## What it did NOT change

- The mobile app never talks to this service; no public routing, no new
  app-facing contract; voice frames stay byte-identical.
- No tool loop at the time (Ruling BV) — superseded by f8-b4, which landed
  the loop here: the tool runtime lives in this feature at
  `src/feature/llm/services/tools/` (`ToolRegistry`, `ToolExecutor`,
  the schema check in `lib/mcp`) and the controller drives
  `chatWithTools`/`chatWithToolsStream`.
- Model artifacts stay in the shared `models/llm/` tree — never copied.
- The legacy `LlmService` stays linked and initialized in the legacy binary
  (Ruling BF); the symbol proof for the legacy is unchanged by this task.

## Compose volume

The docker compose must mount the shared `models/` tree (at least
`models/llm`) into this service's working directory — the engine reads the
GGUF relative to the `[llm]` config keys.

## Owner settings

`src/feature/settings/llm-settings.cc` is the catalog an owner may change
through `argus.settings.v1.Settings`, registered on the same gRPC listener as
`argus.llm.v1.Chat` (`argus::contracts::settings-wire`). It lists only
owner-meaningful keys, in three groups: `sampling` (temperature and max
tokens are basic; top-k, top-p, the four penalties and the seed are advanced),
`memory` (recall top-k, recall deadline, recall token budget, extraction
wait, compaction token budget, the camera-events toggle and embedding
preload) and `engine` (context size, GPU layers, threads, batch threads, KV
type, flash attention, n_batch, n_ubatch). Paths, model files, the database
and schema, targets, credentials and the listener are never in it.

The five memory knobs apply live: `MemoryService` and `GraphRecall` read them
from `ConfigService` on every recall, extraction and compaction, so a
persisted change is seen by the next one. The engine keys are read when the
llama context is built and say "restart"; `memory.observe_camera_events` is
read once by `main.cc` to decide whether the encounter consumer starts, and
`memory.embedding_preload` once at embedding init, so both say "restart" too.

The ten sampling keys apply live: `llm.temperature`, `llm.max_tokens`,
`llm.top_k`, `llm.top_p`, `llm.min_p`, the four penalties and `llm.seed`.
The set is one `LlmSampling` value inside `LlmService`
(`packages/clients/llm/src/llm/llm-service.hxx`), guarded by its own mutex
and copied once per generation. `main.cc` registers
`settings.onChange(... refreshSampling())`, so a change persisted through
`argus.settings.v1` is resolved again from `config.toml` and the next
generation samples with it: the request in flight keeps the values it started
with, the following turn reads the new ones. `resolveSampling()`
(`feature/llm/services/sampling-config.cc`) is the one reader. An absent key
takes the catalog's fallback (0.3, 256, 20, 0.8, 0, 64, 1.1, 0, 0, random
seed), so the fallback the app shows is what runs; before, an absent
`llm.temperature` ran at 0 and an absent `llm.max_tokens` at 16. A
hand-edited value is clamped to a sane range instead of reaching llama.cpp:
temperature 0–2, max tokens 16–4096, top-k 1–200, top-p 0.05–1, min-p 0–0.5,
penalty window 1–1024, repeat penalty 1–2, frequency and presence penalties
0–2, seed ≥ 0 (0 means a new seed per generation). The catalog's own ranges
are narrower where the app offers a slider (max tokens 32–2048).

`llm.min_p` is new (advanced, 0–0.5, 0 = off). When it is above 0 the chain
gains `llama_sampler_init_min_p` after top-p: it drops every token whose
probability is below that fraction of the best token's, which keeps a high
temperature from picking stray tokens. It is the knob the llama.cpp and
Liquid AI sampling guides pair with temperature; top-k and top-p stay as they
were. Each key carries its unit for the app (`tokens`, `entries`, `ms`,
`layers`, `threads`; unitless otherwise).

The engine keys still say "restart": they shape the llama context, which is
built once. Every spoken answer, which is what a voice call hears on every turn (the voice
session sends no temperature), uses `llm.temperature` and takes `llm.max_tokens`
as its budget.

The `settings` entry of `[rpc.callers]` is the only credential the settings
service accepts, and `LlmConfig::resolveRpc()` removes it from the chat
callers: the settings caller cannot chat and a chat caller cannot change
settings. The settings service rides the chat listener, so it is reachable
only when `rpc.address` and at least one other caller are set.

## Constrained generation (chat path)

- The chat DTO carries `tools` (default true), `grammar` (GBNF source, max 8
  KiB) and `grammar_required`. `tools:false` keeps the request on the direct
  engine path even when tools are registered, on either leg; raw JSON callers
  (argus-guard) use it to avoid the memory-tool preamble (~400 tokens of
  prefill on every call). A `grammar` installs a `llama_sampler_init_grammar`
  sampler rooted at `root`; when the grammar fails to compile,
  `grammar_required` aborts the generation instead of sampling free. Off-turn
  memory workers (`processCompact`/`processProfile`) pass an empty grammar
  explicitly.
- `user_id` (D4) scopes tool execution to the authenticated caller. Who
  may declare it is bounded per caller credential (see "Caller-bound
  identity" below).

## Encounter-closed consumer (camera guard feed)

With `[memory] observe_camera_events = true`, `main.cc` wires an
`EncounterClosedConsumer`
(`src/feature/encounter-closed/services/encounter-closed-consumer.cc`): a
durable JetStream consumer on `argus.guard.v1.encounter_closed`
(`ARGUS_GUARD`, durable `argus-llm-encounters`, `maxDeliver = 10`, poison
`Term` after 3 failed attempts). Each event is receipted in
`encounter_closed_inbox` (same states as argus-sync's
`notification_delivery_inbox`: `received`/`dispatched`/`conflict`/
`dead_lettered`, SHA-256 canonical fingerprint, conflict never captured) and
captured exactly once through the injected `capture`. Settled receipts older
than 14 days — twice the stream's 7-day max age, so nothing can redeliver
them — are deleted at most once a day after a capture, 500 at a time
through a partial index on `updated_at`; a full batch brings the next one a
minute closer, so a first run over a long backlog never holds the graph lock
or the loop for more than one bounded statement. A busy venue closes
thousands of encounters a day and the table kept them all. The capture
calls `observeSystemEvent` with the owner's scope —
never rule-parsed, never a fact. The owner resolves through
`IdentityClient::listNotifiableUsers` (first notifiable user, their language
for the summary line). The consumer stops before `memory.shutdown()` in the
teardown order, waiting in-flight handlers out. This is the only camera feed
long-term memory reads; the raw `object_detected` subscription stays an
episodic throttle beside it.

## Maintenance windows as episodes (2026-10, G3)

Beside the encounter consumer, `main.cc` wires a `MaintenanceEndedConsumer`
(`src/feature/encounter-closed/services/maintenance-ended-consumer.cc`) under
the same condition (`observe_camera_events`, a notifiable owner): a durable
JetStream consumer on `argus.guard.v1.maintenance_ended` (`ARGUS_GUARD`,
durable `argus-llm-maintenance`, `maxDeliver = 10`, poison `Term` after 3
failed attempts). Each window is receipted in the same guard-feed inbox,
`encounter_closed_inbox`, keyed by its own event id
(`guard-maintenance-ended:<id>`), with the same states, conflict rule and
purge (the encounter consumer's daily sweep settles both), and captured once
through `observeSystemEvent` with channel `maintenance`, the owner's scope and
language, at the window's end. The summary (`maintenance_episode::summary`)
names who declared it, the cameras and the place, and the day and the times
on the host's clock: "Mantenimiento de cámaras (visita del técnico o limpieza)
declarado por Luis, en las cámaras Patio y Garaje, el lunes 19 de octubre de
10:05 a 10:40 (35 min)." plus how it ended when it was not "ya terminé". The
episode tier finds it by its words, so "¿cuándo vino el técnico?" recalls it
(FTS on "tecnico"). `maintenance-ended-consumer-test` pins the parse, the
summary in both languages, the single capture, the poison path and the FTS
recall by owner scope. Not run in G3.

## The memory stack (Phase 4 step 7)

`packages/memory` and `packages/intent` became features of this service:
`src/feature/memory/` (`argus::memory`) and `src/feature/intent/`
(`argus::intent`, removed on 2026-10-12 with the model router it served;
"The decider: rules, Laya and explicit triggers"), with the schema at
`database/schema.sql`
and the provisioning in this service's `scripts/provision.sh`. Nothing linked
them from outside — the memory stack's only caller is the assistant turn beside
it, and the intent router's only caller was this feature's gate — so the two
`add_library` targets (`memory-core`, `memory-catalog`) and the standalone
project behind them bought a build graph nobody read. The history that made
them packages is below and stands: the stack is in process, and the reason
has not changed.

What the merge retired besides the folders: `memory-catalog` as a separate
target. Its justification was that linking memory would drag cnats into a
consumer that did not want it; the consumer is now the same binary that
already links `argus::lib::nats` for the encounter feed and the catalog
replica, so the split separated nothing. `src/memory/memory-dto.{hxx,cc}`
(the retired `/memory/v1` request bodies) went with it: zero includers
since f8-b3, and the only user of `lib/validation` and `lib/errors` in the
stack — both left the module's dependency list with it.
`tool-contracts.hxx` left `packages/clients/llm` for
`src/shared/vocabulary/` (§9.3 of the architecture plan): the tool runtime
and the memory feature both read it, which is what `src/shared/` is for.

- **The memory stack** (`MemoryService`, owned BY VALUE by the host — no
  singleton): `VecDb` (sqlite-vec), `SqliteGraph` (memory.db graph tables),
  `EntityResolver`, `GraphRecall`, `MemoryFormation`, extraction (the lexicon
  tier with its coverage floor, `CoveredLexiconExtractor`, and the temporal
  resolver) and embeddings (onnxruntime + unigram
  tokenizer). The stack is THE capacity of the brain: the boot gate fails
  loudly rather than running without memory. Feed handlers marshal onto the
  Drogon loop — cnats dispatcher threads must not block.
- **`memory.db`** with the memory tables VERBATIM from `database/schema.sql`,
  its `memory_vec` partitions (boot-applied, idempotent) and the four catalog
  replicas (`catalog_person`/`catalog_camera`/`catalog_zone`/
  `catalog_stream`). The schema file is configurable (`[memory] schema_file`,
  defaulting to this service's `database/schema.sql`) so the shared queries
  and `VecDb` apply the same DDL the service ships.
- **The catalog replica feed (Ruling BX)**: `argus.identity.v1.change`
  (`kind == "identity"` person rows, published through the `identity_change`
  sink) plus `argus.camera.v1.change` replay camera/person changes into the
  replicas. Since closure item 5 the replica holds one **durable JetStream
  consumer per stream** (`catalog_feed::defaults()`: `argus-llm-catalog-camera`
  on `ARGUS_CAMERA`, `argus-llm-catalog-identity` on `ARGUS_IDENTITY_CHANGE`).
  Both are ordered — one unacknowledged message at a time, 10 deliveries — so
  a redelivered row can never land after a newer one and regress the replica,
  and both use `deliverAll = true`, which decides where a consumer's cursor
  starts and so applies only when that consumer is first created — the broker
  refuses to change an existing consumer's policy, so a changed policy takes a
  new durable name; the cursor itself survives a restart because the bus
  creates the durable and then binds to it. The applies are idempotent upserts
  and deletes, and a failed statement throws, so the message is nak'd for
  redelivery instead of acked. The durables attach only after the snapshot
  fill below has run, from the same beginning advice (and whether the fill
  succeeded or not): the fill skips any table that already holds a row, so a
  replay landing first would leave a fresh replica with the few rows the
  retained window happened to carry instead of the whole catalog. Attached
  after it, the from-scratch replay is applied on top of the snapshot, and an
  idempotent upsert of an older row followed by its newer one converges on the
  snapshot's value. The plain wildcard subscription is gone: it existed for
  `camera_stream` rows, which nothing publishes today, and a future change to
  that table is camera's own, on the camera subject the replica already holds.
  Every apply runs on one `BlockingStrand` of the Light lane, so the arrival
  order is the stream order and the synchronous SQLite write no longer
  blocks IOLoop 0 (it used to run there under the graph mutex), and the host builds the replica whenever `nats.url` is set (not only
  when `connect()` succeeds) so a bus that comes up later still attaches. On
  boot, replica tables still empty get ONE snapshot fill from the typed rows
  the host fetched over `argus.identity.v1.ListPersons` and
  `argus.camera.v1.ListCatalog`; populated tables are never re-seeded. The fill
  runs even when the change feed never connects (`CatalogReplica::seedSnapshot`
  static entry — main.cc calls it when NATS is absent or failed), because
  otherwise a no-NATS boot would serve an empty catalog forever. Feed handlers
  accept two event shapes: the camera audit diff (`kind == "audit"`,
  field-level changes for one row) and the plain SocketEmitDto change shape
  (`{operation, option, info}`). Person and camera deletes tombstone (the
  source tables' `deleted_at` predicate); zone and stream rows are physical
  deletes, since their gazetteer query has no `deleted_at` filter.
- **The face index stays in the legacy**: `[memory] create_face_vec = false`
  skips `face_vec` creation (`ConfigService::hasKey` + a dual-shape read,
  since `getString` cannot surface TOML booleans); the key absent keeps the
  pre-cutover legacy behavior (create it).
- **The sqlite3_config ordering (Ruling BW)**: the read-only
  `[identity]`/`[camera]` snapshot clients must install BEFORE
  `loadConfigJson` — Drogon's first sqlite3 client creation performs
  `sqlite3_config(SQLITE_CONFIG_MULTITHREAD)` — while the memory stack's
  raw connections open only after the loop begins (the `deferStore`
  pattern). `main.cc`'s wiring preserves this ordering.
- **The chat port (Ruling BZ)**: the production chat leg is
  `InProcessMemoryChat` — the `IMemoryChat` port over the in-process
  `LlmService`, no wire hop. `WireMemoryChat` survives header-only because
  the back-pressure suite exercises the port over the wire shape.
  Back-pressure is the bounded work queue (`[memory] queue_bound`, default
  64): at the bound, a new job drops with a WARN. The worker's jobs are
  embed, compact, rebuild and profile; the extract job is gone with the
  off-turn extractor.
- **Models stay in the shared `models/` tree** — `models/memory/` for the
  embeddings, `models/decide/` and `models/extract/` for the Laya and GLiNER
  bundles, never copied — and arrive through this service's
  `scripts/provision.sh`, beside the LLM weights.
- **No migrations**: `database/schema.sql` is additive and applied
  idempotently at boot; `argus.db` is never touched, and `memoryDbFile()`
  refuses it outright.
- **The mobile app never talks to the memory stack**; no gateway routing, no
  app-facing contract. Identity change events are only the catalog-replica
  feed.

### Why the memory capacity exists

F4-6 of the `migracion-microservicios` plan extracted the memory stack out of
the legacy monolith (Rulings BW-CA). `argus-memory` was a capacity-only
sibling service (like argus-vlm): the voice session never talked to it; the
CONSUMERS are the background workers (memory formation, compaction, profiling,
procedures) and, since f8-b3, the LLM's own turn.

### f8-b3: the service becomes a package (2026-09-08)

The user's ruling: the LLM's memory should not be a wire hop away — the
assistant turn needs the memory tools in process, and the memory worker
gets LlmService as a direct call. What died with the process: `src/main.cc`,
the `/memory/v1/*` HTTP surface and its controller, port 7033 with its compose
service and config template, and the remote wire adapter (`memory-remote` /
`RemoteMemoryServiceAdapter`) that existed for the retired legacy's Ruling BY
cutover gate. The database key inverted in the same move: memory no longer
rides the host's `[database] file`; it owns `[memory] db_file` (fallback
`[database] file`, final default `database/memory.db`).

Step 7 carries that ruling to its end — the package is a feature now, and the
`memory-catalog` target it introduced to keep cnats out of an unwilling
consumer is gone, because this binary is the only consumer and it wants the
bus.

### The mounts

The compose mounts the shared `models/` subpaths (`models/llm`,
`models/memory`, `models/extract` and `models/decide`, all read-only) and the
memory database into this service's working directory — the GGUF weights, the
ONNX artifacts, the two bundles and `memory.db` are read relative to the
config keys.

### Stranger isolation (camera guard)

The memory stack serves authenticated users. A person the camera sees is
foreign to the system: their speech, intents and recall requests must never
become facts, preferences, reminders or episodes. `observeSystemEvent` and
every tool handler reject
`userId <= 0`; camera events are written only as episodes through
`observeSystemEvent` with the owner's scope and never pass through the rule
parser, so a stranger can never teach the assistant anything. See
`docs/history/plans/camera-guard-automation-plan.md` (6b).

### Camera event episodes

With `[memory] observe_camera_events` argus-llm subscribes
`argus.camera.v1.object_detected` and records one episode per camera+rule (120 s
throttle) through `observeSystemEvent`, scoped to the first notifiable user and
carrying the person ids as entities. Episodes only: no rule parsing, no facts,
and never anything a camera-only person said (see stranger isolation).

### Encounter-closed inbox (camera guard feed)

`encounter_closed_inbox` receipts the durable `argus.guard.v1.encounter_closed`
feed in `memory.db` (`MemoryGraphRepository::claimEncounterClosed` and
settles): `received` replays, `dispatched` drops redeliveries, `conflict`
(same id, different canonical fingerprint) never captures, `dead_lettered`
parks poison, and an unknown persisted status fails closed to `dead_lettered`.
The inbox DDL is additive and `SqliteGraph::open` now always applies the
schema file, so existing stores gain the table without losing a row. Each
receipt captures exactly one owner-scoped episode through `observeSystemEvent`
— this is the only camera feed that survives as long-term memory.

## The decider: rules, Laya and explicit triggers

`turn::kDeciderIds` is `{"rules", "triggers", "laya"}`
(`src/feature/llm/services/turn/decider.hxx`). The turn decides with
`FirstOf{RuleDecider, TriggerDecider, LayaDecider}` (`productionOrder`, ruled
2026-10-12):

1. `RuleDecider` runs first: the module commands (`tools/module-command.cc`)
   and the app commands (`appCommandFor`), exact by construction.
2. `TriggerDecider` (`deciders.{hxx,cc}`) runs second. It runs
   `RuleParser::parse` over the phrase catalog ("recuerda que", "remind me
   that", ...) and routes `memory.remember`, or `memory.remind` when the
   utterance asks to be reminded (`recuérdame`, `avísame`, `remind me`, an
   alarm or a timer) **and** names one non-recurring instant
   (`TemporalResolver`) or a recurrence ("Recurring reminders from the
   assistant" below); "apunta que el mecánico llega el martes" is a fact.
   Its candidate is exact, at confidence 1.0, with the source
   `memory_save, trigger` or `reminder_set, trigger`, so it acts without a
   second signal.
3. `LayaDecider` runs last: the Laya model reads the utterance against its
   bundle's own questions and proposes one offered tool with a calibrated
   confidence, a runner-up and `now` ("Laya and GLiNER: the turn's engines").
   It acts at its bundle's operating point (the pilot: 0.77) and, on a write,
   only with `now` at or above the bundle's `now` (0.5); below the act
   threshold the verdict is Pass and the LLM only speaks.

Why the triggers come before Laya. With Laya first, Laya answered every turn it
read, so an explicit "recuerda que…" that the pilot scored 0.55–0.74 was passed
over and never reached the triggers: on `tests/fixtures/intent/eval-production.tsv`
(61 real turns, 58 of them memory) the shipped order acted on 0%, and the prod
`llm-tier-eval` scored 0 precision and 0 recall on every memory tool. Triggers
first, the 35 explicit-trigger turns route through them (0 wrong ACT) and the
other 26 reach Laya (`llm-turn-engines-test`, "the production order…"). The
act threshold stays the bundle's calibrated one; lowering it to 0.70 was
measured (traffic 9.8% ACT with one wrong ACT) and rejected.

`installTurnEngines` (`services/turn-engines.{hxx,cc}`) composes the stack, and
`main.cc` and the evals call the same function. When the Laya bundle loads, the
stack and the write guard's witnesses are `{rules, triggers, laya}`; when no
Laya bundle loads, the adapter's default stack stays `{rules, triggers}` with
witnesses `{rules, triggers}`. A Laya model whose bundle validated but whose
session is not ready (it failed to open or cannot run) abstains, and the turn
has already heard the rules and the triggers.

**Degradation is a contract.** No decider bundle on disk, a bundle that fails
validation, a label no tool serves, or a model that does not open: one log line
names the bundle and why (`the Laya decider is off: ...; the turn runs on rules
and explicit triggers`), and from then on only the rule commands and the
explicit triggers route. Nothing fails the turn. The trigger tier exists so
that an explicit ask to write ("recuerda que el código del portón es 1234") is
honoured even with no model: the user asked for the write in so many words, and
no score is needed to believe it.

The owner's decisions of 2026-10-12 and 2026-10-16 made Laya and GLiNER the
only engines and took the engines they replaced, with their artifacts and
configure-time pins, from the build and the provisioning. The replaced
router's explicit-trigger tier survives as `TriggerDecider`, and
`tests/fixtures/intent/eval-production.tsv` (real traffic) stays;
`decider-eval.py --traffic` reads it.

## Phase 4 step 9: config resolution into `src/config/` (D20)

The five things `main.cc` resolved inline are `src/config/llm-config.{hxx,cc}`
(`argus::llm-config`): `resolveListener()` (`ListenerConfig::resolve(7032)`),
`resolveRpc()` (`rpc.address` plus the `rpc.callers` pairs),
`resolveIdentity()` (`identity.target`/`identity.rpc_secret`, read by the
catalog-snapshot fill and by the encounter consumer),
`resolveCameraTarget()` (`camera.grpc_target`) and `resolveMemory()`
(`memory.observe_camera_events`). `main.cc` keeps `config.toml` loading,
`drogonConfig` and the `nats.url` gate on the optional bus.

## Tools over MCP (2026-10, context plan section 5)

One assistant serves the whole system; the modules are tool providers and
argus-llm is the MCP client that aggregates them (`packages/lib/mcp`,
`packages/contracts/mcp`, `packages/clients/mcp`). Why MCP and why the
stateless 2026-07-28 revision: every provider is a request in and a result out,
nothing remembered between two calls, which a unary RPC carries without a
session to lose on a restart.

- **Providers.** The core tools are served in process by a provider named
  `llm` over `LocalTransport` (`core-tools.cc`): `memory.remember`,
  `memory.recall`, `memory.forget` (destructive), `memory.remind`,
  `reminder.list` and `app.open`. The modules' tools come from their own
  services over `argus.mcp.v1.Mcp/Rpc` on the provider's existing internal gRPC
  listener: camera `app.show_camera`, guard `app.set_guard_mode`, productivity
  `calendar.*`, `project.*` and `task.*`, settings `modules.*`. Every tool
  declares `argus/module` and `argus/capability` in its `_meta`; the provider
  is the authority and checks them again (`tool_gate::capabilities()`), the
  model never decides one.
- **Wiring.** `LlmConfig::resolveToolProviders()` reads `[camera]
  grpc_target/credential`, `[guard] target/credential`, `[productivity]
  grpc_target/credential` and `[modules] target/credential` (the settings
  listener); a provider whose target is empty or whose credential is not a
  paired secret is skipped, so a partial install serves the tools it has. Each
  provider's credential is its own (`[rpc.callers] llm` on guard and settings,
  `[grpc] caller_llm` on camera and productivity), never a fleet secret;
  `scripts/lib/common.sh` pairs them.
- **`ToolDirectory`.** One thread reads every provider at boot, retries a
  failed one with a 2 s to 30 s backoff, refreshes all of them every five
  minutes and whenever the module feed says the enabled set changed
  (`requestRefresh`). A provider that answered keeps its last list while it is
  unreachable and a call to it answers `unavailable` in the user's language
  (`toolUnreachable`); one that never answered contributes nothing. The registry
  merges local and remote tools sorted by name, so the declarations, and with
  them the cached prompt prefix, are the same in every process; a name declared
  twice keeps its first declaration.
- **Per turn.** The controller builds a `ToolAudience{role, modules}` (the
  caller's role from the wire, the module snapshot of the gate) and
  `ToolExecutor::offered` returns the tools whose capability the role holds
  ignoring modules: a tool of a module that is off is still offered, because a
  decider must be able to name it so the user can be offered the module. The
  offered tools are what the deciders may name; none of them is declared to the
  model. An absent or unknown role holds nothing, so such a turn gets the
  direct engine path.
- **Execution order** (`tool-executor.cc`): unknown tool, permission denied,
  date-time normalization, schema validation, module inactive, grounding,
  handler. Permission precedes the schema so an unpermitted caller learns
  nothing of a tool's shape; the schema precedes the module check so a vague
  call is corrected before an offer is made.
- **A tool of a module that is off** answers `module_inactive`. The result
  carries the module's own facts (what it is and its examples, `module-offer.cc`:
  `data.facts`), the grounding records an offer for the user, and the speaker is
  told that the module is off and what could be offered; the user's yes on the
  next turn becomes `modules.enable` for the Owner and `modules.request` for
  anyone else, run by the turn itself ("The turn: decide, fill, run, speak").
  The ledger keeps the original request as a pending intent (below), except for
  `app.*` tools, which only ever act on the live app, and destructive ones.
- **Turning a module on or asking for it is never a model's whim.**
  `modules.enable` and `modules.request` run only when the user's own
  utterance asked for it or is an affirmative reply, on a later turn, to an
  offer made for that module (`spoken-intent.cc`, refusal code
  `needs_spoken_yes`). A destructive tool whose schema carries a `confirmation`
  argument (`modules.disable`, `calendar.cancel_event`) is two calls: the first
  returns the preview and a one-use six-character token
  (`argus::mcp::ConfirmationLedger`, 120 s, bound to user, tool and target), the
  second must carry it and the runtime refuses it unless the current utterance
  is an affirmation from a later turn than the preview. `memory.forget` is also
  destructive but keeps its older rule, enforced by the memory service: it runs
  only when the utterance is a forget request. Purging a module's data is not a
  tool at all: `modules.open_purge_screen` only emits `app.open {screen:
  "modules", module}`.
- **Grounding** (`tool-grounding.cc`) holds the rules above and the older one:
  `app.set_guard_mode` to any mode but armed runs only when the user's own
  utterance names that mode or is that command (`needs_spoken_words`).
- **Date-times.** A property with `format: "date-time"` is the one place a
  time travels. `time-arguments.cc` rewrites it before validation: a required
  one is first read from the user's own utterance (`call_time`, the resolver
  `memory.remind` always used), which wins over what the model wrote; otherwise
  the model's ISO string (`iso_time::parse`) or natural phrase ("mañana a las
  tres") is used; the result is ISO with the host's offset, the only form the
  providers parse. When nothing resolves the schema check refuses the missing
  argument. `LfmAdapter::clockNote` is the one place `asksAboutTime` is consulted:
  a turn that asks for the date or the time carries the reference line
  ("Referencia, menciónala solo si te preguntan la fecha o la hora: ..."),
  inserted as its own system note before the last user message, never into the
  persona, so the cached prefix stays byte-identical. The tool turn
  (`ToolChatInput::clock`, framed by `LfmAdapter::spokenMessages`) and the
  tool-less turn (`LfmAdapter::chatPlain`) both read it from there.
- **Reminders.** `memory.remind` keeps scheduling the call exactly as before
  and, for a reminder with a time, also writes the user's row through
  `ProductivityReminderClient` (`infra/productivity-reminder-rows.cc`): the
  target is always the caller (`JwtContext.sub` as the wire declares it),
  whatever the model puts in the arguments, and another user's row is a 404 in
  productivity; there is no second write path and no Owner override.
  `reminder.list` reads the caller's own rows the same way. A reminder with no
  time creates no row. A recurring reminder carries a `repeat` argument and
  writes series rows ("Recurring reminders from the assistant").
- **App actions.** A provider that wants the app to do something returns
  `argus/appAction {name, arguments}`; `remote-tool.cc` hands it to
  `ToolContext::emitAction`, which the controller turns into a `ClientAction`
  on the stream, ordered with the text tokens. Without an emitter the tool
  refuses, so a non-call caller never hears that something happened when
  nothing did. `app.show_camera` takes a `camera` (id or words, resolved by
  `text_norm::matchName`) and a `view` (`live` or `snapshot`);
  `app.set_guard_mode` takes an optional `environment` (guard environments,
  2026-10): an exact or one-word match acts, several matches ask which, none
  lists the places, and no `environment` keeps the old meaning, every
  environment.
- **The model is told nothing about the tools.** The first end-to-end measurement
  of the assistant (789 judged cases, 2026-10-06) found that the model made no
  call at all to `calendar.*`, `task.*`, `project.*`, `modules.*` or
  `reminder.list` and claimed work it had not done, and a prompt sentence per
  tool was tried first (`argus/policy`, `ToolSpec::policy`, `tool_policy`). The
  owner's decision replaced it: the model never sees a tool, a declaration, a
  policy sentence or a call, and those lines, the `argus/policy` `_meta` key
  and the loop that parsed calls are gone.
- **A reply that claims what no tool did never reaches the user**
  (`reply-claims.cc`, phrase tables in `reply-claim-lexicon.cc`, one table per
  language: Spanish with its Peruvian colloquialisms, English). A claim is a
  first-person completion ("agendé", "he guardado", "ya te lo anoté", "I've
  scheduled", "he ajustado", "ajusté la calefacción"), a present-tense performative with an
  object ("creo una reunión", "agendo la cita"), a "quedó agendada", a saved-note
  statement ("esa información está guardada"), a promise to act now ("claro, puedo
  activar la agenda ahora"), and, only when the user's own words
  asked for something, a bare marker ("listo", "confirmado", "hecho", "done",
  "all set"). Questions, offers, plans, conditions and negations ("¿quieres que
  lo agende?", "voy a agendar", "no lo agendé", "cuando lo agende te aviso",
  "creo que mañana llueve") are not claims. A claim is legitimate when a tool
  that writes (not read-only, or an app action) succeeded in the turn; a failed
  tool or a read-only success does not make it true. The read-only list is
  `turn/tool-effects.json`, so a turn whose only successful actions are reads
  (a listing, `app.open`, `app.show_camera`) may say what the read did and
  nothing more: after a screen was opened or a camera shown ("abrí la agenda",
  "te muestro la cámara", "I opened the agenda") the opening phrases and the
  bare markers ("listo", "done") stay allowed, and a claim of a state change
  is cut: a participle of "activado", "desactivado", "encendido", "apagado",
  "cambiado", "configurado" (also "turned on", "switched off", "enabled",
  "changed", "configured", ...) with no copula before it, or a phrase that
  says it just happened ("ya está activado", "quedó activado"). A description
  is left alone ("las cámaras que están activas", "your cameras are online",
  "la alarma sigue activada"): a copula within two words before the
  participle ("está", "están", "sigue", "queda", "is", "are", "remain") makes
  it a state, not an act. A state-change word is judged only when the user's
  own words asked for something, and each reply is judged only against the
  lexicon of the turn's own language, so an English "active", "quite" or
  "complete" is not read as a Spanish performative. Where it applies: in the speak
  stage (`LfmAdapter::chatTurn`) every streamed reply passes `ClaimGate`, which
  lets a sentence through only when it has ended and judges it first, so a clean
  reply still arrives sentence by sentence and a claim is replaced by the honest
  reply mid-stream (what was already spoken stays), and a synchronous reply is
  judged whole; what counts as a write is what the turn really performed (a
  destructive tool's preview is not a write); a trailing generic offer is
  dropped by `withoutTrailingOffer` beside the streaming `OfferStripGate`, and
  both fire only on a sentence that *is* the offer (`standaloneOffer`: after one
  leading interjection the sentence must begin with an offer phrase and every
  word after it must be a conversational particle — "más", "hoy", "ahora" /
  "else", "you", "today", "now", "further"; `kOfferPaddingEs`/`kOfferPaddingEn`,
  the offer phrases themselves untouched), so a sentence that merely carries an
  offer behind a comma ("Apunté la leche, ¿necesitas algo más?"), or follows it
  with a content word ("¿En qué puedo ayudarte con la cámara?", "Do you need
  anything for the trip?"), keeps every word; the streaming gate releases every
  sentence as soon as it
  completes and holds only one that could still be the trailing offer. An
  assistant turn with no tools offered at all (`toolsEnabled` but the role holds
  none) runs the same composition through `LfmAdapter::chatPlain`/
  `chatPlainStream`, the controller's path whenever `requestTools` comes back
  empty: the clock note, the same check (`withoutFalseClaims`, `ClaimGate`) and
  the same strip. Requests that did not
  enable tools (summaries, extraction) still get the clock note and the strip
  but are never judged for claims. The app-action claim check that
  existed before (`claimsAppAction`) still runs for turns that ask for an app
  action.
- **The decider stack is one seam.** Rules, then the explicit triggers, then
  Laya ("The decider: rules, Laya and explicit triggers"); a decision reaches the turn as a candidate the turn judges with
  its own thresholds, and a decided call goes through the same executor, so
  permissions, the module check and grounding apply to it as to any other.

`tests/unit/llm-tool-runtime-test.cc`, `llm-turn-flow-test.cc` (the turn and the
claim guard on its sync and streaming paths), `llm-reply-claims-test.cc` (the
phrase tables against claims and non-claims in es, Peruvian es and en, the
request detector, the opened-screen mode, state changes against descriptions,
the language restriction, `ClaimGate`, `withoutFalseClaims`), `llm-app-command-test.cc`,
`llm-tool-providers-test.cc` (provider aggregation, per-turn filtering,
unreachable providers, the core server acting for the declared caller),
`llm-spoken-intent-test.cc` and `llm-time-arguments-test.cc` pin the above;
`memory-reminder-test.cc` pins the reminder rows, among them that the tool
cannot create or change a reminder for another user whatever the arguments.

## The turn: decide, fill, run, speak (2026-10-06)

The owner's decision of 2026-10-06: the LLM is only the conversational layer. It is never told a
tool exists, never writes a call and never chooses one. A 1.2B model given the whole tool list
called none of the agenda, task, project and module tools in 789 judged cases and claimed the work in
prose; a larger model was not an option on this hardware. Every turn that carries tools
(`LfmAdapter::chatWithTools` and `chatWithToolsStream`, both `chatTurn`) now runs four stages, the
first three without a generation (decide, which includes the judgement of the decision, fill, run, speak):

1. **Decide** (`services/turn/decider.hxx`, `deciders.{hxx,cc}`, `turn-flow.cc`). A `Decider` reads
   the utterance, the tools this turn offers and the module snapshot and answers with at most one
   `Candidate`: the tool, the arguments it already knows, the slots it wants filled (`fill`), a
   `confidence`, its own `id()` (`rules`, `triggers`, `laya`), the `runnerUp` (another tool and its own
   confidence), `exact` (the decision came from words that name the tool, not from a score) and
   `confident` (the decider's own gate said act). `RuleDecider` is `appCommandFor` and the module
   command rules (`tools/module-command.cc`); `LayaDecider` runs the Laya bundle and names any
   offered tool its label set maps, with the runner-up label's tool as `runnerUp` and `now` from the
   same forward pass; `TriggerDecider` is the explicit-trigger rule tier that answers when Laya is
   not ready, naming `memory.remember` or `memory.remind` with the user's own words; `FirstOf` stacks
   deciders, so the rules, the trigger tier and a learned model are interchangeable and stackable
   behind the same interface. Another learned model is one more `Decider`; nothing else changes.
   **The judgement.** `DecisionPolicy{act, ask, margin}` is data, per decider: `[decide]` is the default and
   `[decide.<id>]` overrides it (`LlmConfig::resolveDecision`, `PolicySet`, main.cc composes them).
   `judge` answers **Pass** below `ask` (plain conversation; if the user asked for something, the
   speaker is told nothing matched and that it must not say it did anything), **Choose** when the
   runner-up is itself above `ask` and closer than `margin`, **Act** at or above `act`, **Ask**
   between. A decider that does not trust its own decision (`confident = false`) never reaches Act.
   A tool that writes or destroys acts only when the candidate is `exact` or a second signal agrees.
   The router's four memory tools earn that exactness from the arbitration itself: the rules read
   the words, the model reads them, and the arbitration decides which reading stands (a trigger, an
   agreement at `kAgreeFloor`, or a confident score that outranks the rules), so a decision it
   reaches is the tier's own second answer and the family's precision is what `fast.metrics` gates
   per class. Before 2026-10-16 the router left every model-decided memory write to the witnesses,
   which never name one, so the turn asked instead of saving and only the rule tier executed:
   `tool.memory.remember.recall` 0.227848 (18 of 79), `memory.remind` 0.013333 (1 of 75),
   `memory.forget` 0.156250 (5 of 32), `selection.memory.accuracy` 0.241379, while the read-only
   `memory.recall` (no second signal needed) passed at 0.520. With the arbitration as the second
   answer the same prod build measures 0.569620, 0.773333, 0.625000 and 0.620690, every memory
   precision at 0.90 or above, `group.memory.falseActionRate` 0.007663 and `selection.accuracy`
   0.642504 to 0.802306.
   What is a write is data: the read-only tools are the list in `turn/tool-effects.json` (compiled
   into `tool-effects.hxx` by CMake; `calendar.list_events`, `task.list`, `project.list`,
   `modules.list`, `modules.explain`, `reminder.list`, `memory.recall`, `app.open`,
   `app.show_camera`), every other tool is a write, whatever it says about itself, so
   `app.set_guard_mode` waits. The second signal is, in order: `useSecondSignal`'s function when one
   is installed (it receives the candidate, the words, the language and the assistant's previous
   turn); else the decider's own `now` (`Candidate::now`, the probability that the user asks to do it
   now, from the same forward pass as the choice) against its policy's `nowMin`, when `nowMin > 0`,
   and a decider that sends no `now` is asked; else `TurnFlow::useWitnesses`, the other deciders,
   one of which must name the same tool at its own Act level. Without a second signal the turn
   Asks. Demotion is configuration: `[decide.<id>] witness_only = true` makes that
   decider's word a second signal only, so it can never reach Act alone on a write or destroy tool
   (it still acts on reads), which is how the rule paths step aside once a learned decider covers
   their families. Every decision is counted (`DecisionTally`, by decider, exactness, tool family,
   language and what became of it: act, ask, choose, pass or guard) and written to the log as
   `turn-decision decider=... exact=... family=... tool=... lang=... verdict=...` without the
   utterance; `GET /llm/v1/config` returns the counts as `decisions`.
2. **Slots** (`slots.{hxx,cc}`, `slot-lexicon.cc`, `gliner-extractor.{hxx,cc}`). The fields to fill are
   the candidate's `fill` plus the schema's required properties not yet present. A date-time goes
   through the same normalization the executor uses (`call_time` over the user's words, ISO
   accepted); a module is matched against the snapshot's names; a title or name comes from
   the text engine: `turn::GlinerExtractor`, a span of the user's own utterance scored above the
   bundle's `pairThreshold`, and, when GLiNER's bundle did not load, its model cannot run or it
   finds no span, `RuleText` (what is left of the sentence once verb, noun and time are taken out).
   A slot nothing fills is never guessed: the system asks it in its own words
   (`turn-texts.cc`, es and en, per tool and slot) without a generation, keeps a `Pending` of kind
   `Slot` for five minutes, treats the next utterance as the answer (unless it is itself a command
   the deciders Act on), asks again once and gives up on the third failure.
3. **Run** through `ToolExecutor` exactly as before: capability filter, schema, module gate, grounding,
   then the provider. The executor's refusal, the module-off offer and a destructive tool's preview
   are results like any other.
4. **Speak.** The model receives the caller's history untouched and, when something was decided, one
   final system note with the findings: what was
   done (the tool's own words: the stored title and time are in it), what was refused, what is waiting
   for confirmation (never the code), what module is off and what can be offered, that the user said
   no, or that nothing matched. `toolCallsAllowed` is false, there is no tool list in the prompt and no
   call syntax anywhere. The reply guard of the claims section still stands after it: a claim with no
   write behind it is replaced. A turn that ended in a question is answered by the system with the
   question itself, no generation.

**Pendings** are what makes a turn answerable by "yes": the executor keeps the latest destructive
preview (with its code) and the latest module offer per user (`ToolGrounding`), `PendingTurns` keeps
the latest slot question, intent confirmation or two-way choice per user. The newest of them is the one
the next utterance answers. A yes runs it (a preview is re-issued with the stored code, an offer
becomes `modules.enable` for the Owner and `modules.request` for anyone else, a confirmation or a
choice runs the held candidate with the words the user first said, because a bare "sí" cannot ground
`app.set_guard_mode`, `modules.enable` or a memory write); "the other one" (`slot_lexicon::Group::Other`)
runs the runner-up of a choice; a no drops everything and the speaker is told so; anything else drops
the pending and is decided as a new utterance. A caller with no user id keeps no pending. The preview's
code reaches the grounding as
structured data (`data.confirmation`), never in the text the speaker reads.

**A task that needs a project** is held, not lost. When `task.create` (or any tool) refuses with
`project_needed`, `ambiguous_project` or `unknown_project` and names the user's projects in
`data.projects`, the turn drops the refusal, keeps a `Project` pending with the task as it was
(arguments included) and asks "¿En cuál proyecto va? Casa, Trabajo o Viaje." itself. The next
utterance is read for that slot only: the project whose every word was said wins (the longest if
several), so "el de la casa" completes the held task in Casa; an answer that names none or several
asks again once and then gives up; "no" drops the task. "Ninguno", "crea uno" or "none of them" never
creates anything silently: with a name given ("crea uno llamado Hogar", "se llama Hogar") or asked
("¿Cómo se llama el proyecto nuevo?") the turn offers "¿Creo el proyecto «Hogar» y anoto la tarea
ahí?", and only on a yes runs `project.create` and then the held task with that project. With no
project at all (`no_projects`) the first question is the name of the new project. There is no
default project, and a provider that does not name the projects leaves its own refusal to be said.

The questions and the notes are one thing now: the acts. `speech-render.cc` composes one short
line per act per language — the only copy the backend owns, and the user never receives it —
prefixed with `Nota de la app:` / `App note:`, then the context block. Each line is the fact
plus the one thing to do with it ("falta la fecha y hora; pregunta solo por eso en una sola
pregunta corta."), because the 1.2B model follows the nearest cue and a bare fact never says
what to do; the known-slot parenthetical and the `label: value` shape are gone. It is the
situation the turn's last user message carries after the user's words (a `DatePart` is resolved
to its spoken surface against `now` at render time); no act JSON survives. `speechOf` places the
act on the outcome, so a unit test asserts the exact act a path reaches and never a sentence.
`turn-texts.{hxx,cc}` is gone.

Configuration (`config.toml.example`, deploy template): `[decide]` with `act`, `ask` and `margin` (0.90 / 0.60 /
0.10 in the template), the policy of a decider whose bundle declares none (Laya's own thresholds come from its
bundle), and `[decide.rules]` with `witness_only`; a `[decide.<id>]` section may override `act`, `ask` and `margin`
for one decider. Absent or invalid keys leave the compiled default policy (`act = ask = 0.90`, no margin).

`tests/unit/llm-turn-flow-test.cc` pins the stages: the judge and the per-decider policies, deciders
that stack and abstain, the rule decider offering only offered tools, the trigger decider routing only an
explicit trigger with the words as its argument, the title and time stored and spoken back, a missing time, title and module asked and answered,
a new command replacing a question, the ask band and the choice (yes, the other one, no), the write
guard with and without a second signal, previews that never leak the code, confirmed with the stored
code and refused after a no, a module offered and then enabled or requested by role, an anonymous
caller, the speak prompt with no tool list, a question answered without a generation (sync and
streamed), the claim guard after a failed tool, a preview and a read, and the prefill prefix.
`llm-module-command-test.cc` pins the rule tier (tuning numbers, not a held-out result); the sealed
set is judged by `tests/eval/decider-eval.py`.

## The refusal leg (2026-10-17, RF)

A request that targets data the caller cannot read, or an action that does not
exist, never reaches generation as a subject to talk about: it is decided by
rules and rendered from a fixed line, so the model can neither invent a state
nor answer for someone else. `turn/refusal-rules.{hxx,cc}` is that rule tier,
consulted by `TurnFlow::run` before any decider, and the decision comes from the
utterance, the caller's role and the module snapshot alone — never from the LLM.

Four reasons, one per surface: `no_relay` (a message to pass to somebody: the
messenger of F8/F9 does not exist yet, so the turn says it cannot pass it and
offers a reminder instead), `private_person` (another person's location,
presence or private rows for a caller who is not the Owner; presence stays the
Owner's alone), `module_off` (a security question while `surveillance` is off)
and `no_action` (a reminder to postpone, which no tool can do). The person named
in the utterance travels with the act, so the line names them and never repeats
what was to be said ("dile a María que estoy en camino" cannot become "voy en
camino" in Argus's mouth). `speech-render.cc::refusalLine` holds the four lines
per language (es, es-PE and en share one Spanish set) and `LfmAdapter::speakAct`
returns them before it touches the engine: a deterministic refusal costs no
generation, and a refusal a tool really produced (`not_found`,
`project_create_unavailable`) still goes through the model as before.

**The verbless polarity ask.** "la privacidad de la cámara del garaje" names a
target with no verb and no on/off word: the turn asks instead of guessing, with
one fixed question (`slotQuestion`, `privacy` slot, "¿La activo o la quito?"),
and no pending is held for it. The false-state guard is the net behind it.

**The belts.** `speech::impersonates` is a `GuardVerdict::Impersonated`, a hard
failure: a first-person claim of being somebody of the house, whether it
identifies itself ("soy María", "this is David") or opens with a vocative
("María, I'm on my way!"). `speech::withoutUngroundedClaims` drops sentences
that talk about the app's own instructions or notes, and any person-location
claim whose place is not in the turn's context block, replacing a dropped
location with the refusal line and an otherwise emptied reply with the honest
"no data" line; the streaming gate (`reply_claims::ClaimGate`, `scrub` and
`replacement`) applies the same sentence by sentence. An ask turn may not report
done: `reply_claims::claimsCompletion` reads completion markers as a claim on a
turn whose act is a question, whatever the user asked for, which is what
`call-faithfulness-eval --render-acts` measures (its `retryShare` ceiling of
0.05 is now wired into ctest as `call-faithfulness-render-acts`).

**Who is in a room, whether someone is home (G3).** Every utterance the
whereabouts rule reads (below, "Where is someone") is `private_person` for a
caller who is not the Owner, so "¿quién está en la sala?", "¿está David en
casa?" and "is Ana home?" are refused deterministically like "¿dónde está
David?", the person named. "¿dónde está la cámara del patio?" is not a person:
the where-person refusal skips a camera target, and the rule never reads one.

**The act's own words ground the reply.** `LfmAdapter::speakAct` grounds the
location scrub on the act's situation (the tool's fact plus the context
block), not on the context block alone: a place the tool itself answered
("David Acme está en la sala") is not cut as unsupported, while a place no
tool and no context named still is.

`tests/unit/llm-refusal-rules-test.cc` pins the rules and the lines; the guard
tests pin impersonation, the location scrub and the ask turn. The pack that
scores it end to end is `context-scenarios-eval` (`noRefusal`, `privacyLeaks`,
`falseRelays` — the last one now counts an impersonation reply too).

## The duress screen (2026-10-13, F4)

The household can set a duress phrase in guard (`services/guard/CONTEXT.md`,
"The duress phrase"). When a household member says it to Argus, guard raises
the same silent alert its duress code raises, and the speaker hears an ordinary
answer. The detection lives here, at the turn, and it is not a tool. The LLM
never chooses a tool, is never told this exists, and no route or MCP tool for
it is declared.

**A matched utterance never travels on as heard (the reviewer's F4 gap, closed
2026-10-13).** The words are replaced by a neutral placeholder, `…`
(`llm_speech::kWithheldUtterance`, `packages/contracts/llm/src/llm/llm-speech.hxx`,
the one spelling llm and voice share), before anything downstream sees them:
the history the call keeps, the frame the app shows, the logs, the turn's
decision, its tools, the memory graph and the model's prompt. The placeholder
is whole, never a sentence with words cut out of it, so nothing around the
phrase hints at what was removed. **No model answers a withheld turn** (the
reviewer's ruling on the reply, 2026-10-13): voice answers it through the one
path it takes for every turn without words, a "didn't catch that" line from a
small per-language set rotated by the call's seed
(`services/voice/CONTEXT.md`, "One reply for every turn without words"), so a
withheld turn and an unintelligible one cannot be told apart.

- **Voice asks first.** `services/voice` calls `POST /llm/v1/screen` (gRPC
  `argus.llm.v1.Chat/Screen`, the SDK's `LlmClient::screen`) the moment STT
  answers, before it logs, sends the `stt` frame, stores the user entry,
  extracts a name, reacts or offers anything. From then on it uses the text
  the screen returned. The route answers `{text}`: the same words, or the
  placeholder. Like the chat routes, it lets only the `voice` caller declare a
  user and a role (`boundToCredential`/`boundToCaller`); anyone else is never
  screened. A screen that fails leaves voice with the words as heard, and the
  turn below still withholds them from everything this service does with
  them.
- **The turn screens again.** `LlmController::chatSync` and `chatStreamSync`,
  the entry both legs share, pass the request through `screenTurn` before
  anything is decided. It answers `ScreenedRequest {request, withheld}`: the
  last user message (as `LfmAdapter::spokenText` reads it) replaced by the
  placeholder when the screen withholds it, every other message untouched.
  Prefill requests, callers without a user id or a known role, and a process
  with no screen installed pass through as they are.
- **A withheld turn ends at once.** When `screenTurn` withholds, the turn
  decides nothing, runs no tool and prompts no model: `chatStreamSync` sets
  the `speech` marker `unheard` (`llm_speech::kUnheard`) and ends the stream
  with its sentinel alone, and `chatSync` answers an empty text with the same
  marker. The marker reaches voice on the HTTP sentinel's `speech` key and on
  `ChatToken.speech`, and voice corrects the transcript it showed and answers
  the turn as unheard. Voice only meets this when its own screen did not
  answer, or when the key changed between the two screens.
- **Who.** `DuressScreen` (`feature/duress`, module `argus::duress`) screens
  only a speaker who holds `safety.duress` under the current module snapshot
  (Owner, Resident, surveillance on). Guard checks again.
- **On the caller's thread, bounded.** The screen folds the utterance and
  derives every window of the phrase's word count with
  `secret_phrase::spokenIn` (PBKDF2, `packages/lib/text`): about 50 ms for a
  20-word utterance with a 6-word phrase in a release build, and only for an
  eligible speaker while a phrase is set. The HTTP route runs it on the light
  lane, never on the event loop. At most the last 96 words are screened. A
  clean answer is remembered per speaker for two minutes (a SHA-256 of the
  words), so the turn that follows voice's screen of the same words does not
  derive them again. A new key clears that memory, so a remembered clean
  answer never hides a phrase set since.
- **The key.** A worker thread reads guard's key (`GuardDuressClient::phraseKey`,
  the `[guard] target` and `credential` the tool provider already uses) at
  start and every minute. It reads it at once when guard's
  `argus.guard.v1.change` carries the `duress` resource notice
  (`duress_feed::changesPhrase`). An unreachable guard is asked again with a
  backoff of 2 s doubling to 60 s, and the last key read stays in force. The
  process holds only the key: the phrase's words are never in it.
- **Before guard first answers**, no key is known and nothing can be withheld.
  The words go on as heard, and a copy waits (at most 32) to be matched once
  the key arrives, so the alert is still raised. That boot window is the one
  place the words can travel unredacted.
- **The raise.** A match becomes one pending `RaiseSpoken {userId, role, key,
  saidAt}` per speaker, handed to guard by the worker. `RAISED` or `REPEATED` settle it.
  `STALE` and `NOT_ALLOWED` drop it and read the key again, because words that
  are no longer the phrase are no alert. An unreachable guard is retried with
  the same backoff for as long as the process runs, and once more at stop (the
  `llm-duress` drain).
- **When the words were said (reviewer ruling, 2026-10-13).** `saidAt` is the
  instant the words reached this service's screen, in epoch seconds: the
  moment `DuressScreen::screen` was called, before any hashing. That is
  voice's `POST /llm/v1/screen` right after STT answered, or the turn itself
  when voice did not screen it. Voice's own capture or transcript time is not
  passed on today, so this is "llm's turn arrival" in the ruling's terms. It
  trails the speech by the STT time, usually well under the few seconds a
  long turn takes. Words heard before guard first answered keep the time they
  were heard, not the time the late match found them. The outbox row stores
  the same instant, and every retry sends it, after a restart too. Guard keeps
  it beside the time it received the raise (`services/guard/CONTEXT.md`,
  "Two times per spoken alert"). `DuressScreenDependencies::clock` replaces
  the clock in tests.
- **The raise survives a restart (the reviewer's F4 gap, closed 2026-10-13).**
  - **Written in the same turn.** The match is written down before `screen`
    answers: one row per speaker in `duress_raise_outbox` (this service's
    `memory.db`, `repositories/duress-raise`, `infra/sqlite-duress-outbox`).
    A row holds the speaker's id and role, the SHA-256 of the phrase key it
    matched and the time it was said. It never holds the key and never the
    words.
  - **Crossed out only when settled.** Every answer that settles the raise
    crosses its row out. A guard that does not answer leaves the row as it is.
  - **Read back at start.** The worker reads the rows back and waits for
    guard's key. When the digest still matches, it hands the raise over with
    that key. The memory store opens only when the event loop starts
    (`memory.init({.deferStore = true})`), after the worker, so the worker
    asks again every `retryBase` until the store can be read, and only then
    counts as idle. A row whose digest no longer matches (the phrase changed or was
    removed while argus-llm was down) is crossed out without asking guard,
    since guard would answer `STALE`.
  - **A restart test follows the real start order** (worker before store, as in `main.cc`), as fafe0a6d showed.
  - **Durability (reviewer ruling, 2026-10-13).** The insert goes through
    the outbox's own connection to the same `memory.db`
    (`SqliteDuressOutbox`, opened on first use from the shared connection's
    file name). That connection runs `synchronous = FULL` with a 5 s busy
    timeout (`kWriterBusyTimeoutMs`), so the insert waits for the shared
    writer, and a committed raise row survives a power cut as well as a
    restart. The conversation path keeps `NORMAL` (`SqliteGraph`).
  - **What stays on the shared path.** The read-back and the delete that
    crosses a row out stay on the shared connection and its lock. A lost
    delete only repeats a raise, and guard folds that.
  - **A write that fails** leaves the raise in memory only, as before, and
    says so in the log.
  - **Never written down.** Words matched late (the boot window above) are
    written only once they match. The late queue itself is never written,
    since it holds words, so a restart inside the boot window forgets what
    had not been matched yet. That loss is accepted (reviewer, 2026-10-13).
  - **A row that may come back.** A row guard settled but argus-llm could not
    cross out may reach guard again after a restart. Guard's repeat window
    answers `REPEATED`.
- **Logs.** Never the utterance, the key or the speaker. The screen says only
  that guard did not answer, or answers again. The relay says only that "an
  alert" waits for guard, how many it carried across a start and how long the
  oldest has waited.

`tests/unit/llm-duress-screen-test.cc` covers:
- a member who says the phrase is withheld at once and raises once, with the
  key it matched;
- nobody else is withheld: other words, a guest, an anonymous caller, an
  unknown role;
- the retry until guard takes the raise, with one pending raise per speaker;
- nothing is withheld without a phrase;
- a refreshed phrase replaces the old one, and the clean memory never hides it;
- what is said before guard first answers still raises;
- the turn carries the placeholder and the rest of its history unchanged, and
  the turn hook's caller rules hold;
- the model's prompt holds no trace of the phrase;
- a withheld turn reaches no model: the stream ends at once with the
  `unheard` marker, and the unary chat answers the same marker with no text;
- the memory graph keeps the same words when they are not the phrase, and
  keeps no trace of them once withheld;
- a match is written down before the screen answers, once per speaker, as the
  key's digest and never the words, and a settled raise is crossed out;
- a raise written down survives a restart of argus-llm between the match and
  guard's acknowledgement, and reaches guard with the key it matched;
- raises written down before a restart are read once the memory store opens,
  even when the worker started first, as `main.cc` starts it;
- the raise row commits through its own connection (`synchronous` 2) while
  the conversation connection stays at 1, waits for the shared writer, and
  the delete goes through the shared connection;
- a raise whose phrase changed while argus-llm was down is crossed out without
  asking guard;
- the raise carries the instant the words reached the screen, every retry
  carries the same, a restart sends the time the row stored, and words matched
  once guard answers carry the time they were heard;
- the change notice that asks for a refresh.

## The per-turn context block (2026-10-08)

A call used to hand the model every fact the app carried — the camera list,
the guard mode, the agenda — on every turn, baked into `history[0]`.
On a greeting, a joke or a question nothing has an answer for, that is noise
the 1.2B model recites or is distracted by, and it is prefill every turn pays
for.

`turn::ContextSelector` (`services/turn/context-selector.{hxx,cc}`) decides,
per turn, which context facets that turn may see. The facets are
`ContextFacet` (`facet-lexicon.hxx`): `Camera`, `Agenda`, `Guard`,
`Reminders`, `Memory`, `Modules`, `People`, `Tasks`, `Projects` — the last
three added 2026-10-17, when the N pack showed the people, task and project
facts the app offers were unreachable by any utterance. The decision is one
the turn already made: the decided tool's facet (`facetForTool`: `calendar.*`
→ agenda, `task.*` → tasks, `project.*` → projects,
`memory.remind`/`reminder.list` → reminders, `memory.*` → memory,
`modules.*` → modules, `app.show_camera` → camera, `app.set_guard_mode` →
guard; `app.open` implies nothing) unioned with the facet words and phrases
the utterance itself names (`facetsInText`, es and en), so an agenda turn
keeps the agenda fact and a greeting keeps none. The utterance is folded with
`text_norm::folded` (punctuation becomes space), so the first word of
`¿Vino sola?` is `vino` and not `¿vino`: the lexicon's single words match
question-first words too. The guard lexicon carries the arrival and panic
utterances (`llegada`/`llego`/`entrada`, `quién fue`/`qué hago`/`llama a la
policía`), the singular forms of the agenda and reminder words (`cita`,
`reunión`), and the obligation phrasings that name a reminder without the word
(`tengo que`, `cuándo toca`, `cuándo vence` for a task), all es and en.

The facts travel per turn, not in the history: `ChatRequest::contextFacts`
(a `std::vector<ContextFact>{facet, text}`, additive). `LfmAdapter::chatTurn`
and `chatPlainTurn` select the facts the turn's facets cover, drop the rest
(never blanked into empty headers), order them most-relevant-first, and compose
them into one framed block — a frame that marks
the text as data written by others, never instructions
(`turn::contextBlock`, es and en, in `context-selector.cc`, its only consumer).
The frame also states, in the user's language, that the people the data names
are third parties and not the person talking to Argus, and the identity line
the turn appends (`role_access::speakerLine`) now reads `Hablas con David (el
propietario de la casa) en una llamada de voz.` — it names the second person
the model speaks to, so the owner's profile can no longer be read as a fact
about someone else and offered as an answer. A fact whose facet the build does not
know is dropped rather than injected. The profile and the name stay in the
static prefix and are always present; the clock note and the situation ride in the last user
message, the clock at its head and the situation after the user's words. The composed block is what `LlmChatOutcome::contextBlock`
returns, so the eval scores `missedFact`/`leakedFact` against the exact prompt
the turn sent.

No production caller fills `ChatRequest::contextFacts` today (`context_facts`
left the wire with e808604b), so in a call the agenda facet's data is what
`calendar.list_events` answers. That tool expands recurring events with their
skipped and moved occurrences applied (G4, `services/productivity/CONTEXT.md`,
"Recurring occurrences"): "¿qué tengo mañana?" never hears a skipped
occurrence and hears a moved one at its new time. A producer of an agenda
fact, if one is added, must read the same expansion and not the stored
`starts_at`.

Degradation: a turn that selects nothing composes no block, which is exactly a
plain chat and shares the cached prefix. The Laya facet pass (one `noul`
question per unsure facet) is U24's; nothing links it here.

### Where the facts come from: the server reads them (2026-10-19)

The wire field `context_facts` was removed in e808604b (it is `reserved` in
`packages/contracts/llm/llm.proto`) and nothing replaced it, so in production
`ChatRequest::contextFacts` is always empty: the model saw data only when the
decider ran a tool, and a plain or follow-up question (`¿está armada la
vigilancia?`, `¿qué tengo mañana?`, `¿se fue?`) reached it with no data at all.
The scenario eval hid it, because it injected `request.contextFacts` straight
from the fixture.

The facts are now read on the server. `LfmAdapter::turnContext` runs the
selector, and when the turn carries no facts of its own and selected at least
one facet it calls the `turn::ContextFetch` hook
(`services/turn/context-fetch.{hxx,cc}`) with the facets, the tools offered to
this audience, the audience, the turn's `ToolContext` and the names of the
tools the decider already ran this turn. The hook is injected through
`LfmAdapterInput::contextFetch` (none by default) or `useContextFetch`;
`main.cc` installs the production one, `turn::toolContextFetch`, over the
adapter's `ToolExecutor`. `chatPlainTurn` has no audience and never reads.

The production fetch is a deterministic system read, never the model choosing
a tool. Each facet has one read tool:

| Facet | Tool | Arguments |
|---|---|---|
| agenda | `calendar.list_events` | `from` = start of the local day, `to` = `from` + 1 + `agenda_days` days, `limit` 5 |
| reminders | `reminder.list` | `limit` 5 |
| tasks | `task.list` | `limit` 5 |
| projects | `project.list` | `limit` 5 |
| modules | `modules.list` | none |
| camera | `camera.status` (argus-camera) | none |
| guard | `guard.status` (argus-guard) | none |

`memory` is not read here (recall is injected separately) and `people` has no
read tool every role holds (`guard.where_is` does not exist; presence is the
Owner's alone), so neither facet fetches. A facet is skipped when its tool is
not in the offered list, is not in `tool-effects.json`'s read-only list, is
not runnable for the audience under the module gate
(`tool_access::runnable`), is refused by the conversation surface, or was
already run by the decider this turn (its result is already in the
situation). The read goes through `ToolExecutor::execute` with the channel
`context_fetch`, `decided = false` and no app-action emitter, so capability,
module and surface gating are exactly those of a decided call. Each
`ToolResult` that is `ok` becomes up to `max_lines` facts of its facet, one per
line of `output` (or of `data.facts` when the output is empty), each clipped at
a word boundary to `max_chars` with an ellipsis; a failed read adds nothing and
never fails the turn.

The reads run in facet order, one after another; the budget is checked before
each read, so a turn spends at most `budget_ms` plus the read that was already
running when it ran out (a remote tool's own transport timeout is 15 s; a
read never starts after the budget is spent). The `[context]` keys, all
optional and clamped by `LlmConfig::boundContext`:

| Key | Default | Range |
|---|---|---|
| `context.budget_ms` | 300 | 0-5000 |
| `context.max_lines` | 4 | 0-10 |
| `context.max_chars` | 220 | 40-600 |
| `context.agenda_days` | 2 | 0-14 |

`facetForTool` maps `camera.status` to camera and `guard.status` to guard, so
a turn the decider routes to either keeps its facet.

The scenario eval measures this path with `--facts fetched`: the request
carries no facts, the stub read tools answer with the scenario's facts of
their facet (one per line) to a `context_fetch` read, and to a decided call
too when the scenario has facts of that facet; `--facts injected` (the old
mode) still sets `request.contextFacts` from the fixture and stays what ctest
runs. `--fetch-lines` and `--fetch-chars` override the line cap and the clip
for an experiment. Measured 2026-10-19 at temperature 0.3, seed 42, 63 cases:
injected casePass 19, missedFacts 39, invented 9, ungroundedNumbers 0;
fetched casePass 19, missedFacts 43, invented 7, ungroundedNumbers 3;
fetched with six lines casePass 17, missedFacts 46, invented 6. The fetched
misses are the production gaps the injected mode hid: the `people` facts no
tool serves, the agenda lines past the cap, and a facet the decider already
ran (its result reaches the situation, not the block).

## The prompt is a prefix (2026-10-10)

`LlmService::prefill` reuses the KV cache across calls: it rewinds the cached
sequence to the longest common token prefix of the new prompt and decodes only
what follows. That prefix is the prompt the previous call built, so it is
reproducible from the next turn's request only if the request re-sends the
turns it already sent, byte for byte.

**Nothing rewrites a past turn's text.** A turn the model has already seen
comes back from the caller exactly as it was first sent. A rewrite of an
earlier turn moves the divergence to that turn, and every turn after it
re-decodes the whole call — even a single line prepended to the first user
message, which is how a language directive on the user turn took the wire
test's reuse from 12 of 33 tokens to 0 of 38 on every turn after the first.

**Anything per-turn the model needs travels with the turn that needs it.** The
clock note sits at the head of that turn's last user message and the selected
situation block after the user's words; both are part of the turn when it is
first sent. A per-turn fact the caller keeps — an app note — is a message of
the caller's own history, so the caller re-sends it unchanged.

**The session's language is not per-turn.** It is a fact of the call, stated
once at the head of the system message — the prompt's prefix-stable part — and
never again on a user turn. `LfmAdapter::spokenMessages` composes that head and
is the only writer of it.

## Pending intents (2026-10, context plan section 5)

A request for a tool of a module that is off is not lost. `ToolExecutor` hands
it to an `IntentLedger` (`PendingIntentService`, `feature/pending-intent/`,
one table `pending_intent` in memory.db: user, role, module, tool, arguments,
language, the utterance, the session, the state and the times). Lifecycle:
**offered** (the user was offered to turn the module on; one open offer per
user and module, a newer one replaces the older) -> **waiting** (the user said
yes and `modules.enable` succeeded) -> **done** (the module became active, the
stored call ran as the user who asked, with their role, language and the
utterance, `decided` set because a person chose it, and the user was told) or
**failed** (the install failed or was cancelled, or 24 hours passed: the
request is saved as the user's own reminder through the shared reminder
service, due at its own time when it has a future one and in a minute
otherwise, and the user is told so) or **expired** (an offer unanswered for an
hour). Settled rows are purged after 30 days.

- **Survives restarts.** It is a table, not memory; a sweep every minute
  expires stale offers, fails stale waits and also runs the intents of a module
  that came up while nobody was watching.
- **The module feed.** `ModuleIntentFeed` is argus-llm's own durable consumer
  (`argus-llm-intents`) on `argus.settings.v1.module`: an enabled-set frame
  with a module that is now enabled and active posts `activated`, a job that
  ended `failed` or `cancelled` posts `failed` with its reason. Ignored: frames
  that are not settled, jobs that are not installs, anything malformed.
- **At least once, idempotent.** The feed and the sweep may both fire, and a
  restart between the call and the row moving to done runs the call again: the
  order is call, then `done`, then the notice. The tools it can run are
  idempotent for that reason (`calendar.create_event` carries an idempotency key
  derived from the arguments). A call that answers `module_inactive` leaves the
  row waiting and says nothing; one that fails for any other reason becomes a
  reminder and a notice like a failed install.
- **Telling the user.** `NotificationIntentNotifier` creates one notification
  of type `assistant_task` through `CreateNotifications` (argus-llm is admitted
  there only for that type, one recipient and no call plan:
  `services/notification/CONTEXT.md`), with a command id derived from the row so
  a retry is a duplicate, not a second notice. A notice that cannot be
  delivered is logged and does not undo the work.
- **Not here.** Argus-llm never enables a module on its own and never installs
  anything; it only asks the settings provider on a clear spoken yes from the
  Owner.

`tests/unit/llm-pending-intent-test.cc` covers every transition, the language
of the notice, the failure fallback, the sweeps, a restart over the same
database, the worker thread and the whole path through the executor.

## The assistant in a call (2026-10-03, turn since 2026-10-06)

The voice session and argus-llm agreed this contract with the voice agent:

- **Messages.** History[0] is the system persona with the stable call facts.
  User messages carry only what STT heard. Every later `system` message is a
  note: the tone note right after its user message, context notes appended
  where they arrive. Notes are rendered in place and never read as the user's
  words. The deciders and every tool read the last user message through
  `LfmAdapter::spokenText`, which also drops a trailing parenthesized line,
  the shape the tone note had when it was appended to the user's content.
- **One speak prompt.** Every spoken answer is the caller's history as it
  came, then (only when something was decided) one final system note with the
  findings, so a turn with nothing to say is exactly a plain chat and shares the
  cached prefix of the call, and a prefill-only request (`prefill_only`, sent
  while the greeting plays) primes exactly it. Answers are
  generated with `toolCallsAllowed = false`: the sampler gives
  `<|tool_call_start|>` a -inf logit bias.
- **Explicit app commands are decided first**
  (`feature/llm/services/tools/app-command.cc`, the first rule of `RuleDecider`) when the turn offers the app
  tools, which only a call with `clientActions` does. Guard mode needs a
  guard word (vigilancia, modo, guardia, alarma, seguridad, guard, mode,
  security, alarm), a verb (pon, activa, cambia, pasa, set, switch, turn,
  change) and a mode word. The word after "modo"/"mode" wins, otherwise the
  first mode word, from noche, nocturno, fuera, ausente, casa, armado, night,
  away, home and armed. `app.show_camera` needs a display verb (muéstrame,
  enséñame, abre, pon, show, open, or "quiero/déjame ver") and a singular
  cámara/camera. The name is taken from around that word: "la cámara del
  garaje, por favor" → `garaje`, "the garage camera" → `garage`, an empty name
  means the last notice. `app.open` needs an opening verb and a screen word.
  An utterance with `?` is never routed. A terse utterance (seven words or
  fewer, not opened by an interrogative) that says "modo <mode>" next to a
  guard word routes without a verb: speech recognition drops the first word
  of a barged-in command ("con la vigilancia en modo noche"), and the model,
  given the bare words, claimed the change without calling the tool. A live
  call showed the model answering "¡Listo!" to "pon la vigilancia en modo
  noche" without calling the tool. The reply guard forbids confirming an action that no tool ran.
- **A claimed app action is checked before it is spoken.** When the user's
  words ask for something in the app (a guard word, a camera, "abre" plus a
  screen, never a question) and no app tool ran this turn, a reply that claims
  the action ("cambié", "activado", "abrí", "mostrando", "listo", "changed", ...)
  is replaced by the honest reply. The live call that found this had lost its
  first word to speech recognition ("con la vigilancia en modo noche") and the
  model answered "Cambié la vigilancia a modo noche" with no tool.
- **App tool results speak the call's language.** The guard modes, screens
  and camera reach the model as labels in the call's language: "La app puso
  la vigilancia en modo fuera de casa.", "The app set the guard mode to
  away.", "La app abrió la agenda.", "La app está mostrando la cámara
  garaje." The enum values (`home`, `night`, `away`, `armed`) never appear in
  the text the model reads. A live call had the model say "modo outside of
  home" in a Spanish answer.
- **Memory writes keep to the user's words.** `memory.remember` and
  `memory.remind` check the model-written `text` and `value` against the
  user's utterance. When fewer than 60 % of the argument's words were said,
  the utterance replaces the text and the triple is dropped. A fact copied
  from a note or a camera offer, or the model's paraphrase, is never stored as
  the user's fact, and "recuérdalo" alone stores nothing. An explicit request
  ("recuerda que …") whose extraction comes back incomplete is stored from its
  rule clause instead of being dropped.
- **Explicit means the user's words, not a decider's confidence.** A
  model-decided call counts as explicit only when the rule parser recognizes
  a trigger or a statement, or when it is a reminder whose words name a time
  (`TemporalResolver`). "Recuérdame mañana a las nueve llamar al dentista",
  which has no "que", now stores the whole clause; before, an incomplete
  extraction kept "llamar al". A reminder that names no one is the
  speaker's: its subject is the user's own entity. So is a note asked for
  with a rule trigger ("anota que llegó el paquete", "recuerda que el wifi se
  cae cada semana") whose lexicon extraction finds no subject. With the lexicon as
  the only extractor such requests otherwise store nothing. A
  statement-start match is not a timed request: the vocabulary's "la cámara"
  statement start made "muéstrame la cámara 3" a reminder, and the 3 read as a
  time. Without a trigger or a subject it stores nothing.
- **A save trigger outranks a cancellation inside it.** "don't forget that
  the dog eats at 7" contains the cancellation "forget that". The rule
  triggers are tried first and a cancellation counts as a forget only when no
  trigger matched, so the sentence answers `memory.remember`
  (`llm-turn-flow-test`). "Enciende la luz de la cocina" stores nothing; an
  empty extraction must not let it through the rule path. The tool handlers
  extract with the lexicon tier, the only one since the off-turn extractor
  left on 2026-10-12; inside a call's turn that extractor had measured 23 s of
  `tool_ms` on a loaded machine.
- **A command is not a statement about its object.** The vocabulary's
  statement starts are noun phrases ("la cámara", "la luz", "the heating"),
  accepted within the first five words, so "muéstrame la cámara 3",
  "enciende la luz de la cocina" and "turn off the heating" were stored as
  facts about the camera, the light and the heating. `lib/phrase` now has a
  `Command` phrase kind (imperatives, request openers such as "puedes",
  "por favor", "can you", "quiero ver") plus the accented-clitic imperative
  form ("muéstrame", "explícame"; the memory verbs "recuérdame", "apúntalo"
  and the like excepted). A statement start is ignored when its clause, after
  fillers, opens with a command; `RuleParser::isCommand` answers the same for
  the whole utterance, and a model-decided reminder whose words open with a
  command is not a timed request either.
- **Plain save requests are triggers.** "save that", "save this", "store
  that", "guarda esto", "guárdame esto", "memoriza que", "no te olvides de
  que", "quiero que guardes que", "toma nota" and their variants were not in
  the vocabulary. A routed save request used to reach `memory.remember` on
  the model's word, formation refused it as not explicit, and the model then
  answered "lo he guardado" for a fact that was never written (9 of 16 such
  probes before this change).
- **A note the lexicon cannot structure stays a note.** An explicit request
  whose lexicon extraction finds no complete triple, or one that covers less
  than `extract.lexicon_min_coverage` (0.6) of the clause's words
  (`CoveredLexiconExtractor`), is stored at once as the speaker's note
  (`predicate = nota`, the clause as written), so the confirmation the user
  hears is true and the fact survives a restart. Until 2026-10-12
  `MemoryService::refineLater` then queued a job that ran the off-turn
  extractor on the utterance when the chat engine was idle and replaced the
  note with a triple when the model found one; that job, `memory.extract_wait_ms`
  and the model left together, and the note is now final. The memory behaves
  as it always did on an install without that extractor.
- **A newer value closes the older one; notes accumulate.** `upsertFact`
  looked for the open fact with the same entity and predicate and bound three
  of `CLOSE_FACT`'s four parameters, so since the close was scoped to its
  owner (`ref_id`) nothing was ever closed and a "supersedes" edge pointed
  at a fact that stayed open. It binds the owner now and adds the edge only
  when a row was closed. A note is many-valued (`supersedes = false`): every
  "anota que …" stays, and saving the same note twice keeps one row.
- **`memory.forget` takes a `query`**, the fact in the user's words, instead
  of a `fact_id` the model could only invent. It deletes the user's fact
  that shares the most words with the query (see "Forgetting deletes"
  below), scoped to the caller (`scope = 'user' AND ref_id = ?`), and
  succeeds only when a row went. It runs only when the user's own
  utterance asks to forget (`RuleParser::isCancellation`, and no save
  trigger in it); otherwise the tool answers "Dime con tus palabras qué
  quieres que olvide" and nothing is touched. A query that is not grounded
  in the utterance is replaced by the utterance. Before, any id of any user
  closed, and "olvidado" was answered even when nothing matched.
- **Tool outputs are spoken material**: localized to the call's language,
  with no ids ("Guardado: mi hermana viene los domingos.", "Saved: …"). The
  app tools answer in the call's language too. `memory.recall` honours the
  owner's `memory.recall_top_k`; the tool path read a fixed 8.
- **Permission before schema.** The executor refuses a role before it
  validates arguments, so an unpermitted caller learns nothing about a tool's
  shape. `ToolRegistry::names()` is sorted, so the declarations, and with them
  the cached prefix, are the same in every process.

## Real-time behaviour (measured 2026-10-03)

Host: Ryzen 7 5825U (8 cores, 16 threads), CPU only (`gpu_layers` resolves
0, see the tier note), LFM2.5-1.2B-Instruct-QAD-Q4_0. Prod builds: the
baseline is 48b75ba2, the commit before this work, built from a worktree.
Decode on `ThreadBudget::lightThreads()` = 4 threads, prefill on
`batchThreads()` = 8. Other agents shared the machine, so each run gives its
load average. Every figure was taken after the 16:15 fix of the shared
helpers' busy-wait.

**A seven-turn Spanish call** (`/llm/v1/chat-stream`, temperature 0, owner,
memory and policy tools; turn 2 is a routed save). TTFT is time to the first
streamed token.

| | turn 1 | turn after the save | the turn after that | turns 2–7 median / p90 | tokens decoded per turn |
|---|---|---|---|---|---|
| baseline, old message shape (load 5–7) | 3.3–3.6 s (677 cold) | 1.7–1.8 s (360 re-prefilled) | 3.7–4.4 s (775 re-prefilled) | 427 ms / 3.7 s | 275 |
| this work (load 1–5) | 2.9–3.2 s (751 cold) | 0.86–1.33 s (85) | 0.44–0.55 s (53) | 425 ms / 860 ms | 146 |
| this work, primed with `prefill_only` (load 1–5) | 0.33–0.44 s (31) | — | — | 552 ms / 928 ms | 43 |
| this work, primed (load 7–9) | 0.44 s | — | — | 522 ms / 1.38 s | 43 |

The prime itself takes 3.1–3.5 s for 723 tokens. The voice session sends it
while the greeting plays, so the user never waits on it. In the voice agent's
end-to-end run (Release TTS, end of speech → first audio, five turns) the
baseline measured 3456 / 1635 / 1523 / 1602 / 660 ms and this work
823 / 1511 / 1870 / 1754 / 1199 ms. The first token came at 195–561 ms. The
later turns vary with the length of the reply's first sentence, which is now
the dominant cost (voice and TTS side).

**Engine speed.** Prefill runs about 250 tokens/s on the 750-token call
prompt and 140–175 tokens/s on a 258-token one. Decode is 28–31 tokens/s
with 4 threads. A sweep at load 4–6 gave 4 threads 28.7–29.3, 6 threads
30.9 and 8 threads 30.7 tokens/s. The +5 % is within that load's noise
(decode is memory-bandwidth bound), so `lightThreads()` stays and keeps the
other cores for STT and TTS during a call.

**Decode takes the physical cores (2026-10-17).** The same host, prod build,
two sweeps of the same two-cap method on `/llm/v1/chat-stream` (`tools=false`,
`reset_context`, the same prompt at `max_tokens` 64 and 192; the rate is the
128 extra tokens over the wall-time difference, median of 3). With the machine
at busy 17 %: 4 threads 27.4 tok/s, 6 threads 30.4, 8 threads 32.3 — 4 → 8 is
+18 % where the 2026-10-03 sweep had called that step +5 % and noise. With the
machine at busy 41 % (other units' builds saturating memory bandwidth):
8 threads 23.0 against 4 threads 22.7, +1 % — the same wash as before, at a
cost of twice the CPU time for the same request (71 s against 37 s). Decode is
bandwidth-bound, so the step only pays when the machine is not already
saturated, and it is the machine a call actually runs on (a home server with
one user talking) that the budget is chosen for. Decode moved to the new
`ThreadBudget::decodeThreads()` (the physical cores, `hw/2`, capped at 8);
prefill keeps `batchThreads()`. The Laya session's fallback stopped being a
hardcoded 4 and reads `lightThreads()` — 4 on this host, so the session is
unchanged, but the count is a budget now (root rule 13b). The call-level probe
(three replies alone, then three while the TTS streamed the previous sentence,
same 192-token request on both allocations) put the shipped one at 14.6 s for
the reply with the TTS at RTF 1.067 and a 1.5 s deficit — over realtime — and
the new one at 8.2 s with the TTS at RTF 0.880 and a 0.3 s deficit.

**The turn without the model loop (2026-10-06).** Same host, prod build, the real LFM2.5-1.2B,
`llm-tier-eval` over 220 turns of the judge corpus (productivity 50, memory 50, none 40, modules 30,
app 20, inactive 30; stub tools with instant handlers; load average about 5), one sequential run each,
through `heavy.sh 6` in chunks. The model loop (the build of 11:33) took mean 1694 ms, p50 1424,
p90 2567, p95 3217 and max 14054, and ran 62 tool calls; the turn (decide, fill, run, speak) took mean
1316 ms, p50 1355, p90 1969, p95 2184 and max 2708, and ran 105. A first run of the turn that still
appended a persona sentence to the first system message measured p50 1566 ms, because those tokens were
prefilled again every turn: the speak stage adds nothing to the caller's history now. A tool turn is one
generation and a question is none, so the tail disappears; the median moves little.

The measurements below used `argus-tool-bench` and `tests/fixtures/tools/` (`check.tsv`,
`negatives.tsv`); both are gone. `tests/eval` replaced them in 2026-10 (judge corpus
`tests/fixtures/eval/cases.jsonl`, gates in `tests/eval/gates.json`,
`docs/operations/voice-quality-eval.md`), and the numbers stay here as the 2026-08 history.

**Tool selection, HTTP chat without app tools** (`check.tsv` +
`negatives.tsv`, 150 rows, temperature 0, each row in its own language;
"stored" means a memory write that succeeded):

| | saves stored (25) | false writes (125 non-save rows) | mean latency per row |
|---|---|---|---|
| baseline (load 5–9) | 19, and every miss was answered "lo he guardado" | 1 | 2.4–3.8 s |
| this work (load 3–9) | 24 | 2 ("la alarma se activa a las 10", "la cámara de mi teléfono no funciona") | 1.3–1.5 s |

**Tool selection inside a call** (`argus-tool-bench --voice`: the real
controller, `clientActions`, recording handlers, `check.tsv`):
- Camera requests: 56 of 56 as expected. The five explicit ones
  ("muéstrame/checa la cámara N", "show/check camera N") open
  `app.show_camera`.
- Saves: 19 of 20.
- Neither: 22 of 27.
- Mean TTFT: 374 ms (camera), 789 ms (save), 328 ms (neither).

**After the command, trigger and note changes (2026-10-04)**, HTTP chat
without app tools, same 150 rows plus 16 explicit save requests and 10 app
commands (`check.tsv`, `negatives.tsv` and two scratch probe sets), the prod
build at HEAD against the prod build with the changes, temperature 0, each
row in its own language and persona:

| | saves stored (25) | explicit save probes stored (16) | commands that wrote or tried a write (10) | false writes (66 `none`) |
|---|---|---|---|---|
| HEAD (llm as of ed766524) | 24 | 7, every miss answered "lo he guardado" / "I've saved" | 10 tried, 1 stored ("show me the camera 3") | 2 |
| this work | 25 | 15 ("apunta el código del wifi, es casa2024" has no trigger) | 0 | 2 ("la alarma se activa a las 10", "la cámara de mi teléfono no funciona", both statements) |

Of the eight rule notes the run left, the off-turn extractor of that day
refined two ("el código del portón es 1234", "the gate code is 1234") and kept
six as notes; since 2026-10-12 there is no off-turn refinement and all eight
stay notes. Inside a call (`argus-tool-bench --voice`, `check.tsv`): camera 56/56
and saves 19/20 in both builds; "neither" rows 22/27 → 23/27 with 3 → 2
memory writes.

**Persona language (A/B).** The call prompt is written in English and says
"Reply strictly in Spanish/English". A Spanish-written prompt for Spanish
calls was compared on `argus-tool-bench --voice --row-lang` (150 rows each
in its own language, temperature 0 and 0.85, two seeds, 600 replies per
variant): no reply in either variant was in the wrong language or mixed
languages, and tool selection was identical (59/59 camera, 25/25 saves,
45/66 neither). The English prompt stays. The switch that was heard live
("modo outside of home" in a Spanish answer, 3 of 16 "¿en qué modo está la
vigilancia?" turns in the 2026-10-04 call runs) happens inside a call with
an app situation note, which the single-turn bench does not reproduce; it
needs a call-level A/B.

## Timed reminders call the user (2026-10, "Argus calls you")

`memory.remind` stores the reminder as before; when the user's own utterance
names a time that `call_time::resolve` (`services/extract/call-time.{hxx,cc}`)
turns into an instant within 30 days, it also asks argus-notification to call
the user at that time (`CallService.ScheduleCall` through
`argus::clients::notification`, `[notifications] target/credential`, paired
with notification's `[grpc] caller_llm`). The answer then ends with "Te
llamaré a las 09:00." only when the call was scheduled.

The contract is narrow on purpose: the model cannot call anyone. The time
comes from the user's words, not the model's arguments; the recipient is the
speaking user; the topic is the grounded reminder text with the time phrase
cut out ("llamar al dentista"); the command id is
`memory-remind:<factId>:<fireAt>`, so a repeated tool call schedules once.
The parser (`call_time::read` in `call-time.{hxx,cc}`, word tables in
`call-time-words.{hxx,cc}`) reads es (Peruvian included) and en in the host's
local clock, which is Lima's in production, and answers with an instant, or
with nothing, or with a `DayConflict`. A phrase needs a clock part; a day part
and a part-of-day marker are optional and may stand before or after it. Only
the first clock and the first date are read.

| Clock part | Reading |
|---|---|
| 24-hour or colon form with hour 0 or 13-23, or a leading zero ("18:30", "07:30", "00:30") | as written, any qualifier ignored |
| hour 1-12 + "a. m.", "a.m.", "am", "AM", "4am" | 12 is 00:00, otherwise the hour |
| hour 1-12 + "p. m.", "p.m.", "pm", "PM", "4pm" | 12 is 12:00, otherwise hour + 12 |
| "de/en/por la mañana", "in the morning", "esta mañana" | the hour (12 is noon) |
| "de la madrugada" | the hour (12 is 00:00) |
| "de/en/por la tarde", "in the afternoon", "in the evening", "esta tarde" | hour + 12 (12 is noon) |
| "de/en/por la noche", "at night", "tonight", "esta noche", "tomorrow night" | 1 to 5 the hour (small hours), 6 to 11 hour + 12, 12 midnight |
| no qualifier | 1 to 6 hour + 12 (the afternoon), 7 to 11 the hour (the morning), 12 noon |
| "mediodía", "noon", "midday" | 12:00 |
| "medianoche", "midnight" | 00:00 at the end of the named day (the next 00:00 when no day is named) |

An explicit qualifier always wins over the bare-hour reading. Minutes keep
their value in every spelling: "y media", "y cuarto", "y 10", "menos cuarto",
"menos 10", "en punto", "5:30", and in English "half past five", "quarter to
six", "ten past five", "eleven fifteen", "five forty five", "five oh five",
"5 o'clock". "menos" takes the minutes off the hour as spoken, so "a las 6
menos cuarto" is 17:45 and "a las 8 menos cuarto" 07:45.

| Day part | Reading |
|---|---|
| none | today when the instant is still ahead, otherwise tomorrow |
| "hoy", "today", "esta tarde/noche/mañana", "tonight", "this evening" | today; a bare 7 to 11 whose morning has gone is tonight ("hoy a las nueve" at 15:20 is 21:00); when every reading of today has passed `read` answers `passedToday` (tomorrow at that hour) and resolves nothing, and the turn asks "Esa hora ya pasó hoy. ¿Mañana a las 8 de la mañana?" / "That time has already passed today. Do you want tomorrow at 8 AM?": a yes takes tomorrow, another time is read, a no drops it |
| "mañana", "tomorrow", "pasado mañana", "day after tomorrow" | +1, +2 days |
| a weekday ("el viernes", "on Friday") | the next one strictly after today, unless "hoy" is said |
| a day of the month, no month ("el 15", "el quince", "the 15th") | the next occurrence: this month while the instant is still ahead, otherwise the next month that has that day |
| a day and a month ("el 15 de octubre", "october 15th", "15th of October") | this year, or next year when it has passed |
| a weekday and a day of the month that agree ("el viernes 9") | that date |
| a day, a month and a 4-digit year ("el 15 de octubre de 2027", "October 15, 2027") | that date when it falls inside the 12 months; outside them (or already past) nothing is resolved and the turn asks again |
| a weekday and a day of the month that disagree ("el lunes 9" on a Friday 9th) | nothing is resolved; `read` reports the two candidates in `conflict` and the turn asks which one |
| a weekday and "hoy", "mañana" or "pasado mañana" that disagree ("mañana lunes" when tomorrow is Thursday) | the same: `conflict` carries both days and `relative`, and the turn asks "¿Mañana jueves o el lunes 12?" |

With no day named, a bare 7 to 11 whose morning has already passed today is
tonight's reading when that is still ahead ("a las nueve" at 15:20 is 21:00,
"a las ocho menos cuarto" 19:45, "at 9:15" 21:15) and tomorrow morning when
it is not; with "hoy" the same evening fallback applies but a morning and an
evening that have both passed are asked about, never moved to tomorrow
silently; with any other day named it is that day's morning, and a bare 1 to
6 is always the afternoon.

Relative phrases ("en 20 minutos", "in half an hour") are capped at 30 days;
explicit dates are accepted up to 12 months from now (`read` answers
`farAway` past that, and the turn says "Esa fecha queda fuera de lo que puedo
agendar: llego hasta dentro de 12 meses. ¿Para qué día y a qué hora?" and
reads the answer for that question). The call itself is set by
argus-notification, whose horizon is the same 12 months; a call it refuses or
cannot confirm is never claimed (below).

Judgement calls the rules left open, kept as they stand until the owner
decides (each is one line in `call-time.cc`): "12 de la mañana" is noon;
"las 2 de la noche" is 02:00 (the small hours) and "las 5 de la noche" too;
"mañana a medianoche" is the 00:00 that ends tomorrow while "12 a. m." on a
named day is that day's 00:00; "next friday" is the coming Friday; a year is
read only as four digits after a month.

A weekday that disagrees with the day of the month is a clarifying question,
never a guess. `slots::fill` (for a date-time slot) and the turn before
`memory.remind` runs ask `call_time::read` for a `DayConflict`; the turn then
says "¿El lunes 12 o el viernes 9?" / "Do you mean Monday the 12th or Friday
the 9th?" (the weekday's nearest occurrence first, then the number with its
real weekday) and keeps a pending (`Awaiting::Day`, the two candidate instants
as ISO in `Pending.values`, the original words for grounding). The answer is
read for that question only (`slots::chooseDay`: a weekday name, a day number
or "el primero/segundo", "first/second"); a date-time slot takes the chosen ISO
and the call goes on with the other slots read again from the original words
(a conflict against today or tomorrow is asked the same way, the relative
day first, and `slots::chooseDay` also reads "mañana", "hoy" or "tomorrow"
for it), while `memory.remind`, whose time is its user's words, is run with
`ToolContext::heardAt` set to the chosen instant, which `scheduleReminder`
prefers over resolving the words (the executor normalizes a call's times with
the turn's own clock, `ToolContext::now`, so a question answered later is not
read again against another instant). An answer that names neither is asked once
more and then given up; a new command or a no replaces the question.

Every created event and reminder is read back with the day and the time it
resolved to, in words, from the tool's own result and not from the model:
`calendar.create_event` says "Quedó agendado «Reunión con Pedro» para el jueves
8 a las 3 de la tarde." (and "«Dentist» is scheduled for Thursday the 8th at
3 PM."), `task.create` says "Quedó agregada la tarea «…» (Proyecto), para el
viernes 8 de marzo.", and
`memory.remind` appends "Te llamaré el jueves 8 a las 3 de la tarde." or, when
only a reminder row was saved, "Quedó en tus recordatorios para el jueves 8 a
las 3 de la tarde." (`reminder-readback.{hxx,cc}`). The facts are plain
sentences because the turn renders each as a plain fact under a label
(`Nota de la app:`) that already ends in a colon. The wording is
`spoken_time` (`packages/lib/text`), shared with the confirmation question,
and is built to be said back: the resolver reads "a las 3 de la tarde", "a
mediodía" and "a las 12 de la madrugada" (00:00) as the same instants
(`call-time-test`'s round trip over every hour and quarter). The month is named
only when it is not the current one.

The read-back is guaranteed, not left to the model: the tool puts the moment
(`readback`) and the sentence (`readbackSentence`) in its result data, the turn
keeps them on the finding, and `LfmAdapter::chatTurn` appends the sentence to
the spoken reply, sync or streamed, when the reply does not already carry the
moment (compared folded, without a leading "el"/"on"), so it is never said
twice and never left out; a reply the claim guard cut is not appended to.

A call is claimed only when argus-notification confirmed it. `memory.remind`
asks the scheduler for a `ReminderCallOutcome` (`Scheduled`, `TooFar`,
`InThePast`, `TooMany`, `Unavailable`, `Refused`, mapped from the call
service's codes by `NotificationReminderCalls`; any exception from the
scheduler is `Unavailable`) and sets `callScheduled` in its result. A call
that was not confirmed says "Lo guardé en tus recordatorios, pero no pude
programar la llamada." / "I saved it in your reminders, but I could not
schedule the call.", then the reason when there is one ("Solo puedo llamarte
hasta dentro de 12 meses.", "El servicio de llamadas no responde ahora.") and
"Era para el jueves 8 a las 3 de la tarde."; without the reminder row it says
"Lo guardé, pero …". The reply guard treats "te llamaré", "te voy a llamar",
"te llamo a las…", "I will call you", "I'll call you" (not questions, offers
with "si quieres"/"if you want", or negations) as a claim that needs
`callScheduled`: without it the sentence is cut like any false completion, on
the sync path, the stream and a plain reply (`claimsCall`, `TurnState::called`,
`GateInput::callsConfirmed`).

Whether the scheduled call rings, is only a
notification or is spoken into a live call is the user's call preference
(`assistant`), decided by the notification service. The descriptor no longer
says "no suena ninguna alarma".

## Recurring reminders from the assistant (F6a, 2026-10-19)

"Recuérdame cada lunes a las 8 sacar la basura" and "remind me every day at
9 to take my pills" become one productivity reminder series each, through the
same `memory.remind` tool and the same row writer as a one-off reminder.
Productivity already stores and fires a series (`recurrence_rule` on the
reminder RPC, `services/productivity/CONTEXT.md` "Recurring reminders and
snooze"); the RPC path forwards the rule untouched, so productivity did not
change.

**The rule tier reads the recurrence** (`repeat_rule::read`,
`src/feature/memory/services/extract/repeat-rule.{hxx,cc}`, beside
`call-time`, whose fold and tokens it reuses). It is deterministic and reads
es (Peruvian forms included) and en in one pass, whatever the call language:

| Heard | Rule (`repeat` argument) |
|---|---|
| "cada día", "todos los días", "a diario", "diario" (not after an article: "el diario" is a newspaper), "diariamente", "todas las mañanas/tardes/noches", "cada noche", "every day", "everyday", "daily" (not after an article), "every morning/evening/night", "once a day", "una vez al día" | `FREQ=DAILY` |
| "cada 2 días", "cada dos días", "interdiario", "día por medio", "día de por medio", "un día sí y otro no", "every other day", "every 2 days", "cada quincena" (15 days) | `FREQ=DAILY;INTERVAL=n` |
| "cada lunes", "todos los lunes", "los lunes", "los lunes y miércoles", "los martes, jueves y sábados", "every Monday", "each Monday", "on Mondays and Wednesdays", "mondays" | `FREQ=WEEKLY;BYDAY=MO[,WE...]` |
| "entre semana", "de lunes a viernes", "del lunes al viernes", "los días hábiles/laborables/útiles", "días de semana", "every weekday", "on weekdays", "Monday through Friday", "from Monday to Friday" | `FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR` |
| "los fines de semana", "cada fin de semana", "every weekend", "on weekends" | `FREQ=WEEKLY;BYDAY=SU,SA` |
| "cada semana", "todas las semanas", "semanalmente", "una vez por semana", "every week", "weekly" (not after an article), "once a week" | `FREQ=WEEKLY` (the first occurrence's weekday) |
| "cada dos semanas los viernes", "cada dos lunes", "every other Monday", "every 2 weeks" | `FREQ=WEEKLY;INTERVAL=2[;BYDAY=FR]` |
| "cada mes", "todos los meses", "mensualmente", "una vez al mes", "every month", "monthly", "once a month" | `FREQ=MONTHLY` (the first occurrence's day) |
| "el día 5 de cada mes", "el 5 de cada mes", "cada 5 de mes", "on the 5th of every month", "every month on the 5th" | `FREQ=MONTHLY;BYMONTHDAY=5` |
| "cada año", "todos los años", "anualmente", "every year", "yearly", "annually", only with a named month ("cada año el 5 de marzo") | `FREQ=YEARLY` |

The argument grammar is the RFC 5545 subset `FREQ=DAILY|WEEKLY|MONTHLY|YEARLY`
with optional `INTERVAL=1..999`, `BYDAY=SU,MO,TU,WE,TH,FR,SA` (weekly only)
and `BYMONTHDAY=1..31` (monthly only), each key once, written in that order
by `repeat_rule::format` and read in any order by `repeat_rule::parse`.

Not a recurrence, and so a one-off reminder or no reminder at all, exactly as
before: "cada vez que" / "every time", "cada uno", "el lunes" / "on Monday"
alone, "este fin de semana", "el diario", "the weekly report".

**Heard but not keepable is refused, never shrunk to a one-off.** A
recurrence the reminders cannot store is still heard as one
(`RepeatReading::mentioned` without a `repeat`): any hour or minute unit
("cada hora", "cada 2 horas", "every hour"), more than once per unit ("dos
veces al día", "twice a day", "3 times a week"), vague cadences ("de vez en
cuando", "from time to time", "cada rato", "every so often"), an exception
("todos los días excepto el domingo", "except"), a yearly cadence with no
month, and mixes the store has no shape for ("el primer lunes de cada mes").
The turn answers `speech::Refused{memory.remind, unreadable_repeat}`, a fixed
line in the user's language ("No entendí cada cuánto repetirlo, así que no lo
guardé. Puedo repetir un recordatorio cada día, ciertos días de la semana,
cada semana, cada mes o cada año, por ejemplo: cada lunes a las 8."), and runs
nothing. The handler refuses the same way (`code: unreadable_repeat`) when it
is reached without the turn, and a `repeat` argument it cannot parse is
refused too.

**The time comes from the existing clock reader.** `call_time::clockOf` reads
the clock part alone (the same clock grammar as the one-off table above) and
answers a minute of the day, with the day part the recurrence named as its
qualifier ("todas las noches a las 9" is 21:00), and the bare-hour reading
used when a day is named (1 to 6 the afternoon, 7 to 11 the morning). It
never takes the evening fallback a one-off "a las 8" takes after 08:00,
because a series has no "today". The first occurrence is the next matching
instant from now in the host's local clock (Lima's in production):
`repeat_rule::firstOccurrences` returns one instant per named weekday, the
next valid month that has the named day for a monthly series (the 31st skips
November), and today or tomorrow at that time otherwise. A yearly series
anchors at the instant `call_time::resolve` reads from its date.

**The turn** (`TurnFlow::proceed`, for `memory.remind` only and not while a
slot is being answered) reads the recurrence from the user's words, writes
`repeat` into the call's arguments and skips the one-off day questions (a
conflict, a passed hour, a far date) that have no meaning for a series. A
readable recurrence with no time asks "¿A qué hora te lo recuerdo cada vez?" /
"At what time should I remind you each time?" (`AskReason::RepeatTime`, a
pending `Awaiting::Clock`); the answer is appended to the original words, so
the call runs with "recuérdame cada lunes sacar la basura a las 8" and every
slot is read again from them. An answer with no clock is asked once more and
then given up (`Misunderstood`); a new command or a no replaces the question.
The GLiNER extractor leaves `repeat` empty: the rule tier is its only filler,
and with no bundle at all the recurrence still routes and fills, which keeps
the degradation contract.

**The trigger decider** routes an explicit recurring ask to `memory.remind`
even when the phrase catalog has no trigger for it ("recuérdame cada lunes a
las 8 sacar la basura" has no "que"): the utterance must ask to be reminded
(`kReminderAsks`), must not be a question (`RuleParser::isQuestion`, which
also counts the recall markers such as "recuérdame el día"), and must name a
recurrence. A monthly series on a named day is decided before that question
test (`asksMonthlyOnADay`, G3): an ask to be reminded with no question mark,
a recurrence that reads as `FREQ=MONTHLY;BYMONTHDAY=n`, and a word after the
ask that is not a question word (que, what, cuándo, when, dónde, quién, cómo,
cuál, si, cuánto) is `memory.remind`, with or without a clock, in es and en
("recuérdame el día 5 de cada mes pagar el alquiler", "avísame el 20 de cada
mes que toca pagar el colegio", "remind me on the 5th of every month to pay
the rent"); with no clock the turn asks the time as for any series. "¿qué
tengo el día 5?", "recuérdame qué tengo el día 5 de cada mes" and "remind me
what I pay on the 5th of every month" stay questions. A readable
one that the catalog also parses as a save ("recuérdame que mi hermana viene
los domingos") needs a clock to become a reminder; without one it stays a
fact, as it was. An unreadable one always goes to `memory.remind`, so the
refusal is said instead of a fact being stored.

**The handler** (`MemoryService::handleRemind`) stores the fact as before and
then, for a series, writes one productivity row per named weekday (a series
row has one anchor, and productivity's rule grammar has no `BYDAY`, so "los
lunes y miércoles" is two weekly series, each anchored on its own day), with
`recurrenceRule` the productivity rule (`FREQ` and `INTERVAL` only:
`repeat_rule::productivityRule`; productivity also reads
`FREQ=MONTHLY;BYMONTHDAY=n` since G4, firing on the last day of a shorter
month, so a monthly series may carry its day) and the idempotency key
`memory-remind:<factId>:<firstAt>`. The title is the reminder's words without
the recurrence and the clock phrases. **No call is scheduled for a series**:
the notification service's call is a single instant, and calling only for the
first occurrence would be a promise the later ones break; productivity fires
every occurrence through its own agenda notices. `callScheduled` is false, so
the reply guard cuts any "te llamaré".

**What is said.** The tool result carries `readback`, the cadence and the time
(`reminder_readback::repeatMoment`: "cada lunes a las 8 de la mañana", "los
lunes y miércoles a las 7 de la mañana", "de lunes a viernes a las 7 de la
mañana", "el día 5 de cada mes a las 9 de la mañana", "every Monday at 8 AM",
"every weekday at 7 AM", "on the 5th of every month at 9 AM"), and the
sentence "Te lo recordaré cada lunes a las 8 de la mañana; la primera vez será
el lunes 12." / "I will remind you every Monday at 8 AM, starting on Monday
the 12th." The done act keeps the read-back and the speech guard asks for a
reply that carries it, as for a one-off. A series no row writer accepted says
"Lo guardé, pero no pude dejarlo en tus recordatorios, así que no se
repetirá."; one with no time says "No me dijiste a qué hora, así que todavía
no quedó en tus recordatorios."

Tests: `repeat-rule-test` (the phrase table es/en, the unreadable and the
one-off negatives, the clock reader, the grammar round trip, the first
occurrences), `reminder-readback-test` (the spoken cadence in both languages),
`llm-turn-flow-test` (the rule reaches the tool's arguments, the refusal, the
time question and its answer, the trigger routing) and `memory-reminder-test`
(one row per weekday with the productivity rule, no call, the refusal). The
judge corpus carries `remind-076` to `remind-082`, which check the `repeat`
argument by value.

## Audit fixes (2026-10-05, cloud audit #31-#35, #96-#99, #111)

### Caller-bound identity (#32, #99)

The gRPC leg used to accept the `user_id`, `caller_role`, `tools` and
`client_actions` any listed caller declared, so the guard credential could
run the memory tools as any user. The server now resolves which
`[rpc.callers]` entry presented the credential and passes the request
through `boundToCaller` (`feature/llm/controllers/llm-controller.hxx`):
only the `voice` caller (`kIdentityCaller`) declares a user, a role, the
tools, client actions and a session id; every other caller is a Guest
with `userId = 0`, `tools = false`, no client actions and no session.
argus-guard already sends `tools = false`, so nothing it relies on changes.

The loopback HTTP leg applies the same rule when a `voice` entry exists in
`[rpc.callers]`: a request that presents that secret in
`x-argus-credential` keeps its declared identity, any other request is
bound as an anonymous caller. The HTTP client sends `llm.grpc_credential`
in that header on both legs. With no `voice` caller configured (the native
default today) the HTTP leg keeps trusting the loopback, and boot logs a
warning; `scripts/setup.sh` still has to pair `services/llm/config.toml`
`[rpc.callers] voice` with `services/voice/config.toml` `[llm]
grpc_credential` for native installs (owned by the setup script, not this
service).

### Actions that lower security or delete need the user's words (#31)

- `app.set_guard_mode` to any mode but `armed` runs only when the user's
  own utterance is that same command (`appCommandFor(utterance)` yields
  `app.set_guard_mode` with the same mode). Otherwise the tool refuses and
  tells the model to ask the user to say it ("pon la vigilancia en modo
  casa"): that spoken command is the confirmation. The current mode is not
  known here, so every non-`armed` mode is treated as possibly lowering.
- `memory.forget` runs only when the utterance is a forget request (above).
- Camera summaries, announcements and app notes reach the model as quoted
  system notes, never as assistant turns (`services/voice/CONTEXT.md`).

### Forgetting deletes (#33)

`MemoryGraphRepository::forgetFact` deletes, in one `BEGIN IMMEDIATE`
transaction on the vec0 connection (the only one that can touch
`memory_vec`), the fact and every older version it superseded (the
`supersedes` chain), their `memory_fact_fts` entries through FTS5's
`'delete'` command, their `memory_vec` rows and their `supersedes` edges.
The profile cache of that user is dropped. `embedAndStore` checks that the
fact still exists under the vec lock before it writes vectors, so a queued
embed cannot resurrect a forgotten fact's vectors.

`memory_vec.memory_id` is now a signed key (`memory-vec.hxx`): a fact is
its id, an episode is its negated id. Facts and episodes used to share the
id space, so re-embedding fact 42 deleted episode 42's vectors and the
recall could return an unrelated fact for an episode's vector. The vec0 DDL
lives in `packages/lib/sqlite`, so the discriminator is the sign rather than
a `kind` column. Vectors are written as float32 BLOBs instead of JSON text.

A vector write (`MemoryGraphRepository::replaceVecRows`) is one
`BEGIN IMMEDIATE` transaction that first checks the memory still exists
(an open fact, or the episode), then deletes the key's rows and inserts
every view with one prepared statement. A queued embed can therefore not
write vectors for a fact forgotten while it was embedding.

**Boot migration of an older store (layout 2).** The vectors are an index
derived from `memory_fact` and `memory_episode`; the memories themselves
never move. `memory.db`'s `PRAGMA user_version` records the vector layout
(`memory_vec::kLayout`, 2). At boot, a store below 2 that holds vector rows
was written under the shared id space (and possibly as JSON text), so its
`memory_vec` is recreated and the worker's `Rebuild` job re-embeds every
open fact and every compaction episode that is not rolled up, with the
semantic dedup off (a rebuild must neither drop a fact's vectors nor bump
its priority). The layout is written only when the rebuild finished: a
stop in the middle, or no embedding model on disk, leaves it below 2 and
the next start runs it again. A store with no vector rows takes layout 2
at once. Vectors of facts that were only closed (superseded, or "forgotten"
by the old close-only `memory.forget`) are not rebuilt. The rows of such
closed facts stay in `memory_fact`, because a fact the old `memory.forget`
closed cannot be told apart from one closed for another reason. Before
4117d76d three paths closed a fact, all through the same `CLOSE_FACT`
(`valid_to = updated_at = now`, nothing else written): supersession (the only
one that also writes a `supersedes` edge from the old fact to the new),
`memory.forget`, and the note refinement in `MemoryFormation`, which closes
the refined note, or closes the new fact again when the note was already
gone, with no edge either. A closed fact without a `supersedes` edge is
therefore a forgotten fact or a refined note, and the only trace of a
refinement is a fact of the same user created in the same second, which a
forget can match by chance. Deleting on that guess could erase data the
user never asked to forget, so the migration deletes nothing; a forget
from now on deletes for real.

### `procedure.run` removed (#96)

The tool ran `MATCH` against `memory_procedure`, a plain table, so it
always failed, and the table had no `scope`. Nothing wrote procedures
(`recordProcedure` had no caller), so the tool, its handler, the
repository code and the table are gone from `database/schema.sql`. An
existing store keeps an unused empty table.

### Streams, drains and the engine (#35, N1, #97)

- `LlmService::generateStream` checks `loaded_` (now atomic), the context
  and the model after taking the engine mutex, so a generation that waited
  for the lock while `shutdown()` ran answers 503 instead of using a freed
  context.
- `main.cc` registers drains with `shutdown_signal::onStop` before
  `run()`: the HTTP streams (`StreamSlots`, whose stop makes every token
  callback abort), the gRPC server (`Shutdown` with a 2 s deadline on its
  own thread), the encounter consumer and the catalog replica (new
  deliveries are nak'd, drained when nothing is in flight) and the memory
  worker (Compact/Profile jobs dropped, drained when the worker exits).
- The encounter consumer and the catalog replica write SQLite on a Light
  `BlockingStrand` each instead of `runInLoop` on IOLoop 0. The catalog
  snapshot seed also runs inside the `BlockingTask` that fetches it.

### Cost (performance items)

- `embedAndStore` computes every embedding (chunks and the synonym view)
  outside the `VecDb` mutex; the lock is held only for the dedup probe and
  for the one replace transaction, so a background embed no longer holds
  the recall of a live turn for 50-300 ms.
- The memory worker's off-turn extraction waited for the chat engine to be
  idle on every job, so it, e5 and the main model did not run at once
  on the same cores. The job and the model were removed on 2026-10-12;
  compaction still waits for an idle engine on its `preferIdle` jobs and
  requeues when the engine stays busy.
- `ExtractionService::extractAsync` had no caller and was removed (#111); the
  rest of `ExtractionService` went with the off-turn extractor on 2026-10-12.

## The `llm` component (selectable modules, 2026-10-05)

argus-llm owns the core catalog component `llm`
(`services/settings/modules.json`; `services/settings/CONTEXT.md`,
"Modules"): `llm/LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf`. Its source is `provisioned`: only the host produces
it (`services/llm/scripts/provision.sh`), so this service reports it and
never fetches it.

- `feature/settings/llm-components.{hxx,cc}` builds the shared
  `DiskComponentHost` for `llm` with no fetch; `main.cc` attaches it to the
  `SettingsRpcService` when the settings caller is paired. The models root is
  `[components] models_dir` (`models` by default, `/opt/argus/models` in the
  deploy), the root the catalog's paths are relative to.
- `ComponentStates`: `installed` when every catalog file exists and is
  non-empty, otherwise `host_only` with the host command; `bytesPresent` sums
  the catalog sizes of the files present. `ready` is `the controller's `LlmService::isLoaded()``, never the
  files alone.
- `InstallComponent` answers `host_only` and touches nothing;
  `RemoveComponent` is refused (`INVALID_ARGUMENT`): a provisioned
  component's files are the host's and `llm` is core.
- The engine still refuses to boot without its model, as before, so a missing
  component normally shows as an unreachable owner in `GET /modules`; the
  states above answer while the service runs. Caveat: `[llm] model_path` pointing at another file loads that file; the component reads the catalog's file only.

`tests/unit/llm-components-test.cc` drives the wire in process: host_only and
the command when missing, partial byte counts, installed, `ready` following
the engine flag, install answering host_only with an untouched models dir,
remove refused with the files kept, a foreign component refused, the default
root.

## Voice quality evaluation (2026-10-06)

`tests/eval/` measures what the assistant understands, in harnesses that share corpora
(`tests/fixtures/eval/`, authored in `intent-training` and published here) and one gate file
(`tests/eval/gates.json`, one section per harness). Since the owner's decision that the LLM only
speaks, a decider picks the tool, a slot layer fills its arguments and the LLM speaks about the
result, so there is one harness per stage and each drives a process over a language-neutral
line protocol: `decider-eval.py` (coverage, precision and false-route rate per family, a confidence
sweep, an operating point chosen on the selection sets and then one reading of the sealed set),
`slot-eval.py` (arguments to the minute, a missing slot reported and never guessed) and
`conversation-eval.py` (false completion at zero, faithfulness to the result, language, length). The
sealed set `fixtures/eval/sealed.jsonl` is pinned by sha256 in `gates.json`, never opened by whoever
tunes a decider and never reported utterance by utterance; its ceiling for module-family false routes
(0.5%) is judged pooled, per family and on the authored near-miss stratum. The operating point is fitted on
the selection sets against point rates at `decider.fitWrongActMax` (half the gate), with a floor of `decider.minStratum` negatives per stratum; a Wilson
upper bound is taken only at the final read, over the near-miss stratum pooled across the sealed set and SEALED-2
(`decider.certification`), and `decider-eval-test.py` fails if the fit ever reads a bound or if the bound is
computed anywhere but the report and that certification. A process that cannot
start is a visible skip (77). The scoring is tested without a model (`decider-eval-test`,
`slot-eval-test`, `conversation-eval-test`, each mutation-checked). `production-decider --bundle <decider bundle
dir>` is the production decider stack over a bundle — rules, then Laya, the explicit triggers as the fallback —
answering `decider-eval.py`'s line protocol with the tool, the confidence, the runner-up and `now`. `llm-tier-eval`
drives the whole turn (decide, fill, run, speak) with stub tools and the real model, and it and
`call-faithfulness-eval` run the shipped pipeline through `installTurnEngines` (`--decide-bundle` and
`--extract-bundle`, by default `models/decide/bundle-laya-mvp` and `models/extract/bundle-gliner-pilot` beside the LLM
weights' models root); both print `[REFUSED]` and exit 1 rather than report a gate when Laya or GLiNER is not up.
`llm-tier-eval --no-engines` measures the rules, the triggers and the rule text alone. Why the numbers are what they are, and how they were
measured: `docs/operations/voice-quality-eval.md`.

Which tools a role is offered is data, not a table in the harness: `tests/eval/tool-visibility.json` lists, per
role, the tools whose required capability the role holds, and per tool its capability and module. It is
generated from the sources that decide it (`role-access.hxx` and `capability.hxx` for the roles and the
capability table, the six MCP provider files for each tool's `.capability` and `.module`) by
`python3 -I services/llm/tests/eval/tool-visibility.py --write services/llm/tests/eval/tool-visibility.json`,
and `tool-visibility-test.py` fails when the committed file differs from what those sources declare, when a
tool resolves to a module other than its capability's (a helper such as `spec()` that assigns the module wins over the
literal, as it does at run time), when the decider's vocabulary is not exactly the tools
the backend declares, or when a tool annotated read-only is missing from `turn/tool-effects.json` (the
harness reads its read-only list from that file, which is what the turn flow compiles). The test needs
no build. `tests/eval/eval-tools.cc` (for `llm-tier-eval`) is still a hand mirror of the providers' tool
specs; a snapshot of every provider's `tools/list` that both sides test against would remove it.

## Laya and GLiNER: the turn's engines

The owner's decision of 2026-10-12: Laya decides and GLiNER extracts, by
default, with the pilot bundles, and no configuration chooses another engine.
The engines the pilots were swapped in for are removed and no longer installed
— replaced by Laya and GLiNER, see the retirement record
[docs/history/reports/legacy-router-extractor-retired.md](../../docs/history/reports/legacy-router-extractor-retired.md)
(2026-10-17). Swapping in a newly trained model is replacing the bundle's
files.

Both engines ride the turn's own seams; nothing in the MCP path, the tool
runtime, the pendings or the audit knows they exist. `installTurnEngines`
(`services/turn-engines.{hxx,cc}`) is the one composition, called by `main.cc`
and by the evals:

- **The decider.** `turn::LayaDecider` behind `RuleDecider` in a `FirstOf`, with
  `TriggerDecider` ahead of it ("The decider: rules, Laya and explicit
  triggers").
- **The extractor.** `turn::GlinerExtractor` is the slot layer's text engine,
  behind `slots::TextSlots`. When its bundle does not load, its model cannot
  run, or it finds no span above the bundle's threshold, the rule tier
  (`slots::RuleText`) answers. `turn::ModelText` and the retired
  `ExtractionService` are gone; the memory feature's capture is the lexicon
  tier with its coverage floor (`CoveredLexiconExtractor`,
  `extract.lexicon_min_coverage`).

`TurnEngines::pipeline()` names what is up: `rules+triggers+laya` or `rules+triggers`,
then `+gliner` or `+rule-text`.

### Config keys, and their allowed values

| key | values | meaning |
|---|---|---|
| `decide.bundle_dir` | a path (default `models/decide/bundle-laya-mvp`) | the decider bundle the loader opens |
| `decide.act` / `.ask` / `.margin` | 0..1 (template 0.90 / 0.60 / 0.10) | the policy of a decider whose bundle declares none; Laya's thresholds come from its bundle |
| `decide.threads` | `0` (default) \| n | Laya's intra-op threads; `0` means 4, clamped to the host's hardware threads (`layaSessionThreads`) |
| `decide.cpu_arena` | `false` (default) \| `true` | the ORT CPU memory arena |
| `decide.mem_pattern` | `false` (default) \| `true` | the ORT memory-pattern planner |
| `decide.prepacking` | `false` (default) \| `true` | `session.use_prepacking` |
| `decide.mmap` | `true` (default) \| `false` | hand ORT the path (mmap) instead of a read buffer |
| `decide.rules.witness_only` | `false` (default) \| `true` | the rule decider's word is a second signal only |
| `extract.bundle_dir` | a path (default `models/extract/bundle-gliner-pilot`) | the extractor bundle |
| `extract.lexicon_min_coverage` | 0..1 (default 0.6) | the share of a clause the memory lexicon must cover for its triple to stand |
| `extract.threads` / `.cpu_arena` / `.mem_pattern` / `.prepacking` / `.mmap` | as above | the same five session options |

The template carries keys and values only; the allowed values are here, never
in `config.toml.example`. The compiled defaults (`src/config/llm-config.cc`) are
the same two pilot paths, so a config without the keys still loads the pilot
bundles. Gone with the old engines: `[intent]`, `decide.engine`,
`[decide.router]`, `[decide.laya]` (`bundle_dir`, `sha256`, `agent_config` and
the session options), `extract.engine`, `[extract.gliner]`,
`extract.model_path`/`prompt_format`/`gpu_layers`/`n_ctx`/`max_tokens` and
`memory.extract_wait_ms`.

### The bundle contract

A bundle is a directory: `model.onnx`, `tokenizer/`, `labels.json`,
`decision.json`, `max_len`, `model-card.md`, `manifest.json`, `sha256`, and for
a decider `questions.json` (the layout `intent-training/scripts/kit_bundle.py`
writes). `turn::BundleLoader` validates it at load with nothing configured but
its directory: every file the bundle's `sha256` file lists must match, no
unlisted file is allowed, `manifest.json` must name the model the slot serves
(`laya` for the decider, `gliner` for the extractor) and record the sha256 of
`model.onnx` (`modelOnnx.sha256`), and a decider's manifest must record the
sha256 of its `questions.json` (`questions: {file, sha256}`)
(`llm-bundle-loader-test`, one refusal per case). There is no configured pin and
no compile-time pin: the bundle is its own manifest. Validation runs **once at
load and is latched**; there is no re-hash per turn. The artefact may be INT8 or
fp32: the loader takes the file it is given and never assumes a quantisation.

`decision.json` carries the thresholds and the decode, **never config**: Laya's
`act`/`ask`/`margin`/`now` (with a `calibration` block, `platt` or `isotonic`,
applied to the raw score before the judge reads it — `turn::CalibrationModel`)
and its `head_max_len` and `temperature`, and GLiNER's
`pairThreshold`/`maxSpanWords` — the pair scorer's own vocabulary, because the
pilot is a `BoundaryExtractor` and has no span-model width axis. The thresholds
are fit on the **calibration** split only, and `manifest.json` records which
split (`calibration.fitOn`). The loader surfaces that split
(`BundleLoader::fitSplit`) from the manifest, never re-derives it, and a bundle
whose thresholds were fit on dev, OOT, IND, CH or a sealed split is refused by
Unit A's gate.

`labels.json` has **one shape per kind of bundle, and the loader refuses the
other for each** (`llm-bundle-loader-test`, one refusal per kind). A **decider**
bundle carries `{"labels": ["memory_save", ...]}`, the label array in the
model's own logit order, which the loader preserves. An **extractor** bundle
carries `{"model": "gliner", "logitOrder": "perType", "types": {<type>: [<field>, ...]}}`,
the type-keyed object whose every value is that type's logit order. The order
cannot be flattened into one list because the schema is an input: the caller
renders the picked type's own fields into the structure prompt, so the map is
what the decode indexes by (`BundleLoader::typeFields`, beside `labels`).

### Installing a newly trained model

`services/llm/scripts/install-bundle.sh <decide|extract> <bundle-dir>` verifies
the bundle with the loader's checks plus one of its own (a decider's labels all
name a tool in `services/llm/tools/laya-labels.tsv`), copies it beside the slot
(`<slot>.incoming`), verifies the copy, keeps the replaced bundle as
`<slot>.previous` and swaps; `--rollback` swaps the two back. `--slot DIR` names
the slot directly; otherwise it is `[decide]`/`[extract]` `bundle_dir` from
`services/llm/config.toml` (or `--config PATH`), defaulting to the shipped
slots. Before the swap the copy's model runs on the bundle's own golden vectors
through `llm-bundle-check` (the build's, or `--checker PATH`): a model that
misses them is refused and the slot is untouched; a bundle that carries none is
installed with a warning; `--skip-parity` installs without the checker, which
`provision.sh` uses only when argus-llm is not built yet. Restart argus-llm to
load the new bundle. `services/llm/scripts/install-bundle-test.sh` tests the
script and `scripts/build-all-test.sh` runs it.

### Precision: an fp32 or an int8 bundle

`manifest.json` declares `precision`, `fp32` or `int8` (`turn::BundlePrecision`);
any other value is refused. A bundle that predates the key is read from its
`artefactKind` (`int8-…` is int8, `fp32…` is fp32), which is how both pilot
bundles (`int8-embedding`: the embedding table alone is INT8, every MatMul fp32)
read as int8; a bundle that declares neither is refused. The loader validates an
int8 bundle exactly like an fp32 one, and the precision is printed on the
engine's ready line.

What an INT8 bundle contains is what intent-training's exporter writes today:
`onnxruntime.quantization.quantize_dynamic` (`QInt8`, per channel,
`reduce_range`; `scripts/onnx_tools.py`), so the bundle is still one
`model.onnx` with no external data and no calibration or scale file — dynamic
quantization computes the activation scales at run time — beside the same
`tokenizer/`, `questions.json` (Laya), `labels.json`, `decision.json`,
`max_len`, `model-card.md`, `manifest.json` and `sha256`. `kit_bundle.py`
(Laya) and `gliner_bundle.py bundle` (GLiNER) write that layout; neither writes
`precision` or golden vectors yet, and `tests/eval/bundle-golden.py` adds both.

The session needs no option of its own for INT8: `ORT_ENABLE_ALL` fuses the
exporter's `DynamicQuantizeLinear` + `MatMulInteger` pairs into the CPU
execution provider's `DynamicQuantizeMatMul`, the one execution provider the
service registers, so there is nothing to fall back to — a quantized operator
the CPU EP did not implement would fail `Ort::Session` creation and turn the
engine off with its log line, never run in fp32. Measured on a fully quantized
copy of the pilot (every MatMul INT8, 337 MB against 701 MB), an onnxruntime
1.24.4 profile assigns every node to `CPUExecutionProvider`, with
`DynamicQuantizeMatMul` 40% of kernel time where fp32 `MatMul` was 67% on the
pilot.

**Dynamic INT8 makes batching part of the model.** `DynamicQuantizeLinear` takes
one scale per activation tensor, over every row of the batch, so an INT8 model's
scores depend on which rows run together. laya's `ONNXAgent` runs all of a
state's questions as one batch, padded to the longest row with the tokenizer's
pad id, attention 0 on the padding, `marker_pos` 0 and `marker_mask` false past
each row's markers; `turn::LayaBundleModel` now does the same (one `Run` of
batch 2 for `tool` and `now`). Run one question at a time, the fully quantized
copy missed its own Python reference by up to 0.44 in probability and 0.74 in
`now`, with 3 of 18 decisions different; batched like the reference, the worst
gaps are 5.0e-05 and 4.6e-05 and no decision differs. On the fp32 pilot the
batched run reproduces the reference as before (worst gap 5.0e-05).

### Golden vectors: parity per bundle

An INT8 model does not reproduce an fp32 model's scores, so parity is checked per
bundle, against vectors the bundle carries. `manifest.json` records them:

```json
"golden": {"file": "golden.json", "sha256": "<hex>",
           "tolerance": {"probability": 0.002, "now": 0.002}}
```

(`{"score": 0.002}` for an extractor). The file must be listed in `sha256` under
that hash, or the bundle is refused at load. Its content:

- **Laya** — `{"kind": "laya", "reference": "...", "modelSha256": "...",
  "probes": [{"text", "lang", "sequences": {<qid>: {"ids", "markers"}},
  "tool": {<label>: p}, "choice": <label>, "now": p}]}`: the sequences of every
  question for the probe text, rendered by `laya.common.build_sequence` at the
  bundle's `max_len` and `head_max_len`, and the probabilities, choice and noul
  reading laya's `ONNXAgent` gives with the bundle's own `model.onnx`.
- **GLiNER** — `{"kind": "gliner", ..., "probes": [{"id", "text", "lang",
  "type", "fields", "inputIds", "wordIndices", "wordMask", "queryIndices",
  "queryMask", "reference": {<field>: [{"text", "start", "end",
  "confidence"}]}}]}`: the rows `intent-training/scripts/gliner_bundle.py
  fixture` writes, with the spans Python `onnxruntime` decodes from the
  bundle's own graph at its `pairThreshold`/`maxSpanWords` (flat overlap
  policy).

`turn::checkLayaGolden`/`turn::checkGlinerGolden` (`services/turn/bundle-golden.cc`)
require the inputs to be identical — the renderer and the prompt builder are
model-independent and stay exact — and the scores within the bundle's own
tolerance, with the same choice or the same spans. `llm-bundle-check <decide|extract>
<dir>` runs them on a bundle as argus-llm loads it (exit 0 passed, 1 failed, 2
invalid, 3 no golden vectors); `install-bundle.sh` runs it before the swap, and
ctest runs it on the installed bundles (`llm-bundle-check-decide`,
`llm-bundle-check-extract`, skipped while a bundle carries no vectors);
`llm-bundle-golden-test` holds the comparison to its tolerances. The writer is
`tests/eval/bundle-golden.py --kind decide|extract --bundle DIR --precision
fp32|int8 [--rows ROWS]`, run on a freshly exported bundle with the
onnxruntime `parity-references.lock.toml` pins (1.24.4, the version argus-llm
links): it writes `golden.json`, records it and the precision in the manifest
and reseals the listing. The pilot bundles carry no vectors, so their parity
stays the repository fixtures above (`parity-pilot.json` and the GLiNER
references); a copy of the Laya pilot with vectors written by the tool passes
`llm-bundle-check` with worst gaps 5.0e-05 / 4.8e-05. The shipped GLiNER bundle
*is* the pilot, so `llm-bundle-check-extract` skips and the extractor leg has no
compiled parity gate on the host: **the next GLiNER bundle must ship
`golden.json`**, written by the same tool, so that leg passes rather than skips.

### The operating point is the bundle's

The thresholds a model was calibrated for ship in the bundle's
`decision.json`, which the bundle's own `sha256` listing pins and which the
exporter writes: Laya's `act`, `ask`, `margin`, `now` and the `calibration`
block, GLiNER's `pairThreshold` and `maxSpanWords`. When `decision.json`
declares a Laya policy, that policy is the effective one for the `laya` decider
(`configuredPolicies` in `main.cc`, and the same in the evals); `[decide]`
`act`/`ask`/`margin` (0.90 / 0.60 / 0.10 in the template) and `[decide.laya]`
are only the fallback for a bundle that declares none. The pilot declares
`act` 0.77, `ask` 0.77, `margin` 0.0, `now` 0.5 on Platt-calibrated
confidence, so 0.77 is what runs; a new bundle sets its own the same way.

**A tool class acts at its own tier, and it may only be stricter.** `decision.json`
may carry `tiers`: a class names the tools it covers and the `actAtOrAbove` they
act at, in the calibrated space like every other threshold, and a class that names
no tool is the default the other tools keep. `thresholdsFor` resolves a tool to
`max(class, default)` per threshold — act, ask and margin — so a class can raise
the bar on its tools and never lower it, which is what precision first means here:
a destructive tool is never easier to trigger than the tools beside it. The read is
one function, so `judge` (`turn-flow.cc`), `LayaDecider::confident`
(`laya-decider.cc`) and the policy the composition installs all answer the same
class for the same tool. On the shipped MVP bundle that floors the destructive
class's 0.829932 up to the decider's 0.838439, and the two values are the release's
own — one from each tier's budget — never invented here.

Two lines at load say what the data did not cover (`turn-engines.cc`): one lists
every tool the bundle's labels can reach that no class names — a renamed tool falls
to the default tier and must be visible in the log, not only in the data — and one
names a class the rule floored, with the value it declared and the decider's own.
Both are warnings about the bundle, not about the turn: the turn runs either way.

### The bundles, and how they arrive

Two local artefacts, exported by the sibling `intent-training/` project and packed
into this layout by its `scripts/mvp_bundle.py`; no URL serves either, and the
repository's `models/` carries neither.

`models/decide/bundle-laya-mvp` is the Laya MVP of release `mvp-2026-10-14`: the L3 +
L7 + seed-7 soup with the embedding-only INT8 (`model.onnx` 700,845,867 B, sha256
`626d7257…`), a `decision.json` that carries the release's own thresholds into the
calibrated space the judge reads (`act` = `ask` = 0.838439, the stricter tier, both
readings recorded in the bundle) and its own golden vectors.
`models/extract/bundle-gliner-pilot` is the GLiNER pilot bundle: the epoch-7 adapter's
whole forward, embedding-only INT8 (`model.onnx` 540,153,983 B, sha256 `bd5841bb…`),
its decode the pilot's `pairThreshold` 0.1 and `maxSpanWords` 16. It is the extractor
this batch ships, the final one: no re-export is awaited, and the released
`gliner-e4-int8.zip` is not it (that artefact carries an encoder-only ONNX graph,
`input_ids`, `attention_mask` → `last_hidden_state`, not the six-input, four-output
extraction graph `GlinerExtractor` runs). A future retrain is a bundle swap like any
other: export the whole forward, quantise its embedding, install it in the same slot
with `install-bundle.sh`, and nothing in the service changes.

Both bundles carry their base model's licence text and a NOTICE as `LICENSE` and
`NOTICE`, listed in `sha256` and recorded in `manifest.json`; the loader reads neither,
and `install-bundle.sh` verifies them like every other file.

`services/llm/scripts/provision.sh` checks the two slots and, when one is missing,
installs it through `install-bundle.sh` from `ARGUS_DECIDE_BUNDLE_SOURCE` /
`ARGUS_EXTRACT_BUNDLE_SOURCE` when set, and otherwise warns with the copy step.
`argus-deploy/docker-compose.yml` mounts `models/decide` and `models/extract`
read-only into `argus-llm`; `scripts/provision-host.sh` copies the two slots into
`ARGUS_MODELS_DIR` when that differs from the repository's `models/`, falling back to
the pre-rename `bundle-laya-pilot` / `pilot-bundle` directories when the named ones are
not there; and `scripts/deploy-mounts-test.sh` checks the mounts and that the deploy
config names both bundles and selects no engine.

### The label set is the only coupling

`turn::layaLabels()` is the one map from a model label to the tool it names,
pinned beside the code as `tools/laya-labels.tsv` and asserted equal to it by
`llm-turn-engines-test`. `turn::unknownLayaLabel(labels)` answers the first
label with no entry in that map, and the composition refuses the whole bundle on
one log line rather than routing to a tool that does not exist. The check reads
the map, **never the live `ToolRegistry`**: at boot the registry holds only the
core provider (`tools.refresh("llm")`), while a module's tools — `productivity`'s
`calendar.create_event`, `camera`/`guard`'s `app.show_camera` — arrive later
through `ToolDirectory`, and only while their module is active; a boot check
against the registry turned the whole decider off on the first module-served
label (`calendar_create`), a silent fallback. A module-served label whose tool is
not offered at a given turn is already dropped by `isOffered` in
`LayaDecider::decide`, so the label map is the whole coupling and the per-turn
offer is the rest. A bundle that renames or adds a label within that map needs
no code change, and a bad one is a visible fallback, not a misroute.

### A gate run refuses to measure a fallback

A fallback is a working turn, which is exactly why it must never be measured as
the shipped pipeline. `call-faithfulness-eval` and `llm-tier-eval` build the
engines through `installTurnEngines` and print `[REFUSED]` and exit 1 unless
both Laya and GLiNER are up; `llm-tier-eval --no-engines` is the explicit way to
measure the rules, the triggers and the rule text alone. `speech-ttft-eval.py`
takes `--server-log` and `--require-engine` (repeatable) and, once the warm-up
turns that load the engines have run and before any measured request, returns
non-zero unless each required engine's ready line (`the Laya decider is ready
on`, `the GLiNER extractor opened`) is in the log — the line is written on first
use, so the check follows the act warm-up that loads the engine.
`Candidate.source` (and `Pick.source`) carry the engine and label that actually
spoke, so a fallback is visible in the trace and in the eval instead of
inferred. A number taken on a fallback is therefore never compared against a
previous run's.

### The tokenizers, named

- **Laya**: the checkpoint's own `tokenizer/tokenizer.json` — a Hugging Face
  `tokenizers` **BPE** with a SentencePiece Metaspace normalizer (space becomes
  `▁`, prepended), `byte_fallback`, `ignore_merges`, **256,000** pieces and
  580,604 merges, `<bos>`/`<eos>` added tokens, and the `PreTrainedTokenizerFast`
  for `convaiinnovations/laya-multilingual` (mmBERT-base). The fixture's
  `tokens` are `encode_text(..., add_special_tokens=False)`.
- **GLiNER**: the backbone's own tokenizer — the pilot bundle ships
  `fastino/gliner2.5-multi-v1`'s DeBERTa-v3 SentencePiece **Unigram** (250,101
  pieces, a Metaspace pre-tokenizer that prepends `▁`, no `byte_fallback`,
  `[P]`, `[C]`, `[SEP_STRUCT]`, `[SEP_TEXT]` among its added tokens). It is
  applied **word by word, to the lowercased word**, exactly as the extractor
  collector does, so every word carries its own `▁` and the splitter's offsets
  are the ones the decode maps back to characters.

### Train/serve parity, and its provenance

**The Laya sequence is the training renderer's, byte for byte (I1, 2026-10-12).**
The reference is `laya.common.build_sequence` with the question normalised by
`laya.agent.Agent._to_internal`; the port is `turn::layaQuestionFromJson` plus
`turn::layaBuildSequence` (`services/turn/laya-sequence.cc`). The frozen fixture
is `tests/fixtures/laya/sequence-parity.json`, written by
`tests/eval/laya-parity.py --bundle <bundle> --training <intent-training>` with
laya 0.3.28, transformers 5.19.0 and tokenizers 0.23.2 against the shipped
`bundle-laya-pilot` (tokenizer sha256 `dbba919c…`, `questions.json` sha256
`1ce9e1a2…`), `intent-training` at `028309b`. It renders the bundle's own `tool`
and `now` questions and six authored ones that reach every branch of the
renderer — choice criteria as an object in file order with null, empty, integer,
boolean, float, nested and escaped values; a list of labels and instructions that
carry the mask token; thirty long options; a score question; two noul questions
with keyed criteria, custom labels and structured instructions — each at three
shapes (the bundle's `max_len` 1024 / `head_max_len` 256, then 64 / 24 and
40 / 8, which reach the per-option cap, the head cap and the dropped marker),
over fifteen states (Spanish, English, runs of whitespace, emoji, the mask token,
the empty string and a ~1,300-token text): **360 sequences, all reproduced id for
id and marker for marker**, and the fifteen state tokenizations with them
(`llm-sp-tokenizer-test`). The test reads the tokenizer of the installed bundle
(`models/decide/bundle-laya-mvp`, or `ARGUS_LAYA_BUNDLE`) and says so and skips
when none is installed or when the installed tokenizer is not the one the
fixture was frozen from; a new bundle with a new tokenizer regenerates the
fixture with the same command.

What the port changed, measured on the same fixture with the previous renderer:
the shipped `tool` question was already identical in all 45 of its renderings,
but the others were not (`choice-object` 0 of 45, the two noul questions 0 of 45,
`score` 15, `now`, `choice-mask` and `choice-long` 30 each). jsoncpp sorts object
keys, so a choice question with descriptions lost its file order and with it the
logit order; structured criteria and non-string instructions were written as
jsoncpp's compact JSON rather than Python's `json.dumps` (`", "` and `": "`,
non-ASCII kept, `repr` floats such as `1e-05` and `1e+16` — `turn::layaPythonJson`);
noul criteria were read by position rather than by their `true`/`false` keys and
noul labels were not stripped; and a `head_max_len` under 16 wrapped the
per-option budget in unsigned arithmetic, so the cap never applied. The question
file is now read with an insertion-ordered JSON (`nlohmann::ordered_json`), and
every shape laya's own `_check_question` refuses is refused here too, never
rendered another way. One narrowing is deliberate: a choice label must be a
string, because laya prints a number or a bool with Python's `str`.

**One question set (I2).** The bundle carries the training run's own
`questions.json`, `intent-training/scripts/kit_bundle.py` records its sha256 in
`manifest.json` (`questions: {file, sha256}`), and the training-side decider
reads the same file from the checkpoint. The loader refuses a decider bundle
that carries no `questions.json` or whose manifest records another hash
(`llm-bundle-loader-test`), so the question set a model was trained on is the
one it is served with. The defect of record — the r1 fixture reproduced by
`laya_questions.py` and not by the checkpoint's own file — is closed with that
fixture: `parity-r1.json`, its question file and its provenance are gone.

**NFC before tokenization (I3).** Neither tokenizer normalises Unicode: Laya's
`tokenizer.json` replaces nothing but the space marker, so the reference gives
different ids for NFD and NFC spellings of one word. Normalisation therefore
belongs to the serving pipeline, before tokenization: `utf8proc` 2.12.0 (Conan,
tier-1 `lib/text`, `text_norm::nfc`) composes every message and context fact in
`boundToCaller`, which both the loopback HTTP leg and the gRPC leg run before
the turn reads anything, and GLiNER's word pieces are composed again where they
are tokenized. `tests/fixtures/laya/nfc-pairs.json` freezes Python's ids for
both spellings, and the test checks C++ NFC(nfd) and NFC(nfc) against Python's
NFC ids.

**Scores.** The installed bundle's own `golden.json` is the ONNX reference over
the graph the service loads (laya's `ONNXAgent`, onnxruntime 1.24.4, over the
bundle's `model.onnx`), and `parity-pilot.json` carries the pilot's texts and
decode for the tokenizer and sequence legs; `llm-laya-runtime-test` compares the
C++ probabilities and the noul reading with the installed bundle's golden at the
same 2·10⁻³ tolerance, and `doctest::skip` makes it a visible skip, never a
pass, when the bundle ships no vectors.

GLiNER's frozen fixture is `tests/fixtures/gliner/parity-pilot.json` (38 cases,
five types, the pair scorer's own decode), generated by
`intent-training/scripts/gliner_bundle.py fixture` against the fp32 pilot
bundle; the shipped GLiNER bundle is the embedding-only INT8 artefact
(`model.onnx` 540,153,983 B, sha256 `bd5841bb…`), and a reference frozen from it
with Python `onnxruntime` sits beside the fixture. The pilot fixture carries,
per case, the text, the type and its field order, the six input tensors (ids,
attention mask, word indices and mask, query indices and mask), the splitter's
word offsets, the decode and the reference's own prediction, so the C++ port is
checked row by row in four separate harnesses — `llm-gliner-runtime-test`, the
word split, the prompt tensors, the decoded spans, and the end-to-end turn.
`tests/fixtures/gliner/overlap.json` and `random-overlap.json` freeze the
decoder's own policy: the thirty-one corpus rows whose candidate set holds two
overlapping spans of one query, and 400 random interval sets with the
reference's answer. **On the corpus the two policies agree** — greedy
first-wins and the reference return the same spans on all 83 of those queries,
the eight rows G1b froze included, so those rows pin the decode but do not
discriminate it; **of the 400 random sets 149 do**, and a greedy decoder fails
every one of them. Both counts are frozen beside the cases and the test checks
its own reading against them (`llm-gliner-decoder-test`).

### The Laya decode, and what a bundle must declare

`turn::LayaBundleModel` (`laya-decider.cc`) is the runtime behind
`turn::LayaModel`: it loads `questions.json` from the bundle, renders each
question with `turn::layaBuildSequence`, and runs the graph once per turn, the
`tool` and `now` rows padded into one batch exactly as laya's `ONNXAgent` does
(see "Precision" above for why that matters for INT8).
The pilot's `tool` question is a `choice` and `now` is a `noul`; `qtype` is
`QuestionKind`'s own value (`Choice` 0, `Score` 1, `Noul` 2), which is the
number the checkpoint trained with. The five graph inputs are `input_ids`,
`attention_mask`, `marker_pos`, `marker_mask` (the graph's only bool tensor,
which is why `turn::OnnxInput` carries a `boolean` flag) and `qtype`; the output
is `logits`, one row per question and one score per option, and `marker_mask`
is true on each row's own markers and false on the padding.

The probabilities are `softmax(logits[:k] / temperature[qtype])`, exactly
laya's own arithmetic, and the `calibration` block of `decision.json` then maps
the maximum to the confidence the judge reads (`turn::CalibrationModel`), and
`p[1]` of the `noul` row to `Candidate.now`. `temperature` is a property of the
weights — the checkpoint's `rl_agent_config.json` carries
`[3.0035, 1.0, 5.0]` for the pilot — and at the identity default the softmax
sharpens so hard that the calibration saturates and every turn acts.
`BundleDecode` therefore reads `head_max_len` and `temperature` from
`decision.json`, and the loader refuses a decider bundle that declares only one
of them or neither. The shipped r2 bundle declares `head_max_len` 256 and
`temperature` `[3.0035, 1.0, 5.0]`.

### Precision, then latency and CPU, then memory (David, 2026-10-08)

"It's not free rein to consume what it wants — the main focus is speed and
precision." The priority order is **precision, then latency and CPU, then
memory**, and memory has a ceiling again, measured where it is spent:

- **The gate is 1 GB of warm RSS delta per model, measured inside a serving
  process — through this loader**, never in Python. `turn::OnnxSession` records
  `residentBytes()` immediately before and after `Ort::Session` is built and
  keeps the difference (`warmRssDeltaBytes()`), logs it once, and the
  call-faithfulness eval prints it per engine (`laya_rss_mb`, `gliner_rss_mb`)
  with their sum beside the process RSS. 600 MB is the **target reported beside
  the gate**, not the gate.
- The free runtime savings are how the ceiling is met without touching the
  model, and they matter more now: **CPU arena off and memory pattern off are
  the loader's defaults for both engines** (the five `[decide]` and `[extract]`
  session keys above), with
  prepacking off, mmap load and intra-op threads from `ThreadBudget`, applied by
  default **when the parity harness shows identical outputs**. Measured like for
  like on GLiNER, the two options do **not** move a first load: 1402.3 MB with
  both on against 1382.8 MB with both off, and **1398.5 MB in service**. The
  0.93 GB G1b reported beside 1.61 GB is a **second** session in one process,
  where the first session's freed pages are already inside `before` — G1b's own
  manifest records 593 MB more resident before the lean reading — so it is not
  the options' doing. fp32 misses the gate on this engine too, and a session
  option is not the fix.
- The loader loads INT8 or fp32 without caring which; Unit A's quality gates
  choose (INT8 ships within 0.5 pp of fp32 on dev, OOT, IND and CH with parity
  passing; fp32 only if INT8 costs precision and fp32 still fits the 1 GB
  ceiling and the latency gate).
- **The Laya reading, first measured 2026-10-08 on the pilot bundle** (`bundle-laya-pilot`,
  its `sha256` file hashing to `9e74075c…`, fp32, `model.onnx` 1230.6 MiB), single load in a fresh
  process with arena off, pattern off, prepacking off and mmap on: **session
  load delta 1330.1 MB, process warm delta 1522.2 MB**. The same run in service
  (call-faithfulness with the Laya engine on) prints `laya_rss_mb=1313.3`,
  `process_rss_mb=2435.5`. Turn the arena and the pattern **on** and the same
  reading is 1318.9 / 1513.3 MB — the two options barely move Laya, because the
  cost is the fp32 weights themselves and not the runtime's planning. The same
  measurement taken as a third load in one process reads 918.6 MB; that is
  glibc reusing what the previous session freed, not a property of the model.
  **fp32 therefore fails the 1 GB gate** and the embedding-only quantisation is
  not avoidable on this pair; see the unit's report.
- **The shipped Laya bundle is the embedding-only INT8 artefact** (`bundle-laya-pilot`,
  its `sha256` file hashing to
  `29b1cfaed940561e19f3649151976aaacebdc2d75bbc0601aefef480f9a946f5`,
  `model.onnx` 700,845,867 B), and it clears the ceiling where fp32 did not:
  single load in a fresh process, arena off, pattern off, prepacking off, mmap
  on, prints **session load delta 754.7 MB, process warm delta 947.0 MB**. The
  shipped GLiNER bundle is the same shape of artefact and reads **session load
  delta 810.7 MB, process warm delta 882.3 MB** one engine per fresh process,
  and the two loaded together in one `argus-llm` print `process_rss_mb=1790.6`
  beside the LLM; see the end-to-end report.
- Vocabulary pruning is dropped.

This replaces the earlier "memory is reported, not gated" wording of the same
day.

### The merge gate, and how to pass it (ruled 2026-10-12)

**Superseded 2026-10-16.** The owner's ruling makes the released INT8 bundles
(`mvp-2026-10-14`) the initial MVP models and drops the quality gate this
section describes: no `llm-tier-eval` comparison against master, no
`selection.accuracy` floor, no sealed-set validation and no re-scoring of the
int8 exports. What is required instead is integrity — the bundles in the
installer layout, installed through `install-bundle.sh` with their golden
vectors passing, the degradation contract, the engines the pilots displaced
gone from build and provisioning, the llm suites green with a prod boot, and
RSS and decide/extract p95 reported rather than gated. The text below records
the gate as it stood on 2026-10-12.

The cutover branch is ready and parked: it is merged only when the owner's
trained INT8 bundles beat master where master works today (the no-regression
rule). With the pilots, the shipped pipeline drops `memory.recall` recall from
0.520 to 0 and `memory.remember` from 0.228 to 0.025, and `selection.accuracy`
falls under its gate, because the pilot Laya scores plain memory statements and
questions below its 0.77 operating point and only explicit triggers still route
them.

**The bar** — master `ef85ad81` as it stood before the cutover, `llm-tier-eval`
in a prod build, 789 cases, beside the branch on the pilots; its readings are
in the retirement record
([docs/history/reports/legacy-router-extractor-retired.md](../../docs/history/reports/legacy-router-extractor-retired.md),
"Superseded merge-gate bars").

`call-faithfulness-eval` (prod, 32 cases, temperature 0.3, seed 42) is the same
on master and on the branch: casePass 29/32, and four gates fail on both
(`missedNameAsk` 1, `recital` 1, `relevance` 1, `retryShare` 0.333 against
0.05) — not the cutover's.

**The gate.** The new bundles install through `install-bundle.sh` with their
golden vectors checked, and in prod `selection.accuracy` ≥ 0.55, every memory
metric at least master's value, and `call-faithfulness-eval` unchanged
(casePass ≥ 29, no gate failing that does not fail on master). Then the series
is merged and pushed in one go.

**The procedure**, from the repository root of the branch's worktree:

1. Rebase on master and check the series still applies: `git rebase master`.
   `scripts/lib/tidy-baseline.txt` is the usual conflict: take master's file,
   then re-measure, never hand-merge — scan the llm project on master and on the
   branch with `scripts/lib/tidy_scan.py`'s logic, lower each count by the
   findings that disappear from `services/llm` files only, and set `tus` to
   master's value minus the TUs the branch removes plus those it adds (this
   series: −9 +5).
2. Build: `HEAVY_MIN_FREE_GB=6 ~/.cache/argus-scratch-backup/heavy-gate.sh 6
   ./scripts/build-all.sh dev --only llm --jobs 6` (0 errors, 0 warnings), then
   the same with `prod` for the evals.
3. If the exporter has not written them, add the golden vectors and the
   precision to each exported bundle (onnxruntime pinned to 1.24.4, the version
   argus-llm links): `python3 services/llm/tests/eval/bundle-golden.py --kind
   decide --bundle <laya> --precision int8` and `... --kind extract --bundle
   <gliner> --precision int8 --rows <gliner_bundle.py fixture rows>`.
4. Install: `services/llm/scripts/install-bundle.sh decide <laya>` and
   `services/llm/scripts/install-bundle.sh extract <gliner>`. Each verifies the
   bundle, runs `llm-bundle-check` on its golden vectors before the swap, and
   keeps the pilot as `<slot>.previous` (`--rollback` restores it).
5. Measure in prod:
   `ctest --test-dir services/llm/build/prod -R '^(llm-bundle-check-decide|llm-bundle-check-extract|llm-tier-eval|call-faithfulness-eval)$' --output-on-failure`
   (both bundle checks must pass, not skip), then the decider on real traffic:
   `python3 -I services/llm/tests/eval/decider-eval.py --decider
   "services/llm/build/prod/tests/eval/production-decider --bundle
   models/decide/<slot>" --gates services/llm/tests/eval/gates.json --select
   services/llm/tests/fixtures/eval/cases.jsonl --traffic
   services/llm/tests/fixtures/intent/eval-production.tsv` (the pilot's
   reading: 57.4% ACT, memory coverage 60.3%, 0 false actions), and the decide
   latency of the "Decide latency" table below (the budget is p95 150 ms).
6. Gates of the merge: `--only llm`, `scripts/build-all-test.sh`, and the
   comment, deps and routes gates; then the reviewer merges and pushes the
   series.

### Decide latency: the pilot's recorded gap (2026-10-12)

The budget is p95 150 ms per decide (`gates.json`, `performance`). Measured on
the development machine (Ryzen 7 5725U), idle, 200 sequential requests from
`cases.jsonl` through `production-decider`, pinned to eight logical CPUs:

| build | run | threads | p50 | p95 | RSS |
|---|---|---|---|---|---|
| dev | batched | 2 | 1188 ms | 1251 ms | 1004 MB |
| prod | one run per question (before) | 2 | 221 ms | 233 ms | 969 MB |
| prod | batched | 2 | 297 ms | 311 ms | 985 MB |
| prod | batched | 4 | 195 ms | 205 ms | 981 MB |
| prod | batched | 8 | 200 ms | 209 ms | 975 MB |

The time is the ONNX run: Python onnxruntime 1.24.4 alone gives the same
numbers, the sequence build is negligible in a release build, and the pilot's
fp32 `MatMul`s are 67% of kernel time (only its embedding table is INT8). Graph
optimisation is already `ORT_ENABLE_ALL`, prepacking does not move it and more
than four threads do not help, so the production default is four threads
(`decide.threads = 0`), clamped to the host. **The pilot ships at about 205 ms
p95, over budget: a recorded, temporary gap** — the owner made the pilot the
default now, and his INT8 encoder is the fix. Batching costs the fp32 pilot
about 6% at four threads (35% at two) and is kept, because it is the
reference's behaviour and what INT8 parity needs.

**The budget the MVP ships with** (measured 2026-10-17, integration pass, prod
build, this machine): the shipped INT8 Laya (`bundle-laya-mvp`, `model.onnx`
`626d7257af95`, 668 MB) through `production-decider --bundle` and the project's
perf harness, 200 requests after 20 warm-up turns, idle machine, four threads —
**p95 193.5 ms, max 197.1 ms** on the decisions the rule and trigger tiers
leave (the tiers answer the median request in 0.18 ms themselves), 328.9 CPU ms
per call, **972 MB resident** (peak 1008 MB) and 3.9 s to load; in service the
Laya session reads a 782–799 MB warm RSS delta, inside the owner's 1 GB-per-model
ceiling. It is a budget and not an expectation: the earlier "1.5–2× and half the
model" **did not hold and could not**, because the pilot already ships the
embedding-only INT8 artefact — the release's INT8 file is the same 668 MB and
the model path reads the pilot's own bracket (205 ms p95). `gates.json`'s
`performance` block still carries the pre-MVP 150 ms / 600 MB figures and no
test runs it. The bundle reproduces its own golden vectors — 18 probes, 0
decisions differing, worst probability gap 4.98·10⁻⁵, worst `now` gap
4.97·10⁻⁵ against the 2·10⁻³ tolerance.

### The C++ tokenizer and the GLiNER decode

Neither model's tokenizer is covered by the memory feature's `UnigramTokenizer`:
that class hardcodes a whitespace-collapse normalizer, ignores the file's own
`normalizer`/`pre_tokenizer`, has no byte fallback and no `ignore_merges`. What
covers both is `turn::SpTokenizer` (`services/turn/sp-tokenizer.cc`), driven by
the bundle's own `tokenizer.json`:

- **Laya** — `convaiinnovations/laya-multilingual` at the pilot checkpoint: a
  Hugging Face `tokenizers` **BPE** (SentencePiece-style), a `Replace` normalizer
  (" " → "▁"), a `Metaspace` pre-tokenizer, `byte_fallback`, `ignore_merges`
  false, `fuse_unk`, 256,000 pieces and 580,604 merges, `<bos>`/`<eos>`/`<mask>`.
- **GLiNER** — the mDeBERTa-v3 backbone: a **Unigram** model, 250,101 pieces, a
  `Sequence` normalizer (whitespace collapse, NFC, right strip) and a
  `Metaspace` pre-tokenizer. `SpTokenizer` reads both families from the same
  file; its NFC step is a no-op, because composition runs before it (I3 above).

`turn::glinerDecode` (`services/turn/gliner-decoder.cc`) is the pure decoder:
sigmoid, per-query threshold, `max_width`, the three overlap policies and the
half-open token→character mapping. **`Flat` is gliner2's `flat`, which
normalises to `disallow`: weighted interval scheduling, not greedy first-wins.**
The DP is the reference's own — exact-boundary duplicates collapsed to their
highest ranked representative, a predecessor table over the spans sorted by
`(end, start, -score)`, and the tie-break chain score, then set size, then the
best-ranked selection — and `Longest` is the reference's other policy (a span
strictly contained by another is dropped). `llm-gliner-decoder-test` holds it to
the eight frozen rows where greedy and the reference disagree
(`tests/fixtures/gliner/overlap.json`) and to 400 random interval sets
(`random-overlap.json`).

## The Vulkan shader cost, paid once per machine (2026-10-19)

On 2026-10-09 the vendored llama.cpp cost about 425 s of glslc edge time in every
fresh build tree (137 generation edges, 1922 shader variants), and `services/vlm`
paid its own copy. The shader generation is not ccache-able and llama.cpp b10305
has no variable for a prebuilt generator, so the cost was moved instead of fought:
`argus_vendored_llama()` (`cmake/argus-vendored.cmake`) builds llama.cpp once per
machine and build type at configure time, with `LLAMA_BUILD_MTMD` ON, into
`~/.cache/argus/vendored/llama-<type>-<key>`, and both this service and `services/vlm`
import it from there (`find_package(llama CONFIG)` plus an imported `mtmd`). A new
worktree or a deleted `build/` reuses the prefix in under a second;
only a new submodule commit, a different option set or another compiler builds it
again. The build type follows the consumer on purpose: a Release `libllama.so`
imports `std::string` members that a Debug service's archives define weakly, and
mold then extracts those members (turn engines, GLiNER, all of onnxruntime) into
every test, which measured 90 MB → 1 GB per executable.
`docs/history/plans/build-speed-plan.md` holds the numbers.

## Merge gate for the parked Laya/GLiNER branch

- Rebase over b8f32d76's duress screening. The controller hands each identified caller's words to the
  duress screen (`src/feature/duress/`) before the turn is decided, so the cutover branch's turn changes
  must keep that order.

## First-token latency on prod (2026-10-15)

Measured on the prod build (`./scripts/build-all.sh prod --only llm`), the machine quiet, with a copied config on its own port and the models linked; no phone. Machine: AMD Ryzen 7 5825U (8 cores, 16 threads), 30 GB RAM, the iGPU (RADV Renoir, Vulkan) visible to llama.cpp but not used (gpu_layers 0: the hardware probe reports a GPU only in builds with ARGUS_HAS_NCNN_GPU). Load at load time: threads 4, batch threads 8 (all 16 CPUs).

Representative turn: Spanish system text, a memory block, a five-turn history, a short question. The stream's sentinel gives prompt, reused and decoded tokens; the first token is the first streamed piece.

- Cold (no reusable prefix, 347 to 348 prompt tokens, full prefill, 10 runs): first token p50 1.45 s, p95 1.49 s.
- Warm (the history grows one turn per run, 344 to 659 prompt tokens, 341 to 637 reused, 22 to 38 decoded, 10 runs): p50 0.72 s, p95 0.76 s, excluding the first run, which had no reuse (1.50 s).

The cost is the prefill, about 250 tokens per second on this CPU. Reuse of the common prefix is what keeps a warm turn near 0.7 s. The target is first token under 1.5 s on prod; the cold p95 sits at that line, so a prompt that grows past about 350 tokens without reuse goes over it. The prefill log line (prompt, reused, decoded, ms, batch threads) is what shows which case a live turn is in.

The dev build is not the reference: it measured 26 to 48 s turns, and its process was pinned to CPUs 8 to 15 (8 effective threads, 2 decode and 4 batch), on top of the debug build.

## Eval gates red with the weights present (backlog, 2026-10-16)

`llm-tier-eval` and `call-faithfulness-eval` exit 77 (skipped) when the LLM weights, a turn engine's bundle, or a debug build is present in the wrong way. Where the weights and the bundles are present (the prod build, the stack's models), both run and their gates fail. The failure is not a batch-3 regression: the same numbers come from `ca416a52` and from `6176b907`, both built fresh with the same models (`LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf` and the same engine files). Nothing in `services/llm` changes between them but a log line.

- `llm-tier-eval`, memory selection and recall (both commits): `selection.memory.accuracy` 0.241379 against 0.600; `tool.memory.forget.recall` 0.156250 against 0.600; `tool.memory.remember.recall` 0.227848 against 0.500; `tool.memory.remind.recall` 0.013333 against 0.750; `falseCompletionRate` 0.042 to 0.057 against 0 (en, neutral, pe, stt variants).
- `call-faithfulness-eval` (both commits): `retryShare` 0.333333 against 0.050; `missedNameAsk`, `recital` and `relevance` 1.000000 each against 0 (32 cases, 34 turns).

The gates stay as they are. Until this is worked, the build that runs these evals with the weights present exits 8 on exactly these two tests; the same build with the weights absent reports them skipped. The memory selection is the first to look at: it is the one whose recall tools fail, and the faithfulness failures are the retry share of the same turns.

### What the first working pass measured (2026-10-16)

The memory selection is green: the decider of that day's router marked its candidate exact when its own arbitration decided
(see "The turn" above), and the same prod build that measured 0.241379 measures `selection.memory.accuracy`
0.620690 with every memory recall and precision inside its gate; the details are in the commit and the
unit's report. `falseCompletionRate` stays red, and the unit's dump shows why it is not the memory path's:
the metric is the old tool-loop marker list applied per sentence with no negation rule, so it fires on
sentences the shipped gate is right to leave alone — "la lavadora no está registrada en el sistema"
(`registrada`), "we seem to have forgotten the toolbox" (`forgotten`), "estoy listo para ayudarte, pero el
módulo productividad no está activo" (`listo`), "it was completed in 1889" (`completed`), "which modules are
active" (`active `) — while one real miss remains in the shipped gate itself: `kEnglishRequests` has no
"erase", so `asked` is false on "…, erase it" and the guard lets "That's done, I've cleared the home
insurance details." through. Both are follow-ups, not measurement changes.

`call-faithfulness-eval` (prod, 32 cases/34 turns, `--temperature 0.3 --seed 42`): the three slot turns
that retried were the guard doing its job on a bad question — "Necesito confirmar la fecha y hora…"
(`not_a_question`) and "¿Cuál es la tarea que falta en la nota de la app?" (`slot_not_asked`, which the
cue list did not know) — so the `title`/`name` cue lists now carry the asks the model actually produces
("que tarea", "cual es la tarea", "nombre de la tarea"; "what task", "which task", "task name") and the ask
situation names the missing slot twice ("falta el nombre de la tarea; pregunta solo por el nombre de la
tarea, en una sola pregunta corta"): `retryShare` 0.333333 to 0.111111 on the shipped corpus (one turn,
`es-slot-task-turn`, still speaks a statement first). `missedNameAsk` on `es-name-unknown`, `relevance` on
`es-greeting-name` and `recital` on `es-memory-only` are the model's own misses on corpus rows added after
the pins were measured (the U19 prompt items); their fix is the call prompt
(`services/voice/src/feature/voice/call-history.cc`, another unit's surface in this batch) or the corpus
itself, never the fixture.

## The conversation's stored text (2026-10-14)

The conversation stores what the screen let through or rewrote, never the raw
utterance when the screen changed it. The chat path screens the turn first, and
only then opens the conversation (the leg takeover and the tail), so the stored
user turn and the model's last user message are the same text: the screen's text.

- A pass returns the utterance unchanged (the duress screen does this), so the
  stored text is the spoken text of the request.
- A rewrite that does not withhold replaces the request's user message, and the
  stored turn is the rewrite.
- A withheld turn is answered with the withheld speech, opens nothing, records
  nothing and does not change the active leg.

The reply is stored as the assistant turn on the same leg, only when it is not
empty. Test: llm-controller-conversation-test (the rewriting screen case, and the
withheld case).

## The messenger and the app's conversation thread (F8/F9, 2026-10-19)

- **One conversation per person.** The `conversation` feature (from `track/b4-conversations`) keeps the turns, the
  active leg and the switch note per user; `voice.use_conversation` stays off until the conversation path passes
  its tests (ruling Q16). Memory facts carry a scope (`personal` by default, `household` by cue) and recall reads
  the listener's personal facts and the household's, never another person's.
- **The app's thread.** Every identified turn (a `userId` bound by the `voice` caller, not withheld by the duress
  screen) is mirrored to argus-notification's inbox through `ConversationTranscript` (`AppendTurns`), off the turn
  and best effort, with `expires_at` from `conversation.retention_days`.
- **The relay.** `message.relay` is chosen by the rules tier (`relay_command`: "llama a X y dile que", "dile a X
  que", "avísale a", "mándale a", "cuéntale a", "tell X that", "let X know that"; "dile que" alone is an answer) and
  never by the LLM. The refusal leg lets it through when the tool is offered and refuses impersonation in any case
  (`impersonation`: on behalf of someone else, hiding the sender). The tool resolves the recipient without side
  effects and reads the words back; only the sender's yes in the next turn, with the one-use code bound to that
  recipient and those words, sends it (consent). `RelayService` keeps the reply window in `relay_thread` and hands
  the letter to the `MessageCourier`, argus-notification's `RelayMessage`; a message said in a live session is also
  noted in the recipient's conversation. Answers name the message they answer; the hourly sweep settles expired
  threads in the inbox.
- **Typed messages do not pass through here.** The app's `POST /conversation/{counterpartId}/message` is
  argus-notification's own route over the same inbox `relay` (`services/notification/CONTEXT.md`, "A typed
  message"): it opens no `relay_thread`, so "respóndele" by voice finds only the spoken relays, and its hourly cap
  is the inbox's, beside `messenger.max_relays_per_hour`.
## Aiming a camera and camera maintenance are rules, never the model's choice (2026-10, AL-B7 and TP-A5)

`tools/sight-command` is a rule tier the `RuleDecider` asks after the app commands. "mira hacia la puerta" and "gira la cámara de la sala a la entrada" become `camera.goto_preset {camera, preset}` (a camera word or `hacia`/`towards` is required, so "mira a mi hermana" stays conversation); a direction ("a la izquierda", "up") or the home words ("su posición de vigilancia") become `camera.look`. "voy a limpiar la cámara del patio" (a future word plus a maintenance verb, and a camera or `mantenimiento`) becomes `guard.maintenance_start`, with "por 30 minutos" as `minutes`; a finished word with a camera or maintenance word ("ya terminé con la cámara") becomes `guard.maintenance_end`, and a bare "ya terminé" stays with the other deciders; "¿por qué me avisaste?" and "¿quién movió la cámara?" read `guard.maintenance_status` (read-only in `tool-effects.json`). As every rule, each routes only when the tool is offered to the caller (`camera.aim`, `guard.maintenance`). `llm-sight-command-test` pins the phrases; not run in T2.

## Where is someone: a rule, for the Owner only (2026-10, G3)

`tools/whereabouts-command` is a rule tier the `RuleDecider` asks after the sight commands, and it routes `guard.where_is` (argus-guard's MCP tool, read-only in `tool-effects.json`, capability `presence.read`: the Owner alone) only when the tool is offered, so for anyone else the refusal leg has already answered. It reads, with or without a question mark:

- a person: "¿dónde está David?", "¿dónde anda Luis?", "¿en qué parte de la casa está Marta?", "where is Ana?", "where's David right now?" (`person`), and "¿está David en casa?", "is Ana (at) home?" when the sentence opens with the verb;
- a room: "¿quién está en la sala?", "¿quiénes están en la cocina?", "¿hay alguien en el garaje?", "who is in the kitchen?", "is anyone in the living room?" (`room`), only for the rooms the guard knows (sala, salón, living, cocina, oficina, estudio, despacho, garaje, cochera, depósito and their English names);
- the household: "¿quién está en casa?", "who's home?" (no argument).

A name is one or two words with nothing after them but tails (ahora, mismo, now, right, en este momento, please...), and never an article, a possessive, a pronoun, a relation word or a camera: "¿dónde está la cámara del patio?", "¿dónde está el hielo?", "where is justin bieber's home base", "Where is he?" and "¿hay alguien en el patio?" (a camera question) are not routed. The guard answers the room, home or away and how sure Argus is (`services/guard/CONTEXT.md`, "Where is someone"); the model only speaks that answer. `llm-whereabouts-command-test` pins the phrases, the negatives of the sealed set and the offer check; `llm-turn-flow-test` the whole turn for the Owner and the refusal for a Resident. Not run in G3.

## The camera surface (2026-10, CC5 of the camera call plan)

`ChatRequest.surface` (`ConversationSurface`: `app`, `camera`, `camera_visitor`; wire field 19 and HTTP `surface`) tells argus-llm the turn comes from a camera conversation. `ToolAudience` carries it, and `ToolExecutor::execute` refuses any decided tool whose capability is outside `kCameraWatchCapabilities` (camera) or `kCameraVisitorCapabilities` (visitor). For a member on a camera the refusal sends a `watch_confirm` notification to their own phone (`infra/watch-confirm-notifier`, data `{kind, url, tool, arguments}`, `url` an in-app path) and answers `confirm_in_app`; otherwise it answers `camera_floor`. The offer is unchanged, so the decider still recognises the request and the refusal is spoken about, never silently dropped.

## Privacy and safety at the camera (2026-10, gaps G2)

- **The minor surface.** `ConversationSurface::CameraMinor` (`camera_minor`, wire `CONVERSATION_SURFACE_CAMERA_MINOR = 3`, HTTP `surface`) is what argus-voice declares for a household member the Owner marked as a minor (`services/identity/CONTEXT.md`, "Minors"). `ToolExecutor` runs only the tools whose capability is in `role_access::kCameraMinorCapabilities` (`assistant.voice`, `reminders.read`, `reminders.write`, `agenda.read`, `projects.read`, `safety.panic`, `safety.read`, `notifications.read`): never `camera.view`, `safety.respond` or a write to the agenda or the projects, and never a `watch_confirm` to the phone; the refusal is `camera_floor`.
- **A minor's memory.** `MemoryService::setLearningGate` holds a rule the `memory.remember` tool asks before it forms a fact: `MinorLearningGate` (`feature/memory/infra`) reads identity's `ListPrivacy` (the messenger's identity client) at most every `[memory] minor_refresh_ms` (60000, settings catalog group `memory`) and refuses a user identity marks `minor` without `guardian_consent`, with the code `learning_off` and "No puedo guardar eso en la memoria sin el permiso de tu tutor." When identity does not answer, the last answer stands; before the first answer nothing is learned. Reminders the minor asks for are not learning and still run. No identity target configured, no gate.
- **The anonymous duress screen.** A camera speaker nobody recognised (no user id, a camera surface) is screened too: `ScreenRequest.surface = 4` (HTTP `surface`) and `ChatRequest.surface` mark it, `screenTurn` passes `anonymousCamera`, and `DuressScreen::screenAnonymous` withholds the words when the household's phrase is in them. llm raises nothing for that speaker (`RaiseSpoken` needs a member): argus-voice raises the camera duress itself through guard (`services/voice/CONTEXT.md`, "Duress from someone nobody recognised"). Before guard's key is known the words go on as heard and nothing is queued, since there is nobody to raise for.
- **Only voice declares a camera.** `boundToCaller` now resets `surface` to `app` for every caller but `voice`, so no other caller can ask the anonymous screen whether some words are the phrase.
- **Tests.** `llm-duress-screen-test` (the anonymous camera speaker withheld with no raise, the turn screen, the caller binding, no key yet; the minor's `memory.remember` refused and the gate's identity answer and its last answer), `llm-tool-runtime-test` (the minor set at a camera), `capability-test` (the minor set inside the camera set).
