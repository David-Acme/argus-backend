# The watchman: Argus listens and speaks through the cameras

Status: design recorded with the owner on 2026-10-12, not started. It replaces F2 of
`realtime-and-backlog-plan.md` (variant A, an Owner-opened call) with the owner's wider goal, and it
is the "privacy and identity design first" that plan required for variant B. Web research was done on
2026-10-12; every external claim below carries its source in the research notes summarised in
section 9, and anything not measured on our own cameras is marked as an assumption to measure.

## 0. What the owner asked for

Argus behaves like a watchman who is always looking through the cameras. When someone in the house
says its name ("Argus", "Argos", and close variants), the camera that heard it best starts paying
attention and a conversation opens through that camera's microphone and speaker, as if it were a
call. Argus knows who is speaking (face and voice), sees the person (so "¿cómo me veo?" works), can
tell whether the person is facing that camera, follows the person from camera to camera while the
conversation lasts, never answers its own voice (echo), and behaves sensibly when the same person is
already talking to Argus on the phone.

## 1. What exists today (inventory, backend `88670973`)

- **Camera microphone.** PCMA (A-law) 8 kHz mono on both RTSP streams (`services/camera/CONTEXT.md:772`).
  The only path to speech recognition is guard's `Listen` RPC
  (`services/camera/src/feature/actions/camera-action-rpc-service.cc:780-980`): one ffmpeg per call,
  1–10 s, energy endpointing, one-shot STT. Nothing listens continuously.
- **Camera speaker.** The Tapo 8800 talk channel (`shared/services/tapo/tapo-talk-client.*`), A-law
  8 kHz, 120 ms packets, a per-camera line lock (`tapo-driver.cc:246-336`). Announce synthesises the
  whole text before speaking; live talk exists for the app (`feature/talk/camera-talk-service.cc`).
  The camera ducks its own mic while it plays, so the hardware is half duplex.
- **Voice engine.** `services/voice`: Silero VAD per stream, denoise, half/duplex turn loops,
  barge-in, streamed TTS at 16 kHz, per-turn speaker probe (`voice-session-service.cc`). A session is
  opened only by argus-sync's credential for an authenticated app user (`voice-rpc-service.cc:154-167`).
- **Identity.** Face identification for camera frames (`IdentifyPerson`, callers auth and camera),
  guard's encounter pipeline, the 3D-Speaker voiceprint (16 kHz model, a soft signal, caller voice
  only), household and per-user privacy flags (`household_privacy`, `user_privacy`, `PrivacyGate`),
  `CameraAudioPolicy` (household `camera_audio` and every user's consent).
- **No wake word anywhere.** sherpa-onnx's keyword spotter is vendored but not built.
- **History.** The monolith's lab (`labs/voice-test --camera`, validated live on the C225 on
  2026-08-09, "fluid, no false turns") proved the loop: RTSP mic → VAD → STT → LLM → TTS → 8800
  talk, echo handled by flushing ~780 ms of mic audio after each reply. The lab was deleted with
  `labs/` in `d2756952`.

## 2. Architecture

One sentence: **argus-voice grows an ear per camera; argus-camera stays the camera's owner and
exposes the microphone feed and a streaming speaker; identity answers who; the session belongs to
the person, not to the camera.**

- **Where the ear lives: `services/voice`, a new feature `watch`.** Voice already owns VAD, denoise,
  turns, barge-in and the session engine, and camera `AGENTS.md` rule 1 keeps speech engines out of
  argus-camera. The ear needs no new service.
- **Microphone feed.** argus-camera gains an RPC that opens a continuous audio feed for one camera
  (PCM 16 kHz frames over a server stream, fed by one persistent RTSP reader per enabled camera,
  reconnecting on loss), gated by `CameraAudioPolicy` plus the new watchman switch. Voice never
  learns a go2rtc URL or a camera credential (rule 27: through the client, never the infra).
