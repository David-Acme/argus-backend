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
  - Since the 2026-10-05 audit (#98) the body is parsed by
    `TranscribeDto` (`feature/stt/dtos/`): content type and alignment are
    still 400, an empty body or one longer than 120 s of audio
    (3 840 000 bytes, the gRPC stream's own ceiling) is 422 on `body`, and
    the int16 → float conversion runs inside the Heavy `BlockingTask` with
    the transcription, reading the request's body in place, instead of on
    the Drogon loop. `client_max_body_size` in the template drops from 64M
    to 4M.
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

## Real-time behaviour (measured 2026-10-03)

Host: Ryzen 7 5825U (8 cores, 16 threads), CPU only, prod builds of the
baseline (48b75ba2) and of this work, `nemo_transducer` (sherpa-onnx
FastConformer RNN-T int8, en/de/es/fr), `ThreadBudget::computeThreads()` = 8.
The figures are medians of 5 requests over the HTTP leg, loopback round trip
included. The audio is the TTS preview voices cut to 2, 4 and 6 s. Other
agents shared the machine, so the load average is given for each run.

| utterance | baseline (load 5–7) | this work (load 1–3) | this work (load 7–9) |
|---|---|---|---|
| es 2 s | 118 ms | 61 ms | 100 ms |
| es 4 s | 172 ms | 97 ms | 148 ms |
| es 6 s | 203 ms | 116 ms | 197 ms |
| en 2 s | 89 ms | 57 ms | 97 ms |
| en 4 s | 175 ms | 88 ms | 170 ms |
| en 6 s | 230 ms | 127 ms | 192 ms |
| es/en alternating, 2 s | **1228 ms** | **70 ms** | **88 ms** |

The single-language decode is the same code in both builds and follows the
load: RTF 0.02–0.06. The alternating row is the change, below. In the voice
agent's live call the server-side STT time was 38–70 ms per turn.

**Language switches no longer reload the model.** A request whose language
differed from the loaded recognizer's rebuilt the recognizer inside the
blocking leg, even for the engines that ignore the language
(`nemo_transducer`, `nemo_ctc`, `omnilingual`: the transducer is one
multilingual model). Alternating es/en requests cost 1228 ms each against
104 ms for the same request in one language: the whole 131 MB encoder was
loaded again every time. `SttService::languageBound()` names the two engines
whose recognizer depends on the language (`whisper`, `canary`); for the
others a switch only records the language. Two calls in different languages,
or the camera's listen path beside a call, no longer pay a model load per
turn. `whisper` now maps `auto` to its own auto-detect (an empty language)
instead of an invalid code.

**Streaming (`TranscribeStream`).** The engine is offline: FastConformer
attends over the whole utterance, so it cannot emit a final transcript before
the audio ends. The stream instead moves the decode off the end of the turn.
The caller pushes audio while the user speaks and sends `flush` when its own
VAD hears the pause begin. The server decodes everything received so far and
answers a partial. If the pause becomes the endpoint, the final is that
partial, returned without a second decode (`decode_ms = 0`). If speech
resumes, the caller keeps pushing, including the pause audio it held back,
and the final decodes all of it. argus-voice adopted it on 2026-10-04: it flushes 128 ms into the
pause and its endpoint fires at 384 ms, so the decode at the pause
(≈40–210 ms above) finishes inside that wait; the server-side transcript time
after the endpoint went from a median 50 ms to 0 (see the voice CONTEXT). The transcript is ready when the endpoint fires,
instead of 100–210 ms after it. The cost is one extra decode when speech
resumes after a flushed pause. A partial is best effort: when every slot is
busy it repeats the last text instead of waiting, while the final waits for a
slot until the stream's deadline. Streaming models with a true incremental
encoder (sherpa-onnx online zipformer and NeMo streaming FastConformer) exist
for English, and for Spanish only as a separate single-language model. That
would mean two artifacts, a per-language accuracy regression against the
offline model and a new provisioning path, so the offline model stays and
only the decode moves.

## Pinned model download (2026-10-05 audit #30)

`scripts/provision.sh` downloads the model with `curl -fL` into a `.part`
file and moves it into place only after its SHA-256 matches the pin in the
script, from a pinned revision (no `resolve/main`). The pins were filled on
2026-10-05 from the Hugging Face LFS metadata and the GitHub release digest
of that revision, and checked against the files installed on the
development host. A present file whose hash does not match is replaced; a
pin left empty makes the script refuse to download and say so, and
`ARGUS_ALLOW_UNPINNED_MODELS=1` downloads it and prints its hash to pin.

## The `voice-stt` component (selectable modules, 2026-10-05)

argus-stt owns the core catalog component `voice-stt`
(`services/settings/modules.json`; `services/settings/CONTEXT.md`,
"Modules"): the four NeMo FastConformer transducer files (`stt/nemo-transducer-*`). Its source is `provisioned`: only the host produces
it (`services/stt/scripts/provision.sh`), so this service reports it and
never fetches it.

- `feature/settings/stt-components.{hxx,cc}` builds the shared
  `DiskComponentHost` for `voice-stt` with no fetch; `main.cc` attaches it to the
  `SettingsRpcService` when the settings caller is paired. The models root is
  `[components] models_dir` (`models` by default, `/opt/argus/models` in the
  deploy), the root the catalog's paths are relative to.
- `ComponentStates`: `installed` when every catalog file exists and is
  non-empty, otherwise `host_only` with the host command; `bytesPresent` sums
  the catalog sizes of the files present. `ready` is `SttService::instance().isLoaded()`, never the
  files alone.
- `InstallComponent` answers `host_only` and touches nothing;
  `RemoveComponent` is refused (`INVALID_ARGUMENT`): a provisioned
  component's files are the host's and `voice-stt` is core.
- The engine still refuses to boot without its model, as before, so a missing
  component normally shows as an unreachable owner in `GET /modules`; the
  states above answer while the service runs. Caveat: `[stt] engine` selects another model (whisper, canary, ...): the engine then loads, `ready` would be true, but the transducer files may be missing, so the component reads `host_only` while the service transcribes.

`tests/unit/stt-components-test.cc` drives the wire in process: host_only and
the command when missing, partial byte counts, installed, `ready` following
the engine flag, install answering host_only with an untouched models dir,
remove refused with the files kept, a foreign component refused, the default
root.

## Accuracy baseline (measured 2026-10-06)

`tests/eval/stt-wer-eval.cc` loads the deployed engine in-process (`nemo_transducer`, `es`, the
same models directory the service mounts), decodes 16 kHz mono clips and reports word and
character error rates per set. `scripts/stt-eval-data.py` fetches the clips into `models/stt-eval/`
(untracked) and writes `manifest.tsv` with the license and SHA-256 of every clip; the ctest entry
`stt-wer-eval` (label `eval`) skips with exit code 77 when the clips or the model are missing.
Nothing here changes the engine: it is a baseline, and `tests/eval/gates.json` fails a build
whose error rate rises above it plus a margin.

| set | license | clips | words | WER (accents kept) | WER (accents folded) | CER |
|---|---|---|---|---|---|---|
| Common Voice es, test split, 14 accent groups plus unlabelled | CC0-1.0 | 136 | ~1,150 | 5.98% | 5.52% | 1.87% |
| OpenSLR 73, Peruvian Spanish, 40 female and 40 male clips | CC-BY-SA-4.0 | 80 | 763 | 5.50% | 3.67% | 1.60% |
| both | | 216 | ~1,900 | 5.80% | 4.83% | 1.77% |

Decode time is 0.25 of real time on this machine (16 threads, debug build). Normalisation
lowercases, drops punctuation and hyphens and keeps digits as the reference wrote them.

What the number does not say: the clips are read sentences (Wikipedia-style text for Common
Voice, short phrases for OpenSLR) recorded close to the microphone, not conversational household
speech through a far-field microphone or a phone call. Common Voice names its own accent group
`Andino-Pacífico: Colombia, Perú, Ecuador, oeste de Bolivia y Venezuela andina`; it scored 14.1% on
85 words, which is too few to separate from the 5.8% mean, so it is reported and not gated. About
2,000 words give a 95% interval of roughly one percentage point either way. OpenSLR 73 is
CC-BY-SA, used here only to measure; no clip or transcript is stored in the repository.
