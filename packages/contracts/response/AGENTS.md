# argus_contracts_response

The refusal envelope on the gRPC wire: the protobuf message a service answers
with and the two functions that turn it into a `grpc::Status` and back.

## What this is

A CONTRACT and the one contract here that is not header-only: it owns both the
schema (`response.proto`, at the package root rather than under the group's
shared `proto/` root) and the translation unit that fills it
(`src/response/response-rpc.cc`). `argus_response_rpc_contract()` builds them
into `argus::contracts::response-wire`, and this package now calls it itself
rather than leaving the first consumer to do it. It declares no
`argus_contracts` vocabulary target: the two functions and the message are the
whole surface.

The consumers are the gRPC boundaries: `contracts/tts` declares `tts-wire`
with `DEPENDS argus::contracts::response-wire`, `packages/clients/tts` and
`services/tts` link `tts-wire`, and every refusal that crosses a gRPC boundary
in the tree goes through these two functions.

## Layout

- `response.proto` — `argus.response.v1.ErrorResponse`, one `single` record or
  a `list` of up to sixteen, each a `{code, message}` pair.
- `src/response/response-rpc.hxx` — `toRpcStatus(const ResponseException&)` and
  `fromRpcStatus(const grpc::Status&)`, in `argus::response`; 4 files include
  it.
- `src/response/response-rpc.cc` — the mapping in both directions and the
  limits it enforces: a status outside 400–599, an empty or over-long record,
  more than sixteen errors, more than 4096 bytes of details.

## Rules

- The HTTP status and the gRPC code are two spellings of one refusal, and the
  mapping is fixed: 400 and 422 answer `INVALID_ARGUMENT`, 401
  `UNAUTHENTICATED`, 403 `PERMISSION_DENIED`, 404 `NOT_FOUND`, 409
  `ALREADY_EXISTS`, 429 `RESOURCE_EXHAUSTED`, 499 `CANCELLED`, 502 and 503
  `UNAVAILABLE`, 504 `DEADLINE_EXCEEDED`, anything else `INTERNAL`.
- A refusal that cannot be carried is not sent as itself: an over-long
  message, a status outside the range or a list over the limit becomes 500
  `INTERNAL_ERROR` "Internal service error", and a payload whose status
  disagrees with the transport code, or whose bytes are not an
  `ErrorResponse`, becomes 502 `BAD_GATEWAY` "Invalid service response".
- The transport's own failures map back the same way when the status carries
  no details, and a status that says OK with nothing in it is treated as a
  caller's mistake, answered 500 rather than passed through.
- Rule 25: the folder IS the module, and the wire module is its declaration —
  an explicit source list, never `file(GLOB)`.

## Tests

- `tests/unit/response-contract-rpc-test.cc` — the round-trip for a single
  refusal and for a list, the status-to-code mapping outbound, the transport
  codes inbound, the three refusals the wire cannot carry, and the two ways a
  payload is refused rather than trusted.
