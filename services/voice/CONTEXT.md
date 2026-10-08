# argus-voice — CONTEXT

## Why the voice service exists

F6-3 v2 of the `migracion-microservicios` plan makes argus-voice the pilot
pure-gRPC service: the voice orchestration stack (one voice session per
client: VAD turn detection → STT → LLM chat → TTS synthesis, plus the
reaction engine and the noise/RNNoise path) leaves the legacy monolith and
speaks only `argus.voice.v1`. The `/sync` voice wire is frozen, and since
sub-step 3a-1c the forwarder that renders the typed server frames back into the
exact JSON/binary the app expects is argus-sync's
(`services/sync/src/feature/transport/infra/voice-grpc-relay.cc`).

## What it owns

- **The voice session** (`voice-session-service`), driven by the bidi
  `VoiceService/Connect` stream: `VoiceStart` (typed identity and mode),
  `VoiceStop`, `VoiceSkip`, raw PCM bytes; server side `VoiceStt`,
  `VoiceAssistant`, `VoiceEvent`, `VoiceDone`, `TtsChunk`, and in duplex
  sessions `VoiceTurn` and `VoiceInterrupted`. PCM is raw 16 kHz s16le in both
  directions, exactly the WS binary frame the `/sync` forwarder relays.
- **The engine seam** (`voice-engine-seam`): remote-only. There is no
  in-process engine registry in argus-voice — STT/TTS/LLM compile to the
  remote HTTP adapters only (argus-stt 7030, argus-tts 7029, argus-llm 7032
  internal wires), so the binary never links sherpa-onnx/ONNX/llama.cpp
  engine code. Failure surfaces as the session's existing degrade paths
  (`stt_failed` reaction, logged TTS/LLM fallbacks), never a crash.
- **The identity write** (`GrpcVoiceIdentity`): the spoken-name persist is a
  typed `IdentityService.UpdateUser` call to argus-identity's gRPC listener
  (`identity.target`, `argus-identity:7040`). argus-voice owns NO database —
  zero DB clients. The identity feature owns identity.db and emits the change
  through its own NATS sink, whose payloads argus-sync's fan-out dispatches
  onto `/sync` (the publisher of that emit is the identity surface, not this
  service). An empty `identity.target` or a failed call logs and the
  session continues (best effort, as the legacy async persist was).

## Session behavior decisions (moved from code comments)

- While the assistant speaks, mic frames are dropped in a half-duplex session
  (the app mic picks up Argus's own audio; feeding it to the VAD would
  self-trigger). A duplex session keeps them and applies the barge-in rule
  instead (see "Full duplex and barge-in").
- RNNoise runs only when a batch is not below the silence RMS gate
  (`vad.denoise_gate_rms` and no recently-decaying voice) — that gate is
  where most CPU is saved when nobody talks. A bypassed batch breaks the
  denoiser's input stream, so the first batch after a bypass resyncs it
  (`NoiseSuppressor::resync`: resamplers and the 48 kHz backlog restart;
  the RNNoise noise estimate and the AGC are kept). A mute or a VAD reset
  resets the whole denoiser with the VAD (`resetListening`), so the AGC
  does not carry a level across a cut. The denoiser reuses its buffers
  (`AudioResampler::processInto`), and the worker keeps the batch and the
  cleaned samples in per-session buffers: the audio path allocates nothing
  once they have grown.
- A completed VAD turn runs STT → reaction → LLM stream → TTS per sentence;
  a turn failing never kills the worker thread (caught and logged), and
  `speaking` is cleared by the `SpeakingGuard` RAII so a TTS throw cannot
  leave the session deaf.
- The reaction frame is emitted BEFORE the LLM generates: the avatar reacts
  while Argus thinks.
- LLM tokens are appended verbatim (llama tokenization puts the space at the
  start of the next token; per-token stripping glues words) — only the
  model's `Argus:` preamble is stripped once from the accumulated buffer.
- TTS chunk boundaries: first sentence flushes at any `. ! ?` boundary;
  later ones need `kMinSentenceChars`; `, ; :` split only past
  `kClauseMinChars` with a look-ahead tail; run-on text is cut at the last
  space past `kHardMaxChars`.
- The conversation the LLM sees is `CallHistory`
  (`src/feature/voice/call-history.{hxx,cc}`), described in "The call's
  context" below. It is append-only so argus-llm can reuse its KV cache: its
  prefix cache only hits when the next prompt extends every token already
  decoded. That is why the stored reply is the raw generated text (`full`,
  not the spoken, prefix-stripped text) when nothing interrupted it, and why
  the history trims in one block instead of dropping a pair every turn, which
  re-prefilled the whole prompt on every turn once the conversation was long.
- A turn the LLM cannot answer is rolled back (the user message and its tone
  note leave the history) and a short localized line is spoken: silence
  after speaking was indistinguishable from a broken session.
- Every turn carries the session's `userId` and the call's `sessionId`
  (`voice-<userId>-<start ms>`, argus-llm's `ToolContext::sessionId`, the
  source reference of what memory stores during the call); without the user
  id the memory tools of argus-llm refuse, and a routed memory turn fell into the tool loop and
  cost several extra generations before any audio.
- Every turn also carries the session's role (`UserRole` from the
  `VoiceStart` identity) and language: argus-llm offers and runs only the
  tools that role may use (`role_access::hasAccess`), and an absent role on
  its wire is a Guest. Before this the LLM ran every voice turn as a Resident
  in Spanish, whoever spoke.
- The STT language is a field of each transcription request
  (`VoiceTranscribeInput::language`), not adapter state. The adapter used to
  be a process-wide static with a language setter, so a Spanish and an
  English session running at the same time overwrote each other's language.
- `voice:skip` (and `stop`) cancel the whole turn, not only the speech: the
  per-turn `turnStop` source feeds both the TTS calls and the LLM stream's
  `LlmStreamInput::cancellation`, which `TryCancel`s the gRPC call (or shuts
  the HTTP socket). argus-llm stops generating at the next token of a
  cancelled call and frees its slot, so the next turn does not queue behind
  an answer nobody will hear. A cancelled generation is logged at info, not
  as an LLM failure.
- A sentence's `voice:assistant` frame is sent AFTER its audio chunks, on
  purpose: the app treats `voice:assistant` as the signal to flush and play
  the PCM it has buffered for that sentence. Sending the text first would
  flush an empty buffer and play each sentence's audio one frame late, so the
  order stays as it is.
- Silero v5 takes `[64 context | 512 new]`; the context of the next window
  is the tail of the current one (`window_.end() - 64`).
- After a turn the VAD is reset but the PCM queue is NOT cleared: audio
  captured while the LLM was thinking may hold a real interjection.
- The worker waits for PCM at most 150 ms (`kIdleTick`) before it looks at
  its notices again, so a camera offer or an action correction is delivered
  even while the microphone is muted and no PCM arrives.
- A `VoiceStart` with `resume` set is a call that continues after the app
  lost its socket: the server side of the old call died with the socket, so
  the session starts again with the same identity but does not greet, and
  primes argus-llm at once. The app keeps its transcript and resends its
  notes and situation; the conversation history before the cut is gone.
- `voice:mute` drops the PCM queue and resets the VAD (the duplex worker
  owns the VAD, so the reset is a flag it applies), and PCM is ignored while
  muted. Before, the client only stopped sending; the server kept the
  half-said utterance open and finished it with whatever came after the
  unmute.
