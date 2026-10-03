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
  exact prompt the next turn starts from: persona, policy, declarations and
  history, through the same hop path. It prefills that prompt and answers an
  empty completion, without routing and without running a tool. The voice
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
    terminated by a final JSON sentinel line `\n{done:true,
    prompt_tokens, reused_tokens, decoded_tokens}\n`. The sentinel is the
    client's exact end-of-stream marker; token framing is "chunks until the
    sentinel line". Generation runs on a producer thread (the TTS stream
    pattern), the engine mutex serializes concurrent generations. A client
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
  unknown wire role is `UserRole::Guest`, the least privileged role, never
  Resident, and an absent `lang` keeps the tool runtime's Spanish default; a
  `lang` outside `es`/`en` is refused. The tool loop used to run every turn
  as a Resident in Spanish. Now the controller offers the model only the
  tools `role_access::hasAccess` grants that role for each descriptor's
  `accessTable`/`accessPermission` (`ToolExecutor::permittedTools`), and the
  executor still checks every call, the routed ones included. A role with no
  permitted tool (Guard and Guest have no `memory` row) gets the direct
  engine path.
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
  `validateArguments`) and the controller drives
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

The sampling keys say "restart" for now. `resolveSampling()`
(`feature/llm/services/sampling-config.cc`) is the one reader of them and
`LlmService::init()` copies its result into the engine's members, but those
members and the inline `defaultTemperature()`/`defaultMaxTokens()` accessors
are declared in `packages/clients/llm/src/llm/llm-service.hxx`. Making them
live needs that header to hold the set as one guarded value with a
`refreshSampling()` that `main.cc` calls from the registry's `onChange`; once
it does, the nine sampling specs flip to `SettingApply::Live`.

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
- `user_id` (D4) scopes tool execution to the authenticated caller.

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
from outside — the memory stack's only caller is the tool loop beside it and
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
  Every apply runs marshalled onto IOLoop 0 so the arrival order is the stream
  order, and the host builds the replica whenever `nats.url` is set (not only
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
procedures) and, since f8-b3, the LLM's own tool loop.

### f8-b3: the service becomes a package (2026-09-08)

The user's ruling: the LLM's memory should not be a wire hop away — the
tool-calling loop needs the memory tools in process, and the memory worker
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
of the router `argus::llm`'s gate drives: rules (`argus::lib::phrase`) decide
explicit triggers, fastText classifies the rest into six classes
(`memory_save`, `memory_recall`, `reminder_set`, `memory_forget`, `camera`,
`none`), and the LLM's own tool calling keeps every turn the router is not
confident about. The classifier picks the tool; the model only writes its
arguments and the prose. Operating point 0.90 / 0.10, precision-gated — a
gated false `none` costs one LLM round trip, a false tool call writes a fact
nobody stated. **Degradation is a contract**: no model on disk, or a
sub-threshold score, and the router abstains so tool calling runs byte for
byte as it did before. The model is a published artifact carried in-repo
(`models/intent/intent.bin`, 12.6 MB) with a configure-time SHA256 pin in
`src/feature/intent/models/`; training lives OUTSIDE this repo, in the sibling
`intent-training/` project, and only the artifact, its card and the frozen
eval fixtures cross over. `fasttext` is built from the `third_party/fastText`
submodule (inference only, static lib) by this service's project file, the way
it bootstraps llama.cpp and sqlite-vec.


## Phase 4 step 9: config resolution into `src/config/` (D20)

The five things `main.cc` resolved inline are `src/config/llm-config.{hxx,cc}`
(`argus::llm-config`): `resolveListener()` (`ListenerConfig::resolve(7032)`),
`resolveRpc()` (`rpc.address` plus the `rpc.callers` pairs),
`resolveIdentity()` (`identity.target`/`identity.rpc_secret`, read by the
catalog-snapshot fill and by the encounter consumer),
`resolveCameraTarget()` (`camera.grpc_target`) and `resolveMemory()`
(`memory.observe_camera_events`). `main.cc` keeps `config.toml` loading,
`drogonConfig` and the `nats.url` gate on the optional bus.

## App tools (conversation mode)

