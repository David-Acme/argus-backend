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
  (unknown while away/armed, alert zone, night, escalation, repeat visits of
  one person, two strangers in one event) plus a severity raise; expected
  guests and companions only lower soft cases - a resident companion to Low,
  any other known companion to Medium - and never below a floor.
- Every person in an event is counted, not only the primary track: a known
  companion no longer hides a stranger, and the stranger is the event's
  subject. A *stranger* is a person the camera scanned and reported
  `Unrecognized`; a person reported `Unobservable`, or carried with no
  identity verdict at all (camera identity off), is unknown but not a
  stranger, so two residents walking away from the camera do not reach the
  two-strangers floor. An incident is `known` only when every person in it is.
  The camera reports a scanned crop in which identity found no face as
  `Unobservable` (`IdentifyPersonResponse.face_found`), so a person seen
  from behind is never a stranger.
- `repeatVisits` (the unknown-signature counter) stays a calibration input
  and is not a floor: it counts observations, not visits, and grows for as
  long as the signature lives.
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
- Publishes `argus.guard.v1.heartbeat` (argus-notification's raw camera
  notifier yields while fresh) and `argus.guard.v1.encounter_closed` with a
  finalized redacted summary; long-term memory reads only that summary.
- Uploads incident evidence with a retention manifest (`guard_evidence`) and a
  daily deletion worker covering SQLite and the private object store.

## Posture, deterrence and weapons (2026-10)

The deterministic matrix answers two questions, and they are separate:
how bad is it (`guard_policy::evaluate`, the danger) and what may the
camera do about it out loud (`guard_policy::deterrence`). Notifying never
depends on who is present; voice and siren always do.

**Posture.** `[guard.schedule]` (off by default) derives the effective
mode from the local time, the manual mode stored by `POST /guard/mode`
and the property's hours; `guard_schedule::resolve` is the one place that
does it, and `GET /guard/mode` reports both (`mode` stays the manual one,
`effectiveMode`, `occupancy`, `publicPresent` and `staffOnly` are added).
A manual `armed` always wins. Inside `open` windows (a restaurant or shop
serving) the public is present; inside `staffed` windows only staff is;
outside both, a commercial schedule is closed and takes `closed_mode`
(`away` or `armed`). A home schedule with only `asleep` turns a manual
`home` into `night` while the residents sleep. Window syntax:
`"tue-sun 12:00-16:00, 20:00-24:00"`, days optional, a range may cross
midnight.

**Danger by posture.**
- Public present: an unknown person is a customer, so `Low` (journaled,
  not notified); an unknown in an alert zone (cash office, back door,
  kitchen) is `Medium`. No stranger, night or mode floor applies.
- Staff only: the alert zone floor is `High`, not `Critical`, and the
  night flag is ignored (prep and cleaning run at night).
- `night` mode: any unknown is at least `High`, inside a zone too. The
  camera's `night` flag on the event feeds the same night floor in the
  other modes, so a monitor-zone intruder at 03:00 is no longer daytime.
- An expected guest caps the result at `Medium` (one notification) in
  every mode. A weapon is the exception, below.

**Weapons.** The assessment's tags are a closed vocabulary (the grammar
enumerates them) and `weapon` (also `knife`, `gun`, `firearm`,
`weapon_like`) is new. Corroborated by the model's own threat of at least
`medium`, a weapon makes the event `Critical` above any floor and past an
expected guest: a guest who carries a knife is not a guest. `carrying_box`
no longer raises anything; a delivery is not a threat.

**Deterrence ladder.** Voice needs `High`; the siren needs `Critical`.
- Public present or staff only: silent always - never talk to or sound a
  siren at customers or staff.
- A weapon while people are present (`home`, `night`, open, staffed):
  silent. A siren in front of an armed person escalates the risk to the
  people inside; the owner is notified immediately instead.
