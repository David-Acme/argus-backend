# Close-all programme, 2026-10

The owner asked on 2026-10-19 that every open point be closed. One section per unit, measured.

## 1. The doubted lens mask on the wire (`privacyObeyed`)

- Backend:
  - `CameraRow.privacy_obeyed = 21` (additive) and the camera row's `privacyObeyed` reach the sync pull, the camera sync RPC, the sync gateway and the overview.
  - The privacy audit's before/after carry the persisted `(state, obeyed)` pair. The compare-and-set covers that tuple, and a retry that finally lands publishes its own audit.
- Frontend:
  - schema 13 adds `camera.privacy_obeyed`, mapped through sync, the view cache and the overview contract;
  - an unknown state that the server obeys is masked and labelled "Privacidad (sin confirmar)" / "Privacy (unconfirmed)" on the card placeholder and the home tile.
- Measured:
  - camera 41/41 and sync 21/21 suites green;
  - `two refreshes of one camera in flight give exactly one audit` passed 20/20 runs;
  - frontend 1654 tests, `tsc` for app and tests, lint, comment, boundary and bundle checks green.

## 2. The refusal leg (RF), verified

RF's two commits (`9e833a14`, `d21b520f`) were brought onto master and verified for the first time. The verification found and fixed these defects:

- **Word splitting in the refusal rules and the guard.** Every non-ASCII byte split a word, so "María" read as "Mar" and "¿dónde está?" never matched. Both splitters now keep UTF-8 letters and treat ¿ ¡ « » as separators.
- **Impersonation guard.**
  - A self-introduction ("soy David", "this is David") is always impersonation.
  - A vocative with a first-person cue ("María, estoy en camino") counts only on a relay turn, so Argus can still greet the user by name.
  - A self-introducing sentence is dropped from every reply path: streaming, plain and act. This was measured stochastically in the pack ("Hola, soy David…").
- **Person locations.** A location is guarded only when a person is the subject right before "está en" / "is in". "La reunión con Andrea está en la agenda" is no longer replaced by a privacy refusal.
- **The streaming scrub.** It dropped the whitespace between sentences ("días**.Es un concepto…"). Whitespace-only segments are now kept.
- **Relay refusals.**
  - A refusal echoes only a proper name. "mi chibolo" and "mi hijo" said back by Argus were a regionalism and a wrong possessive.
  - Multi-word postpone phrases had never matched ("para más tarde", "for later"). They now match as phrases, together with a move verb, so "pon un recordatorio para después de comer" stays a reminder.
- **Questions.**
  - The ask note made the model answer with statements (not_a_question 1 → 10). It now asks for one short question ending in a question mark.
  - Questions with known options, dates, choices and confirmations are rendered deterministically from the act, as RF already did for refusals, so they always name what the guard checks.

Measured on the prod build with the shipped pipeline (rules + triggers + Laya + GLiNER, `LFM2.5-1.2B-Instruct-QAD-Q4_0`, temperature 0.3, seed 42):

- **`call-faithfulness-render-acts`:** `retryShare` 0.4839 → 0.0323 (1 of 31; ceiling 0.05). GATE PASSED, for the first time.
- **The 15 refusal cases** (reminder, errand, privacy, role-inactive, relay-panic × es, es-PE, en): `falseRelays` 0, `noRefusal` 0, `privacyLeaks` 0, `regionalism` 0. GATE PASSED on three runs.
- **llm unit suites:** 73/73.

Left to the consolidated phase, because the smoke gate does not measure them:

- `questionMissing` on "ya lo hice" turns;
- `missedFacts` on relay-panic openings;
- the 40-reply blind sample.

The shipped bundles `decide/bundle-laya-mvp` and `extract/bundle-gliner-pilot` were installed into the main checkout's `models/`. They were added beside the pilots and nothing was replaced.

## 4. CC0: the watchman switches

- **Identity.**
  - `household_privacy.watchman` with its acknowledgement (`watchman_ack_version/by/at`), mirroring `visitor_recognition`. `PATCH /privacy/household {watchman: true}` needs `acknowledge: true`.
  - `location` as a per-person choice with an opt-out default: an omitted value keeps the stored one. It also exists as a household ceiling.
  - The proto `PrivacyChoices` carries `location` (8) and `watchman` (9), and `ListPrivacy` fills both.
  - A boot column migration adds the new columns to an existing database.
- **Camera.**
  - `camera.watchman` and `camera.no_audio`, default off, are settable through `PATCH /camera/{id}`. They are carried on the row JSON, the sync `CameraRow` (22, 23) and the sync gateway.
  - A boot migration adds them.
- **lib/auth.**
  - `kCameraWatchCapabilities` holds what a camera conversation may do: voice, camera view, reminders, agenda, projects, safety and notifications. It never holds the guard mode, settings, roles, presence or the directory.
  - `kCameraVisitorCapabilities` holds what a stranger may do: voice, panic and safety read.
  - Each set comes with its `…For` and `has…` helpers.
- **Legal.**
  - `docs/operations/legal-and-privacy.md`.
  - `scripts/privacy/watchman.{es,en}.md` and `--accept-watchman-notice` in setup and provisioning, recorded as `watchmanAcknowledged`.
  - The installer-responsibility and camera-audio sentences in the host notice and in the app's notice.
  - The notice moves to version 2 in identity, the scripts and the app together.
- **Measured.**
  - identity 31/31, camera 41/41, sync 21/21, packages 132/132, `privacy-consent-test` green.
  - The full dev build with all 15 projects' tests green.
  - Frontend: 1654 tests, `tsc`, lint and the comment gate green.
- **Owner decision recorded.** The per-camera flags follow `camera.manage` (Owner and Resident). The household switch stays Owner-only and acknowledged, so a Resident alone can never make a camera listen.

## Item 5: AL-B1/B2, who aims the camera

- **Move windows.** The scene log carries each move; the health monitor and the operator pause through it, so a pan is not a scene change or a new track.
- **Lease.** Argus < Viewer < Person, park and idle release 60 s, `[ptz]` config.
- **Home and drift.** `set_home`/`go_home`, the automatic return after a look or a follow, `ptz_automatic_move` audit, `homeDrift` health metric and a bounded recalibration.
- **Guard on duty.** `GetDuty` on guard's presence RPC; camera's `DutyGate` (15 s cache, fails closed); `CameraAction::Move`/`Preset` for Resident and Guard; a Guard cannot save, delete or set home. Credentials provisioned by setup, provision-host and native-stack.
- **Measured.** camera 42/42 (new `camera-ptz-lease-test`, three health cases), guard 42/42, packages 132/132 (role-access pins the new rows), comment/deps/routes gates, `rpc-credentials-test`, `internal-pki-test`, `setup-config-only-test` green.

## Item 14: the tidy debt, paid back under the baseline

