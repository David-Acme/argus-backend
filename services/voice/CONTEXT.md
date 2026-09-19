# argus-voice — CONTEXT

## Why the voice service exists

F6-3 v2 of the `migracion-microservicios` plan makes argus-voice the pilot
pure-gRPC service: the voice orchestration stack (one voice session per
client: VAD turn detection → STT → LLM chat → TTS synthesis, plus the
reaction engine and the noise/RNNoise path) leaves the legacy monolith and
speaks only `argus.voice.v1`. The mobile app keeps talking to the gateway
unchanged — the `/sync` voice wire is frozen, and the gateway renders the
typed server frames back into the exact JSON/binary the app expects.

## What it owns

- **The voice session** (`voice-session-service`), driven by the bidi
  `VoiceService/Connect` stream: `VoiceStart` (typed identity), `VoiceStop`,
  `VoiceSkip`, raw PCM bytes; server side `VoiceStt`, `VoiceAssistant`,
  `VoiceEvent`, `VoiceDone`, `TtsChunk`. PCM is raw 16 kHz s16le in both
  directions, exactly the WS binary frame the gateway forwards.
- **The engine seam** (`voice-engine-seam`): remote-only. There is no
  in-process engine registry in argus-voice — STT/TTS/LLM compile to the
  remote HTTP adapters only (argus-stt 7030, argus-tts 7029, argus-llm 7032
  internal wires), so the binary never links sherpa-onnx/ONNX/llama.cpp
  engine code. Failure surfaces as the session's existing degrade paths
  (`stt_failed` reaction, logged TTS/LLM fallbacks), never a crash.
- **The identity write** (`GrpcVoiceIdentity`): the spoken-name persist is a
  typed `IdentityService.UpdateUser` call to the gateway's internal identity
  listener (`identity.target`). argus-voice owns NO database — zero DB
  clients. The gateway owns identity.db and re-fans the change out on
  `argus.sync.v1.change` (the publisher of that emit is the gateway, not
  this service). An empty `identity.target` or a failed call logs and the
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
- The reaction tone note rides the tail of the request copy, never the
  system prompt (prefix must stay constant for KV reuse) nor stored history.
- After a turn the VAD is reset but the PCM queue is NOT cleared: audio
  captured while the LLM was thinking may hold a real interjection.

## Stream lifecycle decisions

- Identity metadata `x-argus-user` / `x-argus-role` must be present at
  `Connect` (UNAUTHENTICATED otherwise). Role validation happened ONCE at
  the gateway; the service only requires presence and applies row scoping.
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
- `[identity].target` points at the gateway's internal identity gRPC
  listener (gateway default `127.0.0.1:7040`).
- No alarm/siren trigger path exists here (voice reactions never carry alarm
  triggers); no camera frames, no sync-table CRUD, no schema.
- Cleartext loopback/internal-network gRPC is a documented F6 limitation;
  mTLS arrives with the eventual trust root.
- The canonical build is the service's standalone graph. From the repository
  root use `scripts/build-all.sh dev --only argus-voice`; its CTest graph
  compiles the voice suites with `argus::voice-core`.