- **Speaker.** argus-camera gains a streaming talk RPC over the existing `talkLine()` (the live path
  the app already uses), so voice streams TTS chunks as they are produced instead of synthesising a
  whole announcement first. Camera rule 10 ("audible actions only from guard") is amended to admit
  voice for a watchman session, with the same line lock.
- **Session.** voice opens a session itself for a watchman conversation (an internal opener beside
  the sync-credential `Connect`; voice rule 3 amended), bound to the identity resolved in section 4,
  with the conversation context owned by the person. The camera is only the current input and output.

## 3. The listening chain

1. **Always on, cheap:** per enabled camera, the 16 kHz feed → Silero VAD gate → keyword spotter
   (stage 1), tuned for recall. A RAM ring of ~2 s per camera; nothing is written to disk before a
   confirmed wake.
2. **Stage 1, the keyword spotter.** Start with **sherpa-onnx KWS** (already a dependency, Apache-2.0,
   open vocabulary: each variant is one line with its own boost and threshold, phone-level keywords
   in the zh-en model). No published false-accept rate and no Spanish model exist, so it is measured
   first (section 7). If it cannot reach about **1 false accept per hour per house at ≥95% recall**
   on our cameras, train a **livekit-wakeword** head (Apache-2.0, multilingual positives, measured
   0.08 FA/h at 86% recall for its own word) on Spanish TTS and family recordings passed through the
   camera chain (16 kHz → 8 kHz → A-law → back). Training lives in the `argus-ai` repo, never here.
   Rejected: openWakeWord's pretrained models (CC BY-NC-SA), Porcupine (online activation, licence
   terms), Snowboy and Mycroft Precise (unmaintained).
3. **Stage 2, confirmation on the same box.** On a stage-1 fire, the ring plus the next ~1 s go to
   argus-stt with the name as a hotword; accept only if a variant (edit distance ≤ 2) sits in a
   vocative position ("Argus, …", "oye Argus"). A short earcon plays on the camera only after stage
   2, so false fires stay silent. "Argu" alone never passes without stage 2 (prefix of "argumento").
   Every stage-1 fire is logged with its stage-2 verdict and scores, without audio, to tune thresholds.
4. **Arbitration between cameras.** The first stage-1 fire opens a 300–400 ms window; every camera
   that fired in it is scored (KWS score, keyword SNR over its own noise floor, stage-2 confidence)
   and only the best one opens the conversation. One server sees every camera, so this is a local
   decision, not a negotiation between devices (the way Amazon ESP works). The same wake heard at
   similar level on many cameras at once, or inside a TV exclusion zone drawn per camera, is treated
   as broadcast audio and dropped.
5. **Never self-trigger.** Argus's replies never pronounce its own name, and a stage-1 fire that
   overlaps that camera's TTS is checked against the text being spoken.

## 4. Who is speaking, and are they talking to Argus

- **Face first.** On a confirmed wake, voice asks camera for a 1–2 s main-stream window around the
  wake (the sub-stream makes a face at 3 m about 17 px; the main stream about 50 px) and identity
  matches the faces in it through the existing camera identification path. Assumption to measure:
  faces of 40–80 px at 2–4 m on the deployed Tapo models.
- **Which face spoke**, only when two or more faces are visible: an audio-visual active speaker
  model, **Light-ASD** (MIT, 1 M parameters, 77.5 mAP on faces under 64 px), run only on wake
  events. Its training data licence (AVA labels) must pass the licence register first, and it was
  trained at 25 fps on 16 kHz audio, so it is measured on our 15 fps / 8 kHz footage before use.
- **Voice as a soft signal**, never authentication (existing rule): the 3D-Speaker probe on the
  conversation's turns. The model is 16 kHz and camera audio is 8 kHz upsampled, so its scores on
  camera audio are calibrated separately from app audio.
