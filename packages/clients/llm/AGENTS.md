# argus_clients_llm

The argus-llm wire seen from the caller's side: the chat contract, the tool
vocabulary a descriptor is written in, and the HTTP client that speaks it.

## What this is

A module, not a service: one `argus_clients(NAME llm ...)`, a STATIC library
whose include root is `src/`, so a consumer writes `<llm/llm-service.hxx>` and
links `argus::clients::llm`. Four packages link it — `services/llm`
(`llm-core`, `llm-wire-test`), `packages/memory` (`memory-core`),
`services/voice` (`argus::voice-core`) and `services/guard` (`argus-guard` and
its `guard` feature module): six link lines in five CMakeLists, four of which
also add the package to their own standalone tree by path. It carries no
engine — `llm-service.cc` (llama.cpp) belongs to `services/llm`, and nothing
in this package may link llama. argus-memory (formation and chat), argus-voice
(session streaming) and argus-guard (assessment) all reach argus-llm through
the client here.

## Layout

- `src/llm/llm-service.hxx` — `ChatMessage`, `ChatRequest` (with `maxTokens`,
  `temperature`, `resetContext`, `toolsEnabled`, `stop`, `grammar` and
  `grammarRequired`), `TokenCallback`, `LlmPrefillStats`, `GenerateInput`, and
  the `LlmService` in-process engine's own declaration; 14 files include it.
- `src/llm/tool-contracts.hxx` — `tools::ToolContext`, `tools::ToolCall`,
  `tools::ToolResult`, `tools::ToolArgumentSpec` and `tools::ToolDescriptor`;
  9 files include it.
- `src/llm/details/llm-remote.hxx` — `LlmRemoteConfig` with its `resolve()`,
  `LlmStreamInput`, and `LlmHttpClient` (`chat`, `chatStream`); 6 files
  include it.
- `tests/support/fake-llm-server.hxx` — the in-process HTTP server a suite
  drives the client end to end against; 2 files include it.

## Rules

- Rule 25: the folder IS the module. One `argus_clients(NAME llm ...)` with an
  explicit source list, never `file(GLOB)`.
- Include prefixes are load-bearing: `<llm/llm-service.hxx>`,
  `<llm/tool-contracts.hxx>`, `<llm/details/llm-remote.hxx>`.
- What a consumer sees: `LlmHttpClient` — a full completion out of `chat`, and
  a token-at-a-time `chatStream` whose sentinel becomes `onToken("", true)`
  with the prefill stats written through `LlmStreamInput` — plus
  `LlmRemoteConfig` and the chat DTOs. What it must not see: no protobuf type,
  no stub, no URL, no retry policy. This package compiles no `.proto` at all
  (it is declared without `PROTO`, an HTTP wire and not a gRPC SDK); the URL
  arrives at runtime; and there is no retry to configure — one socket per
  call, one deadline, `Connection: close` on the unary leg.
- Every failure is a `std::runtime_error`: `argus-llm <code>: <message>` off
  the frozen `{status, info, errors}` envelope, or `argus-llm <what>` for a
  transport failure ("unreachable at <url>", "response has no header block",
  "chunked body truncated", "stream ended without a sentinel"). A caller that
  cannot reach argus-llm degrades on that documented path, never a crash.
- The endpoint is runtime config, not a constant: `LlmRemoteConfig::resolve()`
  reads `llm.remote_url` and `llm.remote_timeout_ms` through `ConfigService`.
  Exactly two files declare them — `services/voice/config.toml`
  (`127.0.0.1:7032`, 120000) and `argus-deploy/config.voice.toml`
  (`172.19.0.32:7032`, 120000); neither argus-llm config carries either knob,
  so the switch lives on the consuming side. The declared values are
  scheme-less `host:port`, which `parseUrl` accepts; an empty value makes
  `enabled()` false and a non-positive `remote_timeout_ms` keeps the header's
  120000 default.
- No engine here, and no tool machinery either: the registry, validator and
  executor are argus-llm's (D16). `tool-contracts.hxx` is the vocabulary
  `packages/memory` declares its descriptors in and `services/llm` runs them
  over — a measured deviation from §2.3, deliberate and recorded (§9.3,
  Phase 4 step 7), because a package may not include a service's source.
- The remote transport sits under `details/`, which §2.3 reserves for channel,
  credentials, retry and envelope parsing — and five files outside the package
  still include it: `services/guard`'s `src/main.cc`, `guard-assessment.cc`
  and `tests/unit/guard-assessment-live-test.cc`, `services/voice`'s
  `voice-engine-seam.hxx`, and `packages/memory`'s `wire-memory-chat.hxx`.
  That is a known, flagged deviation: `LlmHttpClient` is the method surface
  §2.3 puts in `src/llm/<name>-*-client.{hxx,cc}`, and no file here carries
  that name (the translation unit is `details/llm-remote.cc`).
- `llm-service.hxx` carries both the wire DTOs and `LlmService`'s class
  declaration, so `services/llm` links a package named "client" to get its own
  contract. Splitting the DTOs out, the shape `tts-wire.hxx` has, is open.

## Tests

- `tests/unit/llm-client-test.cc` — four cases: the chat and stream legs end
  to end through `tests/support/fake-llm-server.hxx` (token order, the
  sentinel's 7/0/7 stats, both request counts, a coalesced generation that
  still arrives sentinel-free, a scheme-less url); the refusal and the
  truncated stream pinned by message; the hostless-url and
  unreachable-endpoint refusals; the two config knobs with their defaults and
  the non-positive-timeout fallback.
- `services/voice/tests/unit/voice-llm-remote-test.cc` drives the same fake
  server, adding `tests/support` to its include path (the only CMakeLists that
  does).
