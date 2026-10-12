# Real time everywhere, and the backlog after the 2026-10-11 demo

Status: recorded with the owner on 2026-10-12, not started. Model training (Laya, GLiNER runs and
their data) is out of scope here; plugging the trained models into the product is in scope. Every
finding below was verified against the code on 2026-10-12 (backend `48dd097f`, frontend `d43bcd3`);
line numbers are from those commits.

## 0. The principle

The app is real time. A command travels over HTTP; the server performs it; every resulting change
comes back over `/sync` to every connected device of every user allowed to see it, and every screen
that shows that state re-renders without being reopened. That includes changes no user commanded:
schedules, sensors, the assistant, other members. The app paints persisted local data first and then
reacts to live sync (frontend `AGENTS.md`).

Acceptance for every item of section 2 is the **two-device test**: the same account (and, where the
audience differs, a second role) on the phone and the desktop side by side; the change is made on one;
the other shows it within 2 s with the screen left open, after a background/foreground cycle, and
after a reconnect that missed the change.

## 1. Hygiene first

| # | Item | Done when |
|---|---|---|
| H1 | Push backend (10 commits ahead) and frontend (17 ahead) after a secret scan of the outgoing history | both remotes at local HEAD |
| H2 | Rotate the pairing code used at the demo and delete `~/Desktop/argus-pairing-qr.png` | old code refused |
| H3 | The desktop heals a changed server IP by itself (it kept keyring `argus:net.ip` = the old LAN address after a network change and every request failed with "Could not prepare access.") — re-discover over mDNS and `relocate` on a connect failure, the way a changed port already heals (`c127611`) | moving the laptop to another network needs no manual step |
| H4 | Build follow-ups from the demo night: the `if(NOT TARGET gRPC::grpc_cpp_plugin)` guard around the substrate's system branch, the dev-path proof (`build-all.sh dev --only auth`), a `docker buildx prune` step now that the builder runs `gc = false` (`argus-deploy/buildkitd.toml`), and resuming the docker image series to all-green when the owner wants images | gates green, images tagged |
| H5 | Review rule added after the black-window bug (`09d3494`, `TypeError: Illegal invocation`): a frontend unit headed to the desktop is accepted only after its exported web bundle renders in headless chromium with the Tauri CSP (Bun and Hermes do not check `this` on timers) | probe part of the frontend gate |

## 2. Real time (priority 1)

### 2.1 What is live today