- **Before.** 2346 findings over 1365 TUs against a baseline of 2184; 23 checks above their count. Most of it: `bugprone-unchecked-optional-access` (+56) and `modernize-use-designated-initializers` (+63).
- **What changed.**
  - The 63-row pin table of `llm-context-selector-test` uses designated initializers.
  - Tests that read an optional after `REQUIRE(x.has_value())` take it through one shared helper, `valueOf` (`packages/lib/test-support/src/test-support/value-of.hxx`), which throws on an empty optional. clang-tidy 23 counts `.value()` as unchecked, and a `REQUIRE` is not a branch it follows.
  - The camera control service checks `result && result->ok` where it used to read through `succeeded()`.
  - The swappable pairs take a struct: `CameraInstant` (scene log), `PtzInstant` (lease), `DutyInstant` (duty gate), `NameWindow` and `JoinNamedInput` (llm turn), and the test helpers `ConfigFiles` and `ClockTime`.
  - The small checks: single-char appends, `starts_with`, `std::array` for the device digest, `static_cast` for the health event, braced returns in `cert-service`, `emplace_back`, `[[nodiscard]]`, `scoped_lock`, `string_view` for `statusName`, range-for in the Laya runtime test, a raw string in the saga test.
  - `llm-refusal-rules-test` drops its own doctest main and links `argus::lib::doctest-main`.
- **Found on the way.** `llm-duress-screen-test`'s once-per-speaker case raced its 10 ms retry under a loaded build (4 raises for 3). It now runs with a 200 ms retry base, so the second utterance lands while the first raise is pending.
- **After.** 2169 findings over 1368 TUs, no check above its count; the baseline is ratcheted to 2169.

## T3: the backlog track

Not built and not run yet: every unit below passed the comment, dependency and route gates, and the batch builds and runs once at integration.

- **Debt (D2, D3, D6, D7, D8, R1).** The shared runner quits Drogon inside `main()` and the auth suites drop their own; the guard settings table lists every catalog key; the legacy encounter outbox is dropped once its pending rows have crossed; a resource frame for an unknown role is acked quietly; the writerless sync tables are measured on first pull, with a staged retirement plan, because the server cannot tell an app's schema and removing them now would break the v10/v11 apps.
- **F1, TURN.** coturn in compose with credentials from setup and provision, `GET /rtc/ice`, relay servers in the `/rtc/token` grant; the app (web, phone, desktop) uses them. The relay-down acceptance is a checklist in the frontend `CONTEXT.md`.
- **F1, patrol.** Scheduled preset tours (`/camera/{id}/patrol`), a pure `patrol_plan::decide`, the PTZ lease as Argus (the lowest holder) and the scene-log move windows.
- **F3, APNs VoIP.** The key stays with the push relay (argus-cloud): a home holding it could push to every Argus phone. The home already rings `fcm` and `apns_voip` devices, never `apns`; one routing test added.
- **F6a, recurring reminders from the assistant.** `memory.remind` takes a `repeat` rule read in es, es-PE and en; a weekday set becomes one weekly series per day; no call is scheduled for a series; a cadence the rule cannot hold is refused and nothing saved. "Recuérdame el día 5 de cada mes" is read as a question and left to Laya.
- **F6b, one occurrence of a series.** `recurrence_exceptions` on `reminder` and `calendar_event`, `PATCH …/occurrence` for each, 409 on a row that does not repeat, 422 on a time the series does not produce. A reminder occurrence is only skipped or moved between its neighbours, so the firing order never changes. Frontend schema v14 and the three agenda actions. Open: later occurrences of a recurring event are still not announced; `calendar.list_events` ignores exceptions; a snoozed reminder answers 422 to "this occurrence".
- **Needs a live stack.** Golden recordings of `/rtc/ice`, the patrol routes and the 13 hand-written occurrence probes; a call through the relay from mobile data; the coturn container started once.

## F8/F9: parallel conversations and the messenger

Track T4. Backend branch `feat/t4-conversations` (base `integ/close-batch-3`), frontend branch
`feat/t4-conversations-ui` (base frontend `origin/master`). Nothing was compiled, built or run: the owner ordered
the tests to run once at the end. The gates that were run: `check-comments`, `check-deps` and `check-routes`
(baseline refreshed with `route_scan.py`), all green.

- **What was ported.** From `track/b4-conversations`: the per-person conversation store, the takeover and switch
  note, the purge, the leg contract (`ConversationLegKind`, `ChatRequest` fields 17 and 18, `VoiceStart.leg` moved
  to field 6 because 5 is now `later_lines`), the voice leg arbiter and route, and the memory scopes with recall
  scoped by the listener. From `track/b4-messenger`: the relay tool, rules family, recipient resolution, relay
  threads, caps, visitor ask and turn integration. Every ported test links `argus::lib::doctest-main`.
- **Left on the tracks, not ported.** The STT utterance batcher, the TTS fair queue and turnstile, the sequence
  scheduler (P1/P2 gated), the leg correlation detector and the P3 harness: they are not needed for the
  conversation threads or the messenger and are untouched in STT and TTS.
- **Found and fixed while porting.** `kActionPhrases` gained an eleventh row in an array of ten; `relay-service.hxx`
  named `ConversationPort` outside its namespace; `stt-client.cc` read a `conversationKey` that `StreamInput` does not
  have (dropped with the batcher); `ConversationPurgeTask` and `setChannel` took three parameters.
- **Decisions (the plan's recommended options).**
  - The app-visible threads live in argus-notification (`conversation_message`), not in argus-llm: llm is internal
    by its rules, notification already owns what reaches a person, has the push path, `Announce` through voice and
    the change feed into sync. argus-llm writes there through `ConversationService` and keeps its own store for the
    hot context.
  - A message never rings (owner ruling 8.4): said inside a live session, else a `household_message` notification.
  - Consent: the sender hears the exact words read back and confirms them in the next turn; the code binds the
    recipient and the words.
  - No impersonation: "de parte de", "en nombre de", "hazte pasar por", "anónimo", "on behalf of",
    "anonymously" and the like are refused even when the relay is offered.
  - The sender learns only the delivery state (`sent`, `delivered`, `notified`, `answered`, `expired`), never the
    recipient's read state or whereabouts.
  - Typed messages from the app are not part of this unit: the app reads, marks read and clears.
- **Contracts.**
  - Sync table `conversation_message`, `TableName::ConversationMessage = 24`, `TABLE_NAME_CONVERSATION_MESSAGE = 24`,
    own rows, deletions on. Row: `id, userId, counterpartId (0 = Argus), counterpartName, direction (out|in), kind
    (turn|relay|reply|visitor), body, leg (camera|phone|laptop|null), state, messageKey, replyToKey, isRead, readAt,
    createdAt, updatedAt, deletedAt`. No new `SyncOperation`: `Add`, `Delete` and user audit diffs.
  - Routes on argus-notification: `GET /conversation`, `PATCH /conversation/{counterpartId}/read`,
    `DELETE /conversation/{counterpartId}`; filter chains in `route-baseline.txt`.
  - gRPC: `NotificationService.PullConversation` (sync caller), `ConversationService.RelayMessage`,
    `SettleMessage`, `AppendTurns` (llm caller). Notification kind `household_message`, data
    `{kind, fromUserId, messageKey, counterpartId}`.
  - role-access: `conversation_message` read/update/delete for Resident, Guard and Guest, baseline and own-row;
    capability `messages.relay` (core, baseline, every registered role) gates the app's messenger.
  - `voice:start` payload `leg: "phone" | "laptop"`.
  - Schema: argus-notification `database/schema.sql` gains `conversation_message`; argus-llm gains `conversation`,
    `conversation_turn`, `relay_thread`, `visitor_notice`. Frontend WatermelonDB schema 13 to 14 with a
    `createTable` migration.
- **Frontend.** The `conversation_message` table, its sync and audit handling, the Conversations list and thread
  screens painted from the local database, mark read and clear through HTTP, the Messages entry on Inicio gated
  on `messages.relay`, the `household_message` notification opening its thread, and the leg on `voice:start`.
- **Commits.** Backend: `59271c4f` (conversation core and memory scopes), `e76d8677` (inbox, sync table, routes,
  role access), `8810bbf8` (messenger), plus the documentation commit. Frontend: `7debe1e`, `c0a31f4`, `0e6138c`,
  `6d2af17`.
- **What remains.**
  - The build and every test of llm, notification, sync, voice, packages and the frontend.
  - `voice.use_conversation` stays off until the conversation path passes its tests (ruling Q16); the app's
    Argus thread is filled regardless, from every identified turn.
  - Golden HTTP fixtures for the three `/conversation` routes (`scripts/fixtures/http/`), and the frontend
    `HTTP_CONTRACTS` entries that depend on them.
  - The STT, TTS and LLM parallelism of the plan's section 3 and the co-located handling of section 4 (with the
    watchman).
  - A visitor's notice cannot be answered back to the visitor; typed messages from the app; the call preference
    `live_announce` is not consulted before announcing a message.
  - The P1 to P5 measurements.
