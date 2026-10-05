# argus_clients_llm

The argus-llm wire seen from the caller's side: the chat contract and the two
transports that speak it — the gRPC client `argus::llm::Client` and the HTTP
client the loopback face still runs on.

## What this is

A module, not a service: one `argus_clients(NAME llm ...)`, a STATIC library
whose include root is `src/`, so a consumer writes `<llm/llm-service.hxx>` and
links `argus::clients::llm`. Three units link it — `services/llm` four times
(the `argus-llm` executable's module list, the `argus::llm` feature module,
its `argus::memory` feature and, since Phase 4 step 6c, `argus::llm-rpc`),
`services/voice` (`argus::voice-core`) and `services/guard` (`argus-guard` and
its `guard` feature module): seven link lines in seven CMakeLists, plus the
package's own test target, and three of those units also add the package to
their own standalone tree by path. It carries no
engine — `llm-service.cc` (llama.cpp) belongs to `services/llm`, and nothing
in this package may link llama. `services/llm` (memory formation and chat
among its features, compiled into the brain since f8-b3), argus-voice
(session streaming) and argus-guard (assessment) all reach argus-llm through
the client here.

The package is the SDK for the internal gRPC wire as well: it links
`argus::contracts::llm-wire` **PRIVATE**, so the generated stub and the wire
message types stay out of its public headers and a server reaches the wire by
its own name — `services/llm`'s `argus::llm-rpc` names
`argus::contracts::llm-wire` itself. `argus::llm::Client` is the client half of
the `Chat` service `packages/contracts/llm/llm.proto` declares.

## Layout

- `src/llm/llm-service.hxx` — `ChatMessage`, `ChatRequest` (with `maxTokens`,
  `temperature`, `resetContext`, `toolsEnabled`, `stop`, `grammar`,
  `grammarRequired`, `userId`, `role`, `lang`, `clientActions`, `sessionId`
  (at most 128 bytes), `prefillOnly`, and the in-process-only
  `toolCallsAllowed`, which never crosses a wire), `TokenCallback`,
  `LlmPrefillStats`,
  `LlmStreamInput`,
  `GenerateInput`, and the `LlmService` in-process engine's own declaration;
  16 files include it.
- `src/llm/llm-client.{hxx,cc}` (46 + 187) — `argus::llm::kMaxTimeout` (two
  minutes), `ClientConfig` (target, credential, timeout), `Capabilities`
  (loaded, the engine's default token cap and temperature, the context size and
  the three last-prefill counters) and `argus::llm::Client` with
  `capabilities()`, `chat()` and `chatStream()`; 5 files include the header.
- `src/llm/llm-remote.{hxx,cc}` (60 + 512) — `LlmRemoteConfig` with its
  `resolve()`, `LlmHttpClient` (`chat`, `chatStream`) and the façade
  `LlmClient` (`chat`, `chatStream`, `remote()`), which picks the leg per call;
  6 files include the header.
- `tests/support/fake-llm-server.hxx` (236 lines) — the in-process HTTP server
  a suite drives the client end to end against; 2 files include it.

## Rules

- Rule 25: the folder IS the module. One `argus_clients(NAME llm ...)` with an
  explicit source list, never `file(GLOB)`.
- Include prefixes are load-bearing: `<llm/llm-service.hxx>`,
  `<llm/llm-client.hxx>`, `<llm/llm-remote.hxx>`.
  No `details/` level survives in this package: the `-client` name §2.3
  reserves is the gRPC client's, and the HTTP transport keeps the `-remote`
  name beside it, the spelling `stt`, `tts` and `vlm` use.
- The gRPC client's constructor is its gate, and its refusals are the shared
  vocabulary, not a `runtime_error`: an empty target or credential, and a
  timeout of zero or below or over `kMaxTimeout`, are 400 `InvalidRequest` —
  the timeout carrying `"timeout must be within 1 and 120000 ms"`, while two
  minutes exactly is accepted. `chat` and `chatStream` refuse a request that
  violates the wire bounds before the channel is used (no messages, 65 of them,
  an empty role or one over 32 bytes, empty content or over 32 KiB,
  `maxTokens` outside `0..4096`, a temperature outside `-1..2`, a grammar over
  8 KiB, a negative `userId`) with that same 400, and `capabilities()` refuses
  a reply outside `0..4096` / `0..2` / `1..2^22` / `0..2^20` as 502
  `InvalidResponse`. The client declares `temperature` and `tools` on every
  call, so the wire never sees either absent from this side; an absent one
  belongs to a raw caller, and the server resolves it to the engine's default
  (`temperature` `-1`, the tool loop on) exactly as an omitted HTTP key.
- Statuses map the way the rest of the tree's clients map them, with the one
  llm-specific split: `CANCELLED` is 504 `DeadlineExceeded` once the call's own
  deadline has passed and 499 `Cancelled` otherwise, and every other status
  goes through `argus::response::fromRpcStatus`. So the server's typed refusal
  arrives as its own `ResponseException` — 401 `Unauthorized` for a credential
  the server does not list, 503 `LlmEngineNotLoaded`, 429 `Busy`, 400
  `InvalidRequest`, 500 `InternalError` for what the server had to sanitize —
  and a `DEADLINE_EXCEEDED` or `UNAVAILABLE` status is 504 or 503.
