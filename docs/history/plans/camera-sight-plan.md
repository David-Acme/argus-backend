# Camera sight: tamper detection and the active look

Date: 2026-10-17. Requested by the owner, written by the reviewer session, implemented by the implementer session.

## What the owner asked

- **Loss of sight.** When someone covers, blinds or disables a camera, Argus says so: "perdí la visión de la cámara X". This is not privacy mode. The lens mask is the owner's choice, and it is covered by unit PM.
- **The active look.** On a camera that pans and tilts, Argus moves it to see a YOLO detection more clearly. It confirms or discards the detection before alarming, follows an intruder, and returns home afterwards.

Both parts follow the research below. Every number marked "measure" must be measured on the cameras before it is fixed.

## Research summary

### Tamper detection

- **Covering.**
  - Covered: global entropy and edge energy fall against a learned reference.
  - Partially covered: the same test per block of a grid. This is the standard family from Gil-Jiménez, Aksay/Temizel/Çetin and Ribnick.
  - Sources: Sağlam's METU thesis, Bosch's VCA tamper white paper.
- **Defocus.** Loss of edges or high-frequency energy while brightness stays normal.
- **Re-aim.** The stable reference edges shift by more than about 5 px.
- **A covered IR camera at night.** The IR LEDs reflect, and the image goes fully white. TP-Link's own Tapo FAQ treats that as a cover.
  - Bosch notes that its brightness-only check misses a close cover and a torch held to the lens.
- **Timing.**
  - Bosch checks every 10 s and needs two hits, so it alarms after 20–29 s.
  - Axis uses a "minimum duration" per detection, and relearns a scene after 10 minutes of the same view.
  - Axis pauses on a PTZ move, then waits 30 s to settle and 10 s to relearn.
- **False-alarm causes the vendors name:**
  - lights switched on or off;
  - the day/night IR switch;
  - headlights;
  - rain or snow drops;
  - sun reflections;
  - spider webs lit by IR.
- **Wi-Fi jamming and deauthentication.**
  - Police reports show jammers costing 10–50 dollars used before burglaries.
  - Losing several cameras at once is the signature. That is an inference from those reports; no vendor documents it.
- **Evidence leaves the house.** Recorders get stolen, so a pre-event clip and the last good frame must reach the owner before the recorder can be taken.
- **EN 62676.**
  - Detecting a masked or blinded camera is required only at grade 4.
  - Signal loss is required at grades 2–4.
  - Failures must be reported within seconds.
  - This comes from a TC79 slide, not the standard's full text.

### PTZ with detection

- **Frigate's autotracking does not support Tapo.**
  - It needs an ONVIF RelativeMove expressed as a share of the field of view, plus MoveStatus. Tapo has neither.
  - Tapo firmware updates have broken ONVIF pan/tilt several times.
  - So Argus drives Tapo through its local API: `relativeMove` steps, `motorMove`, presets, `manualCalibrate`, `getMotorCapability`, and `setTargetTrackConfig` for the camera's own tracking.
- **The C200/C210 class has no optical zoom.** "Closer" means re-centering the target and cropping the main 2K/3 MP stream.
- **No published Tapo figures** for speed, command latency, settle time or blur. They must be measured.
- **Frigate's proven rules:**
  - freeze detection and frame differencing while the camera moves;
  - calibrate move time against distance;
  - a dead zone of 3–8 % of the frame;
  - lead a moving target only when its velocity is consistent;
  - one move in flight at a time;
  - a return preset after a timeout, 10 s by default.
- **Commercial patterns:**
  - a return to the home point after 10–300 s (Reolink "monitor point", Hikvision "park time");
  - manual control takes priority over automatic control;
  - wear limits on tours (Amcrest);
  - zone masks stop matching once the camera moves.
- **TMA AVS-01** scores how verified an alarm is. Level 2 is "human presence confirmed or highly probable", the level a look should reach before it calls.

## What exists in Argus today

Verified on batch 6773a201.

- **Health monitor and tamper.**
  - The health monitor samples every 60 s.
  - `classify` gives Covered only for a dark frame with no texture.
  - A saturated white frame reads Bright, and Bright is not tamper.
  - Guard opens `camera_tamper` after 300 s (240 s for unreachable).
  - The alert is Critical in away, armed and night, and rings a call.
  - There is no clip, no last frame, no partial cover check, no re-aim edge test and no multi-camera rule.
- **PTZ.**
  - Tapo: step, move, stop and presets.
  - ONVIF: ContinuousMove, RelativeMove and presets.
  - Only Owner and Resident may move a camera, and only over HTTP from the app.
  - There is no lease between people or between people and Argus, and no home position.
  - `getMotorCapability` and calibration are never called.
- **The aim window.**
  - The aim window is one shared timestamp.
  - Any move, even one that returns home, drops the reference for about 120 s.
  - The operator does not know a move is in progress.
- **The operator.**
  - The operator runs YOLO26n on the sub stream at 2/6/10 fps, with one 0.45 threshold for all 80 classes.
  - Zones, the privacy mask and person tracks are all in pixels, so they stop matching once the camera moves.
  - There is no second look on the main stream.
