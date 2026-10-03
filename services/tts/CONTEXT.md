# argus-tts — CONTEXT

## Why the tts service exists

F4-2 of the `migracion-microservicios` plan extracts the TTS engine out of
the legacy monolith (Rulings BG-BJ). `argus-tts` is a sibling service with
its own binary, own CMake preset and zero HTTP exposure: it exists so the
legacy's voice session and camera talk stop linking the engine in-process
and instead call it over the internal wire.

## What it owns

- **The onnxruntime TTS engines** — Supertonic 3 and Kyutai Pocket TTS, chosen
  per language (see "Two engines, chosen per language" below). Supertonic
  (shared-tree `TtsService`, compiled into
  this binary via the F2-1 PORTED pattern), loaded at boot from the shared
  `models/tts` tree (`tts.models_dir`; the build symlinks `../models`
  next to the binary). Boot aborts if the engine fails to load — the
  service is useless without its capacity. The capability tier is derived
  without a Vulkan probe (no ncnn here), so `tts.steps_cap` pins the
  legacy denoising-steps ceiling on GPU hosts.
- **The internal wire (Ruling BH)**:
  - `POST /tts/v1/synthesize` → binary float32 PCM,
    `audio/x-argus-pcm-f32` + `X-Argus-Sample-Rate`.
  - `POST /tts/v1/synthesize-stream` → chunked float32 PCM with the same
    headers, one HTTP chunk per engine chunk; synthesis runs on a detached
    producer thread that pushes through the async stream. A client that
    disconnects stops the synthesis at the next chunk boundary
    (`TtsStreamInput::stopRequested`), so the shared engine is not held for
    text nobody will hear.
  - `GET /tts/v1/config` → `{sampleRate, defaultSpeed, loaded}` so the
    legacy adapters can honor the engine's configured speed without a local
    engine (additive beyond Ruling BH's two endpoints — needed by the
    camera-control adapter that wraps defaultSpeed + sampleRate +
    synthesize).
  - Request: `{text, style_id?, lang?, speed?}` validated through the
    shared DSL; errors use the frozen `{status, info, errors}` envelope
    (422 validation, 503 `TTS_NOT_LOADED`).
  - Trust model: no auth, loopback bind by default — internal-network only,
    never announced or published.
- **Config**: `[tts]` (engine knobs, mirroring the legacy block), `[server]`
  (loopback listener, default 7029), the `[rpc]` / `[rpc.callers]` gRPC gate
  and `[drogon.app]` — no database, no NATS, no JWT/device keys.

## Synthesis cache

Repeated short lines are served from a bounded in-memory cache (64 entries,
text up to 300 chars) keyed by text/voice/lang/steps/speed. The first call
pays the engine (~3.3 s per greeting on CPU); every repeat is a memory copy
(sub-millisecond), which is what makes the guard's fixed greetings and
announcements feel immediate.

## What it did NOT change

- The mobile app never talks to this service; `/camera/{id}/talk` keeps
  legacy ownership (Ruling BC) and the internal wire stays loopback-only,
  unannounced.
- Model artifacts stay in the shared `models/tts` paths — never copied.
- The legacy `TtsService` stays linked in the legacy binary: only the boot
  init is gated behind `tts.remote_url` (Ruling BI), so the symbol proof
  for the legacy is unchanged by this task.

## Compose volume

The docker compose must mount the shared `models/` tree (at least
`models/tts`) into this service's working directory — the engine reads
`models/tts/onnx` and `models/tts/voice_styles` relative to `tts.models_dir`.

## Phase 4 step 9: config resolution into `src/config/` (D20)

The listener and the optional gRPC leg are resolved by
`src/config/tts-config.{hxx,cc}` (`argus::tts-config`):
`TtsConfig::resolveListener()` (`ListenerConfig::resolve(7029)`) and
`TtsConfig::resolveRpc()` (`rpc.address` plus the `rpc.callers` credential
pairs, empty ones dropped). `main.cc` keeps `config.toml` loading,
`drogonConfig` and the boot gate on the resolved address and credentials.

## Owner settings

