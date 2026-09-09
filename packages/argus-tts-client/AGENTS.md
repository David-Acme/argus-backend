# argus-tts-client

The argus-tts wire seen from the caller's side: the contract and the
client that speaks it.

## What this is

A module, not a service. argus-camera (the Tapo talk path) and
argus-voice both synthesize speech through argus-tts over HTTP, and
argus-tts itself needs the same contract — so the wire lives here rather
than in any one of the three.

## Layout

- `src/shared/services/tts/tts-wire.hxx` — the request/response contract.
- `src/shared/services/tts/remote/tts-remote.{cc,hxx}` — the HTTP client.
- `tests/support/fake-tts-server.hxx` — the fake every consumer's suite
  drives instead of standing up argus-tts.

## Rules

- Rule 25: the folder IS the module. One `argus_module(NAME tts-client ...)`.
- Include prefixes are load-bearing: consumers include
  `<shared/services/tts/remote/tts-remote.hxx>` and
  `<shared/services/tts/tts-wire.hxx>`.
- No engine. Synthesis happens in argus-tts; nothing here loads a model,
  and nothing here may depend on onnxruntime.
- A caller that cannot reach argus-tts degrades to a documented envelope
  (the 502 the camera talk path returns), never a crash.
