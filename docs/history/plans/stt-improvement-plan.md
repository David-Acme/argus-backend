# Better speech recognition at the same latency

Status: written 2026-10-19 from the closing ledger's row "STT improvement: hotwords, cleanup,
streaming, same latency" (`closing-plan-2026-10.md`), not started on `master`. It absorbs the plan
note and the uncompiled Phase 1 code of branch `track/b4-stt-quality` (section 2). Every finding
below was read from the code on 2026-10-19 (backend `e19dfd19`); nothing has been built, run or
measured for this document.

## 0. What the owner asked for

Better transcripts in calls and at the cameras, with no added wait for the user. The priority is the
owner's standing order: **precision > speed > memory**, each model capped at **1 GB** of resident
memory. Four strands:

1. **Streaming recognition**: partial text while the user speaks, without a later or worse final.
2. **Hotwords / contextual biasing**: the household's names, the camera names, "Argus" and its
   variants, hour phrases. The reported errors to beat: "Argos"/"Arbus" for "Argus", "a la serie"
   for "a las seis", "¿quién usted?" for "¿quién está?".
3. **Transcript cleanup**: fillers, punctuation, numbers, applied where it helps and never where it
   hurts the decider.
4. **Model comparison**: the deployed `nemo_transducer` against the other engines the service already
   loads (`whisper`, `canary`, `nemo_ctc`, `omnilingual`) and the two newer candidates the branch
   found (Parakeet TDT 0.6B v3, Nemotron 3.5 ASR streaming 0.6B).

