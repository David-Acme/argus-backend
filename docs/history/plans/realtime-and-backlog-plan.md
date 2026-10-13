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
| H3 | The desktop heals a changed server IP by itself (it kept keyring `argus:net.ip` = the old LAN address after a network change and every request failed with "Could not prepare access.") — re-discover over mDNS and `relocate` on a connect failure, the way a changed port already heals (`c127611`) | moving the laptop to another network needs no manual step. Residuals (2026-10-12, frontend `050916e`): `session.service.performRefresh` heals no address of its own — its transport failure returns `unavailable` and the surrounding request's heal covers the ordinary path; and a mid-flight failure that is neither connect-phase nor a timeout (a reset or a truncated body after the request was written) still reads `NETWORK_ERROR` on all three transports and can re-send, which needs a distinct code across `NetErrorCode`, the `net-persistence` list and `service-error.ts` together |
| H4 | Build follow-ups from the demo night: the `if(NOT TARGET gRPC::grpc_cpp_plugin)` guard around the substrate's system branch, the dev-path proof (`build-all.sh dev --only auth`), a `docker buildx prune` step now that the builder runs `gc = false` (`argus-deploy/buildkitd.toml`), and resuming the docker image series to all-green when the owner wants images | gates green, images tagged |
| H5 | Review rule added after the black-window bug (`09d3494`, `TypeError: Illegal invocation`): a frontend unit headed to the desktop is accepted only after its exported web bundle renders in headless chromium with the Tauri CSP (Bun and Hermes do not check `this` on timers) | probe part of the frontend gate |
| H6 | Every Docker service reads the host's zone. Found 2026-10-13: the compose file set no `TZ`, so each container's `localtime_r` read UTC. Affected readers in a container: guard schedules (a 22:00 schedule armed at 17:00 Lima), notification lead times and quiet hours, llm call times ("mañana a las nueve") and the memory time phrases, and the speech time formats. The native development run already uses the laptop's zone, so only Docker deployments were affected. | `argus-deploy` passes `TZ: ${ARGUS_TZ}` to all 27 services through one `x-host-zone` anchor, `provision-host.sh` writes `ARGUS_TZ` from the host zone, the sync context carries `timeZone`, and `deploy-mounts-test` asserts every service carries `TZ` and that a stripped copy is reported. Follow-up: every runtime image installs `tzdata` (the zone database no longer rests on what the base image happens to ship) and `provision-host.sh` warns on stderr when no source gives a zone and `ARGUS_TZ` falls back to UTC; `deploy-mounts-test` asserts both (a runtime stage without `tzdata` is reported; the fallback prints a clean `UTC` and warns, a readable zone does not warn) |

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
  - Episodes ship now with `{guard, episodes}` (create, transition, close, review, retain and the
    retention purge; touches behind the leading+trailing `guard.episode_touch_publish_s` gate).
  - **Incidents publish nothing yet:** `GET /guard/incidents` has no reader in the app, so its writes
    stay unrung. The registry row stays as the contract. When a screen reads it, incident writes
    publish `{guard, incidents}` with the ids of the inserted or re-graded incident, behind the same
    leading+trailing gate, and the Security summary counts are already answered by the episodes read
    (`security-screen.tsx:39-40`), so no other scope is involved.
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
  Users screen chips freeze. Guard announces `{guard, presence}` on every presence change and the
  screen re-reads `GET /guard/presence`; `Heartbeat` stays the user's own frame. Who is home is the
  **Owner's alone** (owner decision, 2026-10-12): Guard, Resident and Guest do not read household
  presence, for now.
- R17 Role-change robustness: if the best-effort `AuthContextChanged{resync}` RPC fails, the 60 s
  revalidation sends only `ContextUpdate`; have it also resync, or have the client resync when the
  context role differs from the stored role.
- R18 Rename skips `publishCatalog`, so the 10–30 s user caches in auth and sync lag.

