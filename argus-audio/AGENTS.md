# argus-audio

The PCM helpers more than one service needs.

## What this is

A module, not a service: no database, no listener, no process. It exists
because argus-camera (the Tapo talk path) and argus-voice (session and
engine seam) both resample audio, and neither may reach into the other's
`src/`.

## Layout

- `src/shared/wrapper/audio/audio-resampler.{cc,hxx}` — the resampler.

## Rules

- Rule 25: the folder IS the module. One `argus_module(NAME audio ...)`;
  explicit source lists, never `file(GLOB)`.
- Include prefixes are load-bearing: consumers include
  `<shared/wrapper/audio/audio-resampler.hxx>`, so the path under `src/`
  keeps that shape.
- Keep it dependency-light. It links `argus_common` and nothing else; a
  service-specific dependency here would push that closure into every
  consumer.
