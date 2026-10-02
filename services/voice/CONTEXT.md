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
  `VoiceService/Connect` stream: `VoiceStart` (typed identity), `VoiceStop`,
  `VoiceSkip`, raw PCM bytes; server side `VoiceStt`, `VoiceAssistant`,
  `VoiceEvent`, `VoiceDone`, `TtsChunk`. PCM is raw 16 kHz s16le in both
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

- While the assistant speaks, mic frames are dropped (the app mic picks up
  Argus's own audio; feeding it to the VAD would self-trigger).
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
- The history is append-only so argus-llm can reuse its KV cache: its prefix
  cache only hits when the next prompt extends every token already decoded.
  That is why the reaction tone note is appended to the stored user message
  (a note sent once but not stored diverged the next prompt at that very
  message), why the stored reply is the raw generated text (`full`, not the
  spoken, prefix-stripped text), and why the history trims in one block —
  past 21 messages it keeps the system prompt and the last 10 (five whole
  turns) — instead of dropping a pair every turn, which re-prefilled the
  whole prompt on every turn once the conversation was long.
- A turn the LLM cannot answer is rolled back (the user message leaves the
  history, so roles keep alternating) and a short localized line is spoken:
  silence after speaking was indistinguishable from a broken session.
- Every turn carries the session's `userId`; without it the memory tools of
  argus-llm refuse, and a routed memory turn fell into the tool loop and
  cost several extra generations before any audio.
- Silero v5 takes `[64 context | 512 new]`; the context of the next window
  is the tail of the current one (`window_.end() - 64`).
- After a turn the VAD is reset but the PCM queue is NOT cleared: audio
  captured while the LLM was thinking may hold a real interjection.

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
