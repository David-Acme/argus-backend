# argus_contracts_stt

The transcription boundary: the eleven refusals it answers with and the schema
both sides of the wire are generated from.

## What this is

A CONTRACT, and the third of the four here that is not header-only: alongside
the header-only `argus::contracts::stt` vocabulary target it owns
`argus_stt_rpc_contract()`, the CMake **function** that declares
`argus::contracts::stt-wire` from the `stt.proto` beside it. A consumer does
not link the wire target blind — it includes this package and calls the
function, which first calls `argus_response_rpc_contract()` (the refusal
envelope every gRPC answer here carries), then `lib/grpc`, then the proto. The
include root is `src/`, so a consumer writes `<stt/stt-errors.hxx>`.

`packages/clients/stt` is where the module is established, for the gRPC client
and the transitional HTTP fallback both; `services/stt`'s `argus::stt-rpc`
module names `argus::contracts::stt-wire` itself for the server side — the
client links the wire PRIVATE, so the wire reaches a server by its own name —
and the vocabulary for the engine and the controller.

## Layout

- `src/stt/stt-errors.hxx` — the eleven refusals: `SpeechEngineNotLoaded` (its
  own code, 503, the engine not loaded), `InvalidRequest` 400, `BodyNotPcmS16`
  400, `PcmBodyMisaligned` 400, `Unauthorized` 401, `Cancelled` 499,
  `DeadlineExceeded` 504, `Busy` 429, `InternalError` 500, `InvalidResponse`
  502 and `Unavailable` 503. 7 files include it.
- `stt.proto` — `package argus.stt.v1`: one `Transcription` service with two
  **unary** RPCs, `Capabilities` (rate, loaded, current and default language and
  the languages the engine accepts) and `Transcribe` (`repeated float` samples,
  a rate and an optional language in, the transcript out). No streaming RPC: a
  turn is one buffer, not a chunk stream. It sits at the package root, not
  under the group's shared `proto/` root, so the function passes this directory
  as the single `PROTO_ROOT`.
- `CMakeLists.txt` — the vocabulary declaration and `argus_stt_rpc_contract()`,
  defined here because the wire belongs to the package that owns the schema.

## Rules

- A refusal reaches the caller as a `grpc::Status` with the `ErrorResponse` in
  its details: the server hands `toRpcStatus(...)` to the call rather than
  building a status of its own, so these eleven definitions are also the
  statuses the client reads back.
- `SpeechEngineNotLoaded` is the catalog's one domain-specific code — the other
  ten are shared `ErrorCode` spellings. The pairing with 503 is the convention
  a reviewer checks first, and `ServiceUnavailable` in every other catalog in
  the tree answers 503 too. `BodyNotPcmS16` and `PcmBodyMisaligned` belong to
  the HTTP leg alone (the gRPC leg carries `repeated float`, so there is no
  content type and no alignment to refuse), and `Unavailable` is exported with
  no in-tree reader exactly as the `tts` catalog's is.
- The proto is the contract for the samples as well: `samples` is float in
  [-1, 1] at `sample_rate` — the voice session's own float form, not the s16
  the HTTP leg posts — so a change to either field is a wire break for argus-stt
  and its callers at once.
- Rule 25: the folder IS the module. An explicit source list, never
  `file(GLOB)`.

## Tests

- `tests/unit/stt-contract-catalog-test.cc` — the eleven refusals as a pinned
  table, each entry's wire legality, and that no two say the same thing.
