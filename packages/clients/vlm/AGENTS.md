# argus_clients_vlm

The argus-vlm wire seen from the caller's side: the modern gRPC client, the
transitional HTTP client that answers the same caption, and the façade that
picks between them.

## What this is

A CLIENT module, not a service: one `argus_clients(NAME vlm ...)`, a STATIC
library whose include root is `src/`, so a consumer writes
`<vlm/vlm-remote.hxx>` and links `argus::clients::vlm`. It compiles the wire —
`argus_vlm_rpc_contract()`, the function `packages/contracts/vlm` owns, is
called in this package's `CMakeLists.txt` — and links
`argus::contracts::vlm-wire` PRIVATE, so no protobuf header reaches a consumer
through it. Its public `DEPENDS` are `argus::lib::config` (the transport
knobs), `argus::lib::errors`, `argus::lib::runtime` (`BlockingTask`, which
carries the blocking gRPC call off the event loop), `argus::lib::text` (for
`base64::encode`), `argus::contracts::vlm` (the refusal catalog) and Drogon.

Seven link lines in five CMakeLists carry it: `services/vlm` twice
(`argus_vlm-rpc` PUBLIC for the RPC server that answers this very protocol, and
the `argus-vlm` executable's module list), `services/guard` four times (the
`argus-guard` executable, `services/guard/CMakeLists.txt:112`; the `guard`
feature module that calls it, `src/feature/guard/CMakeLists.txt:28`; and the two
live suites `vlm-client-live-test` and `guard-assessment-live-test`, :199 and
:222), plus this package's own suite. Two CMakeLists also add the package by
path — one `if(NOT TARGET argus::clients::vlm)` guard each in `services/vlm`
(:37) and `services/guard` (:83) — so both still build standalone. No engine
lives here — description is `services/vlm`'s (llama.cpp + `libmtmd`), and
nothing in this package may link it.

The package carries two transports and one façade: `vlm-client.{hxx,cc}` is the
gRPC client for `argus.vlm.v1`, and `vlm-remote.{hxx,cc}` holds the
transitional HTTP client (`VlmHttpClient`) together with `VlmClient`, the entry
point guard holds, which picks the transport per call. `services/vlm` consumes
the gRPC client's types to serve the protocol.

## Layout

