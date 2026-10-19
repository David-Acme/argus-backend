# The camera's own detectors as hints, YOLO always decides

Status: written 2026-10-19 from the closing ledger's row "ONVIF native detectors as hints, YOLO
decides" (`closing-plan-2026-10.md`), not started. The owner's rule (2026-10-14): a camera's native
detectors are useful as hints; YOLO always decides. Every finding below was read from the code on
2026-10-19 (backend `e19dfd19`); nothing has been built, run or measured for this document.

## 0. The rule

A **hint** is anything the camera itself reports about its scene: ONVIF motion, and the analytics
events some cameras publish (person, vehicle, line crossing, intrusion). A hint may:

- raise a camera's inference cadence (the operator looks more often, sooner);
- wake a camera that the operator has put to sleep (section 3.3, new);
- open or extend a recording episode in `motion` / `events` mode, as ONVIF motion already does.

A hint may **never**:

- publish `argus.camera.v1.object_detected` or any other event, notification or alert;
- change a severity, a rule of the nine-rule table, a dwell, an identity verdict or a zone;
- reach argus-guard in any form (guard's posture rejects "motion detected" alerts:
  `services/guard/CONTEXT.md`, "Site, camera context and episodes").

The detector, the zones, the privacy mask, identity and the nine-rule table stay the only path to an
event (`services/camera/CONTEXT.md`, "ONVIF motion events over PullPoint", ruling (a)). This plan
extends that ruling; it does not reopen option (b) (a `motion` subject for guard) or option (c)
(camera motion replacing the frame-diff gate), both still rejected.

## 1. What exists today

- **ONVIF client** (`services/camera/src/shared/services/onvif/`): SOAP over Drogon
  (`onvif-soap`, `onvif-transport`, `onvif-client`), PullPoint (`CreatePullPointSubscription`,
  `PullMessages` PT10S / limit 10, `Renew` at half-life of PT60S, `Unsubscribe`), Digest across
  connections, `Connection: close` on every events request and a guarded `shutdown(SHUT_RDWR)` of the
  socket, at most one events connection per camera.
- **Parsing** (`onvif-parse.cc`): exactly two topics, `RuleEngine/CellMotionDetector/Motion`
  (`IsMotion`) and `VideoSource/MotionAlarm` (`State`) (`kMotionTopics`, `kMotionItems`).
  `onvif-types.hxx` carries only `OnvifMotion{moving}`; `publishesMotion` reads
  `GetEventProperties`. **No analytics topic (person, vehicle, line, field) is parsed**, and the
  source token is ignored (one video source).
- **The subscriber** (`services/camera/src/feature/onvif-events/onvif-motion-{events,watch}.{hxx,cc}`):
  one event-loop thread for all cameras, a 5 s rescan of `CameraRepository::findLive`, one watch per
  enabled **non-Tapo** ONVIF camera on `OnvifDriver::endpointOf`, backoff 5 → 60 s, a 2 s drain.
  Switch: `[onvif] events = true` (`services/camera/config.toml.example`). Runs only while the
  operator runs and the surveillance module is active.
- **The board** (`services/camera/src/shared/services/camera-motion/camera-motion-board.{hxx,cc}`):
  one episode per camera, a 5 s hold, a 10-minute guardrail that mutes a stuck episode;
  `raisesCadence` is read by the operator; `onReport` feeds the recorder (`main.cc:529`).
- **The operator cadence** (`services/camera/src/feature/operator/camera-operator-service.cc`,
  `currentInferenceFps`, line ~533): `burst_fps` (10) for `burst_ms` after a person,
  `active_fps` (6) while tracks or pending events exist, `active_fps` while the board raises it,
  otherwise `max_fps_inference` (2). **There is no sleeping state**: every live camera is analysed
  at least at 2 fps all day. The frame-diff gate (`operator.motion_gate`, default `false`) is the
  only other cost saver.
- **Moves.** `CameraSceneLog` (`src/shared/services/camera-driver/camera-scene-log.{hxx,cc}`) records
  each PTZ move window; the operator and the health monitor pause during it
  (`services/camera/CONTEXT.md`, "Who aims the camera"). A camera's own motion detector fires on its
  own pan, so a hint inside a move window is noise by construction; today nothing filters it.
- **Tapo cameras are out.** Their ONVIF service on port 2020 offers PullPoint, but the Tapo driver
  drives them and the subscriber skips them. The Tapo local budget is **three connections, RTSP and
  ONVIF counted in** (`services/camera/CONTEXT.md`, "Two budgets bound every consumer"): the
  preloaded sub pull, the talk session and a `main` viewer already fill it.
- **Tests with no camera**: `services/camera/tests/unit/{onvif-client-test,onvif-motion-events-test,camera-motion-board-test,camera-operator-test}.cc`.

## 2. What is missing

