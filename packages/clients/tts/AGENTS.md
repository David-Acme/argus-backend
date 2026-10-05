# argus_clients_tts

The argus-tts wire seen from the caller's side: the modern gRPC client, the
transitional remote client that speaks the same wire, and the vocabulary both
of them put on it.

## What this is

A CLIENT module, not a service: one `argus_clients(NAME tts ...)`, a STATIC
library whose include root is `src/`, so a consumer writes
`<tts/tts-remote.hxx>` and links `argus::clients::tts`. It compiles the wire —
`argus_tts_rpc_contract()`, the function `packages/contracts/tts` owns, is
called in this package's `CMakeLists.txt` — and links
`argus::contracts::tts-wire` PRIVATE, so no protobuf header reaches a consumer
through it.

Four link lines in four CMakeLists carry it: `services/tts` twice
(`argus_tts-rpc` PUBLIC for the RPC server that answers this very protocol,
and the synthesis feature, which reads the vocabulary), `services/voice`
(`argus_voice-core`, the session's spoken answers) and `services/camera`
once (`argus::camera-control`, the module that owns the Tapo talk path; the
two talk suites take it transitively). Three CMakeLists also
add the package by path — four `if(NOT TARGET argus::clients::tts)` guards in
all, two in `services/tts` and one each in voice and camera — so each service
still builds standalone.

The package carries two client flavours, and that is worth stating plainly:
`tts-client.{hxx,cc}` is the modern gRPC client, and `tts-remote.{hxx,cc}` is
the transitional HTTP one with its own raw-socket transport and chunked-body
decoding. Voice and camera consume the transitional one; argus-tts consumes
the gRPC one to serve the same protocol.

## Layout

- `src/tts/tts-client.hxx` — `argus::tts::Client` with `ClientConfig`
  (`target`, `credential`, `timeout` defaulting to 30 s), `Capabilities`,
  `AudioChunk`, `SynthesisInput` and the `Quality` enum: `capabilities` is
  unary, `synthesize` streams chunk by chunk through `onChunk`; 4 files
  include it.
- `src/tts/tts-remote.hxx` — `TtsRemoteConfig` with its `resolve()`,
  `TtsHttpClient`, and `TtsClient`, the entry point consumers hold, plus
  `TtsRemoteStreamInput`, `TtsHttpStreamInput` and `WireRequest`; 4 files
  include it.
- `src/tts/tts-wire.hxx` — the vocabulary both flavours share: `TtsLang` (32
  enumerators, `kTtsLangCount` and a `static_assert` against it), `langCode`,
  `TtsQuality`, `TtsRequest`, `TtsChunkCallback` and `supportedLangCodes()`;
  6 files include it.
- `tests/support/fake-tts-server.hxx` — the in-process HTTP server serving the
  internal wire, driven by three suites outside the package
  (`voice-tts-remote-test.cc:4`, `camera-talk-cutover-test.cc:4`,
  `camera-action-rpc-test.cc:3`); 3 files include it, and the include path is
  added by those CMakeLists. This package's own suite does not use it, so its
  `CMakeLists.txt` does not add `tests/support` to any target.

## Rules

- Rule 25: the folder IS the module. One `argus_clients(NAME tts ...)` with an
  explicit source list, never `file(GLOB)`.
- Include prefixes are load-bearing: `<tts/tts-client.hxx>`,
  `<tts/tts-remote.hxx>`, `<tts/tts-wire.hxx>`.
- What a consumer sees: `TtsClient` — `defaultSpeed`, `sampleRate`,
  `synthesize`, `synthesizeStream` and `remote()` — with `TtsRequest`,
  `TtsLang` and `TtsQuality` as its arguments; and `argus::tts::Client` with
  `ClientConfig` when it wants gRPC itself. What it must not see: no protobuf
  type, no stub, no generated header, no URL constant, no retry policy. The
  endpoint is runtime configuration; the gRPC leg is one channel per client
  with a deadline, the HTTP leg one socket per call.
- The gRPC client's constructor is its gate, and it throws before dialling: an
  empty target, an empty credential, a timeout of zero or below, or a timeout
  over two minutes is `ResponseException(400, TtsErrors::InvalidRequest)` —
  status 400, wire code `"BAD_REQUEST"` — the definition is
  `packages/contracts/tts/src/tts/tts-errors.hxx:12`, the spelling
  `packages/lib/errors/src/errors/error-code.hxx:35`. Two minutes exactly is
  accepted.
- The endpoint resolves from process-global config, and this package owns both
  spellings: `TtsRemoteConfig::resolve()` reads `tts.remote_url` and
  `tts.remote_timeout_ms` (a non-positive timeout keeps the header's 30000);
  `rpcClient()` reads `tts.grpc_target` with `tts.grpc_credential`.
  `TtsClient::remote()` is true when either URL is set, and every entry point
  throws `std::runtime_error("tts.remote_url is not configured")` when neither
  is.
- The four templates that declare the HTTP pair all do it in an `[tts]`
  block, scheme-less: `services/voice/config.toml.example:15-16` and
  `services/camera/config.toml.example:65-66` (`127.0.0.1:7029`, 30000),
  `argus-deploy/config.voice.toml.example:16-17` (`172.19.0.29:7029`, 30000)
  and `argus-deploy/config.camera.toml.example:65-66` (`argus-tts:7029`,
  30000). The gRPC pair is declared in no toml at all — only runtime
  overrides in `services/tts/tests/unit/tts-rpc-test.cc` set it — so nothing
  ships the gRPC path enabled.
- Failure vocabulary: the gRPC path throws `ResponseException`, one of the
  nine contract refusals; the transitional path throws `std::runtime_error`
  with a frozen message ("argus-tts remote_url has no host" from
  `TtsHttpClient`'s constructor, "tts.remote_url is not configured" from the
  entry points, "argus-tts unreachable at <url>" and the rest from the
  transport). One ragged edge, measured and left as it is:
  `argus::net::parseEndpoint` runs `std::stoi` on the port, so a URL like `host:abc` escapes as
  `std::invalid_argument` rather than as a frozen message.
- Flagged deviations from §2.3, none of them hidden: there is no `details/`
  folder — the chunked decoding lives in the anonymous namespace of
  `tts-remote.cc`, while the URL parsing and the socket (connect, send,
  receive) are `packages/lib/net`'s `argus::net`, shared with the `stt` and
  `llm` clients that used to carry their own copies, and the gRPC channel and credential
  helpers are `lib/grpc`'s `argus::client`; `tts-wire.hxx` is read by the
  service as well as by callers — `tts-rpc-server.hxx`, `tts-service.hxx` and
  `onnx-utils.hxx` in `services/tts`, and `voice-session-service.hxx` in
  `services/voice`; and `supportedLangCodes()` is declared by the client's
  header at `tts-wire.hxx:126` but defined by the service in
  `services/tts/src/feature/synthesis/infra/supertonic/onnx-utils.cc:14`, so
  the client promises a symbol only argus-tts can provide.

## Tests

- `tests/unit/tts-client-test.cc` — five cases, and no server of any kind: the
  gRPC constructor's gate as a pinned table (an empty target, an empty
  credential, a zero timeout and a 121 s timeout, each 400 and `"BAD_REQUEST"`,
  with two minutes exactly accepted); `TtsHttpClient`'s hostless-URL refusal
  message by message, and two URLs a host makes acceptable; the endpoint
  resolution against the runtime knobs (unset is empty/false/30000, pointed is
  echoed and enabled, 1234 is 1234, and 0 leaves 30000 standing); the entry
  point's refusal with no endpoint configured, message and four call sites,
  plus `remote()` turning true once the knobs point somewhere; and the
  vocabulary (`TtsRequest`'s defaults, four `langCode` spellings, and all 32
  codes distinct and two characters). Server-free on purpose: everything it
  pins is decided before any I/O, so the suite cannot flake on a port.
- The package's fake is exercised by its consumers' suites instead
  (`services/voice/tests/unit/voice-tts-remote-test.cc`,
  `services/camera/tests/unit/camera-talk-cutover-test.cc`,
  `services/camera/tests/unit/camera-action-rpc-test.cc`), which is where the
  HTTP leg is tested end to end.