- **Guard.**
  - Guard's only extra evidence is one person crop plus VLM describe and listen.
  - A belief below the threshold is just logged as `belief_suppressed`.
  - There is no "get more evidence" state, no PTZ RPC and no look tool.

## Part A: loss of sight (unit TP)

It runs after PM's tri-state fix, because a lens-masked camera is never tamper.

### A1. Detection per type

The recommended values are a synthesis of the sources above. Every window is owner-tunable through the settings wire.

| Type | Feature | Away / armed / night | Home |
|---|---|---|---|
| Full cover or spray | entropy and edge energy against the reference, very low | 15 s | 300 s notice |
| IR whiteout (covered at night) | 60 % or more of pixels saturated and edges gone | 15 s | 300 s notice |
| Blinding (torch, laser) | 60–70 % saturated and edge loss | 10 s | 120 s notice |
| Partial cover | 4×4 grid: 25–30 % or more of the textured reference blocks lost | 45 s | 300 s notice |
| Defocus | edge energy under 40 % of the reference, brightness normal | 60 s | maintenance notice |
| Re-aim | the stable reference edges shift by more than 5 px, or match poorly | 20 s | 300 s notice |
| Dark | underexposed. It is tamper only when it is sudden, with no day/night switch and no aim, or when it hits one camera of a group | 45 s | none |
| Signal loss | no fragments or health replies. Wired 15 s, Wi-Fi 30 s | as stated | 240 s notice |
| Several cameras at once | 2 or more cameras of one environment lost (by any type above) within 30 s | Critical at once | High, Critical if the household is away |

### A2. Avoiding false alarms

- Hold every check for 30 s after the day/night switch.
- Pause every check while Argus moves a camera, and for the measured settle time after (Part B).
- Relearn the reference after 10 minutes of a stable new view, or when the owner confirms it.
- Rain, fog, a dirty lens and IR spider webs produce slow blur, which sends a maintenance notice ("limpia el lente"), never an alarm.
- Headlights are ruled out by the windows.
- Never tamper:
  - a lens-masked camera;
  - a camera inside its own move window;
  - a camera whose module is off.

### A3. Cadence

- In protection modes, the checks read the operator's frames, which are already decoded when a stream is up. So a check runs at least every 5 s with no extra grab.
- At home, the 60 s sample is kept.

### A4. Evidence

- On onset, the recorder closes a clip with trigger `tamper`, which carries the pre-roll. It records in every mode except Off.
- The last good frame before onset is attached to the incident through the private storage capability.
- The app fetches and caches that frame and the clip when the push arrives, so the evidence survives the loss of the server.
- If the server itself dies, the phone's dead man's switch, which exists already, raises "Argus no responde".
- The copy, in the user's language: "Perdí la visión de la cámara {nombre}" plus the type ("tapada", "deslumbrada", "girada", "sin señal"). The multi-camera case says: "Perdí la visión de {n} cámaras a la vez".

### A5. Tests and measurement

- Run classify on synthetic frames for every row of A1, and over a day/night switch.
- Check the windows per mode.
- Check the multi-camera escalation: once per group.
- Check that onset requests a clip.
- Check that no tamper fires for a masked or aiming camera.
- Check the copy in es and en.
- Measure in the sandbox, never on the owner's cameras:
  - the time from cover to alert, per type;
  - the false-alarm rate over a recorded normal day.

## Part B: the active look (unit AL)

### B0. Measure first

This needs the owner's permission and presence, because it moves his real cameras.

- Per model and firmware, record:
  - the degrees per `relativeMove` step and per `motorMove` unit;
  - the latency from command to motion;
  - the settle time;
  - the delay until the image is free of blur;
  - the field of view, from `getMotorCapability`.
- Store the values as a per-model calibration table, and record the firmware version.
- Until then:
  - every stage below runs against a fake PTZ driver in the sandbox, which simulates FOV, speed and settle;
  - only read-only calls are made to the real cameras.

### B1. The lease and the home

- **A per-camera PTZ lease**, held by one of three, highest first:
  - a person moving the camera: Owner, Resident, or the Guard role while on duty;
  - a live viewer, only while that viewer holds the controls;
  - Argus.
- **A person always wins.** After the person releases, Argus waits a park time of 60 s.
- **Every automatic move is audited:** the reason, the target and the duration.
- **The home preset ("posición de vigilancia")** is set by the owner in the app. The active look is available only on cameras that have one.
- **Return home** after the timeout:
  - 10 s without the target while looking;
  - 30 s after a follow ends.
- **Drift.** After every return, the frame is compared with the stored home reference. On drift, run `manualCalibrate`, at most once a day, and notify on failure.
- **The camera's own auto-track** (`setTargetTrackConfig`) is switched off while Argus drives the camera, and restored afterwards.
- **No move** while the lens is masked, while the module is off, or while a person holds the lease.

### B2. Move windows scoped to an operation

