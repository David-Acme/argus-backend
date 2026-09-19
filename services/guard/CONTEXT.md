# argus-guard

Autonomous camera security. Consumes `argus.camera.v1.object_detected` through
a durable JetStream consumer, turns identity-enriched observations into a
deterministic danger level and raises only policy-authorized actions. It owns
`guard.db` and never touches another service's database.

## What it does

- Consumes the camera stream through the durable `argus-guard` JetStream
  consumer (`ARGUS_CAMERA`), deduplicates by `eventId` in
  `guard_observation_inbox` and acknowledges after commit; the inbox plus the
  serialized observation queue make redelivery and restarts idempotent.
- Evaluates the deterministic danger matrix (`guard-policy.cc`): hard floors
  (unknown while away/armed, alert zone, night, escalation, repeats) plus a
  severity raise; expected guests and trusted companions only lower soft cases.
- Merges bounded semantic evidence (`guard-risk.cc`) from a closed vocabulary
  of observable tags; unknown tags are dropped and the model can never lower a
  hard floor.
- Persists every incident in `guard_incident`, every action in `guard_action`
  (with `encounter_id`) and the authorization trail in `guard_action_outbox`;
  `guard_encounter_transition` records each state change with its revision.
- Runs a fast hard-floor lane: `danger >= high` raises notify/announce/alarm
  immediately without waiting for greeting, listening or the VLM/LLM.
- Keeps the model read-only: `GuardAssessment` may call `vision.describe` and
  `camera.listen`, never alarm, siren, announce or any other physical tool, and
  its output is grammar-constrained (GBNF) to the decision schema.
- Every autonomous effect passes through one policy enforcement point
  (`guard-action.cc`) and a persisted outbox before execution; camera commands
  carry a `commandId`, encounter id and expiry, and the camera deduplicates
  them.
- Dialogue is bounded: greeting only with an accepted camera acknowledgement,
  endpointed listening, one repair attempt on silence/noise, then assessment.
- Publishes `argus.guard.v1.heartbeat` (the gateway raw fallback yields while
  fresh) and `argus.guard.v1.encounter_closed` with a finalized redacted
  summary; long-term memory reads only that summary.
- Uploads incident evidence with a retention manifest (`guard_evidence`) and a
  daily deletion worker covering SQLite and the private object store.

## Why the identity hop

Rule 27: the guard cannot read `identity.db`. Recipients and person data come
through the identity SDK, and incidents are stored locally.

## Flags

`[guard] enabled`, `profile`, `default_mode`, `notify_level`,
`announce_level`, `alarm_level`, `announce_text`, `announce_lang`,
`alarm_seconds`, `arm_siren`, `siren_seconds`, `veto_scope`,
`action_cooldown_s`, `repeat_window_s`, `max_actions_per_hour`,
`expected_guests`, `loiter_checks`, `staging`, `encounter_timeout_s`,
`heartbeat_s`, `consumer_durable`, and the greeting keys
(`greet_enabled`, `greet_known`, `greet_texts`, `greet_known_text`,
`greet_listen_seconds`, `greet_reply_texts`, `greet_repair_text`).
VLM/LLM assessment lives behind `[guard.assess] enabled`, `mode`, `vlm_url`,
`llm_url`, `timeout_ms` and `max_tool_rounds`.

## Reliability

Every observation is a staged saga. `guard_observation_inbox` stores the stage
and a JSON checkpoint; each stage (incident, encounter, dialogue, assessment,
resolve, effects, evidence) persists with idempotent keys before the next one
runs, so a redelivery resumes instead of repeating. Incidents, assessments,
actions and outbox intents are keyed by `event_id` or a deterministic
`command_id` and written with `INSERT OR IGNORE`; `performEffect` plans the
intent first and replays a `sent` intent without calling the camera again.
On the second-to-last broker delivery (`delivered >= max_observation_attempts
- 1`) the observation is parked in `guard_dead_letter`, the inbox row becomes
`dead_lettered` and the last broker attempt stays available in case the
process dies while writing the dead letter; the JetStream max-deliveries
advisory is logged for the crash-before-claim case. `guard_action_outbox.response` stores the full effect
result so a replayed `sent` intent reconstructs the listen transcript and
`speechDetected` without touching the camera. The incident phase (incident,
one-time guest consumption, checkpoint) commits in one SQLite transaction.

