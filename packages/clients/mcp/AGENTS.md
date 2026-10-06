# argus_clients_mcp

The caller's side of `argus.mcp.v1`: a `Transport` for `argus::mcp::McpClient`
that exchanges one frame with a tool owner over gRPC.

## What this is

A CLIENT module: one `argus_clients(NAME mcp ...)`, a STATIC library whose
include root is `src/`, so a consumer writes `<mcp/grpc-tool-transport.hxx>`
and links `argus::clients::mcp`. It calls `argus_mcp_rpc_contract()` (owned by
`packages/contracts/mcp`) and links `argus::contracts::mcp-wire` PRIVATE, so
no protobuf header reaches a consumer through it. Its one consumer is
`services/llm`, the assistant that aggregates every owner's tools.

## Layout

- `src/mcp/grpc-tool-transport.{hxx,cc}` — `ToolEndpoint` (`target`,
  `credential`, `timeout`, 15 s by default) and `GrpcToolTransport`. The
  credential travels as `x-argus-credential` through `addCallerCredential`,
  the deadline through `setDeadline`.

## Rules

- The constructor is the gate: an empty target, an empty credential, a
  non-positive timeout or one over two minutes throws `std::invalid_argument`
  before dialling.
- A failed call answers `std::nullopt` and logs the status once; the
  `McpClient` above reports it as a `Transport` failure, so a wrong credential
  and an unreachable owner look the same to the assistant (it answers that the
  tool is not available) and different in the log.
- One channel per transport; the caller owns how many it keeps per owner.

## Tests

`grpc-tool-transport-test` — a live listener on an ephemeral loopback port:
discover, list and call end to end; a wrong credential and a listener that is
not there as transport failures within the deadline; and the four refusals at
construction.
