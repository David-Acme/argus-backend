# argus_contracts_tts

The text-to-speech boundary: the nine refusals it answers with and the schema
both sides of the wire are generated from.

## What this is

A CONTRACT, and the second of the four here that is not header-only: alongside
the header-only `argus::contracts::tts` vocabulary target it owns
`argus_tts_rpc_contract()`, the CMake **function** that declares
`argus::contracts::tts-wire` from the `tts.proto` beside it. A consumer does
not link the wire target blind — it includes this package and calls the
function, which first calls `argus_response_rpc_contract()` (the refusal
envelope every gRPC answer here carries), then `lib/grpc`, then the proto. The
include root is `src/`, so a consumer writes `<tts/tts-errors.hxx>`.

`packages/clients/tts` is where the module is established, for the modern
client and the transitional HTTP fallback both; `services/tts`'s
`argus::tts-rpc` module names `argus::contracts::tts-wire` itself for the server
side — the client links the wire PRIVATE, so the wire reaches a server by its
own name — and the vocabulary for the synthesis feature. `services/camera` and
`services/voice` synthesize through the client rather than the wire.

## Layout

- `src/tts/tts-errors.hxx` — the nine refusals: `TtsNotLoaded` (its own code,
  503, the engine not loaded), `InvalidRequest` 400, `Unauthorized` 401,
  `Cancelled` 499, `DeadlineExceeded` 504, `Busy` 429, `InternalError` 500,
  `InvalidResponse` 502 and `Unavailable` 503. 7 files include it.
- `tts.proto` — `package argus.tts.v1`: one `Synthesis` service with
  `Capabilities` and a **server-streaming** `Synthesize` that answers a stream
  of `AudioChunk`, plus the `SampleFormat` and `Quality` enums. It sits at the
  package root, not under the group's shared `proto/` root, so the function
  passes this directory as the single `PROTO_ROOT`.
- `CMakeLists.txt` — the vocabulary declaration and `argus_tts_rpc_contract()`,
  defined here because the wire belongs to the package that owns the schema.

## Rules

- A refusal reaches the caller as a `grpc::Status` with the `ErrorResponse` in
  its details, on the streaming call as much as on the unary one: the server
  fills `queue.status = toRpcStatus(...)` rather than sending an error message
  down the stream. So these nine definitions are also the nine statuses the
  client has to read back.
- `TtsNotLoaded` is the catalog's one domain-specific code — the other eight
  are shared `ErrorCode` spellings. The pairing with 503 is the convention a
  reviewer checks first, and `ServiceUnavailable` in every other catalog in the
  tree answers 503 too.
- The proto is the contract for the samples as well: a change to
  `SampleFormat`, `Quality` or the `AudioChunk` fields is a wire break for the
  camera talk path and the voice service at once.
- Rule 25: the folder IS the module. An explicit source list, never
  `file(GLOB)`.

## Tests

- `tests/unit/tts-contract-catalog-test.cc` — the nine refusals as a pinned
  table, each entry's wire legality, and that no two say the same thing.