`src/feature/settings/tts-settings.cc` is the catalog an owner may change
through `argus.settings.v1.Settings` (registered on the same gRPC listener
as synthesis, `argus::contracts::settings-wire`). Speed and quality are
basic; diffusion steps, the step cap and the chunk length are advanced; all
of those apply live, because `main.cc` calls `TtsService::refreshDefaults()`
when the registry reports a change, the step keys are already read per
synthesis, and `Capabilities` reports the default speed through a function
instead of the boot snapshot (voice and camera ask for it per call). The
edge/join silences and the thread count are read when the engine is built
and say "restart". The `settings` entry of `[rpc.callers]` is the only
credential the settings service accepts, and it is removed from the
synthesis callers: the settings caller cannot synthesize and a synthesis
caller cannot change settings.

## Voice style ids

A style id names a file under `voice_styles/`, so it may only hold letters,
digits, `_` and `-` (16 at most): the DTO refuses anything else with a 422
and `TtsService::resolveVoice` refuses it on every path, gRPC included. A
value such as `../../x` used to read any reachable `.json` file as a voice
style.

## Two engines, chosen per language

Supertonic 3 is slow to start speaking (about 1 s of silence for a short
sentence and up to 4 s for a paragraph, because it synthesizes a whole chunk
before the first sample leaves) but runs anywhere and its Spanish is solid.
Kyutai Pocket TTS is a streaming autoregressive model: the first audio leaves
in about 30 ms and it runs roughly ten times faster than real time on CPU. The
owner's own listening put Pocket ahead in English and behind in Spanish, so
the choice is per language rather than global:

| Key | Choices | Level | Applies |
|---|---|---|---|
| `tts.engine_es`, `tts.engine_en` | `pocket`, `supertonic` (default `pocket`) | Basic | Live |
| `tts.pocket_variant_es` | `fast` (6 layers), `quality` (24 layers), default `fast` | Basic | Live |
| `tts.pocket_voice_es`, `tts.pocket_voice_en` | the predefined voices installed for that language | Advanced | Live |
| `tts.pocket_reference_es`, `tts.pocket_reference_en` | a `.wav` name inside `models/tts/pocket/references/`, empty for the predefined voice | Advanced | Live |
| `tts.pocket_temperature` (0.3), `tts.pocket_lsd_steps` (1) | sampling knobs | Advanced | Live |
| `tts.normalize_text` | toggle, default `true` | Advanced | Live |

Every key is read on the next synthesis, so a change needs no restart. Any
other language always uses Supertonic, and so does a Pocket language whose
models are not installed or fail to load: the service logs once and answers
with Supertonic, so a missing download degrades quality instead of breaking
speech. A `quality` choice without the 24-layer files installed falls back to
`fast`.

**Voices.** Callers keep sending Supertonic voice ids (`M1`..`F5`), which the
wire validates unchanged. When Pocket answers a language, it uses the owner's
configured Pocket voice for that language and ignores the request id, so the
house has one voice per language instead of a random mapping. The installed
voices are `lola` (the Spanish default, recorded in Spanish), `alba`, `eve`,
`fantine`, `giovanni`, `marius`, `javert`, `michael` for Spanish and `alba` (the
English default), `eve`, `jane`, `mary`, `marius`, `javert`, `michael`,
`george` for English. All of them come from CC0 or CC BY recordings. Kyutai's
`jean`, `cosette` and other voices built from non-commercial datasets (EARS,
Expresso) are deliberately not installed.