## Action safety

`arm_siren` defaults to false. Siren arming is a camera-side lease
(`lease_seconds`): the camera disarms on expiry even if guard never sends the
disarm command, and a startup/sweeper reconciliation covers restarts. The
sweeper never deletes a lease while the camera row or its driver is
unreachable; it keeps it and retries. Camera effect RPCs require both the
fleet secret and the `x-argus-service: argus-guard` identity.

## Dialogue

The encounter owns the dialogue state (`dialogue_turns`, `dialogue_goal`,
`listening_until`, `last_line`, `last_heard`, `last_heard_at`) as a goal
machine: `verify_identity` (challenge) -> `await_reply` (listen window) ->
`await_help_reply` (the grounded offer plus a fresh listening window) or
`repair_misheard` -> `done`. Greetings rotate without repeating the last
line, listening is skipped while a reply window is open, turns are capped by
`max_dialogue_turns`, repair speech is deferred until after the assessment,
and every spoken line (configured or generated) passes the code-side gate in
`guard-dialogue` before it reaches the camera.

## Test surface

`GET /health` (unfiltered) and the owner-only administrative API:
`GET|POST /guard/mode`, `GET /guard/incidents?limit=N`,
`GET /guard/decisions?limit=N` (read-only decision journal),
`GET|POST /guard/expected-guests`, `DELETE /guard/expected-guests?id=N` and
`POST /guard/person/{id}/promote` (forwards the owner bearer token and device
fingerprint to argus-identity).
Every `/guard` route runs the full `DeviceFilter → ValidJsonFilter → JwtFilter
→ RoleFilter` chain; `/guard` is outside the sync table map, so the role checks
admit Owner and deny every other role. The gateway proxies `/guard` to this
listener (`[guard] proxy_url`). Expected guests accept `cameraId`, `personId`,
`hostUserId`, `oneTime` and an explicit window; one-time windows are consumed
atomically on first match.

## Build

```bash
./scripts/build-all.sh dev --only argus-guard
```

## Belief gate and decision journal (Round 6)

Severity (how bad this is if true, code-owned, the model never lowers it)
and belief (how confident the perception is true) are separate quantities.
The deterministic danger matrix still assigns severity; `guard-belief.cc`
scores belief from a closed observable vocabulary (detector median strength,
persistence, track stability/jitter, tri-state identity, fresh camera health),
each signal a bounded integer weight, summed and compared against asymmetric
per-severity thresholds (critical 1 < high 3 < medium 5 < low 7). Every weight
and threshold is a `[guard.belief]` key with per-camera overrides under
`[guard.belief.camera."<id>"]`. The engine is pure (no database, no NATS, no
model) and the camera publishes the history it needs (score median/samples,
zone/track windows, area spread) rather than guard faking it.

`guard.decision_mode` governs the belief gate only: `shadow` (default)
journals the belief verdict without enforcing it, while per-encounter
notification threading applies in both modes; `enforce` lets the belief gate
suppress effects below threshold within the configured `gate_scope`
(`notify` default, `communication` for notify+announce, `all` for every
effect). Alarm and siren-arm additionally require no hard floor, so a hard
floor always lets physical effects through regardless of scope. Every
suppressed effect kind gets its own `guard_action` row and lands in the
journal's `suppressed_kinds` array, so suppressed physical effects stay as
visible as suppressed notifications. Every observation reaching the effects
stage writes one
`guard_decision_journal` row (idempotent by `event_id`, failure logged never
thrown): severity and rank, hard floor, belief score with applied signal
names, threshold, the legacy and belief verdicts side by side, whether it
notified, the effective mode and the suppression reason. Encounter closes
journal their outcome the same way. `GET /guard/decisions?limit=N` reads the
journal through the owner-only API.

## Quiet log and calibration collection (Round 11)

