# argus_clients_stt

The argus-stt wire seen from the caller's side: the modern gRPC client, the
transitional HTTP client that answers the same transcript, and the façade that
picks between them.

## What this is

A CLIENT module, not a service: one `argus_clients(NAME stt ...)`, a STATIC
library whose include root is `src/`, so a consumer writes `<stt/stt-remote.hxx>`
and links `argus::clients::stt`. It compiles the wire —
`argus_stt_rpc_contract()`, the function `packages/contracts/stt` owns, is
called in this package's `CMakeLists.txt` — and links
`argus::contracts::stt-wire` PRIVATE, so no protobuf header reaches a consumer
through it.

Six link lines in six CMakeLists carry it: five consumers — `services/stt`
three times (`argus_stt-rpc` PUBLIC for the RPC server that answers this very
protocol, the `argus-stt` executable's module list, and the stt feature) and
one each in `services/voice` (`argus_voice-core`, the session's turns) and
`services/camera` (`argus::camera-actions`, the listen path) — plus this
package's own suite. Three CMakeLists also add the
package by path — one `if(NOT TARGET argus::clients::stt)` guard each in
`services/stt`, voice and camera — so each service still builds standalone. No
engine lives here — recognition is `services/stt`'s (sherpa-onnx), and nothing
in this package may link it.

The package carries two transports and one façade: `stt-client.{hxx,cc}` is the
gRPC client for `argus.stt.v1`, and `stt-remote.{hxx,cc}` holds the transitional
HTTP client (`SttHttpClient`) together with `SttClient`, the entry point
consumers hold, which picks the transport per call. Both live consumers hold the
façade; `services/stt` consumes the gRPC client to serve the protocol.

## Layout

- `src/stt/stt-client.hxx` — `argus::stt::Client` with `ClientConfig`
  (`target`, `credential`, `timeout` defaulting to 30 s), `Capabilities`,
  `TranscribeInput` (`samples`, `sampleRate`, `language`, `cancellation`):
  both RPCs are unary — `capabilities()` and `transcribe(input)` — and the
  sample form is float in [-1, 1], not the s16 the HTTP leg posts; 4 files
  include it.
- `src/stt/stt-remote.hxx` — the wire contract (`kWireSampleRate` 16000 and
  `kPcmScale` 32768.0F, the voice session's float/int16 mapping),
  `SttRemoteConfig` with its `resolve()`, `SttWireRequest`,
  `SttHttpClient::transcribe`, and the façade `SttClient` with `transcribe`
  and `remote()`; 6 files include it.
- `tests/support/fake-stt-server.hxx` — the in-process HTTP server a suite
  drives the HTTP client end to end against; 4 files include it (this
  package's own suite and three consumer suites), and three CMakeLists put
  `tests/support` on an include path — this package's own, `services/voice`
  and `services/camera`.

## Rules

- Rule 25: the folder IS the module. One `argus_clients(NAME stt ...)` with an
  explicit source list, never `file(GLOB)`.
- The include prefix is load-bearing: `<stt/stt-remote.hxx>` and
  `<stt/stt-client.hxx>`. The spellings are these and nothing else — the HTTP
  header moved from `src/shared/services/stt/remote/` and no consumer kept the
  old one.
- What a consumer sees: `SttClient::transcribe(samples, lang)` — 16 kHz mono
  float samples in, the transcript string out — plus `SttClient::remote()`,
  `SttRemoteConfig` and the two wire constants; and `argus::stt::Client` with
  `ClientConfig` when it wants gRPC itself. What it must not see: no engine
  handle and no ONNX or sherpa type, no Drogon controller, no protobuf type, no
  stub, no generated header, no URL constant, no retry policy.
- Transport selection is one decision in one place, from process-global config:
  `SttClient::transcribe` takes the gRPC path when `stt.grpc_target` is set —
  `stt.grpc_credential` as the caller's credential, `stt.remote_timeout_ms` as
  the deadline — and otherwise the HTTP path when `SttRemoteConfig` is enabled;
  with neither set it throws `std::runtime_error("stt.remote_url is not
  configured")`. `SttClient::remote()` is true when either is set. The gRPC
  client is cached in a `std::atomic<std::shared_ptr<RpcCache>>` that holds the
  target it was built for: `rpcClient()` re-reads `stt.grpc_target` on every
  call and adopts the cached client only while the target still matches, so a
  second thread that raced the first adopts the winner's client and a runtime
  change of the target rebuilds it before the next call.
- The HTTP body is `samples.size() * 2` bytes of `audio/x-argus-pcm-s16`
  posted to `/stt/v1/transcribe?lang=<lang>` (an empty `lang` is resolved
  server-side), one socket per call with `stt.remote_timeout_ms` as the single
  deadline. An empty body is refused before any socket opens.
- The gRPC client's constructor is its gate, and it throws before dialling: an
  empty target, an empty credential, a timeout of zero or below, or a timeout
  over two minutes is `ResponseException(400, SttErrors::InvalidRequest)` —
  status 400, wire code `"BAD_REQUEST"`. Two minutes exactly is accepted. The
  empty target and the empty credential carry the catalog message
  (`"Invalid transcription request"`); a timeout outside the band carries its
  own on the same status and code — `"timeout must be within 1 and 120000 ms"`.
  The same gate covers the input: empty samples or a sample rate outside
  8000..192000 is refused locally, before the channel is used, and so is a
  cancellation token that is already stopped (499). `capabilities()` validates
  the reply it receives — a rate inside 8000..192000, a non-empty `language`, a
  non-empty `default_language`, a non-empty language list — and refuses a
  malformed one with 502 `SttErrors::InvalidResponse`.
- Failure vocabulary, two flavours: the gRPC path throws `ResponseException` —
  one of the eleven contract refusals, or what
  `argus::response::fromRpcStatus` maps a bare transport failure to (503
  `"Service unavailable"`) or a malformed detail to (502
  `"Invalid service response"`); the HTTP path throws `std::runtime_error`
  with a frozen message — `"argus-stt remote_url has no host"` from
  `SttHttpClient`'s constructor, `"argus-stt transcribe needs a non-empty
  body"`, `"stt.remote_url is not configured"`, `"argus-stt unreachable at
  <url>"`, and `"argus-stt <code>: <message>"` off the frozen
  `{status, info, errors}` envelope. A caller that cannot reach argus-stt
  degrades to the documented `stt_failed` path, never a crash. One ragged edge,
  measured and left as it is: `parseUrl` runs `std::stoi` on the port, so a URL
  like `host:abc` escapes as `std::invalid_argument` rather than as a frozen
  message.
- The endpoint is runtime config, not a constant: `SttRemoteConfig::resolve()`
  reads `stt.remote_url` and `stt.remote_timeout_ms` through `ConfigService`.
  Three runtime configs declare them — `services/voice/config.toml`
  (`127.0.0.1:7030`, 30000), `argus-deploy/config.voice.toml`
  (`172.19.0.30:7030`, 30000) and `argus-deploy/config.camera.toml`
  (`172.19.0.30:7030`, 30000) — and four templates carry the same pair for
  the next install: `services/voice/config.toml.example` and
  `services/camera/config.toml.example` (`127.0.0.1:7030`),
  `argus-deploy/config.voice.toml.example` (`172.19.0.30:7030`) and
  `argus-deploy/config.camera.toml.example` (`argus-stt:7030`). No argus-stt
  config carries either knob, and `services/camera/config.toml` has no `[stt]`
  block at all, so the camera transcriber is off in that tree and answers an
  empty transcript. The declared values are scheme-less `host:port`, which the
  client's `parseUrl` accepts; an empty value makes `enabled()` false and a
  non-positive `remote_timeout_ms` keeps the header's 30000 default. The gRPC
  pair (`stt.grpc_target`, `stt.grpc_credential`) is declared in no toml at
  all — only runtime overrides in this package's own suite and in
  `services/stt/tests/unit/stt-rpc-test.cc` set it — so nothing ships the gRPC
  path enabled.
- The `-client` name is the gRPC one here, as §2.3 lays it out; the HTTP client
  and the façade keep the `-remote` spelling their consumers already import.
  This package has **no `details/` directory**: the URL parsing, the socket
  plumbing, the PCM scaling and the envelope parsing share the top-level
  `stt-remote.cc` with `SttHttpClient`, and the gRPC channel and credential
  helpers are `lib/grpc`'s `argus::client`. A known, flagged deviation, left as
  the layout put it.
- `stt-remote.hxx` is not the caller's half alone: it is the one vocabulary
  both sides of the HTTP wire agree on, which is why the controller that serves
  `/stt/v1/transcribe` includes the client header rather than a copy of the
  constants.

## Tests

- `tests/unit/stt-client-test.cc` — six cases: the HTTP wire end to end through
  `tests/support/fake-stt-server.hxx` (transcript per `lang`, both request
  counts, the 3200-byte s16 body, the constants, a scheme-less url, and the
  façade answering over it); the refusal mapped to the frozen envelope and the
  unreachable endpoint, each pinned by message; the empty-body and hostless-url
  refusals, which open no socket; the two config knobs with their defaults and
  the non-positive-timeout fallback; the gRPC constructor gate as a pinned
  table (empty target, empty credential and the two out-of-band timeouts — zero
  and 121 s — each 400 `"BAD_REQUEST"`, the timeouts carrying
  `"timeout must be within 1 and 120000 ms"`, two minutes accepted) plus the
  local empty-samples refusal;
  and the façade's transport selection against the runtime knobs (nothing set →
  the `stt.remote_url is not configured` refusal and `remote()` false, a gRPC
  target → `remote()` true and a 503 `SERVICE_UNAVAILABLE` off a dead port, the
  knob cleared → back to the refusal).
- The fake server has four includers: this suite and three consumer suites —
  `services/voice`'s two remote suites and `services/camera`'s action-rpc
  suite.