**Wire shape unchanged.** Pocket produces 24 kHz audio. It is converted to the
rate `Capabilities` already announces (Supertonic's 44.1 kHz) with
`packages/lib/audio`'s `AudioResampler`, as the block-processed-audio rule
requires. That resampler works on int16, so samples pass through 16-bit on
the way, about 96 dB of headroom, which is below the model's own noise floor.
`GET /tts/v1/config` additionally reports `engines: {es, en}`, the engine that
would answer each language right now, which accounts for missing models.

## The Pocket runtime (`infra/pocket/`)

This is a C++ port of the upstream generation loop, built on the ONNX Runtime
the service already links. It neither vendors nor links PocketTTS.cpp:

- PocketTTS.cpp's exporter only works with the January 2026 English model
  (pocket-tts 1.x/2.0 API, SentencePiece, WAV voice cloning). It cannot
  export the multilingual models (pocket-tts 3.x, `tokenizer.json`, predefined
  voice states). The ungated multilingual weights also carry no usable
  cloning encoder, and the runtime is built around cloning from a WAV.
- What carries over is its design, which the port reproduces: explicit-state
  graphs, KV caches updated in place through `Ort::IoBinding` (no per-step
  copy of the 48 MB cache), and latent generation on one thread with Mimi
  decoding on another.

Pieces: `pocket-bundle` (manifest), `unigram-tokenizer` (HF `tokenizer.json`:
Unigram Viterbi, Metaspace and byte fallback, checked against the Python
`tokenizers` ids in the test), `pocket-voice` (safetensors voice states),
`pocket-prompt` (upstream `prepare_text_prompt`), `pocket-engine` (the
loop: text prompt, autoregressive backbone, LSD flow step, EOS plus frames
after it, pipelined Mimi decoding with the 5 ms fade-in), `pcm-rate-converter`
and `reference-audio`. An engine per variant is loaded lazily on the first
request that needs it and kept for the life of the process. Thread counts come
from `ThreadBudget::ttsThreads()` (or `tts.threads`): the backbone gets half
and the decoder half, and the two tiny graphs run on one thread.
Measurements showed ORT spinning threads and the multi-threaded flow step cost
more than they gave. Every engine call runs under the service's
`synthMutex_`, the same mutex pattern as Supertonic. The per-request stop
callback is checked before each backbone step and each decode, so a
disconnected or cancelled caller frees the engine within one frame (about 10 ms).

**Voice cloning** (`tts.pocket_reference_*`) needs a bundle exported with
`--cloning` from Kyutai's gated cloning weights (`kyutai/pocket-tts`, after
accepting its terms). The public, ungated weights that provisioning installs
ship an untrained speaker projection: cloning with them yields unintelligible
speech (measured: WER 1.0), so provisioning does not export the encoder, and
the service ignores a reference with a one-time warning. With a cloning bundle
the reference `.wav` (16-bit PCM or float, any rate, at most 10 s used,
trailing silence trimmed to an 80 ms pause as upstream does) is encoded once
and the resulting voice state is cached by file name, size and mtime. The name
must be a plain `*.wav` file directly inside `models/tts/pocket/references/`.
It is canonicalised and refused if it resolves anywhere else, so the setting
cannot read an arbitrary path. The clone path is tested mechanically
(`ARGUS_TEST_POCKET_CLONING_DIR`). Its audio quality could not be judged
without the gated weights.

## Text normalization and prosodic chunking (`text/`, `argus::tts-speech-text`)

Before either engine sees the text, `normalizeSpeechText` writes out what a
TTS model mispronounces (Spanish and English, other languages untouched):
cardinals with each language's separators and the gender of the noun that
follows (`200 personas` → doscientas, `21 cámaras` → veintiuna), decimals,
negatives, percentages, currencies (€, $, £, US$, EUR, USD, MXN, GBP, with
cents), units (km, km/h, kg, °C, h, GB, …), ordinals (`1.º`, `3.ª`, `1.er`,
`1st`), dates (`dd/mm/yyyy`, ISO), clock times, the common abbreviations
(Sr., Sra., Dr., Dra., Ud., etc., aprox., Mr., Dr., e.g., …), `#` and
`&`/`@`/`+`/`=`, and markdown emphasis. `¿?¡!` are kept for prosody. Times
follow one convention: `14:30` → "las dos y media de la tarde" (12-hour clock
plus the part of the day when the hour is unambiguous in 24-hour form, :15/:30
as "y cuarto/y media"), and `2:30 pm` → "two thirty in the afternoon".

Measured on the owner's sentence ("El Sr. Pérez llega a las 14:30 con el 25 %
del pedido, unos 3,5 km después del 1.º de octubre"), with the service's own
ASR model (NeMo transducer) as the judge: WER without/with normalization is
0.62/0.07 for Pocket fast, 0.59/0.07 for Pocket quality and 0.45/0.10 for
Supertonic. Without it, Pocket reads "14:30" and "25 %" as noise.