- Each turn logs `Voice: turn latency stt_ms=… llm_first_token_ms=…
  tts_first_audio_ms=… total_ms=…`, measured from the moment the VAD closed
  the turn (the endpoint silence, `min_silence_frames` × 32 ms, comes before
  that), so the budget of a slow answer is visible in the deploy's info log
  without a transcript.
- `RemoteVoiceTts` caches argus-tts's speed and sample rate for 10 s. Each
  sentence used to ask for both before synthesizing, two round trips per
  sentence on the path to first audio; the owner's speed setting still
  reaches the next call within 10 s.
- The PCM queue is a `SampleRing` of 30 s of 16 kHz audio; past that the
  oldest samples are overwritten. It used to be a vector that was erased
  from the front (about 1.9 MB moved per 10 ms frame when full) and swapped
  out per batch (a new allocation every batch). A worker stalled behind a slow turn cannot grow memory without
  bound, and audio that old is no longer an interjection worth answering.

## Streamed transcription

The STT request no longer waits for the end of the turn. When the VAD enters
speech, the session opens argus-stt's `TranscribeStream`
(`IVoiceStt::openStream`, `TurnTranscript` in
`src/feature/voice/turn-transcript.{hxx,cc}`) and pushes the utterance as
the VAD buffers it (`VadService::utterance()`). When the pause reaches a third
of the endpoint (`max(2, min_silence_frames / 3)`, 4 windows = 128 ms of the
384 ms endpoint) it sends `flush`; argus-stt decodes what it has while the VAD
is still waiting for the endpoint, and the audio of the rest of the pause is
held back. If speech resumes, the held-back pause and the new speech are
pushed and the next pause flushes again. At the endpoint the turn takes the
stream (`HeardTurn`) and `finish` returns the partial without a second decode
when nothing came after the flush. A turn the VAD discards closes its stream
unread. The stream is per utterance; in a duplex call the worker opens the
next one while the turn thread finishes the previous.

The unary request stays the fallback: on the HTTP leg (`stt.grpc_target`
empty) `openStream` returns nothing, and a stream that fails to open, push or
finish, or returns no text, is replaced by one `transcribe` of the turn's
samples. The turn latency log says which one ran (`stt=stream` /
`stt=unary`).

Measured 2026-10-04 (scratch prod argus-llm, argus-stt, argus-tts with Pocket,
prod argus-voice built from a clean worktree, the gRPC call harness, 32 turns
per variant, duplex): the server-side `stt_ms` (endpoint → transcript) went
from median 50 ms (mean 64–73) to 0, and the user-side time from the end of
the clip to the `voice:stt` frame from median 240 ms to 164 ms. The same
binary on the HTTP leg against the gRPC leg cut the turns to the same sample
counts, so the stream changes when the transcript is ready, not what the VAD
hears.

## The lead-in clause (bfb86590), measured

The first chunk of a reply also ends at a comma within its first 24
characters ("Estoy bien,", "Gracias,"). Same runs, the HEAD build against the
same build with those lines removed: turns whose reply opened with such a
clause reached the user's ear (the `voice:assistant` that plays the sentence)
at a median 1.39 s with the lead-in (1.14 s with streaming too) against
1.6–2.0 s for the same turns without it ("Estoy bien, gracias." spoken as one
chunk). The pause between the lead-in and the rest was 0 ms in 15 of 16 cases
(the rest was synthesized before the lead-in finished playing) and 1.04 s
once, behind a 6.4 s first token on a long answer. It stays. Over all turns
the first audible sentence is still dominated by the length of the first
sentence, because the app plays a sentence only when its whole audio has
arrived: median 2.6–2.8 s, p90 4.6 s.

A call harness that starts the next utterance right after Argus's reply
lost the first word on 1 to 4 of 32 turns in every variant (HEAD included):
the user's onset fell inside the playback estimate, where only the last 320
ms of pre-roll survive (see "Full duplex and barge-in"). It was the duplex
playback estimate, not the transcript path: the estimate started each
sentence when its `voice:assistant` was sent, after the whole sentence was
synthesized, while the app plays every `tts_chunk` as it arrives. The
estimate ran late by each sentence's synthesis time (2.4 s on a 69-byte
sentence), so speech that began after Argus had gone quiet still went to the
barge-in listener. The estimate now advances per chunk (see below).
Re-run of the same harness (2026-10-04, prod argus-voice, Pocket es-quality,
scratch engines): with the fix 0 of 44 mid-call turns lost their first word
(28 during a full build-all on the host, 16 on an idle host); the previous
binary lost 11 of 126 in the earlier runs and 2 of 16 in an interleaved A/B
on the idle host. The call's first turn ("Hola Argus") lost "Hola" in 2 of 4
loaded runs and in 1 of 4 idle runs of the previous binary as well: the
harness starts it 0.7-0.95 s after the greeting's playback ends, outside any
estimate, so that one is the VAD's onset after a stretch of digital silence
(five windows over 0.45 needed, ten windows of pre-roll), not playback.

## Full duplex and barge-in

`VoiceStart.mode` (`VoiceMode`, default `VOICE_MODE_HALF_DUPLEX`) selects
the session behaviour. Half-duplex is the behaviour described above, byte for
byte: the greeting and every turn run inline on the worker, PCM is dropped
while `speaking`, and no new frame is ever sent. The relay sends
`VOICE_MODE_DUPLEX` only when the app's `voice:start` carries
`{"mode":"duplex"}`, so the current app keeps the half-duplex wire.

A duplex session splits the work in two threads. The worker owns the audio:
it never stops reading PCM (`feedPcm` does not drop while speaking), runs the
denoiser and owns the VAD. Each assistant turn (the greeting included) runs
on the session's turn thread (`launchTurn`), so the VAD keeps running while
the LLM generates and TTS streams. Only one turn runs at a time: launching
the next joins the previous one, which is already finished or cancelled.
`processTurn` therefore does not reset the VAD in duplex; the worker owns it.

While the assistant is audible the worker does not run normal turn
detection: `VadService::listen` runs the model on each 512-sample window,
keeps a pre-roll of `pre_roll_frames + barge_min_frames` windows, and counts
consecutive windows at or above `[vad] barge_threshold` (0.7). When the count
reaches `[vad] barge_min_frames` (8 windows, about 256 ms) it is a barge-in.
That is stricter than normal turn start (`threshold` 0.45 for
`min_speech_frames` 5) because the mic also hears Argus: residual echo after
the device's own cancellation tends to be short or of middling probability,
and a self-interrupt is worse than a late one. A window below the threshold
resets the count, so blips never add up.

The count is only armed once the turn's first TTS audio has been sent and
`[vad] barge_guard_ms` (300 ms) has passed since: before that there is
nothing to interrupt, and the first instants of playback are where echo is
strongest. Before the guard the windows feed only the pre-roll.

"Audible" means the turn thread is still running or the estimated playback
has not ended. The app's playout is a stream: it plays every `tts_chunk` the
moment it arrives (`VoicePlayout.write`), and the server streams TTS faster
than real time. So `sendDuplexChunk` moves the estimate per chunk:
`playbackEnd = max(playbackEnd, now) + chunk duration`, which is the first
chunk's arrival plus the audio sent, and also models an underrun (a chunk
that arrives after the previous audio ran out starts when it arrives).
`voice:assistant` carries only the text. It used to add the sentence's
duration when it was sent, after the sentence was fully synthesized, which
put the end of playback late by the synthesis time; the seam suite pins the
fix ("Speech after the streamed audio has played is a new turn, however
late the sentence closed": 2 s of audio streamed over 1.2 s, speech fed
2.6 s after the first chunk reaches STT whole; on the old estimate it went
to the barge-in listener and no turn was heard). When the
assistant stops being audible without a barge-in, the barge-in counters are
cleared and normal turn detection resumes on the same VAD state, keeping the
last `pre_roll_frames` windows as the pre-roll (`VadService::endListening`).
It used to reset the VAD, which threw away the pre-roll: a user who started
talking as Argus finished lost the first word ("Pon la vigilancia en modo
noche" reached STT as "con la vigilancia en modo noche", so the command was
never recognized). A live call on scratch engines showed it on six of
fourteen turns. A `voice:skip` ends the estimate at once.

