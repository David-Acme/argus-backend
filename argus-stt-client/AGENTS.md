# argus-stt-client

The argus-stt wire seen from the caller's side.

## What this is

A module, not a service. argus-voice transcribes through argus-stt over
the internal wire, and argus-stt's own controller includes the same
header — the header IS the contract (16 kHz mono s16 PCM in, transcript
out), so it lives here rather than in either service.

## Layout

- `src/shared/services/stt/remote/stt-remote.{cc,hxx}` — contract + client.
- `tests/support/fake-stt-server.hxx` — the fake consumers' suites drive.

## Rules

- Rule 25: the folder IS the module. One `argus_module(NAME stt-client ...)`.
- Include prefixes are load-bearing:
  `<shared/services/stt/remote/stt-remote.hxx>`.
- No engine. Recognition happens in argus-stt (sherpa-onnx); nothing here
  may link it.
- A caller that cannot reach argus-stt degrades to the documented
  `stt_failed` path, never a crash.