1. Analytics topics (person, vehicle, line crossing, field intrusion) and a per-vendor topic table.
2. Any measurement of what a hint is worth: no hint log, no counters, no replay harness.
3. A sleeping cadence for the CPU the hints could save.
4. Move-window and IR-switch suppression of hints.
5. Tapo cameras, which are the owner's deployed cameras (C225), under their connection budget.

## 3. Design

### 3.1 One hint vocabulary

`onvif-types.hxx` grows `OnvifHint{kind, active}` with `kind` one of `Motion`, `Person`, `Vehicle`,
`LineCross`, `Intrusion`, `Tamper` (a camera-domain enum inside the feature that uses it, rule 25
"enums live where they are used"). `onvif-parse.cc` maps topics to kinds through one constant table,
`{topic suffix, item name, kind}`, so a vendor is a row, not a branch. The rows are filled from the
topic inventory of M1 (section 4), never from memory; Profile M's
`RuleEngine/ObjectDetection/*` and the vendor rule-engine topics are the expected sources, to be
confirmed on each camera's `GetEventProperties`. `Tamper` is parsed and only logged in this plan
(a covered lens is a health question for the monitor, not a hint).

### 3.2 The board stays one board

`CameraMotionBoard` keeps one episode per camera; a report carries its kind. Any kind opens or extends
the episode (same 5 s hold, same 10-minute guardrail), so the recorder's `motion` mode and the
operator see one rule. Two additions:

- **Suppression windows**: a hint received inside a `CameraSceneLog` move window (plus 2 s settle)
  or within 3 s of a day/night switch Argus commanded through camera-control is dropped (a switch
  the camera makes on its own is not reported today; M1 measures how often it fires a hint) and counted as `suppressed`.
- **Kind on the episode**: a `Person` or `Vehicle` hint lets the operator skip `motion_gate` for that
  window (the camera already says something is there), and nothing else. It never sets
  `burst_fps`: burst follows a YOLO-confirmed person only.

### 3.3 Sleeping cameras

A new cadence below idle, `objects.sleep_fps` (recommended 0.2, one frame every 5 s), used only when
**all** of these hold for that camera:

- its hint subscription is healthy (a pull answered within the last 2 × PT10S, the subscription is
  live and not in backoff);
- the camera has no `alert` zone (the operator already holds its zones through
  `feature/operator/zone-provider.{hxx,cc}`). The operator does not know guard's posture today; a
  later unit may let a camera with an alert zone sleep while its environment is not armed, which
  needs the posture through a client, never guard's database;
- the camera is not in a privacy mode or a move window (those already pause the operator);
- no track, pending event or episode is open.

A hint wakes it on the operator's next tick (at most one sleep interval; the operator's sleep is
made interruptible by the board's listener, so the wake is immediate, not up to 5 s; today
`runCamera` sleeps its whole interval with `drogon::sleepCoro`, which is fine at 0.5 s and not at 5 s). A subscription that
drops wakes it to idle at once and keeps it there until the subscription is healthy again for 60 s.
The periodic sleep frame stays, so a camera whose detector misses a person is still looked at.
Sleeping saves inference CPU only: the sub-stream pull stays preloaded (go2rtc), and the health
monitor's sampling is unchanged.

`sleep_fps` defaults to `max_fps_inference` (sleep off) and is turned on per camera only after the
camera passes the M2 gate (section 5): a camera row flag `hintSleep`, set by the Owner, audited and
live over `/sync` like the other camera settings.

### 3.4 Tapo cameras

The subscriber learns Tapo's ONVIF endpoint (port 2020, the camera account) through the catalog the
drivers already read. Because the events long poll is a fourth connection on a three-connection
device, a Tapo watch runs **only when M5 shows the long poll does not take a slot**, or else under an
eviction rule: the watch parks (and the camera cannot sleep) while a `main` viewer or a talk cycle
holds the budget, and resumes after. The eviction is the recommended default; M5 decides whether it is
needed.

### 3.5 What is observable

`/health` gains per camera: subscription state, hints by kind, suppressed hints, hours at each cadence
today, and wake latency (hint receipt → first analysed frame). One log line per hint at debug level
(camera, kind, receipt time, state), never the camera's address or account. No database table: the
hint is not domain data, and the measurement reads logs and counters.

## 4. Measure first

Before any behaviour changes (O0 and O1 are observation-only):

- **M1 Topic inventory.** `GetEventProperties` on every camera model at hand (the owner's C225 over
  port 2020, any generic ONVIF camera on the bench); the topic set, the items, whether analytics are
  enabled by default, and what each fires on in a 30-minute scripted walk (person, car, pet, tree,
  headlights, IR switch).
- **M2 Hint recall against YOLO.** Reference: Argus-side `continuous` recordings
  (`services/camera/src/feature/recording/`) replayed offline through the operator at a constant
  6 fps; every reference person or vehicle event is ground truth. **Hint recall** = share of
  reference events with a hint of any kind between 1 s before and 0.5 s after the event's first
  frame; reported per camera, day and night, by kind. At least 7 days per camera.
