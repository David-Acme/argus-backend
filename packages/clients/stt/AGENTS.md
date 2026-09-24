# argus_clients_stt

The argus-stt wire seen from the caller's side: 16 kHz mono s16 PCM in, a
transcript out.

## What this is

A module, not a service: one `argus_clients(NAME stt ...)`, a STATIC library
whose include root is `src/`, so a consumer writes `<stt/stt-remote.hxx>` and
links `argus::clients::stt`. Three packages link it — `services/stt`
(`stt-core`, `stt-wire-test`), `services/voice` (`argus::voice-core`) and
`services/camera` (`argus::camera-actions`, the module that owns the listen
path): four link lines in three CMakeLists, each of which also adds the package
to its own standalone tree by path. The header IS the contract and both sides
include it: argus-voice
transcribes every turn through it once `stt.remote_url` is set, argus-camera's
listen path transcribes through it, and argus-stt's own controller includes it
for the constants. No engine lives here — recognition is `services/stt`'s
(sherpa-onnx), and nothing in this package may link it.

## Layout

- `src/stt/stt-remote.hxx` — the wire contract (`kWireSampleRate` 16000 and
  `kPcmScale` 32768.0F, the voice session's float/int16 mapping), plus
  `SttRemoteConfig` with its `resolve()`, `SttWireRequest`, and
  `SttHttpClient::transcribe`; 4 files include it.
- `tests/support/fake-stt-server.hxx` — the in-process HTTP server a suite
  drives the client end to end against; 4 files include it.

## Rules

- Rule 25: the folder IS the module. One `argus_clients(NAME stt ...)` with an
  explicit source list, never `file(GLOB)`.
- The include prefix is load-bearing: `<stt/stt-remote.hxx>`. The new spellings
  are this and nothing else — the header moved from
  `src/shared/services/stt/remote/` and no consumer kept the old one.
- What a consumer sees: `SttHttpClient::transcribe(samples, lang)` — 16 kHz
  mono float samples in, the transcript string out — plus `SttRemoteConfig`
  and the two wire constants. What it must not see: no engine handle and no
  ONNX or sherpa type, no Drogon controller, no URL, no retry policy. The body
  is `samples.size() * 2` bytes of `audio/x-argus-pcm-s16` posted to
  `/stt/v1/transcribe?lang=<lang>` (an empty `lang` is resolved server-side),
  one socket per call with `stt.remote_timeout_ms` as the single deadline.
- Every failure is a `std::runtime_error`: `argus-stt <code>: <message>` off
  the frozen `{status, info, errors}` envelope, or `argus-stt <what>` for a
  transport failure ("unreachable at <url>", "response has no header block",
  "closed the connection before answering") — and an empty body is refused
  before any socket opens. A caller that cannot reach argus-stt degrades to
  the documented `stt_failed` path, never a crash.
- The endpoint is runtime config, not a constant: `SttRemoteConfig::resolve()`
  reads `stt.remote_url` and `stt.remote_timeout_ms` through `ConfigService`.
  Three files declare them — `services/voice/config.toml` (`127.0.0.1:7030`,
  30000), `argus-deploy/config.voice.toml` (`172.19.0.30:7030`, 30000) and
  `argus-deploy/config.camera.toml` (`172.19.0.30:7030`, 30000). No argus-stt
  config carries either knob, and `services/camera/config.toml` has no `[stt]`
  block at all, so the camera transcriber is off in that tree and answers an
  empty transcript. The declared values are scheme-less `host:port`, which the
  client's `parseUrl` accepts; an empty value makes `enabled()` false and a
  non-positive `remote_timeout_ms` keeps the header's 30000 default.
- §2.3 gives a client a `details/` for channel, credentials, retry and
  envelope parsing, and a `<name>-*-client.{hxx,cc}` for the method surface.
  This package has **no `details/` directory**: the URL parsing, the socket
  plumbing, the PCM scaling and the envelope parsing share the top-level
  `stt-remote.hxx`/`stt-remote.cc` with `SttHttpClient`, and no file here
  carries the `-client` name. A known, flagged deviation, left as the layout
  put it.
- `stt-remote.hxx` is not the caller's half alone: it is the one vocabulary
  both sides of the wire agree on, which is why the controller that serves
  `/stt/v1/transcribe` includes the client header rather than a copy of the
  constants.

## Tests

- `tests/unit/stt-client-test.cc` — the package's first unit suite; four
  cases: the wire end to end through `tests/support/fake-stt-server.hxx`
  (transcript per `lang`, both request counts, the 3200-byte s16 body, the
  constants, a scheme-less url); the refusal mapped to the frozen envelope and
  the unreachable endpoint, each pinned by message; the empty-body and
  hostless-url refusals, which open no socket; the two config knobs with their
  defaults and the non-positive-timeout fallback.
- The fake server has four includers: this suite and three consumer suites —
  `services/voice`'s two remote suites and `services/camera`'s action-rpc
  suite. Three CMakeLists put `packages/clients/stt/tests/support` on an
  include path: those two services, and `services/stt`, whose `stt-wire-test`
  carries the path but includes nothing from it.