**Productivity and activity**
- R19 Activity never shows productivity, notification or assistant actions and does not update live:
  only identity, auth and settings feed `user_action_log`, and `insertAction` pushes nothing. Add the
  action publishers and push the new row.
  Owner ruling (2026-10-12) for R19b, under "what a person tells Argus privately stays theirs":
  every user-initiated create, update and delete in productivity is journaled (not only updates);
  for personal rows (reminders, a person's own calendar events and projects) the journal carries only
  who, the kind of action, the table and the time, never the row's content or its before/after diff
  ("Ana creó un recordatorio — 10:42"); for shared or household rows (a shared project or event) it
  may also carry the title; marking notifications as read is not journaled; the identity rename gets
  its missing action row (R18).
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
  - Ruled and landed 2026-10-12: `person` is fed (identity's `PersonFeed`: `Add` on enrolment,
    module-audit diffs on touch, observation and promotion, household rows only, the eleven pull
    fields); it stays core, so no purge-list entry. `camera_stream` and `reminder_detail` leave the
    client (frontend `32c8f32`, schema v11), the server keeps serving them under D8. No path
    soft-deletes a household person yet, so no `Delete` frame exists until one does.
- R26 While a module is off its tables are skipped in audit pulls but the client cursor advances, so
  changes in that window are lost after re-enabling.
  - Landed 2026-10-12 (frontend `32c8f32`): a module that goes from off to on drops its local
    tables and cursors and re-pulls them from zero; the shared audit watermark carries on.

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
  **Argus-side recording is ON HOLD (owner, 2026-10-13)**: the owner will clarify some points before
  it is built; the spec below and `argus-recorder-design.md` are the record, nothing is implemented.
  The rest of F1 (snapshot, SD timeline, presets, mute, ONVIF note) continues.
  **Argus-side recording, owner-approved spec and rulings (2026-10-13)**, after web research into
  Frigate, Milestone, Synology, Blue Iris, ZoneMinder, Shinobi and Scrypted:
  - Event recording with pre-roll: a continuous per-camera ring of the last N minutes (default 5) as
    keyframe-aligned fMP4 segments of about 4 s, stream copy only (no transcoding), timestamps from the
    server's NTP clock with the camera offset kept in the manifest. The ring lives in tmpfs when the
    host has the RAM for it, otherwise on the SSD (sequential writes; SMART wear shown on the storage
    dashboard). An important event (guard episode high/critical, intruder, escalation, panic) keeps
    the pre-roll and records until the event ends plus a post-roll (default 2 min; panic 10 min);
    overlapping events extend one clip; a clip is capped at 30 min and rolls into a linked clip.
    Segments are promoted to the private RustFS store during the event, not at its end. Always armed
    (Home mode too); main stream; audio except on "no audio" cameras; nothing in privacy mode.
  - A clip is a signed manifest over segments (SHA-256 per segment, hash-chained manifests, an
    instance Ed25519 key), never a re-mux; gaps from a camera dropping are explicit in the manifest
    and trigger an SD-card backfill of that window as a second, separately hashed source. Export is
    a byte-identical stream-copy MP4 plus the manifest and a verify tool; nothing is burned in.
  - Access log: every view, scrub, download, export, lock and delete of a recording is an audit row
    (who, device, clip, action, range, time), server-side only, kept a year. Owner-only access in v1.
  - Evidence lock: the Owner can lock a clip with a reason; locked clips skip retention and the disk
    quota; incident locks last up to 120 days, longer only with an explicit authority-request reason.
  - Retention, applied by default even though a private home may fall outside the directive:
    30 days (configurable to 60), 120 for a locked incident, counted in calendar days, deletion within
    2 days of expiry. Disk: a per-camera quota (default 50 GB or 10% of the volume) and a fixed free
    reserve; oldest unlocked first; if only locked clips remain, the ring keeps running and the Owner
    is alerted.
  - Resilience: a recorder process separate from the HTTP surfaces, so a backend restart does not
    stop recording; fragmented MP4 written to a temp name and renamed; a boot reconcile of store and
    index that aborts if it would delete more than half.
  - Cameras over a worker's rest area get a "no recording" flag besides "no audio". Data-subject
    exports in v1 are an Owner-chosen time range and camera; automatic blurring of third parties is a
    later item. Measure first: each camera's GOP and bitrate on stream1 (ffprobe), and its RTSP
    stability.
- F2 Talking to Argus through the cameras: superseded on 2026-10-12 by the owner's wider goal, the
  always-listening watchman, designed in `camera-watchman-plan.md` (wake word, identity, hand-off,
  echo, privacy, measurements first).
- F3 Argus calls you like a messaging app (owner, 2026-10-12): a native incoming-call screen on the
  locked phone, rung by a push that wakes the app, the conversation over the existing LiveKit call.
  Android: ConnectionService (self-managed) + a full-screen-intent call notification and a
  `phoneCall`/`microphone` foreground service, declared for Play's full-screen-intent policy. iOS:
  CallKit + PushKit; Apple requires every VoIP push to report a call to CallKit at once. Push: the app
  registers its token; the owner's own push relay (`argus-relay`, on the same server as the future
  private tunnel) holds the Firebase and APNs credentials and forwards the engine's generic
  "Argus te está llamando" intent; Android through FCM (free), iOS VoIP through APNs directly (FCM
  cannot carry PushKit pushes). Media stays between the phone and the house's LiveKit: on the LAN at
  home, through the owner's tunnel outside (a self-hosted TURN on that server where a direct path
  fails). No per-call third-party service: fixed costs only (Google Play account once, Apple
  Developer Program yearly for iOS, the relay/tunnel server).