On barge-in the worker cancels the turn exactly as `voice:skip` does
(`interrupt` + `turnStop.request_stop()`, which cancels the LLM stream and
the TTS call), sends `voice:interrupted {id}` with the interrupted turn's id,
and puts the VAD straight into the speech state with the pre-roll as the
start of the utterance: the windows that triggered the barge-in and the ones
before them are the first syllables, and normal end-of-turn detection takes
it from there. A turn is interrupted at most once: the `barged` flag stops
listening until the next turn starts. Once barged, the turn sends no more
`tts_chunk`, no `voice:turn` and no `voice:assistant` (chunk, turn and
interrupted frames are sent under the same `duplexMutex`, so no audio of an
interrupted turn can follow its `voice:interrupted`); a `voice:assistant`
after the interruption would make the app flush and play stale audio.

Turn ids are per session and monotonic, starting at 1. A turn takes its id
when it produces its first audio, and `voice:turn {id}` is sent right before
that first `tts_chunk`; a turn that never produces audio (STT failed or was
empty) takes no id, so every id the app sees has a `voice:turn`. In duplex
each `voice:assistant` also carries `turnId`.

Duplex wire additions (all additive; the relay renders them):

| Frame | Direction | Payload |
|---|---|---|
| `voice:start` | app → server | optional `{"mode":"duplex"}` |
| `voice:turn` | server → app | `{"id": <int64>}` |
| `voice:interrupted` | server → app | `{"id": <int64>}` |
| `voice:assistant` | server → app | `{"text": ..., "turnId": <int64>}` (`turnId` duplex only) |

The VAD model is a seam (`VadModel`, created through `IVoiceVad` in
`VoiceEngineSeam`, Silero by default) so the suites drive barge-in with
scripted probabilities instead of the ONNX model.

## Two-stage interruption: duck, then barge (2026-10-07)

Android's hardware AEC treated the phone's own playback as the near-end
signal and suppressed the microphone while Argus spoke, so in an RTC call the
user's voice never reached the VAD and no barge-in ever fired. The frontend
now builds the WebRTC ADM with a software AEC, which hands the near-end
speech through during playback. That makes an interruption a two-stage
reaction on the server, because for the first frames a user who is only
thinking out loud looks exactly like one who means to take the turn:

- **Duck.** While a duplex call is listening, `VadService` counts windows at
  or above `[vad] duck_threshold` (0.5) and, after `[vad] duck_min_frames`
  (3 windows, about 96 ms), reports `ducking()`. The duplex loop forwards
  every change of that state to the sink
  (`VoiceSessionSink::duckPlayout(bool)`), and the RTC call's `PlayoutGain`
  ramps the outgoing audio down to 30 % over `duckFrames` 12 frames (about
  120 ms) and back up over `releaseFrames` 30 (about 300 ms). The ramp is
  applied per played 10 ms frame and each frame starts at the gain the
  previous one ended with, so it is continuous. When the user stops before
  `[vad] barge_threshold`, `[vad] duck_release_frames` (10 quiet windows,
  about 320 ms) lift the duck and the turn goes on. The duck belongs to
  armed calls only (the barge guard has elapsed), and it never survives the
  end of the microphone feed: every path that stops feeding mic audio
  (mute, a VAD reset, a stalled reader) releases it within one 150 ms idle
  cycle, because the empty-batch path of the duplex loop syncs the sink too,
  and a session teardown (`stop()`) releases it on the loop's exit before the
  join returns.
  `VadService::reset()` forgets a live duck with the rest of its state, so
  unmuting cannot re-duck for a few hundred milliseconds with nobody
  speaking.
- **Barge.** Eight armed windows at or above `[vad] barge_threshold` (0.7)
  still end the turn exactly as before: the LLM stream and the TTS call are
  cancelled, `voice:interrupted` carries the interrupted turn's id, and the
  call flushes what it has queued. What is already in the `AudioSource`
  (up to its 40 ms queue) plays out; the head of the ring (up to 60 ms) is
  sent as a tail whose first sample is scaled by the gain that was current
  when the flush arrived and whose last is silence, so the waveform
  continues instead of jumping at the cut; the rest of the ring is dropped
  and the gain returns to unity for whatever plays next. Bound on the tail
  after the decision: 40 ms of queue plus 60 ms of fade (the phone check is
  the measurement). The source
  keeps the buffered LiveKit mode — the SDK header names it for agents that
  generate audio independently of real time, which is what TTS does — and
  the queue is 40 ms rather than the default 100 ms; queue 0 is real-time
  mode and would need the producer to be paced by a real-time media source,
  which the ring-fed playout thread is not. The phone check must listen for
  underrun or crackle at 40 ms; the next step would be 60 ms, never 0.
  Because the blocking `AudioSource::captureFrame` cannot run without a
  LiveKit room, the fade capture itself is verified on the phone, not in a
  unit test; the gain math (including that the first faded sample continues
  at the current gain), the ramp continuity and the duck lifecycle are
  unit-tested (`voice-playout-gain-test`, `voice-session-seam-test`).

Two known differences are parked on purpose. `voice:skip` neither flushes
nor fades, so a skip keeps playing the queued speech (pre-existing,
app-initiated, and audibly a different scope from an interruption). And a
flush resets the sink's gain to unity without updating `session.ducked`, so
the session and the sink can disagree about a duck until the next
transition; no practical impact at the current call sites, where a barge-in
releases the duck anyway and a farewell ends the call.

## The spoken opening is opt-in (`[voice] opening`, 2026-10-07)

David: the greeting Argus spoke the moment a call was accepted was slow, heavy
and often unnecessary. It ran a TTS round trip before anything else, it held
the call in the assistant's speaking state for the seconds its audio lasted,
and the app now marks connect and disconnect with its own local earcons, so a
user no longer learns that the call is up from Argus's voice. The name question
moved into the conversation: a user who volunteers a name is still persisted
through `IdentityService.UpdateUser` and the call prompt asks for the name when
it needs it.

`[voice] opening` is `"none"` by default — in `config.toml.example`, in the
deploy template and in the code (`VoiceConfig::resolveOpening()`): an absent or
unrecognised value is `none` with a `LOG_WARN`, never a boot refusal. It is read
once per session start and is not a catalog key: the owner edits `config.toml`
and the next call reads it.

What the flag gates is the **default greeting**, never a line the caller asked
for. The one rule (`openingLineFor`, read by `start` and by
`VoiceSessionService::openingWillBeSpoken`) is:

- a resumed call says nothing at start, whatever the mode and whatever the join
  carried;
- else an explicit `VoiceStart.opening_line` is spoken, in *both* modes: it is
  the reason for the call, and a claimed proactive call (argus-notification's
  `call-<id>`, whose line argus-sync puts on the join) must not go silent — a
  panic or alert call that says nothing is a safety regression. argus-notification
  is the other half of that path: a proactive call nobody heard is reported
  `declined` and pushes its missed-call notification (`call-engine.cc`);
  argus-voice's half is that the line is spoken and that its playout drain marks
  `openingSpoken`, which is what `callOutcomeOf` turns into `completed`;
