# Closing plan, 2026-10-16

The owner asked to close every open point as fast as possible.

**Roles:**
- **Implementer session, `backend-3e`.** Writes the code and splits the work across its own subagents.
- **Reviewer session, `backend-37`.**
  - Checks code quality, logic and requirements on every patch.
  - Re-runs the tests.
  - Runs the device tests with the owner.

Merging to `master` stays fast-forward and follows the reviewer's acceptance of the exact SHAs.

**Out of scope: Bonsai 2 2B.** It was tested on 2026-10-16 and is kept as a documented alternative (`docs/history/reports/model-eval-bonsai2-2b.md`). It is not implemented under this plan.

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

## 0b. How the implementer splits the work

- **Parallel subagents in worktrees.** The implementer runs up to 4 subagents, one per independent unit, each in its own `wt/` worktree on its own `fix/*` or `feat/*` branch.
  - Units that touch the same files never run in parallel.
  - Every subagent prompt restates the no-comment rule.
- **Builds are the serial resource.** Code writing runs in parallel; heavy builds and test runs queue through the gate one at a time, each with all 16 cores.
- **Per-unit cheap checks** happen inside each subagent: tsc, eslint, unit tests, and the single-project build. The full `build-all.sh dev` runs once per integration batch.
- **Integration.** The implementer rebases accepted units onto `integ/backend-candidate` or `integ/frontend-candidate`. It sends the reviewer the batch SHAs with each unit's report, never merges to `master`, and never pushes `master`.
- **Parallel tracks that do not collide:**
  - A: Phase 0, the audit (docs only).
  - B: Phase 2.1 (`services/notification` and `services/voice`).
  - C: Phase 3 (`services/guard`).
  - D: Phase 4 (`services/llm` evals).

  Then Phase 2.2–2.4 and the F5 app side as frontend tracks, and Phase 4b's backend units. Phase 5 starts after the reviewer accepts the Phase 0 table.
- **Reporting.** Every unit report states:
  - the branch and SHAs;
  - the files touched;
  - the commands run, with their exit codes;
  - the acceptance criterion of this plan, and the evidence that it holds.

## Phase 0: status audit (1–2 h, one subagent)

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

**Likely cause, to confirm with a test.**
- `services/notification/src/feature/call/services/call-copy.cc` builds `openingLine`. Some call kinds start with `greeting()`, which ends in a period ("Hola, X. "), and put the reason in the next sentence. Others use `reasonGreeting()` ("Hola, X, …"), which keeps it in one sentence.
- `services/voice/src/feature/voice/voice-session-service.cc` `speakOpening()` splits the line with `splitSentences()` and speaks only what fits `kOpeningBudgetMs = 4000` (`opening-budget.hxx`). The rest goes to `laterLines`, released later as announcements.

**Task.**
- Every call kind's opening carries its reason inside the first sentence, through `reasonGreeting()` or an equivalent.
- The budget never splits a greeting from its reason.
- No extra LLM round trip before the first sentence.

**Acceptance:**
- a table-driven test over every call kind in `call-copy.cc` asserts the first sentence contains the reason;
- an `opening-budget` test shows a greeting is never spoken alone while the reason waits;
- the first TTS chunk contains the reason;
- its latency from answer is measured before and after on a prod build;
- a test pins the opening text.

### 2.2 A session lost to the keystore is recovered cleanly

**Decided 2026-10-16: no second copy of the session.** Expo's guidance is that values restored from Auto Backup cannot be decrypted once the Keystore key is gone, so an unreadable value means signed out.

**Task:**
- **Exclude from Auto Backup every store whose key lives in the Android Keystore.** That is the `expo-secure-store` entries (verify the generated rules) and `argus_push_keyset_prefs`. Do it through a config plugin, because `android/` is generated.
- **Record the exact cause when a stored session disappears.** Log the `SecureStoreModule` warning and the key name, and emit a `restore` trace with a reason.
- **Make the re-entry one step.** The app shows "Vuelve a entrar con tu rostro" and goes straight to face login, keeping the pairing. No pairing code is asked again while the instance trust is intact.

**Acceptance:**
- uninstall, reinstall and cold start on the S21: no unreadable-key error, and push registers;
- a forced unreadable token leads to face login with the pairing kept;
- tests cover the restore-reason mapping.

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