`feature/llm/services/tools/app-tool-descriptors.cc` registers
`app.show_camera` (camera read), `app.open` (any role) and
`app.set_guard_mode` (camera update, i.e. owner and resident, matching the
guard surface in `role-access.hxx`). They are offered only when the
request sets `clientActions`, which only voice calls do; their handler
hands the validated call to `ToolContext::emitAction`, which the
controller turns into a `ClientAction` on the stream (`ChatToken.action`,
ordered with the text tokens). Without an emitter the tool refuses, so a
non-call caller never hears that something happened when nothing did.

## The tool loop in a call (2026-10-03)

The voice session and argus-llm agreed this contract with the voice agent:

- **Messages.** History[0] is the system persona with the stable call facts.
  User messages carry only what STT heard. Every later `system` message is a
  note: the tone note right after its user message, context notes appended
  where they arrive. Notes are rendered in place and never read as the user's
  words. The router and every tool read the last user message through
  `LfmAdapter::spokenText`, which also drops a trailing parenthesized line,
  the shape the tone note had when it was appended to the user's content.
- **Same system prompt on every hop.** The routed prose answer, the prose
  answer after an exhausted or repeated loop, and every tool hop carry the
  same persona, policy and declarations, so their prompts share the cached
  prefix. Prose answers are generated with `toolCallsAllowed = false`: the
  sampler gives `<|tool_call_start|>` a −∞ logit bias, so the declarations
  being present cannot make a prose answer open a call that would be spoken.
- **A routed call is shown as a call.** The fast tier's call is rendered into
  the history as the model would have written it
  (`<|tool_call_start|>[memory.remember(text='…')]<|tool_call_end|>`, the
  template's own pythonic form) before the tool's result, so the model
  confirms a call it can see. A routed call the tool refuses still falls
  back to the tool loop with every tool offered. The eval showed the router's
  false positives ("enciende la luz de la cocina", "cuándo viene mi
  hermana") are exactly the calls memory formation refuses, and a reply
  built from "No pude guardar eso" answered them worse than the model did.
  The baseline's other failure, routed saves that formation dropped while
  the model claimed "lo he guardado", is fixed at its source: explicit
  requests now store.
- **Each tool runs at most once per turn.** A tool that already succeeded in
  this turn is not run again in a later hop: the model gets the earlier
  result. A hop made only of repeats ends the loop with a prose answer.
  Calls to a tool the turn did not offer are dropped. A reply that tried to
  call but held nothing runnable is answered in prose instead of being spoken
  with its markup.
- **The parser reads the template's form exactly.** `PythonicScanner` is
  quote-aware: brackets, parentheses and commas inside a quoted argument do
  not end the call, and escapes decode. It accepts JSON arguments, `True`,
  `False` and `None`, and a bare call without the list brackets.
- **Memory writes keep to the user's words.** `memory.remember` and
  `memory.remind` check the model-written `text` and `value` against the
  user's utterance. When fewer than 60 % of the argument's words were said,
  the utterance replaces the text and the triple is dropped. A fact copied
  from a note or a camera offer, or the model's paraphrase, is never stored as
  the user's fact, and "recuérdalo" alone stores nothing. An explicit request
  ("recuerda que …") whose extraction comes back incomplete is stored from its
  rule clause instead of being dropped.
- **`memory.forget` takes a `query`**, the fact in the user's words, instead
  of a `fact_id` the model could only invent. It closes the user's open fact
  that shares the most words with the query. The close is scoped to the
  caller (`scope = 'user' AND ref_id = ?`) and succeeds only when a row
  changed. Before, any id of any user closed, and "olvidado" was answered
  even when nothing matched.
- **Tool outputs are spoken material**: localized to the call's language,
  with no ids ("Guardado: mi hermana viene los domingos.", "Saved: …"). The
  app tools answer in the call's language too. `memory.recall` honours the
  owner's `memory.recall_top_k`; the tool path read a fixed 8.
- **Permission before schema.** The executor refuses a role before it
  validates arguments, so an unpermitted caller learns nothing about a tool's
  shape. `ToolRegistry::names()` is sorted, so the declarations, and with them
  the cached prefix, are the same in every process.