- else `opening = "spoken"` speaks the greeting for the call's language and the
  known name, once, added to the history — today's behaviour, byte for byte;
- else (the shipped `none`, no line) the session speaks nothing at all: no
  `history.addAssistant`, no `speak`, and the worker goes straight to `primeLlm`
  + `duplexLoop`/`workerLoop`, listening from the first batch.

**Upgrade note.** An installation whose `config.toml` has no `[voice]` section —
every install before this change — gets the silent default greeting on the next
call and needs `[voice] opening = "spoken"` to keep it. Nothing else about the
call changes: the earcons the app plays, the notes, the situation, barge-in, the
offers and the farewell lines are all unaffected.

`RtcCall` asks `openingWillBeSpoken` for its first `lk.agent.state`
(`rtc_wire::agentStateForOpening`) — `listening` when nothing will be spoken,
instead of sitting in `thinking` until `thinkingTimeout` (20 s) or the first
turn. The session start log line carries the mode (`opening=none|spoken`) and
the RTC line the decision under its own key (`speaks_opening=0|1`), beside
`carried=` for the line the join brought.

Time from the session accepting the call to the session listening, measured
2026-10-07 on the call of 18:10 UTC in `~/argus-demo/logs/` (duplex over WebRTC,
greeting spoken, `prod` argus-tts with Pocket): the greeting's stream in
`tts.log` is `chunks=71 bytes=988548`, 247137 float samples at the 48 kHz the
client announces, **5.15 s** of audio (the same log's cached farewell lines
cross-check the rate: `353508` bytes are cached as `es, 1792 ms`). The duplex
loop leaves the barge-in listener when the playback estimate ends, 5.15 s after
the first chunk, and nothing in the log contradicts it: the first `barge-in`
line is the *second* turn's, and the user's first words (the turn of 22528
samples that closed at 18:11:06.179, so 18:11:04.4–05.8) went to normal turn
detection, which needs the estimate over. The first answer's audio came at
18:11:06.902, 9.30 s after the session start, 3.55 s of it the user's own turn
and 1.65 s the silence before they spoke. With `opening = "none"` the same
session is listening before its first batch: the seam suite measures 22.7 ms
from `start()` to the first mic window the VAD processes (asserted under
500 ms), and the greeting's 5.15 s of audio and its TTS round trip are gone.

## The call's context

`CallHistory` owns the message list a turn sends to argus-llm and keeps one
rule: a user message carries only what STT heard. argus-llm's router, its
memory capture and the `utterance` its tools fall back to all read the last
user message, so anything else written into it ends up stored as something
the user said. The reaction tone note used to be appended to the user
message, and a routed `memory.remember` saved "recuerda que mi hermana viene
los domingos\n(Tono: cálido y corto.)".

- Entry kinds: the prompt (history[0], role `system`), notes and the
  situation (app context), events (what the app could not do), user, tone
  (role `system`, right after its user message) and assistant. argus-llm
  treats every `system` message after the first as a note, never as an
  utterance.
- Notes and the situation that arrive before the first LLM request fold into
  history[0]: nothing is cached yet. After that they are appended as
  `system` messages at the end, so the prompt stays a prefix of the next one;
  rewriting history[0] for every note re-prefilled the whole prompt (about a
  thousand tokens with the tool declarations) on the next turn. A repeated
  note or an unchanged situation adds nothing.
- The situation (`VOICE_CONTEXT_SITUATION`) is the app's own view of the
  house, built from what the signed-in user may see: guard mode, today's
  agenda, recent camera events and cameras that are offline. A new one
  replaces the old in the trimmed prompt; until a trim both stay in history
  and the newer one is simply the later message. argus-llm has no agenda or
  guard read, so this is how "¿qué tengo hoy?" or "¿está armada la casa?"
  gets an answer without a tool hop.
- The trim counts whole turns: past 10 user turns it keeps the last 5 and
  rebuilds history[0] as the persona, the notes, the latest situation and
  "Earlier in this call, oldest first:" with one line per dropped user,
  assistant or app event (at most 12 lines of 140 characters). The trim
  re-prefills from the first message anyway, so rewriting history[0] there
  costs nothing extra, and the call keeps what was asked, offered and
  confirmed in the turns it drops. Tone notes are not kept.
- argus-llm is primed once the greeting has been spoken and again after
  every trim: the same request a turn would send, with `prefillOnly`, on its
  own thread with the call's stop token (`callStop`, requested at hang-up).
  By the end of the greeting the app's first notes have arrived and folded
  into history[0], so the primed prefix is exactly what turn 1 extends, and
  the first answer decodes only the user's words instead of the ~700-token
  prompt. The priming never runs as a turn, so turn detection and barge-in
  are untouched; argus-llm skips it when a real generation holds the engine.
- An interrupted answer (barge-in or `voice:skip`) is stored as the sentences
  that actually produced audio, not as everything the LLM generated: the
  next turn must not assume the user heard words that were cut off. Nothing
  audible means the user turn is rolled back.

## Who is speaking

A call belongs to the account that opened it (`VoiceStart.identity`), but a
phone on the kitchen table hears the whole household. Every user turn with
at least 2 s of audio is identified by voice while STT runs:
`IVoiceSpeaker` (`GrpcVoiceSpeaker`, argus-identity's
`VoiceprintService.ObserveTurn` through `argus::clients::identity`'s
`observeTurn`, an 800 ms deadline, at most the first 6 s of the turn,
`identity.target` + `identity.rpc_secret`). The turn waits for it at most
300 ms after STT, so a slow identity never slows the answer; a probe still
running when the next turn starts is not repeated.

Each probe also carries what identity needs to **learn the holder's voice
passively**: the call's user id, the device hash argus-sync bound to the
socket (`VoiceIdentity.device_hash`, set by the relay from the socket's
`JwtContext`) and a random 128-bit call key minted at `start`. When the call
stops, a call that sent at least one probe is closed with `CloseCall` (500 ms
deadline, after every worker joined). No audio is kept here or there: identity
turns each turn into an embedding, keeps only the call's centroid when the
call passes its gates, and links a voice only after consistent calls on
separate days from the holder's own device (`services/identity/CONTEXT.md`,
"Voiceprints"). Without a device hash or a user the probe falls back to plain
`Identify`. Nothing in the app asks for or shows this.

