# Frontend end to end: the scripted lane, the real desktop and the Android emulator

Status: written 2026-10-19, not started. It continues the owner's decision of 2026-10-06 (e2e with
tester-army's `e2e`, native Android checks) and the frontend's section-6 work of 2026-10-12. Every
finding below was read on 2026-10-19 from the frontend worktree (`wt/frontend-t3-backlog`,
`b86ba59`) and the backend (`e19dfd19`); paths under `frontend/` are relative to the frontend
repository. Nothing was run for this document.

## 0. The principle

The app paints persisted local data first and then reacts to live sync (backend `AGENTS.md`,
frontend `AGENTS.md`). An e2e test proves that on the engine the user actually runs: Hermes on
Android, WebKitGTK inside Tauri on the desktop. A Chromium run against the exported bundle is a fast
and useful gate, and it is **not evidence for the desktop** (owner, 2026-10-07: "en verdad no es web,
es tauri"). Each critical flow therefore names the lane that is its evidence.

Standing owner rules this plan obeys:

- AVDs (`Argus_API35`, `Argus_Tablet_API35`) default to the laptop's keyboard, webcam, audio and GPU
  for the owner's own use; **every unattended run forces** `-camera-front emulated
  -camera-back emulated` and a virtual microphone (`PULSE_SOURCE` on a null sink), so the owner's
  webcam and microphone are never opened without him.
- The desktop is Tauri: UI checks on WebKit with mocked IPC; the real app through `tauri-driver` and
  `WebKitWebDriver` on the release binary, with an isolated test keyring, never the owner's.
- The exported web bundle is loaded in headless Chromium as a probe before any desktop-bound unit is
  accepted (`frontend/scripts/probe-web-bundle.ts`, `bun run check:bundle`).
- **Tests never touch a production NATS broker**: no `4222` (`argus-cutover`), no `4224`; a real
  backend lane runs only on a scratch broker, and the harness refuses to start otherwise.
- Heavy jobs (emulator, real stack, release builds) run one at a time through the RAM gate.

## 1. What exists today

- **The scripted lane** (`frontend/tests/e2e/`, frontend `CONTEXT.md` "End-to-end: two devices
  against a scripted Argus"): tester-army `e2e@0.19.0` with `@e2e-dev/web@0.14.0`, pinned
  (`frontend/package.json`), config `frontend/tests/e2e/e2e.config.ts` (one target `desktop-web`,
  `workers: 1`, `retries: 0`). The app is the exported bundle signed in through
  `support/tauri-shim.ts` (plays `__TAURI_INTERNALS__`: secure storage, `argus_request`,
  `argus_socket_*`), Argus is `support/fake-argus.ts` served by `support/serve.ts` on loopback, and
  `/__e2e/*` drives it (drop sockets, refuse reconnects, route specs, reset). Device B is a second
  context (`support/devices.ts`) in the same Chromium (`support/chromium.ts`, `/usr/bin/chromium` or
  `ARGUS_CHROMIUM`). Every request off loopback is aborted and asserted absent. Telemetry off.
- **Covered flows** (22 specs): `two-device` (the section-0 test: live, after background, after a
  missed reconnect), `sessions`, `people`, `activity`, `modules`, `guard` (mode, episodes),
  `safety`, `presence`, `project-members`, `calendar-shares`, `recurring-reminder`,
  `reminder-snooze`, `notification-delete`, `duress-phrase`, `response`, `inactive-role` (shows the
  panic button), `module-dialogs`, `welcome` (up to the pairing code step), `call-window` (the
  desktop call window's IPC), `route-status`, `accessibility`, `visual` (PNG set
  `linux-x64-chromium153`).
- **The probe** (`frontend/scripts/probe-web-bundle.ts`): renders only the unpaired first screen; the
  frontend `CONTEXT.md` names a mocked paired session as its open residual.
- **The device checklist** (frontend `CONTEXT.md`, "Device checklist", Q3): nine manual items
  (camera live audio, WebRTC live view and fallback, calls, the QR sheet with the keyboard, the iOS
  media module, screen readers on the welcome hero, the two-device check on hardware, FLAC playback,
  iOS native audio), "not run by agents".
- **`verify`** runs comments, boundaries, `tsc`, lint, unit tests, the probe and the scripted lane.
- **The backend's real stack for tests**: `scripts/scratch-stack.sh` (a scratch JetStream broker on a
  chosen port, the eight request-serving services from `scripts/native-stack.sh`, optional AI
  services, seeded rows and role sessions via `scripts/seed-golden.py`), and
  `scripts/nats-scratch-run.sh` for a throwaway broker.

What is missing:

1. **No WebKit run at all.** Every e2e and the probe run on Chromium. The cached Playwright WebKit
   build on this machine was found broken after a system upgrade (missing `libicudata.so.74`,
   2026-10-11), and `@e2e-dev/web` connects over CDP, which WebKit does not speak.
2. **No real desktop run.** No `tauri-driver`, WebdriverIO or `WebKitWebDriver` anywhere in the
   frontend; `WebKitWebDriver` is not in Arch's `webkit2gtk-4.1` package (a system install, the
   owner's step).
3. **No Android lane.** Only `@e2e-dev/web` is installed; whether tester-army ships a mobile engine at
   a pin compatible with `e2e@0.19.0` is unverified.
4. **No real backend under any e2e.** Pairing (HMAC proof and CA), face login, WebRTC/LiveKit, the
   camera live view and the assistant are never exercised end to end.
5. **No CI for the frontend.** The frontend repository has no `.github/workflows/`; the backend's
   `.github/workflows/ci.yml` builds the backend only.
6. Flows without a spec: full pairing, face login, cold-start paint before the socket, camera live,
   a call with Argus, the agenda's day view, pressing panic.

## 2. The lanes

| lane | engine | Argus | where it runs | role |
|---|---|---|---|---|
| **A scripted** (exists) | Chromium over CDP, `tauri-shim` | `fake-argus.ts` | every `verify`, PR gate | fast logic and two-device sync; probe |
| **B WebKit** | Playwright WebKit (or WebKitGTK's own `MiniBrowser` over WebDriver if Playwright's build stays broken), IPC mocked with `@tauri-apps/api/mocks` `mockIPC` driven by the same seed | `fake-argus.ts` | `verify` once stable | the desktop's engine for every lane-A spec |
| **C real desktop** | the release `tauri build` binary through `tauri-driver` + `WebKitWebDriver` (WebdriverIO) | scratch stack | nightly on the laptop | the desktop's real IPC: keyring, reqwest TLS with the pinned CA, socket, call window, rtc |
| **D Android** | `Argus_API35` (and `Argus_Tablet_API35` for the second device) on KVM, the dev client or a release APK | scratch stack (emulator reaches the host at `10.0.2.2`, paired by address since mDNS does not cross) | nightly on the laptop | Hermes, Nitro `argus-net`, expo-secure-store, WatermelonDB on SQLite, expo-camera, LiveKit |
| **E hardware** | the owner's phones | dev stack | before a release that touches the area | the device checklist, unchanged |

Lane D's driver: **tester-army first** (owner's choice, 2026-10-06, over the earlier Maestro
suggestion). E0 reads its README and releases at the pin; if no Android engine exists, the
recommended option is a thin engine for the tester-army runner over `adb` and UIAutomator2, kept
in `frontend/tests/e2e/support/android/`, so specs and fixtures stay one suite. Maestro or Detox
are recorded as the fallback only if the runner's engine interface cannot host it.

### Lane C and D safety preflight (one script, both lanes)

`frontend/tests/e2e/support/real-stack.ts` (new) refuses to start unless: the stack root is not a git
work tree (as `scratch-stack.sh` already requires), every service config's NATS URL names the scratch
port and that port is neither `4222` nor `4224`, the emulator was launched with emulated cameras and a
null-sink `PULSE_SOURCE`, the keyring is the test collection under a private `dbus-run-session`, and
no `models/` path is the owner's live one. It tears the stack and the broker down after the run and
deletes every stream it created.

## 3. Critical flows

Each flow names its evidence lane, its two-device form where the audience differs, and the mutation
that must fail it (the convention lane A already follows).

| flow | lanes | two-device | test sketch | mutation that fails it |
|---|---|---|---|---|
| **Pairing** | A (scripted proof), C, D | — | code → client HMAC → `serverProof` checked → CA stored; on D, pairing by address `10.0.2.2` | a server proof with the wrong CA fingerprint is accepted |
| **Face login** | A (scripted verdict), D | — | D: emulated front camera with an Android virtual-scene poster image of a test face enrolled on the scratch identity; A: `fake-argus` answers the verdict | a refused face signs in; a disabled account (`ACCOUNT_DISABLED`) is not refused |
| **Cross-device QR login** | A, C+D | phone approves desktop | desktop shows the QR; the phone session approves the challenge; the desktop signs in | approval from a revoked session succeeds |
| **Sync, paint then live** | A, B, C, D | yes | cold start with the socket refused: persisted rows are on screen; socket allowed: live `Add` arrives; on D the app is killed and relaunched offline | the screen waits for the socket before painting |
| **Camera live** | C, D (A only for the overview) | — | a stream-only RTSP camera on the scratch stack fed by an `ffmpeg` test pattern through go2rtc; first frame within 3 s; WebRTC blocked → the `/media` socket fallback and its badge | the fallback never engages |
| **Calls** | A (`call-window`), C, D | ring on both devices of a user | LiveKit from `scripts/livekit.sh` + argus-voice with STT/TTS on the scratch stack; the virtual mic plays a recorded question; the reply's transcript appears; hang-up releases the mic (`dumpsys audio` on D) | the ring keeps sounding on the other device after answer |
| **Reminders and agenda** | A (exists), D | yes | reminders arrive from Argus (the assistant or a scripted `Add`; no manual creation in the app, owner rule 2026-10-07), snooze and complete on one device, both move; the agenda's day view shows the event and the reminder | a snooze moves only the acting device |
| **Guard panic** | A, C, D | yes (Resident presses, Owner receives) | panic from Home and from the on-hold home of an inactive role; the Owner's device shows the response card live; `POST /guard/panic` is never module-gated | the panic route answers `MODULE_DISABLED` with surveillance off |

Two-device composition: lane A's two contexts stay the everyday form. The real form is **phone
(lane D) + desktop (lane C)** on the same scratch stack, which is the section-0 test on the real
engines; two AVDs together only when the RAM gate allows it (each AVD is about 2–4 GB with its
GPU).

## 4. Measure first (E0)

- Run lane A twenty times in a row on an idle laptop and on a loaded one: the flake count per spec,
  wall time p50/p95. This is the baseline the flake policy measures against.
- Lane B feasibility: does Playwright WebKit start here (and with which `libicu`), and does
  `mockIPC` cover every command `tauri-shim.ts` plays today?
- Lane C feasibility: is `WebKitWebDriver` installable (owner's step, by command), does
  `tauri-driver` drive the release binary on WebKitGTK 4.1, and how long does a release build take?
- Lane D feasibility: tester-army's Android support at the pin; emulator cold boot and snapshot boot
  time; RAM with the scratch stack plus one AVD plus the desktop binary; whether the virtual-scene
  poster yields a face the identity service matches at 0.50 cosine.
- Every image used as a face or a voice for these tests passes the licence register first (no
  scraped faces; a consenting household member's photo or a licensed synthetic set).

## 5. CI shape

- **Pull requests (GitHub-hosted, Ubuntu, Chromium)**: a new `frontend/.github/workflows/verify.yml`
  running `bun run verify` (lane A and the probe), with the visual set for that runner's platform
  recorded once or skipped by name as it is today. No backend, no secrets, no network beyond package
  installs.
- **Nightly (the owner's laptop, not a self-hosted runner)**: a public repository must not run
  untrusted pull requests on a self-hosted runner, so lanes B, C and D run from a local nightly script
  (`frontend/scripts/e2e-nightly.ts`, new) under the RAM gate, which builds the release binary and the
  APK, boots the scratch stack, runs the lanes, writes a dated report with per-spec status and
  timings into the scratch backup, and tears everything down. The owner reads it in the morning.
- **Before a release**: lane E's checklist for the areas the release touches, as today.

## 6. Flake policy

- Lanes A and B keep `retries: 0`. A test that fails once is investigated, never re-run to green.
- No sleeps: every wait is on a condition with a deadline (`/__e2e` control, a visible row, a socket
  count). Fixed clock (`support/clock.ts`), fixed zone and locale, reduced motion, as today.
- Lanes C and D allow **one retry**, and a pass on retry is recorded as **flaky**, not green.
- A spec enters the gating set of its lane only after **20 consecutive green runs**; a gating spec
  that flakes twice in 20 runs is quarantined (tagged, out of the gate, listed in the nightly report
  with an owner and a 7-day deadline to fix or delete). Quarantine never exceeds 10 % of a lane.
- A flake fix lands with the mutation that proves the test still fails for the right reason.

## 7. Units

Each unit lands with its own acceptance, the reviewer's acceptance of the exact patch before merge,
and `bun run verify` green; desktop-bound units also pass the probe.

| # | Unit | Done when |
|---|---|---|
| E0 | Measurements of section 4, recorded in frontend `CONTEXT.md` | baseline flake table and the feasibility answers written |
| E1 | Lane A gaps: full pairing with a scripted proof, face login with a scripted verdict, cold-start paint with the socket refused, panic from Home and from the on-hold home, the agenda's day view; the probe extended to a mocked paired session | each new spec fails under its mutation |
| E2 | Frontend CI workflow for `verify` on pull requests | green on a pull request with no secrets |
| E3 | Lane B on WebKit with `mockIPC` for the lane-A suite | every lane-A spec green on WebKit, or each exception named with its engine cause |
| E4 | The real-stack preflight and teardown (`support/real-stack.ts`) over `scripts/scratch-stack.sh` | the preflight refuses a 4222/4224 URL, a git-tree root, a real webcam or mic, the owner's keyring |
| E5 | Lane C: release binary under `tauri-driver`; pairing, QR login, sync paint-then-live, panic | 20 consecutive green nightly runs |
| E6 | Lane D: the tester-army Android engine (or the thin adapter); AVD launch with emulated cameras and the null-sink mic; pairing by address, face login with the poster, sync offline relaunch, reminders, panic | 20 consecutive green nightly runs |
| E7 | Media flows on C and D: camera live with the test-pattern RTSP camera and the WebRTC fallback; a call with Argus through LiveKit with the virtual mic | first frame ≤ 3 s; mic released after hang-up |
| E8 | The real two-device form: phone (D) + desktop (C) for the section-0 test, sessions and panic | the 2 s bound holds on both directions |
| E9 | The nightly script and report | a week of nightly reports with flake counts |

Order: E0 → E1 ∥ E2 → E3 → E4 → E5 ∥ E6 → E7 → E8 → E9. Device and hardware runs come at the end of
the programme (owner rule, 2026-10-19).

## 8. Owner decisions (recommended options, recorded 2026-10-19)

1. **Stay on tester-army for every lane**, with a thin Android adapter if its engine is missing.
   Recommended over a second framework, which would split fixtures and conventions.
2. **Nightly lanes run on the laptop from a script, not a self-hosted runner**, because the
   repositories are public. Recommended over a self-hosted runner.
3. **Face login on the emulator with a virtual-scene poster** of a consenting household member's
   photo, kept outside the repository. Recommended over a `v4l2loopback` webcam (a kernel module,
   a system change) and over skipping face login on Android.
4. **Lane A stays the pull-request gate**; WebKit joins `verify` only after E3 is stable for
   20 runs.
5. **System installs are the owner's steps**: `WebKitWebDriver` (and any WebKit runtime library) is
   requested from the owner as an exact command, after the public repositories are pushed.
6. Open for the owner when E0 reports: whether a second AVD may run beside the stack and the desktop
   on this laptop, or the phone-plus-desktop pair is the only real two-device form.