- `src/vlm/vlm-client.hxx` — `argus::vlm::Client` with `ClientConfig`
  (`target`, `credential`, `timeout` defaulting to 30 s), `kMaxTimeout`
  (120 s), `Capabilities` (`loaded`, `maxInputPx`, `defaultMaxTokens`) and
  `DescribeInput` (`jpeg`, `prompt`, `cameraId`, `maxTokens`, where `0` asks for
  the engine's default): both RPCs are unary — `capabilities()` and
  `describe(input)`, which answers the caption. 5 files include it.
- `src/vlm/vlm-remote.hxx` — `VlmDescribeInput` (`jpeg`, `prompt`,
  `cameraId`), `VlmDescribeResult` (`caption`), `VlmHttpClient` and the façade
  `VlmClient` (`explicit VlmClient(std::string baseUrl, double timeoutS = 8.0)`,
  a coroutine `describe(const VlmDescribeInput&) const` returning
  `drogon::Task<std::optional<VlmDescribeResult>>`, and `remote()`). 6 files
  include it, 4 of them in guard (`main.cc`, `guard-assessment.cc` and the two
  live suites) — the four lines this step repointed from `<vlm/vlm-client.hxx>`,
  the name the gRPC client now carries.
- `src/vlm/vlm-client.cc` — the gRPC channel, the credential header and the
  local gate; `src/vlm/vlm-remote.cc` — the HTTP body, the `POST
  /vlm/v1/describe` path, the reading of the `{status, info}` envelope and the
  façade's transport choice.
- `CONTEXT.md` — the package's "why" (rule 20).
- `tests/unit/vlm-client-test.cc` — the package's suite, described below.

## Rules

- Rule 25: the folder IS the module. One `argus_clients(NAME vlm ...)` with an
  explicit source list, never `file(GLOB)`.
- The include prefix is load-bearing: `<vlm/vlm-remote.hxx>` for the façade and
  `<vlm/vlm-client.hxx>` for the gRPC client. The `-client` name is the gRPC
  one, as §2.3 lays it out; the HTTP transport and the façade moved to the
  `-remote` spelling the stt package already uses, so guard's four include lines
  are the only consumer edit the move cost.
- What a consumer sees: `VlmClient::describe`, a coroutine handing back a
  `std::optional<VlmDescribeResult>`, plus `VlmClient::remote()` and the two
  DTOs; and `argus::vlm::Client` with `ClientConfig` when it wants gRPC itself.
  What it must not see: no engine handle, no llama or OpenCV type, no protobuf
  type, no stub, no generated header, no URL constant, no JSON key, no HTTP
  method or path, no retry policy.
- Transport selection is one decision in one place, from process-global config:
  `VlmClient::describe` takes the gRPC path when `vlm.grpc_target` is set —
  `vlm.grpc_credential` as the caller's credential, the constructor's
  `timeoutS` as the deadline — and otherwise the HTTP path to the constructor's
  `baseUrl`. `remote()` is true when either is set. The gRPC client is cached in
  a `std::atomic<std::shared_ptr<RpcCache>>` that holds the target it was built
  for: `rpcClient()` re-reads `vlm.grpc_target` on every call and adopts the
  cached client only while the target still matches (`compare_exchange_weak`),
  so a second thread that raced the first adopts the winner's client and a
  runtime change of the target rebuilds it before the next call. The blocking
  gRPC call runs inside `BlockingTask`, off the Drogon event loop, as rule 13c
  requires of a coroutine caller.
- The HTTP wire it owns: a POST to `/vlm/v1/describe` whose body is `image_b64`
  — standard-alphabet base64 with padding, so `argus` is `YXJndXM=` — with
  `prompt` always present (even when empty) and `camera_id` only when the
  caller set one. A value comes back only when the status is 200, the body
  parses as JSON, `info` is an object and `caption` is not empty.
- The gRPC client's constructor is its gate, and it throws before dialling: an
  empty target or an empty credential is `ResponseException(400,
  VlmErrors::InvalidRequest)` — status 400, wire code `"BAD_REQUEST"`, message
  `"Invalid vision request"` — and a timeout of zero or below or over two
  minutes is the same status and code carrying its own message,
  `"timeout must be within 1 and 120000 ms"`. Two minutes exactly is accepted,
  and the server serves it (see `services/vlm`'s deadline ceiling). `describe`
  refuses an empty JPEG or a negative `maxTokens` locally, before the channel
  is used; the upper token ceiling (4096) and the prompt and camera-id lengths
  are the server's to refuse. `capabilities()` validates the reply it receives
  — `max_input_px` within `1..16384`, `default_max_tokens` within `0..4096` —
  and refuses a malformed one with 502 `VlmErrors::InvalidResponse`, and so
  does a `describe` that answers an empty caption.
- Failure vocabulary: `argus::vlm::Client` throws `ResponseException` — one of
  the eleven contract refusals, or what `argus::response::fromRpcStatus` maps a
  bare transport failure to (503 `"Service unavailable"`) or a malformed detail
  to (502), and a `CANCELLED` status that arrives after the call's own deadline
  is reported as 504 `DeadlineExceeded`. The façade turns every
  `ResponseException` the gRPC leg raises into `std::nullopt` — the gRPC
  client's own construction included, so a leg pointed at a target with an empty
  credential, or carrying a timeout outside one millisecond to two minutes,
  answers no caption rather than throwing out of the coroutine — which is what
  guard already reads as "no caption". The HTTP leg is `std::nullopt` for every
  refused envelope and for a call with nothing to send (an empty `jpeg` or an
  empty `baseUrl` returns before the wire), but a transport failure is NOT
  `std::nullopt`: in drogon 1.9.13, `HttpRespAwaiter::await_suspend`
  (`lib/inc/drogon/HttpClient.h:384`) turns any non-Ok `ReqResult` — connection
  refused as much as a timeout — into a `drogon::HttpException` through the
  coroutine, and `drogon::sync_wait` rethrows it
  (`lib/inc/drogon/utils/coroutine.h:500`). The one consumer only checks the
  optional, at `guard-assessment.cc:298` and `:362`, so that exception escapes
  there; the suite pins both sides of it.
- No endpoint config of its own for the HTTP leg, by design: argus-guard reads
  `guard.assess.vlm_url` (declared in the deploy config at
  `argus-deploy/config.guard.toml:131`, `http://172.19.0.31:7031`, inside the
  `[guard.assess]` block at :128, and in both templates —
  `argus-deploy/config.guard.toml.example:109`,
  `services/guard/config.toml.example:113`) and
  hands the façade that URL with `guard.assess.timeout_ms / 1000.0` — default
  8000 ms, so 8.0 s, in both templates, while the deploy config sets 60000 at
  :133 (`services/guard/src/app/main.cc:155-159`). An empty URL
  means guard builds no façade at all, so the gRPC knobs only matter where the
  HTTP URL is also set. The gRPC pair (`vlm.grpc_target`,
  `vlm.grpc_credential`) is declared in no toml at all — only runtime overrides
  in this package's own suite and in `services/vlm/tests/unit/vlm-rpc-test.cc`
  set it — so nothing ships the gRPC path enabled.
- Flagged deviations from §2.3, neither hidden: there is no `details/` folder —
  the HTTP body and the envelope parsing share `vlm-remote.cc` with the façade,
  and the gRPC channel and credential helpers are `lib/grpc`'s
  `argus::client`; and the façade surface is declared in the global namespace
  (`VlmClient`, `VlmHttpClient`, `VlmDescribeInput`, `VlmDescribeResult`),
  where the gRPC client lives in `argus::vlm`, because guard's four files
  already spell the unqualified names.

## Tests

- `tests/unit/vlm-client-test.cc` — six cases, four of them against a real
  drogon server on a loopback port standing in for argus-vlm. "The describe
  request carries the JPEG as base64 on the wire": the round trip returns the
  canned caption and the captured request is a POST to `/vlm/v1/describe` whose
  `image_b64` is `YXJndXM=`, with `prompt` present and `camera_id` sent, then a
  bare ask that still sends an empty `prompt` and omits `camera_id`. "The
  describe envelope decides what the caller gets": a pinned table of four
  refusals — a blank caption, a non-object `info`, a body that is not JSON, a
  503 envelope — each of which must leave the caller without a value. "Describe
  stops before the wire, and raises when the wire is dead": an empty `jpeg` and
  an empty `baseUrl` are `nullopt` with nothing sent, while the same client
  against a dead port raises `drogon::HttpException`. "The gRPC client
  validates its configuration before it dials": the constructor gate as a
  pinned table (empty target and empty credential → `"Invalid vision
  request"`, the two out-of-band timeouts — zero and 121 s — →
  `"timeout must be within 1 and 120000 ms"`, all 400 `"BAD_REQUEST"`, two
  minutes accepted) plus the local empty-JPEG and negative-`maxTokens`
  refusals. "The guard facade selects its transport from the runtime knobs":
  nothing set → `remote()` false and `nullopt`; a gRPC target → `remote()` true
  and `nullopt` off a dead port rather than an exception; the knob cleared →
  `remote()` false and `nullopt` again, while a second façade built over the
  fake server's URL answers the canned caption on the HTTP leg. "A misconfigured
  gRPC leg answers no caption instead of throwing": with the target set and the
  credential empty, and again with a live HTTP leg behind a timeout past two
  minutes, the façade answers `nullopt` and raises nothing — the case is written
  against a live HTTP server so a fallback to the HTTP leg would answer the
  canned caption and fail the check.
- The gRPC leg against a real server — the roundtrip, the refusals read back
  from the wire, the façade over gRPC and the real engine — is pinned by
  `services/vlm/tests/unit/vlm-rpc-test.cc`, not here, because only that
  project can compose the server. The guard live suites need argus-vlm running.