- **M3 Recall gain from cadence.** The same footage replayed at idle 2 fps with and without the logged
  hints applied: reference events found only when hints raise the cadence (short passes at the edge
  of the frame), and the time to the first event, median and p95.
- **M4 False-hint rate.** Hint episodes with no reference detection of any class inside the episode,
  over all hint episodes, by kind, per camera, day and night; plus the hours of raised cadence they
  cost. Suppressed hints counted separately (they show what the move and IR filters remove).
- **M5 Connection budget on Tapo.** With the sub pull, the talk session and a `main` viewer open, does
  a PullPoint long poll on port 2020 get a 401 or break another consumer? Measured on the live line,
  when it is quiet, never in a loop (the probe rule).
- **M6 CPU saved.** Operator process CPU (cores) and inference count per camera per day at idle vs
  with sleep, on the prod build, N = 1, 2 and 4 cameras.

## 5. Gates

- **Sleep may be enabled on a camera** only when M2 hint recall for person events is **≥ 99 %**
  over ≥ 7 days on that camera, day and night separately, and M3 shows the median time to the first
  person event under sleep + hints is no more than **+500 ms** over idle without hints.
- **Hints stay on by default** (they only raise cadence) when M4's raised-cadence hours do not exceed
  **4 h per camera per day**; above it, the kind that causes it is disabled for that model in the
  topic table.
- **CPU**: sleep is worth shipping when M6 shows **≥ 40 %** less operator CPU on a quiet camera.
- **No event changes**: on the replayed footage the set of published events with hints on equals or
  is a superset of the set with hints off, and every published event is a YOLO detection (a test
  pins that no code path from the board reaches the event sink).

## 6. Units

Each unit lands with its own acceptance, its own commit series with a Report line, the reviewer's
acceptance of the exact patch before merge, and `./scripts/build-all.sh dev --only camera` at
0 errors / 0 warnings. Device tests on the owner's cameras run at the end of the programme
(owner rule, 2026-10-19), against the dev stack only.

| # | Unit | Done when |
|---|---|---|
| O0 | Observation: the `/health` counters and the hint log line of 3.5, cadence transitions logged by the operator | counters visible with the scripted transport in `onvif-motion-events-test`; no behaviour change (the operator tests unchanged) |
| O1 | Topic inventory tool: log the full `GetEventProperties` topic set per camera on subscribe | M1 written into `services/camera/CONTEXT.md` per model |
| O2 | Hint vocabulary and topic table (3.1); board reports carry a kind; suppression windows (3.2) | SOAP fixtures for each topic row in `onvif-client-test`; fake-clock tests for move and IR suppression in `camera-motion-board-test`; a test that the board has no path to `IObjectEventSink` |
| O3 | Replay harness: recorded segments + logged hints → operator at a chosen cadence, offline, reporting M2–M4 | the harness reproduces the reference event set at 6 fps on a recorded day |
| O4 | Tapo on the subscriber with the eviction rule (3.4), behind `[onvif] tapo_events = false` until M5 | M5 recorded; no 401 on any consumer with the rule on in the live-line test |
| O5 | Sleep cadence (3.3), off by default, the per-camera `hintSleep` flag with audit and sync (frontend coordination: one toggle in the camera's settings) | the gates of section 5 met on at least one camera; wake latency on `/health` |
| O6 | Person/vehicle hints skip `motion_gate` for the window | M3 shows no lost reference event with `motion_gate = true` plus hints |

Order: O0 → O1 → O2 → O3 → (M2–M6 on the dev stack) → O4 → O5 → O6.

## 7. Risks

- Camera analytics differ by firmware and are often off by default; a model with no person topic
  still gets motion, which is today's behaviour.
- The Tapo budget is the hard constraint: a hint must never cost the owner a live view or a talk
  cycle. The eviction rule errs toward the viewer.
- Sleeping trades CPU for latency on the first frame; the alert-zone exclusion keeps the alert path at
  idle cadence or above.
- A camera clock is not read (receipt time only, ruling of 2026-10-13); M2's window absorbs one round
  trip.

## 8. Owner decisions (recommended options, recorded 2026-10-19)

1. **Hints never leave argus-camera.** Recommended over a guard-facing `motion` subject (option (b)),
   which would duplicate the person path with the noisiest signal.
2. **One board, kinds on the report.** Recommended over a second board for analytics, which would
   give the recorder and the operator two rules.
3. **Sleep is opt-in per camera, after its own measured gate**, and never on a camera with an alert
   zone (whatever the posture, until the posture reaches the operator). Recommended over a global
   switch.
4. **Tapo hints under an eviction rule** that yields to viewers and talk. Recommended over excluding
   Tapo, since the deployed cameras are Tapo.
5. **No app surface beyond the per-camera sleep toggle**; hints are diagnostics on `/health`.
6. Open for the owner when M1 reports: whether `Tamper` should later feed the health monitor as a
   "camera covered" signal (it would still not be an alert by itself).