The latency bar: the final text's p95 after the end of speech stays within **+50 ms** of today's p95,
measured on the prod build, on this CPU, in the same run (the branch note's G3; adopted here).

## 1. What exists today

- **The engine.** `services/stt/src/feature/stt/services/stt-service.{hxx,cc}`: one global
  sherpa-onnx `OfflineRecognizer` behind a mutex, five engines (`SttEngine`), `nemo_transducer` the
  default and the only one provisioned (`services/stt/scripts/provision.sh`, the `voice-stt`
  component in `services/settings/modules.json`). `createRecognizer` sets no `decoding_method`, so
  sherpa runs greedy search, and no hotwords. `languageBound()` rebuilds only for `whisper` and
  `canary`. `num_threads` is `ThreadBudget::computeThreads()`.
- **The wire.** `packages/contracts/stt/stt.proto`: `Capabilities`, unary `Transcribe`, and the
  bidirectional `TranscribeStream` (`TranscribeChunk{samples, sample_rate, language, flush}`,
  `TranscribeUpdate{text, final, samples, decode_ms}`), served by
  `services/stt/src/app/rpc/stt-rpc-server.cc` and wrapped by `packages/clients/stt/src/stt/stt-client.{hxx,cc}`
  (`TranscribeStream::push/flush/cancel/finish`). The HTTP leg `POST /stt/v1/transcribe`
  (`feature/stt/controllers/stt-controller.cc`, `dtos/transcribe-dto.cc`) stays the fallback.
- **"Streaming" today is a moved decode, not an incremental one.** `services/stt/CONTEXT.md`
  ("Streaming"): the offline FastConformer cannot emit a final before the audio ends; argus-voice
  flushes 128 ms into the pause and the endpoint fires at 384 ms, so the decode of the prefix finishes
  inside the wait (`services/voice/CONTEXT.md`, "Streamed transcription";
  `services/voice/src/feature/voice/turn-transcript.{hxx,cc}`, `voice-engine-seam.cc`
  `RemoteSttStream`). Measured 2026-10-04: server-side `stt_ms` after the endpoint went from a median
  50 ms to 0. There are **no live partials while the user speaks**; the only partial is the flush.
- **Latency on file** (`services/stt/CONTEXT.md`, "Real-time behaviour", 2026-10-03, prod, Ryzen 7
  5825U): 57–127 ms for 2–6 s utterances at load 1–3; RTF 0.02–0.06.
- **Accuracy baseline** (`services/stt/CONTEXT.md`, "Accuracy baseline", 2026-10-06):
  `services/stt/tests/eval/stt-wer-eval.cc` over clips fetched by `scripts/stt-eval-data.py` into
  `models/stt-eval/` (untracked, `manifest.tsv` with licence and SHA-256). Common Voice es 136 clips
  WER 5.98 %, OpenSLR 73 Peruvian 80 clips WER 5.50 %, overall 5.80 % (CER 1.77 %), gated by
  `services/stt/tests/eval/gates.json`. Read speech close to the microphone: no far-field camera
  audio and no conversational set is measured yet.
- **The camera path.** `services/camera/src/feature/actions/stt-transcriber.cc` (guard's `Listen`):
  PCMA 8 kHz upsampled to 16 kHz, unary transcription. The watchman and camera-call plans
  (`camera-watchman-plan.md` section 3, `camera-call-plan.md`) already assume STT hotwords for the
  name "Argus"; this plan delivers that mechanism.
- **Cleanup.** No transcript cleanup stage exists between STT and argus-llm. The LLM side strips a
  few leading words for its own purposes (`kLeadingFillers` in
  `services/llm/src/feature/llm/services/tools/reply-claims.cc`); the decider (Laya), the slot layer
  (`services/llm/src/feature/llm/services/turn/slots.cc`, `slot-lexicon.cc`) and GLiNER read the raw
  transcript. Audio cleanup (high-pass, gain) does not exist in `packages/lib/audio` on `master`
  (only `audio-resampler` and `endpoint-detector`).
- **Settings.** `services/stt/src/feature/settings/stt-settings.cc`: `stt.language` (live) and
  `stt.engine` (restart). No hotword keys on `master`.

## 2. Reusable work on `track/b4-stt-quality` (read-only inspection, 2026-10-19)

Thirteen commits on top of `09ce08bc`, last 2026-10-14, **none compiled or run** (the commit reports
say so: the build waited on a lock). The branch is behind `master` (build-speed batch, camera plans),
so it is rebased, not merged.

| commit | what | verdict |
|---|---|---|
| `0c444552`, `dd7e9424` | `docs/design/stt-quality.md`: gates G1–G6, the candidate table, the C0–C7 measurement rows, fifteen questions and the coordinator's answers (section 13) | **Adopt** as the measurement design; this plan cites it and does not repeat it. Its Q-answers are recorded as decided (section 8) |
| `16c61d71` | encoder size measured (131,113,682 bytes; 107 MB is the tarball) in `services/stt/CONTEXT.md` | **Adopt** |
| `69f8adfb`, `ccaf5ea1`, `1df8fdba` | `packages/lib/audio/src/audio/{biquad-high-pass,rms-normalizer,audio-profile}.{hxx,cc}` with unit tests | **Adopt after review**: rule 20 scan, rule 2 (3+ params), build, tests. Wiring waits for U3's numbers |
| `e2f4c1e5`, `af7a2f93` | `packages/lib/text/src/text/hotword-list.{hxx,cc}` (pure builder, names from the caller) and `packages/lib/phrase/src/phrase/details/hour-vocabulary-es.hxx` | **Adopt after review**. Check `lib/text` → `lib/phrase` is a tier-1 → tier-1 edge `scripts/check-deps.sh` accepts |
| `5d950387`, `43ee890e` | `stt.hotwords_enabled`, `stt.hotwords_score` in the settings catalog | **Adopt after review**; the keys do nothing until U4 wires them, so they ship together with U4, not before |
| `69ce06d6`, `423625e1` | `services/stt/tests/eval/stt-quality-eval.cc` (one config per process: hotwords, cleanup, background set, flush latency p50/p95, RTF, peak RSS; `--engine online` for the Nemotron streaming candidate) with `eval-corpus.hxx`, `wer-text.hxx` shared with `stt-wer-eval` | **Adopt** as U1's harness after it compiles and reproduces the 2026-10-06 baseline within ±0.3 points |
| `bf89a635` | `services/stt/NOTICE` (models + measurement-set licences) | **Adopt**; the Nemotron OpenMDW-1.1 text is still to read before any pin |

What the branch did **not** do and this plan adds: live partials, the hotword wire fields and the
voice-side list assembly, transcript (text) cleanup, a far-field camera set, the engines already in
code (`whisper`, `canary`, `nemo_ctc`, `omnilingual`) in the comparison, and a prod-build latency run.

## 3. Design

### 3.1 Streaming, in two steps

- **Keep the flush architecture.** It already removed the post-endpoint wait; nothing in this plan
  touches the 128 ms flush or the 384 ms endpoint unless U2's sensitivity table says so.
- **Live partials (additive).** While speech continues, argus-voice may send a `flush`-like
  `partial = 5` request every `stt.partial_interval_ms` (recommended 640 ms); argus-stt answers a
  `TranscribeUpdate{final: false}` from a best-effort decode of the prefix, skipped (last text
  repeated) when no inference slot is free, exactly as the flush partial already behaves. With the
  offline model, a partial costs one prefix decode (RTF 0.02–0.06, so about 20–60 ms of CPU per
  second of audio); the bar is that it never delays the final (G3) and never takes the slot the
  final needs. Who consumes partials: the app's live caption over the voice frames (frontend
  coordination, rule "API contracts"), the barge-in decision, and the watchman's stage 2. No consumer,
  no partials: the interval defaults to 0 (off) until a consumer lands.
- **A true incremental encoder** (Nemotron 3.5 streaming, N1 in the branch note) is the second step
  and only if it wins on the gates (section 5). It would make partials nearly free and keep the final
  decode at one chunk. It is gated on the CPU runtime existing in the pinned sherpa-onnx
  (`third_party/sherpa-onnx` at `546df6f9`; the branch note found the family only under
  `sherpa-onnx/csrc/qnn/`). A sherpa bump is its own unit, with every engine re-run.

### 3.2 Hotwords

- Mechanism (verified by the branch at the pin): per-stream hotwords via
  `SherpaOnnxCreateOfflineStreamWithHotwords`, weight `hotwords_score`, applied only under
  `decoding_method = "modified_beam_search"` (`max_active_paths` is the cost knob). Greedy search
  does not bias, so the switch to beam search is measured on its own row first (it changes WER and
  RTF even with an empty list).
- The list is built **per home by argus-voice** at session start with the branch's
  `hotword_list` builder: members' names (through `argus::clients::identity`), camera names (through
  `argus::clients::camera`), "Argus" with "Argos"/"Árgus" as targets, the hour phrases from
  `lib/phrase`. Environment names wait (coordinator Q4: no guard client). Capped by
  `stt.hotwords_max` (recommended 64).
