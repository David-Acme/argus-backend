# argus_contracts_mcp

The wire a tool owner serves its MCP server on, and the one adapter that turns
an `argus::mcp::McpServer` into a gRPC service.

## What this is

A CONTRACT with a wire half, like `settings`: `argus_mcp_rpc_contract()` is
the CMake function that declares `argus::contracts::mcp-wire` from `mcp.proto`
together with `McpRpcService`. A service that owns tools includes this
package, calls the function, links the wire target and registers one
`McpRpcService` on the gRPC server it already runs. The include root is
`src/`: `<mcp/mcp-rpc.hxx>`.

## Layout

- `mcp.proto` — `package argus.mcp.v1`: one `Mcp` service with one unary
  `Rpc(Frame) returns (Frame)`, a `Frame` being one JSON-RPC message as text.
  The protocol is MCP's, not this file's: a stateless request and result need
  no streaming, no session and no schema of their own.
- `src/mcp/mcp-rpc.{hxx,cc}` — `McpRpcService`, a callback service over a
  `McpServer` and a `FleetCallerGate`. The gate admits only the callers it is
  given (`llm` by default); a paired fleet caller that is not one of them is
  `PERMISSION_DENIED`, an unknown or absent credential `UNAUTHENTICATED`. A
  frame over 256 KiB is `INVALID_ARGUMENT` before it is parsed. A tool that
  answers from another thread finishes the call from there; the reply is
  guaranteed once by the server.

## Rules

- A surface is never built open. The constructor throws `std::invalid_argument`
  for a missing server, a gate with no paired caller (`open()` or
  `pairedCount() == 0`, which is every `CHANGE_ME` placeholder) or an empty
  caller list, so a service whose `llm` credential is not paired serves no
  tools at all rather than tools to anyone.
- The caller context inside a frame (`argus/context`) is trusted because the
  channel it arrived on is admitted: the credential decides who may declare an
  identity, never the frame.
- Everything inside the frame, including JSON-RPC errors and tool errors, is
  an `OK` RPC; gRPC statuses speak only of admission and size.

## Tests

`mcp-rpc-test` — a live in-process server: the paired `llm` credential lists
and calls, a handler that answers from another thread, no credential or a
wrong one, a fleet caller that is not `llm`, an oversized frame, garbage
inside an admitted frame, a notification, and every way a surface refuses to
be built.
