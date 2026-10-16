# Closing plan, 2026-10-16

The owner asked to close every open point as fast as possible. A fast implementer agent writes the code. The reviewer (this session) gives it the units, checks code quality and requirements on every patch, and runs the device tests with the owner. Merging to `master` stays fast-forward and follows the reviewer's acceptance.

## 0. Ground rules for the implementer

- **The rules.** The root `AGENTS.md` binds every change:
  - no comments of any kind;
  - parameter structs for 3+ arguments;
  - enums, never raw strings;
  - thrown refusals;
  - one module per folder;
  - the dependency tiers.

  The frontend follows `frontend/AGENTS.md` and `frontend/CONTEXT.md`.
- **One unit at a time.** A unit is one package or one service, or one frontend feature. Each unit ends with its own commit or commits, and with a report:
  - what changed;
  - the tests run, with their output;
  - the gates.
- **Builds: one at a time, with all cores.** Builds go through the RAM gate:
  - `flock -x -o ~/.cache/argus-phase2/heavy-one.lock ~/.cache/argus-scratch-backup/heavy-gate.sh <capGB> …`
  - export `CMAKE_BUILD_PARALLEL_LEVEL=16` for CMake;
  - pass `--max-workers=16` to Gradle;
  - run `./gradlew --stop` after every Gradle run.

  Never run two heavy jobs at once: the laptop froze on 2026-10-03.
- **Verification per unit.**
  - Backend: `./scripts/build-all.sh dev --only <project>` with 0 errors and 0 warnings, the route, deps and comment gates, and the project's tests.
  - Frontend: `bunx tsc` over `.` and `tests`, `bunx eslint src` clean, `bun test tests/unit`. Add the push JVM tests when native code changes, and a release APK build when the device needs it.
- **Commits.**
  - Message: English, one unit per message, with a `Report:` line and no attribution footer.
  - Branch: `integ/*` or a `fix/*` branch. Never commit directly to `master`.
  - Staging: explicit paths only, never `git add -A`.
- **Device work** is the reviewer's, with the owner's phone. The implementer never touches the phone or the owner's live stack.

## Phase 0: status audit (1–2 h)

The backlog in `realtime-and-backlog-plan.md` §3–§9 was written on 2026-10-12, and much of it has landed since: real time, mTLS on the internal callers, call delivery, reminder snooze and more.

**Task.** Produce `docs/history/reports/backlog-status-2026-10-16.md`: one row per item, with status `closed (sha)`, `open` or `partial (what is missing)`. The items:

- F1–F9;
- S1–S5;
- Q1–Q3;
- D1–D14;
- the §8 decisions;
- the queue of `REVIEWER-RESUME.md`.

**Rule.** Every claim is checked against `origin/master`: grep the code and read the commit.

**Housekeeping in the same unit:**
- list the branches whose commits all landed on `master` under new hashes, for example `fix/alert-intent-contract` and `fix/alert-silence-with-ring`, to delete after review;
- list the stale worktrees, for example `wt/frontend-telecom-voip`.

## Phase 1: calls, verified on the device (reviewer and owner, 1–1.5 h)

The code is on `master` (frontend `1d9025e`). The reviewer runs, with the owner:

1. Talking to Argus with the app in the background (assistant call).
2. Call settings: switch Speaker, Phone and Bluetooth; grant notifications from the panel.
3. Onboarding Continue: the system dialogs in a row, then "Todo listo".
4. Tapping the ongoing notification returns to the call; hang up from the notification.
5. A quick hang-up, three times: no answered call is left on the server.
6. A call over LTE.
7. The ring screen, dark and light.
8. Answered elsewhere: "taken" only for another device.
9. Hanging up before Argus speaks: no tray note.

Each failure becomes a unit for the implementer, with the logs attached.

## Phase 2: call quality (implementer, 1 day)

### 2.1 The reason in the first sentence

**Problem.** Today Argus sometimes opens with "Hola administrador" and gives the reason seconds later.

**Task.**
- Trace the opening line from the call's `openingLine` and reason (notification `call-copy.cc`, the push `reason`, `services/voice` first turn, `services/llm` opening act).
- Make the first spoken sentence carry the reason, with no extra LLM round trip before it.