- Wire, additive (no field reused): `TranscribeChunk.hotwords = 6` (repeated string),
  `TranscribeChunk.hotwords_hash = 7`; `TranscribeRequest.hotwords = 4`, `hotwords_hash = 5`.
  The branch note proposed 5/6 on the chunk; 5 is taken here by `partial`, so the numbers move.
  `services/stt/tests/unit/stt-wire-test.cc` and `stt-rpc-test.cc` are extended. The camera's
  `stt-transcriber.cc` sends the camera's own list (camera names, "Argus").
- Post-recognition alias rewrites ("Arbus" → "Argus") are **not** in the first pass: only if biasing
  leaves the error, and then per home.

### 3.3 Transcript cleanup

Two layers, both deterministic and measured, never a model:

- **Audio, before STT** (branch Phase 1): high-pass ~80 Hz and RMS normalisation on voiced segments,
  per capture profile (phone, camera). RNNoise stays in voice as shipped; whether it helps the STT is
  a measured row (C1 vs C2), and GTCRN only if it wins per source.
- **Text, after STT**, a new pure function in `packages/lib/text` (`transcript-cleanup`), called by
  argus-voice before the turn leaves for argus-llm:
  - fillers: a static per-language list in `packages/lib/phrase/src/phrase/details/`
    ("eh", "este", "o sea" only at the start, "mmm", "um", "uh"), removed only at utterance
    boundaries and between pauses, never inside a name;
  - punctuation: none added by a model. `nemo_transducer`'s output casing and punctuation are
    recorded in U1 (to measure); if it emits none, the question mark for an interrogative opening
    word ("quién", "qué", "cuándo") is the only rule, because the LLM's prompt reads it;
  - numbers: **no inverse text normalisation into the decider's input.** Laya, the slot lexicon and
    GLiNER were trained and gated on spoken forms ("a las seis"); turning them into digits is
    measured first with `services/llm/tests/eval/decider-eval.py` and `slot-eval.py`
    (`docs/operations/voice-quality-eval.md`). The written form is used only for display (the app's
    transcript), as a separate field.
  The cleaned text and the raw text both travel; the raw stays in the turn log for evaluation.

