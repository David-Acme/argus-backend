# argus-stt — CONTEXT

## Why the stt service exists

F4-3 of the `migracion-microservicios` plan extracts the STT engine out of
the legacy monolith (Rulings BK-BN). `argus-stt` is a sibling service with
its own binary, own CMake preset and zero HTTP exposure: it exists so the
legacy's voice session stops loading the sherpa-onnx recognizer in-process
and instead transcribes over the internal wire. It follows the argus-tts
pattern (F4-2) one engine later.

## What it owns

- **The sherpa-onnx STT engine** (shared-tree `SttService`, compiled into
  this binary via the PORTED pattern), loaded at boot from the shared
  `models/stt` tree (`stt.models_dir`; the build symlinks `../models` next
  to the binary). Boot aborts if the engine fails to load — the service is
  useless without its capacity. One global recognizer per Ruling BE: the
  engine knob `stt.language` is the boot default and the "" param resolves
  to it.
- **The internal wire (Ruling BL)**:
  - `POST /stt/v1/transcribe` — binary body `audio/x-argus-pcm-s16`
    (16 kHz mono int16, the float/int16 mapping the voice session itself
    uses, ±1 LSB quantization). `lang` comes from the query string
    (`?lang=es`) or the `lang` header; `""` resolves from the service's
    `stt.language`. Response is the frozen envelope with
    `info.text`; a `lang` different from the current recognizer language
    rebuilds the recognizer inside the transcribe's blocking leg — never on
    the Drogon IO thread (the legacy global semantics, ledgered as the
    BE contention, not fixed here). Latency is logged per request
    (`samples`, `lang`, `ms`).
  - `GET /stt/v1/config` — `{language, defaultLanguage, loaded}`
    (additive, internal-only).
  - Errors: frozen `{status, info, errors}` envelope — 400 `BAD_REQUEST`
    (wrong content-type / non-int16-aligned body), 422 validation (empty
    body, unsupported lang), 503 `STT_NOT_LOADED`, 404/405 routing. The
    success body carries `info.text` (the envelope wraps all JSON
    responses).
  - Trust model: no auth, loopback bind by default — internal-network only,
    never routed through the gateway.
- **Config**: `[stt]` (engine knobs, mirroring the legacy block) +
  `[server]` (loopback listener, default 7030) only. No database, no NATS,
  no JWT/device keys (Ruling BN): the dead `voice_session`/`voice_message`
  tables were dropped in F6-1 and nothing here persists anything.

## What it did NOT change

- The mobile app never talks to this service; voice frames and `/sync` are
  untouched and the gateway routing is untouched.
- VAD (Silero) and RNNoise stay in the legacy voice session (Ruling AY) —
  only the transcribe leg moves.
- Model artifacts stay in the shared `models/stt` paths — never copied.
- The legacy `SttService` stays linked in the legacy binary: only the boot
  init is gated behind `stt.remote_url` (Ruling BM), so the symbol proof
  for the legacy is unchanged by this task.

## Compose volume

The docker compose must mount the shared `models/` tree (at least
`models/stt`) into this service's working directory — the engine reads
`models/stt/*.onnx` relative to `stt.models_dir`.