`chunkProsodic` replaces the old regex sentence splitter for both engines. A
paragraph splits into sentences (abbreviation- and decimal-aware), and only a
sentence over the budget splits further: at `;`/`:`, then commas, then dashes,
then words, and never mid-word unless a single word exceeds the budget.
Neighbouring pieces are re-merged up to the budget, so pauses fall where a
reader would breathe. The budget is `tts.max_chunk_len` characters for
Supertonic, and for Pocket it is 50 tokens measured by its own tokenizer
(upstream's `max_token_per_chunk`). It is a pure module with its own test and
belongs to this service, not `packages/lib/text`, because nothing else speaks
text aloud: rule 23's 2+ rule has not been earned, and rule 24 forbids
structure ahead of its consumer.

## Pocket models: where they come from

Provisioning (`scripts/provision.sh`, run by `scripts/setup.sh` and
`provision-host.sh --with-models`) builds each variant from Kyutai's
**official** public weights (`kyutai/pocket-tts-without-voice-cloning`, CC BY
4.0) instead of downloading a third party's ONNX:

1. It downloads `model.safetensors` and `tokenizer.json` at pinned revisions
   with SHA-256 pins, into a `.part` file that is moved into place only after
   its hash matches.
2. It runs `tools/export-pocket.py` in a cached uv toolchain with every version
   pinned (pocket-tts `41cbc84`, torch 2.14.1 CPU, onnx 1.23.1, onnxruntime
   1.30.0). The exporter makes the streaming state explicit, exports the four
   graphs, **checks each fp32 graph against the official PyTorch model**
   (aborting above 1e-3, measured at ~1e-6), and quantizes the three big graphs
   to int8 per channel.
3. It compares every produced file with the SHA-256 recorded in the script.
   The export is deterministic, and a fresh toolchain reproduced the pins byte
   for byte. A mismatch is only a warning, because the parity check already
   gated the graphs, and different hardware may legitimately differ in the
   last bit.
4. It stages the export in `pocket/.<variant>.part`, renames it atomically,
   and downloads the predefined voices with SHA-256 pins.

The cost is a one-time ~1 GB toolchain in `~/.cache/argus/pocket-export` and
about a minute per variant. Rejected alternatives: the community INT8 bundles
(`Alias-Alias/pocket-3-3-0-tts-onnx`) carry a September Spanish model that
predates the 1 October retrain, only 8 voices (two of them non-commercial),
and full-length caches that cost 2x per step. PocketTTS.cpp's exporter cannot
produce the multilingual models (see above). Committing ~500 MB of graphs is
excluded by the rule that weights never enter git.

Sizes on disk: es fast 125 MB of graphs plus 46 MB of voices, es quality 355 MB
plus 189 MB, en 125 MB plus 50 MB. The 24-layer model is downloaded too
(`ARGUS_TTS_POCKET_QUALITY=0` skips it, `ARGUS_TTS_POCKET=0` skips Pocket),
so the Basic `quality` choice works on every installation that can afford the
disk. RAM is only spent on a variant once it is used. Licence and attribution:
`models/tts/pocket/NOTICE`. The runtime code is original (MIT upstream
pocket-tts semantics, no PocketTTS.cpp source copied).

## Measurements (Ryzen 7 5825U, 8 cores / 16 threads, `ThreadBudget::ttsThreads()` = 8)

Measured through the internal HTTP stream (`/tts/v1/synthesize-stream`) of a
Release `argus-tts`, the median of 3 runs. First audio is the time to the first
PCM byte; RTF is total time divided by audio seconds (lower is faster). The
short sentences have 15-16 words, the paragraphs 66-67.

| Engine | Lang | First audio short / long | RTF short / long |
|---|---|---|---|
| Supertonic 3 (M1) | en | 1028 / 3613 ms | 0.230 / 0.215 |
| Supertonic 3 (M1) | es | 1172 / 3993 ms | 0.235 / 0.210 |
| Pocket fast (alba) | en | 26 / 32 ms | 0.093 / 0.102 |
| Pocket fast (lola) | es | 30 / 35 ms | 0.097 / 0.103 |
| Pocket quality, 24 layers (lola) | es | 78 / 113 ms | 0.297 / 0.320 |

Reference points: upstream PyTorch on the same machine reaches RTF 0.33 (es 6
layers), 0.83 (es 24 layers) and 0.31 (en). The standalone PocketTTS.cpp
binary (old January English model, int8) reaches RTF 0.085-0.091 with 22-28 ms
to first audio, about the same as this port.

Resident memory of the service: 592 MB with Supertonic alone, 994 MB with
Supertonic plus Pocket es fast and en loaded, 1.40 GB with Supertonic plus the
24-layer Spanish model. ASR word error rate on the test sentences: Pocket es
0.00 / 0.05, Pocket en 0.13 / 0.18, Supertonic es 0.00 / 0.00, Supertonic en
0.13 / 0.06 (short / long). The 24-layer Spanish model is 3x slower than the
6-layer one, with the same WER on these sentences, so `fast` is the default.
Whether `quality` sounds better is for the owner's ears.