- `LlmStreamInput::cancellation` is the caller's way out of a stream: on the
  gRPC leg a stop request `TryCancel`s the call and `chatStream` throws 499
  `Cancelled`; on the HTTP leg it shuts the socket down and throws
  `argus-llm stream cancelled`. A stop that arrives after the `done` token
  is not an error. `ChatRequest` carries the caller's `role` (default
  `UserRole::Guest`) and `lang` (`""`, `es` or `en`, anything else is 400);
  the HTTP body now sends `user_id`, `role` and `lang` as well, which it used
  to drop, plus `session_id` and `prefill_only` when they are set. A
  `sessionId` over 128 bytes is refused locally with 400 before any channel
  is used.
- The stream's `done` token is the client's end-of-stream marker:
  `chatStream` delivers each token through `onToken(text, false)`, writes the
  three counters into `LlmStreamInput::stats` from the terminating token, then
  calls `onToken("", true)` — the same spelling the HTTP sentinel produces. A
  stream that ends without one is 502 `InvalidResponse`; an empty generation is
  not an error on either leg.
- The HTTP stream's sentinel is out of band: it starts with
  `kStreamSentinelMark` (ASCII 0x1E, declared in `llm-service.hxx`), which
  the server strips from every token, so the client ends the stream only at
  that byte and a generated `{"done":true}` is just text; a marked chunk
  that does not parse is `argus-llm malformed stream sentinel`. The HTTP
  client sends `llm.grpc_credential` as `x-argus-credential`
  (`kCallerCredentialHeader`) on both legs, and argus-llm binds a declared
  user, role and tool loop to the `voice` caller's credential.
- The HTTP leg keeps its own, older failure type: every failure is a
  `std::runtime_error` — `argus-llm <code>: <message>` off the frozen
  `{status, info, errors}` envelope, or `argus-llm <what>` for a transport
  failure ("unreachable at <url>", "response has no header block", "chunked
  body truncated", "stream ended without a sentinel"). **The façade does not
  unify the two legs' error types** — a `LlmClient` caller sees
  `ResponseException` on the gRPC leg and `std::runtime_error` on the HTTP one
  — which is why every in-tree caller catches `std::exception` (guard's
  assessment round loop, the voice seam's session path).
- The endpoint is runtime config, not a constant, and there are two pairs of
  knobs. The HTTP leg is `LlmRemoteConfig::resolve()` over `llm.remote_url` and
  `llm.remote_timeout_ms`; the gRPC leg is `llm.grpc_target` and
  `llm.grpc_credential`. A non-empty target takes the gRPC leg on every call,
  an empty one falls back to HTTP, and `remote()` is true when either is
  configured. The façade keeps its gRPC client in a
  `std::atomic<std::shared_ptr<RpcCache>>` whose entry carries both the target
  **and the credential** it was built for (`compare_exchange_weak`), so a
  runtime change of either knob rebuilds before the next call and a rotated
  credential cannot silently reuse the stale client. The gRPC client's timeout
  is the configured budget clamped to `kMaxTimeout` — a non-positive budget
  takes that ceiling — because the typed leg's own gate refuses a longer one
  the HTTP leg would have accepted. Exactly four files declare the HTTP pair
  under an `[llm]` block: `services/voice/config.toml` and its
  `config.toml.example` (`remote_url = "127.0.0.1:7032"`, 120000) and
  `argus-deploy/config.voice.toml` beside its example
  (`172.19.0.32:7032`, 120000); the other `remote_url` pairs in those files and
  in the camera configs belong to `[stt]` and `[tts]`. And no toml
  in the tree declares the gRPC pair; the switch lives on the consuming side.
  The declared values are scheme-less `host:port`, which `parseUrl` accepts; an
  empty URL makes `enabled()` false and a non-positive `remote_timeout_ms`
  keeps the header's 120000 default, which is also the gRPC client's ceiling.
- No retry is configurable on either leg: one socket per call with one
  deadline and `Connection: close` on the HTTP unary leg, one channel per
  cached client with one deadline on the gRPC one.
- No engine here, and no tool machinery either: the registry, validator and
  executor are argus-llm's (D16). `tool-contracts.hxx` left this package at
  Phase 4 step 7 for `services/llm/src/shared/vocabulary/` (§9.3): the
  vocabulary `services/llm`'s memory feature declares its descriptors in and
  its tool runtime executes, which a tier-3 client could no longer hold once
  its two consumers were features of one service.
- `llm-service.hxx` carries both the wire DTOs and `LlmService`'s class
  declaration, so `services/llm` links a package named "client" to get its own
  contract. Splitting the DTOs out, the shape `tts-wire.hxx` has, is open.

## Tests

- `tests/unit/llm-client-test.cc` (316 lines) — six cases: the chat and stream
  legs end to end through `tests/support/fake-llm-server.hxx` (token order, the
  sentinel's 7/0/7 stats, both request counts, a coalesced generation that
  still arrives sentinel-free, a scheme-less url); the refusal and the
  truncated stream pinned by message; the hostless-url and
  unreachable-endpoint refusals; the two config knobs with their defaults and
  the non-positive-timeout fallback; the gRPC client's gate — the constructor
  table above plus the request-bound table, driven through `chat` and through
  `chatStream`; and the façade's leg choice, which asserts that a target
  nothing listens on answers 503 `SERVICE_UNAVAILABLE` on both legs **while the
  fake HTTP server's request count does not move** (no fallback when the knob
  is set), that a target set with an emptied credential is 400, and that
  clearing both knobs returns to the HTTP leg.
- `services/voice/tests/unit/voice-llm-remote-test.cc` drives the same fake
  server, adding `tests/support` to its include path (the only CMakeLists
  outside this package that does). The gRPC leg's own end-to-end suite is the
  service's,
  `services/llm/tests/unit/llm-rpc-test.cc`.