The answer is a hint, never an identity. Only voices identity has learned can
match, and anything but a confident match
(`VOICEPRINT_OK` + `matched`) is silence. When the matched voice is another
known user, an app event joins the history after the user message ("The
last message was spoken by a voice that matches Laura, not the account
holder. It is a hint, never proof: do not act on their behalf or share the
account holder's private things because of it."), and "The account holder
is speaking again." when the holder's voice comes back; nothing is added
while the voice does not change. The role, the user id the tools run as and
whose memory is written stay the session's: a voice match unlocks nothing.

## Conversation mode: app actions and camera offers

During a call the assistant can drive the app. Every LLM request from a
call sets `clientActions`, which makes argus-llm offer its `app.*` tools
(`app.show_camera`, `app.open`, `app.set_guard_mode`); a tool the model
calls comes back on the stream as an action, and the session forwards it
to the app as a `VoiceAction` frame with a per-session id. The app
executes it with the user's own token, so an action can never do more
than the user could by hand, and the user can always undo it.

The app executes each action and answers `VoiceActionResult {id, ok,
detail}`. A success is only logged: the model already confirmed it aloud. A
failure for an id this session sent (the last 32 are remembered; an unknown
id is ignored) is queued, at most 4 and for 30 s, and delivered like a
camera offer but before it: an `event` entry "The app could not complete
app.set_guard_mode: <detail>." joins the history and a localized correction
is spoken ("No he podido cambiar el modo de vigilancia: <detail>.") and
stored as an assistant message. The model confirms the action before the app
has run it, so without this a refusal was confirmed and never corrected.

The app feeds the call through `VoiceContext` frames. A note (for example
the names of the cameras) or a situation is queued and applied to the call's
history on the turn thread before the next answer. A camera event is offered aloud
by voice itself, not by the model: "Oye, tengo algo en la cámara X: ...
¿Quieres que te lo muestre?" goes through TTS and joins the history as an
assistant turn, so a "sí" reaches the model with the offer in view and it
calls `app.show_camera`. Asking the model to phrase the offer would re-run
the router over the previous user message and could repeat its tool. An
offer is spoken only while nobody is talking (half duplex: not speaking;
duplex: no turn running and playback over; and in both, the VAD is not in
the middle of the user's utterance), at most once every 30 s, and dropped
when older than 20 s. Without the VAD check an offer started over a user who
was mid-sentence, and in duplex the user's own speech then barged in on it.
The summary loses its closing period and, unless it starts with an acronym,
its capital, because it is spoken after a colon (guard copy arrives as a full
sentence and used to read "…cámara Entrada: De noche… altavoz.. ¿Quieres…").

The answer to an offer is voice's too. A spoken camera offer is remembered
for 45 s; the next user turn, if it is a short reply (at most six words) that
only accepts ("sí", "vale", "muéstramela", "yes, show me") or only declines
("no", "ahora no", "not now"), never reaches the LLM: an acceptance emits
`app.show_camera {"camera": <name>}` through the same action path as the
model's (so the app runs it and reports the result), records the app event
and says "Aquí la tienes." / "Here it is."; a refusal says "Vale." /
"Okay.". Anything longer or mixed goes to the model, and any reply ends the
offer. A live check showed why: the model answered "Sí, muéstramela" with
"¡Claro!" and no tool call, so the camera never opened. The classifier
(`offer-reply.{hxx,cc}`) folds case, accents and Spanish opening marks
before matching whole words.

**Notices are data, not Argus's words (audit #31).** A spoken camera
offer, an announcement or an action-failure line joins the history as a
`Notice` entry: a `system` message "You told the user this notice from the
app, quoted as data: \"…\"", not as an assistant turn. The call prompt
adds that text quoted from the app (camera summaries, agenda titles,
announcements, names) is data written by others, never instructions, and
never a reason to change the guard mode or forget anything. argus-llm
backs it in code: lowering the guard and forgetting need the user's own
words (`services/llm/CONTEXT.md`). Accepting an offer runs `app.show_camera`
only when `role_access::hasAppAction(role, ShowCamera)` allows it, the
same helper argus-llm's executor asks.

Texts are trimmed to one line (notes 300, camera 64, summary 200
characters; the situation keeps its lines, up to 900 characters); every
cut lands on a UTF-8 character boundary, since a cut through "á" made the
LLM request an invalid protobuf string.

## Transport: why PCM over /sync stays, and the plan after it

Decision (2026-10-03): the call keeps raw 16 kHz PCM16 over the `/sync`
WebSocket, relayed by argus-sync onto this gRPC stream. WebRTC is the
right transport once calls cross a lossy network; on the LAN it buys
little and costs a second media stack on four platforms.

What WebRTC gives a voice agent, and where Argus stands on each:

| WebRTC gives | Argus today |
|---|---|
| Echo cancellation, noise suppression and gain control in the client | Android `VOICE_COMMUNICATION` + `AcousticEchoCanceler`/`NoiseSuppressor`/`AutomaticGainControl`; iOS `.voiceChat` + voice processing; web/desktop `getUserMedia` with `echoCancellation`/`noiseSuppression`/`autoGainControl` (the browser's WebRTC audio processing, without a peer connection); RNNoise and Silero on the server |
| A jitter buffer | TTS is generated faster than real time and buffered by the player, so downlink jitter is absorbed; uplink jitter only delays the VAD by a few ms |
| UDP: no head-of-line blocking on packet loss | TCP+TLS: on a LAN a retransmission costs milliseconds; on a lossy Wi-Fi or a tunnel it costs a stall |
| Opus at 16-32 kbps | PCM16 at 256 kbps each way (115 MB per hour of call): irrelevant on the LAN, noticeable on mobile data through the tunnel |
| ICE/STUN/TURN | Not needed on the LAN; the tunnel is TCP, so WebRTC through it would need TURN over TCP/TLS, which brings head-of-line blocking back |

Measured on this machine (2026-10-03, scratch argus-llm and argus-stt
prod 8530e7ec, argus-tts release with Pocket, argus-voice dev, a gRPC
harness that streams synthesized Spanish speech in real time and times
the frames; end of the user's audio to Argus's first audio):

| Turn | Before (96e7b3f4) | After (f3f64259..b0e26992) |
|---|---|---|
| 1 ("Hola Argus, ¿cómo estás hoy?") | 3456 ms | 823 ms |
| 2 (agenda) | 1635 ms | 1511 ms |
| 3 (cats) | 1523 ms | 1870 ms |
| 4 (guard mode) | 1602 ms | 1754 ms |
| 5 ("vale, gracias") | 660 ms | 1199 ms |

The first turn is the priming (argus-llm prefilled ~720 tokens while the
greeting played). Turns 2-5 differ by the reply the model chose, mostly by
the length of its first sentence; the per-turn log of the new build splits
them: VAD endpoint 384 ms (before the clock starts), STT 38-70 ms, LLM
first token 195-561 ms, first token to first audio 250-1290 ms (the first
sentence being written; Pocket's first chunk is ~90 ms in release, ~1 s
in a debug build at RTF ~2). The lead-in flush (bfb86590) attacks that
last term and was not yet in this measurement. The network is not where a
turn's time goes. End of speech to first audio is the VAD endpoint (`min_silence_frames`
× 32 ms = 384 ms), STT (40-60 ms), the LLM's first token (240-500 ms with
a warm prefix) and the first sentence plus its first TTS chunk.

Plan:
1. Done in this change: client-side audio processing on every platform,
   including desktop (web microphone + AudioWorklet player), resume of a
   call after a lost socket, and the server pipeline cuts (priming, cached
   TTS capabilities, call history that keeps the KV prefix).
2. Next, Opus over the same socket for remote calls. Negotiated, so the
   current PCM path stays the default and the fallback: `voice:start`
   gains `{"codecs": ["opus", "pcm16"]}`; argus-voice answers a new
   `VoiceReady {codec, sample_rate, frame_ms}` frame before the greeting
   (the relay renders it as `voice:ready`); until it arrives the app sends
   PCM, so an old server never receives Opus. Audio becomes length-prefixed
   20 ms Opus packets (5 per WebSocket message, 100 ms) in new
   `ClientFrame.opus` / `TtsChunk.opus` fields, so the relay stays a byte
   relay that picks the field from the negotiated codec. argus-voice links
   libopus (system `opus` 1.6 is present; the Conan recipe `opus/1.5.2`
   would put it in the root manifest), one encoder (VOIP, 24 kbps, 16 kHz)
   and one decoder per session. Clients: WebCodecs `AudioEncoder`/
   `AudioDecoder` where `isConfigSupported({codec: "opus"})` says yes
   (WebView2, recent WKWebView; WebKitGTK only with GStreamer's opus
   plugin), Android `MediaCodec` (`audio/opus` decoder from API 21, encoder
   from API 29), iOS `AVAudioConverter` with `kAudioFormatOpus`; anything
   else keeps PCM. Gain: about 10x less bandwidth; latency unchanged on
   the LAN.
3. Then WebRTC for calls through the tunnel, as an additive transport in
   argus-voice (libdatachannel 0.24 from Conan Center: ICE, DTLS-SRTP, an
   Opus track with its RTP packetizer), with SDP offer/answer and ICE
   candidates signalled over `/sync` as new `voice:rtc_*` frames on the
   same authenticated, device-bound socket. Clients: browser
   `RTCPeerConnection` (WebKitGTK only when built with GStreamer
   `webrtcbin`; otherwise stay on 2), `react-native-webrtc` 124 with
   `@config-plugins/react-native-webrtc` (the newest published pairing is
   for SDK 56; SDK 57 needs a check), and native AEC moves into the
   WebRTC audio module on mobile. The relay needs no media path: media
   goes peer to peer between the app and argus-voice. Without UDP reach
   (tunnel without TURN) the app falls back to 2, then to PCM.

Sources: LiveKit, "Why WebRTC beats WebSockets for realtime voice AI";
Pipecat/RTC League, "WebRTC vs WebSockets for real-time voice AI"; Expo
config-plugins `react-native-webrtc` compatibility table; libdatachannel
on Conan Center (0.24.0); WebKitGTK `enable-media-stream` and
`permission-request` (used by the desktop shell, frontend e4bc731).

## Realtime calls over WebRTC (LiveKit), 2026-10-04

The plan above (Opus over the socket, then a peer-to-peer WebRTC transport
of our own) was replaced by a decision with the owner: calls move to a
self-hosted LiveKit SFU, and argus-voice joins each call's room as the agent
participant. The PCM-over-`/sync` path stays, untouched, as the fallback
until WebRTC is proven on every platform; it is removed in its own change.
The contract every side codes against (token route, room and identity names,
data topics, call lifecycle, proactive calls) is
`agents/RTC-CONTRACT.md` in the coordination scratch; its stable parts are
restated here and in `services/sync/CONTEXT.md`.

**Why an SFU and not our own peer connection.** One media server for every
platform's mature client SDK (browser, React Native, the Tauri desktop
through the Rust SDK), echo cancellation inside the WebRTC audio module on
each client, Opus with a jitter buffer and congestion control, ICE over UDP
with an ICE/TCP fallback and an embedded TURN for later, and camera video can
ride the same rooms later. LiveKit server is Apache-2.0, a single Go binary,
about 20 MB resident idle on this machine.