- F4 Guard: a voice duress phrase, a persistent PIN lockout (in memory today), the single-member
  household case.
  - F4 follow-up, ruled 2026-10-13: fuzzy matching of the duress phrase (one word off). It trades
    against false alerts and needs measuring on real STT before any change. Matching stays exact after
    normalisation until then.
- F5 App siren button with its consent setting (owner decision).
- F6 Productivity: reminder snooze and recurrence (recurring reminders fire once), project member and
  event share management screens with permissions reflected in the UI, task assignee validation and
  clearing, recurring calendar events beyond the first occurrence, notification delete, the summary's
  active-project count (`archived` is not a backend status), the assistant's `calendar.list_events`
  including shared events.
  - Follow-ups ruled 2026-10-13, not in the recurrence unit:
    - F6a The assistant creates a recurring reminder ("recuérdame cada lunes..."): slot parsing in
      services/llm. That service is frozen behind the parked Laya branch, so this lands after the cutover merge.
    - F6b Editing one occurrence of a series (the RFC 5545 exception). Out of scope until the series
      model is settled; the occurrence entry id `${eventId}@${occurrenceStartIso}` is already in place for it.
    - F6c Agenda paging. The agenda's window is cumulative, so each "load more" recomputes every occurrence
      from the first day. Measured on 2026-10-13: a two-year window with 20 daily series costs 49.7 ms in a
      second household zone, over the 16 ms bar; 8.8 ms in the device's own zone. The fix comes first as a
      cache of zone offset changes per window (about 40 lines in `recurrence.ts`), and paging only if the
      daily same-zone case grows past a frame.
- F7 The `agronomy` module (`coming_soon`).
- F8 Argus as the household's messenger (owner, 2026-10-12, wanted for sure): "Argus, llama a David y
  dile que venga a comer". The router picks a `relay` tool (the LLM never picks tools), the slots are
  the recipient (`text_norm::matchName` over the household) and the message in the speaker's words;
  Argus delivers it through the best channel the recipient has right now (an open conversation, the
  camera that sees them once the watchman exists, otherwise a notification; never a ringing call, so a
  message does not disturb, owner's ruling of 2026-10-12), and carries the
  answer back ("dile que ya voy") to the sender's open conversation or as a notification. It relays
  only what the sender said: nothing from the sender's private memory travels with it. Who may relay
  to whom is a capability in `role-access.hxx` (a visitor leaves a message, never rings a resident);
  presence decides the channel but is never revealed to the sender (the sender hears "se lo dije" or
  "no contestó, le dejé el mensaje", not where the person is).
- F9 Parallel conversations, one per person (owner, 2026-10-12): Argus holds a separate conversation
  with each user at the same time, on any device, each with its own hot context; one conversation per
  account that follows the person between camera, phone and laptop; everything extracted from every
  conversation feeds the one shared memory, and **what a person tells Argus privately is never used in
  another person's conversation** (household facts are shared, personal ones stay the person's). Needs
  a per-user conversation store, parallel LLM sequences, fair STT/TTS scheduling and co-located device
  coordination; designed in `parallel-conversations-plan.md`.

## 5. Security and deployment hardening

