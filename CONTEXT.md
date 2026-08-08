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
  to control: `fastText`, `hnswlib`, `ncnn`, `sherpa-onnx`. Built via
  `add_subdirectory` with `EXCLUDE_FROM_ALL`.
- Convention: Conventional Commits in English. Remote `git@github.com:David-Acme/argus-backend.git`,
  branch `main`.

## Dependency resolution notes (Conan conflicts, learned the hard way)

- `nlohmann_json` pinned to **3.11.3** — jwt-cpp/Drogon use this version.
- `opencv/4.13.0` built **headless**: `with_protobuf=False`, `with_eigen=False`,
  `with_ffmpeg=False`, `with_wayland=False`, `with_gtk=False`, `with_vulkan=False`.
- `eigen/5.0.1` **removed** (2026-08-06): it was declared in `conanfile.txt` and
  linked in `CMakeLists.txt` but had zero uses in `src/` or `labs/`. The tracker
  planned in `OPTIMIZATION_AND_MEMORY_PLAN.md` uses `cv::KalmanFilter` instead,
  so no dependency comes back.
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
| `FaceDB` | HNSWlib | In-memory 128-dim index | Loaded from SQLite at startup |
| `LlmService` | llama.cpp (submodule b10305) | LFM2.5-1.2B-Instruct-Q4_K_M | `models/llm/` |
| `VisionService` | llama.cpp + libmtmd | LFM2.5-VL-450M (Q8_0 + mmproj F16) | `models/vision/lfm2vl-25/` |
| `SttService` | sherpa-onnx | Whisper tiny | `models/stt/` |
| `TtsService` | Supertonic 3 | ONNX models | `models/tts/` |
| `JwtService` | jwt-cpp | HS256, instance class | — |
| `ConfigService` | tomlplusplus | TOML config reader | `config.toml` |
| `DbService` | Drogon DbClient | SQLite async client | `database/argus.db` |

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
- **Vision**: REPLACED with **SmolVLM2-500M-Video-Instruct (ONNX int8, 0.5B)** —
  `HuggingFaceTB/SmolVLM2-500M-Video-Instruct`: 3 sessions (SigLIP vision
  encoder base patch-16/512 → 64 image tokens, merged Llama3 decoder with
  fp32 KV cache, token embeddings) + GPT-2 byte-level BPE tokenizer. ChatML
  prompt `<|im_start|>User:<image>Can you describe this image?
  <end_of_utterance>\nAssistant:` with the `<image>` block expanded to
  `<fake_token_around_image><global-img><image>x64<fake_token_around_image>`;
  the 64 image rows are filled at runtime with the encoder features
  (inputs_merger pattern). Greedy decode; prefill runs the whole prompt in
  one pass, then iterative decode with growing attention_mask/position_ids.
  Frame cache: the vision encoder output is cached per image hash (2 slots) —
  repeated camera frames skip the ~0.5s encoder pass. The 500M is a full VLM
  (VQA + captioning) and beats Florence-2 on quality while being faster and
  lighter on CPU. NOTE: `vision.image_size` is now fixed at 512 by the model;
  the encoder runs in ~0.5s and decode ~24ms/token on the reference machine.
  **Tunables in `config.toml` `[vision]`**: `max_tokens` (default 64, caption
  is 1-3 sentences) and `threads` (0 = auto). ORT sessions use
  `spin_duration_us=1000` + `spin_backoff_max=8` (ORT #28096 recommended).
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
- **llama.cpp via Conan**: `llama-cpp/b6565` in `conanfile.txt` (latest stable
  on Conan Center, 2025-09). Third-party submodule removed (was pinned to
  b10216 for the LLM; mtmd is no longer built — vision moved to ONNX).
  API notes for b6565: `llama_model_params` uses `use_mmap`/`use_mlock`
  booleans (the newer `load_mode`/`LLAMA_LOAD_MODE_MMAP` from b10216 does not
  exist yet). Target: `llama-cpp::llama-cpp`. Supports the LFM2 arch used by
  LFM2.5-1.2B.
- **ncnn Vulkan**: kept ON — the reference machine has an AMD iGPU (RADV
  RENOIR) that ncnn uses via Vulkan; machines without GPU fall back to CPU.
- **Voice test** (`labs/voice-test/`): standalone `argus-voice-test` binary that
  chains STT → LLM → TTS for a spoken conversation (EN/ES) to measure quality
  and latency end-to-end. PortAudio for mic (16 kHz, software resample
  fallback for devices without 16 kHz) and speaker (plays TTS PCM at its
  native 44.1 kHz so pitch stays natural). Turn-taking uses **Silero VAD v5
  (ONNX, `models/vad/silero_vad.onnx`, ~2.3MB, ~0.1ms/chunk)**: continuous
  speech probability with hysteresis (0.5 start / 0.35 end), ~250ms min
  speech, ~500ms silence hangover, and a 300ms pre-roll so the leading
  phoneme is never clipped (this fixed "hola" being transcribed as garbage).
  The LLM replies fully first, then the TTS speaks with its native chunking
  (natural, not choppy). Ctrl+C exits cleanly (shared std::atomic<bool>).
  `SttService::setLanguage("es"/"en")` switches Whisper at runtime; the
  `[stt] language` config defaults it. Run: `build/prod/labs/voice-test/
  argus-voice-test`.

## Tapo camera integration — Phase 1 (2026-08-06)

Local control and audio-out protocols for the TP-Link Tapo C225, standalone and
validatable before any media or pipeline work exists. Media, detection, tracking
and the memory system are planned in `OPTIMIZATION_AND_MEMORY_PLAN.md`.

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
  preference, and the talk-channel knobs (`talk_framing`, `talk_mode`,
  `talk_packet_ms`).

### Verified offline (no camera needed)

`--ts-dump` writes 1 s of 440 Hz A-law as MPEG-TS. `ffprobe` parses PAT/PMT/PES
and reports `pid=100`; extracting the ES yields exactly 8000 bytes. Decoding it
back measures peak −8.70 dB (= 12000/32768), RMS = peak − 3.01 dB (pure sine)
and 879 zero-crossings/s (= 2 × 440 Hz).

**Stream type — the value flipped twice, read this before changing it again.**
Muxing the same audio with ffmpeg and reading back its PMT gives `stream_type
0x06` with `stream_id 0xBD` (private stream), and that was the first choice
because ffmpeg's output is what pytapo feeds the camera. `TapoTsConfig` now
carries `streamType 0x90` / `streamId 0xC0` instead. `0x90` is what pytapo
itself writes, so the C225 plausibly wants it, but the change predates any
recorded measurement against the physical camera, and the ffprobe check above
no longer reports `stream_type=6`. Whichever value survives, record *how* it
was confirmed here — a wrong stream type fails as silence, not as an error.

### Still unverified — needs the physical C225 (Phase 1 gate)

Which credential the control channel accepts (camera account vs `admin` +
cloud password), the exact `searchDetectionList` response shape, the 8800
multipart body framing (`talk_framing`), and which digest password variant the
talk channel wants. Each has a probe flag and is persisted rather than
hardcoded, so one `tapo-probe` run against the camera settles all of them.

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

**Barge-in**: the microphone is closed as soon as the first audio plays,
otherwise the VAD hears Argus through the speakers and interrupts itself. Real
barge-in during playback needs acoustic echo cancellation (WebRTC APM or
similar) and is not implemented.

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

- `user` — accounts (role, is_active, soft-delete via deleted_at)
- `person` — known people linked to users via `user_id`
- `face_embedding` — persisted face embeddings (hex-encoded 128-dim float BLOBs)
- `refresh_token` — JWT refresh token lifecycle (is_valid, is_used, device_hash, expires_at)
- `camera`, `zone`, `event`, `person_event`, `reminder`, `reminder_detail`, `context_note`
- `camera_stream`, `schema_version`
- `audit_log` — audit global por módulo (`changes` = JSON diff de `JsonDiff`, `priority`, `create_user_id`)
- `user_audit_log` — audit a nivel de usuario (= `audit_log` + `user_id`), sincronizable por usuario
- `notification` — notificaciones personales por usuario (type/title/body/data/is_read)
- `notification_token` — push tokens por sesión (`UNIQUE(user_id, device_hash)`)
- `user_action_log` — historial server-side de acciones (write-only, NO sync)

## Sync engine (WebSocket, one-way server→cliente)

- **Ruta WS**: `/sync` con **solo `JwtFilter`** (sin `DeviceFilter`; el binding de
  dispositivo queda en HTTP). Solo sincronización; notificaciones y
  token-register van por HTTP.
- **Operaciones** (`src/shared/contracts/sync-operation.hxx`): `InitialInfo=0` (connect, envía
  `{id, role, isActive}` — sin lista de módulos),
  `Synchronize=1` (datos creados/eliminados, global + nivel usuario: notification),
  `SynchronizeAuditLog=2` (diffs globales, tablas decididas por el backend según rol),
  `SynchronizeUserAuditLog=3` (diffs a nivel usuario, filtrado por `sub`). Eventos en vivo:
  `Add=4`, `Delete=5`, `Log=6` (emitidos por `AuditLogService`/`NotificationService`).
- **Respuestas WS**: `SocketEmitDto` `{operation, option(TableName), info}`; errores
  `{type:"<type>_error", status, error}`.
- **Entidades syncables**: `user`, `camera`, `camera_stream`, `zone`, `reminder`,
  `reminder_detail` (implementan `Syncable`) + `notification` (dedicado por usuario).
  Fuera del sync: `event`, `person`, `context_note`.
- **Snapshot por día**: `AuditLogService`/`UserAuditLogService` buscan la entrada del
  `record_id` en el día y fusionan el diff (`JsonDiff::compareChanges`).
- **RoomManager**: instancia, estado `thread_local` a nivel de archivo; rooms de módulo
  (`1 + TableName`) y room de usuario (`1000 + sub`); `emitUser` llega a las N sesiones activas.
  Ciclo de vida vía `RoomManagerServiceAdapter` (IService) en `ServiceRegistry`.
- **HTTP**: `PATCH /notification/read` y `POST /notification-token` (cadena completa de filtros).

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
