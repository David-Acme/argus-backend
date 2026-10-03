# argus-stt — CONTEXT

## Why the stt service exists

F4-3 of the `migracion-microservicios` plan extracts the STT engine out of
the legacy monolith (Rulings BK-BN). `argus-stt` is a sibling service with
its own binary, own CMake preset and zero HTTP exposure: it exists so the
legacy's voice session stops loading the sherpa-onnx recognizer in-process
and instead transcribes over the internal wire. It follows the argus-tts
pattern (F4-2) one engine later.

## What it owns

- **The sherpa-onnx STT engine** (`SttService`, the stt feature's own facade
  compiled into this binary through `argus::stt`), loaded at boot from the
  shared `models/stt` tree (`stt.models_dir`; the build symlinks `../models`
  next to the binary). Boot aborts if the engine fails to load — the service is
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
    never announced or published.
- **The internal gRPC face (Phase 4 step 6, D16)** — `src/app/rpc/` compiles
  `argus::stt-rpc`, the server side of `argus.stt.v1`: a unary
  `Capabilities` (rate, loaded, current and default language, the languages
  the engine accepts) and a unary `Transcribe` (float samples + rate +
  language in, the transcript out). It answers the same `SttService` the HTTP
  controller drives, through one `SttRpcInput` the composition root fills — a
  live `Capabilities` callback, not a boot snapshot, so `loaded` and
  `language` stay true as the engine changes, and the language gate beside it
  as a `std::function<bool(const std::string&)>` bound to the service's own
  `isSupportedLanguage`, so a request is validated without copying the
  `Capabilities` list and an empty language still resolves server-side.
  Composition is opt-in: the leg
  exists only when `rpc.address` and at least one non-empty `[rpc.callers]`
  pair are set, and it refuses an unlisted caller with 401 before any engine
  work — the credential header must arrive exactly once, zero or two entries
  are the same 401 — refuses empty samples / a rate outside 8000..192000 / a
  language the engine does not accept with 400, and refuses a caller-declared
  deadline more than two minutes out with that same 400 while serving a caller
  that declares none. It answers 429 when the inference slots are
  exhausted and maps every `ResponseException` to its wire status (anything
  else becomes a 500 with no leaked detail). The wire's rates are the
  client's: the samples are float in [-1, 1] at `kWireSampleRate`, the same
  form the voice session works in.
- **Config**: `[stt]` (engine knobs, mirroring the legacy block) +
  `[server]` (loopback listener, default 7030) + `[rpc]`
  (`address`/`callers`, both empty in the template) only. No database, no
  NATS, no JWT/device keys (Ruling BN): the dead `voice_session`/
  `voice_message` tables were dropped in F6-1 and nothing here persists
  anything.

## What it did NOT change

- The mobile app never talks to this service; voice frames and `/sync` are
  untouched and the internal wire stays loopback-only, unannounced.
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

## Phase 4 step 9: config resolution into `src/config/` (D20)

The listener and the optional gRPC leg are resolved by
`src/config/stt-config.{hxx,cc}` (`argus::stt-config`):
`SttConfig::resolveListener()` (`ListenerConfig::resolve(7030)`) and
`SttConfig::resolveRpc()` (`rpc.address` plus the `rpc.callers` credential
pairs, empty ones dropped). `main.cc` keeps `config.toml` loading,
`drogonConfig` and the boot gate on the resolved address and credentials.

## Owner settings

`src/feature/settings/stt-settings.cc` is the catalog an owner may change
through `argus.settings.v1.Settings`, registered on the same gRPC listener as
`argus.stt.v1` (`argus::contracts::settings-wire`). Both keys are advanced:
the call language an owner thinks about lives in argus-voice, which sends a
language on every request, so the STT default only serves callers that send
none.

- `stt.language` (`es`/`en`/`auto`, fallback `es`) is **live**.
  `transcribe()` resolves an empty request language through
  `configLanguage()` on every call, and a language different from the
  loaded recognizer's already rebuilds the recognizer inside the blocking
  leg. A new default therefore costs one rebuild on the next request that
  carries no language, and never mismatches the loaded model.
- `stt.engine` (`nemo_transducer`/`whisper`/`canary`/`nemo_ctc`/
  `omnilingual`, fallback `nemo_transducer`) is **restart**. The engine is
  resolved once by `init()` into `engine_`, and every later recognizer
  rebuild (a language switch) reuses it. Before this, `createRecognizer`
  re-read `stt.engine` on every language switch, so a changed key would have
  silently swapped the engine on the next switch; pinning it makes
  "restart" true.
- An absent `stt.engine` used to mean whisper while the template, the
  provisioning and this document all name `nemo_transducer`. The code now
  runs `nemo_transducer` when the key is absent or unrecognized (`whisper`
  must be named), so the catalog fallback is what the service really runs.
  `SttService::configEngine()` / `engineName()` are the one spelling.

The thread count is not a key (`ThreadBudget::computeThreads()`), so it is
not in the catalog; models_dir, ports, addresses and callers never are. The
`settings` entry of `[rpc.callers]` is the only credential the settings
service accepts, and `SttConfig::resolveRpc()` removes it from the
transcription callers (`settingsCallers` / `withoutSettingsCaller`): the
settings caller cannot transcribe and a transcription caller cannot change
settings. `stt-settings-test` checks both directions on a live listener.
