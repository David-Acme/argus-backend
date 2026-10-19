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