Round 11 closed the journal gaps without changing a single live verdict.
Early-watch staging rows (previously the one suppression with no record)
journal with reason `staging`; per-row `summary` text renders signal names
in the notification body's phrasing; `near_miss_margin` on the list and
summary endpoints surfaces below-threshold rows for retrospective review.
`POST /guard/decisions/{eventId}/feedback` stores resident labels that never
retune anything live. `noveltyScore` (per-camera hour-of-week EMA),
`repeatVisits` (unknown-signature clusters reusing the cross-camera
machinery), `quiet_hold`/`budget_hold` (`[guard.quiet_hours]`, default off,
markers only) and `assessMs` are journaled calibration inputs, never
decision inputs. Sustained tamper (`moved`/`covered`/`blurred` past
`tamper_sustained_s`) notifies live once per episode as `camera_tamper`;
there is no live danger floor from camera health (Round 13).

## Tamper design (Round 12)

Round 11 shipped two live bugs, both fixed here. First, persistence and
trustworthiness shared one never-refreshed timestamp, so `camera_tamper`
could only fire at exactly elapsed == 300 s. Now the camera monitor
republishes a steady state every tick (heartbeat, no new key) and guard
tracks `firstSeenMs` (state-entry anchor) against `tamperSustainedS` and
`lastSeenMs` (latest sample) against `healthStaleS` — two questions, two
timestamps. The notify cooldown lives in `guard_state`
(`tamper_notified_<cameraId>`), surviving restarts (superseded in Round 13
by onset-based episode keys with one notification per episode); after a restart the
window re-arms from the first post-restart sample, because guard cannot
tell pre-restart persistence from a fresh state and the persisted cooldown
already prevents re-spam. Second, plain `dark` counted as tamper, so every
no-IR exterior camera floored nightly detections to High. Now only
`moved` (re-aimed), `covered` (dark AND featureless — a working night
camera still resolves sensor texture) and `blurred` (sustained defocus)
qualify. `dark`
alone, `bright` and `unreachable` never do: darkness is a normal night
condition with zero interference information, which answers the
"normal at 03:00" question without any schedule. The notification body
and the journal summary render through the single shared
`beliefSignalPhrase`; identity phrases stay out of the body parens at the
call site because the body prefix already states identity.

## Tamper episodes (Round 13)

The hourly re-notify cooldown is replaced by one notification per episode:
onset, last-seen and notified-onset live in `guard_state`, so restarts
neither reset accrual nor re-page. Recovery clears the episode only after
fresh healthy readings span the sustained window, then a new degradation
starts a new episode. The live danger floor from camera health is removed
entirely — poor image quality is distrusted through the belief penalty,
never by raising unrelated traffic.

## Notification threads (Round 6)

One encounter owns one notification thread, persisted additively on the
encounter (`notify_command_id`, `notify_count`, `notify_highest_rank`): the
first crossing notifies, then only a strictly higher severity tier notifies
again. Same-tier repeats journal `thread_suppressed` and skip only the notify
effect so replays still complete announcements and alarms; effect command ids
are fixed per kind (notify 1, announce 2, alarm 3, siren 4) so a skip never
renumbers a resume. The journal flip and the thread slot commit in one
transaction: either both are durable or neither is, and a retry heals a
previously diverged slot by recording it when the flip no longer fires.
Bodies are deterministic from persisted
observables (identity phrase, zone, dwell seconds, up to three applied belief
reasons), never model-written: "Unrecognized person in the alert zone for
18s (strong detection, lingering, steady track)".

## Round 7 notes

- An absent `identityState` key means identity was unavailable (matcher never
  ran), scored as its own zero-weight signal — never as unrecognized. Only an
  explicit key earns the +2/-2/-3 identity signals.
- Belief configs resolve once per camera per `belief_refresh_s` (default
  300 s) into a TTL cache keyed by camera id; the `BeliefConfig` member
  initializers are the single source of defaults.
- `guard_decision_journal` also constrains `decision_mode` and `severity` by
  CHECK; pre-existing tables gain both plus `suppressed_kinds` through the
  additive rebuild migration.

## Live tests (opt-in, never green by default)

The `*-live-test` suites skip silently without their variables; a default
run passing means nothing about the wire:

- `ARGUS_NATS_URL` (e.g. `nats://127.0.0.1:4222`) arms
  `guard-dlq-live-test` and `guard-dlq-service-live-test` against isolated
  streams, subjects, durable names and temporary databases. Never point them
  at deployment streams.
- `ARGUS_VLM_TEST_URL` plus `ARGUS_VLM_TEST_IMAGE` arm
  `vlm-client-live-test` and `guard-assessment-live-test` against a real VLM
  listener and a real image file.