### 3.4 Model comparison

All rows on the same harness, the same clips, the same prod build, one heavy run at a time:

| engine | already in code | provisioned | expected role |
|---|---|---|---|
| `nemo_transducer` FastConformer en/de/es/fr int8 | yes | yes | C0 baseline |
| `nemo_ctc` | yes | no | cheaper decode, likely worse WER; measured as the speed floor |
| `canary` | yes (`use_pnc = 1`) | no | punctuation and casing for free; language-bound rebuild |
| `whisper` (tiny) | yes | no | expected weaker on Spanish; measured once to close the question |
| `omnilingual` | yes | no | breadth; measured on es only |
| Parakeet TDT 0.6B v3 int8 | no | no | strongest offline candidate; RSS vs the 1 GB cap and decode inside the 256 ms window are the risks |
| Nemotron 3.5 streaming 0.6B int8 | no | no | true streaming; CPU runtime at the pin unconfirmed |

Each new artifact needs its licence read on the upstream release (not a mirror), a SHA-256 pin in
`provision.sh` (`fetch_pinned`), its files in the `voice-stt` component and a `NOTICE` row before it
can become the default.

## 4. Measure first (U1, before any wiring)

Sets (branch note section 8, extended):

- **S1** Common Voice es test (CC0), **S2** OpenSLR 73 Peruvian (CC-BY-SA, measurement only), as
  fetched by `scripts/stt-eval-data.py`.
