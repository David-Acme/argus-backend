# argus-mcp

The Model Context Protocol, small and first-party: JSON-RPC 2.0 framing, the
`server/discover`, `tools/list` and `tools/call` methods of the 2026-07-28
revision, a JSON Schema subset to validate tool arguments, a server a service
registers its tools on and a client the assistant aggregates them with. Tier 1
(`argus_lib_mcp`, `argus::lib::mcp`): it links Drogon (jsoncpp and the logger)
and nothing else, no gRPC, no contract, no client and no service. The wire
that carries a frame between two processes is `contracts/mcp` (the server
side) and `clients/mcp` (the caller's side); this package only knows a frame
as a string and a `Transport` as the thing that exchanges one.

## Why the 2026-07-28 revision

The current specification is stateless: no `initialize` handshake, no session.
Every request carries `_meta` with `io.modelcontextprotocol/protocolVersion`
and `io.modelcontextprotocol/clientCapabilities`, a server answers
`server/discover`, results carry `resultType: "complete"` and the cacheable
ones (`server/discover`, `tools/list`) carry `ttlMs` and `cacheScope`. A
stateless protocol is what a unary RPC wants, and it is what each of Argus'
tool owners is: a request in, a result out, nothing remembered between two
calls. `initialize` is answered with `-32601` and names the one version this
server speaks; `-32022` names it for any other version a request asks for.

## Layout

- `src/mcp/json-rpc.{hxx,cc}` — `RpcRequest`, `RpcRejection`, `RpcResponse`,
  `RpcError`, `ErrorCode` (the standard codes and `-32021`/`-32022`),
  `parseRequest`, `parseResponse`, `requestFrame`, `resultFrame`,
  `errorFrame`, `parseJson`. A frame that is not an object, nests past 64
  levels or carries a `null` id is refused as such, never thrown.
- `src/mcp/schema.{hxx,cc}` — the input schema builders (`object`, `text`,
  `integer`, `number`, `boolean`, `choice`, `list`, `emptyObject`), `violation`
  (the first thing a value breaks, in the words the tool runtime has always
  used: `missing required argument 'x'`, `argument 'x' must be a string`,
  `argument 'x' has an invalid value`) and `unsupported` (a schema the server
  refuses to register: not an object schema, a `$ref`, a foreign dialect, more
  than 16 levels deep). Supported keywords: `type`, `enum`, `const`,
  `properties`, `required`, `additionalProperties`, `items`, `minItems`,
  `maxItems`, `minLength`, `maxLength`, `minimum`, `maximum`,
  `exclusiveMinimum`, `exclusiveMaximum`, `allOf`, `anyOf`, `oneOf`. Other
  keywords (`description`, `format`, `x-*`) are annotations and are ignored.
- `src/mcp/tool.{hxx,cc}` — `ToolSpec`, `ToolAnnotations`, `CallerContext`,
  `ToolInvocation`, `ToolOutcome`, `AppAction` and their JSON. The Argus
  extensions live in `_meta` under the `argus/` prefix: a tool's
  `argus/module` and `argus/capability`, a call's `argus/context` (who is
  asking: user id, role, language, session, the utterance, whether the
  decision was the model's) and a result's `argus/appAction` (an action the
  conversation's app must run).
- `src/mcp/server.{hxx,cc}` — `McpServer`: `add` (asynchronous handler that
  answers through a `Reply`, once, from any thread), `addSync`, `setGate`
  (the owner's authority: a `Refusal{code, message}` becomes a tool error
  that never reaches the handler), `handle` (a frame in, the answer through a
  callback) and `handleBlocking`. Registration throws on a bad name, a
  duplicate, a handler-less tool or an unsupported schema, so a service that
  boots has tools that can be served.
- `src/mcp/client.{hxx,cc}` — `McpClient` over a `Transport`: `discover`,
  `listTools` (follows `nextCursor`, at most 64 pages) and `callTool`. A
  failure says which kind it was: `Transport` (nothing came back), `Protocol`
  (a JSON-RPC error) or `Malformed` (an answer that is not one).
- `src/mcp/local-transport.hxx` — the in-process `Transport`: `argus-llm`
  reads its own core tools through it, so its memory tools travel the same
  path as everyone else's.
- `src/mcp/confirmation.{hxx,cc}` — `ConfirmationLedger`: the one-use,
  120-second, six-character token a destructive tool hands back with its
  preview and takes in its second call. It is bound to the user, the tool and
  the target, bounded at 256 pending tokens and replaced on a new preview.

## Rules

- Errors that a model can act on are tool results with `isError: true`
  (invalid arguments, a refused gate, a handler that threw); errors about the
  request itself (unknown tool, missing `_meta`, a frame that is not JSON) are
  JSON-RPC errors. A handler that throws never reveals why: the model reads
  `The tool failed: <name>`.
- The server never trusts what it was not given: the caller context is data a
  paired caller declares, which is why `contracts/mcp` admits the `llm`
  credential alone and refuses to be built open.
- Tools are registered at boot and read afterwards; the server holds no lock
  and `add` is not for a running process.
- Everything compiled here is a pure function of its arguments, apart from the
  ledger's mutex and the server's reply guard.

## Tests

`json-rpc-test` (framing, rejections, nesting), `schema-test` (every keyword,
the wording, depth, builders), `tool-test` (names, spec/outcome/context round
trips), `confirmation-test` (one use, binding, expiry, capacity),
`mcp-server-test` (discover, list order, call, gate, handler faults,
asynchronous replies, the timeout, registration refusals) and
`mcp-client-test` (an end-to-end read over `LocalTransport`, pagination, every
failure kind).