## Items 7–9: TP, AL B3–B8, YG

Track T2, camera sight (`docs/history/plans/camera-sight-plan.md`). By the owner's order nothing in this track was compiled, built or tested: the unit tests are written and run with the full build after every track merges. The static gates (`check-comments.sh`, `check-deps.sh`, `check-routes.sh`) were run.

### 7. TP: loss of sight, the maintenance context (A5) and its tests (A6)

- **Camera.** The health monitor measures saturation, entropy, edges and sharpness against the reference, the 4×4 block loss, the view shift and the chroma, and classifies `whiteout`, `blinded`, `partial_cover`, `dark_sudden` beside `covered`, `blurred` and `moved`. The day/night switch holds the checks for 30 s and each light keeps its own reference. In protection the cadence is 5 s. On onset a `tamper` clip is requested and the last good frame is stored as a still clip row, read through the clip capability. `camera_clip.trigger` widens to `tamper` and `look` with a boot rebuild. `CameraActionService.Relearn` is new.
- **Guard.** A 5 s tamper sweep with the plan's windows per type and mode (`guard.tamper.*`), the several-cameras rule once per group, the slow-blur lens notice, the new copy. Declared maintenance (`guard_maintenance`, six routes, Owner/Resident anywhere, Guard on duty in its environment, Guest and Unknown refused), seen maintenance from the known-face sightings, the home question (`guard_tamper_ask`), duress flagging, the unknown-person end and the view check with relearn.
- **Contracts.** Wire codes `GUARD_OFF_DUTY`, `PRESETS_NOT_YOURS`, `MAINTENANCE_OUT_OF_SCOPE`, `NOT_A_TAMPER` (the two camera refusals of AL-B1 now carry their own codes instead of `FORBIDDEN`); resource scopes `(camera, aim)` and `(guard, maintenance)`; capabilities `camera.aim` and `guard.maintenance`; the `ptz/lease` camera action row.
- **Recommended options taken.** Duress is read from a duress alert of the declarer within 15 min (the phrase or the PIN already raise one), not from a PIN in the maintenance body. "Seen" uses the known-face sightings of the object feed; the voiceprint is not consulted. Tamper incidents keep `rule = camera_tamper`; a group incident stores `status: group` and the camera ids. A slow blur sends a passive notice in every mode.
- **Not measured.** Cover-to-alert time per type and the false-alarm rate over a recorded day (A6) need the sandbox run at the end; the golden HTTP fixtures for the new guard routes are recorded with the live fleet then.

### 8. AL B3–B8: the active look

- **B3.** The second look on the main stream for every camera that cannot look: doubtful person events (confidence 0.25–0.45, a box under 3 % of the frame, an unknown person in protection) are re-detected on up to 3 main-stream crops within 3 s; confirmed they go out with the better crop, not confirmed they are dropped and the area cools 60 s.
- **B4.** `CameraActionService.Look` and the guard's verification state: in protection a doubtful observation on a PTZ camera with a home and `activeLook` waits for a look (centering with the dead zone and the no-aim sectors, the settle, the second look, 8 s budget, 6 moves a minute, a daily cap); a clear one alarms at once and the look runs for evidence. Not confirmed is a discard with a 60 s cooldown. While Argus holds the camera away from home the health monitor keeps the home reference and the operator drops its pixel zones and blacks out the no-aim sectors.
- **B5.** Follow after a confirmed look in away or armed: one target, re-acquired at the center after each own move, the lead only on a consistent velocity, one move in flight, ends 10 s after the target is lost and returns home 30 s later.
- **B6.** Patrol through the camera's `patrolPresets`, shipped off (`ptz.patrol_enabled = false`), only away or armed, every 10 min at most, pausing for any viewer or person lease.
- **B7.** `camera.goto_preset`, `camera.look`, `guard.maintenance_start`, `guard.maintenance_end`, `guard.maintenance_status` over MCP, routed by the llm's rule tier (`sight-command`), never chosen by the model; Owner, Resident and a Guard on duty.
- **B8 (backend half).** `GET`/`PATCH /camera/{id}/aim`, `POST /camera/{id}/ptz/lease`, `ResourceChanged (camera, aim)`, the aim settings in the synced camera `config`, look crops and tamper evidence as still clip rows.
- **B8 (app).** Frontend `feat/t2-camera-sight-ui`: the "Argus está mirando" indicator and the lease state on live view, the viewer hold/release around the PTZ panel, set/go home and a Guard on duty moving the camera, the `GUARD_OFF_DUTY`/`PRESETS_NOT_YOURS`/`CAMERA_CONTROL_LOCKED` refusals in Spanish and English, the "Mirada activa" settings with the no-aim sector editor, the tamper notices, the evidence kept on arrival, "Fui yo", the maintenance panel and the view and home questions; painted from the persisted camera row first, then from `GET /camera/{id}/aim` and the `ResourceChanged` frames. Component and model tests written, not run.
- **Recommended options taken.** The look's crops are stored on the camera as clip rows and the RPC answers their ids, so the incident card reads them through the clip capability. The position is Argus's own estimate from relative moves (Tapo `motorMove` is degrees); a move to a non-home preset makes it unknown and Argus then neither looks nor follows until the camera is home. The patrol schedule is the household's away/armed posture.
- **Left.** B0, the per-model calibration on the owner's cameras, and the B9 measurements (false alarms avoided, look latency p50/p95, moves per day) come at the end with the owner present; the FOV, settle and dead zone are config defaults until then. Each maintenance window as a memory episode in the llm is not built.

### 9. YG: the YOLO detector can see the GPU