- `home`: voice, never the siren (the residents are there).
- `night`: voice; the siren at `Critical` (an entry or a weapon outside a
  sleeping house).
- `away`: voice first; the siren only when the visit persists (second
  check of the encounter), in an alert zone, or with a weapon.
- `armed`: the siren at `Critical`, immediately.
When the siren runs it runs before the voice line, and the default line is
a deterrent ("Atención: está en una propiedad privada. El propietario ya
ha sido avisado."), not the greeting.

The research behind this (alarm fatigue, TMA AVS-01 levels, OSHA's silent
robbery guidance, the EDPB position on face recognition of customers) is
in the 2026-10 audit report; the per-profile defaults it suggests are the
starting point for `[guard.schedule]`.

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

The closed-encounter feed is a durable outbox on its own stream. The row is
enqueued inside the encounter-close transaction and published by
`flushEncounterOutbox`, which drains up to 100 pending rows in one bounded
statement and marks a row `sent` only after the JetStream PubAck — so a row the
broker stored but the database could not mark is republished under the same
event id, which the stream's duplicate window and the consumer's idempotent keys
absorb. The mark is an assertion rather than a repair: nothing deletes an outbox
row, so `markEncounterSent` can only fail for a row that is not there, and a
write error arrives as a thrown exception, which both call sites catch so the
rows left in the pass stay pending. Guard owns `ARGUS_GUARD`
(`argus.guard.v1.>`, 7-day retention, 2-minute duplicate window) so a closed
encounter survives a restart for a durable consumer, and the drain re-runs that
ensure on the pass after a refused publish, which is what lets the feed heal a
stream deleted broker-side instead of latching at boot. A stream that exists but
declares other subjects is refused by the reconcile on purpose, so it stays
broken until an operator reconciles it. Guard's drain has no worker and no
progress cadence — it runs from the close path and the encounter sweep — so its
own refusal line counts the rows of that pass, on top of the one `NatsBus` emits
per refused publish, and unlike the sibling sinks it never breaks the pass: a
missing stream costs every pending row at once rather than stalling the ones
behind a head row.

Guard registers itself with `shutdown_signal` before `drogon::app().run()` —
so the hook's handlers are what `run()` installs Drogon's own `sigaction`
over — and
its drain is the whole service rather than a worker of its own: `drained()`
means no coroutine of the service is suspended mid-await, which is what lets a
SIGTERM finish the guard work already in flight — an observation being
processed, an encounter sweep — before Drogon's `quit()` destroys the database
client manager those coroutines reach. `requestStop()` clears the service's
own `alive` flag, stops the timers and stops the retry pump (an atomic flag,
because the stop arrives on the signal path while the pump's tick runs on the
loop); an observation
that arrives after it is dropped by the queue rather than claimed, so a stop
never leaves a half-processed observation behind. A run that never reports
drained is bounded by the hook's 10-second deadline, which quits anyway and
names the drain in the log.

## Action safety

The per-camera hourly cap, the per-encounter hourly cap and the per-person
cooldown count only effects that left: `succeeded`, `duplicate_succeeded`,
`in_flight`, `pending` and `indeterminate`. Rows written for an effect that
never ran (`rejected`, `denied`, `budget_denied`, `belief_suppressed`,
`thread_suppressed`) used to count too, so one Critical event (notify,
announce, alarm and a refused siren_arm) filled the camera's four effects an
hour, and a sustained tamper retried every sweep and kept adding rows, so a
real intrusion later that hour was capped.

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
`GET /guard/decisions/summary?from=&to=&nearMissMargin=`,
`POST /guard/decisions/{eventId}/feedback`,
`GET|POST /guard/expected-guests`, `DELETE /guard/expected-guests?id=N` and
`POST /guard/person/{id}/promote` (forwards the owner bearer token and device
fingerprint to argus-identity).
Every `/guard` route runs the full `DeviceFilter → ValidJsonFilter → JwtFilter
→ RoleFilter` chain; `/guard` is outside the sync table map, so the role checks
admit Owner and deny every other role. The listener terminates TLS with the
instance certificate and announces one `_argus-route._tcp` instance per logical
route; the app reaches it directly, with no proxy in front (Phase 3d step 1c
removed the gateway that used to relay `/guard`). Expected guests accept
`cameraId`, `personId`, `hostUserId`, `oneTime` and an explicit window;
one-time windows are consumed atomically on first match.

## Build

```bash
./scripts/build-all.sh dev --only guard
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
  `guard-dlq-live-test`, `guard-dlq-service-live-test` and
  `guard-encounter-drain-live-test` against temporary databases and, for the
  path each one drives, an isolated stream, subject and durable name;
  `guard-dlq-service-live-test` still ensures the production `ARGUS_GUARD` on
  start, as it always has. Never point them at deployment streams. The drain
  suite names its stream and its subject family after the process and its start,
  because a stream whose subjects another stream already claims is refused
  rather than created — the production family, which a broker that has run guard
  already carries, could not show that the drain creates its own stream. The
  stream it creates is never reclaimed: `maxAge` bounds the messages, not the
  stream, and `NatsBus` has no stream-delete, so each run leaves one behind.
- `ARGUS_VLM_TEST_URL` plus `ARGUS_VLM_TEST_IMAGE` arm
  `vlm-client-live-test` and `guard-assessment-live-test` against a real VLM
  listener and a real image file.

## Phase 4 step 9: `src/config/` and the shared vocabulary (D20)

The block that filled `GuardService::Config` field by field in `main.cc`, and
`guard_belief`'s belief weights with it, is `src/config/guard-config.{hxx,cc}`
(`argus::guard-config`), nine resolvers wide: `resolveDb`, `resolveListener`,
`resolveNotifications`, `resolveIdentity`, `resolveActions`,
`resolveAssessEndpoints`, `resolveAssessment`, `resolveService` and
`resolveBelief(cameraId)`. Three types a feature and the config module both
name moved to `src/shared/vocabulary/`: `guard-mode.hxx` (from
`feature/guard/vocabulary/`, the 2+ rule now that two units read it),
`belief-gate-scope.hxx` and `belief-config.hxx` (extracted from
`guard-belief.hxx` byte for byte). Every public spelling survives —
`GuardService::Config` and `GuardAssessment::Config` are in-class aliases of
the config module's structs, and `BeliefConfig`/`BeliefGateScope` keep their
global names — so no consumer of a moved type changed an expression. The step
also gave `main.cc` the two lines every other service already had,
`setExceptionHandler(ErrorHandler::handleException)` and
`setCustomErrorHandler(ErrorHandler::unmatchedRoute)`: a refusal thrown in a
guard handler is the shared envelope now, and guard was the one service
missing that registration. Its hand-rolled `/health` stays — the deviation is
recorded in `packages/lib/http/AGENTS.md`, and no compose healthcheck polls
guard.

The service config and the belief config did not share a guard, and the merge
had to keep both. `main.cc`'s `configIntOr` was positivity-based
(`value > 0 ? value : fallback`), because every key it read is a count or a
window that only makes sense positive; `guard-belief.cc` read its weights with
a presence test (`hasKey`), because a belief weight is negative by design —
`weight_identity_known` ships as `-3`. One module now carries both policies:
`configIntOr`/`configInt64Or`/`configDoubleOr`/`configBoolOr` serve the
service resolvers, and `beliefIntOr`/`beliefInt64Or`/`beliefDoubleOr` serve
`resolveBelief` alone, so a weight set to a negative number or to `0` is
honoured rather than silently replaced by the struct default. The distinction
is pinned by a case in `tests/unit/guard-belief-test.cc` ("belief keys are
honoured whatever their sign"), which sets a negative per-camera weight, a
zero weight and a negative global weight.
