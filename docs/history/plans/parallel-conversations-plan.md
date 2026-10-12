# Parallel conversations, one per person, and Argus as messenger

Status: design written on 2026-10-12; the owner ruled on section 8 the same day; not started. It details F8 and F9 of
`realtime-and-backlog-plan.md` and is taken up with `camera-watchman-plan.md` once every point of the
real-time plan is finished. Facts about the current code come from an inventory of 2026-10-12; external
claims come from web research the same day (sources in section 9).

## 0. What the owner asked for

- Argus holds **one conversation per person, in parallel**: one user on the phone, another through a
  camera, a third on a laptop, all at the same time. Each person has their own conversation, like their
  own chat.
- **One conversation per account, following the person** between camera, phone and laptop with its
  context hot in memory, so a switch is instant and Argus knows "we were talking on the camera, now on
  the phone".
- Everything said in every conversation is extracted into **one shared memory**, so the system keeps a
  single global context of the home.
- **What a person tells Argus privately is never used in another person's conversation.**
- **Argus is the household's messenger**: "Argus, llama a David y dile que venga a comer".
- Several people may talk to Argus **in the same room** at once (one on their phone, another to the
  camera), and it must not mix them up.

## 1. What exists today

- **LLM:** one llama context, one sequence (`seq_id 0`), one engine mutex (`llm-service.cc:351`,
  `llm-service.hxx:182`). Admission slots let up to 1–4 requests in (`inferenceSlots()`), which then
  queue on the mutex with no bound but the caller's 120 s deadline; the prefix cache is per process, so
  two users evict each other and every switch re-prefills (`services/llm/CONTEXT.md:28-31`).
- **STT** decodes under one mutex (`stt-service.cc:236`); **TTS** takes one unfair timed mutex per
  prosodic chunk (`tts-service.cc:297-308`).
- **Conversation history lives in argus-voice, per session** (`CallHistory` in each `Session`, keyed by
  the transport sink, `voice-session-service.hxx:289`) and dies with it; a second device of the same
  user opens an unrelated session. Per user and device-independent today: llm's pendings (5 min) and
  the long-term memory graph.
- **Calls:** argus-notification's engine already enforces one *ringing* call per user and folds
  follow-ups into its opening line (`call-query.hxx:18-24`, `call-engine.cc:552-634`).
- **No cross-device self-echo protection**, and `announce(userId)` speaks into every live session of the
  user at once.

## 2. One conversation per person

- **`Conversation`, keyed by `userId`**, owned by argus-llm (the service that already owns pendings and
  memory): the turn history, a running summary of about 60 words in the user's language, the open
  pendings, the active entities, the last leg and its kind. It replaces argus-voice's per-session
  `CallHistory` as the source of truth; voice holds only audio state.
- **Legs.** A conversation has exactly one *active* leg (a camera, the phone, the laptop) at a time.
  When the same user starts talking on another device, **the new device takes over** (Google Meet's
  "Switch here"): it becomes the active leg, the old one hears "seguimos en tu teléfono" and stops
  capturing for this conversation. A camera is shared: it stops being this user's leg, it is never hung
  up on anyone else. A takeover is refused only when the new leg's speaker is not verified as the
  account holder (a camera turn whose voice or face does not match).
- **The switch note.** On a switch, one line goes into the context tail, in the user's language, where
  the memory block already goes: "La conversación continúa desde el teléfono; antes estaban en la
  cámara de la sala." The conversation itself continues; nothing restarts.
- **Hot context.** The authoritative state is text (turns, summary, pendings), persisted to argus-llm's
  database after every turn for crash recovery. The accelerator is the person's own KV sequence
  (section 3): kept in RAM while the person is active (LFM2.5's KV is about 12 KiB per token, so 24–48 MiB
  at 2–4K tokens), dropped to text after about 15 minutes idle, rebuilt with one prefill when they
  return. A switch is a pointer change of the active leg, so the target is under 100 ms from the
  takeover to Argus being ready to answer on the new device.
- **Calls** keep the engine's rule of one ringing call per user, and an answered call becomes that
  user's active leg. The hand-off of a live call between devices follows `camera-watchman-plan.md`
  section 5b (make-before-break, one LiveKit identity per leg, never two open microphones).

