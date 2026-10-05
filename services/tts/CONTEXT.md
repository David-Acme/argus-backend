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
    headers, one HTTP chunk per engine chunk; synthesis runs on a worker
    of the Heavy blocking lane admitted by `StreamSlots` (at most the Heavy
    lane's thread cap minus one streams; one more is 429 `Busy`) and pushes
    through the async stream. It used to start one detached thread per
    request with no bound. `main.cc` registers the slots as a
    `shutdown_signal` drain: a stop request makes every stream stop at its
    next chunk, and the drain is done when none is running. A client that
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
| `tts.pocket_variant_es` | `fast` (6 layers), `quality` (24 layers), default `quality` | Basic | Live |
| `tts.pocket_voice_es`, `tts.pocket_voice_en` | `jean` (default) and the eight Commons voices of that language | Advanced | Live |
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
wire validates unchanged; in practice every caller sends the default `M3`.
When Pocket answers a language, the owner's configured Pocket voice for that
language answers every id, so the house has one voice per language and the
setting is what the owner hears. The owner chose Kyutai's male voice `jean`
for both languages (2026-10-03, "una voz M de Jean tanto para inglés como para
español"), so `jean` is the default of `tts.pocket_voice_es` and
`tts.pocket_voice_en` and every male id resolves to it. A rule that sent every
`M*` id to `jean` regardless of the setting was rejected: since callers only
send `M3`, it would have made the voice setting unreachable. The gender of the
id matters only when the configured voice is missing from the variant that
answers. Then an `M*` id falls back to `jean` and an `F*` id to the Commons
voice of the language (`lola`, `alba`), then that Commons voice, then any
installed voice, then Supertonic. The service logs each fallback once
(`TtsService::resolvePocketVoice`, pure and tested).

The selectable voices are `jean`, `lola`, `alba`, `eve`, `fantine`,
`giovanni`, `marius`, `javert`, `michael` for Spanish and `jean`, `alba`,
`eve`, `jane`, `mary`, `marius`, `javert`, `michael`, `george` for English.
All of them except `jean` come from CC0 or CC BY recordings. **`jean` is
built from the EARS dataset (speaker p010), licensed CC BY-NC 4.0: it is for
non-commercial use only.** The owner accepted it for internal testing until
he builds his own voice. Provisioning installs it only with the explicit
opt-in described below, and `cosette` and the other non-commercial voices
are not offered. Measured with argus-stt on the preview sentence, `jean`
works with all three models: WER 0.00 on es fast and es quality and 0.17 on en
("I am" for "Hi, I'm"), the same range as the Commons voices.

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
and `reference-audio`. The engine and voice the configuration selects are
loaded right after boot (see "Warm-up after boot"); any other variant loads on
the first request that needs it, and every loaded engine is kept for the life
of the process. Thread counts come
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

Sizes on disk: es fast 125 MB of graphs, es quality 355 MB, en 125 MB; a voice
state is 4-7 MB for the 6-layer models and 15-33 MB for the 24-layer one. The
downloads that build them are the weights (es fast and en 219 MB, es quality
672 MB) plus the one-time toolchain. RAM is spent on a variant once it is
selected (it is warmed after boot) or used. Licence and attribution: `models/tts/pocket/NOTICE`. The runtime code is
original (MIT upstream pocket-tts semantics, no PocketTTS.cpp source copied).

### What gets installed

Provisioning installs what the configuration selects and nothing else. It
reads the `[tts]` config it provisions for: `services/tts/config.toml` from
`setup.sh`, `argus-deploy/config.tts.toml` from `provision-host.sh`, or
`ARGUS_TTS_CONFIG`. For each Pocket language it installs the variant that
language needs (`es-quality` or `es-fast` per `tts.pocket_variant_es` when
`tts.engine_es` is `pocket`, and `en` when `tts.engine_en` is `pocket`). Per
variant it installs the configured voice and the Commons voice of the
language (`lola`, `alba`), so a missing configured voice never leaves the
language mute. `ARGUS_TTS_POCKET_VARIANTS` overrides the variant list,
`ARGUS_TTS_POCKET_QUALITY=0` picks `es-fast`, and `ARGUS_TTS_POCKET=0` skips
Pocket. `--plan` prints the selection without downloading.

Anything else is added later, with the same pins:
`services/tts/scripts/provision.sh --variant es-fast` (a variant and its
selected voices) or `--voice en:george` (one voice). The app can also ask
argus-tts to do it (next section). `--catalog` lists every installable
component with its download size and licence, and every run writes it to
`models/tts/pocket/catalog.txt`, which is how argus-tts knows sizes and
licences without parsing the script.

**Non-commercial voices are opt-in.** `jean` is pinned for all three models
but installed only when `ARGUS_TTS_POCKET_NONCOMMERCIAL_VOICES=1` is set.
`scripts/setup.sh dev` sets it for the owner's local development and prints a
notice; `setup.sh prod` and `provision-host.sh` never set it. When `jean` is on
disk, `NOTICE` gains a section with its source (EARS p010 via
`kyutai/tts-voices`), its licence and how to remove it. A voice's staging is
part of its variant's atomic rename, so a variant never appears without
voices.

## Installing on demand (`feature/provisioning/`, `argus::tts-provisioning`)

The owner sees each option's state in Configuración before choosing it. The
settings registry asks `PocketProvisioning::choiceStates` for the engine,
Spanish variant and voice choices. Each answers with its availability, its
download size from the catalog and the host command that installs it:

| Availability | Meaning |
|---|---|
| `installed` | on disk (for the engine: a variant that `pocketSelection` would use) |
| `installable` | this process can install it; choosing it starts the install |
| `installing` | queued or running |
| `failed` | the last attempt failed; choosing it again retries |
| `hostOnly` | only the operator can install it, with the command shown; the registry refuses the choice as `notInstalled` |

Whether this process may install anything is probed once at boot
(`probeProvisioningHost`). The provisioning script must sit next to the
models it would write: the canonical models dir must be `<root>/models/tts`,
the Pocket dir `<root>/models/tts/pocket`, and the script
`<root>/services/tts/scripts/provision.sh`. That is true for a native run (the
build symlinks `models` next to the binary) and false in a container, where
everything missing is therefore host-only. Voices also need `bash`, `curl` and
a writable models dir; variants also need `uv` or the cached export toolchain.
Non-commercial voices install on demand only when the host config says
`tts.pocket_noncommercial_voices = true`. That key is not in the owner
catalog, `setup.sh dev` writes it with the environment opt-in, and both
templates ship `false`.

When a settings change needs a missing, installable component (the variant
and voice the new configuration selects), `PocketInstaller` queues it. One
worker thread runs `bash provision.sh --variant|--voice <component>` through
`posix_spawn`, never a shell string. Component names must match the catalog's
charset. The child runs in its own process group, gets the service's absolute
config path in `ARGUS_TTS_CONFIG` and none of the inherited `ARGUS_TTS_*`
variables. Its output goes to the service log, and shutdown terminates the
group. The choice is persisted at once; synthesis keeps its fallback (`quality`
→ `fast`, a missing voice → the chain above) and picks up the new files on the
next request, because variants and voices are found on disk per request. The
app polls while a choice says `installing`.

## Warm-up after boot

The first Spanish greeting after a restart used to pay the load of the
24-layer model (ONNX Runtime session creation for 355 MB of graphs, 1.8-4.4 s
in the release build depending on machine load), which is where VOICE's 3.8 s
first greeting came from. `main.cc` now starts one background thread right
before `drogon::app().run()` that calls `TtsService::warmUp()`. Health answers
as soon as Drogon listens; the warm-up never runs on an event loop.

`warmUpPlan()` names, per language (es, en), what the next request would use.
For a Pocket language that is the variant `pocketSelection` picks (so `quality`
without its files is `fast`, and a variant that failed to load is skipped) and
the voice `resolvePocketVoice` gives the id every caller sends (`M3`). A
language Pocket cannot answer (Supertonic configured, no variant installed, or
no voice) warms Supertonic's `M3` style, a small JSON file. Nothing else loads:
no unselected variant and no other voice. `warmUp()` loads each target through
the same `pocketEngine`, `pocketVoice` and `resolveVoice` calls a request
makes, so the caches, `pocketFailures_` and the one-time warnings end up
exactly as the lazy path would leave them. A model that fails to load is logged
once and marked failed, and the next request falls back as it always did. The
lazy path itself is unchanged; the warm-up only runs it earlier. The plan is
tested on a fake model tree, without loading a model.

Each language is warmed under `synthMutex_` and checked against `stopping_`
and `generation_`, like a synthesis. The warm-up only takes the lock when it is
free (it polls `try_lock` every 20 ms) and pauses 200 ms between languages, so
a request that queued behind the Spanish load is served before the English load
starts. It does not compete with a stream that is already speaking either: it
only takes a lock nobody holds, and a stream takes it back between chunks at
once. A request sent the moment health answers therefore gets its first audio
no later than before. Without the pause it lost the lock to the English load
and arrived 0.3 s later than the lazy path. A shutdown during the warm-up waits for the one
model being loaded (about 1.7 s here), then the thread stops without loading
the next.

A settings change to engine, variant or voice is not warmed: the new selection
loads on its first request, as before. Warming it would need a worker that
outlives each change, and it would take the engine lock for a whole load at a
moment no request chose, where the lazy path only loads at the start of a
request. The delay it would save follows a rare owner action, so the trade is
not worth it.

Measured on the release build (`build/prod`, Ryzen 7 5825U, idle) with a
scratch instance of the template config (es Pocket `quality` + `jean`, en
Pocket + `jean`), a short Spanish sentence through `/tts/v1/synthesize-stream`,
3 runs each, before and after interleaved:

| Case | Lazy (before) | Warm-up (after) |
|---|---|---|
| Request 15 s after health: request to first audio | 1.84-1.87 s | 0.067-0.070 s |
| Request at health: process start to first audio | 2.47-2.52 s | 2.44-2.48 s |
| Process start to health | 0.60-0.62 s | 0.60-0.64 s |
| Warm-up of es `quality` + en | none | 2.3 s, 3.5 s when a request goes first |
| Service memory before the first request | 0.45 GB | 1.49 GB |

Earlier, with other builds running, the lazy first request took 4.3-4.7 s to
its first audio. The second request was 0.06-0.07 s in every run, so a loaded
engine and voice is all the first request was missing, and no warm-up
synthesis is needed. The memory is what both languages cost once used (1.33 GB
lazily after one Spanish request); it is now spent at boot and stays under the
compose limit (`ARGUS_TTS_MEMORY_LIMIT`, 2 GB).

## The wire test runs Supertonic

`tts-wire-test` pins `engine_es` and `engine_en` to `supertonic`. It was
written for Supertonic (its config sets `quality = "low"`), and its 30 s bound
is a liveness bound on a low-quality Supertonic synthesis. When Pocket became
the default for both languages, the unpinned test started timing the first
load of the 24-layer Pocket model in the Debug build, which links a Debug ONNX
Runtime: 13.6 s of load plus a Debug synthesis whose length depends on the
random seed made the request 25.9 s on an idle machine and 30.8-31.9 s under
load. Pinned, the request takes 0.5-0.6 s idle and 2.2 s with all 16 hardware
threads busy, and the whole test 5 s instead of 38 s. The test also checks that
`/tts/v1/config` reports Supertonic for both languages, so the pin cannot
silently stop applying. The Pocket path through the service keeps its own
coverage in `tts-pocket-test`.

## Voice previews (`tools/tts-preview/`)

The app plays a bundled clip for every engine, variant and voice option, so
the owner can hear an option before installing it, offline and instantly.
`make-previews.sh` builds them reproducibly. `argus-tts-preview` loads the
shipped `config.toml.example`, sets the engine, variant and voice of each
manifest line as runtime overrides and synthesizes through `TtsService`
with `tts.pocket_seed = 7`, which seeds Pocket's sampling noise (0 keeps the
per-process random seed). The sentences are "Hola, soy Argus. Tu reunión
empieza a las 16:30." and "Hi, I'm Argus. Your meeting starts at 4:30 pm."
The clips are Supertonic es/en with `M3` (the only Supertonic voice callers
use, since no setting chooses another), and Pocket es fast, es quality and en
with each of the nine selectable voices. Each is encoded as Opus 24 kbps in
Ogg (web and desktop, which WebKitGTK decodes with stock GStreamer) and as
AAC 32 kbps in M4A (Android and iOS). They go to the frontend's
`src/assets/audio/tts-previews`: 29 clips of 3.2-7.6 s, 413 KiB Opus and
618 KiB AAC. Rebuild after adding a voice or changing an engine:
`./scripts/build-all.sh dev --only tts`, install every variant and voice
(including `jean` with the opt-in), then run
`services/tts/tools/tts-preview/make-previews.sh`.

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
6-layer one, with the same WER on these sentences. The owner listened and
chose `quality` ("utilizar calidad máxima", 2026-10-03), so it is the default;
`fast` stays one tap away.

## Pinned Supertonic downloads (2026-10-05 audit #30)

`scripts/provision.sh` fetches the Supertonic files from a pinned
revision (`SUPERTONIC_REVISION`) with a SHA-256 per file
(`SUPERTONIC_SHA256`), into a `.part` file that is moved into place only
after its hash matches, the way the Pocket files already were. The pins
were filled on 2026-10-05 (revision `3cadd1ee`, hashes from the Hugging Face
LFS metadata and, for the JSON files, from the files of that revision) and
match the files installed on the development host. A pin left empty makes
the script refuse to download that file and say so, and
`ARGUS_ALLOW_UNPINNED_MODELS=1` fetches from `main` and prints each file's
hash to pin. Present files are kept. The stt, vlm and voice provisioning
scripts follow the same rule.
