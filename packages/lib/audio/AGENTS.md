# argus-audio

The PCM helpers more than one service needs.

## What this is

A module, not a service: no database, no listener, no process. It exists
because argus-camera (the Tapo talk path) and argus-voice (session and
engine seam) both resample audio, and neither may reach into the other's
`src/`.

## Layout

- `src/audio/audio-resampler.{cc,hxx}` — the resampler.
- `src/audio/endpoint-detector.{cc,hxx}` — `EndpointDetector`, the streaming
  endpointer camera's talk capture runs: an `EndpointConfig` of floors, frame
  counts and windows, and the `EndpointStatus` each frame returns.

## Rules

- Rule 25: the folder IS the module. One `argus_lib(NAME audio ...)`;
  explicit source lists, never `file(GLOB)`.
- Include prefixes are load-bearing: consumers include
  `<audio/audio-resampler.hxx>`, so the path under `src/`
  keeps that shape.
- Keep it dependency-light. It links nothing but the leaves it needs; a
  service-specific dependency here would push that closure into every
  consumer.