- `CameraSceneLog` replaces the shared aim timestamp with one move operation: start, expected end, and whether it returns home.
- While an operation runs, the health checks, tamper checks, motion gate and dwell rules pause.
- An operation that returns home re-compares against the stored home reference. It does not re-baseline, so the camera has no 120 s blind spot after every look.

### B3. The second look, for every camera

This stage needs no motor.

- A doubtful detection triggers 2–3 grabs of the main stream, cropped around the box. YOLO runs again on those crops, and face matching runs on the main-stream crop: about 50 px at 3 m on main, against 17 px on the sub stream, as in the watchman plan.
- A detection is doubtful when:
  - its person confidence is between 0.25 and 0.45;
  - or its box is small (under 3 % of the frame);
  - or it is an unknown person in a protection mode.
- **Confirmed:** the observation goes on, carrying the better crop.
- **Not confirmed:** it is discarded, with a cooldown of 60 s per area.
- The budget is under 3 s. This helps fixed cameras as much as PTZ ones.

### B4. The look, for PTZ cameras

- **A new `Look` RPC** on the camera actions contract, called by guard:
  - input: `{camera_id, track_id, bbox, reason, budget_ms}`;
  - output: `{confirmed, class, confidence, crop ids, moved}`.
- **Centering:**
  - the box offset from the frame center, times the calibrated degrees per pixel;
  - a dead zone of 5 %;
  - a lead for a moving target only when its velocity is consistent;
  - one move in flight at a time.
- **Then:** wait the settle time, grab the main stream N = 3 times, and run detection again.
- **Budget:** 8 s for the whole look, wear limits included:
  - at most 6 moves a minute per camera;
  - a daily cap, logged.
- **Guard's verification state.** In a protection mode, a doubtful observation on a PTZ camera with a home preset asks for a `Look` before an alarm.
  - Confirmed: an incident, with the crops from the look.
  - Not confirmed: a discard, with a cooldown.
  - A clear detection never waits for a look. It alarms at once, and the look only adds evidence.
- **Privacy:**
  - Zone masks are valid only at home.
  - The owner draws no-aim sectors (pan/tilt ranges) in the app, such as a neighbour's window.
  - A look never centers into a no-aim sector, and frames taken away from home black out the parts that fall inside one.

### B5. Follow

- In away or armed mode, after a confirmed person, the camera follows that track while it stays in view, using the lead and the dead zone.
- One target only; it never switches mid-follow.
- It ends 10 s after the target is lost, then the camera returns home 30 s later.
- Recording continues the whole time, because the incident clip is open.

### B6. Patrol

This stage ships switched off.

- The patrol goes through presets, only in away or armed mode, and only on a schedule the owner sets.
- A tour runs at most once every 10 minutes, with the daily wear cap.
- It pauses for any viewer or person lease.

### B7. The assistant

- **Commands:** "mira hacia la puerta" and "gira la cámara de la sala a la entrada" are rules plus slots, never chosen by the LLM:
  - `camera.goto_preset` takes a preset by name;
  - `camera.look` takes a direction.
- **Who:** Owner, Resident, and Guard on duty.
- **Lease:** a person's lease applies to the assistant's moves too.

### B8. App (Android, iOS, desktop)

- **Setup:** the home preset, and the no-aim sectors. To draw a sector, move the camera and mark where it starts and ends.
- **Toggles per camera:**
  - "Mirada activa": on by default for PTZ cameras with a home preset, in protection modes;
  - "Seguir intrusos": on in away or armed;
  - "Patrullar": off.
- **While watching:** an "Argus está mirando" indicator on live view, and the lease state ("Tú controlas la cámara" / "Argus la está usando").
- **Incident card:** the look's crops and the clip.

### B9. Tests and measurement

- Use a fake PTZ driver with a simulated FOV, speed and settle.
- Test the mapping from box to angle, the dead zone, the lead and the wear caps.
- Test the lease priority and the park time.
- Test that the look never centers into a no-aim sector.
- Test that a look causes no tamper, no health re-baseline and no 120 s blind spot.
- Test the verification state machine: confirmed, discarded and cooldown.
- Test the return home and the drift check.
- On a recorded set, measure:
  - the false alarms avoided by B3 and B4;
  - the look latency, p50 and p95;
  - the moves per day.

## Order

1. TP (Part A), after PM's tri-state fix.
2. AL-B1 and B2: the lease, the home and the move windows, on the fake driver.
3. AL-B3: the second look on the main stream, which helps every camera.
4. AL-B4: the look and guard's verification.
5. AL-B5: follow.
6. AL-B7 and B8: the assistant and the app.
7. AL-B6: patrol, shipped off.
8. AL-B0: the measurement on the real cameras, when the owner is home. It sets the calibration table and the final windows.

## Owner decisions taken as recommended

- The Guard role may move a camera only while on duty. Today Guard cannot move one at all.
- A clear detection alarms at once. Only doubtful ones wait for a look.
- Patrol ships switched off.
- Automatic privacy by presence (the Nest style) is still left for later.