Calendar events, the Hoy agenda, projects, tasks, reminders (including the assistant's MCP writes),
notifications, cameras (create, edit, delete, online/offline when `[health]` is on), zones, users and
roles, invitations, sessions (revocation kicks the device), modules (jobs pushed, 1 s / 1 %), live
context and capabilities. The engine itself is sound: all 14 `SyncOperation` values have a client
handler, reconnect replays from the persisted cursors, field diffs map completely, every device of a
user shares the user room. Nothing here is to be changed except where a gap below says so.

### 2.2 One mechanism, not eighteen patches

- **Contract.** A new additive operation `ResourceChanged` (next free value, `resource_changed`),
  `{domain, scope, ids?}`, modelled on `ResponseUpdate = 10`. Declared in
  `packages/contracts/sync/src/sync/sync-operation.hxx`; AGENTS.md rule 18 lists it (and lists
  `ResponseUpdate`, missing today).
- **Producers.** Each owner publishes through its outbox to a change subject that argus-sync
  consumes (`change-feed-consumer.cc` today subscribes only to camera, productivity and
  notification), with the audience resolved by sync's rooms: Owner role room, module room, or the
  user's room. guard joins the producers (it is not an `EmitToUser` caller and emits nothing today).
- **Consumers.** `shared/hooks/use-remote-resource.ts` and `use-infinite-list.ts` (`useRemoteFeed`)
  take the domains they depend on and reload on a matching `ResourceChanged`, and also on reconnect
  (`synchronizeService.onConnect`). Screens keep their HTTP reads; they stop being frozen.
- Synced tables stay on Add / Delete / Log; `ResourceChanged` is only for state served over HTTP.

### 2.3 Gaps by domain (each closes with the two-device test)

**Guard** — emits nothing today except presence.
- R1 Mode, one or all environments, from a tap, the voice tool or a duress disarm
  (`guard-feature-service.cc` `setMode` ~688). Screens: Security, Home `GuardCard`, environment card,
  camera detail badge (`use-guard.ts:77`).
- R2 Scheduled mode transitions: the effective mode is computed at read time (`guard-schedule.cc`),
  never pushed, not even re-evaluated on the open device. A guard timer detects posture changes and
  emits.
- R3 Environments create/update/delete and camera placement (`:613`, `:640`, `:676`, `:733`).
- R4 Episodes and incidents opened, touched, closed, reviewed, retained (`guard-service.cc` encounter
  paths, `guard-feature-service.cc:804-835`); the Security list and its ongoing / pending-review
  counts follow. The episode timeline follows too.
- R5 Expected guests add/remove and one-time consumption on arrival (`:309-361`,
  `guard-repository.cc:292`).
- R6 Response lists and on-duty (`response-feature-service.cc:98,134`).
- R7 **Functional bug:** the safety PIN and duress mode (`safety-service.cc:194,221,245`). A PIN set
  on the phone leaves the desktop's `safetyService.known.hasPin` memo stale, so it disarms without
  asking and the server refuses. Emit to the user's room (PIN) and Owners (duress); drop or
  invalidate the memo.

**Camera**
- R8 Device settings (privacy, LED, day/night, motion, auto-track, alarm/siren, `sounding`) and the
  siren/alarm/announce fired by guard (`camera-control-feature-service.cc:91-113`,
  `camera-action-rpc-service.cc`): persist as camera columns with `publishAudit`, or emit
  `ResourceChanged{camera, id}`.
- R9 Presets save/delete (`:80-89`).
- R10 Detections and stream health: today a 15 s poll while Cameras or Home is focused; the `event`
  table is served empty (`synchronized-service.cc:432`), so the Home activity mosaic, the camera events
  list and the summary's event counts are always empty. Either feed `event` from the object-detected
  stream with Add, or push overview invalidations and drop `event` from the client.
- R11 Deleting a camera soft-deletes it, so zones get no tombstone; emit zone `Delete`s.

**Settings and identity**
- R12 `PATCH /settings/{owner}`, profile apply/revert and first-run (`settings-gateway-service.cc:139-175`,
  `settings-profile-service.cc:78-129`) emit nothing; other devices keep old values.
- R13 Household privacy and each user's own choices (`privacy-feature-service.cc`): today catalog +
  journal only. `PrivacyGate` must ask a resident for consent as soon as the Owner turns a feature on,
  not after a restart.
- R14 Visitors: rename, forget, merge, split, sample delete, new sightings, expiry, visitor settings
  (`visitor-feature-service.cc`, recognition, retention).
- R15 Voiceprints learned, forgotten or erased on consent withdrawal (`voiceprint-audit.cc`,
  `passive-enrollment-service.cc`).
- R16 Household presence: `Heartbeat` reaches only the user's own room (`heartbeat-feed.cc`); the
  Users screen chips freeze. Send presence changes to Owner/Guard rooms too.
- R17 Role-change robustness: if the best-effort `AuthContextChanged{resync}` RPC fails, the 60 s
  revalidation sends only `ContextUpdate`; have it also resync, or have the client resync when the
  context role differs from the stored role.
- R18 Rename skips `publishCatalog`, so the 10–30 s user caches in auth and sync lag.

**Productivity and activity**
- R19 Activity never shows productivity, notification or assistant actions and does not update live:
  only identity, auth and settings feed `user_action_log`, and `insertAction` pushes nothing. Add the
  action publishers and push the new row.
- R20 A confirmed optimistic change hides a remote change to the same record for up to 60 s
  (`shared/libs/optimistic.ts`, `OPTIMISTIC_TTL_MS`); clear on any server update for that record.
- R21 Grantees never receive the other members' / share holders' rows (scoped pull excludes
  `project_member` and `calendar_event_share`); deleting a project or event leaves its member/share
  rows orphaned on server and devices.

### 2.4 Transport robustness (frontend)

- R22 Reconnect immediately on foreground and on network change (no AppState or network listener in
  the sync layer today; resume waits on a stale backoff of up to 30 s), then catch up.
- R23 Desktop socket pong timeout (`src-tauri/src/net/socket.rs` sends pings but never checks for
  pongs); close after about twice the ping interval, like OkHttp on Android.
- R24 Refetch on reconnect for `use-responses.ts` and the session hooks.
- R25 Dead tables the client still observes: `event` (R10), `camera_stream` (no writer),
  `reminder_detail` (no writer), `person` (no live emit; four fields dropped by the mapper; missing from
  the surveillance purge list `module.constant.ts:19`). Feed each one or remove it from
  `SYNC_TABLE_KEYS` and its projections.
- R26 While a module is off its tables are skipped in audit pulls but the client cursor advances, so
  changes in that window are lost after re-enabling.

## 3. Laya and GLiNER in the product (not training)

- I1 C++ sequence parity for Laya: the renderer's segment assembly differs from Python
  (`services/llm/CONTEXT.md` ~1849).
- I2 One question set: the bundle export writes and hashes a single `questions.json` (~1874).
- I3 NFC normaliser at serving time: utf8proc as a tier-1 package, ICU as fallback — **owner
  approval pending** (~1868).
- I4 Cutover behind `[decide] engine` / `[extract] engine` (defaults `router` / `nuextract` today),
  deploy configs, the 1 GB memory gate on the host; fastText and NuExtract removed in the flipping
  change, per the three-axis measurement.
- I5 The training kit gets a CUDA lock (today `torch==2.14.1+cpu` and the CPU index in
  `scripts/kit_env.py`) and a test suite that passes on a clean clone (several suites read
  `~/.cache/argus-scratch-backup`, `/tmp` and gitignored `data/*-src`). Repository:
  `git@github.com:David-Acme/argus-ai.git` (private).

## 4. Features to build

- F1 Camera: Argus-side recording (`record_mode` is stored, nothing records), snapshot to file, SD
  recordings with a timeline, patrol and preset rename/delete, mute live audio on the native player,
  ONVIF control for generic cameras, TURN relay for remote WebRTC.
- F2 Talking to Argus through a camera, variant A: a call the Owner opens from the app, bound to the
  Owner's identity, half duplex, time-boxed — a bridge in argus-camera over `argus::clients::voice`
  (`voice.Connect` + RTSP mic + `SendTalk`). Validated live on the C225 on 2026-08-09 in the
  monolith; removed in the split. Always-listening with a wake word (variant B) needs a privacy and
  identity design first.
- F3 Native calls: CallKit/PushKit on iOS, ConnectionService and a microphone foreground service on
  Android, APNs/FCM push (no device push leg exists).
- F4 Guard: a voice duress phrase, a persistent PIN lockout (in memory today), the single-member
  household case.
- F5 App siren button with its consent setting (owner decision).
- F6 Productivity: reminder snooze and recurrence (recurring reminders fire once), project member and
  event share management screens with permissions reflected in the UI, task assignee validation and
  clearing, recurring calendar events beyond the first occurrence, notification delete, the summary's
  active-project count (`archived` is not a backend status), the assistant's `calendar.list_events`
  including shared events.
- F7 The `agronomy` module (`coming_soon`).

## 5. Security and deployment hardening

- S1 mTLS on internal gRPC with an internal CA (voice and others are cleartext today).
- S2 Tunnel home link: TLS 1.3 with a pinned relay, a MAC on every frame after AUTH, the device side
  refusing unauthenticated connections.
- S3 Per-service NATS accounts (nkeys, subject and `$JS.API` scoping).
- S4 Per-service S3 credentials scoped by prefix; `rc` keys out of command arguments.
- S5 Legacy refresh tokens two rotations old refused without revoking their family (accepted gap).

## 6. Frontend quality programme

- Q1 Tests mirror `src/`; component tests for critical screens (welcome, inactive role, response,
  module dialogs); an e2e suite with tester-army/e2e (none exists); Playwright visual regression at
  three widths.
- Q2 Welcome hero accessibility (F3 of 2026-10-10): one accessible stage hides the chips, the bubble
  and the unlabeled replay.
- Q3 Device validation: camera live audio and WebRTC on phones, the QR sheet keyboard, the iOS build
  of the camera media module.

## 7. Backend quality and debt

- D1 clang-tidy: 2,358 findings over 1,173 TUs, held by the ratchet; pay it down per unit.
- D2 `[[nodiscard]]` across `Syncable` implementations; argus-sync shutdown drain for the audit
  sweep; a test for the `AuditRetentionService` timer; the Drogon exit-time heap corruption in suites
  that boot SQLite and NATS.
- D3 Guard retention: the doc says 90 days and "≤ 0 keeps all"; the code clamps 1–60, default 30.
  Align one or the other.
- D4 `voice` and `tunnel` layouts (voice without `app/`; tunnel exempt by D19).
- D5 Confirm and close if done: architecture plan Phase 3d step 5, `check-deps` alias spelling,
  the visitors/incidents mobile UI, `IKnownPersonMatcher` in camera.
- D6 Stale docs: `AGENTS.md:1086` (steps 2b-2d are done), `services/sync/CONTEXT.md:437` (the outbox
  landed), `tool-calling-reduction-plan.md` (superseded by the decider + slots design),
  `frontend/CONTEXT.md:305` and `:541`.

## 8. Decisions awaiting the owner

1. Laya and GLiNER cutover, and utf8proc for NFC.
2. App siren button and its consent.
3. Camera conversation variant A.
4. Settings profiles: RAM floors 14/7 GB; whether Equilibrado uses the fast es model.
5. Remote access design: tunnel protocol and the fallback if the relay is down.
6. VLM gRPC cutover.
7. Open since the migration plan: orphan tables, Artifactory vs local Conan, QuickJS plugins on mobile.

## 9. Long programmes already agreed (unchanged)

- Guard shadow mode: calibration only after the collection window; measure the VLM's false-abstain
  and false-confirm rates before it may lower belief.
- LLM context precision (C1 → C0b → C0c): fix the rendered context, never the model size.
- Calibrate `voiceprint.unfamiliar_ceiling` and face liveness on real data.

## 10. Order

1. Section 1 (hygiene).
2. Section 2 (real time): first the mechanism (2.2) and transport (2.4), then guard R1–R7, camera
   R8–R11, settings/identity R12–R18, productivity R19–R21, dead tables R25–R26.
3. Section 3 (models in the product), as the owner decides section 8 item 1.
4. Section 6 together with section 2's frontend work (tests land with the changes they cover).
5. Section 4 by owner priority; section 5; section 7 continuously.

Each unit follows the implementation protocol: one package or service per unit, `build-all.sh dev
--only <project>` with zero warnings, the route and tidy gates, an independent review that re-runs the
tests, and for anything user-visible the two-device test of section 0.