**Integration: the official C++ SDK, prebuilt.** `livekit/client-sdk-cpp`
reached 1.0 in June 2026 and ships signed release archives per platform;
1.12.0 (2026-09-23) is pinned. It is itself a thin C++ layer over the Rust
SDK's C FFI (`livekit-ffi` 0.12.80, the same core the Python and Node agent
SDKs use), so it is the FFI option with a C++ API already written and
maintained upstream. Building the Rust FFI here instead would need a cargo
toolchain and a libwebrtc download in every image build; the archive is 13 MB
and links two shared libraries. `src/feature/rtc/CMakeLists.txt` downloads
the archive for the host architecture at configure time into
`third_party/livekit-sdk/` (gitignored), verifies it against
`livekit-sdk.sha256` (x64 and arm64 pins) and imports `LiveKit::livekit` as a
SYSTEM target, so `-Wall -Wextra` stays clean on our code. The SDK hides
its protobuf (no exported `google::protobuf` or `absl` symbol), so it does
not collide with the gRPC stack of this binary; it needs the system
`libcurl` and `libssl` and glibc 2.38 (Debian trixie has 2.41). The two
libraries are copied beside `argus-voice` after the link (`$ORIGIN`) and
into the image. License in `src/feature/rtc/NOTICE`.

**Who dispatches the agent.** argus-sync's `/rtc/token` route asks
`VoiceService.JoinRoom` (unary, `caller_sync`) before it answers the client,
with the room, the agent's own LiveKit token (minted by argus-sync, which
holds the only copy of the API secret), the user's participant identity, the
typed `VoiceIdentity`, the mode, `resume`, and for a proactive call its
`call_id`, `opening_line` and `call_kind`. JoinRoom answers once the agent
has connected and published its track, so a 200 to the client means Argus is
already in the room. A second JoinRoom for a room the agent is in answers
`already` (a resume inside the rejoin window). LiveKit's own agent dispatch
and webhooks were not used: they would need argus-voice to hold the API
secret and an HTTP listener, and the identity (role, device hash, language)
is argus-sync's to give.

**One call = one `RtcCall` (`src/feature/rtc/rtc-call.{hxx,cc}`), a
`VoiceSessionSink` like the gRPC stream.** The session service is shared:
`main.cc` owns one `VoiceSessionService` that both the gRPC streams and the
RTC calls drive, so the turn logic, the call history, barge-in, the camera
offers, app actions and the voiceprint probes are the same code on both
transports. Per call:

- a control thread connects, then waits for the user's microphone track and
  starts the session only once it is subscribed (the greeting, or the
  claimed call's opening line, is never spoken into a track that is not up);
  it publishes the outbound data messages and the agent state, and ends the
  call;
- a reader thread reads the decoded track (`livekit::AudioStream`, 48 kHz
  from the Opus decoder), downmixes, resamples to 16 kHz with
  `AudioResampler::processInto` (added for this: it reuses the caller's
  buffer, so the hot path allocates nothing once the buffers have grown) and
  feeds `VoiceSessionService::feedSamples` (the float entry `feedPcm` now
  shares);
- a playout thread takes TTS from a 120 s `BasicSampleRing<int16_t>` (the
  ring became a template) in 10 ms frames into a `livekit::AudioSource` with
  a 40 ms queue. `sendServerFrame` only copies a chunk into the ring, so the
  session's turn thread never blocks on the network while it holds
  `duplexMutex`; each chunk goes out as soon as it is synthesized, which is
  the lowest latency the TTS allows. A tail shorter than one frame is padded
  after 40 ms without new audio.

Barge-in is the session's own (duplex VAD listening, `voice:interrupted`;
the duck-and-fade staging is "Two-stage interruption" above): on the
interrupted frame the call keeps the queue playing and fades the ring head
into silence, so at most the source queue (40 ms) plus the ~60 ms fade are
heard after the decision. The session's playback estimate still advances per
chunk; it is a little early here (the source paces in real time) which only
makes the barge-in window shorter.

Data messages replace the `voice:*` text frames one for one (topics
`argus.stt`, `argus.assistant`, `argus.turn`, `argus.interrupted`,
`argus.action`, `argus.event`, `argus.done`; and from the app
`argus.context`, `argus.action_result`, `argus.mute`, `argus.skip`,
`argus.hangup`), reliable, JSON, addressed to the call's user only; data from
any other participant is ignored. `src/feature/rtc/rtc-wire.{hxx,cc}` is the
mapping, pure and tested. They travel on the room rather than on `/sync`
because the room is the call: ordered with the audio, gone with the call, and
no relay hop. Messages that arrive before the session started (the app sends
the camera names right after connecting) are queued, at most 16, and applied
right after the start, as the `/sync` relay does. Measured: the C++ SDK
delivers a packet sent in the first instant after the user connects with no
participant attached (it has not registered the sender yet), so until the
session has started a packet without a sender is accepted as the user's.
Nobody else can be in the room to send one: the room name carries the user
id and only argus-sync, for that user, mints tokens for it. Before this the
first notes and the situation of every call were dropped. `voice:assistant` no longer
means "flush and play": the audio is the track.