## 3. Parallelism on one server

- **LLM.** One model in memory, one llama context with `n_seq_max = 4` (one sequence per active
  conversation, `kv_unified = false` since conversations share no long prefix) and one scheduler thread
  in place of the request mutex. Each step builds one batch with a decode token for every active
  sequence plus prefill chunks capped at a few hundred tokens; round-robin between sequences, decode
  ahead of prefill, so nobody's speech stalls behind another person's long prompt. A sequence's state
  is saved and restored with `llama_state_seq_*` when it is evicted and comes back. CPU decode is bound
  by memory bandwidth, so batching amortises reading the weights; how much on our hardware is measured
  first (`llama-batched-bench -npl 1,2,4`, P1). LFM2.5 is a hybrid model (10 convolution + 6 attention
  layers), so multi-sequence batching and per-sequence save/restore are verified on tag b10305 before
  anything is built on them (P2).
- **STT.** One recognizer behind a queue; utterances ready within a few milliseconds are decoded
  together (`DecodeMultipleOfflineStreams`), never two threads in one recognizer.
- **TTS.** Synthesis is already split into sentence chunks; chunks are served round-robin per
  conversation through a fair queue, so a person's first audio waits at most one chunk of someone
  else's.
- **One budget, no product limit.** The LLM's lanes, STT and TTS share one machine; their thread
  budgets are planned together (`ThreadBudget`). The number of parallel sequences follows the hardware
  (`n_seq_max` from the machine, not a constant), and there is no product cap on conversations. When a
  machine is saturated, a new turn hears a short "un momento" and waits in a fair queue, never a 429.

## 4. Several people in the same room

