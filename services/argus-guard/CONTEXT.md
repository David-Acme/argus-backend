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
