# Camera call: talk with Argus through the cameras, and know where people are

Date: 2026-10-17.
- Owner approval: "aplica todo lo recomendado y comienza a implementar todo lo propuesto". Develop fast, validate every flow, test at the end.
- Base: `camera-watchman-plan.md`, whose architecture, privacy section and owner decisions of 2026-10-12 still hold.
- This plan adds three things on top of it:
  - the first deliverable scope;
  - the edge cases found by research on 2026-10-17;
  - proximity, a new layer.

## Rules for the implementer

- Reuse what exists. There is no new microservice:
  - `services/voice`: a new feature `watch`, the ear and the conversation;
  - `services/camera`: the microphone feed RPC and the streaming speaker RPC;
  - `services/identity`: who is speaking;
  - `services/llm`: the context, the memory and the tools, as in phone calls;
  - `services/sync` and `services/notification`: the live view on the phone and the hand-off;
  - `services/guard`: alerts and presence.
- No comments of any kind. Rule 20 holds in every file and every delegated prompt.
- Enums, parameter structs, thin controllers and rule 27 hold as everywhere.
- The LLM never chooses a tool. It only speaks, as in calls today.
- Every rule below that names a number makes that number a config key in the owning service's `config.toml.example` and in its settings-wire catalog. The value given here is the default.
- Measurements that need the owner's voice and his real cameras (M1–M4 of the watchman plan) come after the build. Until then everything runs against recorded audio, generated audio and the fake camera.

## Hardware facts (measured or documented)

