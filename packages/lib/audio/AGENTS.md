# argus-audio

The PCM helpers more than one service needs.

## What this is

A module, not a service: no database, no listener, no process. It exists
because argus-camera (the Tapo talk path) and argus-voice (session and
engine seam) both resample audio, and neither may reach into the other's
`src/`.

## Layout

- `src/audio/audio-resampler.{cc,hxx}` — the resampler: a windowed-sinc
  filter whose position is an exact rational (`whole + fraction/den`, with
  `den = target / gcd`), so the read point never drifts and the filter
  phases are a fixed table built once in the constructor (`den` phases,
  capped at 1024). The earlier tap cache keyed on a floating-point fraction
  that drifted with every step, so it kept inserting new 65-tap entries for
  as long as a stream ran: tens of MB per hour per voice or talk stream, and
  a cache miss on most samples. `audio-resampler-test` pins a tone within one
  percent for the voice rates, chunked input equal to one block, and an
  exact output count over ten minutes of streaming.
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
