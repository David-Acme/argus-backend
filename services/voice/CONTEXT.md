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
  where most CPU is saved when nobody talks.
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
- The PCM queue holds at most 30 s of 16 kHz audio; past that the oldest
  samples go. A worker stalled behind a slow turn cannot grow memory without
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
ms of pre-roll survive (see "Full duplex and barge-in"). It is the duplex
playback estimate, not the transcript path.

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
has not ended. The server streams TTS faster than real time and the app
plays each sentence when its `voice:assistant` arrives, so each sentence's
audio duration is added to a `playbackEnd` estimate at that moment. When the
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
functions.

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
- What the user said and what Argus answers are logged at debug level
  only; info carries their length. A transcript is personal data and the
  reply can quote recalled memories, and the deploy keeps info logs.
- A second `VoiceStart` on a live stream is ignored; it used to replace the
  session and orphan the first one's worker thread for good.

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
