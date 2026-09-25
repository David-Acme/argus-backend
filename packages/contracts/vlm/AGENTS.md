# argus_contracts_vlm

The vision boundary: the eleven refusals it answers with and the schema both
sides of the wire are generated from.

## What this is

A CONTRACT, and the fourth of the five here that is not header-only: alongside
the header-only `argus::contracts::vlm` vocabulary target it owns
`argus_vlm_rpc_contract()`, the CMake **function** that declares
`argus::contracts::vlm-wire` from the `vlm.proto` beside it. A consumer does
not link the wire target blind — it includes this package and calls the
function, which first calls `argus_response_rpc_contract()` (the refusal
envelope every gRPC answer here carries), then `lib/grpc`, then the proto. The
include root is `src/`, so a consumer writes `<vlm/vlm-errors.hxx>`.

`packages/clients/vlm` is where the module is established, for the gRPC client
and the HTTP face the guard's assessment still runs on; `services/vlm`'s
`argus::vlm-rpc` module names `argus::contracts::vlm-wire` itself for the server
side — the client links the wire PRIVATE, so the wire reaches a server by its
own name — and the vocabulary for the engine and the controller.

## Layout

- `src/vlm/vlm-errors.hxx` — the eleven refusals: `VisionEngineNotLoaded` (its
  own code, 503, the engine not loaded), `BodyNotJsonObject` 400,
  `InvalidRequest` 400, `ImageNotDecodable` 400, `Unauthorized` 401,
  `Cancelled` 499, `DeadlineExceeded` 504, `Busy` 429, `InternalError` 500,
  `InvalidResponse` 502 and `Unavailable` 503. 7 files include it — the client,
  the gRPC server, the controller and the engine, plus the three suites.
- `vlm.proto` — `package argus.vlm.v1`: one `Vision` service with two **unary**
  RPCs, `Capabilities` (loaded, the input-pixel ceiling and the engine's default
  token cap) and `Describe` (the JPEG bytes, an arbitrary prompt, the camera id
  and a token cap in, the caption out). No streaming RPC: a description is one
  image, not a tile stream. It sits at the package root, not under the group's
  shared `proto/` root, so the function passes this directory as the single
  `PROTO_ROOT`.
- `CMakeLists.txt` — the vocabulary declaration and `argus_vlm_rpc_contract()`,
  defined here because the wire belongs to the package that owns the schema.

## Rules

- A refusal reaches the caller as a `grpc::Status` with the `ErrorResponse` in
  its details: the server hands `toRpcStatus(...)` to the call rather than
  building a status of its own, so these eleven definitions are also the
  statuses the client reads back.
- `VisionEngineNotLoaded` is the catalog's one domain-specific code — the other
  ten are shared `ErrorCode` spellings. `BodyNotJsonObject` belongs to the HTTP
  leg alone (the gRPC leg carries `bytes image_jpeg`, so there is no JSON body
  to refuse), while `ImageNotDecodable` is the gRPC leg's spelling of the bytes
  a caller sends that are not an image: the HTTP controller answers that case
  with a 422 field envelope instead, because its surface carries field
  granularity and a status face does not. `Unavailable` is exported with no
  in-tree reader exactly as the `stt` and `tts` catalogs' is.
- The proto is the contract for the image as well: `image_jpeg` is a complete
  JPEG, never a base64 string — the HTTP leg's `image_b64` is that surface's
  own encoding and does not cross this wire, so a change to either field is a
  wire break for argus-vlm and its callers at once. `max_tokens` is the same
  steering the engine seam takes, and `0` means the engine's own default.
- Rule 25: the folder IS the module. An explicit source list, never
  `file(GLOB)`.

## Tests

- `tests/unit/vlm-contract-catalog-test.cc` — the eleven refusals as a pinned
  table, each entry's wire legality, and that no two say the same thing.