- S1 mTLS on internal gRPC with an internal CA (voice and others are cleartext today). Shape decided
  2026-10-13: files under `${ARGUS_DATA_DIR}/certs/internal`, a CA valid for 10 years and service
  certificates for 2, renewed by `provision-host.sh` (and natively by `setup.sh`) when fewer than 60
  days remain, each service's health warning under 30 days, the per-caller secret kept as the
  authorisation on top of mutual TLS, and an additive migration whose plaintext listeners stay until
  the owner's deploy check (their removal is a later cleanup unit). Step 1, 2026-10-13:
  `scripts/lib/internal-pki.sh` issues the CA (ECDSA P-256) and one certificate per service that
  speaks internal gRPC (13, for server and client use), renews them, rotates the CA in two passes
  (the next CA joins every trust bundle first and takes over a day or more later, counted from its
  `notBefore` rather than its file's time, the old one trusted until it expires), so each pass is
  followed by a rolling restart, one service at a time with no outage, and
  `scripts/internal-pki-test.sh` proves it, mutual TLS with openssl included
  (`argus-deploy/CONTEXT.md`, "The internal CA"). Step 2, 2026-10-13: `packages/lib/grpc` loads
  those files (`internal-tls.{hxx,cc}`: server credentials that require and verify the client's
  certificate, channel credentials that present the service's own and check the server's name, and a
  client target written `tls:<host>:<port>` that `makeChannel` serves with the certificate the
  process installed, every other target staying plaintext), proven against a live loopback server by
  `internal-tls-test`; a process installs its certificate once, before its first channel, and never
  swaps it while it runs. Step 3a, 2026-10-13: `lib/http`'s `tls_health::report` turns the PEMs a
  service loaded into its health section (the loaded bundle's CA count, the days left on the
  certificate and on its issuing CA, a warning under 30 days for either), proven by
  `tls-health-test`. Step 3, the health warning, needs services that read their trust bundle, so it
  follows step 2, and its scope is three items: each service's health response reports how many CAs
  its loaded trust bundle holds; a CA's own expiry gets the same 30-day health warning as the
  service certificates; and before a pass that would move the certificates to the next CA,
  provisioning prints a check that the two-CA bundle is live on every service, which it prints and
  never gates on. Order approved 2026-10-13: 3a, then the services one commit each, smallest first,
  each shown booting with `[internal_tls]` set (settings first among the mains track D shares,
  defining the listener port pattern both tracks use; guard, llm and voice last, after track C),
  then 3b (provisioning's pre-pass-2 print), then the clients' `tls:` targets, and after the owner's
  deploy check the cleanup that removes the plaintext listeners.
- S2 Tunnel home link: TLS 1.3 with a pinned relay, a MAC on every frame after AUTH, the device side
  refusing unauthenticated connections.
- S3 Per-service NATS accounts (nkeys, subject and `$JS.API` scoping). Status 2026-10-13
  (track B): the permission table with its checked-in rendering, one nkey per service issued by
  provisioning, and the client that connects with its own key and inbox are done; removing the
  shared user is the last step and waits for every service to switch. That last step is an owner
  deploy step, in this order: re-run `scripts/provision-host.sh` (it issues the seeds and writes
  each service's key into its config); bring the stack up (`docker compose build` and
  `docker compose up -d`; the first deploy recreates the broker, whose mounts changed); run
  `scripts/nats-users-check.sh`, which must list every connection under its service and none as
  `SHARED` (exit 0); only then the cleanup that removes the shared user. Both credentials stay
  valid until that check passes. Any later change to `argus-deploy/nats-permissions.conf` reaches
  the running broker only through a reload, `docker compose kill -s HUP nats`, after provisioning
  and before the services that need the new grants start.
- S4 Per-service S3 credentials scoped by prefix; `rc` keys out of command arguments. Decided
  2026-10-13 by test: RustFS 1.0.0-beta.12 enforces a service account's prefix policy
  (`scripts/rustfs-prefix-policy-test.sh` against the exported binaries: all 20 requests answered as
  the policy says, cross-prefix reads, writes, deletes, copies, multipart uploads, listings and
  bucket calls refused with 403; the same test with whole-bucket policies fails 12 of them). So S4
  keeps one bucket with one prefix-scoped service account per writer, the ruling's first option:
  identity `portraits/` and `faces/`, camera `cameras/`, guard `guard/incidents/`. A RustFS upgrade
  re-runs the test before it ships. Step 2, 2026-10-13: no key on a command line or in a process
  list. `rustfs-init` runs on the pinned RustFS image, which carries curl (`rustfs/rc` does not, and
  `rc admin service-account create` takes keys only as arguments), and creates the bucket and the
  shared application account through the S3 and admin APIs signed by curl; the keys reach curl only
  in a 0600 config file and a piped request body, built from the secret files, never in an argument,
  an environment or a shell variable (`scripts/rustfs-init-test.sh` logs every program's arguments
  and environment and the `sh -x` trace). The shared account keeps its provisioned keys, which the
  services' configs carry, until it is removed last; the per-service accounts take server-generated
  keys, the admin API's JSON answer written straight into a 0600 file. `provision-host.sh` writes
  the S3 secrets straight into their files and into the configs without a shell value, and
  `deploy-mounts-test` fails if the `set -x` trace of a dry run carries one.
  Step 3, 2026-10-13: `rustfs-init` also creates one account per writer, scoped to the prefixes
  its code writes, with keys the server generates; the admin API's answer goes straight into
  `${ARGUS_DATA_DIR}/rustfs/accounts/<service>/credentials.json` (0600). A re-run keeps the keys; an
  account lost on the server is created again, and a lost file gets a new account with the old one
  removed. `scripts/rustfs-init-test.sh` proves each account's prefixes on an exported RustFS.
  Step 3 delivery, 2026-10-13: each writer signs with its own account first. `S3StorageService`
  reads `[storage.s3] credentials_file`, the writer's account directory mounted read-only into it
  alone at `/run/secrets/s3`, and falls back to `access_key`/`secret_key` only while that file is
  missing or unreadable (`s3-credentials-test` checks both paths down to the request's
  signature). The config fallback is transitional, like the shared NATS user above: the owner
  deploy step that removes the shared credential also removes the S3 keys from the configs, in
  this order: re-run `scripts/provision-host.sh` (it creates the account directories the writers
  mount and names the file in their configs); bring the stack up (`docker compose build` and
  `docker compose up -d`; `rustfs-init` issues the accounts before the writers start); check that
  the identity, camera and guard logs say their keys come from `/run/secrets/s3/credentials.json`
  and that none warns the file cannot be used (the notice comes with each writer's first storage
  call: identity's object-deletion sweep within a minute, camera's first detection evidence,
  guard's first incident evidence); only then the cleanup that removes the shared
  account (from RustFS and from `rustfs-init`, which would create it again) and the
  `access_key`/`secret_key` keys (from those three configs and from the provisioning that writes
  them). Native runs still take the shared keys from `scripts/setup.sh`, so the cleanup's
  checklist starts there: `setup.sh` gives native identity, and any other native writer (every
  native config that carries `[storage.s3]`, the native stack's copies under
  `argus-deploy/data/native/` included, and camera's or guard's once their native templates gain
  object storage), its own account file, `credentials_file` naming
  `${ARGUS_DATA_DIR}/rustfs/accounts/<service>/credentials.json`, the file `rustfs-init` writes
  for that writer; only after that do the shared keys leave the native configs and
  `sync_storage_credentials` stop writing them. Both credentials stay valid until that check
  passes.
  Trace follow-up, 2026-10-13: no provisioning secret is a shell value any more, in
  `provision-host.sh` or in `setup.sh`'s config steps (NATS seeds and password, JWT, refresh and
  fingerprint secrets, every RPC credential, LiveKit and tunnel secrets, pairing code, S3 keys).
  `deploy-mounts-test` runs both twice under `bash -x`, builds the inventory from what they wrote
  and fails on any secret in a trace or any change on a re-run (before: 83 of 89 provisioning and
  57 of 57 native secret values in the traces; after: none).
- S5 Legacy refresh tokens two rotations old refused without revoking their family (accepted gap).

Rulings of 2026-10-12 (reviewer, owner away and delegating): S2 is superseded by the separate Argus
Cloud project (link v2, its C2). S3 uses per-service NATS **users with permissions inside the one
existing account**, not separate accounts, because JetStream streams belong to an account and moving
them would touch state; each service's publish/subscribe allow-lists come from one table beside
`nats-subject.hxx`, enforced by a test, sync included (an explicit list, not a wildcard). S1 uses a
**separate internal CA**, never the instance CA the apps pin, with renewal by re-running provisioning
and a warning 30 days before expiry; the per-caller secret stays as authorisation. S4 uses one bucket
with per-service prefix policies only if RustFS is proven by test to enforce them, per-service buckets
otherwise. Order S3 → S4 → S1; every migration is additive and removes the old shared credential last.

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
- D8 Named cleanup from the retirement of three writerless tables (2026-10-12): `event`
  (frontend `5ee5a8f`, schema v10), `camera_stream` and `reminder_detail` (frontend `32c8f32`,
  schema v11). Stop serving them in `SynchronizedService` and drop them from the pull list, once no
  supported client asks for them. The new client no longer pulls them and ignores a frame for them,
  but an installed app still does, so the server keeps its answers (`emptyTableNode` and the
  `{"event", ...}` pull row and DTO field for `event`; the `camera_stream` and `reminder_detail`
  pull rows and DTO fields) until the supported client set has moved. The enumerators stay for
  ever: `TableName`'s values are on the wire and `sync-contract-vocabulary-test` pins every one of
  them. Two of them may get a real writer later rather than go: F1 (recording) could feed
  `camera_stream` and F6 (reminder snooze and recurrence) `reminder_detail`; a feature that does so
  brings the table back to the client as a new schema version, never by reviving the dropped one.
- D7 Named cleanup from the encounter-outbox migration (2026-10-12): drop `guard_encounter_outbox`
  once every installation has booted a build that carries the copy into `change_outbox`. It is no
  longer created and no longer read or written, but deleting it is a data removal and needs an
  explicit owner reset or a later release, never a side effect of a feature build.
- D9 CLOSED (`2e41b5ae`). argus-tts's streaming route answered an empty 200 to a request carrying
  `Connection: close`. Drogon marks every response answering a non-keep-alive request as one that must
  close, *after* the handler returns, and its send path then refuses to start an async stream on it
  ("Chunking Set CloseConnection"), so no flag the route sets survives. The route now hands such a
  client to the buffered synthesis the non-stream route already serves: it hears the answer, the socket
  closes behind it, and it gets no incremental audio, which is named in `services/tts/CONTEXT.md`.
  `tts-wire-test` pins it, and one consequence with it: a closing client takes no stream slot, so the
  busy case asks as a streaming client.
- D10 CLOSED (`a62a7f78`). `scripts/build-all-test.sh` exited 1, twice over: `check-vendored` exits 2
  when a tree holds no `third_party/*/argus-patches` and `build-all.sh` ran it under `set -e`, so the
  self-test's own no-git-tree case aborted (the orchestrator now asks the question first and skips with
  a log line, as it already did for the comment gate), and a fixture still appended its stray label to
  what it took for a map after the bundle's `labels.json` became a list. Self-test green; the fix
  touched no project.
- D11 CLOSED as measured, no change needed. The race needs the runner to be a *member*, because member
  initialization runs before the constructor body that adds the database client; a local declared after
  `addDbClient` is safe by source order. Classifying all 47 `AppRunner runner;` sites that way leaves
  exactly two racing fixtures, both productivity's (reminder and mcp), fixed with R21 (`797b70c8`);
  camera, guard, identity and the rest declare their runner as a local inside the case, added after the
  client. The earlier "about twenty files" count came from the grep shape rather than that distinction.
- D12 The unary `POST /tts/v1/synthesize` takes no admission slot of its own: only the stream route
  checks `streams_`, so a closing client now reaches synthesis without a slot and so does any unary
  caller. The engine still serialises on its mutex, so nothing breaks, but the waiting is unbounded;
  argus-tts listens internally, which is what keeps the risk low. The fix is the unary path taking the
  same admission as the stream, answering 429 when full, with a test.
- D13 `tts-wire-test` and `tts-rpc-test` hard-fail when no TTS model is present (`REQUIRE` on
  `isLoaded()` in both), while the rest of the suite skips on a missing artifact, as the pocket case
  and the `eval` label do (exit 77, with a reason). The two must skip with a reason too, so a worktree
  without the models is reported as skipped, not red. Until then the gate procedure links the models
  (`docs/operations/build-and-test.md`). No fix now.
- D14 Marking a thread read (`PATCH /notification/thread/{threadKey}/read`) has the same gap the thread
  delete had before it moved server-side: the app marks only the notifications it has loaded, so a
  thread with more entries than the loaded page keeps its older entries unread. The fix is the same
  server-side thread route, scoped to the caller, in one transaction, with one audit diff per row. Not
  now.

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