- **Fusion.** Each signal becomes a calibrated log-likelihood ratio, weighted by its quality (face
  size and frontalness, SNR, utterance length); a missing signal adds zero. **Face and voice that
  disagree mean "unknown"**: Argus asks "¿quién me habla?" or answers at the lowest role. Unmatched
  people, children included, default to the lowest role.
- **Facing the camera.** Coarse head pose (frontal within about ±30°), from face landmarks plus
  `solvePnP` or a head-pose model whose weights pass the register; eye gaze is not usable at these
  face sizes (13.9° error at 30–60 px, and the common gaze weights are trained on Gaze360,
  non-commercial). Head pose is a soft boost to "addressed to Argus", together with the wake word in
  vocative position and the utterance going through the intent router. Google's Look and Talk works
  only within ~1.5 m; the closer analogue is Amazon's Conversation Mode (head orientation fused with
  acoustic directedness).
- **"¿Cómo me veo?"** The VLM describes the identified speaker's crop from the same window; the
  answer speaks about appearance only, never identity of other people in frame.
- **What a camera conversation may do.** There is no liveness at a distance (a photo on a TV, a
  recording), so a camera session never authorises a sensitive action: no disarming, no reading
  another person's or private data aloud, no changing settings or roles. Those keep the app with
  face login. The allowed set is a declared capability subset (`camera_watch` capabilities in
  `role-access.hxx`, intersected with the person's own), answered by the same tables as everything
  else. Panic and the safety phrase always pass.

## 5. Conversation, echo and hand-off

- **Half duplex first.** While a camera speaks, plus its measured return delay plus 300 ms, its ear
  and STT input are muted. This is the monolith lab's proven approach. Barge-in only when mic energy
  exceeds the expected echo (the TTS envelope through a per-camera learned gain) by about 10 dB and
  VAD agrees.
- **Echo cancellation later, measured.** The reference (TTS sent over 8800) and the echo (back over
  RTSP) travel two network paths through the camera's own clock, with an unknown, per-utterance
  delay of likely 200–1000 ms; the camera also drops about the first second it is sent. Linear AEC
  (WebRTC AEC3 via `webrtc-audio-processing`, BSD-3) needs that delay found first (cross-correlation
  of the TTS envelope against the mic per utterance). It comes after the delay is measured on our
  cameras and never blocks the feature.
- **Following the person.** The conversation keeps a rolling speech SNR per camera, on VAD-active
  frames of the conversation's speaker only (optionally confirmed by the voice embedding). Switch the
  active camera only when another one beats it by about 6 dB for 2 consecutive turns, never mid
  utterance or while Argus speaks, and hold the switch for about 10 s to avoid flapping. Only the
  input and output move; the context stays. Thresholds are assumptions to measure; camera gains
  differ per unit, so SNR is calibrated per camera.
- **Already on a call from the phone or desktop.** The cameras ignore that person's wake word while
  the call lasts (the phone's own loudspeaker near a camera would otherwise wake it): the face match
  or the voice embedding says it is the caller. A different person at a camera can still wake Argus
  in their own session. Offering to move the call from the phone to the camera is an owner decision
  (section 8).
- **Ending.** The conversation closes after a silence timeout or "gracias, Argus"; a maximum length
  applies, as for live talk today.

## 6. Privacy (Peru) and defaults

- Ley 29733 and its reglamento (DS 016-2024-JUS) exempt purely domestic processing by individuals.
  The ANPD video surveillance directive counts audio as camera data, keeps the domestic exemption
  unless recording is constant, reachable from the internet and covers common or public areas, asks
  for signage and 30–60 days retention, and forbids sound capture in a domestic worker's rest areas
  outright. Biometrics are sensitive data; consent for non-household people is written. This is not
  legal advice: whether the exemption holds with street-facing cameras or a domestic worker is a
  question for a Peruvian lawyer.
- **Defaults:** the watchman is a household switch, **off**, turned on by the Owner with an
  acknowledgement; each camera has its own watchman flag and a "no audio" flag the Owner must set for
  rest areas; each person can opt out of being recognised by face or voice in a camera conversation;
  no audio is kept before a confirmed wake and nothing at all beyond the conversation's own turns
  (which follow the existing voice retention); an earcon marks the start of every conversation; all
  processing stays on the device. It extends `CameraAudioPolicy` and `PrivacyGate`, and every switch
  is live over `/sync` (the real-time plan's identity items).

## 7. Measure before building

Nothing above is tuned by guesswork. A measurement unit comes first, on the deployed cameras, with
the owner's consent for the recordings:

- **M1** Keyword spotting: false accepts per hour on ≥24 h of household audio per camera (Spanish
  conversation, TV, kitchen), recall on ≥200 wakes from several household speakers at 1–5 m, for
  sherpa-onnx KWS with the variant list; CPU per camera stream.
- **M2** The speaker's return path: TTS sent → heard back on RTSP, delay and its spread per
  camera; the first-second drop; ducking behaviour.
- **M3** Faces: size in pixels at 1–5 m on each camera model and stream; face match rate on that.
- **M4** Voiceprint on camera audio: score distributions for the household at 8 kHz, against the
  app's 16 kHz.
- **M5** CPU and memory with N cameras listening (the budget rule: memory capped at 1 GB per model).

## 8. Decisions for the owner

1. The watchman's scope as written, off by default and turned on with an acknowledgement.
2. Who may wake it: household members only, or anyone (unknown people answered at the lowest role).
3. What a camera conversation may do (the `camera_watch` subset): proposed yes to reminders,
   calendar, questions, "¿cómo me veo?", camera view requests, panic; no to disarming, settings,
   roles, other people's data.
4. Phone call plus camera: the cameras stay quiet for the caller (proposed), or Argus offers to move
   the call to the camera.
5. The wake word variants ("Argus", "Argos", "Árgus"; "Argu" only through stage 2).
6. Recording household audio for M1–M4 with everyone's consent (kept in `argus-ai`, never shipped).
7. The order: after section 2 of the real-time plan (proposed), or interleaved.

## 9. Order and research notes

**Order:** W0 the privacy switches and capability subset (with the real-time plan's identity items),
M1–M5, W1 the camera microphone feed and streaming speaker RPCs, W2 the ear (VAD + stage 1 + ring),
W3 stage 2 and self-trigger guard, W4 arbitration, W5 the session bridge and half-duplex
conversation (the first owner-visible milestone: say "Argus" to one camera and talk), W6 identity
fusion and the capability subset, W7 hand-off between cameras, W8 the phone-call rule, W9 vision
("¿cómo me veo?", head pose, active speaker), W10 echo cancellation for barge-in. Each unit follows
the implementation protocol and the review protocol of the real-time plan.

**Research consulted (2026-10-12):** sherpa-onnx KWS docs and pretrained model list;
openWakeWord, livekit-wakeword and microWakeWord repositories; Picovoice pricing and benchmark;
Amazon's word-level wake word verification paper and AVS cloud verification docs; WebRTC AEC3
sources and `webrtc-audio-processing`; SpeexDSP manual; DTLN-aec paper; go2rtc issue #1008 (Tapo
talk start-up drop); Amazon ESP and device arbitration papers; Home Assistant voice satellites;
TalkNet, Light-ASD and LoCoNet papers and repositories; L2CS-Net, MobileGaze, ETH-XGaze, 6DRepNet,
MediaPipe face detection; Gaze360 licence; Google Research "Look and Talk"; Amazon Conversation
Mode; Apple device-directed speech papers (arXiv 2403.14438, 2406.09617); VoxCeleb face–voice fusion
(arXiv 1811.10813); 3D-Speaker models; Ley 29733, DS 016-2024-JUS, Directiva 01-2020-JUS/DGTAIPD.
