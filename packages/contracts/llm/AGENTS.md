# argus_contracts_llm

The chat boundary: the refusals it answers with and the schema both sides of
the wire are generated from.

## What this is

A CONTRACT, and the fifth of the five here that is not header-only: alongside
the header-only `argus::contracts::llm` vocabulary target it owns
`argus_llm_rpc_contract()`, the CMake **function** that declares
`argus::contracts::llm-wire` from the `llm.proto` beside it. A consumer does
not link the wire target blind — it includes this package and calls the
function, which first calls `argus_response_rpc_contract()` (the refusal
envelope every gRPC answer here carries), then `lib/grpc`, then the proto. The
include root is `src/`, so a consumer writes `<llm/llm-errors.hxx>`.

`packages/clients/llm` is where the module is established, for the gRPC client
and the HTTP face argus-guard, argus-voice and `services/llm`'s memory feature
still run on;
`services/llm`'s `argus::llm-rpc` module names `argus::contracts::llm-wire`
itself for the server side — the client links the wire PRIVATE, so the wire
reaches a server by its own name — and the vocabulary for the engine and the
controller.

## Layout

- `src/llm/llm-errors.hxx` — the ten refusals: `LlmEngineNotLoaded` (its own
  code, 503, the engine not loaded), `BodyNotJsonObject` 400,
  `InvalidRequest` 400, `Unauthorized` 401, `Cancelled` 499,
  `DeadlineExceeded` 504, `Busy` 429, `InternalError` 500,
  `InvalidResponse` 502 and `Unavailable` 503.
- `llm.proto` — `package argus.llm.v1`: one `Chat` service with two **unary**
  RPCs and one **server-streaming** RPC — `Capabilities` (loaded, the engine's
  default token cap and temperature, the context size and the three last-prefill
  counters), `Chat` (the messages, the generation steering and the caller's
  user id, the caller's `CallerRole` and `lang`, the call's `session_id`
  and `prefill_only` in, the completion out; `CALLER_ROLE_UNSPECIFIED` is
  read as a guest, and `prefill_only` asks the engine to load the prompt into
  its cache and answer an empty completion) and `ChatStream` (the same request in, a
  `ChatToken` per token out, terminated by a token whose `done` is set and
  which carries the prefill stats the HTTP leg's sentinel line carries). It is
  the second streaming RPC among the five boundary contracts (`response`,
  `stt`, `tts`, `vlm`, `llm`) after `argus.tts.v1.Synthesize` — the voice
  session's bidirectional `Connect` and the camera's `Subscribe` also stream —
  and it
  sits at the package root, not under the group's shared `proto/` root, so the
  function passes this directory as the single `PROTO_ROOT`. `ChatToken.speech`
  (field 8) is additive and backend-internal: it carries the same
  `speech_unavailable` marker the HTTP sentinel's `speech` key carries, and
  `services/voice` reads it there instead of inferring the marker from an empty
  reply. No app-facing client reads it — the app never speaks this wire — and it
  is dormant while no in-tree config sets the gRPC pair (`rpc.address` and
  `[rpc.callers]` are empty everywhere).
- `CMakeLists.txt` — the vocabulary declaration and `argus_llm_rpc_contract()`,
  defined here because the wire belongs to the package that owns the schema.

## Rules

- A refusal reaches the caller as a `grpc::Status` with the `ErrorResponse` in
  its details: the server hands `toRpcStatus(...)` to the call rather than
  building a status of its own, so these ten definitions are also the statuses
  the client reads back.
- `LlmEngineNotLoaded` is the catalog's one domain-specific code — the other
  nine are shared `ErrorCode` spellings. `BodyNotJsonObject` belongs to the HTTP
  leg alone (the gRPC leg carries typed fields, so there is no JSON body to
  refuse), and the streaming leg has no "no tokens" refusal: an empty
  generation is a legitimate answer on both legs, so a stream that produced no
  text still ends with its `done` token. `Unavailable` is exported with no
  in-tree reader exactly as the `stt`, `tts` and `vlm` catalogs' is.
- The proto is the contract for the assistant turn as well: `tools` false keeps a
  request on the direct engine path and `user_id` scopes tool execution, so a
  gRPC caller that omits `user_id` runs tools with `0`, the same scope the HTTP
  leg gives a body that omits `user_id`. `tools` and `temperature` are declared
  proto3 `optional`, so a caller that declares neither takes the engine's
  defaults — the tools on, the engine's own temperature — exactly as an
  HTTP body that omits both keys does; an explicit `temperature` of `-1` is the
  engine-default sentinel the client sends by default, while the HTTP DTO
  refuses that value and expects the key to be absent instead. `grammar` is the
  same GBNF source the
  HTTP DTO carries, and `grammar_required` the same abort-on-uncompilable
  switch.
- Rule 25: the folder IS the module. An explicit source list, never
  `file(GLOB)`.

## Tests

- `tests/unit/llm-contract-catalog-test.cc` — the ten refusals as a pinned
  table, each entry's wire legality, and that no two say the same thing.