- **Phone and laptop legs** are authenticated and near the mouth: what they capture belongs to that
  account. A voice that does not match the stored one on such a leg is not a rejection (a new
  microphone, a cold, a noisy room all change a voice): Argus keeps the person's identity, takes the
  device and its surroundings into account, learns the new conditions gradually, and at most asks,
  rarely and lightly, the way a person would ("¿estás resfriado? te escucho distinto"). It never asks
  twice in a short time and never makes the person prove who they are. Speech on such a leg that is far (low level against the user's own running level) and
  correlated with a louder copy on another leg is not theirs, and is dropped there.
- **Camera legs** do not know who speaks. Every turn of about 2 s or more is checked with the voice
  embedding (and the face, with the watchman); a short turn ("sí", "para") inherits the last verified
  speaker of that camera within about 10 s; anything else is answered as unverified, with no personal
  data (the rule Alexa Voice ID and Google Voice Match apply).
- **The same speech on two legs.** When two legs hear speech at the same moment, their envelopes are
  cross-correlated (about ±300 ms); if it is the same speech, the louder leg owns it and the other drops
  it. In practice the camera ignores a person talking into their own phone, and that phone ignores the
  neighbour unless the neighbour stands next to it.
- **Argus hearing itself on another device.** The server knows exactly what it is playing on every leg.
  It keeps about 3 s of each leg's playback and correlates every other leg's microphone against it
  (lag window about 500 ms for network jitter); a match is echo and never reaches VAD or STT. It does
  not try to subtract the echo (no shared clocks between devices); it only ignores it. A second gate
  drops a turn whose transcript matches text Argus just spoke on any leg. The phone and desktop keep
  their own WebRTC echo cancellation on, and the agent side enables it explicitly where it captures.
- **Not now:** personal VAD and target-speaker extraction (Google's Personal VAD, VoiceFilter-Lite,
  SpeakerBeam) have no deployable open model with ONNX today; WeSep (Apache-2.0) is revisited when it
  ships weights and an export.

## 5. One memory, private by default

- Every conversation feeds the memory graph through the extraction it already runs. Each fact carries
  its **scope**: *personal* (the speaker's own life, schedule, feelings, what they said about
  themselves) or *household* (the home, its devices, shared events, things said for everyone).
  Personal is the default; household is set when the fact is about the home or shared entities, or when
  the speaker says it is for everyone.
- **Recall is scoped by the listener.** A conversation recalls its own person's personal facts and the
  household's facts, never another person's personal facts. The system as a whole still knows
  everything (guard, the summary for the Owner of what concerns the home), but it never tells one person
  what is private to another.
- The Owner can review and correct a fact's scope in the memory screen.

## 6. Argus as messenger (F8)

- "Argus, llama a David y dile que venga a comer". The router picks a `relay` tool (the LLM never picks
  tools); its slots are the recipient (`text_norm::matchName` over the household) and the message in the
  sender's own words.
- **Delivery, best channel first:** David's open conversation if he has one; the camera that sees him
  (once the watchman exists); a call through the call engine whose opening line is the message ("Te
  llama Ana: dice que vengas a comer"); a notification if he does not answer.
- **The answer travels back.** "Dile que ya voy" reaches Ana's open conversation, or her notifications.
- **Privacy.** Only what the sender said travels, nothing from the sender's memory. Presence chooses the
  channel but is never revealed to the sender: Ana hears "se lo dije" or "no contestó, le dejé el
  mensaje", never where David is.
- **Never a ringing call for a message** (owner, 2026-10-12): a relayed message must not disturb. It
  arrives as a notification; if a camera sees the recipient, Argus says it there; if the recipient is
  already in a conversation or a call with Argus, the message is passed inside it ("Ana te manda decir
  que vengas a comer"). The call engine is never used to ring someone for a message.
- **Who may relay to whom** is a capability in `role-access.hxx`: every registered household member may
  send a message to another.
- **Strangers** (a visitor at a camera, the watchman's section 4c) may ask for someone ("¿está David?").
  Argus gives no information (not whether anyone is home, not where anyone is); it asks the visitor what
  they need, and notifies the resident that someone is asking for them, with the visitor's answer.

## 7. Measure first

- **P1** LLM throughput and per-user latency at 1, 2 and 4 parallel sequences on the target CPU.
- **P2** LFM2.5 multi-sequence batching and `llama_state_seq_*` save/restore on b10305.
- **P3** Voice verification on Peruvian Spanish at 8 kHz (camera) and 16 kHz (phone), by turn length.
- **P4** Cross-correlation of same-speech and self-echo across a phone and a camera, with real network
  jitter and the camera's G.711.
- **P5** CPU budget with N conversations, STT and TTS together.

## 8. Order and decisions

Order: P1–P2, the conversation store and the switch note (section 2), parallel LLM sequences, fair STT
and TTS, memory scopes (section 5), the messenger (section 6), then P3–P4 and co-located handling
(section 4) together with the watchman.

Owner decisions (2026-10-12):
1. A second device of the same person **takes over** the conversation by itself.
2. **Every registered person has their own account** and talks to Argus through it. A voice that
   sounds different on the person's own device is handled gently (section 4), never as an intruder.
3. Memory scope (section 5): **personal by default**, household when about the home or shared
   entities, or when the person says it is for everyone.
4. Messages never ring: notification, the camera that sees the recipient, or inside an open
   conversation (section 6). Strangers get no information; the resident is notified.
5. **No product limit on simultaneous conversations.** Capacity follows the hardware (a more powerful
   computer is planned; 4–5 users is the expected household); past it, the fair queue of section 3
   applies, never a refusal.

## 9. Research consulted (2026-10-12)

Google Personal VAD and Personal VAD 2.0, VoiceFilter-Lite, WeSep, SpeakerBeam-SS, TargetVoice;
sherpa-onnx (diarization, speaker id, `DecodeMultipleOfflineStreams`); 3D-Speaker ERes2Net/ERes2NetV2
EER by duration; near-field detection patents (US 12126971, US 10867619); multi-device echo
cancellation (Korse, Thiergart, Habets 2025) and summed-reference patents; Alexa's fingerprinting of
known audio; Alexa Voice ID, Google Voice Match, HomePod personal requests and device arbitration;
Alexa Conversation Mode; Google Meet "Switch here"; llama.cpp `llama.h` (`n_seq_max`, `kv_unified`,
`llama_state_seq_*`), llama-server parallel slots, prompt and RAM caches, batched-bench; the
LFM2.5-1.2B model card and config; LiveKit audio capture defaults.