- **S3** camera far-field: S1+S2 passed through the camera chain (16 → 8 kHz → A-law → back to
  16 kHz, using the camera's own decoder path), plus the owner's private camera recording with its
  reference held outside the repository (aggregates only, never in a file in git).
- **S4** hotword subset and **S5** hotword-free negatives; **S6** background voices (the owner's
  segment plus S1 mixed at 10 and 0 dB SNR, seeded).
- Size: about 2,000 words per set gives about ±1 point at 95 %; S3's private part is too small to
  gate and is reported only.

Metrics per set and per profile: WER (accents kept and folded), CER, hotword error rate and false
insertions, false turns on S6, **final-text latency p50/p95 after the end of speech** with clips
replayed in real time through the real VAD and flush, partial cadence and partial-to-final word
agreement (for 3.1), RTF, peak RSS (VmHWM). Latency comes **only** from a prod build
(`./scripts/build-all.sh prod --only stt`); the load average is written on each row; heavy runs go
through the RAM gate the owner set (one heavy job at a time).

## 5. Gates

From the branch note, adopted: **G1** overall WER ≤ C0 and `gates.json` passes, no set worse than C0
by more than 0.5 point; **G2** hotword error rate strictly below C0 on S3 and the phone set;
**G3** final p95 ≤ C0 p95 + 50 ms; **G4** peak RSS ≤ 1 GB per model; **G5** false hotword
insertions ≤ C0 + 0.5 point; **G6** false turns on S6 ≤ C0. Added here: **G7** partials never move
the final's p95 (G3 measured with partials on) and **G8** the decider and slot gates in
`services/llm/tests/eval/gates.json` do not regress on cleaned text. Choice rule: the cheapest
configuration that passes all gates; between two that pass, the lower WER (precision first).

## 6. Units

Each unit lands with its own acceptance, its own commit series with a Report line, the reviewer's
acceptance of the exact patch before merge, and `./scripts/build-all.sh dev --only <project>` at
0 errors / 0 warnings.

| # | Unit | Done when |
|---|---|---|
| U0 | Rebase `track/b4-stt-quality` onto `master`, compile, review its Phase 1 code (rule 20, rule 2, rule 19, deps tiers) | the branch's unit tests green in `packages` and `stt`; `stt-wer-eval` reproduces the 2026-10-06 numbers within ±0.3 points |
| U1 | Baseline C0 on the prod build: S1–S6, latency p50/p95 through the real VAD and flush, RSS; record `nemo_transducer`'s casing/punctuation behaviour | a table in `services/stt/CONTEXT.md` "Quality baseline v2"; G3's bar is a number |
| U2 | Engine comparison: the five in-code engines plus Parakeet v3 (and Nemotron if its CPU runtime exists at the pin, else recorded as excluded) | one table, every row with WER, p95, RTF, RSS; licence read for every candidate that passes |
| U3 | Audio cleanup rows C1–C5 on the winning engine; wire the profile per capture source in voice and camera only for rows that pass | G1, G3, G6 green; the camera profile chosen by S3 |
| U4 | Hotwords end to end: beam-search row first, then the wire fields, the voice-side list (identity and camera clients), the camera list, the two settings keys from U0 | G2, G3, G5 green; the three reported errors fixed on S4; a test that fails when the bias is absent |
| U5 | Text cleanup in `lib/text` + filler vocabulary in `lib/phrase`; raw and cleaned text both carried; display-only number form | G8 green on `decider-eval` and `slot-eval`; unit tests per rule |
| U6 | Live partials (`partial` field, interval key, best-effort slot rule) with the app's caption as first consumer, coordinated with the frontend | G7 green; a two-device-free check: partials visible in the call screen while speaking |
| U7 | Adopt the winner of U2 if it beats C0 on G1–G4: provision pin, `voice-stt` component files, `NOTICE`, `config.toml.example`, `gates.json` tightened to the new baseline | the full gate set passes on the prod build; the old engine stays selectable |
| U8 | (conditional) Nemotron streaming as the final engine, only if it wins U2 and the CPU runtime exists; a sherpa-onnx bump is a separate unit with every engine re-run | as U7, plus partial cost measured near zero |

Order: U0 → U1 → U2 ∥ U3 → U4 → U5 → U6 → U7 → U8. U4 is the first owner-visible gain ("Argus"
recognised); the watchman's stage 2 (`camera-watchman-plan.md` W3) depends on U4.

## 7. Risks

- Beam search alone may cost more than the 50 ms margin on long utterances; then hotwords run only on
  the flush decode and the final reuses it, as the flush already does.
- Parakeet's int8 encoder is reported at about 622 MB (unverified); with the decoder and runtime it
  may cross 1 GB. Measured, not assumed; over the cap it goes to the owner with numbers.
- A cleaner transcript can hurt Laya, which saw raw text in training: G8 guards it.
- S3's private recording is small; it informs, it does not gate.

## 8. Owner decisions (recommended options, recorded 2026-10-19)

1. **Partials off until a consumer exists**; the app caption is the first one. Recommended over
   always-on, which spends CPU nobody reads.
2. **No inverse text normalisation into the decider**; display-only written numbers. Recommended
   over normalising everything, which would invalidate Laya's and GLiNER's gates.
3. **Hotword list per home, built by voice**, from members and cameras only (branch Q4 answer).
4. **The coordinator's answers of 2026-10-14** (branch note section 13: Q1, Q2, Q3, Q7, Q8, Q10–Q13,
   Q15) stand as decisions of this plan.
5. **A new default engine only after U7's full gate set**; `nemo_transducer` stays installed and
   selectable for a rollback through `stt.engine`.
6. Open for the owner when U2 reports: whether a candidate that wins on precision but exceeds 1 GB is
   acceptable (the standing rule says no; the plan does not assume an exception).