The agent's visible state is the participant attribute `lk.agent.state`
(`initializing`, `listening`, `thinking`, `speaking`), LiveKit's own agent
vocabulary, so the clients read it without a message of ours: thinking on a
final transcript, speaking on the first chunk, listening when the ring has
drained (600 ms after the last chunk, so the gap between two sentences does not flicker the state), on an interruption, and after 20 s of
thinking without audio.

**Ending.** `argus.hangup` ends at once. A user who leaves (a dead connection
the SDK gave up on) has 20 s (`[rtc] rejoin_grace_ms`) to come back with the
same identity and the call continues; after that, or when nobody joins within
60 s (`[rtc] first_join_wait_ms`), the agent sends `argus.done {timeout}` and
leaves. A participant removed by the server (session revoked, account
disabled) ends the call as `revoked` and no done message is sent. LiveKit
itself closes a room whose only participant is an agent (measured: the
agent of a token nobody used was sent away by the server a minute later);
when the user never joined that is a `timeout`, not an error. Measured on the
sandbox (2026-10-04, throwaway session of the owner made through the QR
flow): `/rtc/token` answered in 83 ms with the agent already in the room;
`PATCH /auth/logout` of that session removed the participant 57 ms later
(`PARTICIPANT_REMOVED`) and the agent ended the call as `revoked`. A
`call-<id>` call reports its outcome to argus-notification's
`CallService.EndCall` (`[notification] target` + `credential` =
notification's `caller_voice`): completed once the opening line has played
out, declined when the user left before, failed when the user never joined
or the call broke before.

**Announce.** argus-notification's call engine asks
`VoiceService.Announce {user_id, text, kind, call_id}` (`[grpc]
caller_notification`) before it rings a user. Every live session of that user
(WebRTC or PCM) queues the text as an announcement: it is spoken at the next
quiet moment exactly like a camera offer, before any camera offer, without
the 30 s spacing, dropped after 60 s, and joins the history as an assistant
turn. `delivered` is true when at least one live call took it.

## Latency over WebRTC, measured (2026-10-04)

Setup: scratch prod argus-llm, argus-stt and argus-tts (Pocket es-quality),
prod argus-voice at 4c6a33e8, dev LiveKit on this machine, the same
synthesized Spanish clips (`hola, agenda, gatos, modo, entrada, hermana,
gracias`), two rounds per path run alternately against the same argus-voice.
WebSocket path: the gRPC call harness (`measure.py`), which is the `/sync`
leg minus argus-sync's relay hop, timing the first `tts_chunk`; WebRTC path:
a LiveKit Python client publishing the clip as its microphone in real time
and timing the first decoded agent audio frame above an RMS of 300. Clock:
end of the clip.

| Path | End of speech -> first audio, median | p90 | Transcript after speech, median |
|---|---|---|---|
| WebSocket (first PCM chunk received) | 1699 ms | 1805 ms | 180 ms |
| WebRTC (first audio decoded at the client) | 1568 ms | 2783 ms | 253 ms |

Both are dominated by the model: the reply chosen (the length of its first
sentence) and the LLM's first token move a turn by more than a second, which
is why the two medians differ in the opposite direction of the transport.
The transport itself, measured inside the WebRTC runs, costs about 70 ms on
the way up (the transcript arrives 253 ms after the clip ends against 180 ms
over the socket: Opus encode, the SFU and the agent's jitter buffer and
resampling) and 58 ms on the way down (median from `argus.turn`, sent just
before the first chunk, to the first audible frame at the client; p90 100
ms). So on the LAN WebRTC adds roughly 130 ms per turn to what the raw
socket costs, but the user hears it as a stream with the device's own echo
cancellation, where the socket path only plays as fast as the app's player
drains (and the old half-duplex player waited for a whole sentence: 1.5-5.4
s here). Barge-in over WebRTC: the user's "Espera, espera" over a long
answer produced `argus.interrupted` 542-1191 ms after the clip's onset (the
VAD's 8 windows over 0.7 plus the clip's own lead-in), and Argus's audio
stopped 45-70 ms after that at the client (ring and source queue flushed).

## A revoked session hears why the call ends (2026-10-04)

David's request: when a session is revoked or closed, or the owner disables
the account, during a live call, Argus says a short line in the call's
language and then hangs up. Lines (`src/feature/voice/farewell-lines.cc`):
"Tu sesión se ha cerrado, cuelgo." / "Your session was closed, I'm hanging
up." (logout, refresh-token reuse, any other cause), "Han cerrado esta
sesión, cuelgo." / "This session was closed, I'm hanging up."
(`revokedByOwner`), "Tu cuenta está desactivada, cuelgo." / "Your account
was disabled, I'm hanging up." (`accountDisabled`). The first draft ("...,
voy a colgar.") measured 2.3-3.2 s in Spanish with Pocket and did not fit the
budget; the current ones measure 1.5-2.0 s.

- **No TTS wait.** `FarewellCache` synthesizes the six lines at boot on its
  own thread (`warmFarewells()` in `main.cc`), at 1.12x the voice's speed,
  trims leading and trailing silence (40 ms kept) and keeps them as 16 kHz
  PCM; with argus-tts down it retries every 15 s. A farewell asked before a
  line is cached plays nothing and the call is cut at once.
