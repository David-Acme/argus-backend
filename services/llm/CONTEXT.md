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
  `[intent]` (the router's model path and operating point) +
  `[server]` (loopback listener, default 7032) + `[rpc]`/`[rpc.callers]`
  (the gRPC face). The `[server]`
  listener is internal-network only: the wire is never announced or
  published; so is the gRPC one, which is plaintext and gated by the caller
  credential alone. The database is `[memory]` (`db_file =
  "database/memory.db"`, `schema_file = "database/schema.sql"`) with the
  extractor's `[extract]` beside it, and `[nats]` is the bus the catalog
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

## The memory stack (Phase 4 step 7)

`packages/memory` and `packages/intent` became features of this service:
`src/feature/memory/` (`argus::memory`) and `src/feature/intent/`
(`argus::intent`), with the schema at `database/schema.sql` and the
provisioning in this service's `scripts/provision.sh`. Nothing links them
from outside — the memory stack's only caller is the assistant turn beside it and
the intent router's only caller is this feature's gate — so the two
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
  `EntityResolver`, `GraphRecall`, `MemoryFormation`, extraction (NuExtract
  gguf + lexicon/tiered/temporal) and embeddings (onnxruntime + unigram
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
  64): at the bound, non-extract jobs drop with a WARN and an extract job
  evicts the oldest non-extract job.
- **Models stay in the shared `models/memory/` and `models/extract/` trees** —
  never copied — and arrive through this service's `scripts/provision.sh`,
  beside the LLM and intent artifacts.
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

The compose mounts the shared `models/` subpaths (`models/memory` +
`models/extract`) and the memory database into this service's working
directory — the ONNX/GGUF artifacts and `memory.db` are read relative to the
config keys.

### Stranger isolation (camera guard)

The memory stack serves authenticated users. A person the camera sees is
foreign to the system: their speech, intents and recall requests must never
become facts, preferences, reminders or episodes. `captureExplicit`,
`captureImplicit`, `observeSystemEvent` and every tool handler reject
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

## The intent router (Phase 4 step 7)

`packages/intent` became `src/feature/intent/` (`argus::intent`), the fast tier
of the router `argus::llm`'s gate drives. The rules (`argus::lib::phrase`) and
the fastText model each propose; `IntentRouter::decide` arbitrates:

1. an explicit trigger ("recuerda que", "remind me that") decides alone, even
   against the model, because the user asked for a write in so many words;
2. otherwise a model score at or above 0.90 with a 0.10 margin decides, and
   outranks any rule that proposed another class;
3. otherwise a rule proposal (a statement, a recall marker, a cancellation)
   stands only if the model's own top class is the same and scores at least
   0.50 (`kAgreeFloor`);
4. otherwise the router abstains and the turn goes on without a tool; the LLM only speaks.

A fact becomes a reminder only when the utterance asks to be reminded
(`recuérdame`, `avísame`, `remind me`, an alarm or timer) and names a single
instant; "apunta que el mecánico llega el martes" is a fact. Six classes
(`memory_save`, `memory_recall`, `reminder_set`, `memory_forget`, `camera`,
`none`); the adapter routes only the first four, and `camera` is a decoy class
that keeps camera asks out of `memory_recall`. `none` means the fast tier
abstains: agenda, calendar, task and project utterances carry it because the
productivity tools sit behind a module the classifier cannot see.
`IntentDecision::source` records which proposal decided.

Why the arbitration: measured over 629 distinct judge utterances
(`tests/eval`, `docs/operations/voice-quality-eval.md`), the rules decided
alone with a precision of 0.44 (statements) and 0.31 (recall markers) while
the model decided with 0.96; "remind me to call my mom" was a recall, "who's
at the door" was a saved fact, and a calendar request was a reminder, so 13%
of the turns that should fall through to the LLM were routed to a memory tool.
With the arbitration the same set routes 1.4% of them wrongly. Operating point
0.90 / 0.10, precision-gated: a false `none` costs one LLM round trip, a false
tool call writes a fact nobody stated. **Degradation is a contract**: no model
on disk, or a sub-threshold score, and only an explicit trigger can still
route; every other turn reaches the LLM exactly as before the router existed.
The model is a published artifact carried in-repo (`models/intent/intent.bin`,
12.9 MB) with a configure-time SHA256 pin in `src/feature/intent/models/`;
training lives OUTSIDE this repo, in the sibling `intent-training/` project, and
only the artifact, its card and the frozen eval fixtures (`tests/fixtures/intent`,
`tests/fixtures/eval/cases.jsonl`) cross over. `argus-deploy` mounts
`models/intent` into `argus-llm` (`scripts/deploy-mounts-test.sh` checks it);
before that mount existed the fast tier was off in Docker. `fasttext` is built
from the `third_party/fastText` submodule (inference only, static lib) by this
service's project file, the way it bootstraps llama.cpp and sqlite-vec.

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
  time creates no row.
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
- **The router is one decider.** fastText still decides explicit commands and
  abstains otherwise (`services/llm/src/feature/intent`); `RouterDecider` hands
  its decision to the turn, which judges it with its own thresholds, and a routed
  call goes through the same executor, so permissions, the module check and
  grounding apply to it as to any other.

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
   `confidence`, its own `id()` (`rules`, `router`), the `runnerUp` (another tool and its own
   confidence), `exact` (the decision came from words that name the tool, not from a score) and
   `confident` (the decider's own gate said act). `RuleDecider` is `appCommandFor` and the module
   command rules (`tools/module-command.cc`); `RouterDecider` wraps the fastText `IntentRouter` and
   names the four memory tools, with the argument being the user's own words and the runner-up class
   as `runnerUp`; `FirstOf` stacks deciders, so a rule, the router and a fine-tuned model are
   interchangeable and stackable behind the same interface. A decider that is not here (a learned
   model) is one more `Decider`; nothing else changes.
   **The judgement.** `DecisionPolicy{act, ask, margin}` is data, per decider: `[decide]` is the default and
   `[decide.<id>]` overrides it (`LlmConfig::resolveDecision`, `PolicySet`, main.cc composes them).
   `judge` answers **Pass** below `ask` (plain conversation; if the user asked for something, the
   speaker is told nothing matched and that it must not say it did anything), **Choose** when the
   runner-up is itself above `ask` and closer than `margin`, **Act** at or above `act`, **Ask**
   between. A decider that does not trust its own decision (`confident = false`) never reaches Act.
   A tool that writes or destroys acts only when the candidate is `exact` or a second signal agrees.
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
2. **Slots** (`slots.{hxx,cc}`, `slot-lexicon.cc`, `model-text.{hxx,cc}`). The fields to fill are
   the candidate's `fill` plus the schema's required properties not yet present. A date-time goes
   through the same normalization the executor uses (`call_time` over the user's words, ISO
   accepted); a module is matched against the snapshot's names; a title or name comes from
   `RuleText` (what is left of the sentence once verb, noun and time are taken out) and, where the
   rules find nothing and NuExtract is already loaded, from `ModelText`, accepted only if every
   word of it was said. NuExtract is never loaded inside a turn: asking is cheaper than loading.
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

The questions and the notes are one thing now: the acts. `speech-render.cc` composes one instruction
line per act per language — the only copy the backend owns, and the user never receives it — one line
of act JSON (a `DatePart` is resolved to its spoken surface against `now` at render time) and the
context block. `speechOf` places the act on the outcome, so a unit test asserts the exact act a path
reaches and never a sentence. `turn-texts.{hxx,cc}` is gone.

Configuration (`config.toml.example`, deploy template): `[decide]`, `[decide.router]` and `[decide.rules]` with `act`,
`ask`, `margin` and `witness_only`. Absent or invalid keys leave the default policy (`act = ask = 0.90`, no margin),
which is the router's published operating point with no question band.

`tests/unit/llm-turn-flow-test.cc` pins the stages: the judge and the per-decider policies, deciders
that stack and abstain, the rule decider offering only offered tools, the router decider carrying no
code, the title and time stored and spoken back, a missing time, title and module asked and answered,
a new command replacing a question, the ask band and the choice (yes, the other one, no), the write
guard with and without a second signal, previews that never leak the code, confirmed with the stored
code and refused after a no, a module offered and then enabled or requested by role, an anonymous
caller, the speak prompt with no tool list, a question answered without a generation (sync and
streamed), the claim guard after a failed tool, a preview and a read, and the prefill prefix.
`llm-module-command-test.cc` pins the rule tier (tuning numbers, not a held-out result); the sealed
set is judged by `tests/eval/decider-eval.py`.

## The per-turn context block (2026-10-08)

A call used to hand the model every fact the app carried — the camera list,
the guard mode, the agenda — on every turn, baked into `history[0]`.
On a greeting, a joke or a question nothing has an answer for, that is noise
the 1.2B model recites or is distracted by, and it is prefill every turn pays
for.

`turn::ContextSelector` (`services/turn/context-selector.{hxx,cc}`) decides,
per turn, which context facets that turn may see. The facets are
`ContextFacet` (`facet-lexicon.hxx`): `Camera`, `Agenda`, `Guard`,
`Reminders`, `Memory`, `Modules`. The decision is one the turn already made:
the decided tool's facet (`facetForTool`: `calendar.*` → agenda,
`memory.remind`/`reminder.list` → reminders, `memory.*` → memory,
`modules.*` → modules, `app.show_camera` → camera, `app.set_guard_mode` →
guard; `task.*`/`project.*`/`app.open` imply nothing) unioned with the facet
words and phrases the utterance itself names (`facetsInText`, es and en), so
an agenda turn keeps the agenda fact and a greeting keeps none.

The facts travel per turn, not in the history: `ChatRequest::contextFacts`
(a `std::vector<ContextFact>{facet, text}`, additive). `LfmAdapter::chatTurn`
and `chatPlainTurn` select the facts the turn's facets cover, drop the rest
(never blanked into empty headers), order them most-relevant-first, and compose
them into one framed tail system note beside the act — a frame that marks
the text as data written by others, never instructions
(`turn::contextBlock`, es and en, in `context-selector.cc`, its only consumer). A fact whose facet the build does not
know is dropped rather than injected. The profile and the name stay in the
static prefix and are always present; the clock note stays where it is, before
the last user message. The composed block is what `LlmChatOutcome::contextBlock`
returns, so the eval scores `missedFact`/`leakedFact` against the exact prompt
the turn sent.

Degradation: a turn that selects nothing composes no block, which is exactly a
plain chat and shares the cached prefix. The Laya facet pass (one `noul`
question per unsure facet) is U24's; nothing links it here.

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
  words. The router and every tool read the last user message through
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
- **Explicit means the user's words, not the router's confidence.** A
  router-decided call counts as explicit only when the rule parser recognizes
  a trigger or a statement, or when it is a reminder whose words name a time
  (`TemporalResolver`). "Recuérdame mañana a las nueve llamar al dentista",
  which has no "que", now stores the whole clause; before, an incomplete
  extraction kept "llamar al". A reminder that names no one is the
  speaker's: its subject is the user's own entity. So is a note asked for
  with a rule trigger ("anota que llegó el paquete", "recuerda que el wifi se
  cae cada semana") whose lexicon extraction finds no subject. Without the
  model tier in the turn such requests otherwise stored nothing. A
  statement-start match is not a timed request: the vocabulary's "la cámara"
  statement start made "muéstrame la cámara 3" a reminder, and the 3 read as a
  time. Without a trigger or a subject it stores nothing.
- **A save trigger outranks a cancellation inside it.** "don't forget that
  the dog eats at 7" contains the cancellation "forget that", and the router
  answered it with `memory.forget`. The router now tries the rule triggers
  first and treats a cancellation as a forget only when no trigger matched. "Enciende la luz de la cocina", routed by
  fastText at 0.96, stores nothing; before, an empty extraction let it
  through the rule path. The tool handlers extract with the lexicon tier
  only (`allowModel = false`). NuExtract inside a call's turn measured 23 s
  of `tool_ms` on a loaded machine. The deferred extract job still uses the
  model.
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
  the whole utterance, and a router-decided reminder whose words open with a
  command is not a timed request either.
- **Plain save requests are triggers.** "save that", "save this", "store
  that", "guarda esto", "guárdame esto", "memoriza que", "no te olvides de
  que", "quiero que guardes que", "toma nota" and their variants were not in
  the vocabulary. The router sent such a request to `memory.remember` on
  fastText's word, formation refused it as not explicit, and the model then
  answered "lo he guardado" for a fact that was never written (9 of 16 such
  probes before this change).
- **A note the fast tier could not structure is refined off the turn.** An
  explicit request whose lexicon extraction finds no complete triple is
  stored at once as the speaker's note (`predicate = nota`, the clause as
  written), so the confirmation the user hears is true and the fact survives
  a restart. `MemoryService::refineLater` then queues an extract job for that
  note (`MemoryJob::memoryId` names it, `preferIdle`), and the worker runs
  NuExtract on the utterance after the turn, when the chat engine is idle.
  When the model returns a usable triple it is stored with the clause as its
  canonical text and the note is closed; when the note was forgotten in the
  meantime the new fact is closed too, and when the model finds nothing the
  note stays. Nothing is confirmed later: the user already heard a true
  "Guardado", and a second confirmation would repeat it. NuExtract never runs
  inside a turn: in a call it measured 23 s of `tool_ms` on a loaded machine.
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

Of the eight rule notes the run left, NuExtract refined two off the turn
("el código del portón es 1234", "the gate code is 1234") and kept six as
notes. Inside a call (`argus-tool-bench --voice`, `check.tsv`): camera 56/56
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
`calendar.create_event` says "Agendado: «Reunión con Pedro», el jueves 8 a las
3 de la tarde." (and "Scheduled: «Dentist», on Thursday the 8th at 3 PM."),
`memory.remind` appends "Te llamaré el jueves 8 a las 3 de la tarde." or, when
only a reminder row was saved, "Quedó en tus recordatorios para el jueves 8 a
las 3 de la tarde." (`reminder-readback.{hxx,cc}`). The wording is
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
- The memory worker's extraction (NuExtract) waits for the chat engine to
  be idle on every job, not only on `preferIdle` ones, so NuExtract, e5 and
  the main model do not run at once on the same cores; a busy engine
  requeues the job (dropped once the worker is stopping).
- `ExtractionService::extractAsync` had no caller and is gone (#111).

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
`slot-eval-test`, `conversation-eval-test`, each mutation-checked). `fast-tier-eval` still drives the
production memory router over the judge corpus; `llm-tier-eval` drives the whole turn (decide, fill, run, speak)
with stub tools and the real model. Why the numbers are what they are, and how they were
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

## The gear swap: Laya and GLiNER behind the turn's own seams

The turn's deciders and the slot layer's text engine are seams, and both new
engines ride them; nothing in the MCP path, the tool runtime, the pendings or
the audit changes. `turn::kDeciderIds` gains a third id, `"laya"`; with
`decide.engine = "laya"` the judge arbitrates **rules + laya** and the fastText
router leaves the arbitration entirely, so it is a two-decider judge
(`llm-turn-engines-test`, "with engine laya the judge arbitrates rules and
laya, and the router never proposes"). With `decide.engine = "router"`, the
default, nothing changes. `extract.engine = "gliner"` puts
`turn::GlinerExtractor` behind `slots::TextSlots`; `"nuextract"` keeps
`turn::ModelText`. Both defaults stay on today's path until a measured
promotion; shipping defaults flip in the change that also removes the engine
they replace.

### Config keys, and their allowed values

| key | values | meaning |
|---|---|---|
| `decide.engine` | `router` (default) \| `laya` | which second decider the judge arbitrates against `rules` |
| `decide.laya.bundle_dir` | a path | the decided bundle the loader opens |
| `decide.laya.sha256` | 64 hex | the sha256 **of the bundle's own `sha256` file**, which pins every file transitively |
| `decide.laya.agent_config` | a path \| empty (default) | the checkpoint's own `rl_agent_config.json`, which supplies `head_max_len` and `temperature` for a bundle whose `decision.json` declares neither |
| `decide.laya.threads` | `0` (default) \| n | intra-op threads; `0` means `ThreadBudget::lightThreads()` |
| `decide.laya.cpu_arena` | `false` (default) \| `true` | the ORT CPU memory arena |
| `decide.laya.mem_pattern` | `false` (default) \| `true` | the ORT memory-pattern planner |
| `decide.laya.prepacking` | `false` (default) \| `true` | `session.use_prepacking` |
| `decide.laya.mmap` | `true` (default) \| `false` | hand ORT the path (mmap) instead of a read buffer |
| `extract.engine` | `nuextract` (default) \| `gliner` | which text engine the slot layer asks |
| `extract.gliner.bundle_dir` | a path | the extractor bundle |
| `extract.gliner.sha256` | 64 hex | the sha256 of that bundle's `sha256` file |
| `extract.gliner.threads` / `.cpu_arena` / `.mem_pattern` / `.prepacking` / `.mmap` | as above | the same five session options |

The template carries keys and values only; the allowed values are here, never
in `config.toml.example`.

### The bundle contract

`scripts/install-model-bundle.sh <laya|gliner> <bundle-dir>` writes the two keys
from a verified bundle, and `--rollback` restores the previous one. The bundle
is `model.onnx`, `tokenizer/`, `labels.json`, `questions.json`, `decision.json`,
`max_len`, `model-card.md`, `sha256` and `manifest.json` (the layout
`intent-training/scripts/kit_bundle.py` writes). The loader verifies the
configured pin **first** — it is the sha256 of the `sha256` file — then every
file the `sha256` file lists, and refuses an unlisted file or a missing one
(`llm-bundle-loader-test`, one refusal per case). The artefact may be INT8 or
fp32: the loader takes the file it is given and pins it, and never assumes a
quantisation.

`decision.json` carries the thresholds, and **never config**: Laya's
`act`/`ask`/`margin`/`now` (with a `calibration` block, `platt` or `isotonic`,
applied to the raw score before the judge reads it —
`turn::CalibrationModel`), and GLiNER's `pairThreshold`/`maxSpanWords` — the
pair scorer's own vocabulary, because the pilot is a `BoundaryExtractor` and
has no span-model width axis. They are
fit on the **calibration** split only, and `manifest.json` records which split
(`calibration.fitOn`). The loader surfaces that split (`BundleLoader::fitSplit`)
from the manifest, never re-derives it, and a bundle whose thresholds were fit
on dev, OOT, IND, CH or a sealed split is refused by Unit A's gate.

`labels.json` has **one shape per kind of bundle, and the loader refuses the
other for each** (`llm-bundle-loader-test`, one refusal per kind). A **decider**
bundle carries `{"labels": ["memory_save", ...]}`, the label array in the
model's own logit order, which the loader preserves. An **extractor** bundle
carries `{"model": "gliner", "logitOrder": "perType", "types": {<type>: [<field>, ...]}}`,
the type-keyed object whose every value is that type's logit order. The order
cannot be flattened into one list because the schema is an input: the caller
renders the picked type's own fields into the structure prompt, so the map is
what the decode indexes by (`BundleLoader::typeFields`, beside `labels`).

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
not offered at a
given turn is already dropped by `isOffered` in `LayaDecider::decide`, so the
label map is the whole coupling and the per-turn offer is the rest. A future
bundle that renames or adds a label is therefore a config change with no code
change — and a bad one is a visible fallback, not a misroute.

### A gate run refuses to measure a fallback

The composition degrades a bad bundle to rules + router, and that degradation
was silent to every gate: the render and corpus evals printed `pipeline:
laya+gliner` from the flag rather than from a load, and the TTFT eval drove
whatever the server happened to serve. A gate harness now asserts its engines
are up before it measures. `speech-ttft-eval.py` takes `--server-log` and
`--require-engine` (repeatable) and, once the warm-up turns that load the
engines have run and before any measured request, returns non-zero unless each
required engine's ready line (`the Laya decider is ready on`, `the GLiNER
extractor opened`) is in the log — the line is written on first use, so the
check follows the act warm-up that loads the engine.
`call-faithfulness-eval` exits with `[REFUSED]` after `setupEngines` when a run
asked for `--decide laya` or `--extract gliner` and that engine did not build. A
number taken on a fallback is therefore never compared against a previous
run's.

### Degradation is a contract

A bundle that is missing, unreadable or off its pin — and a model that opens but
cannot run — costs **one log line naming the artefact and the mismatch**, and
the turn continues on today's path: rules + router for Laya, NuExtract for
GLiNER. Nothing fails the turn. `Candidate.source` (and `Pick.source`) carry the
engine and label that actually spoke, so a fallback is visible in the trace and
in the eval instead of inferred. The pin is verified **once at load and
latched**; there is no re-hash per turn.

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

The frozen Laya fixture is `tests/fixtures/laya/parity-r1.json`, written by
`tests/eval/laya-parity.py`. Provenance: the **round-1 pilot checkpoint**
`~/.cache/argus-laya/r1/ck-pilot` (its `tokenizer/tokenizer.json` sha256
`dbba919c…`, `model.safetensors` sha256 `610fa983…`), generated from the
`intent-training` project at commit `fbaafcf8`, fixture sha256 `3417767622…`.
Re-tokenizing the fixture's texts with that checkpoint's tokenizer reproduces
its `tokens` row for row (verified 2026-10-08). The fixture carries, per case,
the text, the token ids, the per-question sequences (ids and markers), the tool
label probabilities and the `now` score — the two sides a parity harness
compares.

GLiNER's frozen fixture is `tests/fixtures/gliner/parity-pilot.json` (38 cases,
five types, the pair scorer's own decode), generated by
`intent-training/scripts/gliner_bundle.py fixture` against the fp32 pilot
bundle whose `sha256` file pinned to
`4f4808d95899e2af90456a08996a4a77c718d323731447d42d77a1ad19177799`. The shipped
GLiNER bundle is now the embedding-only INT8 artefact, its `sha256` file
pinning to
`13f547cf728765ace23d40dbde46ff00cb5fae0718e27f151822b651f9cc0b74`
(`model.onnx` 540,153,983 B), and a reference frozen from it with Python
`onnxruntime` sits beside the fixture. The pilot fixture carries,
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
question with `turn::layaBuildSequence`, and runs the graph once per question.
The pilot's `tool` question is a `choice` and `now` is a `noul`; `qtype` is
`QuestionKind`'s own value (`Choice` 0, `Score` 1, `Noul` 2), which is the
number the checkpoint trained with. The five graph inputs are `input_ids`,
`attention_mask`, `marker_pos`, `marker_mask` (the graph's only bool tensor,
which is why `turn::OnnxInput` carries a `boolean` flag) and `qtype`; the output
is `logits`, one score per option, and `marker_mask` is all true for the single
row a turn sends.

The probabilities are `softmax(logits[:k] / temperature[qtype])`, exactly
laya's own arithmetic, and the `calibration` block of `decision.json` then maps
the maximum to the confidence the judge reads (`turn::CalibrationModel`), and
`p[1]` of the `noul` row to `Candidate.now`. `temperature` is a property of the
weights — the checkpoint's `rl_agent_config.json` carries
`[3.0035, 1.0, 5.0]` for the pilot — and a bundle that does not declare it runs
at the identity default, which sharpens the softmax so hard that the
calibration saturates and every turn acts. `BundleDecode` therefore reads
`head_max_len` and `temperature` from `decision.json` when a bundle carries
them, and `decide.laya.agent_config` names the checkpoint's own
`rl_agent_config.json` when it does not; the shipped pilot bundle declares
neither and needs the config key to reproduce its calibration.

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
  the loader's defaults for both engines** (the five keys above), with
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
  pin `9e74075c…`, fp32, `model.onnx` 1230.6 MiB), single load in a fresh
  process with arena off, pattern off, prepacking off and mmap on: **session
  load delta 1330.1 MB, process warm delta 1522.2 MB**. The same run in service
  (call-faithfulness, `--decide laya`) prints `laya_rss_mb=1313.3`,
  `process_rss_mb=2435.5`. Turn the arena and the pattern **on** and the same
  reading is 1318.9 / 1513.3 MB — the two options barely move Laya, because the
  cost is the fp32 weights themselves and not the runtime's planning. The same
  measurement taken as a third load in one process reads 918.6 MB; that is
  glibc reusing what the previous session freed, not a property of the model.
  **fp32 therefore fails the 1 GB gate** and the embedding-only quantisation is
  not avoidable on this pair; see the unit's report.
- **The shipped Laya bundle is the embedding-only INT8 artefact** (`bundle-laya-pilot`,
  pin `29b1cfaed940561e19f3649151976aaacebdc2d75bbc0601aefef480f9a946f5`,
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

### The C++ tokenizer, and what parity has shown so far

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
  file; NFC is currently a no-op, which is the one documented gap.

`turn::layaBuildSequence` (`services/turn/laya-sequence.cc`) is the pure
renderer: `layaOptions` renders the criteria, the head is
`"<type> question: <instructions>"`, each option becomes `[MASK]` + its tokens
capped at 48 and then at `max(4, (head_max_len - 16) / options)` when the option
block does not fit, the head is capped at `max(8, head_max_len - option tokens)`,
and the state is appended and clamped to `max_len`. `turn::glinerDecode`
(`services/turn/gliner-decoder.cc`) is the pure decoder: sigmoid, per-query
threshold, `max_width`, the three overlap policies and the half-open
token→character mapping. **`Flat` is gliner2's `flat`, which normalises to
`disallow`: weighted interval scheduling, not greedy first-wins.** The DP is
the reference's own — exact-boundary duplicates collapsed to their highest
ranked representative, a predecessor table over the spans sorted by
`(end, start, -score)`, and the tie-break chain score, then set size, then the
best-ranked selection — and `Longest` is the reference's other policy (a span
strictly contained by another is dropped). `llm-gliner-decoder-test` holds it to
the eight frozen rows where greedy and the reference disagree
(`tests/fixtures/gliner/overlap.json`) and to 400 random interval sets
(`random-overlap.json`).

Against the frozen fixture (`parity-r1.json`, ck-pilot's tokenizer and
`max_len` 1024 / `head_max_len` 256):

- **token ids: identical to Python's, row for row (18 of 18)** — the first
  parity harness passes.
- **sequences: length and marker counts identical on all 36; the token content
  differs in the head and option positions.** The fixture's question set is
  `intent-training/scripts/laya_questions.py`'s (a criteria *list*, so options are
  the bare labels), not the checkpoint's own `questions.json`; both were checked
  against the reference, which reproduces the fixture with `laya_questions` (0
  mismatches) and not with the checkpoint's file (36). Fed that question set,
  `SpTokenizer`/`layaBuildSequence` reproduce every length and every marker, so
  what is left is a token-content divergence in the BPE of the head/option
  strings, not a structural one.

### Two corrections of record (2026-10-08)

**The parity statement, said exactly.** Two different artefacts were being
described in one breath. The **tokenizer's ids** are `encode_text(text)` over the
state text, and against the frozen fixture they are **identical to Python's, 18
rows of 18**. The **sequences** are `build_sequence`'s output, and those are
**failing**: every length and every marker count matches Python's, but the token
content of the assembled head and option segments does not. The reference was
checked directly — `laya.common.build_sequence` with the same question set
reproduces the fixture exactly (0 of 36 mismatches), so the reference is settled
and the divergence is in `turn::layaBuildSequence`/`turn::SpTokenizer` on the
head and option strings, not in the reference. The status is **sequence parity
failing**, and the fix is in the renderer's segment assembly.

**NFC is not a gap in `SpTokenizer`; it is missing from the model itself.** The
checkpoint's `tokenizer.json` normalises nothing but the marker
(`Replace` " " → "▁"), so the frozen reference gives **different ids for NFD and
NFC spellings of the same word** — `café`, `mañana`, `cigüeña`, `PINGÜINO`, `el
año pasado` and `¿qué tengo en la agenda?` all differ between the two spellings
under Python. `SpTokenizer` mirrors the file, so it is parity-faithful; putting
NFC inside it would make it *disagree* with the reference. The normalisation
therefore belongs to the serving pipeline **before** tokenisation, and the
parity target is `C++ NFC(nfd) == Python(nfc)`. `tests/fixtures/laya/nfc-pairs.json`
records both spellings and Python's ids for each, and
`llm-sp-tokenizer-test` compares the C++ against both.

**Nothing in the tree provides NFC.** `lib/text` has no normaliser, no Conan
package carries one, and what `argus-llm` already links is protobuf's and
abseil's UTF-8 *validators* (`libutf8_validity`, `libabsl_utf8_for_code_point`) —
validation, not normalisation. The system does have `libicuuc.so.78` and
`utf8proc`, but neither is the tree's to link. **Proposal, not a change:**
`utf8proc` (MIT, ~100 KB, one call — `utf8proc_NFC`) as a tier-1 Conan package in
`packages/lib/text`, with ICU (`icu/*`, ~30 MB, the full normaliser) as the
alternative if a Conan recipe for `utf8proc` does not exist. Awaiting the word
before anything is added.

**The question set is a defect to report, not a choice.** The frozen fixture is
reproduced by `intent-training/scripts/laya_questions.py`'s `QUESTIONS` (criteria
as a list) and **not** by the pilot checkpoint's own `questions.json` (36
mismatches against the same reference). Two files that should be one disagree.
Unit A's export must write the training run's own question file, record its
hash in `manifest.json`, and have the training code read that same file, so they
cannot diverge; this one is flagged rather than picked.

## The fresh-tree Vulkan shader cost (2026-10-09)

The vendored `third_party/llama.cpp` turns `GGML_VULKAN` on when Vulkan, glslc and
SPIRV-Headers are present, and its `vulkan-shaders-gen` ExternalProject then pays a
fixed cost in every fresh build tree: a wave of 137 shader-generation edges and
1922 glslc runs, **about 425 s of edge time** (425.9 s cold; 429.5 s, 424.2 s and
427.3 s in the later samples), beside the ~200 s the rest of a ccache-warm
`./scripts/build-all.sh dev --only llm` takes. This is **a known cost of a fresh
tree, not a defect.** It is paid in every fresh tree, patched or not; it does not
recur on an incremental or no-op build; and `services/vlm` pays its own copy,
because the two projects do not share a build tree. The shader block itself is
recorded in `docs/operations/build-and-test.md`.

**`BUILD_ALWAYS TRUE` is not the cause of it, and cannot be.** At
`third_party/llama.cpp/ggml/src/ggml-vulkan/CMakeLists.txt:177-195` the generator
is declared `ExternalProject_Add(… BUILD_ALWAYS TRUE)`, which re-runs only the
external project's own build, install, done and complete steps — five `.ninja_log`
entries, 47 ms on a second build. The 137 generation edges consume the generator
binary, and CMake emits a dependency on a custom *target* as an **order-only**
dependency: `|| … llama-build/ggml/src/ggml-vulkan/vulkan-shaders-gen` at
`services/llm/build/dev/build.ninja:11492` and `:11501`. Re-dirtying the external
project's stamps therefore cannot reach the generation edges, which is why
`BUILD_ALWAYS` is not — and cannot be — what regenerates a shader.

A patch that sets `BUILD_ALWAYS FALSE` and adds
`ExternalProject_Add_StepDependencies` is proven, and saves exactly the four
external-project edges (5 entries becomes 1, 35-100 ms on a second build) without
removing any of the 425 s. It lives outside the repository, at
`~/.cache/argus-scratch-backup/backend13/shaders/candidate-patch.patch`, because
the tree has no durable mechanism for a submodule patch; it is recorded here so
the cost is known rather than fought.