**Acceptance:**
- the first TTS chunk contains the reason;
- its latency from answer is measured before and after on a prod build;
- a test pins the opening text.

### 2.2 The session survives a lost keystore key

**Status: owner decision first.** `expo-secure-store` deletes entries it cannot decrypt.

**Options:**
- a second encrypted copy outside the keystore-backed store;
- an explicit re-pair path.

Implementation follows the decision.

### 2.3 Desktop audio panel

The Tauri commands `argus_rtc_devices` and `argus_rtc_select_device` and the call settings panel exist.

**Task.** Run them in the desktop app, with WebKit Playwright and `mockIPC`, plus one real run through tauri-driver. Fix what fails.

### 2.4 iOS: CallKit and PushKit, prepared

**Task.** Build an Expo native module equal to `argus-push` on Android:
- PushKit VoIP push leads to a CallKit incoming call;
- answer and end map to the same JS call-control events;
- the audio session goes to CallKit;
- `UIBackgroundModes` gains `voip`.

**Status.** The code and config land, marked "unverified until a Mac build".

## Phase 3: guard vision prompt (implementer, 2–3 h)

**Problem.** LFM2.5-VL-450M described "an object that resembles a knife" on a frame where nobody stood. See `docs/history/reports/model-eval-bonsai2-2b.md`.

**Task.** Rework the guard prompt in `guard-assessment.cc` so a weapon is named only when clearly visible, and posture is described as seen.

**Acceptance:**
- 0 invented weapons across the 8 owner frames × 3 seeds, plus the existing fixtures;
- person recall not worse;
- latency unchanged.

## Phase 4: assistant quality (implementer, 0.5–1 day)

- **`llm-tier-eval`.** Memory selection measures 0.241 against the 0.6 gate. Find the cause in the context build, not in the model size, and raise it to the gate.
- **`call-faithfulness-eval`.** Bring its gates to green on the prod build. Numbers are reported through `scripts/measure-guard.sh`.

## Phase 5: the open backlog, after Phase 0 (implementer, 2–4 days)

The order follows `realtime-and-backlog-plan.md` §10, restricted to what Phase 0 marks open:

1. §3 Laya and GLiNER in the product.
2. F1 Argus-side recording.
3. F4 guard duress and PIN lockout.
4. F6 productivity remainders.
5. The open S items.
6. Q1–Q3.
7. The D items: tidy paydown per unit, stale docs, named cleanups.

F5 (the siren) waits for the owner's consent decision.

## Phase 6: the long programmes (each with its own plan, 6–8 weeks in all)

| Programme | Plan |
|---|---|
| F9 parallel conversations and F8 messenger | `parallel-conversations-plan.md` |
| Camera watchman | `camera-watchman-plan.md` |
| STT improvement: hotwords, cleanup, streaming, same latency | to write |
| ONVIF native detectors as hints, YOLO decides | to write |
| Frontend e2e with tester-army | to write |

Each programme starts with the "measure first" section its plan names, and lands as units under this plan's rules.

## Phase 7 (optional): Bonsai 2 2B as the speaking LLM

Only if the owner wants it. An experimental branch moves `services/llm` to the PrismML llama.cpp fork, then `call-faithfulness-eval` runs on Bonsai 2B against LFM2.5-1.2B. Adoption follows the model-artifact rules of `AGENTS.md` §13e.

## Decisions the owner holds

1. The session copy (2.2).
2. The siren and its consent (F5).
3. Phase 7, yes or no.
4. RAM floors per profile (14/7 GB), the fast Spanish model in Equilibrado, and `gpu_layers`.
5. Remote access fallback and the VLM gRPC cutover (§8 items 5–6).

## Review protocol (reviewer)

For every unit the reviewer:

1. reads the diff against the rules;
2. re-runs the unit's tests and gates;
3. checks that the acceptance stated here holds;
4. accepts the exact SHAs, then fast-forwards `master`.

A unit the reviewer rejects comes back with the concrete defects. Device-visible units close only after the owner sees them work.