- **WebRTC** (`VoiceService.Farewell`, `caller_sync`): the call drops the
  user's audio and data from that instant, flushes whatever Argus was
  saying, publishes `argus.done {reason: "revoked", cause}`, and
  `VoiceSessionService::farewell` stops the turn (the LLM and TTS calls are
  cancelled), mutes the session, blocks every later frame of it and pushes
  the cached line (with its `argus.assistant`) into the playout ring. The RPC
  answers once the line has played out plus the source queue (120 ms tail),
  never later than 2.3 s. A second Farewell for the same call (a disabled
  account produces the user disconnect and then each session's revocation)
  waits for the first instead of cutting it. argus-sync has already revoked
  the participant's publish permissions and removes it right after the
  answer (`services/sync/CONTEXT.md`).
- **PCM over `/sync`**: a new `VoiceFarewell` client frame does the same on
  the gRPC stream: the line goes out as `tts_chunk` frames and a
  `voice:assistant`, the session ignores everything after it.

Measured on the sandbox with throwaway users (2026-10-04, scratch argus-voice,
prod argus-tts): logout mid-call: `argus.done` 46-52 ms after the request,
the line audible from then to 1.74-1.83 s, participant removed at 1.86-2.0 s;
owner disables the account: `argus.done` at 160-225 ms (identity's update
first), line to 2.2 s, room deleted at 2.37 s; PCM call: line frames at
195 ms, `sessionRevoked` and the close at 2.54 s.

**Which ends speak (2026-10-07).** All three `FarewellReason` values are ends
the server decided, not the user, and each keeps its short cached line:
`SessionClosed` (a logout, a refresh-token reuse, anything else argus-sync
names as a cause), `ClosedByOwner` (`revokedByOwner`: the owner closed the
session from another device) and `AccountDisabled` (the owner disabled the
account). The line is the cheapest honest way to end a call that is being torn
down: it is warmed at boot, so it costs no TTS round trip, and it reaches the
user on the call they are already listening to — an on-screen notice would need
the app to be showing the right surface at that instant, and it is not what a
call's client shows. A normal hang-up is not a farewell at all: the app's
`voice:stop` and `argus.hangup` go to `stop()`, which sends `voice:done` and
says nothing, because the user decided and does not need to be told. The
timeout ends speak nothing either — `timeout` while nobody joined, or after
`rejoin_grace_ms` for a user whose connection died — since there is no live
listener left to hear them. Every line the cache warms is therefore still
spoken by some path, and `FarewellCache` keeps warming all six.

## Owner settings

`src/feature/settings/voice-settings.cc` (`argus::voice-settings`) is the
catalog an owner may change through `argus.settings.v1.Settings`, registered
on the same gRPC listener as `VoiceService` (`argus::contracts::settings-wire`).
Basic: the default conversation language (`stt.language`, `es`/`en`, the
languages `voiceLangFromString` accepts), how easily the user interrupts
Argus (`vad.barge_threshold`) and how long Argus waits for the user to
finish (`vad.min_silence_frames`, 32 ms windows). Advanced: the remaining
turn-detection, barge-in and denoise keys of `[vad]`. Groups are
`conversation`, `listening` and `noise`. The `*.remote_timeout_ms` keys,
the remote URLs, the identity target and the caller secrets are plumbing and
stay out of the catalog.

Every key applies at the next session (`SettingApply::NextSession`): a
session builds its `VadService` (`resolveVadConfig()`) and reads its
denoise and barge-guard keys (`resolveVoiceListeningConfig()`) once in
`VoiceSessionService::start`, and the VAD state of a live call is tuned to
those numbers mid-utterance; swapping them under a running worker would mix
two configurations in one turn. Nothing needs a listener: the registry
persists to `config.toml`, and the next `voice:start` reads it.

A fallback is the value the service runs with when the key is absent, so
the code defaults match `config.toml.example`: `VadConfig` holds
`min_turn_ms` 240 and `min_mean_prob` 0.35, and an absent `vad.denoise`
means on. `voice-settings-test` pins every fallback against the resolution
functions. `voice.opening` is the one voice key outside the catalog (the app
does not offer it): `VoiceConfig::resolveOpening()` reads it per session start
and its fallback is `none`.

`[grpc] caller_settings` is the only credential the settings service
accepts (service name `settings`), and `VoiceService.Connect` accepts only
`caller_sync`: the settings caller cannot open a voice session and argus-sync
cannot change settings. An empty `caller_settings`, or one equal to
`caller_sync`, registers no settings service at all.

## Stream lifecycle decisions

- `Connect` requires argus-sync's caller credential before anything else.
  The identity in the metadata and in `VoiceStart` is taken as sent, so a
  caller that only had to name a user - the tunnel's example target was
  this listener, reachable from the internet - could hold a voice session as
  the Owner, read and write their memories through the LLM tools and rename
  them. A refused stream marks itself finishing before `Finish`, and
  `OnDone` stops the session and deletes the reactor without calling
  `Finish` again: a second `Finish` aborted the process, and the reactor
  was never freed.
- The spoken name is taken from "me llamo …", "mi nombre es …", "yo soy …"
  or "soy …" only when the phrase opens the utterance or a clause (after a
  comma, a full stop or "hola"), and only a name of at most three words;
  "yo no soy de aquí" used to rename the user "De aquí".
- The reaction engine reads the turn's text and whether STT failed; the
  signals no caller ever set (capture, recall hits, camera intent, system
  alert) are gone from `ReactionSignals`, and with them the branches that
  could never fire.
- What the user said and what Argus answers are logged at debug level
  only; info carries their length. A transcript is personal data and the
  reply can quote recalled memories, and the deploy keeps info logs.
- A second `VoiceStart` on a live stream is ignored; it used to replace the
  session and orphan the first one's worker thread for good.
- No gRPC callback blocks: the end of a stream (stop the session, which
  joins its threads, drain the writes for up to 2 s, `Finish`) runs on the
  stream's own closer thread, and `OnDone` hands the final stop and the
  `delete` to the Light blocking lane after joining that thread.
  `finished_` is atomic because `connected()` reads it from the session's
  threads.
- Shutdown: `main.cc` registers the gRPC server as a `shutdown_signal`
  drain. The stop request shuts the server down with a 2 s deadline (and
  then the RTC agent) on a thread of its own; the drain reports done when
  that thread has finished. It used to call `Shutdown()` with no deadline
  after `run()`, which waited for every live call.
- An `RtcCall` reports its end (`onEnded_`, the `EndCall` RPC) before it
  marks itself ended, so the agent service cannot reap and join it while
  the report is still running inside a `JoinRoom`.

- Identity metadata `x-argus-user` / `x-argus-role` must be present at
  `Connect` (UNAUTHENTICATED otherwise). Role validation happened ONCE on the
  `/sync` edge (the gateway's before sub-step 3a-1c, argus-sync's filter chain
  now); the service only requires presence and applies row scoping.
- The client SDK holds one write in flight with a bounded queue (frames drop
  past the cap) and a self-hold so the reactor outlives the consumer's
  handle until `OnDone`; the observer is owned by the stream for the same
  reason. Keepalive probes keep idle voice streams alive.
- Server side: one write in flight, bounded backlog (drop + warn), bounded
  drain before `Finish` so `voice:done` is not cut off, `endSession` runs on
  every terminal callback path and is idempotent.

## Wiring decisions

- `[server].grpc_port` (7034) carries VoiceService + `grpc.health.v1.Health`;
  `[server].health_port` (7035) carries the minimal `/health` HTTP listener
  for the compose healthcheck. No filters, no `/sync`, no db clients.
- `[identity].target` points at argus-identity's gRPC listener
  (`argus-identity:7040` in the deploy stack, `127.0.0.1:7040` natively).
- No alarm/siren trigger path exists here (voice reactions never carry alarm
  triggers); no camera frames, no sync-table CRUD, no schema.
- Cleartext loopback/internal-network gRPC is a documented F6 limitation;
  mTLS arrives with the eventual trust root.
- The canonical build is the service's standalone graph. From the repository
  root use `scripts/build-all.sh dev --only voice`; its CTest graph
  compiles the voice suites with `argus::voice-core`.

## Phase 4 step 9: config resolution into `src/config/` (D20)

The two listeners `main.cc` resolved inline are
`src/config/voice-config.{hxx,cc}` (`argus::voice-config`):
`VoiceConfig::resolveHealthListener()` (`ListenerConfig::resolve(7035,
"server.health_port")`) and `VoiceConfig::resolveGrpcListener()`
(`GrpcListenerConfig::resolve(7034)`). `main.cc` keeps `config.toml` loading
and `drogonConfig`; this service never reads `nats.url`.

## Pinned model download (2026-10-05 audit #30)

`scripts/provision.sh` downloads the model with `curl -fL` into a `.part`
file and moves it into place only after its SHA-256 matches the pin in the
script, from a pinned revision (no `resolve/main`). The pins were filled on
2026-10-05 from the Hugging Face LFS metadata and the GitHub release digest
of that revision, and checked against the files installed on the
development host. A present file whose hash does not match is replaced; a
pin left empty makes the script refuse to download and say so, and
`ARGUS_ALLOW_UNPINNED_MODELS=1` downloads it and prints its hash to pin.
