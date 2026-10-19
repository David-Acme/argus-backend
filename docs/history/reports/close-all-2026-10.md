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
- **The floor in argus-llm.** `ChatRequest.surface` (`camera`, `camera_visitor`; proto field 17, HTTP `surface`) filters execution: a tool outside the camera set is never run; a member gets a `watch_confirm` notification (data `{kind: "watch_confirm", url: "/assistant/confirm?tool=<name>", tool, arguments}`) and a spoken pointer to the phone.
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