- **`llm-tier-eval`** (`services/llm/tests/eval/llm-tier-eval.cc`, gate `selection.memory.accuracy` in `gates.json`). Memory selection measures 0.241 against the 0.6 gate. Find the cause in the context build, not in the model size, and raise it to the gate.
- **`call-faithfulness-eval`** (`call-faithfulness-eval.cc`, cases `services/llm/tests/fixtures/eval/call-faithfulness.jsonl`). Bring its gates to green on the prod build. Numbers are reported through `scripts/measure-guard.sh`.

## Phase 4b: owner decisions turned into units (implementer, 1–1.5 days)

### F5: siren button in the app (decided 2026-10-16)

**Behaviour, following Arlo and Doro:**
- tapping Sirena opens a confirmation;
- confirming starts a 5-second countdown with Cancelar;
- the siren then sounds through the camera's existing siren action (`hasCameraAction`), and Detener is always one tap.

**Owner setting.** `guard.app_siren_enabled`, off by default, with its consent text. Roles follow the camera action table; an inactive role or a disabled module refuses as usual.

**Recording.** Each activation and stop writes an audit event and a `ResourceChanged` for the camera.

**Acceptance:**
- unit tests for the countdown and cancel;
- a route test for the role and module refusals;
- a device check with the owner.

### Settings profiles (decided 2026-10-16)

- RAM floors: 14 GB for "Máxima calidad", 7 GB for "Equilibrado".
- "Equilibrado" uses the quality Spanish model; the fast model belongs to "Máximo rendimiento" only. The precedence is precision > speed > memory.
- `gpu_layers = 0` is written explicitly in product config and profiles (CPU-only, decided 2026-10-09).

The data change goes in `services/settings` profiles plus its tests.

### Remote access when the relay is down (decided 2026-10-16)

- At home, the app always talks to the server directly over the LAN (mDNS).
- Away from home with the relay down: the app shows its cached data with the offline notice and retries with exponential backoff. Writes are refused with a clear message, never queued silently.
- No second relay for now.

**Acceptance.** A two-device test with the relay stopped.

### VLM gRPC cutover (decided 2026-10-16)

`services/vlm` moves to the same internal gRPC shape as stt, tts and llm, through its wire client. The frozen HTTP wire stays only where a caller still needs it, and is listed in the report.

## Phase 5: the open backlog, after Phase 0 (implementer, 2–4 days)

The order follows `realtime-and-backlog-plan.md` §10, restricted to what Phase 0 marks open:

1. §3 Laya and GLiNER in the product.
2. F1 Argus-side recording.
3. F4 guard duress and PIN lockout.
4. F6 productivity remainders.
5. The open S items.
6. Q1–Q3.
7. The D items: tidy paydown per unit, stale docs, named cleanups.

F5 is in Phase 4b.

## Phase 6: the long programmes (each with its own plan, 6–8 weeks in all)

| Programme | Plan |
|---|---|
| F9 parallel conversations and F8 messenger | `parallel-conversations-plan.md` |
| Camera watchman | `camera-watchman-plan.md` |
| STT improvement: hotwords, cleanup, streaming, same latency | to write |
| ONVIF native detectors as hints, YOLO decides | to write |
| Frontend e2e with tester-army | to write |

Each programme starts with the "measure first" section its plan names, and lands as units under this plan's rules.

## Owner decisions, resolved 2026-10-16

The owner asked to apply the recommended option for each decision, after research. They are listed in 2.2 and Phase 4b:

- session: no second copy; clean recovery;
- siren: confirmation plus a 5-second countdown, behind an owner setting;
- profiles: 14/7 GB, quality Spanish in Equilibrado, `gpu_layers = 0`;
- remote: LAN direct, cached offline, no second relay;
- VLM: gRPC.

## Review protocol (reviewer)

For every unit the reviewer:

1. reads the diff against the rules;
2. re-runs the unit's tests and gates;
3. checks that the acceptance stated here holds;
4. accepts the exact SHAs, then fast-forwards `master`.

A unit the reviewer rejects comes back with the concrete defects. Device-visible units close only after the owner sees them work.