- **The defect.** `camera-objects` linked both `argus::hardware-profile` (built with `ARGUS_NO_NCNN_GPU`) and `argus::hardware-profile-gpu`; both define `HardwareProbe::get()`, the linker took the first, and the detector always read `vulkan=no`.
- **The fix.** The module links only the `-gpu` variant, so the probe is ncnn's and `objects.use_vulkan` really chooses the backend.
- **Behind a flag.** `objects.use_vulkan` now defaults to `false` in both templates and in the C++ defaults (`ObjectsConfig`, `ObjectDetectorOptions`). It stays out of the settings-wire catalog, whose own test refuses `vulkan` keys as plumbing.
- **Left.** The measurement (GPU against CPU per frame, process CPU, on the reference host) comes at the end of the programme, after the merge; only then is the default revisited. Identity links both variants too and has the same blind probe; it is untouched, because FaceService gates on a discrete GPU and that change is not in T2.

## Item 6: camera call CC1–CC8

Track T1 of the close-all programme: the backend of the camera call plan (`docs/history/plans/camera-call-plan.md`), built on CC0 and AL-B1/B2. The owner ordered no compile and no test run in this track: every unit ships with its unit tests written but not built, and the full build and the suites run once after all tracks merge. The cheap static gates (`check-comments`, `check-deps`, `check-routes`) were run on every unit.

### CC1: the camera's microphone feed and streaming speaker

- **Contract.** `argus.camera.v1.CameraWatchService` (`packages/contracts/proto/argus/camera/v1/watch.proto`) on camera's internal listener, caller argus-voice only (`[grpc] caller_voice` ↔ voice `[camera] credential`, paired by setup, native-stack and provision-host; `voice camera grpc_target camera` in the internal PKI rows). Client: `argus::clients::camera-watch` (`CameraWatchClient`: `listWatch`, a blocking `readMicrophone` with a stop token, `speak` returning a `WatchSpeakerStream`).
  - `ListWatch` → `{cameras: [{camera_id, name, listening, refusal, talk, pan_tilt}]}`.
  - `OpenMicrophone {camera_id}` → stream of `{pcm (s16le 16 kHz mono, 32 ms), sequence, captured_at_ms, gap}`; a refusal ends the stream with `FAILED_PRECONDITION` and the `WatchRefusal` name.
  - `Speak`: `open {camera_id, sample_rate, earcon, conversation_id}`, `pcm`…, `end` → events `READY`, `PLAYING {played_ms, queued_ms}`, `DONE {playback_end_ms}`, `BUSY`, `REFUSED {refusal}`, `LINE_LOST`.