- **Microphone.** A Tapo camera has one mono microphone: PCMA at 8 kHz on RTSP. It cannot give the direction a voice comes from. Time-difference location across cameras is not feasible over Wi-Fi.
- **Half duplex.** The camera ducks its own microphone while its speaker plays. A conversation is therefore half duplex, the same as in the vendor app.
- **Start drop.** The Tapo talk path drops about the first second of audio (go2rtc #1008, still open). Argus speaks through the 8800 talk client, which today handles live talk and announcements.
- **No sound tracking.** No Tapo tracks sound. The C225 tracks only what it sees.
- **Motor.** A pan-tilt Tapo is driven through its local API (`motorMove` in degrees, presets). The settle time is about 1–2 s, measured in `services/camera/CONTEXT.md`.

## Scope

All of these ship, in the order of the units below:
- the watchman switches, the privacy controls and the legal document (watchman W0);
- the microphone feed and the streaming speaker (W1);
- the ear: VAD, the wake word stage 1, the ring buffer and the stage-2 confirmation (W2, W3);
- arbitration between cameras, plus proximity layer 0 (W4);
- the half-duplex conversation (W5);
- identity fusion with the guest and visitor sets (part of W6);
- the phone: a live transcript and "Continuar aquí" (W8 and the explicit-signal part of W11);
- proximity layer 1 (vision and PTZ), which needs `camera-sight-plan.md` B1 and B2;
- proximity layers 2 and 3 (BLE and radar, both opt-in);
- the app on Android, iOS and desktop.

Left for later, from the watchman plan:
- the addressee classifier (4b), trained in `argus-ai`; until it exists, the vocative rule of stage 2 stands in;
- active speaker detection (Light-ASD) and head pose;
- linear echo cancellation (AEC3);
- automatic hand-off signals 3 and 4;
- "¿cómo me veo?";
- following the person between cameras automatically mid-conversation. Layer 0 still picks the right camera at every wake.

## Edge cases and the required behaviour

### Several cameras, echo, self-trigger

1. **Arbitration.**
   - Collect stage-1 fires for 400 ms.
   - Score each camera on its wake score, its speech SNR over its own learned noise floor, and its stage-2 confidence.
   - Only the best camera opens the conversation; the others log `duplicate_wake`.
   - The first camera to fire does not win by being first.
2. **Broadcast media.**
   - The same wake fired at a similar level on three or more cameras within 150 ms is broadcast audio (TV, radio), so drop it.
   - A per-camera TV exclusion flag, set by the Owner, raises that camera's threshold.
   - Every stage-1 fire is logged, without audio, with its verdict.
3. **Own echo.**
   - While a camera plays Argus's voice, its ear is muted. It stays muted for a tail after playback (default 800 ms, then measured per camera, M2).
   - Barge-in needs both the wake word and the speaker's voiceprint. Plain VAD never barges in.
4. **Cross-camera echo.** The server knows everything it plays. While any camera speaks, every other camera of the same environment:
   - raises its stage-1 threshold;
   - drops any wake whose 2 s window correlates with the audio being played (envelope correlation at or above 0.6).
5. **Self-trigger by text.** Replies never contain "Argus", "Argos" or "Árgus". The reply guard rewrites them, and a test pins it.
6. **The start drop.** Prepend a 300 ms earcon plus 700 ms of silence before the first word, and pad 300 ms after the last word.
7. **A busy line.** When someone is talking through that camera from the app, Argus never forces the line.
   - It answers on another camera of the same room if one exists.
   - Otherwise it sends the person a push: "Te escuché en la cámara {nombre}, pero alguien la está usando; sigue en la app."

### Noise, distance, narrowband audio

8. **STT.**
   - The ear sends 16 kHz, upsampled from the camera's 8 kHz, to argus-stt with the household's names as hotwords.
   - Measure the WER of the deployed model on camera audio (generated speech passed through the camera chain: 16k → 8k → A-law → back, plus real recordings later).
   - If the WER is more than 20 % worse than on app audio, fine-tune on mixed-band data in `argus-ai`; that is a follow-up, not a blocker.
   - Bandwidth extension is adopted only if it measures better.
9. **Endpointing.**
   - VAD silence of 700 ms closes a turn.
   - An utterance that ends incomplete gets up to 2 s: a trailing conjunction or preposition, or a rising question with no verb.
   - The maximum turn length is 30 s.
10. **Overlap.** A turn whose voiceprint disagrees with the conversation's speaker and has no wake word in it is not for Argus: drop it. A different person at the same camera opens their own conversation only with their own wake word (section "Identity").

### Failures and latency

11. **Acknowledgement.**
    - An earcon within 300 ms of a confirmed wake.
    - When the first TTS chunk is not ready after 1.5 s, say "Un momento" or "One moment" once.
    - After 8 s with no answer, say "No pude responder ahora; inténtalo de nuevo o sigue en la app" and close the turn.
12. **The camera drops mid-conversation.** It covers a lost RTSP stream, a lost talk line and a reboot.
    - Continue on another camera of the same room when one exists, with the context kept.
    - Otherwise keep the conversation resumable for 3 minutes on the phone ("Continuar aquí"), and end it at that camera.
13. **Limits.** A conversation closes after 20 s of silence, after "gracias, Argus" or "eso es todo", or after 10 minutes.

### Identity and safety

14. **Face first.**
    - On a confirmed wake, grab a 1.5 s main-stream window and run identity on its faces. Faces below the match quality floor do not count.
    - The voiceprint is a soft signal. At 8 kHz far-field it is calibrated separately (M4), and a voice-only match never raises capabilities above the guest set.
15. **No face visible**, because the person has their back turned, is too far, or is backlit or in IR.
    - The guest set applies: general questions, the home's shared matters, reminders for oneself only after identification, and panic.
    - Argus asks "¿Quién me habla?" once, or offers "¿Seguimos en tu celular?". It never guesses a name aloud.
16. **Several faces.**
    - Without active speaker detection (deferred), several matched faces mean the guest set, unless exactly one face is large, frontal and moving its mouth. Otherwise ask.
    - A single matched face, together with other faces that are unknown, gives that member's set, but discreet: no personal details aloud ("te paso los detalles al celular"), per watchman 4e.
17. **Face and voice disagree.** Take the lower of the two sets and log the disagreement.
    - Twins and children are hints only.
    - A child's voiceprint is never enrolled without the consent of a parent who is a household member, and children's audio is never kept.
18. **Spoofing.** No sensitive action from any camera conversation, ever:
    - no disarming, no changing settings or roles;
    - no reading another person's data, no revealing presence;
    - no privacy unmask.

    Those go to the app as a confirmation on the user's own device. "Soy David" counts for nothing.
19. **Strangers.**
    - The visitor set applies: Argus says what it is, takes a message (an `assistant_task` notification), and panic always passes.
    - It never confirms or denies that anyone is home, or when anyone will be back.
    - It registers a visitor's face or voice only under `visitor_recognition`, with the visitor's spoken consent recorded in the conversation, and never for a person who says they are a minor.
20. **Duress.** A duress phrase in a camera conversation answers normally and neutrally aloud, and raises the silent duress alert (the existing duress path). Argus never says it is alerting.
21. **Changing camera re-identifies.** Members alternating at one camera are re-identified every turn.
22. **"Puedes hablar con ella."** For that conversation, the visitor may use what the resident delegates, within the floor of item 18. It ends when the resident leaves the frame or ends the conversation.
23. **Already on a phone call.** The cameras ignore a wake from a person who is in a phone or desktop call with Argus, matched by face or voice. One conversation per person.

### Privacy

24. **Switches.**
    - The watchman is off by default and turned on by the Owner with an acknowledgement.
    - Each camera has a watchman flag and a "sin audio" flag (for rest areas).
    - The household audio consent (`CameraAudioPolicy`) is required.
    - A lens-masked camera (`camera-sight-plan.md`, PM) neither listens nor speaks.
25. **No audio is kept** before a confirmed wake: a 2 s ring lives in RAM only.
    - The audio of a false wake is discarded at once, and only its scores are logged.
    - Conversation turns follow the existing voice retention. Only the Owner reviews them in the app, and a person can delete their own.
26. **An earcon starts every conversation**, because cameras have no listening light. The app shows a signage template for each entrance.
27. **`docs/operations/legal-and-privacy.md`** with the installer-responsibility text (watchman section 6), and the provisioning acknowledgement extended to cover it.

## Proximity: knowing where people are

Owner request, 2026-10-17: Argus should know which room a person is in and which camera is nearest, and turn a pan-tilt camera toward them.

### Layer 0: audio picks the room (no new hardware)

- Every camera with the watchman on keeps a running speech SNR (VAD frames only) over its own learned noise floor, with a per-camera gain calibrated over time.
- On a wake, layer 0 scores the cameras as in arbitration.
- During a conversation, it keeps the SNR of the conversation speaker's frames, voiceprint-confirmed, per camera.
- The SNR flows into the person-location filter below.

### Layer 1: vision confirms and aims (no new hardware)

- Person boxes from YOLO plus face matches from identity give each person a camera, and through the camera's environment and room, a room. A sighting holds for 45 s and then decays.
- At night (IR), person detection keeps its weight and face matches count half.
- **Aiming.** When a conversation opens on a pan-tilt camera and the speaker is not in frame:
  - turn toward the last direction a person was seen there, or step through the presets until YOLO finds a person;
  - never into a no-aim sector;
  - never while a person holds the PTZ lease;
  - never on a lens-masked camera.

  Once found, center the speaker with the move rule of `camera-sight-plan.md` B4 (degrees per pixel from the measured 15° ≈ 200 px, dead zone 5 %, settle 1.5 s). Then return home 30 s after the conversation ends.
- This needs `camera-sight-plan.md` B1 (lease and home) and B2 (operation-scoped move windows), which are pulled forward for it.

### Layer 2: the phone's Bluetooth (opt-in, per user)

- **Android.** While the user is at home (guard presence), the app's existing foreground service advertises a rotating per-user BLE token that the server can resolve. It stops when the user is away.
- **iOS.** It cannot advertise in the background. The server can resolve the phone's identity key (IRK) when it is offered, accepting that it goes quiet when the screen is off.
- **Receivers.** ESP32 boards running ESPHome as Bluetooth proxies, one per room. They are registered in the app with their room, and argus-guard reads them through the ESPHome native API or its web server events. This is a new adapter in guard's presence feature.
- **Without receivers**, layer 2 is simply absent.
- **Battery.** Advertise only while the user is home, at a low duty cycle. Measure the drain on the S21 at the device test.

### Layer 3: radar zones (opt-in hardware)

- An adapter for ESPHome LD2410 and LD2450 sensors and for the Aqara FP2, read through Home Assistant's or ESPHome's local API. It gives presence per zone, and for the LD2450 the x/y position of up to three targets.
- A sensor is registered to a room, and its zone or angle is mapped to a pan-tilt camera's pan, so the camera can aim at a voice before it sees the person.
- It carries no identity: radar says where, and vision or BLE say who.
- Built and tested against a fake sensor. It is proven on hardware only when the owner has a sensor.

### Fusion

- **A per-person room belief**, a discrete filter over the household's rooms. The evidence, strongest first:
  1. a face match;
  2. radar plus a person box in the same zone;
  3. the audio SNR with a voiceprint;
  4. BLE.
- **Decay and switching.** Evidence decays with time. The room switches only after the new room leads by a margin for 3 s, so it never flaps.
- **Where Argus speaks.** From the camera in the most likely room that also has a person sighting in the last 45 s.
- **Privacy.** The belief is Owner data, like household presence: guard rule, Owner only (owner decision 2026-10-12). It is never told to a stranger, and never exposed outside the server except to the Owner's app.
- **Owner-facing:**
  - "¿Dónde está María?" is answered only to the Owner, and only when María has not opted out of location;
  - every member can opt out of layers 0–3 being used to locate them, except for routing their own conversation.

## Units, order and parallelism

The implementer runs up to 5 agents in parallel in separate worktrees, cherry-picking into the batch. Every unit ships with its tests.

| Unit | Content | Depends on |
|---|---|---|
| CC0 | Watchman switches (household, per camera, "sin audio"), the camera and visitor capability sets in `role-access.hxx`, the legal and privacy document with its acknowledgement, the location opt-out | none |
| CC1 | camera: the microphone feed RPC (16 kHz frames, one persistent reader per camera, reconnect, gated by consent, watchman, mask and "sin audio"), the streaming speaker RPC (earcon plus padding, line lock, busy answer) | CC0 |
| CC2 | voice `watch`: VAD, KWS stage 1 (sherpa-onnx KWS built, with the variant list), the 2 s RAM ring, stage 2 (STT plus vocative rule), self-trigger and cross-camera suppression, the broadcast drop, the fire log | CC1 |
| CC3 | Arbitration plus proximity layer 0 (SNR, noise floor, per-camera gain) | CC2 |
| CC4 | The session bridge: half-duplex conversation (mute and tail, wake-plus-voiceprint barge-in, endpointing, the acknowledgement and failure rules, limits, a camera drop, a busy line), using the existing voice session engine and the llm turn | CC2 |
| CC5 | Identity: the face window, the voice soft signal, the guest, visitor and member sets with the discreet mode, disagreement, the duress and spoofing floors, strangers and consent, the phone-call rule | CC4 |
| CC6 | Phone: the live transcript over `/sync` (a new `(voice, watch)` resource or frame, additive, the audience being the speaker only), "Continuar aquí" make-before-break into the existing LiveKit call, resumable for 3 minutes | CC4 |
| CC7 | Proximity layer 1, with `camera-sight-plan.md` B1 and B2 pulled forward | CC3, B1/B2 |
| CC8 | Proximity layers 2 and 3 (the BLE token in the Android FGS, IRK, the ESPHome and FP2 adapters, the fusion filter, the opt-out, Owner-only queries) | CC3 |
| CC9 | App on Android, iOS and desktop: watchman settings and per-camera flags, the acknowledgement, the live conversation view, "Continuar aquí", BLE opt-in, receiver and sensor registration, location opt-out, signage template | CC0, CC6, CC8 contracts |

## Validation at the end: every flow, no bugs

A scripted end-to-end suite in the sandbox. The fake camera serves generated speech (TTS, passed through the camera chain) as its RTSP audio, plays whatever it receives on the talk channel into a capture, and records PTZ moves. Every flow below is one test, run on CPU decode:

1. "Argus, ¿qué tengo hoy?" on one camera: an earcon, the right answer, then 20 s of silence closes it.
2. The same wake heard on two cameras at different SNR: the louder one answers, the other logs `duplicate_wake`.
3. A broadcast wake on three cameras: no answer.
4. Argus speaking on camera A while camera B is near: B never wakes. A reply containing the name is rewritten.
5. "yo he implementado Argus" said mid-sentence: no wake (vocative rule).
6. No face, then a member face, then a stranger face: guest, then member, then visitor capabilities. The stranger asks "¿está David?": a message is taken, presence is not revealed.
7. A disarm request by camera: refused, and a confirmation is sent to the phone.
8. The duress phrase: a neutral answer aloud, the silent alert raised.
9. A busy talk line: another camera answers or a push is sent.
10. The camera dropped mid-conversation: continues on the same-room camera, or resumable on the phone.
11. "Continuar aquí" on the phone: the device leg joins, then the camera leg leaves, and the context is kept.
12. A person on a phone call: their camera wake is ignored, while another person's wake is served.
13. A lens-masked camera, "sin audio", consent withdrawn, or the watchman off: no listening at all, proven by no frames reaching voice.
14. PTZ: the speaker is out of frame, the camera turns to find them, centers them, and returns home after the conversation. It never moves into a no-aim sector, never against a person's lease, never while masked.
15. Proximity: face, radar, SNR and BLE evidence move the room belief with hysteresis. Owner-only queries are honoured, and an opted-out member is never located.
16. Latency: from wake to earcon at most 300 ms, and from end of turn to the first TTS audio p50 measured on the prod build. Report the numbers.

Then the owner's measurements M1–M4 on his cameras and voices, and the S21 device test.
