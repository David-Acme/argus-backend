# CONTEXT.md — Argus Backend project memory

> This file preserves the project's intent, decisions and state so context is
> never lost between sessions. Update it whenever a significant decision is made.

## What Argus is

- **Argus** is a 100% local AI platform for the home. Security ("intelligent
  guard") is the entry point; a local virtual assistant is a secondary capability.
- Philosophy: process everything locally, minimize resources, preserve privacy.
  Cloud only transports packets (tunnel), never processes data.
- AI is invoked **only when it adds value**: events → rule engine → if resolvable,
  execute; else → LLM. This keeps latency/cost low.
- Central abstraction: **Entity** (live object with state), not raw detections.
  The whole backend works with `Entity`.

## Hard constraints / decisions

- **C++20**, Drogon HTTP/WebSocket server. SQLite via Drogon async `DbClient`
  (NO ORM, manual SQL). DB file `database/argus.db`, `number_of_connections: 4`
  (pragmas reapplied at every boot via `DbService::applyPragmas()`).
- **Config**: single `config.toml` file (TOML, parsed via `tomlplusplus`).
  Replaces `.env` + `config.json`. Drogon section converted to JSON at runtime
  and loaded via `loadConfigJson()`. `ConfigService` in `src/shared/services/config-service/`.
- **NO spdlog** — Drogon already provides logging. Do not add spdlog.
- **Smart pointers only** — no raw owning pointers. All service resources use
  `std::unique_ptr` with custom deleters.
- **Services use hardcoded model paths** — all services know their model paths
  internally (no parameters). Paths live at `models/{llm,stt,vision,tts,face}/`.
- **Conan 2** for deps (`conanfile.txt` + `CMakePresets.json`). CMake presets:
  `dev` (Debug) and `prod` (Release), generator Ninja.
- **Git submodules** under `third_party/` for libs that change rarely and we want
  to control: `ncnn`, `sherpa-onnx`, `llama.cpp`, `inspireface`, `fastText`
  (pinned at 1f12150 = v0.9.2 + local C++20 patch). Built via
  `add_subdirectory` with `EXCLUDE_FROM_ALL`. `fastText` was removed on
  2026-08-09 (zero uses in `src/`) and **re-added the same day** as the intent
  engine backend; `hnswlib` was replaced by sqlite-vec for face embeddings.
- **sqlite-vec** is vendored (single-file C extension, v0.1.10-alpha.4, MIT/
  Apache-2.0) at `third_party/sqlite-vec/` with sqlite3 3.53.3 headers. It is
  compiled with `SQLITE_CORE` and registered via
  `sqlite3_auto_extension(sqlite3_vec_init)` in `DbService::installExtensions()`.
  **Ordering constraint**: the registration must run AFTER Drogon's first
  connection (its `std::call_once` calls `sqlite3_config(SQLITE_CONFIG_MULTITHREAD)`,
  which returns SQLITE_MISUSE once sqlite3 is initialized — the app registers in
  the beginning advice after `migrate()`; `VecDb` opens its own connection lazily
  so vec0 only needs to exist from then on). FTS5 is compiled into the conan
  sqlite3 (`sqlite3/*:enable_fts5=True` — Drogon was rebuilt once against it).
- Convention: Conventional Commits in English. Remote `git@github.com:David-Acme/argus-backend.git`,
  branch `main`.

## Dependency resolution notes (Conan conflicts, learned the hard way)

- `nlohmann_json` pinned to **3.11.3** — jwt-cpp/Drogon use this version.
- `opencv/4.13.0` built **headless**: `with_protobuf=False`, `with_eigen=False`,
  `with_ffmpeg=False`, `with_wayland=False`, `with_gtk=False`, `with_vulkan=False`.
- `eigen/5.0.1` **removed** (2026-08-06): it was declared in `conanfile.txt` and
  linked in `CMakeLists.txt` but had zero uses in `src/` or `labs/`. The tracker
  uses `cv::KalmanFilter` instead, so no dependency comes back.
- **conanfile.txt cannot resolve version conflicts** (no `override=True`/`force`).
  Conflict resolution is done by pinning versions + disabling the offending option
  in the consuming package.

## Current build state

- Build is **green**: `cmake --preset dev` + `cmake --build --preset dev -j 8`
  succeeds; server starts on `0.0.0.0:7024`.
- **Build script**: `./scripts/setup.sh` handles Conan deps + cmake configure + build.

## Current services (all implemented)

| Service | Engine | Model | Path |
|---------|--------|-------|------|
| `FaceService` | ncnn (Vulkan) | RetinaFace + MobileFaceNet | `models/face/` |
| `FaceDB` | vec0 (sqlite-vec) | 128-dim cosine KNN, persisted | SQLite (`face_vec`) |
| `LlmService` | llama.cpp (submodule b10305) | LFM2.5-1.2B-Instruct QAD Q4_0 | `models/llm/` |
| `VisionService` | llama.cpp + libmtmd | LFM2.5-VL-450M (Q8_0 + mmproj F16) | `models/vision/lfm2vl-25/` |
| `SttService` | sherpa-onnx | nemo_transducer (FastConformer RNN-T, es/en) | `models/stt/` |
| `TtsService` | Supertonic 3 | ONNX models | `models/tts/` |
| `JwtService` | jwt-cpp | HS256, instance class | — |
| `ConfigService` | tomlplusplus | TOML config reader + runtime writes | `config.toml` |
| `DbService` | Drogon DbClient | SQLite async client | `database/argus.db` |
| `VadService` | Silero VAD v5 (ONNX, instance class) | turn-taking con gate de calidad | `models/vad/` |
| `EmbeddingService` | ONNX Runtime | multilingual-e5-small INT8 (lazy load) | `models/memory/` |
| `IntentService` | fastText (submodule) | argus-intent.ftz (~20 µs predict) | `models/intent/` |
| `MemoryService` | SQLite graph (FTS5 + vec0) | grafo semántico + recall multi-tier | `database/argus.db` |
| `ExtractionService` | llama.cpp | NuExtract-1.5-tiny Q4_K_M (off-turn) | `[extract] model_path` |
| `ReactionEngine` | señales puras (sin modelo) | 10 reacciones priorizadas → `voice:event` | — |
| `StreamHub` / `MediaRelay` / `Go2rtcManager` | go2rtc | fMP4 sobre `/sync` con credit window | — |
| `S3StorageService` / `PrivatePortraitService` | RustFS (S3) | objetos privados + capabilities one-use | loopback :9000 |

## Performance & portability batch (2026-08)

The same binary must perform well on ANY machine (2 cores → 64 cores, with or
without GPU), so every AI service auto-tunes its resources at runtime:

- **`ThreadBudget`** (`src/shared/wrapper/thread-budget/`) — single source of
  truth for adaptive thread counts: `computeThreads` (half hw, 2-16),
  `batchThreads` (half hw, 4-16), `heavyThreads` (3/4 hw, 2-12),
  `lightThreads` (quarter hw, 2-8), `inferenceSlots` (hw/8, 1-4). Applied to:
  ncnn face nets, llama.cpp LLM/Vision (decode vs prefill split), ORT
  intra-op (TTS), sherpa-onnx (STT). NO hardcoded thread counts anywhere.
- **LLM**: `n_threads=lightThreads` + `n_threads_batch=batchThreads` (measured
  +54% prefill on long prompts), warmup decode at init, `use_mmap=true`,
  generation batch reused (no alloc per token), static mutex around context.
  **All tunables live in `config.toml` `[llm]`** (`context_size`, `max_tokens`,
  `temperature`, `threads`/`batch_threads` with 0 = auto ThreadBudget,
  `n_batch`/`n_ubatch`) so a stronger host can be tuned without code changes.
  `context_size` defaulted 128000 → 32768 (Ollama parity): -300MB KV cache
  RAM, faster decode as context grows; fine for short security-analysis turns.
  `ChatRequest.maxTokens`/`temperature` use 0/-1 sentinels → configured
  defaults. System prompt is English (keeps precision) and asks for 2-3 line
  concise answers.
- **Vision**: superceded — see the "Vision: LFM2.5-VL-450M over llama.cpp +
  libmtmd (2026-08-08)" section below. The SmolVLM2 ONNX pipeline it replaces
  ran 3 ORT sessions (SigLIP encoder base patch-16/512, Llama3 decoder with
  fp32 KV, GPT-2 BPE) with a pre-tokenized fixed prompt; `VisionRequest::prompt`
  was dead in that design and is real now.
- **STT/TTS**: adaptive intra-op threads, static mutexes (recognizer, engine,
  voice cache) — thread-safe for concurrent requests.
- **Face**: `identifyMutex_` (global serialization) replaced by a
  `std::counting_semaphore` with `inferenceSlots()` permits — concurrent
  logins scale with cores. Decode is adaptive: `stbi_info` probe → images
  >2048px decode scaled via OpenCV `IMREAD_REDUCED_COLOR_2/4` (libjpeg IDCT
  scaling), small images keep stb full decode. Login buffer moved (no copies).
- **Coroutines**: `BlockingTask` gained a `void` specialization; every AI
  service exposes `*Async` coroutine variants (`chatAsync`, `chatStreamAsync`,
  `describeAsync`, `transcribeAsync`, `synthesizeAsync`,
  `synthesizeStreamAsync`) that wrap the sync method off the event loop and
  marshal streaming callbacks into the loop via `queueInLoop`.
- **Startup**: services initialize IN PARALLEL (plain `std::thread` + `join`,
  no futures) — full stack loads in ~1s on the reference machine.
- **SQLite**: `applyPragmas()` runs at EVERY boot (WAL, synchronous=NORMAL,
  busy_timeout, mmap 256MB, foreign_keys, temp_store) — previously pragmas
  only applied on fresh DBs and regressed after each restart; connections 1→4.
- **HTTP**: `client_max_memory_body_size` 64K→16M (no multipart spooling to
  disk for images). Release build: `-march=native` + `-flto=auto` on the app
  target only; `-Wall -Wextra` always on, third-party includes SYSTEM.
- **llama.cpp as a submodule**: superceded — see the "Vision:
  LFM2.5-VL-450M over llama.cpp + libmtmd (2026-08-08)" section. The Conan
  recipe `llama-cpp/b6565` was dropped because its `lfm2` projector still
  requires `mm.input_norm.*`; the submodule is pinned to tag `b10305`.
- **ncnn Vulkan**: kept ON — the reference machine has an AMD iGPU (RADV
  RENOIR) that ncnn uses via Vulkan; machines without GPU fall back to CPU.
- **Voice test** (`labs/voice-test/`): standalone `argus-voice-test` binary that
  chains STT → LLM → TTS for a spoken conversation (EN/ES) to measure quality
  and latency end-to-end. PortAudio for mic (16 kHz, software resample
  fallback for devices without 16 kHz) and speaker (plays TTS PCM at its
  native 44.1 kHz so pitch stays natural). Turn-taking uses **Silero VAD v5**
  (ONNX, `models/vad/silero_vad.onnx`, ~2.3MB, ~0.1ms/chunk) through
  `VadService` (`src/shared/services/vad/`): per-stream LSTM state, shared
  ONNX session, block-size independent (context = the 64 samples adjacent to
  the window), zero per-window allocations, and a turn-quality gate
  (`min_turn_ms` + `min_mean_prob`). Current tunables live in `config.toml`
  `[vad]` (threshold 0.45 / neg 0.25, 5 speech frames to open, 12 silence
  frames to close, 240ms min turn, 0.35 min mean prob — the gate was relaxed
  on 2026-08-09 after listening to real camera audio); the calibration run
  against recorded camera audio (Task 11) will replace these defaults with
  measured numbers. The camera path speaks through the Tapo talk channel with
  one persistent session per conversation (`sendChunk`), resamples the mic
  with `AudioResampler` (stateful sinc), and gates the echo by discarding
  `talk_drain_margin_ms` of mic audio after the talk (the echo during the talk
  is dropped because the conversation loop is blocked sending) plus a VAD
  reset. The LLM replies fully first, then the TTS speaks with its native
  chunking (natural, not choppy). Ctrl+C exits cleanly (shared
  `std::atomic<bool>`). `SttService::setLanguage("es"/"en")` switches the STT
  language at runtime; the `[stt] language` config defaults it. Run:
  `build/prod/labs/voice-test/argus-voice-test --camera --cloud-pass ...`.

## Tapo camera integration — Phase 1 (2026-08-06)

Local control and audio-out protocols for the TP-Link Tapo C225, standalone and
validatable before any media or pipeline work exists. Media, detection and
tracking evolved through the sections below (streaming, memory, tools).

- **`src/shared/services/tapo/`** — the whole camera protocol surface:

  | File | Role |
  |------|------|
  | `tapo-crypto` | MD5/SHA1/SHA256 (upper hex), AES-128-CBC + PKCS7, base64, hex, CSPRNG nonces, HTTP Digest MD5/SHA-256 builder + challenge parser |
  | `tapo-http` | Blocking HTTP/1.1 client over a RAII `TapoConnection` (BSD socket + optional OpenSSL TLS, verify-off, `SECLEVEL=0` for the camera's self-signed cert). Non-blocking connect + `poll()` timeouts, chunked and Content-Length bodies. The talk channel reuses `TapoConnection` directly for its long-lived multipart upload |
  | `tapo-transport.hxx` | `ITapoTransport` + `TapoResult` + `TapoCredentials`; `TapoTransportKind`/`TapoHashAlgorithm` enums with `toString`/`fromString` |
  | `secure-passthrough-transport` | **Primary**: one login POST returns `-40413` with `nonce`+`device_confirm`; hash algorithm detected by replaying `device_confirm` with SHA256 then MD5; `digest_passwd` login → `stok` + `start_seq`; AES keys `lsk`/`ivb`; per-request `Seq` + `Tapo_tag`; re-login on `-40401`/`-1` |
  | `legacy-stok-transport` | Fallback for old firmware: `{"hashed":true,"password":MD5(pass).upper()}` login, plaintext requests to `/stok=<stok>/ds` |
  | `tapo-client` | Owns transport selection (`auto` probes securePassthrough then legacy), credential candidates, and a `std::mutex` that serializes every request — the camera rejects parallel control requests. `batch()` always wraps in `multipleRequest` |
  | `tapo-api` | Typed methods with parameter structs (`TapoMoveInput`, `TapoAlarmInput`, `TapoEventFilter`, …). `getStatus()` collapses 8 reads into one `multipleRequest` and derives the camera clock offset |
  | `tapo-audio` | Linear resample, mono downmix, G.711 A-law encoder, 16-bit WAV reader |
  | `tapo-ts-muxer` | Audio-only MPEG-TS muxer: PAT/PMT with MPEG-2 CRC32, PES with PTS, PCR in the adaptation field, correct stuffing |
  | `tapo-talk-client` | Port 8800 talk channel: Digest auth with password-variant probing, `Key-Exchange` nonce capture, `talk` session in `aec` mode, multipart `audio/mp2t` parts, **cancellation checked per packet** |

- **`CancellationToken`** (`src/shared/wrapper/cancellation/`) — shared
  `atomic<bool>` behind copy semantics. Phase 1 uses it in the talk send loop;
  Phase 6 extends it to LLM/TTS/VLM/STT.
- **`labs/tapo-probe/`** — `argus-tapo-probe`, standalone binary in the style of
  `labs/voice-test/`. Flags: `--info`, `--creds`, `--batch`, `--presets`, `--audio`,
  `--ptz x,y`, `--ptz-step`, `--day-night`, `--events [hours]`, `--raw '<json>'`,
  `--talk "text"` (synthesizes with `TtsService`), `--talk-file <wav>`,
  `--talk-framing`, `--ts-dump <file>`, `--verbose`.
- **`[tapo]` in `config.toml`** — ports, timeouts, `login_attempts`, transport
  preference, and the backend talk-channel knobs (`talk_framing`, `talk_mode`).
  Lab-only packet tuning lives in `labs/config.toml`.

### Verified offline (no camera needed)

`--ts-dump` writes 1 s of 440 Hz A-law as MPEG-TS. `ffprobe` parses PAT/PMT/PES
and reports `pid=100`; extracting the ES yields exactly 8000 bytes. Decoding it
back measures peak −8.70 dB (= 12000/32768), RMS = peak − 3.01 dB (pure sine)
and 879 zero-crossings/s (= 2 × 440 Hz).

**Stream type — the value flipped twice, read this before changing it again.**
Muxing the same audio with ffmpeg and reading back its PMT gives `stream_type
0x06` with `stream_id 0xBD` (private stream), and that was the first choice
because ffmpeg's output is what pytapo feeds the camera. `TapoTsConfig` now
carries `streamType 0x90` / `streamId 0xC0` instead. **Confirmed working on
2026-08-09**: the talk channel plays audibly end-to-end with these values (a
wrong stream type fails as silence, and the conversation was audible), so
`0x90` / `0xC0` is the value the C225 accepts. Do not flip it again without a
measurement.

### Still unverified — needs the physical C225 (Phase 1 gate)

Resolved on 2026-08-09 via the live conversation sessions: the talk channel
accepts the **sha256** digest password variant (log: "authenticated with
password variant 'sha256'"), the 8800 multipart body works with
`talk_framing = "none"`, and the TS stream type is `0x90`/`0xC0` (see above).
Still unverified: which credential the control channel accepts (camera
account vs `admin` + cloud password) and the exact `searchDetectionList`
response shape. Each has a probe flag and is persisted rather than hardcoded,
so one `tapo-probe` run against the camera settles them.

**Key derivation — the subtle part**: `hashedKey` is NOT `hashedPassword`. It is
`hashedKey = sha256(cnonce + hashedPassword + nonce)` — the same value that
validates `device_confirm` — and only then
`lsk = sha256("lsk" + cnonce + nonce + hashedKey)[:16]` (likewise `ivb`). This
follows `pytapo/transport/pytapo/pytapo.py`, the reference implementation. An
earlier draft of the plan collapsed the two into
`sha256("lsk" + cnonce + nonce + hashedPassword)`, which authenticates fine and
then fails to decrypt every response. Worth keeping documented: the failure mode
looks like a working handshake followed by garbage payloads.

## Vision: LFM2.5-VL-450M over llama.cpp + libmtmd (2026-08-08)

`VisionService` no longer runs SmolVLM2 through ONNX Runtime. It runs
**LiquidAI LFM2.5-VL-450M** (GGUF Q8_0 + mmproj F16) through llama.cpp's
`libmtmd`, which also forced `llama.cpp` out of Conan and into
`third_party/llama.cpp` (tag `b10305`).

**Why the move happened**: `llama-cpp/b6565` is the newest recipe on Conan
Center, and its `lfm2` projector still requires `mm.input_norm.*`, a tensor
LFM2.5-VL dropped — `mtmd_init_from_file()` rejected the mmproj outright. The
ONNX route was rejected too: `LiquidAI/LFM2.5-VL-450M-ONNX` exists, but the
decoder carries heterogeneous state (10 `past_conv.N` + 12 `past_key_values`)
and the preprocessing is `Lfm2VlImageProcessorFast` — dynamic tiling,
`min_tiles=2`, `max_tiles=10`, thumbnail, row/col info. Reimplementing that
tiler by hand risks silent caption degradation; mtmd already ships it tested.

**Measured against the old pipeline** (Release, same machine, 48 tokens):

| | SmolVLM2 ONNX | LFM2.5-VL |
|---|---|---|
| init | 845 ms | **236 ms** |
| 1280×720 caption | 946 ms | 1 088 ms (at `max_input_px=384`) |
| repeat (cache hit) | n/a | **4 ms** |
| generation | ~46 tok/s | **78-91 tok/s** |
| peak RSS | ~1.0 GB | **0.78 GB** |

Accuracy on the same synthetic image (gradient, white square left, red circle
right, "ARGUS" text): SmolVLM2 called it a logo with "AR in the center";
LFM2.5-VL names the gradient, both shapes with correct sides, and reads
"ARGUS". Targeted questions work now too — *"Is there a person? yes or no"* →
`"No."` in 304 ms — because `VisionRequest::prompt` is finally real. The old
implementation had a **pre-tokenized fixed prompt**, so that field was dead.

**Cost is driven by input resolution, not by `image_max_tokens`.** The model
tiles dynamically: 256 px → 83 prompt tokens → ~720 ms; 1280 px → 785 tokens →
~3.1 s. `image_max_tokens` only trims per-tile detail, and below 256 the model
stops reading text in the scene. `[vision] max_input_px` (default 384) is the
knob, and it fits the pipeline design where the VLM sees a person crop rather
than the whole frame.

**Two traps worth remembering**:
- `mtmd_input_text` has a `text_len` field. Leave it zero and the text reads as
  empty, failing with "number of media markers in text (0)" even though the
  marker is right there.
- `llama_chat_apply_template()` is **not** a jinja parser (its own header says
  so) and mangles LFM2.5's template, which uses macros. The ChatML prompt is
  built by hand.

`face-service.cc` now defines `STB_IMAGE_STATIC`: `mtmd-helper.cpp` vendors its
own stb_image, and without that the two copies collide at link time.

## Vulkan offload for LLM + VLM (2026-08-08)

`third_party/llama.cpp` builds with `GGML_VULKAN=ON` whenever the Vulkan
loader, `glslc` and **SPIRV-Headers** are all present; otherwise it degrades to
CPU with a log line. `scripts/setup.sh` installs the three for pacman/apt/dnf.

Measured on the reference AMD Radeon Vega iGPU (RADV RENOIR), full offload
(`n_gpu_layers = 999`) vs CPU:

| | LLM CPU | LLM Vulkan |
|---|---|---|
| process RSS at init | +1 249 MB | **+245 MB** |
| tok/s, short prompt | 36.4 | **49.0** |
| tok/s, ~1 200-token prompt | 27.3 | **48.0** |
| TTFT, ~1 200-token prompt | 2 462 ms | **1 783 ms** |

The RSS collapse is the weights living in VRAM rather than in the process, and
throughput no longer degrades as the context grows (39-49 tok/s from 40 to
5 800 tokens, versus 36 → 27 on CPU).

The VLM benefits too, though the first measurement said otherwise. A single run
per side suggested Vulkan was 20% *slower*; three runs each showed the opposite
and explained why: **CPU 1453/999/1040 ms vs Vulkan 876/881/871 ms**. Vulkan is
~16% faster on the median and collapses the spread from 454 ms to 10 ms. For a
real-time pipeline the predictability matters more than the average. Lesson
worth keeping: this machine has ~13-20% run-to-run variance, so single-run A/B
comparisons are not evidence.

`HardwareProbe::llmGpuLayers()` / `vlmGpuLayers()` therefore offload on **any**
Vulkan device (integrated included) as long as the host has ≥8 GB RAM.
`[llm] gpu_layers` and `[vision] gpu_layers` override it: `-1` auto, `0` force
CPU, `N` offload N layers.

## Real-time voice pipeline (2026-08-08)

The reply used to be generated in full before the TTS started, so latency to
first audio was *whole generation* + *first synthesis*. Two independent causes:
`voice-test` accumulated every token and called `speak(full)` after
`chatStream` returned, and `playPcm()` opened a new PulseAudio stream per chunk
(`pa_simple_new` → `write` → `drain` → `free`); since `drain()` blocks until the
last sample is audible, synthesis of chunk N+1 did not even start until N had
finished playing. The second cause is the important one — it guarantees a gap
between chunks no matter how well the text is split.

**Splitting by tokens is not an option.** Supertonic synthesizes each chunk with
its own intonation contour; cut mid-sentence, it applies a falling
end-of-sentence cadence. The fix is to detect the sentence boundary
**incrementally** over the token stream and never synthesize a fragment:
`completeSentenceEnd(buffer, minChars)` in `tts/onnx-utils`, which reuses the
existing `isSentenceEnd()` (abbreviation list + digit guards) and only returns a
cut once the character *after* the punctuation has been seen.

Playback is one PulseAudio stream per turn (`labs/voice-test/audio.{hxx,cc}`):
`pa_buffer_attr.tlength` bounds how far ahead the audio runs and
`pa_simple_write` blocking on a full buffer paces the producer with no extra
logic. `flushPlayback()` drops what is already queued on interrupt. If
PulseAudio is missing, `openPlayback` returns false and the old per-chunk path
is used — bounded degradation, not a failure. Three stages: generation (the
`chatStream` callback turns tokens into sentences), synthesis and playback (both
on the `speaker` thread). Single consumer, so sentences are synthesized one at a
time and in order. Constants: `kMinSentenceChars = 24`,
`kFirstSentenceMinChars = 0`, `kPlaybackLatencyMs = 700`.

**A quality regression came free with the split, and is worth remembering.**
`autoQuality()` picks diffusion steps from the length of the text it receives.
`speak(full)` used to get 300+ characters → 8-10 steps; a lone sentence is
almost always <100 → 5 steps. Quality dropped system-wide, and worse, it became
*inconsistent within one reply* (a long sentence landed on Medium, short ones on
Low) — that timbre change mid-reply is what was perceived as the TTS
"stuttering". Fix, keeping `auto`: `autoQuality()` never returns `Low` (floor is
`Medium`, `High` enters at score ≥2); `steps_low` 5→6, `steps_high` 10→12; and a
per-device ceiling `HardwareProbe::ttsStepsCap()` (Minimal 5, Low 8, Balanced
12, High 16) that `resolveSteps()` clamps against.

`ThreadBudget::ttsThreads()` = `clamp(hardwareThreads()/2, 2, 8)`. Same split as
`computeThreads()` with a lower cap: diffusion synthesis stops scaling around 8
threads, so on a 64-thread server a larger share would only steal cores from the
LLM. A first attempt at `steps_medium = 10` **with** threads lowered to 6 pushed
a short sentence from 276 to 681 ms (RTF 0.70) — exactly the sentence on the
critical path to first audio — and was reverted. Raising quality and lowering
threads in the same measurement hides which one caused what.

Measured RTF (synthesis time ÷ audio duration; <1 means synthesis stays ahead of
playback): short sentence 0.53, two sentences 0.35, three 0.33, long sentence
0.21, long multi-sentence 0.28. Incremental splitting is 7/7 in
`labs/tts-probe`, including `stable=1` (result does not depend on how the text
arrives split — tested with 1, 3, 7 and 64-character tokens) and `lossless=1`.

**Barge-in**: the local-mic path listens during playback with a dedicated
interrupter thread (its own `VadService`); when the VAD detects speech it
stops the TTS (`speechDetected` → `stopSpeech`), so the speaker is cut in
~sentence granularity. The camera path has no barge-in: the reply is sent in
full over the talk channel and the mic frames during the send are cleared
after it (the loop is blocked sending). Real acoustic echo cancellation
(WebRTC APM or similar) is still not implemented.

## Video distribution over the `/sync` WebSocket (2026-08-08)

WHEP/WebRTC straight from go2rtc to the phone only works inside the house: a
tunnel cannot expose a peer-to-peer port, and making it work outside would need
TURN. Media is therefore relayed as fMP4 over the backend's own port, and the
bytes ride the already-authenticated `/sync` WebSocket rather than a separate
HTTP route — which also keeps the JWT out of a player URL, where it would end up
in the tunnel's logs. Video is only sent while the client asks for it
(subscribe on entering the screen, unsubscribe on leaving), so the socket never
carries more than what is being watched. Both C225 streams are used without
transcoding: `stream2` (sub, ~640×360) for the preview grid, `stream1` for the
detail view — MJPEG at 640×360 and 25 fps is ~6 Mbps, more than H.264 at 1080p.

**The finding that forces the design**: `drogon::WebSocketConnection::send()`
returns `void`. There is no write-completion callback, no high-water mark, and
no way to know whether the socket drained. Pushing fragments at camera pace
grows trantor's buffer without bound against a slow client until the RAM is
gone. The HTTP relay survives by accident because `ResponseStream::send()`
returns `false`. **Credit-based flow control is not a refinement — it is the
only thing preventing the OOM.**

`StreamHub` keeps one upstream per (camera, quality) rather than per viewer, so
three people watching the same camera cost one go2rtc connection, and holds it
for a grace period after the last subscriber leaves so switching screens does
not restart it. fMP4 starts with `ftyp+moov`; the hub caches that init segment
per upstream, sends it to every new subscriber before anything else, and joins
the stream at the next keyframe. Skipping either step is the classic silent
failure of this design: the player shows a black screen and reports nothing.

Binary framing, 12-byte header: `0` magic `0xA7`, `1` version, `2` type
(1=init, 2=media, 3=audio), `3` flags (bit0 = keyframe), `4..5` subId
big-endian, `6..7` reserved, `8..11` seq big-endian.

## Stability & real-time batch (2026-08-08/09)

Executed from `STABILITY_AND_REALTIME_PLAN.md` task by task. Findings worth
remembering:

- **The resampler needs carried state.** The old sinc (Blackman, `kSincHalf=32`)
  restarted at `pos=kSincHalf` per call: applied to 1024-sample blocks at
  8→16 kHz it dropped 6.25% of the audio (in=1024, expected=2048, got=1920) —
  an ~8ms hole every 128ms and a compressed timeline. `AudioResampler`
  (`src/shared/wrapper/audio/`) keeps `history_` (the `kSincHalf` samples the
  kernel looks back) and the fractional `pos_` between calls, so streaming and
  one-pass produce bit-identical samples (asserted in `labs/audio-probe`).
  `tapo_audio::resample` delegates to it.
- **HTTP transfer encoding is read from headers, never sniffed.** The old
  `Fmp4Reader` guessed chunked by scanning for the first `\n` per buffer: a
  `recv` without `0x0A` (≈46% chance for a 200-byte recv) silently dropped its
  bytes, and a `\r`+hex-digit before the `\n` killed the whole stream forever.
  `isChunked(headers)` decides; the chunked state machine consumes the
  terminator byte-by-byte so CRLF splits across buffers are harmless; fragments
  are emitted as whole `moof+mdat` boxes with the keyframe read from
  `trun`/`tfhd` sample flags. The plan's original `moofIsKeyframe` did not
  descend into the `moof` box and always reported keyframe — fixed in the
  implementation.
- **Dead upstreams leave the map.** When the reader thread exits (grace,
  EOF, socket error) its entry stayed in `upstreams_`, so the next subscriber
  got a valid subId on a dead upstream and no bytes arrived until a backend
  restart. `Upstream::dead` is set by the reader thread; `getOrOpen` joins and
  recycles it. The hub also treats non-`EAGAIN`/`EWOULDBLOCK` recv errors as
  fatal instead of spinning at 100% CPU, and the lock order is
  `hubMutex_ → Upstream::mtx`, never the reverse (was ABBA with
  `activeSubscribers()`).
- **The credit window is per connection, not per subscription.** All
  subscriptions of one client share a TCP socket; 6 cameras at 128KB each put
  768KB in flight on one trantor buffer with no ceiling. `ISink::tryReserve`
  /`release` moved the window into the sink; `ack()` credits the sink.
  Subscriptions per client are bounded (`hub_max_subs_per_client`, 429
  `TOO_MANY_REQUESTS`).
- **`VadService` is an instance class while the AI services are static.** The
  LSTM state is per audio stream (one VAD per conversation), but the 2.3MB
  ONNX model is loaded once in a file-static session behind a mutex — the same
  shared-context pattern as the other AI services.
- **The talk session and the mic during talk.** The original plan kept the
  8800 session open with a silence keepalive, and in that state the C225 went
  silent on the RTSP mic while the talk channel was open (measured then;
  `CameraMic` timed out every ~5s). The keepalive was dropped and `sendChunk`
  now keeps the session open across sentences (opening lazily, reopening only
  on send failure; the camera closes it after its own idle timeout.
  Measured 2026-08-09 with that flow, the mic **keeps delivering audio during
  the whole talk** (dropped-frame counters showed `pausa=202752` samples in
  one run), so no mute occurs. The perceived post-talk deafness was a
  **double discard**: a pause over the send plus a drain of the full reply
  duration, deafening the mic for ~2x the reply. Fixed by dropping the pause
  (the loop is blocked in the send anyway and the accumulated echo is cleared
  with `camBuf.clear() + vad.reset()`) and draining only
  `talk_drain_margin_ms` of the speaker's playout tail — verified as a 100%
  fluid conversation with ~780ms drains.
- **The speaker AGC overflowed.** A full-scale `-32768` sample stored its
  absolute peak in `int16_t`, giving `gain = 26000 / -32768 = -0.793` and
  inverting + attenuating the whole sentence. `tapoApplySpeakerGain` computes
  the peak in `int` and clamps (verified by `argus-tapo-probe --offline`).
- **Stale mic audio poisons the turn after blocking stages.** While the VLM
  captures/describes and the LLM generates (seconds each), the mic keeps
  filling the buffer; on resume the VAD fires on audio from the past (this was
  the reported production bug: after "analiza la cámara 1", Argus answered
  things nobody said). Every blocking stage is followed by
  `camBuf.clear() + vad.reset()` before listening resumes, and camera frames
  come from go2rtc (`MediaRelay::snapshotBytes`) instead of a third RTSP
  session.
- **Task 13 (camera audio through go2rtc) — hardware check done.** go2rtc
  serves the camera mic as a mono 8 kHz FLAC track via
  `/api/stream.mp4?src=cam1&mp4=flac` (verified 2026-08-09); `CameraAudioSource`
  needs a FLAC decoder before it is viable, so `CameraMic` (FFmpeg RTSP)
  remains the mic source for now.

## Retired plans and pending verifications (2026-08-09)

`STABILITY_AND_REALTIME_PLAN.md`, `OPTIMIZATION_AND_MEMORY_PLAN.md`,
`PLAN_EXECUTION_STATE.md` (2026-08-09) and later `MEMORY_HUMAN_PLAN.md`,
`EXTRACTION_ENGINE_PLAN.md` (2026-08-13) were deleted: their tasks are either
landed (see the sections above and below) or superseded by this file and the
git history. The extraction-engine verdict (NuExtract-1.5-tiny stays, tier
order = lexicon-first with coverage escalation) is recorded in the "Memory:
deferred extraction, semantic recall, vocabulary in SQLite" section below.
References to them in `AGENTS.md`/`CONTEXT.md` are kept as history. What is
still left to verify before the phase can be called complete:

- **Validated end-to-end by the user on 2026-08-09** (live conversation over
  the physical C225): camera vision (`describeCamera` via go2rtc snapshot —
  Task 15's fix), talk through the camera speaker, mic listening, and the
  LLM/STT/TTS stack — a 100% fluid conversation with no false turns. What
  follows are the formal/optional checks.

- **VAD calibration against recorded camera audio.** The live conversation
  (2026-08-09) validated the VAD + echo gate end-to-end: 100% fluid
  turn-taking, no false turns, ~780ms echo drains. The formal fixture
  calibration remains optional: record `labs/fixtures/camera-quiet.wav`,
  `camera-speech.wav` and `camera-echo.wav` with `argus-voice-test
  --audio-dump`; acceptance: quiet → 0 turns, speech → exactly 3 turns (tune
  `min_silence_frames`/`min_mean_prob`), then derive `talk_drain_margin_ms`
  from the echo fixture. `argus-audio-probe --vad <wav>` reports turns and
  `--vad-probs <wav>` prints the per-window probability timeline for the
  threshold-crossing measurement. The current `[vad]` values (threshold 0.45 /
  neg 0.25, 5 open / 12 close, 240 ms min turn, 0.35 min mean prob) are
  provisional until then.
- **Camera audio through go2rtc.** Verified against the physical C225 on
  2026-08-09: `/api/stream.mp4?src=cam1&mp4=flac` serves a mono 8 kHz FLAC
  track — the go2rtc mp4 module ignores `video=`/`audio=` params (the
  `mp4=flac`/`mp4=all` filter is what enables the PCM family, verified in
  `pkg/mp4/helpers.go`) and its muxer repackages G.711 to FLAC
  unconditionally (`pkg/mp4/consumer.go`), so `decodePcm` in
  `CameraAudioSource` needs a FLAC decoder (libFLAC via Conan) before that
  path is usable; `CameraMic` (FFmpeg RTSP) remains the mic until then.
- **TS muxer stream type.** `streamType 0x90` / `streamId 0xC0` is unverified
  against the C225; a wrong value fails as silence, not as an error. Settle
  with `tapo-probe --ts-dump` against the physical camera and record how.
- **LLM prefill above `n_batch`.** The chunked prefill landed in
  `llm-service.cc`; the user validated LLM, STT, TTS and the VLM on 2026-08-09
  in the live conversation (fluid replies, instant transcription). The formal
  check remains: `argus-llm-bench --llm` should show `ok=yes` for the
  ~2900-token and ~5800-token rows (they failed with "prompt decode failed"
  before the fix).
- **Future phases (not started; decisions preserved from the plans):** the
  detector/tracking/memory/tools work keeps: one RTSP session per camera via
  go2rtc, all media through the backend port, the tool DSL with the native
  `<|tool_call_start|>` token, and the LFM2.5 hybrid handling (6/16 attention
  blocks → KV `f16`; the conv state is not position-truncatable, so prefix
  reuse must use `llama_state_seq_get/set_data` snapshots, never `seq_rm`).
  Open decisions: `min_track_hits` 2 vs 3, YOLO26 AGPL-3.0 vs RF-DETR-Nano,
  face crop retention (privacy). Barge-in/AEC (WebRTC APM) remains out of
  scope.

## Conversational tuning (2026-08-09)

After the user found the replies too rigid and formulaic ("feels like it
ignores me"), three caller/config-level knobs were set (no changes inside
`LlmService`):

- **`[llm] context_size = 32768` default, up to 128000** — the knob always
  existed in `config.toml` (clamped 4096..128000 in `llm-service.cc`; 128000
  is the model's maximum). Default stays 32k; raising it costs ~+300 MB of f16
  KV cache (only 6 of 16 LFM2.5 blocks carry attention) and marginally slower
  decode as the context fills; prefill only pays for real tokens. It pays off
  only if the history actually grows, so the real lever is
  `[conversation] history_messages`.
- **`[conversation] history_messages = 41`** — the conversation keeps 20 turns
  (was a hardcoded 21 messages). `LlmService` prefix reuse (`resetContext =
  false`) only stays incremental while the history head is un-pruned; pruning
  forces a full re-prefill, which is why a larger cap plus a larger context
  helps long conversations.
- **`[llm] temperature = 0.85`** (was 0.7) for more varied, natural phrasing.
- **New system prompt in `labs/voice-test`** — warm, conversational persona
  that must engage with the user's actual words and must not reply with
  generic offers ("how can I help you", "anything else"). The old prompt's
  "direct and brief" guidance produced the stiff assistant register.

## Runtime config writes (ConfigService)

`ConfigService` is read-only in memory, but ALSO supports runtime writes:
`setBool/setString/setInt/setDouble(keyPath, value)`. They update the in-memory
table AND persist the value directly to `config.toml` via a surgical line edit
(comments/formatting preserved, other keys untouched). Guarded by a mutex;
used for one-time flags that must survive restarts without a DB table.

Example: `[pairing] paired` (in `config.toml`). `false` → the server prints the
pairing QR banner at startup and accepts `POST /pairing`; on success the
controller calls `ConfigService::setBool("pairing.paired", true)` (memory +
file), so the next boot skips the banner and `POST /pairing` returns 409
`CONFLICT` ("Server already paired").

## Uniform API response format

Every endpoint returns `{ status, info, errors }`. Errors always
`errors: { code, message }` with `info: null`; success has `errors: null`.
Codes: `BAD_REQUEST`, `UNAUTHORIZED`, `FORBIDDEN`, `NOT_FOUND`,
`METHOD_NOT_ALLOWED`, `CONFLICT`, `VALIDATION_ERROR` (422 adds `fields`). All error
responses go through `AppConfig::get{4xx}Response()`; `errorCode` was added
to `ResponseException` so services emit the same codes as the filters.
A Drogon custom error handler wraps built-in 404/405 in the same envelope.

## Face recognition architecture (ncnn, not InspireFace)

- **Detector**: RetinaFace (ncnn, Vulkan when available) — detects faces + 5-point landmarks
- **Recognizer**: MobileFaceNet 128-dim (ncnn) — computes face embedding
- **Index**: FaceDB (HNSWlib inner-product, 128-dim) — sub-millisecond nearest-neighbor search
- **Threshold**: 80% minimum confidence (`dist < 0.20` in inner-product space)
- **Persist**: FaceEmbedding repository stores embeddings as hex-encoded BLOBs in SQLite
- **Cold start**: FaceDB loads embeddings from database at startup via `loadFromDb()`
- **Pipeline cache**: Vulkan SPIR-V shaders cached to `models/face/face.ncnn.vkcache`
- **Optimizations applied**:
  - ncnnoptimize: fused ~60 conv+BN layers, fp16 weight storage
  - Vulkan on detector when a GPU exists (AMD iGPU RENOIR works; CPU fallback otherwise)
  - `from_pixels_roi` for optimized face crop + `PIXEL_BGR2RGB` inline conversion
  - PipelineCache + VkBlobAllocator/VkStagingAllocator for GPU memory efficiency
  - Bounded concurrency (`counting_semaphore`, `inferenceSlots()` permits)
  - Adaptive scaled decode: images >2048px decoded 1/2-1/4 via OpenCV
    (`IMREAD_REDUCED_COLOR_2/4`), smaller keep stb full decode

## Auth system

- **Login**: multipart/form-data face image → `FaceService::identify()` → person lookup
  → user lookup → JWT issuance (access + refresh)
- **JWT**: HS256, dual secrets. Claims: `sub=userId`, `iss=argus`. No role in token.
- **Refresh tokens**: single-use rotation (`is_used=1` after first rotation), replay-protected
- **Device binding**: hash of User-Agent + IP stored per token, validated on each request
- **Filter chain**: `DeviceFilter → ValidJsonFilter → JwtFilter → RoleFilter`
- **Token extraction**: Authorization Bearer / query param `?token=` / cookie
- **Logout**: invalidates all refresh tokens for user (`is_valid=0`)
- **Roles**: Owner (full), Resident (CRUD resources), Guard (read only), Guest (cameras + auth)
- **Error handling**: centralized via `AppConfig::get400/401/403/404/405/409Response()`
  — uniform envelope `{status, info, errors:{code,message}}` for ALL responses
  (404/405 included via Drogon custom error handler). `ResponseException` carries
  an `errorCode` string; error codes live in `AppConfig::ERROR_CODE_*`.
  Validation exceptions caught globally by `AppConfig::handleException()` → 422
  with `code: VALIDATION_ERROR` + `fields`.

## Validation DSL

Located in `src/shared/validation/`:

| File | Purpose |
|------|---------|
| `rules.hxx` | 31 rule classes (string, numeric, arrays, timestamps, cross-field) |
| `validation_dsl.hxx` | 31 macros (IS_NOT_EMPTY, IS_EMAIL, IS_IN, BETWEEN, etc.) |
| `validator.hxx` | `Validator<T>` + `ValidationException` (422) |

DTOs self-validate via `START_VALIDATION` → macro chain → `END_VALIDATION()`.
Throws `ValidationException` which is caught globally by `AppConfig::handleException()`.
Controllers never use try/catch for validation.

## Dependency injection pattern (manual)

All services and filters hold dependencies as private members with `_` suffix:

```
AuthService: jwtService_, personRepository_, userRepository_, refreshTokenRepository_
JwtFilter:   jwtService_, userRepository_, refreshTokenRepository_
AuthController: service_
```

Never static methods for service classes. Never local/temporary repository construction.

## DTO conventions

- **Request DTOs**: `{Action}Dto` con factory `fromJson()` (camelCase) / `form_multipart()`
- **Response DTOs**: `Response{Action}Dto` con `toJson()` (camelCase)
- **Validation**: DTO factory validates inline via DSL macros, throws on failure
- **Examples**: `LoginDto`, `RefreshTokenDto`, `ResponseLoginDto`, `ResponseRefreshTokenDto`

## Database tables

- Home/collaboration: `user` (role, is_active, soft-delete via deleted_at),
  `person` (linked to users via `user_id`), `person_event`, `event`,
  `reminder`, `reminder_detail`, `context_note`, `project`, `project_member`,
  `project_task`, `calendar_event`, `calendar_event_share`
- Cameras: `camera`, `camera_stream`, `zone`
- People/access: `user_invitation` (solo hash SHA-256 del token, nunca el
  token), `invitation_redemption` (UNIQUE user_id), `stored_file`,
  `user_portrait`, `portrait_preview_capability` (one-use), 
  `portrait_access_request`, `portrait_access_grant`, `device_login_challenge`
  (login cruzado por QR)
- Auth/face: `refresh_token` (is_valid, is_used, device_hash, expires_at),
  `face_embedding` (BLOB 128-dim float; rowid = fila vec0 `face_vec`)
- Audit/notificaciones: `audit_log` (global, `changes` = JSON diff de
  `JsonDiff`), `user_audit_log` (por usuario, sincronizable), `notification`,
  `notification_token` (UNIQUE user_id+device_hash), `user_action_log`
  (write-only, NO sync)
- Memoria: `memory_entity`/`memory_alias`/`memory_fact`/`memory_edge`/
  `memory_episode`/`memory_source`/`memory_procedure` — grafo semántico
  (con sus FTS5: `memory_fact_fts`, `memory_episode_fts`, `memory_alias_fts`)
- Ops/voz: `job` — worker job queue (state, attempts, dedupe_key),
  `schema_version`, `voice_session`, `voice_message` (historial local de voz)

## Sync engine (WebSocket, one-way server→client)

- **WS route**: `/sync` with **`JwtFilter` only** (no `DeviceFilter`; device
  binding stays on HTTP). Synchronization only; notifications and
  token-register go over HTTP.
- **Operations** (`src/shared/contracts/sync-operation.hxx`): `InitialInfo=0` (on
  connect, sends `{id, role, isActive}` — no module list),
  `Synchronize=1` (initial projection and thereafter created/deleted rows),
  `SynchronizeAuditLog=2` (global field diffs; the backend picks tables by
  role), `SynchronizeUserAuditLog=3` (recipient-level field diffs, filtered by
  `sub`). Live events: `Add=4`, `Delete=5`, `Log=6`.
- **WS responses**: `SocketEmitDto` `{operation, option(TableName), info}`;
  errors `{type:"<type>_error", status, error}`.
- **Syncable entities**: 14 repositorios implementan `Syncable` — `user`,
  `user_invitation` (metadata solo Owner, sin token), `camera`,
  `camera_stream`, `zone`, `reminder`, `reminder_detail`, `calendar_event`,
  `calendar_event_share`, `project`, `project_member`, `project_task`,
  `event`, `person` — más `notification` (dedicated per user). Fuera del
  sync: `context_note`, las tablas de archivos privados/portraits y
  `device_login_challenge`. El frontend replica esta superficie en
  `SYNC_TABLE_KEYS`.
- **Bootstrap versus updates**: the first `Synchronize` supplies a complete
  authorized projection. Later normal sync pages use `created_at`, so they only
  add new records; deletes carry only `id` and `deletedAt`. All persisted modifications,
  revocations and notification read state are delivered as audit diffs, never by
  querying an `updated_at`/`syncAt` cursor.
- **Bounded audit cursors**: audit repositories sort on their auto-increment id
  and accept `afterId`/`endId`, returning `id > afterId AND id <= endId` in
  ascending order. `{findLast:true}` returns `watermarkId`; `afterId=0` is valid
  for an empty baseline and `endId` is positive. This gives clients a stable
  interval while writes continue.
- **Granular daily snapshots**: `AuditLogService`/`UserAuditLogService` create
  `JsonDiff::createFlatDiff(before, after)` changes, then compact the daily
  record/table (and recipient for user logs). The compacted snapshot is written
  as a new row so it gets a strictly increasing id; it contains only changed
  fields with previous/current values, not an entity replacement.
- **Publish boundary**: feature services capture `before`/`after` around the
  repository mutation and call `SyncAuditService::publishModule` or
  `publishUsers`. The facade builds diffs and emits `Log`; it deduplicates user
  recipients. Creation keeps `Add`, deletion keeps an id-only `Delete`. Do not
  hand-build full-record `Log`/`Add` messages for an update.
- **RoomManager**: instance class, file-level `thread_local` state; module rooms
  (`1 + TableName`) and a user room (`1000 + sub`); `emitUser` reaches the N
  active sessions. Lifecycle via `RoomManagerServiceAdapter` (IService) in
  `ServiceRegistry`.
- **HTTP**: `PATCH /notification/read` and `POST /notification-token` (full
  filter chain).

## Coding conventions

### Enums for constrained strings

All DB columns with CHECK constraints use `enum class` from `enums.hxx`:
`UserRole`, `EventSeverity`, `CameraRecordMode`, `ZoneType`, `ReminderDetailStatus`, `UserAction`.
Each has `toString()` / `fromString()`.

### Parameter structs (3+ params)

ANY function with 3+ parameters uses a struct (`{Entity}CreateInput` /
`{Entity}UpdateInput`, or any descriptive name). Applies to ALL layers
(repositories, services, controllers, filters). Struct goes in the
corresponding `*-query.hxx` file, or in the class header when there is no
`*-query.hxx`. Never raw multi-parameter signatures.

Construct structs with C++20 **designated initializers** passed inline
(`{.field = value, ...}`), fields in declared order, always listing every
member to avoid `-Wmissing-field-initializers`.

### Repository pattern

```
src/shared/repositories/{entity}/
  {entity}-query.hxx      — SQL + param structs
  {entity}-repository.hxx — class
  {entity}-repository.cc  — impl (using namespace {entity}_query)
```

### Controller thinness (4-8 lines per endpoint)

1. Parse → DTO (validates inline)
2. Extract context from attributes
3. Call service
4. Return `ApiResponse::ok(result.toJson())`

### ncnn optimization conventions

- Always set `use_vulkan_compute` + `use_fp16_packed/storage/arithmetic` on detector nets
- Use PipelineCache + VkBlobAllocator for GPU memory efficiency
- Use `from_pixels_roi` instead of manual pixel loops for face crops
- Use `PIXEL_BGR2RGB` in ncnn `from_pixels` to avoid separate cvtColor
- Run `ncnnoptimize` on models offline for fusion and fp16 conversion
- Recognize that some model architectures fall back to CPU on Vulkan (ncnn limitation)

## Long-term memory: MemoryService (2026-08-09, reworked 2026-08-10)

> SUPERSEDED: this section describes the legacy `memory_l1` / `MemoryStore` /
> `MemoryRecall` architecture (RRF, trigram, `memory_profile`), deleted in the
> 2026-08-11 semantic-graph rework. The CURRENT architecture is the SQLite
> semantic graph (entity/fact/edge/episode + FTS5 + vec0) documented in the
> sections below: "Kùzu gate", "All SQLite access goes through repositories",
> "Memory & Context Redesign", "Memory: deferred extraction, semantic recall,
> vocabulary in SQLite", "Memory: human-feel + recall reliability round",
> "Vocabulary moved to static constants" and "Vocabulary extension".
> Kept verbatim as history.

Ultra-light memory for Argus: **no extra LLM model** (conversation compaction
reuses the MAIN LlmService), **async background pipeline** (embedding of
captures, dedup, compaction), ~25ms recall on limited hardware. Separation of
concerns is strict: `LlmService` stays a context-free mediator; the caller
composes memory around it.

- **Scopes** (`memory_l1.scope`): `global | user | person | device | role` with
  a nullable `ref_id`. Recall always includes `global` + the caller's `user:<id>`
  (+ `person:<id>` when persons are in context) so Argus stays aware of
  system-wide facts, not just per-user ones. Types: `persona | episodic |
  instruction | system`; sources: `rule | llm | ingest` (enums in `enums.hxx`).
- **Capture channels (no extra LLM)**:
  1. *Explicit rules*: "recuerda que X" / "remember that X" — the phrases are
     rows in `memory_phrase` (kind + lang + phrase + memory type); adding a
     language = adding rows, no code.
  2. *Inline tool calls*: the main LLM may emit
     `<|tool_call_start|>save type=... priority=... content=...<|tool_call_end|>`
     during normal generation (prompt-instructed). `ToolParser` extracts them
     from the token stream in `voice-test` (tolerant: malformed blocks are
     dropped, never break the conversation) and strips them from the spoken
     text. Markers are configurable in `labs/config.toml`.
  3. *Ingest*: `reminder`/`context_note` are read directly at recall time.
- **Dedup (two nets)**: (1) synchronous lexical gate in
  `MemoryStore::saveDedup` — normalized equality, substring, word-Jaccard
  (bumps priority/hit_count instead of inserting); (2) **vector dedup in the
  background worker** — when the primary embedding of a new capture finds a
  neighbour at `memory.vector_dedup_sim` (0.93) in the same partition, the
  worker merges (bump + remove the new row). `MemoryStore` is the only writer
  (memory_l1 + both FTS tables + vec rows); `vacuumOrphans()` heals index
  rows whose memory no longer exists.
- **Recall** (`memory-recall.{hxx,cc}`): three ranked lists fused with
  **RRF** — FTS5 `bm25()` (unicode61 words) + FTS5 trigram (substring,
  language-agnostic) + `vec0` KNN (embeddings, per content/expanded view) —
  then reweighted by priority, recency decay and hit_count, with a quadratic
  cosine bonus calibrated on the reference hardware (related pairs 0.83-0.90,
  unrelated ~0.76-0.80). **Lexical fast path**: when FTS produces ≥
  `memory.lexical_min_hits` candidates, the embedding forward is skipped
  (most turns answer in <5ms). `memory.recall_deadline_ms` bounds the turn.
  Capped by `memory.recall_top_k` and the `memory.recall_max_tokens` budget
  (chars≈4×tokens). Measured in `labs/memory-probe --recall-bench`:
  **p50≈28ms, p95≈33ms, precision 30/30 stable** (5/5 runs) on the reference
  machine. Without the embedding model the vector layer is skipped (degraded,
  lexical-only recall).
- **Embeddings** (`embedding-service.{hxx,cc}`): `multilingual-e5-small` INT8
  ONNX (118MB, 384-dim, 100+ languages) via ONNX Runtime (already a dependency),
  downloaded by `scripts/setup.sh` into `models/memory/`. **Loaded lazily** on
  the first real embed (in the worker), never at boot; `memory.embedding_dim`
  truncates the pooled vector (vec0 schema + `Rebuild` migration handle it).
  The tokenizer is a
  hand-rolled **Unigram** implementation (`unigram-tokenizer.{hxx,cc}`) parsing
  `tokenizer.json` (nlohmann_json) — verified **bit-identical token ids** to the
  reference `tokenizers` library on Spanish/English samples, including the
  Metaspace ▁ and `<s>...</s>` post-processing. Pooling: mean over non-pad +
  L2 normalize. e5 prefixes: `"query: "` for recall, `"passage: "` for stored
  content. Note: e5-small cosines on single words are compressed (~0.87 for
  unrelated words); ranking (not absolute cosine) is what matters — validated
  with relative checks in `--embed-check`.
- **`VecDb`** (`sqlite/vec-db.{hxx,cc}`): the only sqlite3 connection with the
  vec0 module. Owns `memory_vec` (float[N] with `partition TEXT PARTITION KEY`
  = `global`/`user:<id>`/`person:<id>`, `memory_id`, `view`) and `face_vec`
  (float[128]); created lazily on first use, all access serialized by
  `VecDb::mutex()` (callers take the lock — `handle()` does NOT lock;
  non-recursive mutex). Schema/dims changes are detected by
  `schemaOutdated()` → recreate `memory_vec` only (face_vec untouched) +
  background `Rebuild`. Connection and all prepared statements are RAII
  (`std::unique_ptr<sqlite3>` and the `SqliteStmt` wrapper in
  `src/shared/wrapper/sqlite-stmt/`).
- **Async worker** (in `MemoryService`): a dedicated thread + job queue
  (condvar). Jobs: **Embed** (chunks + views + vector dedup), **Compact**
  (conversation summary with the MAIN LlmService, `preferIdle` waits for the
  LLM to be free so a live turn is never delayed), **Rebuild** (re-embed
  every memory after a schema/dims migration). `flushPending()` waits for the
  queue AND the in-flight job (`gWorking`) with `memory.flush_timeout_ms`.
- **`MemoryService`** facade: `captureExplicit` (rule parse + saveDedup +
  async embed), `captureToolCall`, `captureImplicit`, `recall` (→
  `{prependText, profileText, usedIds}`), `bumpHitCount`, `enqueueSummary`
  (session end, waits at exit), `enqueueCompaction` (mid-session, preferIdle).
  `voice-test` integrates it: profile → system prompt at conversation start
  (stable → prefix reuse intact), memories → prepended to the user message
  each turn, hit_count bumped after each reply; when the history cap prunes
  turns, `trimHistory` compacts the dropped transcript in the background
  instead of discarding it. Flag `--memory-user <id>` enables it.
- **L3 persona profile**: `memory_profile` is a cache of the solid persona
  facts (type=persona, priority ≥ 60, ordered by priority/hit_count/recency),
  rebuilt from `memory_l1` on the fly when stale (`memory.profile_stale_seconds`)
  and upserted. Injected at conversation start — zero per-turn cost.
- **FaceDB migrated from hnswlib to vec0** (2026-08-09): `face_vec` (rowid =
  face_embedding.id, cosine KNN, best-per-person, 0.80 threshold — semantics
  unchanged). `face_embedding` remains the canonical synced row (sync contract
  intact). `loadFromDb()`/in-memory index/`face.ef_search` are gone (exact scan
  is sub-ms at household scale). hnswlib submodule removed.
- **Labs**: `argus-memory-probe` — `--schema-check` (tables + FTS5 bm25 +
  trigram + vec0 KNN roundtrips), `--capture-test` (es/en phrase parsing),
  `--tool-parse-test` (fragmented stream parsing), `--embed-check` (embedding
  sanity incl. cross-lingual), `--recall-bench` (3-run latency stats),
  `--vec-gate-test` (pure-paraphrase recall gate, incl. the synonym view),
  `--profile-test` (L3 persona cache), `--dedup-bench` (vector merge),
  `--sims` (query→memory cosines for threshold calibration), `--tokens`
  (tokenizer debug). All green on the reference machine.
- **Environment note**: the system CMake 4.4.0 breaks this build's generate
  step (nested `project()` subprojects, "CMAKE_C_COMPILE_OBJECT missing").
  CMake **3.31.6** is pinned via `uv tool install cmake==3.31.6`
  (`~/.local/share/uv/tools/cmake/bin/cmake`) — use that binary for configure
  and build until the subprojects are updated. `scripts/setup.sh` still installs
  the distro cmake; the uv one is the known-good path on Arch.

## Kùzu gate (2026-08-11) — FAILED, fallback to SQLite

Phase 0 of the graph redesign tested both Kùzu candidates as the memory
engine: upstream `kuzudb/kuzu` @ `v0.11.3` (final release, pinned at
`third_party/kuzu`) and the Vela fork @ `v0.12.0-vela.87bf0be`
(`third_party/kuzu-vela`). Both are MIT, both were built fully offline with
`fts`/`vector` statically linked and auto-loaded, both passed the idle-store
budgets in Release @ 5 000 facts (RSS 54 MB at open, KNN p95 13 ms,
traversal p95 2 ms). The real access pattern — consolidation worker
inserting while recall reads, one serialized connection (KuzuDb, VecDb
contract) — fails on **both**:

- upstream v0.11.3: inserts degrade 12 ms → 4–18 s under continuous reads
  (checkpoint starvation), non-deterministic `DirectedCSRIndex` OOB asserts
  (`LocalRelTable::getCSRIndex`), SIGTERM-proof hangs; interleaved
  insert+read hangs from ~100 facts even single-threaded;
- Vela fork: any significant activity through a second connection (read or
  write) asserts in `DirectedCSRIndex`.

Decision: Kùzu is not the memory engine. `SemanticGraph` is backed by the
existing SQLite tables (vec0 + FTS5 + adjacency); phases 2–8 of the plan
proceed unchanged.

## LfmAdapter tool calling + Phase 7 gate (2026-08-11)

Why the adapter changed (no code comments carry this): the LFM2.5 chat
template extracted from the GGUF is ChatML with tools declared inside the
system message as `List of tools: [...]`; it has no `<|tool_*|>` tokens.
Hand-built prompts must never contain literal special tokens — `llama_tokenize`
parses them with `parse_special=true` (llm-service.cc:222), so a literal
`<|tool_call_start|>` inside the system message becomes a real special token
in a position the model never saw trained, which suppresses generation (the
original 0/2 was this, not the model). Raw observation (Tarea 0, bench
`--verbose`) showed the model emits the tool call as raw JSON
(`{"name": ..., "arguments": {...}}`) or its own Spanish-keyed shape, never
the marker-wrapped format. The parser accepts raw JSON, pythonic
`[name(arg="v", ...)]` and marker-wrapped blocks; tool results are appended
as role `"tool"` messages (append-only, preserving LlmService::prefill
prefix reuse). Gate result in plan §9 Phase 7: the 1.2B does not follow the
declared schema (0/20 memory_save, 2/56 camera false positives) — fastText
stays. Separate finding: labs that do not link ncnn (tool-bench) compile
`HardwareProbe::probeVulkan` as a no-op (`__has_include(<gpu.h>)` guard,
hardware-profile.cc:12), so their `gpu_layers=0` is a lab-target artifact;
the product binary offloads to the RADV iGPU.

## All SQLite access goes through repositories (2026-08-11)

Rule: every SQLite query lives in `src/shared/repositories/`. The memory
graph stack now follows it: `src/shared/repositories/memory-graph/`
(`memory-graph-query.hxx` holds all SQL, `memory-graph-repository.{hxx,cc}`
the sync data-access layer taking `sqlite3*` under the caller's store
mutex). `SqliteGraph` became a thin connection+mutex holder delegating to
the repository; `EntityResolver`, `GraphRecall` and the embedding worker use
it too. Legacy exceptions (VecDb/face-db, memory-store/memory-recall)
keep their store-local SQL until their removal.

Phase 1 (2026-08-11) landed on that fallback: `semantic-graph.hxx` +
`sqlite-graph.{hxx,cc}` (memory_entity/alias/fact/edge/episode/source +
FTS5 + recursive-CTE hop recall, §4 schema in schema.sql), one-shot
`migrateLegacy()` (memory_l1 → memory_fact under a legacy entity),
`SqliteGraphServiceAdapter` + `MemoryServiceAdapter` registered at boot
(application.cc), and MemoryService converted static → instance (worker
state as private members; labs use a `gMemory` instance). `--graph-test`
15/15; recall-bench 30/30; build dev 0/0. The submodules stay pinned as reproducer/upgrade path;
`labs/kuzu-probe` (`EXCLUDE_FROM_ALL`, ~40 min compile when built
explicitly) is the gate probe and the crash reproducer
(`labs/kuzu-probe/REPRODUCER.md`, ready to file against Vela). The Wave A
overhead items (4, 7, 9) landed with `--recall-bench` p50 24.4 ms / p95
27.3 ms / 30/30 and `--intent-check` 103/103 — those remain the regression
baselines.

## MEMORY_CONTEXT_REDESIGN.md (folded 2026-08-11, Phase 5)

Historical record of the SQLite-only memory measurements. Superseded by the
graph redesign; kept verbatim as history.

## Memory & Context Redesign — how the LLM receives memory (2026-08-10)

> Status: implemented · Validation in progress · No commit until the user
> asks for it.

## Why

The conversation LLM (LFM2.5-1.2B-Instruct, Q4_K_M) is coherent and fast on
its own: validated with a clean-context session (no memory, no intent) where
the model answered naturally and in the right language. The incoherences
("tu hermana viene los domingos a ver la televisión", "your sister, claro
que sí", "a Rodrigo le gusta mucho el pollo") were caused by HOW the context
was passed to it, not by the model or the embeddings:

1. **English instruction blocks next to Spanish facts**: the `<relevant-memories>`
   header with English examples ("tu hermana means the user's sister") was
   prepended to every user message; the model echoed it ("your sister...").
2. **Profile dump**: `memory_profile` injected ALL persona facts (priority >= 60)
   into the system prompt; a 1.2B chokes on 8+ unrelated facts and confabulates
   details around them ("a ver la televisión").
3. **Common-verb noise**: queries sharing only a verb ("gusta") injected
   unrelated memories that the model then misattributed to the wrong person.
4. **Capture gaps**: natural phrasings ("quisiera que me hagas recordar acerca
   de que...") neither matched the hook tables nor the intent classifier, and
   trailing confirmations ("¿está bien?") blocked `parseStatement`.

## Design decisions

### 1. Clean context by default (no memory, no intent)

`labs/voice-test/voice-test.cc`:

- `systemPromptFor(langCode, withMemory, withIntent)` builds the prompt
  dynamically:
  - **Base** (always): pure conversational persona — language lock, "tú",
    brevity, honesty, "never speak as if you were the user" (say "tu hermana",
    never "mi hermana").
  - **+ memory** (`--memory-user N`): one short memory instruction block in the
    conversation's language + the `<|tool_call_start|>save...` tool instruction.
  - **+ intent** (`--intent`): the camera instruction line.
- Defaults: memory and intent are OFF. The user message goes raw to the LLM.
  No captures, no camera auto-description, no recall injection.
- Flags: `--memory-user <id>` enables memory; `--intent` enables the fastText
  intent classifier + camera auto-description.
- This made the A/B possible: clean model vs memory-enabled model.

### 2. Minimal per-turn memory block, in the conversation's language

`src/shared/services/memory/memory-recall.cc`:

- The English `<relevant-memories>` header with examples was replaced by a
  minimal block: plain lines between `<memorias>` / `</memorias>` (es) or
  `<memories>` / `</memories>` (en). No bullets, no quotes, no instructions
  inside the block.
- A short anti-echo directive closes the block, in the conversation's
  language: "Responde dirigiéndote al usuario: di 'tu hermana', nunca 'mi
  hermana'." — placed exactly where the model tends to mirror the user's
  first-person words.
- The HOW is instructed ONCE in the system prompt, never repeated per turn.

### 3. Selective L3 profile (never a dump)

- `memory.profile_max_facts = 4`: `memory_profile` is rebuilt with LIMIT 4
  (ordered by priority/hit_count/recency), so the system prompt receives a
  compact selection instead of every persona fact.

### 4. Relative score margin

- `memory.score_margin = 0.65`: after ranking, candidates scoring below 65%
  of the best hit are dropped. Fixes the common-verb noise ("gusta" matched
  "a Rodrigo no le gusta el pescado" AND "me gusta el café sin azúcar",
  and the model attributed the coffee to Rodrigo). Genuine multi-memory
  queries (both subjects score similarly) are unaffected.

### 5. Second-person conversion at injection (deterministic, code table)

- Stored facts keep the user's own words ("mi hermana"); the injected block
  is converted with `toSecondPerson()` (code table, es+en markers, word
  boundaries, longer keys first — e.g. "yo soy" before "soy"). Tables are
  small and disjoint, so both languages are applied regardless of input
  language.
- **Why not the LLM**: measured with 3 prompt styles (direct rule, few-shot
  Input/Output, arrow completion) — LFM2.5-1.2B echoed the input verbatim
  ("Me gusta el café sin azúcar" -> unchanged) or contaminated the output
  with the examples ("mi hermana viene los domingos, tú también lo haces...").
  A background LLM conversion job was implemented and reverted after these
  measurements; the deterministic table is 100% precise on the covered
  markers, costs 0 ms, and needs no system-prompt or config additions.
- Applied to the recall block AND the profile (profile content is injected
  at conversation start).

### 6. Capture (data-driven, scales by adding rows)

Trigger phrases and trailing confirmations started as `[memory.phrases.*]` /
`[memory.confirmations.*]` tables in `config.toml`. They now live in the
`memory_phrase` table — see "Memory: deferred extraction, semantic recall,
vocabulary in SQLite" below for why and how they are matched.

- "acerca de que" variants of the "recordar" hooks ("quisiera que me hagas
  recordar acerca de que" -> persona).
- Trailing confirmations ("¿está bien?", "¿ok?", "right?") are stripped before
  the capture gates (question gate + content). Adding a language = adding
  rows.

`src/shared/services/memory/rule-parser.cc`:

- `stripTrailingConfirmation(text, lang)`: data-driven per language, applied
  in both `parse()` (phrase path) and `parseStatement()`.

## What was tried and rejected (evidence)

| Approach | Result |
|---|---|
| English `<relevant-memories>` header + examples per turn | Model echoed the header ("your sister, claro que sí") |
| Profile with all persona facts | Confabulation around unrelated facts |
| LLM second-person conversion (direct prompt) | Echoed input for "me gusta", converted only the example "mi hermana" |
| LLM second-person conversion (few-shot Input/Output) | Echoed input verbatim |
| LLM second-person conversion (arrow completion) | Contaminated output with the examples (merged lists) |
| Config table `[memory.second_person.*]` | Reverted: the user asked for an LLM-based approach; it proved unreliable on the 1.2B, so a code table at injection was chosen instead |

## Files touched

- `labs/voice-test/voice-test.cc` — dynamic `systemPromptFor`, `--intent`
  flag, gated IntentService/camera/memory, clean defaults.
- `src/shared/services/memory/memory-recall.cc` — minimal `<memorias>` block,
  anti-echo directive, `toSecondPerson()`, selective profile (LIMIT 4),
  score margin, raw content retained.
- `src/shared/services/memory/memory-store.cc` — dedup requires >= 2 shared
  words; UTF-8-aware word extraction (accented words no longer split);
  robust index cleanup; `vacuumOrphans()`, `purgeScope()`.
- `src/shared/services/memory/memory-service.cc` — background worker (Embed /
  Compact / Rebuild), `gWorking` flush semantics, `LlmService::isBusy()`
  gating, `flush_timeout_ms`.
- `src/shared/services/memory/rule-parser.cc` — `stripTrailingConfirmation`
  (data-driven), content confirmation strip.
- `src/shared/services/embedding/embedding-service.cc` — lazy model load,
  `embedding_dim` truncation.
- `src/shared/services/sqlite/vec-db.cc` — configurable dims, `view` column,
  migration without touching `face_vec`.
- `config.toml` — `profile_max_facts`, `score_margin`, `[memory]` tuning keys
  (the phrase/confirmation tables moved to SQLite).
- `labs/memory-probe/memory-probe.cc` — `--purge`, `--seed`, `--sims`,
  `--vec-gate-test`, `--profile-test`, `--dedup-bench`; self-cleaning runs.
- `src/shared/wrapper/sqlite-stmt/sqlite-stmt.hxx` — RAII prepared statements.

## Validation

- `argus-memory-probe`: 8 suites green (schema 12, capture 18, tool-parse 5,
  embed 5, recall-bench 30/30 stable, vec-gate 3/3, profile 3, dedup 5).
- `argus-intent-probe --intent-check`: 103/103.
- Clean-context session (security protocol questions): 3/3 coherent replies,
  no memory, no capture.
- Memory-enabled battery: "¿cuándo viene mi hermana?" -> "Tu hermana viene
  los domingos a comer"; "¿qué no le gusta a Rodrigo?" -> "Rodrigo no le
  gusta el pescado" (no coffee misattribution); "¿qué me gusta tomar?" ->
  "Me gusta el café sin azúcar" (user attribution correct); multi-memory
  query returns both facts.
- Builds dev + prod: 0 errors, 0 warnings.

## How to test

```bash
cd build/prod/labs/voice-test
./argus-voice-test                          # clean: no memory, no intent
./argus-voice-test --memory-user 1          # memory on
./argus-voice-test --intent                 # intent/camera on
./argus-voice-test --memory-user 1 --intent # everything
```

## Known limitations

- The 1.2B occasionally mirrors the user's first-person words in questions
  ("¿cuándo viene mi hermana?" -> "Mi hermana viene los domingos") despite
  the injected second-person block and the anti-echo directive. Content is
  correct; the pronoun echo is a model-level residual.
- Existing stored memories (captured before this work) keep their raw form;
  purge + re-capture converts them (e.g. `argus-memory-probe --purge user 1`).
- `score_margin` is calibrated on the reference machine's fixtures; re-check
  with `--recall-bench` after changing it.

# Memory: deferred extraction, semantic recall, vocabulary in SQLite

## Why the model tier never runs on the turn

`MemoryService::captureExplicit` used to run the whole extraction pipeline
inline. When the utterance was one the lexicon could not parse, the turn paid
for loading the 469 MB NuExtract model plus ~1.1 s of decoding — a measured
`turn took 10726 ms`. Memory formation is not something the speaker waits for:
the fact is for *future* conversations.

So the turn now runs the deterministic tiers only (`ExtractInput::allowModel =
false`) and returns a typed outcome:

| `CaptureOutcome` | Meaning |
| --- | --- |
| `Stored` | The lexicon produced a fact inline (microseconds); `factId` is set |
| `Deferred` | A capture signal fired but needs the model: queued as `MemoryJob::Kind::Extract` |
| `Rejected` | No capture signal at all — nothing was queued |

`captureImplicit` (fastText salience) is always `Deferred`: the lexicon cannot
tell an assertion from a question, so that path always needs the model.
`allowModel` gates only the model tier, not the extractor — gating the whole
extractor made every utterance defer, which the 50 ms gate in
`--schema-check` now prevents from regressing (currently 0 ms).

## Why recall grew a semantic tier

Recall v2 resolved entities and fell back to FTS. Embeddings were written by
the worker and never read, so a paraphrase with no shared words could not be
recalled: "la cena se sirve a las ocho" was invisible to "¿a qué hora
comemos?". `GraphRecall::collectSemantic` runs a vec0 KNN when the entity and
lexical tiers return fewer than `memory.lexical_min_hits` facts, keeping the
embedding forward off cheap turns. Semantic hits score `priority * cosine`, so
they rank below an equally-important lexical hit instead of displacing it, and
`memory.vector_min_sim` (0.80) keeps unrelated neighbours out — the
noise-query assertion in `--vec-gate-test` still injects nothing.

The floor alone is not enough for smalltalk: e5 scores "hola" at 0.79,
"gracias" at 0.82 and "buenos días" at 0.85 against a completely unrelated
fact, overlapping the range where real paraphrases live. No threshold
separates them, so the tier is gated structurally instead — it runs for
questions (`?` / `¿`) and for turns of four words or more. Without this,
"hola" answered "¡Hola! Tu hermana viene los domingos". `--vec-gate-test`
asserts the three smalltalk turns inject nothing.

## Why the capture vocabulary lives in SQLite

Trigger phrases, trailing confirmations, statement anchors and recall markers
were split across `config.toml` tables and hardcoded C arrays, and
`RuleParser` scanned every phrase per turn with `find`. `config.toml` is
system-level configuration, not domain vocabulary, and a linear scan gets
slower with every phrase added.

`memory_phrase` and `memory_lexicon` hold that data now, compiled into
`PhraseAutomaton` (Aho-Corasick, p95 0.57 µs at 10k patterns, zero
allocations per match): one pass over the turn text yields every trigger,
confirmation, anchor and marker at once, so adding a language or a phrasing
costs a row, not latency. `PhraseCatalog` publishes an immutable snapshot
behind a mutex, the same shape as `EntityResolver`. The extract layer stays
free of SQL: the memory layer loads `extract::LexiconEntry` rows and calls
`TieredExtractor::rebuild`.

`RuleParser` became an instance class over the catalog. Match positions must
map back to the original text for slicing, so the lowercase pass is
byte-preserving (ASCII only) — accented phrases keep their byte layout.

## Validation of this round

- `argus-memory-probe`: 12 suites green (capture 18, formation 24, schema 17,
  graph-recall 7, entity 7, conflict 4, graph 15, tools 5, vec-gate 9,
  dedup 4, tool-parse 5, embed 5).
- `argus-extract-probe`: `--extract-test` both gates pass (tier 1 alone
  >= 40/60), `--holdout-test` 20/20 recall with 100% subject / verbatim /
  distinctness precision, `--nuextract-test` verbatim >= 90%,
  `--automaton-test`, `--automaton-bench`, `--tier-bench` all pass.
- `argus-intent-probe --intent-eval`: camera P 0.97 / R 0.98, memory_save
  P 0.97 / R 1.00; `--intent-bench` p95 2.7 µs.
- `argus-queue-probe`: all gates pass.
- Builds dev + prod: 0 errors, 0 warnings.

## Known limitations of this round

- The deferred path means a fact captured this turn is not recallable in the
  same turn. Tests bridge it with `flushPending()`; conversation-wise the fact
  lands seconds later.
- `memory_lexicon` predicates are Spanish/English surface forms; a new
  language needs rows for all four kinds, not only predicates.
- `--extract-probe` now needs `database/` (symlinked next to the binary);
  without it the lexicon loads empty and every case falls to the model.

# Phase 4 — the static-only services are gone

Fifteen services held their state in file-scope globals and exposed only
static methods, which `AGENTS.md` §4 forbids: a service is an object with
`_`-suffixed members and manual DI. `MemoryStore` and `MemoryRecall` were
deleted with the legacy store; `RuleParser` became an instance over
`PhraseCatalog`; the remaining twelve were converted in this round.

Two ownership shapes came out of it, and the distinction is what keeps the
result honest rather than a service locator with extra steps:

**Constructor injection** — for anything with exactly one parent:

| Service | Owner |
| --- | --- |
| `EmbeddingService` | `MemoryService` |
| `FaceDB` | `FaceService` |
| `IntentService` | `IntentServiceAdapter` |
| `SttService`, `TtsService`, `VisionService` | their `IService` adapters |
| `GraphRecall`, `PhraseCatalog`, `EntityResolver` | `MemoryService` |

**A single `instance()` accessor** — for the process-wide resources whose
consumers Drogon constructs itself (controllers build their own services, so
they cannot be handed a reference at construction):

| Service | Resource |
| --- | --- |
| `VecDb` | the vec0 connection shared by memory and face |
| `LlmService` | one llama model + context |
| `FaceService` | detector/recognizer nets |
| `Go2rtcManager` | the external go2rtc process |
| `MediaRelay`, `StreamHub` | viewer slots and per-camera upstreams |

These are still real objects with private members — the accessor only answers
"which one", and consumers inside the DI graph still take references
(`MemoryService{VecDb::instance(), LlmService::instance()}`), so a probe can
pass its own instance instead. That is what `labs/memory-probe` does: it owns
`gVecDb`, `gLlm`, `gEmbedding` and builds `MemoryService` on top of them.

Recurring C++ detail worth remembering: a service whose header forward-
declares a heavy type (`Ort::Env`, `TtsEngine`, `llama_model`) and holds it in
a `unique_ptr` cannot have its constructor defaulted **in the header** — the
defaulted constructor instantiates the member destructors, which needs the
complete type. The constructor and destructor are declared in the header and
defined in the `.cc`.

## Validation of Phase 4

- `argus-memory-probe`: 12 suites, 120 asserts, 0 failures.
- `argus-extract-probe`: `--extract-test`, `--automaton-test`,
  `--holdout-test`, `--nuextract-test` all pass.
- `argus-intent-probe --intent-check`: 103/103.
- `argus-queue-probe`, `argus-tts-probe` (real synthesis),
  `argus-vision-check` (init 898 ms, describe 1513 ms, cache hit 5.5 ms):
  all pass.
- `argus-voice-test --text-chat --memory-user 1 --intent`: capture deferred,
  the worker stored the fact, the next turn recalled it, greetings stay clean.
- Builds dev + prod: 0 errors, 0 warnings.
- `ServiceRegistry` untouched: same adapters, same registration order.

# Recall on demand: what a real voice session exposed

Three defects only showed up when the system was driven like a real user, not
by fixtures.

## 1. A greeting pulled the whole memory in

`Hola Argos, ¿cómo estás?` answered *"Veo que estás pensando en algo sobre la
basura"*. The structural gate (question mark or four words) is trivially
passed by a greeting **phrased as a question**, and the cosine floor let
everything through: recall injected all 8 stored facts.

Measured against an 8-fact store:

| turn | best cosine | margin over the rest |
| --- | --- | --- |
| ¿a qué hora se riega el jardín? | 0.889 | 0.097 |
| ¿qué le da miedo a luis? | 0.894 | 0.101 |
| ¿qué no le gusta a ana? | 0.901 | 0.095 |
| ¿cuál es la clave del wifi? | 0.862 | 0.076 |
| Hola Argos, ¿cómo estás? | 0.850 | 0.031 |
| gracias | 0.848 | 0.027 |
| ¿qué tal tu día? | 0.823 | 0.013 |

The **absolute** value cannot separate them — a greeting (0.850) outscores a
real question (0.862). What separates them is the shape: smalltalk is
equidistant from everything, a real question makes one fact **stand out**. So
the semantic tier now admits a fact only when it clears the background by
`memory.vector_margin` (0.05), capped at `memory.semantic_max_facts` (2), and
`recall_top_k` dropped 8 → 4.

The background is a **leave-one-out** mean: measuring a candidate against a
mean it is itself inflating tightens the gate as the store shrinks, which cost
two legitimate paraphrases in a 3-fact store (margins 0.041/0.042 against a
0.05 threshold; leave-one-out puts them at 0.061/0.063). With a single stored
fact there is no background at all, so `vector_strict_min_sim` (0.86) takes
over.

Result per turn: greetings, "gracias", "¿qué tal tu día?" and "¿me puedes
contar un chiste?" inject **nothing**; "¿qué no le gusta a Pedro?" and
"¿cuándo trabaja mi hermana?" inject **exactly the one fact asked for**.

## 2. The salience path stored the whole utterance as the fact

"mira, quiero que me recuerdas acerca de que a Pedro no le gusta el pescado"
was stored with that entire sentence as `canonical`, preamble included,
because the salience gate uses the raw turn as its clause. The extraction was
correct (`no le guste` / `el pescado`) — only the canonical was wrong, and it
is what the model reads and what gets embedded.

Without an explicit trigger the canonical is now rebuilt from the extracted
slots (every one is a verbatim span, so it stays natural Spanish):
`a pedro no le gusta el pescado`. Two related fixes: the missing real-speech
trigger variants ("me recuerdas que", "quiero que me recuerdas acerca de
que", the unaccented "recuerdame que", …) are rows in `memory_phrase` — which
also moved that utterance from the model tier to the lexicon, saving it
inline in microseconds — and `stripPunct` now trims whitespace so slots stop
carrying a leading space.

## 3. The probe suite deleted the user's real memories

`--schema-check` ran `clearUserRows(1)`, and user 1 is a **real account**: a
battery run wiped the memories captured by voice, which then looked like a
recall bug in the next session. The probes now own reserved scratch ids
(`kProbeUser = 990001`, `kFixtureUser = 990007`) and every partition literal
is bound from them. New diagnostics for real data, which fixtures cannot
reproduce: `--recall-user <id> "<query>"` (what a given user's turn injects),
`--sim-scan "<query>"` (cosine against every stored fact, with max/mean/
margin) and `--vec-rows <partition>` (vec0 rows plus orphan detection).

Verified after the fixes: 12 suites / 130 asserts green, the real user's facts
survive the battery, and two consecutive live sessions behave as above.

# Talking like a real user: what messy speech broke

The pipeline was tuned on well-formed sentences. Driven with the way people
actually speak — fillers, self-corrections, trailing requests, imprecise
references — it stored garbage. What a single stress session produced:

| stored canonical | should have been |
| --- | --- |
| `argus mira, este, que` | `a mi madre no le gusta el ruido` |
| `mira apuntalo una cosa el sabado viene mi hermana` | `el sabado viene mi hermana` |

The verbatim invariants passed both: **verbatim is not the same as
meaningful**. The fillers *are* literal spans of the utterance.

Four deterministic fixes, all of them rules or rows, none of them a per-case
table:

1. **Fillers are data** (`memory_phrase`, kind `filler`): "oye", "mira",
   "este", "bueno", "pues nada", "a ver", "es que", "una cosa", "por
   cierto"… `RuleParser::stripFillers` removes them from the head of the
   clause before extraction, repeatedly, using the same automaton pass as
   everything else. New filler = new row.
2. **Triggers can close a sentence.** "el sábado viene mi hermana,
   **apúntalo**" is how people ask to remember something. When the trigger
   match ends the utterance, the content is what precedes it.
3. **A filler is never a predicate.** After extraction, a fact whose
   predicate or subject is entirely a filler/trigger/interrogative phrase is
   dropped — that is what stored `predicate = "mira, este,"`.
4. **The personal "a" stays in the canonical.** Slots are verbatim spans, so
   when the source marks the experiencer ("a mi madre no le gusta"), the
   canonical is taken from that marker instead of concatenating slots.
   Without it the assistant said "tu madre no le gusta el ruido".

## Why the assistant invented relatives

Two separate confabulations, two different causes, both in the recall block:

- Quoting the facts as reported speech ("El usuario dijo: ...") made the 1.2B
  invent a narrator for the quote: *"tu mamá me dice que Pedro no le gusta el
  pescado"*, *"tu tío..."*. The framing asked for a speaker, so it produced
  one.
- Leaving the stored first person made it echo the user: *"Mi hermana trabaja
  los sábados"*.

The block now states facts flatly, rewritten to second person at injection
(`toSecondPerson` — a closed grammatical class, not a vocabulary table), with
one short instruction. And it rides at the **tail of the user turn**, not in
the system prompt: with the facts up front, the 1.2B answered from the
*previous* turn's entity ("¿qué no le gusta a Pedro?" → *"tu hermana..."*).
Moving them after the question fixed that and, as a side effect, made the
system prompt constant again, so the prefill prefix stays reusable.

Measured on the same six turns: **4/6 → 6/6**, no echo, no invented narrator.

## The empty-store trap

Asked about a fact whose deferred capture had not landed yet, recall offered
the only vector it had and the model welded them together: *"tu sobrino
estaba asustado porque el router está en el trastero"*. With fewer than two
facts there is no background to measure a margin against, so
`vector_strict_min_sim` (0.88) is the only gate — it is set high on purpose:
on a nearly empty store, a missed recall is cheaper than an invented link.

## Engine verdict: NuExtract-1.5-tiny stays

LFM2-1.2B-Extract was integrated, measured and reverted. On the shared
fixture, after the subject invariant applied to both: scoping 96% vs 86% for
the LFM, but 93% vs 100% recall and **3.0 s vs 1.2 s** per extraction — and
in a live voice turn it fed the assistant enough noise to keep the
confabulation going. The engine is now a config line
(`[extract] model_path` + `prompt_format`), the class is model-agnostic
(`ExtractionService`, dialects `v1.5 | v2 | lfm`), and the guarantees live in
the invariants, not in the model.

Two integration bugs found on the way, both of which would have poisoned any
verdict: `parse_special = false` in the tokenizer (a ChatML model never saw
its control tokens and echoed the input instead of extracting) and a
temporary lab configuration that outlived the run (the "baseline" measured
the other model).

## Tier order: coverage decides, not the clock

The lexicon still runs first, but its answer is provisional: if the extracted
slots account for less than `extract.lexicon_min_coverage` (0.6) of the
clause's words, the utterance carried more than the lexicon understood and the
model tier decides instead. One general measure, not a list of cases.

The escalation is free for the conversation because extraction already runs
off the turn: "mira una cosa, el sábado viene mi hermana, apúntalo" is saved
inline, while "a ver, escucha, es que el router ese lo tengo en el trastero,
apúntalo" is deferred to the model. Both land correct.

`argus-extract-probe --engine-bench [--engine <gguf>[:format]]` reproduces the
engine comparison in one command, over the same fixture, so the next candidate
is measured instead of argued.

# Memory: human-feel + recall reliability round (2026-08-13)

Driven by live user sessions ("suena robótico, a veces no recuerda") and the
TencentDB-Agent-Memory design (plan `MEMORY_HUMAN_PLAN.md`, since deleted —
its tasks all landed). Summary of what
landed and what it measured:

## Fase 1 — turn-level humanization
- Sampling on memory-bearing turns was greedy (`labs.llm.recall_temperature=0`,
  hardcoded in `LfmAdapter` too): the persona flipped between 0.85 and 0.0
  turn by turn. Now 0.5 configurable; `ToolChatInput.temperature` parametrized.
- Canonicals were built from English lexicon predicates ("madre dislikes el
  ruido"). `MemoryFormation` now slices verbatim clause spans (folded view
  with byte map, `memory-formation.cc`) and keeps the possessor/personal "a".
- Capture acknowledgment: a short note rides in the user turn when a capture
  fired; the assistant now confirms naturally ("Sí, he apuntado…") and the
  prompt tells it not to claim saves that did not happen.
- `toSecondPerson` covers me/mí/conmigo/yo (es) and me/I (en), word-boundary
  checked.
- Camera preamble per language; greeting no longer a generic offer; memory
  instructions describe the block's real position (tail of the user turn).
- `trimHistory` strips stale injected context (ack notes + blocks) from
  surviving user messages (only when the prune already broke prefix reuse).

## Fixes the "real user" sessions exposed
- Pre-existing bug: questions without marks ("…como tu reaccionarias") were
  stored as facts. `isQuestion` now also fires on interrogative words with
  <=3 trailing words (bare "que"/"qué" excluded).
- "ok?"/"verdad?" leftovers in canonicals: `stripTrailingConfirmation` strips
  trailing ?¿! before matching tag confirmations. New filler row "ah".
- `parseStatement` keeps a leading "a " ("a mi madre…" stays intact).
- Statement-start coverage: mother/father/cousins/etc, first-person likes
  (es/en) — untriggered "a mi madre no le gusta el ruido" now captures inline.

## Fase 2 — recall
- Episodes are recallable: `memory_episode_fts` tier + vec neighbours resolve
  via `episodeById`; `bumpEpisodeHits`; compaction re-enqueues when the LLM
  is busy instead of dropping the job.
- Semantic gate adapts to store size: 1 distinct fact → `vector_strict_min_sim`,
  2 → `vector_small_store_sim` (0.84, new config key), >=3 → margin over the
  leave-one-out mean (unchanged). Fixes both facts being dropped on small
  stores while keeping smalltalk clean.
- FTS fast path now checks quality: when the top FTS hit shares <2 words with
  the query, the semantic tier still runs (was suppressed by pure hit count).
- `recall_deadline_ms`, `recall_top_k` (limit) and `recall_max_tokens` (char
  budget ≈4×tokens, line-truncated) are wired.
- Anaphora: pronouns (ella/eso/she/he/…) resolve to the last entity of
  `WorkingMemory.activeEntities`; `addresseeEntityId` is resolved per session
  ("yo"/"me" turns anchor the user entity).
- Extract jobs are deduplicated when an identical (user, text) job is queued
  (the fastText false positive on questions no longer burns repeated
  NuExtract runs; the formation question gate rejects them anyway).

## Fase 3 — layered memory (Tencent-inspired, local)
- Vector dedup now merges: the existing fact gets priority/hit_count bumped
  instead of the new row being silently dropped.
- Episode tier scores by salience + hit_count.
- L3 profile: deterministic (persona/preference >= 70, cap 6 rows, second
  person, es/en) appended to the system prompt; cached with
  `profile_stale_seconds`; optional off-turn LLM polish (isBusy gate,
  re-enqueue). Verified live: session 2 greets "¡Buenos días! Tu café sin
  azúcar está bien, y tu hermana viene el sábado".
- Anti feedback-loop: capture rejects text containing `<memor`; injected lines
  are stripped of <> characters; the block keeps the measured imperative
  instruction ("Usa estos datos para responder.") — the softer Tencent-style
  disclaimer made the 1.2B echo the literal `<memorias>` tag.
- Deadlock fixed: `render()` re-locks the graph mutex; vec resolution now
  fetches under the lock and renders after (found via a hang in
  `--vec-gate-test` that only appeared under the real MemoryService path).

## What remains model-limited (honest)
- The 1.2B still sometimes hallucinates answers ("¿y ella trabaja?" →
  "Trabaja, ¿sabes?"), echoes first person occasionally, and ignores injected
  blocks on some turns. The pipeline now delivers the right facts; phrasing
  quality is bounded by the model.
- `--recall-bench` "unrelated query injects nothing" and `--embed-check`
  (ORT pure-virtual crash) fail on THIS machine even on the pre-change tree —
  environment-specific, to revisit.

Validation: dev+prod builds 0/0; 10 memory-probe suites green (vec-gate 3/3,
recall-bench p50 23 ms 30/30 precision, formation/capture/graph-recall incl.
new episode case); live text sessions exercised messy speech, acks, anaphora,
profile and episode recall.

# Model benchmark round: LFM2.5-8B-A1B + cross-family (2026-08-13)

Criterion from the user: candidate must be CPU-optimized, real-time like the
LFM2.5 family, and MUST NOT emit thinking (<think>) — faster replies, no
visible reasoning. The 2.6B was excluded by decision (template hardcodes
`<think>`, verified in its raw chat_template.jinja). Measured on the reference
machine, Release build, CPU-only (gpu_layers=0), medians; `labs/llm-bench`
(--llm/--memory/--fast) + an Ollama-API script for cross-family runs with
official templates/samplers.

## Results

### Our pipeline (LlmService, chatml, 4 decode threads / 8 prefill)

| Model | TTFT short | tok/s short | tok/s xlong | RSS | recall | attrib | no-invent | think |
|---|---|---|---|---|---|---|---|---|
| LFM2.5-1.2B-Instruct (baseline) | 782 ms | 19.9 | 19.1 | 1.21 GB | 7-8/8 | 8/8 | 2/2 | 0/10 |
| LFM2.5-1.2B-Thinking | 1135 ms | — | — | 1.21 GB | 8/8 | 5/8 | 0/2 | 10/10 |
| LFM2.5-8B-A1B Q4_K_M (chatml) | 2628 ms | 25.3 | 17.0 | 5.36 GB | 8/8 | 6/8 | 2/2 | 10/10 |
| LFM2.5-8B-A1B (native tpl + anti-think sys) | 3419 ms | 15.5 | — | 5.36 GB | 8/8 | 6/8 | 1/2 | 10/10 |

### Ollama (official templates/samplers, ~16 threads)

| Model | TTFT short | tok/s | RSS | recall | attrib | no-invent | think |
|---|---|---|---|---|---|---|---|
| lfm2.5:1.2b (calibration) | 752 ms | 40.2 | 1.2 GB | 7/8 | 8/8 | 2/2 | 0/10 |
| qwen3:4b | 1112 ms | 12.7 | 3.1 GB | 7/8 | 8/8 | 2/2 | 0/10 |
| llama3.2:3b | 765 ms | 15.6 | 2.5 GB | 2/8 | 8/8 | 2/2 | 0/10 |
| gemma3:4b | 2954 ms | 12.1 | 3.3 GB | 5/8 | 8/8 | 2/2 | 0/10 |

## Conclusions

- The "8B with 1.5B active" (LFM2.5-8B-A1B) is genuinely fast on CPU (MoE
  works: 25 tok/s short, faster than the 1.2B baseline) and has the best
  recall (8/8), but it THINKS unconditionally — chatml, native template,
  anti-thinking system prompt, and Ollama think:false all still produce
  <think> on 10/10 turns. The reasoning is baked into the model's
  post-training. Per the user's criterion it is rejected; also TTFT ~2.6-3.5 s
  vs 0.8 s baseline because every reply prepends a reasoning block, and
  attribution drops (6/8).
- 1.2B-Thinking data point: thinking at the same size costs +70% TTFT and
  WORSE attribution/no-invention — confirms "thinking not needed here".
- Out-of-family dense models are 2.5-3.3x slower than the LFM2.5-1.2B on the
  same platform (Qwen3-4B 12.7 tok/s, Llama-3.2-3B 15.6, Gemma-3-4b 12.1 vs
  40.2 for lfm2.5:1.2b in Ollama) and worse at the memory task (Llama 2/8,
  Gemma 5/8; Qwen3-4B 7/8 but terse/robotic phrasing).
- Ollama-vs-pipeline gap for the SAME model (40 vs 20 tok/s) is thread count:
  Ollama dedicates all 16 cores; ThreadBudget deliberately reserves decode
  threads (lightThreads = hw/4) for the other AI services. Relative
  model-vs-model ordering holds in both platforms.
- Verdict: keep LFM2.5-1.2B-Instruct. No candidate meets CPU real-time +
  no-thinking + quality simultaneously.
- Note: `labs/llm-bench` gained `--fast` (memory only, 1 round) and think-turn
  detection; `--memory` now also runs the speed table (was an else-if bug).

## Extended sweep (2026-08-13): candidates with natively disableable thinking

User criterion refined: model must be CPU-optimized, real-time, and thinking
must be DISABLEABLE NATIVELY (not prompt tricks). Tested via Ollama (official
templates/samplers, 16 threads) + our pipeline (chatml, 4 decode threads).

| Model (Ollama) | TTFT short | tok/s | recall | attrib | no-invent | think | RSS |
|---|---|---|---|---|---|---|---|
| lfm2.5:1.2b (champion) | 752 ms | 40.2 | 7/8 | 8/8 | 2/2 | 0/10 | 1.2 GB |
| qwen3:1.7b | 444 ms | 20.7 | 7/8 | 8/8 | 2/2 | 0/10 | 0.3 GB |
| qwen3:4b | 1112 ms | 12.7 | 7/8 | 8/8 | 2/2 | 0/10 | 3.1 GB |
| qwen3:0.6b | 142 ms | 50.5 | 5/8 | 8/8 | 2/2 | 0/10 | 0.5 GB |
| granite4:7b-a1b-h (Granite-4.0-H-Tiny, MoE 7B/1B) | 1787 ms | 21.9 | 8/8 | 8/8 | **0/2** | 0/10 | 4.4 GB |
| granite4:1b-h | 1672 ms | 14.8 | 7/8 | 7/8 | 2/2 | 0/10 | ~1.6 GB |
| llama3.2:3b | 765 ms | 15.6 | 2/8 | 8/8 | 2/2 | 0/10 | 2.5 GB |
| gemma3:4b | 2954 ms | 12.1 | 5/8 | 8/8 | 2/2 | 0/10 | 3.3 GB |
| qwen3.5:2b | — | 12.1 | 0/8 | — | — | 0/10 | — (empty replies) |

Key findings:
- Granite-4.0-H-Tiny (7B/A1B MoE) is natively non-thinking and has the best
  recall (8/8) BUT fails the no-invention gate (0/2): answers "Sí" to
  questions the memory cannot answer and quotes the facts verbatim —
  unusable for a voice assistant that must say "no lo sé". Verbose, 4.4 GB.
- Qwen3 family disables thinking NATIVELY only through its jinja template
  (enable_thinking=False). In OUR pipeline (chatml, no jinja) qwen3:1.7b
  thinks 10/10, recall drops to 6/8, RSS 4.7 GB, 15.9 tok/s — worse than the
  champion in every dimension. Ollama numbers (0 think, 7/8, 444 ms TTFT)
  do not transfer to Argus without template support in LlmService.
- qwen3:0.6b is the speed king (50 tok/s, 142 ms TTFT) but recall 5/8.
- The champion LFM2.5-1.2B-Instruct still wins the combined criterion
  (quality + CPU real-time + zero thinking + 1.2 GB). No tested model
  replaces it. Models in models/llm/bench/ kept for future re-eval.

# Model benchmark round: LFM2-8B-A1B + LFM2-2.6B-Exp (2026-08-14)

New-generation LFM2 (NOT 2.5) candidates, both Q4_K_M, same fixture as the
2026-08-13 round (10-case memory battery via `labs/llm-bench` + an Ollama-API
script at `/tmp/opencode/ollama-bench/bench.py`), same-day baselines included.
Both templates are pure ChatML — **the `<think>` defect of LFM2.5-8B-A1B is
gone (0/10 both platforms, both models)**. That was the criterion that killed
the 8B last round; it is now fixed.

### Ollama (CPU, ~16 threads, defaults)

| Model | TTFT | tok/s | recall | attrib | no-invent | think | RSS |
|---|---|---|---|---|---|---|---|
| lfm2.5:1.2b (champion, re-run) | 795 ms | 41.1 | 8/8 | 8/8 | 2/2 | 0/10 | 1.32 GB |
| LFM2-2.6B-Exp Q4_K_M | 1832 ms | 21.3 | 8/8 | 7/8 | 1/2 | 0/10 | 1.79 GB |
| LFM2-8B-A1B Q4_K_M | 1513 ms | 32.7 | 8/8 | 7/8 | 0/2 | 0/10 | 5.15 GB |

### Pipeline Argus (llama.cpp b10305, Vulkan offload, 4 decode / 8 prefill)

| Model | TTFT short | tok/s | recall | attrib | no-invent | think | RSS | echo |
|---|---|---|---|---|---|---|---|---|
| LFM2.5-1.2B (champion) | 287 ms | 44.0 | 7/8 | 7/8 | 2/2 | 0/10 | 0.83 GB | 0/10 |
| LFM2-2.6B-Exp Q4_K_M | 613 ms | 19.5 | 8/8 | 8/8 | 2/2 | 0/10 | 1.68 GB | **10/10** |
| LFM2-8B-A1B Q4_K_M | 1151 ms | 29.0 | 7/8 | 8/8 | 0/2 | 0/10 | 4.85 GB | 3-4/10 |

Findings:
- **`lfm2_moe` loads fine on llama.cpp b10305** (no bump needed), init 3.7 s
  (8B) vs 1.6 s (1.2B) vs 2.0 s (2.6B).
- **LFM2-2.6B-Exp**: best recall/attribution of the round (8/8, 8/8 in the
  pipeline) and no invention there, but it **echoes the `<memorias>` block
  verbatim on 10/10 turns** (the Ollama battery 6/10) — in a voice assistant
  the speaker would read the injected memories aloud. It is also 2.2x slower
  than the champion (19.5 vs 44 tok/s), 2x TTFT and +0.85 GB RSS. Rejected.
- **LFM2-8B-A1B**: MoE works (29 tok/s beats the 2.6B dense despite 3x params)
  but still 1.5x slower than the champion, TTFT 4x (1151 ms), RSS 4.85 GB
  (5.8x), recall 7/8, and **no-invent 0/2 in both platforms** (invents an
  allergy link on "¿tengo alguna alergia registrada?" and echoes the block).
  The liquid-recommended samplers (temp 0.3/min_p 0.15/rep 1.05) fix the gate
  but case 5 then answers with a hallucination the fixture cannot catch
  ("a Marta le encanta el chocolate caliente") — the no-invent weakness is
  real either way. Rejected.
- **Verdict: champion stays LFM2.5-1.2B-Instruct.** LFM2 fixes thinking but
  regresses speed, RSS and no-invention. GGUFs kept in `models/llm/bench/`
  for future re-eval (Ollama tags:
  `hf.co/LiquidAI/LFM2-8B-A1B-GGUF:Q4_K_M`,
  `hf.co/LiquidAI/LFM2-2.6B-Exp-GGUF:Q4_K_M`).
- Note: pipeline champion today (44 tok/s, 287 ms TTFT) beats the 2026-08-13
  CPU-only rows (20 tok/s) because `HardwareProbe` now offloads to the RADV
  iGPU by default (`gpu_layers=999`); same-day runs are the only valid
  comparisons.

# Vocabulary moved to static constants (2026-08-13)

`memory_phrase` and `memory_lexicon` (schema.sql inserts, ~470 rows) were the
only static-data tables. Per user decision they are GONE — no migration, no
DROP (dev: DB deleted by hand; the code never reads them anymore):

- `src/shared/vocabulary/vocabulary-types.hxx` — `PhraseSeed`/`LexiconSeed`.
- `vocabulary-es.hxx` / `vocabulary-en.hxx` — `inline constexpr std::array`
  generated from the live DB (291 phrases: 219 es + 72 en; 179 lexicon:
  96 es + 83 en), zero allocations at init (`std::span` accessors).
- `vocabulary.hxx` — accessors + `allLexiconEntries()` (→ `TieredExtractor`).
- `PhraseCatalog` no longer touches SQLite (no `SqliteGraph`/repo members);
  `MemoryService::init()` builds phrases + lexicon from constants directly.
- Deleted `src/shared/repositories/memory-phrase/` and `memory-lexicon/`
  (their add/remove methods had zero callers); labs CMakeLists updated.
- `database/schema.sql` ends at the `job` table; no phrase/lexicon DDL.
- New probe gate: `argus-memory-probe --vocabulary-check` (duplicates, kind
  coverage es/en, automaton count). es must cover all 6 phrase kinds; en has
  no recall_marker rows by design (covers 5).
- Adding a phrase/language = editing the language header, rebuilding. No DB
  migration, no boot-time INSERTs, no WAL growth from static seeds.

## Vocabulary extension (2026-08-13)

Vocabulary grew from 291 → 1243 phrases (762 es / 481 en) and 179 → 870
lexicon entries (468 es / 402 en), covering: voseo triggers ("recordá que",
"acordate que", "apuntá que"), mamá/papá + extended kinship (in-laws,
step-family, godparents, twins), pets, ~60 house objects, first-person
preferences ("me encantan", "i can't stand", "i'm allergic to"), ~90 new es
predicates (trabaja en, se levanta a las, le encanta, cumple años el, tiene
que ir, …) and ~130 en (wakes up at, is planning to, was born in, …), en
recall markers (previously absent: "i don't remember", "remind me what",
"what did we talk about", …), interrogatives (adónde, how much, whose, …)
and safe stopword sets. Zero duplicates: the dev DB's unique
(kind, lang, phrase/surface) index + `--vocabulary-check` guarantee it.

Generation pipeline: `extend-vocab.py` (INSERT OR IGNORE into the dev DB
tables, which still exist locally) → `gen-vocab.py` → regenerates the
headers. schema.sql was NOT touched in this round (its table declaration
order is preserved; vocabulary lives only in src/shared/vocabulary/).

# Normal dialogue no longer depends on tool-calling (2026-08-20)

`CLAUDE_PLAN.md` asked for the conversational path to answer, recall and
capture memory **without the model emitting tool calls**, while keeping the
tool infrastructure available for probes and explicit routes. Alternative 5.2
of the plan was implemented (rules + fastText hint + off-turn formation);
5.1 (rules only) survives as the fallback whenever fastText is missing. A
separate LLM router (5.3) and model-driven tool-calling (5.4) were rejected:
one adds an inference per turn, the other adds ~300 tokens of schema and up
to four generations per turn for no measured gain.

## The layering that actually decides

    prediction  ->  policy  ->  persistence  ->  telemetry  ->  training

A prediction is not a policy. fastText is a **hint**; the deterministic
`RuleParser` barrier inside `MemoryService` is what decides. STT output has no
accents and no punctuation, so the classifier will keep mislabelling
retractions and recall questions — that is expected and does not matter as
long as nothing it mislabels reaches formation.

Cascade for implicit capture:

    RuleParser
      trigger explícito           -> captureExplicit (store or defer)
      pregunta / recall / retracto -> reject, nothing queued
      posible declaración          -> fastText (winning class + margin)
                                        -> captureImplicit -> idle queue

## Measured (2026-08-20, same machine, no other inference running)

| Metric | Before | After |
|---|---:|---:|
| generations per turn (normal path) | 1..4 (`maxToolHops`) | 1 |
| tool schema in the system prompt | 993 bytes = **276 tokens** | 0 |
| system prompt size | 338 tok | 62 tok |
| TTFT, cold prefill (median of 7) | **1862 ms** | **565 ms** |
| fastText false `memory_save` on the negative set | 7 | 7 (unchanged) |
| …of those reaching memory formation | 7 | **0** |
| real memories wrongly blocked | — | 0 |
| implicit statements still deferred | — | 3/3 |
| `--intent-check` at production thresholds | 103/103 | 103/103 |
| `--intent-eval` top-1 (valid.tsv) | 0.966 | 0.966 |
| memory-probe battery | 3 failures | 143 ok / 0 fail |

`argus-memory-probe --ttft-bench 7` measured both prompts interleaved on the
same box, same user turn, `resetContext = true`, nothing else inferring:
the tool schema costs **276 tokens and 1297 ms of prefill (3.3x TTFT)**.
Caveat: `resetContext = true` measures the *cold* prefill. In a live session
the constant system prefix is KV-cached, so only the first turn pays the full
1297 ms — but every turn paid the 276 tokens of context, and any prompt that
diverges from the cached prefix forces the reset again. The schema also
invited the model to emit tool calls, which cost up to three extra
generations per turn on top of this.

`argus-memory-probe --compaction-test` runs the real LFM2.5: a session of
greetings, sums, a time question and `no, olvídalo` produces **0 episodes**,
while a session carrying two durable statements produces exactly **1**, whose
summary keeps `hermana`/`alérgico` and drops the arithmetic turn.

Finding from that test: `durableTranscript()` first kept `user: hola argus`,
so the small-talk session still became an episode. Greetings are now dropped
too — a user turn that `stripFillers()` reduces to nothing carries no more
episode material than a question does.

## Two pre-existing bugs the plan's test cases exposed

1. **`PhraseAutomaton` dropped every phrase with an accented edge.** The
   span-edge check used `isWordChar` (`a-z0-9_` only), so a pattern whose
   first or last byte is a UTF-8 lead/continuation byte never matched. 38
   Spanish entries were dead, including the interrogatives `qué` and
   `por qué`, the kinship terms `mamá`/`papá`, the predicate `está` and the
   recall markers `dónde está`/`qué pasó`. Fixed with `isSpanEdge` (accepts
   bytes >= 0x80) applied **only** to the span edges — the surrounding-byte
   checks stay ASCII-only so `¿` and `?` keep separating words.
2. **`isQuestion` read the `que` of `recuerda que` as interrogative.** The
   existing `word != "que"` exemption only applied when the word had 3+ words
   before it, so every explicit trigger was classified as a question and its
   turn was dropped from compaction. Fixed by skipping interrogative hits
   contained in a longer `Trigger` span.

## `MemoryService::init()` and the sqlite ordering

`registry_.initialize()` runs **before** `app().run()`, and drogon calls
`sqlite3_config(SQLITE_CONFIG_MULTITHREAD)` there — it logs a FATAL if
sqlite3 was already initialized. Deferring the store to
`registerBeginningAdvice` fixed the server but silently broke every lab: labs
never call `app().run()`, so the graph never opened and the worker never
started (`--schema-check` failed 3 checks, voice-test had no memory at all).
Now `init(const MemoryInitOptions&)` takes `deferStore`: the
`MemoryServiceAdapter` passes `{.deferStore = true}`, labs get the store
straight from `init()`, and `openStore()` is idempotent either way.

## Capture contract

- `captureExplicit` — trigger or statement recognized by rules. Unchanged.
- `captureImplicit` — rejects invalid user, empty text, questions, recall
  markers and retractions (new `PhraseKind::Cancellation`, 30 es + 26 en
  seeds) **before** anything is queued, and marks the work `preferIdle` so
  `processExtract` waits for the LLM to go quiet (`memory.extract_wait_ms`)
  and re-enqueues instead of competing with the spoken answer.
- `Rejected` now really means nothing was queued and nothing persisted;
  voice-test only prints `[memory] queued by intent` after the service
  accepts, not after the fastText prediction.
- `durableTranscript()` (public, pure) drops transient user turns and the
  assistant answer bound to them. Both `enqueueSummary()` and
  `enqueueCompaction()` go through it, so a session of greetings, sums and
  retractions produces no episode at all.

## `IntentService`: winning class + margin

`match()` used to discard `none` as `ToolIntent::Unknown`, so `fired()`
compared `memory_save` against a threshold and nothing else. The shipped model
is softmax so the hole was latent, but `--intent-train` defaulted to **ova**,
where independent sigmoids make `memory_save 0.95` with `none 0.98` perfectly
possible. Now `ToolIntent::None` exists, `fired()` requires the intent to be
the winning class, clear its threshold **and** beat the runner-up by
`intent.margin` (0.20), and the probe's default loss is softmax to match the
shipped model. Thresholds were **not** lowered — `--intent-check` suggests
0.35/0.40 but 0.60 stays, because precision beats recall for `memory_save`: a
false positive contaminates future facts, a false negative is recoverable
with an explicit trigger.

## `usage.tsv` is telemetry, never labels

`--intent-train --include-usage` now reads `usage-curated.tsv` and fails loudly
if it is missing. `--curate-usage` produces it by running the same
deterministic barrier the runtime uses, then deduplicating. On the 92 historic
rows: **29 accepted, 11 duplicates, 52 rejected** (39 question/recall, 7 no
durable statement, 6 retraction). Every `camera` row was rejected — they were
all `¿dónde está mi coche?`-style recall questions the model had labelled
camera. `train.tsv` gained 126 curated negatives (calculation, time, date,
weather, recall, retraction, small talk, lookup) and
`labs/intent-data/negatives.tsv` is the regression set for
`--intent-test` / `--intent-barrier-test`.

Retraining was measured and **not promoted**: the side-by-side model scored
38/47 on the negative set against the shipped model's 39/47 and still put
`olvidalo` at 0.99. The corpus is not the lever — the barrier is. The
side-by-side artifacts were removed; `models/intent/argus-intent.ftz` is
untouched.

## Pending / deliberately not done

- **`VoiceSessionService` is untouched.** It still has no `MemoryService` and
  no `ConversationService`. Plan §Fase 7 lists eight open decisions first
  (injection, per-session `WorkingMemory`, authenticated `userId`, per-session
  language, capture notification, WebSocket cancellation, mutex/slot
  ownership, WS tests). Integrating it is a design change, not a port of the
  lab.
- `ConversationService::processTurn` has **no callers yet** — voice-test still
  runs its own loops and only uses `recallBlock`/`trimHistory`. It is correct
  and compiled, but the lab loops are what is actually exercised.
- The conservative `isQuestion` costs recall on unaccented implicit
  statements: `pues nada, que a mi sobrino le da miedo la oscuridad` is read
  as a question because of the bare `que`. Explicit triggers still capture it
  (`parse()` matches), and the plan's stated priority is precision over
  recall for `memory_save`, so this stands as a known trade-off rather than a
  fix.
- `parseStatement()` still stores patterns like `la alarma se activa a las 10`
  automatically. Whether ambiguous statements should need confirmation is an
  open product decision; `--capture-test` currently asserts the old behaviour.
- `multilingual-e5-small` as a **second opinion on ambiguous cases only** (not
  per turn) is the next step if precision needs more, before any LLM router.

# Reactions: Argus reacts to the turn, without the model deciding (2026-08-20)

The assistant now derives a **semantic reaction** from each turn and sends it to
the client, which plays it on the avatar's face and lets the prosody of the
spoken answer animate it. Two layers, deliberately orthogonal: the reaction
picks the pose, the voice envelope gives it life. Neither knows about the other,
so each is testable alone.

## Why it is not a model

The dialogue must not emit tags or JSON (see the tool-calling section above), so
`<emotion>joy</emotion>` was never an option, and a second LLM pass would cost
another generation per turn against a 565 ms TTFT. Every reaction instead comes
from a signal the pipeline already produced:

    prediction -> policy -> reaction

`ReactionEngine::react()` is a pure function over `ReactionSignals`; resolution
order **is** priority, so an alarm can never be buried under "thinking":

| # | signal | reaction | `because` |
|--:|---|---|---|
| 1 | system alert | `alarmed` | `system_alert` |
| 2 | STT failed / empty | `confused` | `stt_failed` |
| 3 | `RuleParser::isCancellation` | `acknowledging` | `retraction` |
| 4 | recall hits > 0 | `recognizing` | `recall_hit` |
| 5 | capture stored | `attentive` | `capture_stored` |
| 6 | capture queued | `attentive` | `capture_queued` |
| 7 | camera intent | `curious` | `camera_intent` |
| 8 | question + recall empty | `uncertain` | `recall_empty` |
| 9 | question | `thinking` | `question` |
| 10 | filler-only turn | `warm` | `small_talk` |
| 11 | — | `idle` | `no_signal` |

`recallHits < 0` means recall never ran, which is **not** the same as ran and
came back empty — rule 8 only fires on a real miss. Intensity moves with the
evidence (1 hit → 0.65, 3 hits → 0.95; an alarm is always 1.0) instead of being
a constant per kind.

Cost: the engine is a few `RuleParser` predicates over an Aho-Corasick automaton
whose `match()` measures **p95 5.2 µs at 10 000 patterns** (`extract-probe`
gates), ~137 KB for its own catalogue, and zero new models. It is self-contained
precisely so the lab and the WebSocket path can both use it without dragging
`MemoryService` in.

## The wire carries meaning, not appearance

    { "type": "voice:event",
      "payload": { "reaction": "recognizing", "intensity": 0.7,
                   "because": "recall_hit" } }

The backend never names an avatar expression. `REACTION_SEMANTIC_KEY` in the
frontend maps the reaction to a calibrated pose, so the face can be redesigned
without touching C++ or versioning the socket. `because` is not decoration: it
is how you find out why the face did something odd, and it is the reason this
needs no model.

## Tone rides the user turn, never the system prompt

The reaction also conditions **how** Argus answers (`ReactionEngine::toneNote`).
That note is appended to the tail of the request's last user message — not to
the system prompt, which has to stay byte-identical for KV-cache reuse, and not
to the stored history, where a note from three turns ago would keep steering the
answer. Only the kinds that change the answer carry one: `idle` and `thinking`
are silent, because `thinking` is the common case and every turn would pay
tokens for nothing.

`voice:event` is emitted **before the first token**, so the face reacts while
the model is still generating rather than after it speaks.

## Where the signals come from today

| path | signals |
|---|---|
| `labs/voice-test` (text + voice loops) | all of them — capture outcome, recall block size, camera intent |
| `VoiceSessionService` (production WS) | turn shape, retraction, STT failure |

The WebSocket path starts with 6 of the 10 reactions on real signals.
`recognizing`, `attentive`, `curious` and `uncertain` need `MemoryService`,
which per Fase 7 of the plan is not wired there yet — when it lands they light
up **without a contract change**, because the payload already carries them.

## Per-frame liveliness is computed on the client, on purpose

Streaming a pose per frame would be ~60 messages/second. Instead: the client
already receives the TTS PCM and buffers the whole utterance before playing it,
so it computes the RMS envelope once (`pcmEnvelope`, one pass over memory it
already holds) and walks it in step with playback at ~30 Hz into a single
reanimated shared value. The render worklet reads that value — no network, no
model, nothing per frame beyond a read.

Tests: `labs/reaction-probe --reaction-test` (23 checks) covers the full
priority ladder in es/en, the `recallHits < 0` distinction, intensity growth,
and which kinds may carry a tone note.

## People, access, invitations and private files (2026-08-22)

The people domain is implemented as a local-first, role-scoped system. It must
remain usable on a LAN with no third-party service; a tunnel only changes how a
device reaches this same backend.

### Role projection and real-time changes

- `role-access.hxx` remains the authority for HTTP and sync permissions. It
  maps `/user`, `/invitation` and `/portrait-preview` in addition to the
  existing resource paths.
- `SynchronizedService` scopes **user rows** deliberately: Owner and Guard get
  the people directory; Resident and Guest receive only their own user row.
  Owner alone receives `user_invitation` metadata. A permitted HTTP route still
  applies the same scope in `UserFeatureService::list`.
- Invitation and portrait capability records are never sync payloads. The
  invitation metadata that is syncable excludes the token hash.
- `SyncOperation::AuthContextChanged` (`auth_context_changed`) is emitted after
  a role change. The sequence is: persist user →
  `SocketService::replaceRoleRooms({ userId, oldRole, newRole })` → emit to the
  existing user room with `resync=true`. The connection remains alive; its old
  module rooms are replaced by the new ones. Deactivation is different: refresh
  tokens are invalidated and `disconnectUser` closes the connection.
- Sync bootstraps user/invitation rows once and subsequently pages new rows by
  `created_at`. User edits, activation/role changes, invitation revocations and
  redemption effects are published through the appropriate global or user audit
  log, so an offline device catches up via its bounded audit cursor instead of
  relying on a transient live event.

### Invitation lifecycle

- Persistent tables: `user_invitation` (opaque SHA-256 token hash, non-owner
  assigned role, capacity, expiry/revoke state) and `user_invitation_redemption`
  (one redemption per enrolled user). Repositories live in
  `src/shared/repositories/user-invitation/`.
- `POST /invitation` and `GET /invitation` are Owner operations;
  `DELETE /invitation/{id}` revokes. `POST /invitation/resolve` is the
  pre-auth pairing/enrollment resolver: it returns only role, expiry and pinned
  Argus identity/certificate material, never the stored token.
- `/auth/register` permits the first Owner only on an empty database. Every
  subsequent new face enrollment requires a valid invitation and atomically
  creates the user/person/embedding, consumes capacity and records redemption.
  A face already known issues that person's session without consuming an invite.
- The frontend QR preview is intentionally single-display. Closing it or
  leaving the owner screen revokes the pending invitation; no QR token is
  persisted or recoverable from its synced metadata.
- User/invitation create, update, revoke, redemption and enrollment actions
  append safe records to `user_action_log`. Never store raw QR/token/certificate
  secrets in a log.

### Portrait storage and audited verification

- Face enrollment produces a private portrait object plus `stored_file` and
  `user_portrait` metadata. These private file tables are not in `TableName` or
  the sync surface. Clients receive neither object key nor S3 credential.
- `PrivatePortraitService` and `S3StorageService` are the only storage facade.
  `GET /portrait-preview/{userId}` mints a requester-bound, 60-second capability
  for Owner/Guard; `GET /portrait-preview/{token}/content` atomically consumes
  it before reading the private object. A failed retrieval still consumes the
  capability, so retry requires a new explicit request.
- The consumed image is returned as bytes encoded by the API, never as a bucket
  URL. Successful reads append `UserAction::Read` with the safe event
  `portrait_preview`; image content, object keys, token and credentials never
  enter the audit log.

### Local RustFS deployment

- `docker-compose.yml` defaults to a loopback-only RustFS stack
  (`127.0.0.1:9000`) plus an initializer. It is intentionally independent from
  native backend development so active emulators do not pay its memory cost.
- `scripts/setup.sh --storage-only` generates the ignored 0600 `config.toml`
  from `config.toml.example`, derives the runtime secret files under ignored
  `docker/runtime/`, and starts the initializer. The initializer creates a
  bucket-scoped application account; RustFS root credentials are not mounted
  into the backend. There is no `config.local.toml` overlay.
- The optional backend Docker profile runs with host UID/GID and refuses to
  create missing bind-mount paths, avoiding root-owned development files. No
  Docker build, pull or startup is part of normal feature validation.

### Verification kept with the feature

The backend keeps only operational scripts under `scripts/`; feature checks
are performed through the configured build and manual integration flows. The
2026-08-23 sync/audit implementation also built successfully with
`cmake --build build/dev --target argus-backend -j 2`; `git diff --check` passed
afterward.

## Sync/audit resync (2026-08-23)

- The regular sync stream now bootstraps authorized rows, then advances only by
  `created_at` for creations and id-only deletions. Its update channel is the
  bounded, id-based audit stream rather than a second `updated_at` query.
- `SynchronizedService` supports `{findLast:true}` plus
  `afterId`/`endId` for both audit scopes. The returned watermark makes a finite
  catch-up interval; clients advance their persisted cursor only after applying
  each page.
- `SyncAuditService` centralizes global/user diff publication. Feature updates
  capture before/after data; camera/zone, calendar/projects and user/invitation
  paths now publish only changed fields, while creates remain `Add` and deletes
  are id-only. Notification reads use the same user-audit path.
- Daily audit compaction retains the composed field diff and gives its replacement
  a new monotonic id, preserving convergence for reconnecting clients.

## Main LLM artifact update (2026-08-23)

The active conversation model is now
`LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf` from
[LiquidAI/LFM2.5-1.2B-Instruct-GGUF](https://huggingface.co/LiquidAI/LFM2.5-1.2B-Instruct-GGUF/blob/main/LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf).
The file is 696 MB and its pinned SHA-256 is
`bb741ebb106d543e9de114b843a3d3d73d51c74b5801e69da2abde821a0cb3e1`.

- `config.toml` and the `LlmService` fallback point to the QAD Q4_0 filename.
- `scripts/setup.sh` downloads it to a `.part` file, verifies the checksum and
  moves it into `models/llm/` only after verification. Existing mismatched
  files are replaced; valid files are reused without another download.
- The model keeps the existing llama.cpp runtime, ChatML prompt path and
  LFM Open License 1.0. The repository does not claim a quality improvement
  without a new benchmark; the model swap is tracked as an artifact change.

## Camera lab real-time face detection (2026-09-02)

`labs/camera-control` gained a live face-detection overlay that reuses the
production face stack end to end: `FaceService` (ncnn RetinaFace + ArcFace)
plus `FaceDB`/sqlite-vec for identification against enrolled people. There is
no YOLO anywhere in the project; the detector is RetinaFace.

### FaceService refactor (production code)

- `extract()` used to compute all candidate boxes internally and throw away
  every one but the best. The detector pass now lives in a private
  `runDetector()`, exposed as `detectAll(rgb, w, h)` and
  `extractFace(rgb, w, h, box)`; `extract()` is `runDetector` + best box +
  `extractFace`, so behavior is unchanged and nothing is duplicated.
- While fixing the lab it turned out the RetinaFace decode itself was wrong:
  the score was read from channel 0 of
  `face_rpn_cls_prob_reshape_strideN`, which is the **background** probability
  of anchor 0 (the blob has 4 channels: [bg0, bg1, face0, face1]). With the
  correct channel (`2 + a`) plus the official Tencent/ncnn center-size decode
  (center = anchor center + anchor_size * delta, size = anchor_size *
  exp(delta), landmarks scaled by anchor_size + 1), the detector finally
  produces real boxes. Before this fix the whole face pipeline had silently
  never worked: zero persons, zero embeddings in the database.

### Lab detection engine

- Frames come from a persistent `ffmpeg` subprocess piping rawvideo BGR
  (`-vf fps=<target>,scale=1280:720`) from the go2rtc RTSP relay on port 8555,
  not from `GET /api/frame.jpeg`. One frame.jpeg request takes 0.7–2 s, which
  capped the loop at ~1 fps; the pipe delivers exact 1280×720×3-byte frames at
  the configured `target_fps`. If the pipe dies the loop reopens it after
  500 ms.
- The tick loop detects at `target_fps`, normalizes boxes to 0..1 and pushes
  them over the WebSocket `/api/detections/ws`; `GET /api/detections` is the
  HTTP snapshot with capture history, and the frontend falls back to polling
  if the socket fails.
- Captures gate on `min_score`, a `capture_cooldown_ms` cooldown and IoU < 0.55
  against the previous capture, so one visit produces one crop, not a burst.
  The crop is a JPEG data-URI on the tick/capture payloads.
- Identification runs `extractFace` on the best box and `FaceDB::search`
  (cosine, gate 0.80) only when `[faces] identify` is on and embeddings exist.
- Enrollment is a single sqlite transaction (person → face_embedding) followed
  by the vec0 insert whose rowid equals `face_embedding.id`, so HTTP snapshots
  and vec searches can never disagree about what exists.

### Config and hardware

- The lab section is `[faces]`, not `[detection]`: that section already means
  motion detection. `LabConfig::save()` writes a fixed TOML template, so
  `[faces]` exists in `load`/`toJson`/`save` or a save would wipe it. The UI
  toggle persists `enabled` back to config.toml so the engine restarts on the
  next lab launch.
- Vulkan is gated on `HardwareProbe::get().vulkanDiscrete`: ncnn classifies
  the RADV RENOIR iGPU as non-discrete, and on it Vulkan inference measured
  1228 ms/frame versus 78 ms on CPU (15× slower), so integrated GPUs run CPU
  threads while discrete GPUs keep the Vulkan path.

## Camera lab talk + alarm fixes (2026-09-03)

Both features were wired end to end in the lab but never actually worked
against the C225. Two independent root causes:

### Talk: digest username on the media port

`TapoTalkClient` was given the TP-Link cloud account name (`david.acme26`) as
the digest username for port 8800 and the camera answered 401 to every
password variant (plain, md5, sha256). The only implementation that ever
worked, `labs/voice-test`, hardcodes `username = "admin"` with the cloud
password; the camera's media digest expects that user. The lab now sends
`admin`. Production `TapoDriver::speak()` still falls back to the cloud
account name and is very likely broken the same way — it was left unchanged
only in the sense that nobody has exercised it; align it when the talk path
is wired into the backend feature.

### Alarm: only one write path exists on this firmware

The C225 rejects every plausible alternative — each was probed against the
live device:

- `{"method":"do","params":{"msg_alarm":{"manual_msg_alarm":{"action":…}}}}`
  (pytapo `startManualAlarm`) → **-40210 "Function not supported"**. There is
  no manual one-shot siren request on this firmware, so an alarm button can
  only arm/disarm, not fire the siren on demand.
- `set` on `msg_alarm.chn1_msg_alarm_info` (partial or full mirror) → -40210.
- `setAlertConfig` + `manual_msg_alarm` → -40101 (not part of the schema).

The only accepted write is `setAlertConfig` with the **full**
`msg_alarm.chn1_msg_alarm_info` table, obtained via read-modify-write: read
with `getLastAlarmInfo` (`{"msg_alarm":{"name":["chn1_msg_alarm_info"]}}`,
unwrap `result.responses[0].result.msg_alarm.chn1_msg_alarm_info`), mutate one
field, write back the whole table with `setAlertConfig`. Verified inner
`error_code: 0` for `enabled` on/off and for every volume level. On the C225
that table is:

```
alarm_duration = "0",  alarm_mode = ["light","sound"],  alarm_type = "0",
alarm_volume = "high",  enabled = "off",
light_alarm_enabled = "on",  light_type = "1",  sound_alarm_enabled = "on"
```

`alarm_volume` is a text level ("low"/"normal"/"high"), not a number — "80"
and "medium"/"mid"/"mute" all answer -40101; only low/normal/high are in the
vocabulary. `TapoApi::updateAlarmTable` implements the mirror and both
`setAlarm` (enabled) and `setAlarmVolume` (alarm_volume) go through it.
Numeric inputs (1–100, both in the lab HTTP API and in `TapoDriver`) map to
those levels at the boundary. `getAlertConfig` with empty params answers OK
but returns an empty result on this firmware; the useful getter is
`getLastAlarmInfo`. The response envelope reports `error_code: 0` even when
the inner per-response `error_code` is -40101/-40210, so both the lab and
`updateAlarmTable` unwrap `result.responses[].error_code` before reporting
success.

### Zones: camera frame behind the polygon canvas

`POST /api/zones` had no visual reference — the canvas drew on a dark
background, so risk polygons were blind guesses. The lab now exposes
`GET /api/snapshot` (JPEG from the detection engine's last frame, falling
back to a one-shot `ffmpeg` grab from the go2rtc relay) and the zone canvas
draws that frame as its background with an "Actualizar imagen" refresh
button, so polygons are traced over the real scene the detector sees.

## Golden /sync frames and first-bootstrap crash fix (2026-09-04)

The migration freezes the `/sync` WS contract with golden frames recorded by
`src/test/e2e/golden-sync-test.cc` (plain `main()`, Drogon WebSocketClient +
HttpClient; no doctest). Without `ARGUS_TEST_REFRESH_TOKEN` or a reachable
backend it prints SKIP and exits 0, so CI stays green. With a session it
rotates the token over the real `PATCH /auth/refresh-token` (Drogon's
HttpClient stamps its own default user agent over the request header, so the
recorder calls `setUserAgent` — the refresh endpoint rejects UA mismatches),
plays the frontend bootstrap (all 15 tables, audit watermark + page for both
scopes, `camera:subscribe` → `camera:ready`/`camera:closed`,
unknown-type error probe) and records every frame into
`src/test/fixtures/sync/` (`<scenario>.json` normalized + `.raw.json` +
`manifest.json` with sha256/byteLength/hex256). A later run verifies the new
session against the fixtures structurally: ids, timestamps and secret-ish
values are masked (rules live in the manifest), binary frames compare as kind
only.

Recording procedure (local): copy `build/dev/database/argus.db` + certs into a
sandbox dir, write a test `config.toml` there (test JWT/fingerprint secrets,
`pairing.paired=true`, mdns off), symlink `models/`, seed one owner user +
`refresh_token` row (device hash = HMAC-SHA256 of `UA|127.0.0.1` with
`device.fingerprint_secret`), run the `argus-backend` binary with the sandbox
as cwd, then run the recorder with `ARGUS_TEST_BASE_URL` +
`ARGUS_TEST_REFRESH_TOKEN`. The real production DB/config are never touched.

The recorder found a Phase 0 blocker: `UserRepository::findLast` /
`findLastDeleted` used `filter.userId ? co_await ...(*filter.userId) : ...`.
GCC's coroutine lowering of a conditional operator with `co_await` branches
dereferences the optional in the resumed state before testing the condition
(confirmed in the disassembly: `optional::operator*` runs unconditionally in
state 2, the condition test happens later in state 6), so every Owner
bootstrap with `findLastCreated` on the `user` table (not a personal table →
`userId` disengaged) aborted the process with the
`optional::_M_is_engaged` assert. Both functions now keep the dereference
inside the taken branch. The same ternary-with-co_await shape must not be
reintroduced anywhere.

## NATS event bus foundation (2026-09-04)

Fase 1 of the migration starts here: the gateway will own `/sync`, and every
other service publishes persisted-change events to NATS instead of calling the
gateway. The foundation is `NatsBus` (`src/shared/wrapper/nats/`) over cnats
(`cnats/3.13.0` via Conan; its `nats_static` target is linked PUBLIC into
`argus_common`). The frozen subject naming lives in `argus-contracts/subjects.md`
(`argus.<domain>.v1.<event>`; the concrete `/sync` subject is
`argus.sync.v1.change` with the `SocketEmitDto` payload shape
`{operation, option, info}`, and the gateway subscribes with the frozen
wildcard `argus.*.v1.change` — tail-only `>` was ruled out because a
mid-subject `>` needs nats-server 2.10+, so the convention keeps every
subject valid on any server version).

`NatsBus` keeps no owning raw pointers: cnats handles (`natsConnection`,
`natsOptions`, `natsSubscription`) sit behind `std::unique_ptr` with custom
deleters, and the C-library callback resolves its handler through a map keyed
by the subscription handle. Subscriptions registered before `connect()` stay
pending and activate once the connection is up, because `ServiceRegistry`
initializes services in parallel before the bus is guaranteed to be connected.
`connect()` blocks until the first connection result (it must be reached via
`BlockingTask` from coroutines, never on a Drogon IO thread), while
`publish()`/`subscribe()` are cheap; handlers run on cnats worker threads and
must marshal into the loop for UI/sync work. `drain()` is idempotent and
releases every handler before closing. Config keys: `[nats] url`,
`reconnect_wait_ms`, `max_reconnects`, all optional
(`nats://127.0.0.1:4222`, 2000, 60). The optional live test needs a local
nats-server exported as `ARGUS_TEST_NATS_URL`; without it the suite prints SKIP
and exits 0, mirroring the golden-sync pattern.

## Fase 1 cutover — gateway public listener, legacy internal (F1-5, 2026-09-04)

The strangler cutover is in place: the argus-gateway takes the public TLS 7024
listener (the same instance CA the app pins, same `[cert]`/`[mdns]` keys) and
proxies everything it does not own to the legacy backend on an internal plain
listener (`[[drogon.listeners]] port 7025` — config-gated, zero legacy code
moved). The backend code change this phase is additive only (Ruling H below);
the A/B probe matrix
(`argus-gateway/tools/probe-identity-matrix.sh`, unauthenticated runs) re-run
against the booted modified backend is byte-identical to the committed
captures (01–09, 11, 20–24; the JWT-authenticated captures differ only in the
auth outcome of the no-token run, same conclusion as F1-4).

- **Ruling G — sync reads of identity-owned tables hit `DbService::client()`.**
  The syncable identity-owned set was verified from the code: `user`,
  `person`, `user_invitation` (the SynchronizedService repo list vs the
  identity schema). Their sync read methods (`find`, `findDeleted`,
  `findLast`, `findLastDeleted` in user/person/user-invitation repositories)
  moved from `readOnlyClient()` back to `client()`: on the gateway
  `client()` is identity.db (fresh rows), on the backend
  `readOnlyClient()` falls back to `client()` (same argus.db — byte-identical).
  Non-identity sync tables keep the read-only argus.db path.
- **Ruling S — audit sync reads follow the audit writes (F1-7 adjudication of
  the audit's MAJOR 1).**
  The app's offline audit-cursor pages (`sync_audit_log`/`sync_user_audit_log`
  message types over `/sync`, backed by the `audit_log`/`user_audit_log`
  tables) joined the Ruling G set: `AuditLogRepository`/`UserAuditLogRepository`
  `findSync`/`findLastSync` moved from `readOnlyClient()` to `client()` with
  the same mechanism (backend no-op via fallback; gateway resolves them to
  identity.db). Without this the gateway wrote identity-scope audit into
  identity.db while those pages replayed from argus.db read-only, so the app's
  audit view froze at the cutover. The audit.db split stays a Fase-2 item.
- **Ruling H — legacy user-row reads resolve to identity.db when configured
  (transitional).**
  `DbService::identityClient()`/`setIdentityClient()` (additive named
  read-only client; falls back to `client()` when not installed) is used by
  exactly two read sites: `UserRepository::findById` and
  `RefreshTokenRepository::findByAccessToken`. `UserRepository::findById` is
  the shared user-row lookup, so the redirect rides every caller of it, not
  only the JWT filter: auth-service (register/login, device approve, facial
  challenge, refresh/me), jwt-filter (the per-request auth read),
  sync-service and sync-media-service (socket context + voice greeting),
  user-feature-service and portrait-preview-service (gateway-native `/user`
  routes), project-member-feature-service and calendar-event-share-feature-
  service (target-user existence checks on proxied routes), and
  user-repository's own post-update re-reads. `Application::run()` opens
  `[identity] db` (`file:...?mode=ro`) when the key is configured; without it
  (pre-cutover) nothing changes. This is what lets the legacy accept
  gateway-minted tokens on proxied requests (proven live: gateway-minted JWT
  accepted by the legacy `JwtFilter` through the reverse proxy and directly)
  and what keeps proxied project-member/share target checks from rejecting
  gateway-only users against stale argus.db rows.
- **Divergence ledger (intentional, later phase):**
  (1) post-cutover, legacy reads of `user`/`person` rows outside the
  Ruling H redirect still read argus.db, which no longer receives identity
  writes — display data in legacy domains may go stale. The redirected
  `UserRepository::findById` callers, adjudicated: jwt-filter, sync-service,
  sync-media-service, project-member/share target checks and the repository's
  post-update re-reads are read-only row checks, and identity.db is the
  authoritative user store post-cutover (F1-3 migration plus all
  post-cutover identity writes), so the row a check looks for exists there;
  auth-service, user-feature-service and portrait-preview-service are
  gateway-native post-cutover, so on the legacy they are only reachable by a
  direct internal-listener call the app cannot make. The `person` table has
  no such redirect: legacy sync reads of `person` see stale/empty rows (e.g.
  register-side person lookups no longer run on the legacy);
  (2) the legacy keeps its identity controllers and its `/sync` endpoint in
  the binary (Ruling J — the `/sync` endpoint is the relay target and the
  proxy never forwards gateway-native paths; the binary strip is a
  later-phase task);
  (3) header ORDER of the CORS block differs between gateway-served
  (gateway post-handling order) and direct-legacy responses; the header set
  and values are identical, and no client behavior depends on order.
- **Cutover runtime shape** (verified in the two-process acceptance run):
  gateway `config.toml.example` now documents `[gateway]`
  (host/port/plain/min_protocol), `[identity]`, `[legacy]`
  (`sync_url` re-targeted to the internal listener, `proxy_url`, `db`), and
  shares the SAME `certs/` directory as the legacy (Ruling K); the backend
  `config.toml.example` documents the transitional `[identity] db` key.
  mDNS stays gateway-only in the cutover config (legacy `mdns.enabled=false`).

## Fase 2 step 2 — camera cutover (F2-2, 2026-09-05)

The camera domain (camera, camera_stream, zone) now lives in `camera.db`
(migrated by `tools/migrate-camera`, Ruling V) and is owned by the
`argus-camera` service. The app sees zero wire changes: the gateway relays
the camera CRUD and the camera media legs, and the legacy keeps serving the
camera-control routes against camera.db.

- **Ruling X — legacy camera-row access resolves to camera.db.**
  `DbService::cameraClient()`/`setCameraClient()` is a named client slot
  (falls back to `client()` when unset — pre-cutover no-op). The
  camera/camera_stream/zone repositories read through it, so the legacy
  camera-control routes (`/camera/{id}/ptz|preset|settings|status|presets|
  capabilities|talk`, still legacy this phase) resolve rows created
  post-cutover by argus-camera. `Application::run()` opens `[camera] db`
  read-write (WAL + busy_timeout 5000 pragmas, never DDL) when configured;
  argus-camera opens the same file read-write the same way. Both binaries
  keep the camera code (no strip). The frozen argus.db camera tables stay
  as-is (Ruling V, nothing deleted).
- **CameraChangeSink seam.** The camera/zone feature services emit through
  `camera_change::getSink()` (`src/shared/contracts/camera-change-sink.hxx`).
  The legacy binds `SocketCameraChangeSink` (SocketService rooms +
  SyncAuditService audit insert — the pre-cutover path); argus-camera binds
  `NatsCameraChangeSink`. Legacy `SocketService::publishChange` publishes
  every emit, camera included, to `argus.sync.v1.change` as before; the
  gateway subscribes the wildcard `argus.*.v1.change` and routes by concrete
  subject.
- **Ruling Y — camera audit diffs persist at the gateway.** argus-camera does
  not persist audit rows locally. Updates emit a `CameraAuditEvent`
  (`kind: audit` discriminator, exact SyncAuditService diff shape) over
  `argus.camera.v1.change`; the gateway inserts it verbatim into its audit
  substrate (identity.db `audit_log`, Ruling S read path) BEFORE fanning the
  DB-assigned row out as a `Log` sync event, so online replay and the offline
  audit cursor see the same order. Creates/deletes emit only `Add`/`Delete`
  change events (no audit row) — identical to the legacy path, since the same
  shared feature services run in both binaries. `create_user_id` is null on
  camera-produced rows (the camera domain cannot attribute the caller).
- **Ruling Z — gateway /sync camera reads hit camera.db.** The gateway opens
  `[camera] db` mode=ro as the named camera client and the
  camera/camera_stream/zone sync reads (bootstrap, diff pages, delete scans,
  cursors) resolve to it; TableName 0-23, SyncOperation 0-7, SYNC_LIMIT=200
  and the created_at+rowid cursors are untouched.
- **Media re-target.** The gateway `/sync` relay is composite now: `voice:*`
  legs still relay to the legacy (talk is TTS-load-bearing there until
  Fase 4); `camera:*` legs relay to argus-camera, which owns go2rtc
  (Go2rtcManager fork/exec) and StreamHub fMP4 (`0xA7` frame magic). The
  legacy keeps MediaRelay/CameraAudioSource in the binary but they are
  unreferenced in the cutover config — ledgered, do not strip yet.
- **Proxy routing split.** `SimpleReverseProxy` gained a route table
  (`[camera] proxy_url` backend): prefix `/camera` and `/zone` with max 2
  segments forward to argus-camera; deeper camera-control paths and every
  other path fall through to the legacy backends. No path is served by both
  sides (the legacy `/camera`/`/zone` CRUD controllers stay in the binary —
  strip is a later-phase task).

## Fase 4 step 2 — TTS extracted to argus-tts (F4-2, 2026-09-06)

- **argus-tts is the fourth microservice** (after gateway, argus-camera,
  argus-notification): a dedicated TTS capacity at `:7029`, loopback bind,
  built only from the shared TTS stack (tts-service/tts-engine/onnx-utils/
  style/unicode-processor) plus onnxruntime — no LLM/STT/VAD/ncnn symbols
  (nm -C proof in task-f4-2-report.md). The argus-tts tree
  (`argus-tts/src`) carries its own main, health controller, TTS controller
  and synthesize DTO; it compiles the shared sources directly (same pattern
  as argus-camera), so `models/tts` is read from the same tree via
  `tts.models_dir` — model files are never copied.
- **Internal wire (Ruling BH).** `POST /tts/v1/synthesize` returns float32
  PCM with `Content-Type: audio/x-argus-pcm-f32` and an
  `X-Argus-Sample-Rate` header; `POST /tts/v1/synthesize-stream` returns the
  same PCM chunked. Errors use the frozen app-envelope
  `{status, info, errors}` (422 validation, 400 bad JSON, 404/405 routing,
  503 `TTS_NOT_LOADED` when the engine is not ready). No auth: the service
  binds loopback only and trusts the host network. `GET /tts/v1/config` is
  additive (not in the frozen contract) and exists because the camera-control
  adapter must fetch `defaultSpeed`/`sampleRate` over the wire (Ruling BI).
- **Drogon chunking constraint.** Drogon refuses to send
  `Transfer-Encoding: chunked` on a `Connection: close` response (it skips
  the async-stream callback entirely when `ifCloseConnection()`), so the
  stream leg keeps the connection open and terminates at the terminal zero
  chunk. `TtsHttpClient::stream` de-chunks incrementally against that
  keep-alive response; a `Connection: close` client gets no streamed body.
- **Cutover switch.** `tts.remote_url` in `[tts]`: empty = the legacy
  in-process `TtsService` exactly as before; set = `TtsClient`
  (`src/shared/services/tts/remote/tts-remote.cc`) routes every synthesis to
  argus-tts over the wire. There is NO in-process fallback once remote is
  configured — a down argus-tts surfaces as an exception that each consumer
  maps to degradation (camera talk → 502 `CAMERA_UNREACHABLE` "Text-to-speech
  unavailable"; voice session → the assistant text frame still goes out, only
  the audio leg is dropped). The legacy binary keeps TtsService linked and
  boots init only while `tts.remote_url` is empty (boot-init gate, Ruling BJ).
- **Compose note.** argus-tts needs the models volume mounted (`models/tts`)
  and, like the other capacities, must not open any database.
- **Steps cap pin.** argus-tts derives its capability tier without a Vulkan
  probe (it links no ncnn), so `deriveTier` always lands on Low and the
  tier-derived denoising-steps ceiling is 8; the legacy on a Vulkan host
  reaches 12/16. `tts.steps_cap` (both config templates) pins the ceiling to
  the legacy value (12 = Balanced, 16 = High); unset keeps the
  tier-derived cap (`TtsService::effectiveStepsCap`, unit-tested).
- `labs/tts-probe` gained `--http <url>`: probes a running argus-tts over
  the wire (no local models needed); without the flag it drives the
  in-process engine as before.

## Fase 4 step 3 — STT extracted to argus-stt (F4-3, 2026-09-06)

- **argus-stt is the fifth microservice**: the speech-to-text capacity at
  `:7030`, loopback bind, mirroring the argus-tts scaffold (own binary,
  CMake preset pair `stt`/`stt-prod`, AGENTS.md, /health, config template
  with ONLY `[stt]` + `[server]`). It compiles the shared SttService from
  the shared tree via the same foundation slice pattern as argus-tts
  (`ARGUS_NO_NCNN_GPU=1` instead of argus_common), reads `stt.models_dir`
  from the shared models tree (never copied) and carries no database at all
  (Ruling BN) — no persistence, no migrate tool.
- **Internal wire (Ruling BL).** `POST /stt/v1/transcribe` takes a binary
  body `audio/x-argus-pcm-s16` (16 kHz mono int16 little-endian) and
  `lang` as query parameter or header (`es` | `en` | `auto` | empty — empty
  resolves from the service's own `stt.language`). The response is the
  frozen app-envelope with `info.text` (the `ApiResponse`-shaped success,
  like /tts/v1/config — the brief's bare `{text}` is served inside the
  envelope because every response in this codebase goes through
  ApiResponse). Quantization both directions is `/32768.0F`, matching the
  voice session's own WS-frame conversion so the A/B text is identical.
  Errors: 400 wrong content-type/odd byte count, 422 empty body or
  unsupported lang, 503 `STT_NOT_LOADED`, frozen 404/405 envelopes.
  `GET /stt/v1/config` is additive and internal-only. No auth: loopback
  trust boundary, same as argus-tts.
- **Cutover switch.** `stt.remote_url` in `[stt]`: empty = the legacy
  in-process `SttService` exactly as before; set = `RemoteVoiceStt`
  (`src/shared/services/stt/remote/stt-remote.cc`, dependency-free raw
  socket client) serves the IVoiceStt seam over the wire with a cached
  per-config client (rebuilt only when the remote config changes — the
  F4-2 three-connections-per-request lesson). NO in-process fallback: a
  down argus-stt surfaces as the existing turn degradation
  (`voice:event` `{reaction: confused, because: stt_failed}` — the brief's
  "voice:error frame" is actually this reaction frame, ReactionEngine
  sttFailed → Confused), the worker thread and the session survive. The
  legacy binary keeps SttService linked and skips boot-init while
  `stt.remote_url` is set (Ruling BM); there is no lazy-init path —
  the singleton is only reachable through the seam, which returns the
  remote adapter instead (SingletonVoiceStt stays for the non-cutover
  path). The voice session keeps VAD + RNNoise + workerLoop untouched
  (Ruling AY); `setLanguage` at session start steers the wire's per-request
  `lang` parameter (Ruling BE semantics — the service keeps ONE global
  recognizer and rebuilds it only when the effective language changes).
- **Compose note.** argus-stt needs the models volume mounted (`models/stt`)
  and, like the other capacities, must not open any database.
- `labs/voice-test` gained `--stt-http <url> <wav>`: transcribes a 16 kHz
  mono s16 wav through the running argus-stt wire (no local engine in the
  path); without the flag the STT probe path is unchanged.

## Fase 4 step 4 — Vision extracted to argus-vlm (F4-4, 2026-09-06)

- **argus-vlm is the sixth microservice**: the vision captioning capacity
  at `:7031`, loopback bind, llama.cpp + mtmd (`third_party/llama.cpp`, tag
  b10305) serving the LFM2-VL model. It mirrors the argus-stt scaffold
  (own binary, preset pair `vlm`/`vlm-prod`, AGENTS.md, /health, config
  template with ONLY `[vision]` + `[server]`) and compiles the shared
  `VisionService` from the shared tree via the same foundation slice
  pattern (`ARGUS_NO_NCNN_GPU=1` instead of argus_common). Models are read
  from the shared `models/vision/lfm2vl-25` tree (never copied) and the
  service carries no database (Ruling BN analog) — `VisionService` is
  owned BY VALUE by the controller, not as a singleton (Ruling BO).
- **Internal wire (Ruling BP).** `POST /vlm/v1/describe` takes JSON
  `{image_b64, prompt?, camera_id?}` (base64 JPEG) and answers the frozen
  app-envelope with `info.caption`. Errors: 400 bad JSON, 422
  validation/undecodable image, 503 `VLM_NOT_LOADED`, frozen 404/405
  envelopes. No auth: loopback trust boundary, same as argus-tts/argus-stt.
  `GET /vlm/v1/config` is additive and internal-only. The wire carries no
  max-tokens field — the service's `vision.max_tokens`/`vision.prompt`
  defaults apply on the remote path.
- **Cutover switch (Ruling BQ).** `vision.remote_url` in `[vision]`: empty
  = the legacy in-process `VisionServiceAdapter` exactly as before; set =
  the registry gets `RemoteVisionServiceAdapter`
  (`src/shared/services/vision/remote/`) under the same service name
  `vision` and the legacy binary boots WITHOUT loading the vision model
  (log line "VLM delegated to <url>; in-process VisionService stays
  uninitialized"). This is a capacity move only — no production caller
  changed: callers keep asking the registry for `vision`. The adapter
  keeps the `describeMat`/`describeMatAsync` seam, encodes the Mat as JPEG
  (quality 90) over the wire, holds a caption cache keyed FNV-1(encoded
  JPEG + prompt) — the same-keyed cache as the service's own pixel cache
  (shared `vision-hash.hxx`) — and surfaces a down service as an exception
  carrying the envelope error (try/catch, never a terminate path); no
  production caller changed, so degradation mapping belongs to the
  deferred consumer (Ruling BA).
- **Same-commit vendor constraint.** the mtmd projector is linked against
  the vendored llama.cpp; the legacy and the service must run the SAME
  `third_party/llama.cpp` commit (tag b10305) or the caption distribution
  diverges between in-process and wire paths.
- **Tier divergence pin.** argus-vlm derives its tier without a Vulkan
  probe (it links no ncnn), so `vlmGpuLayers()` lands on 0 where the legacy
  on a Vulkan host offloads layers; `vision.gpu_layers` in the argus-vlm
  config template pins 999 for GPU parity (the F4-2 `steps_cap` lesson
  analog). `vision.caption_cache_slots` sizes both caches.
- **Deferred (Ruling BA).** the `IKnownPersonMatcher` wiring into the
  event pipeline stays deferred — the wire contract does not expose the
  matcher; a later step routes it through the describe wire.
- `labs/vlm-bench/service-check` gained `--http <url>`: drives the
  running argus-vlm describe wire with the synthetic bench frame (no local
  engine in the path); without the flag the in-engine probe is unchanged.