- **Gate.** Module, enabled camera, household watchman, household audio consent, the camera's watchman flag, its "sin audio" flag and the lens mask, rechecked while listening and while speaking.
- **One reader per camera**, reconnecting with backoff, `gap` on the first frame after a loss; at most `max_listeners` cameras.
- **The speaker** never forces the talk line (`BUSY`), and pads the Tapo start drop: 300 ms earcon (or silence), 700 ms silence, the speech paced in 120 ms packets, 300 ms of tail.
- **Config** (`[watch]` in camera's `config.toml.example`, the deploy template and the settings catalog, group `watchman`): `mic_frame_ms` 32, `mic_reconnect_ms` 1000, `mic_reconnect_max_ms` 10000, `mic_linger_ms` 2000, `mic_queue_frames` 64, `gate_recheck_ms` 1000, `board_refresh_ms` 2000, `max_listeners` 16, `earcon_ms` 300, `lead_silence_ms` 700, `tail_pad_ms` 300, `packet_ms` 120, `speak_queue_ms` 60000, `speak_idle_ms` 15000.
- **Tests written:** `camera-watch-test` (gate order, prologue lengths, little-endian pcm, one reader for two subscribers with a reconnect gap and a gate close, busy line, prologue + body + tail packet count), `camera-watch-client-test` (refusal parsing, microphone frames then refusal, speaker round trip).
- **Found on the way, not fixed here:** `argus-deploy/config.camera.toml.example` keeps the identity keys (`auto_enroll`, `identify_interval_ms`, …) under `[notifications]` and, since AL-B1, under `[guard]`; `operator_config::resolveIdentity` reads them from `[identity]`, so the deploy template's values are ignored and the code defaults apply.

### CC2: the ear in argus-voice

- **Stage one, two ways.** argus-stt builds sherpa-onnx's keyword spotter and serves `Transcription.Spot` (bidirectional: `SpotChunk {samples, sample_rate, reset}` → `SpotEvent {keyword, at_sample, start_time}`), `[kws]` in stt's template, deploy template and catalog (`kws.threshold` 0.25, `kws.score` 1.5, `kws.max_active_paths` 4, `kws.trailing_blanks` 1). Without the model files the spotter is absent and `Spot` answers `UNAVAILABLE`; the ear then transcribes speech segments for stage one (`watch.stage1 = "auto"`). No model was downloaded: the model choice and its tokenised keywords file belong to M1.
- **The ear** (`services/voice/src/feature/watch`): one `CameraEar` per listening camera (VAD, 2 s RAM ring, noise floor and speech SNR, stage one), stage two on the ring plus 1 s (the vocative rule: name first, after a greeting, between commas or last after a comma; "Argu" alone and "argumento" never), the echo guard (self mute plus `echo_tail_ms`, cross-camera raise and envelope correlation ≥ 0.6), the broadcast drop (three cameras within 150 ms at similar level), the TV exclusion raise, and the fire log (verdict and scores, no audio, no text).
- **Camera.** `camera.tv_exclusion`, settable through `PATCH /camera/{id} {tvExclusion}`, on the row JSON, sync `CameraRow.tv_exclusion = 24` and `WatchCamera.tv_exclusion = 7`. **App contract:** the camera row gains `tvExclusion` (bool) beside `watchman` and `noAudio`.
- **Self-trigger by text.** `withoutSelfName` rewrites "Argus", "Argos" and "Árgus" in a reply to "tu asistente" / "your assistant" (capitalised at a sentence start); `voice-watch-test` pins it. It is applied to every camera reply in CC4.
- **Config** (`[watch]` in voice's template, deploy template and catalog, group `watchman`, applied at restart): `enabled`, `list_refresh_ms` 10000, `reconnect_ms` 2000, `stage1` auto, `ring_ms` 2000, `post_roll_ms` 1000, `wake_max_distance` 2, `stt_stage1_max_ms` 2500, `stt_stage1_min_ms` 300, `stt_stage1_min_snr_db` 6, `kws_hangover_ms` 400, `kws_retry_ms` 5000, `min_wake_snr_db` 3, `fire_debounce_ms` 1500, `broadcast_window_ms` 150, `broadcast_min_cameras` 3, `broadcast_spread_db` 6, `tv_raise_db` 6, `echo_tail_ms` 800, `echo_correlation` 0.6, `echo_raise_db` 6, `echo_max_lag_ms` 1000, `floor_rise_db_per_s` 0.5, `floor_fall_db_per_s` 10, `fire_log_size` 200; `[camera] grpc_target`, `credential`.
- **Tests written:** `voice-watch-test` (vocative versus referential, distances, the reply rewrite, closing, hand-off, trailing and panic phrases, stage two, echo correlation, mute and tail, noise floor and SNR, broadcast and TV raise, the fire log), `voice-settings-test` (the watch fallbacks), `camera-controller-test` (the row's new field), `stt-settings-test` (the kws keys).
- **Not done:** STT hotwords for the household's names. argus-stt's offline transducer decodes greedily; per-request hotwords need modified beam search in the recogniser, a change to argus-stt measured on the WER harness. Stage two does not need them (its own edit-distance rule covers the name).

### CC3: arbitration and proximity layer 0

- **Arbitration** in argus-voice: a 400 ms window, the SNR bar (plus the TV raise), the gain-normalised SNR ranking, stage two for the best two at once, the score `1.0 × wake + 0.1 × SNR + 1.0 × stage two`, one winner, the rest `duplicate_wake`. Config: `arbitration_window_ms` 400, `stage2_candidates` 2, `weight_wake` 1.0, `weight_snr` 0.1, `weight_stage2` 1.0, `gain_weight` 0.5, `gain_alpha` 0.1 (voice template, deploy template, catalog).
- **Rooms.** `argus.guard.v1.PresenceService.CameraRooms` → `{cameras: [{camera_id, environment_id, room, outdoor}]}`, voice admitted as a caller (`[guard] presence_target`/`presence_credential` ↔ guard `rpc.callers.voice`, paired by setup, native-stack and provision-host). Decision recorded: the room is derived from the guard camera context's role and environment instead of a new room column, so the Owner's existing camera-context screen already sets it; the app needs nothing new for rooms.
- **Layer 0.** Per-camera noise floor and speech SNR (CC2), per-camera gain (`CameraGains`) and the conversation speaker's per-camera level (`SpeakerLevels`).
- **Tests written:** `voice-watch-test` (ranking by normalised SNR beats first-to-fire, the score, rooms and roommates, speaker levels).

### CC4: the half-duplex camera conversation

- **The bridge.** `CameraConversation` is a `VoiceSessionSink` on the existing session engine (half duplex), held by `ConversationManager`; the session engine gained `VoiceSessionOptions` (per-session `min_silence`/`max_turn` VAD frames, no "¿me hablas a mí?", the incomplete-turn hold) and the sink hook `shapeSpeech`, both additive, and the phone and LiveKit paths are unchanged.
- **Behaviour.** Earcon and start-drop padding first; the wake's own audio is the first turn; microphone muted while the camera plays and for the echo tail; barge-in only with the wake word plus the speaker's voiceprint; 700 ms endpointing, up to 2 s for a turn that trails off, 30 s per turn; "Un momento" at 1.5 s, the give-up line at 8 s; closes after 20 s of silence, "gracias, Argus" / "eso es todo", or 10 minutes; a dropped or busy camera hands over to a free camera of the same room with the context kept, otherwise the conversation is resumable for 3 minutes.
- **Config** (voice `[watch]`, template, deploy template, catalog): `end_silence_ms` 700, `incomplete_hold_ms` 2000, `max_turn_ms` 30000, `ack_after_ms` 1500, `fail_after_ms` 8000, `idle_close_ms` 20000, `max_length_ms` 600000, `resumable_ms` 180000, `reply_gap_ms` 700, `camera_drop_ms` 3000.
- **Tests written:** `voice-watch-test` (limits, the acknowledgement and give-up timers, the lines, the hop to a free camera of the same room), `voice-session-seam-test` (the camera session's own endpointing, no addressee question, the sink's speech shaping, the incomplete-turn hold and release).

### CC5: identity, the floors and safety

- **Face first.** `CameraWatchService.FaceWindow {camera_id, window_ms}` → `{faces: [{person_id, user_id, name, role, confidence, quality, visitor, trusted, matched}], crops}` (camera matches the window's person crops through identity); only `accepted` faces count. The voiceprint is a soft signal only.
- **Sets.** Member (camera set), discreet member (an unknown face beside them), guest (no face, two members, or face and voice disagreeing), visitor (only unknown faces). A voice-only match never raises the set.
- **The floor in argus-llm.** `ChatRequest.surface` (`camera`, `camera_visitor`; proto field 19 since integration, HTTP `surface`) filters execution: a tool outside the camera set is never run; a member gets a `watch_confirm` notification (data `{kind: "watch_confirm", url: "/assistant/confirm?tool=<name>", tool, arguments}`) and a spoken pointer to the phone.
- **Safety.** Panic phrases raise `argus.guard.v1.DuressService.RaiseCameraPanic {camera_id, user_id, speaker, said_at, conversation_id}` for anyone (voice credential `rpc.callers.voice`); a member's duress phrase goes through the existing llm screen. The phone-call rule ignores a wake from a person already on a call.
- **Overlap.** A turn in another voice without the wake word is dropped unless the member delegated ("puedes hablar con ella").
- **Notifications.** argus-notification admits voice for one-recipient `watch_busy`, `watch_resume`, `watch_details`, `watch_confirm`, `watch_message` and llm for `watch_details`, `watch_confirm`.
- **Config:** voice `face_window_ms` 1500; camera `face_window_max_ms` 3000, `face_window_max_crops` 6.
- **Tests written:** `voice-watch-test` (every set, voice-only, disagreement, quality floor, delegation phrase), `llm-tool-runtime-test` (camera floor with confirmation, visitor refusal, app unchanged), `guard-context-test` (room keys), `voice-session-seam-test` (the surface reaches the session).
- **Not done:** per-turn re-identification at one camera, active speaker detection (deferred by the plan), the visitor's recorded spoken consent and the minor rule for registration (registration stays the existing identity feature under `visitor_recognition`), and a duress path for an unidentified speaker (duress needs a member; the panic phrase covers anyone).

### CC6: the phone — the live transcript and "Continuar aquí"

Built to the contract the app track (T5) uses:

- **WS.** `SyncOperation::WatchUpdate = 15` (`watch_update`), sent by voice through `EmitToUser` to the speaker's user room only. `info`: `{conversationId, cameraId, cameraName, state (waking|listening|thinking|speaking|resumable|ended), handoff (camera|offering|bridging|device), audience (member|discreet|guest|visitor), startedAt, updatedAt, resumableUntil, endReason, turns: [{id, role (user|argus), text, at, final}]}`, seconds; `resumableUntil` and `endReason` are `null` when absent. `endReason`: `silence|closing|limit|camera_lost|busy|handoff|superseded|shutdown|gate|failed|expired`.
- **Routes** (argus-sync, `kRtcAccess`, every role, route baseline updated): `GET /rtc/watch` → `{conversations: [same shape]}`; `POST /rtc/watch/{id}/end` → `{ended: true}` or `WATCH_NOT_FOUND`.
- **Continue here.** `POST /rtc/token {watchConversationId}` → the usual grant; refusals `WATCH_NOT_FOUND` (404) and `WATCH_EXPIRED` (410). Make-before-break: the camera leg ends when the phone's microphone is live; the same LLM session and its turns continue with "Seguimos aquí.".
- **Hand-off signal.** `WatchUpdate.handoff` goes `bridging` when the token is issued and `device` when the phone took over. **Difference from the contract:** the LiveKit data topic `argus.handoff` is not published (the contract allows either); `WatchUpdate` carries the same states.
- **Notifications.** `watch_busy` and `watch_resume` (data `conversationId`, plus `cameraId`, `cameraName`, `resumableUntil`), `watch_details` (Argus's words after a discreet conversation), `watch_confirm` (from argus-llm, data `url` = in-app path `/assistant/confirm?tool=<name>`, plus `tool` and `arguments`), `watch_message` (a visitor's words, to every active Owner).
- **Difference from the contract:** `POST /rtc/watch/{id}/end` passes `ValidJsonFilter` like every POST (rule 5), so the app sends `{}` as the body.
- **Contracts added:** voice `RtcJoin.watch_conversation_id = 11`, `VoiceService.ListWatchConversations`/`EndWatchConversation`; `ErrorCode::WatchNotFound`/`WatchExpired`; sync control admits voice for `WatchUpdate` only; voice `[sync] control_target`/`control_credential` ↔ sync `rpc.callers.voice` (setup, native-stack and provision-host via the fleet caller rows, internal PKI row).
- **Tests written:** `rtc-token-test` (watch id validation, the continue grant and its join, 404 and 410, list shape, end), `voice-watch-test` (the frame shape, the four notices, the carried turns), `sync-contract-vocabulary-test` (15 ↔ `watch_update`), `error-definition-test`, `capability-test` (the two routes).
- **To record at integration:** the golden HTTP fixtures of the two new routes (`scripts/golden-http.py record --only "GET /rtc/watch" --only "POST /rtc/watch/{1}/end"` on a live stack).

### CC7: proximity layer 1 — sightings and aiming

- **Sightings.** argus-camera notes every evaluated person box (`PersonSightings`, held `sighting_hold_ms` 45 s): camera, track, matched person, normalised centre and size, night, time. `CameraWatchService.Sightings {since_ms}` → `{sightings: [{camera_id, track_id, person_id, known, center_x, center_y, width, height, night, at_ms}]}`, readable by argus-voice and by argus-guard (guard's existing camera caller credential; guard reads `ListWatch` and `Sightings` only).
- **Aiming.** `CameraWatchService.Aim {camera_id, conversation_id, mode FIND|RELEASE}` → `{outcome CENTERED|IN_FRAME|NOT_FOUND|REFUSED|NO_PTZ|RELEASED, detail}`. FIND: never on a camera the watch gate closes (lens mask included), never against a person's PTZ lease (Argus lease, re-checked before every move), the last direction a person was seen since the camera last moved, else up to `aim_max_presets` presets until YOLO sees a person; centring by field of view and dead zone, each move a `CameraSceneLog` move window. RELEASE: home `aim_return_ms` after the conversation ends unless re-aimed. Voice asks FIND when a conversation opens on a pan-tilt camera with no face in the face window and RELEASE when it ends.
- **Decision recorded:** the move rule is expressed as fields of view (`aim_hfov_degrees` 100, `aim_vfov_degrees` 56) instead of degrees per pixel, so it does not depend on which stream's resolution the detector ran on; the plan's 15° ≈ 200 px is the measurement M3 checks it against.
- **Config** (camera `[watch]`, template, deploy template, catalog): `sighting_hold_ms` 45000, `aim_in_frame_ms` 3000, `aim_hfov_degrees` 100, `aim_vfov_degrees` 56, `aim_dead_zone_percent` 5, `aim_settle_ms` 1500, `aim_look_ms` 2000, `aim_max_presets` 4, `aim_return_ms` 30000.
- **Tests written:** `camera-watch-test` (centred, pan share, tilt toward the face, newest sighting after the last move, preset ids from both driver shapes, the sightings hold, the aimer's refusals), `camera-settings-test` (catalog size 50 — it had not been raised for CC1's and CC5's watch keys either).
- **Not done:** no-aim sectors (camera-sight B4/B8 are not in the tree, so none is honoured); the tilt sign per driver and both fields of view are M3; a hop to a roommate camera does not aim the new camera.

### CC8: proximity layers 2 and 3, the fusion, the opt-out and the Owner's queries

- **Fusion** (argus-guard `feature/proximity`): a per-person discrete room belief over the CC3 room keys; evidence face (1.0, half in IR) > radar plus person box (0.8) > voice level (0.6, half without voiceprint) > BLE (0.4 × RSSI share); half-life 20 s, switch after a 0.2 lead held 3 s, forgotten after 5 minutes without evidence. In memory only.
- **Opt-out.** Only members whose identity privacy `location` is on (and the household's) are ever located; a member who turns it off is dropped at the next poll. Voice's own routing never reads the belief.
- **App contracts** (all on argus-guard's HTTPS listener, behind the usual chain, gated by the surveillance module, route baseline updated):
  - `GET /guard/proximity/me` and `PUT /guard/proximity/me {ble: bool}` → `{ble: {enabled, seed (base64, null while off), rotateSeconds, receivers (count)}, located}`; every role (`kGuardAccess`), capability `proximity.own`. Turning the beacon on while located is off answers `409 CONFLICT`; when identity cannot be read, `503`.
  - `GET /guard/proximity/receivers` → `{receivers: [{id, name, room, kind, host, port, online, lastSeenAt (seconds or null)}]}`; `POST` `{name, room, kind: "esphome", host, port}` → `201` with the receiver; `DELETE /guard/proximity/receivers/{id}` → `204` or `404 NOT_FOUND`.
  - `GET /guard/proximity/sensors` → `{sensors: [{id, name, room, model, source, host, port, entityId, cameraId, panOffsetDeg, online, lastSeenAt}]}`; `POST` `{name, room, model: ld2410|ld2450|fp2, source: esphome|home_assistant, host, port, entityId, cameraId?, panOffsetDeg?}` → `201`; `DELETE /guard/proximity/sensors/{id}` → `204` or `404`.
  - `GET /guard/proximity/people` → `{people: [{userId, room, confidence, since, updatedAt, evidence: face|radarBox|voice|ble}]}`, Owner only.
  - Receivers, sensors and people are Owner only (capability `proximity.manage`). `room` is the CC3 camera room key (`<environmentId>:<role>`), which the app takes from the camera contexts.
  - Beacon token: `HMAC-SHA256(base64decode(seed), uint64_be(floor(now / rotateSeconds)))[0:16]` advertised as a 128-bit service UUID; guard accepts the neighbouring rotations and either byte order.
- **Receivers** are read through the ESPHome web server text sensor (`/text_sensor/argus_ble`, `uuid,rssi;…`); the native API was not chosen (it needs a protobuf client). **Sensors**: ESPHome `/binary_sensor/<entity>`, Home Assistant `/api/states/<entity>` with `[guard.proximity] home_assistant_token`.
- **Wire added:** `PresenceService.ReportVoiceEvidence` (voice caller only) and `GuardPresenceClient::reportVoice`; voice reports the loudest camera on each member turn. Guard reads `CameraWatchService.Sightings` with its existing camera credential (`camera.actions_target`/`actions_credential` ↔ camera `grpc.caller_guard`).
- **Config** (`[guard.proximity]` in guard's template and deploy template; the numbers in the settings catalog, group `proximity`, live): `poll_ms` 2000, `rotate_seconds` 900, `ble_rssi_floor` 95, `offline_after_ms` 10000, `request_timeout_ms` 1500, `sighting_window_ms` 45000, `weight_face` 1.0, `weight_radar_box` 0.8, `weight_voice` 0.6, `weight_ble` 0.4, `night_face_factor` 0.5, `half_life_ms` 20000, `switch_margin` 0.2, `switch_hold_ms` 3000, `forget_ms` 300000; template only: `enabled` true, `ble_entity` "argus_ble", `home_assistant_token` "".
- **Tests written:** `guard-proximity-test` (the token against vectors computed independently, the counter, seeds, the table's neighbouring rotations and byte orders, the receiver and radar payloads of a fake sensor, request paths, weights and decay, the switching hysteresis, opt-in and opt-out on a real schema, a beacon heard by a fake receiver locating a member, a face moving them, an opt-out forgetting them, sensor registration and online state), `guard-presence-client-test` (the voice report), `role-access-test` and `capability-test` (the proximity routes), `guard-settings-test` (catalog size 60).
- **To record at integration:** the golden HTTP fixtures of the nine proximity routes (`scripts/golden-http.py record --only "GET /guard/proximity/me"` and the rest) on a live stack.
- **Also fixed here:** root `AGENTS.md` rule 18 now lists `WatchUpdate=15` (CC6 had left it out) and rule 7 the proximity routes.
- **Not done:** iOS IRK resolution; the LD2450 x/y targets and aiming a camera at a voice from a sensor's camera mapping; "where Argus speaks" from the belief (no consumer yet); the assistant's Owner-only "¿Dónde está…?" tool (a guard MCP tool plus a decider route); the S21 battery measurement of the beacon; a hardware proof of the ESPHome receiver lambda and of any radar.

### Measurements left for the end (M1–M4) and what they set

- **M1, the ear:** the KWS model choice, its tokenised keywords file (`[kws] keywords_file`), `kws.threshold`/`score`, and voice `[watch] min_wake_snr_db`, `stt_stage1_*`, `wake_max_distance` from false-accept and miss rates on the owner's cameras.
- **M2, the mouth:** each camera's start drop and return delay — camera `[watch] earcon_ms`, `lead_silence_ms`, `tail_pad_ms`, `packet_ms` and voice `echo_tail_ms`, `echo_correlation`, `echo_max_lag_ms`.
- **M3, aiming:** `aim_hfov_degrees`, `aim_vfov_degrees`, the tilt sign per driver, `aim_dead_zone_percent`, `aim_settle_ms` against the "15° ≈ 200 px" measurement.
- **M4, rooms:** the arbitration weights and gains (`weight_*`, `gain_*`), and the proximity weights, half-life, margin and hold, plus the BLE RSSI floor per receiver.


## Integration

Backend `integ/close-batch-4` (T3, T4, then T2 and T1 merged onto `integ/close-batch-3`) and frontend `integ/frontend-close-4` (T5, T4, T3, T2 merged onto `origin/master`). Nothing was built, compiled or run; the comment, dependency and route gates were run on the backend and pass.

- **One patrol.** T3's scheduled preset tour (`/camera/{id}/patrol`, `camera_patrol`, `PatrolRunner`, `patrol_plan::decide`) is the only patrol. Its steps go through T2's `gotoByArgus` (`AimBoard` activity `patrolling`, Argus away in the scene log, `ResourceChanged (camera, aim)`); T3's `patrolMove` is gone. `PatrolRunner` takes `awayOrArmed` (the guard heartbeat's posture cache) and walks only while the household is away or armed; leaving that posture ends the patrol on the way home. T2's `PatrolService` is deleted; its two other jobs (re-reading the per-camera aim settings every 30 s and the follow sweep every second) moved to `LookRefresh` in `feature/look`. The keys are T2's, folded: `[ptz] patrol_enabled` (the switch for every patrol, **on** by default because each patrol is already the owner's per-camera choice), `patrol_every_seconds` (600, the shortest time between the starts of two laps: a finished lap rests at home until then, a new `Rest` step), `patrol_dwell_seconds` (30, 5 to 600: the dwell of a patrol created without `dwellSeconds`). The aim settings lose `patrol`/`patrolPresets` (the app's aim panel loses its switch). The patrol also waits on the wear caps and yields while the camera follows an intruder or a camera conversation's aimer holds it. A patrol step is not audited by `gotoByArgus` (T3's rule: only `started`/`ended`), and `gotoByArgus`'s only caller is the patrol.
- **T1's aimer and T2's board.** `WatchAimer` moves now mark the `AimBoard` (`looking`, position unknown) and Argus away from home, so the health monitor holds its home reference while a conversation aims the camera; its way home already went through `returnHome`.
- **Renumbered.** `ChatRequest.surface` (T1) took field 17, which T4's `conversation_leg` holds: it is field 19. No other collision: `SyncOperation::WatchUpdate = 15` (T1), `TableName::ConversationMessage = 24` (T4), `ResourceScope` `Aim`, `Maintenance` (T2), the error codes appended by name (`WatchNotFound`, `WatchExpired`, then T2's).
- **Counts.** Camera settings catalog 87 (25 + 37 from T2 + 25 from T1), guard 81, guard purge 27 statements, `kGuardAccess` 22, `kRtcAccess` 5; the route baseline lists 195 routes.
- **Defects fixed.** The deploy camera template now keeps `identify`, `min_face_box_px`, `identify_interval_ms`, `best_shot_ms` and `improve_margin` under `[identity]` and drops the enrollment keys the camera no longer reads. Identity links only `hardware-profile-gpu`, so `FaceService` can see a discrete GPU; it stays on the CPU behind `[face] use_vulkan = false`.
- **Frontend.** WatermelonDB versions are sequential: 14 camera `watchman`/`no_audio` (T5), 15 `conversation_message` (T4), 16 `recurrence_exceptions` (T3); each migration test starts from its own previous version and migrates to its own. T1↔T5: `POST /rtc/watch/{id}/end` sends `{}`, a turn with role `argus` reads as the app's assistant turn (it was dropped), the hand-off rests on `WatchUpdate.handoff` (the `argus.handoff` listener stays and never fires), `WATCH_NOT_FOUND`/`WATCH_EXPIRED` map by code; proximity and "Fui yo" already matched.
- **Not reconciled.** The app has no editor for the scheduled patrol (T3 built none; T2's switch went away with its patrol). Golden HTTP recordings of the new routes still need a live stack.

## Item 3: the deploy security cleanups, proven on a live stack

- **What landed.**
  - The shared NATS `argus` user is gone: one nkey per service.
  - The shared S3 key pair is gone: each writer signs with its own bucket-scoped account.
  - No plaintext internal gRPC listener is deployed; every internal surface answers mutual TLS only.
  - The compose network name and subnet are variables.
- **Proof.**
  - The 14 images were built, with each test stage green except argus-llm's quality evals (item 15). argus-llm's runtime image was built from the same commit.
  - The stack was provisioned from scratch with `provision-host.sh` into a data dir outside `/tmp`, then started.
  - `deploy-security-check.sh` live: 35 green, 0 expected, 0 red; relay and tunnel are reported as not deployed, because their profile is off.
  - Static (`--no-docker --no-probe`): 37 green.
  - `deploy-security-check-test.sh` green.
- **Found only by deploying, and fixed.**
  - guard gated its whole RPC server on the retired plaintext `[rpc] address`, so it served no presence, settings, tools or duress RPC.
  - The guard templates named a `notification` presence caller nothing provisions, so guard refused to boot. Settings named `auth` and `voice` callers that install no module gate; those slots and their fleet pairing rows are gone.
  - guard, settings, tts, stt, vlm and llm did not publish their mutual TLS port on loopback, so the check could not reach them.
  - The argus-llm runtime image lacked `libatomic1`.
  - **Two OpenSSLs in one binary.**
    - Debian's `pkg-config --libs grpc++` put `-lssl -lcrypto` (system 3.5) ahead of the Conan archives, so a binary's own OpenSSL calls split between the two copies.
    - argus-vlm and argus-voice crashed in X509 code: first in gRPC's server handshake, then in `/health`.
    - The link lines now carry Conan's OpenSSL alone (`libgrpc.so` brings its own dependency), and `--exclude-libs` keeps the static copy out of the dynamic table.
  - The image test stages linked the vendored llama/ggml libraries at a BuildKit cache path that is gone at test time. The imported targets now point at the copies in `build/<profile>/bin`, the engines are built with an `$ORIGIN` rpath, and the model tests receive their model folders through named build contexts.
  - Two latent races surfaced under load: the guard saga test against its retry worker, and a use-after-free when the safety service was destroyed with a delivery in flight.
- **Disk.** The laptop disk filled during the image builds. The build directories of two retired worktrees were removed (142 GB freed); no source or data was touched.

## Gaps G5: aim and the speaking camera

Backend `gap/g5-camera-aim` from `origin/master` 67b09f17. It closes the camera-side gaps that Item 6 (CC7 and CC8 "Not done") and Items 7–9 left between the camera call and camera sight. By the owner's order nothing was built or run: the unit tests are written, and the full pass runs after every gap track lands. `check-comments`, `check-deps` and `check-routes` pass. No route was added and the frontend is untouched: the sensor rows already carry `cameraId` and `panOffsetDeg`.

- **1. The conversation aimer keeps to the sectors, the lease and the wear caps.** Every relative move of `WatchAimer` goes through one pure check, `watch_aim_plan::holdFor`, in this order:
  - the Argus lease;
  - the wear caps (`[ptz] moves_per_minute` and `daily_moves`, the same `AimBoard` counters the patrol and the look use);
  - the camera's no-aim sectors, read from its aim settings on the watch board's row.
  - **What a hold does.** A move whose target is inside a sector is never sent. A camera that has sectors and whose position is unknown (after a preset) does not move relatively. The answer is the new `AIM_OUTCOME_HELD` with the reason (`lease_held`, `wear`, `position_unknown` or `no_aim_sector`). Voice only logs it, so Argus says nothing.
  - **Preset steps** of the search check the lease and the caps.
  - **Every move counts** against the caps. A relative move now keeps the estimated position (`AimBoard::recordMove`) and a preset still makes it unknown. This corrects the Integration note: the aimer used to mark the position unknown on every move.
- **2. Argus speaks from the room the person is in.** argus-guard's new `PresenceService.RoomOccupancy` answers, for each room, the strongest belief confidence. It carries no user id and only argus-voice may call it.
  - **The choice** is the pure `chooseSpeakingCamera`. When the accepted wakes span rooms and the most confident candidate room reaches `belief_min_confidence` (0.6) and leads the next by `belief_margin` (0.2), the best camera of that room answers. Otherwise arbitration's winner answers (CC3, unchanged).
  - **Cost.** Voice asks for the occupancy only in that case and keeps it `belief_max_age_ms`.
  - **Opt-out.** Members who turned location off are never in the belief, so the opt-out holds.
- **3. LD2450 x/y targets.**
  - **What guard reads.** For an `ld2450` sensor paired with a camera, guard reads the targets on demand from the ESPHome text sensor `<entity><targets_suffix>` or the Home Assistant `sensor.<entity><targets_suffix>`. The value is `x,y;x,y;x,y` in millimetres and empty slots are dropped.
  - **The RPC.** `PresenceService.SensorTargets {camera_id}` answers argus-camera alone with each sensor's pan offset.
  - **The geometry** is pure, in camera's `watch_aim_plan::radarAim`:
    - the bearing is `atan2(x, y)`;
    - the camera pan that faces the target is the bearing plus `panOffsetDeg`;
    - the target nearest the current pan wins;
    - fixes older than `aim_radar_max_age_ms` and targets beyond `aim_radar_max_range_mm` or behind the sensor are ignored;
    - the move is the pan difference, outside the dead zone.
  - **Assumption.** The sensor is mounted at the camera: there is no parallax correction.
- **4. Aiming at a voice from the radar** is a step of the same `WatchAimer::find`, not a second aimer.
  - **When it runs.** When nobody is in frame, the aimer asks guard for the paired targets and turns through the same `holdFor` check.
  - **What it answers.** It looks `aim_look_ms` and centres the person (`CENTERED`, detail `radar`), or answers the new `AIM_OUTCOME_BEARING` when YOLO does not see them yet.
  - **The fallback.** With no radar, no usable target or an unknown position, it goes back to the preset search.
- **Config keys.**
  - Camera `[watch]`: `aim_radar_enabled` true, `aim_radar_max_age_ms` 3000, `aim_radar_max_range_mm` 6000. They are in the template, the deploy template and the catalog, which now holds 90 settings. The golden `settings.json` gains the three rows by hand.
  - Voice `[watch]`: `belief_enabled` true, `belief_min_confidence` 0.6, `belief_margin` 0.2, `belief_max_age_ms` 2000. They are in the template, the deploy template and the catalog; the voice catalog test counts only the non-watch keys.
  - Guard `[guard.proximity]`: `targets_suffix` `"_targets"`, a string kept in the templates only, like `ble_entity`.
- **Wire, additive.**
  - `AimOutcome` gains `HELD = 7` and `BEARING = 8`.
  - `PresenceService` gains `SensorTargets` (caller camera) and `RoomOccupancy` (caller voice).
  - `GuardPresenceClient` gains `sensorTargets` and `roomOccupancy`.
- **Tests written:**
  - `camera-watch-test`: sector hold, unknown position with and without sectors, lease before wear, the bearing, the offset and the turn, the nearest target, stale fixes and empty slots, the unknown position, the proto-to-fix conversion.
  - `camera-settings-test`: 90 settings and the new fallbacks.
  - `guard-proximity-test`: the target payload and paths; only the paired `ld2450` is read; occupancy per room without ids.
  - `guard-presence-test`: each RPC admits only its own caller.
  - `guard-presence-client-test`: both reads.
  - `voice-watch-test`: belief wins, weak or split belief falls back, one room never asks, best camera of the believed room.
  - `voice-settings-test`: the fallbacks.
- **Left.**
  - A look (`CameraActionService.Look`) does not use the radar: it starts from a box in frame.
  - The hop to a roommate camera still does not aim the new camera.
  - The ESPHome/Home Assistant template that publishes the targets text sensor is documented in guard's CONTEXT but not proven on hardware.
  - The parallax between a sensor and a camera mounted apart is not modelled.
  - The thresholds are config defaults until M3 (aim) and M4 (rooms) are measured.
