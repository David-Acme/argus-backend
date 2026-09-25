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
- **The KV-prefix cache (`cachedTokens_`) stays IN the service** —
  per-process warm state. One llama context, one cache slot: concurrent
  sessions with different prompts THRASH the cache exactly as two voice
  sessions did in-process (the legacy semantics are unchanged, not fixed);
  alternating different prompts reuse 0 tokens each time. The
  `GET /llm/v1/config` leg exposes the last prefill stats so the thrash is
  observable.
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
    pattern), the engine mutex serializes concurrent generations.
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
  `grammar_required`), with `temperature` and `tools` declared proto3
  `optional` so an undeclared one takes the engine's default exactly as an
  omitted HTTP key does; the server re-checks every bound the client checks
  (the wire is a boundary, not everyone is the client) and refuses a
  caller-declared deadline more than a second past
  `argus::llm::kMaxTimeout` — gRPC rounds the relative `grpc-timeout` header,
  so the flat two-minute ceiling the tts and stt servers carry would refuse
  the client's own maximum. A caller arriving while every
  `ThreadBudget::inferenceSlots()` slot is held gets 429 `Busy`; the engine
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
- **Config**: `[llm]` (engine knobs, mirroring the legacy block) +
  `[server]` (loopback listener, default 7032) + `[rpc]`/`[rpc.callers]`
  (the dormant gRPC face). The `[server]`
  listener is internal-network only: the wire is never announced or
  published; so is the gRPC one, which is plaintext and gated by the caller
  credential alone. No database, no NATS,
  no JWT/device keys — nothing here persists anything.

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
captured exactly once through the injected `capture`, which calls
`observeSystemEvent` with the owner's scope —
never rule-parsed, never a fact. The owner resolves through
`IdentityClient::listNotifiableUsers` (first notifiable user, their language
for the summary line). The consumer stops before `memory.shutdown()` in the
teardown order, waiting in-flight handlers out. This is the only camera feed
long-term memory reads; the raw `object_detected` subscription stays an
episodic throttle beside it.
