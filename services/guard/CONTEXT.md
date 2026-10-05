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
  The queue is serialized per camera, not globally: one camera's greeting,
  listen and reply (25-35 s) used to hold every other camera's intrusion
  behind it. Observations of one camera stay in order; cameras run
  concurrently on the loop, and a cross-camera encounter is still matched
  through the encounter table.
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

**Posture.** Each environment's schedule (off by default) derives its
effective mode from the local time, the environment's manual mode (set by
`POST /guard/mode`) and its hours; `guard_schedule::resolve` is the one
place that does it, and `GET /guard/environments` reports both per
environment (`mode` stays the manual one, `effectiveMode`, `occupancy`,
`publicPresent` and `staffOnly` are added; see "Environments" below).
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

**Fast lane for hard floors.** A hard floor (`High` or above from the
policy) runs its effects before the assessment: the assessment can never
lower a floor, so waiting 5-10 s (up to ~50 s worst case) for the VLM and
the LLM only delayed the notification and the siren. The assessment runs
after the effects (the late assessment, `lateAssessed` in the checkpoint)
for the record and to escalate: when it raises the danger (a corroborated
weapon makes it `Critical`), `escalate` sends the tier-increase
notification and whatever the deterrence ladder now allows, under its own
correlation (`<eventId>:escalation`) so the outbox never mistakes it for
the first pass's intents. Soft cases are still assessed before any effect,
because there the assessment may veto or raise.

**Cooldown, staging and continuity.**
- The encounter cooldown is stamped only when an effect actually left
  (accepted or pending), not when the budget check passed: an event whose
  every effect was suppressed used to start a 120 s window that silently
  swallowed the next, higher-tier event. A strict tier increase over the
  encounter's highest notified rank skips the cooldown (never the hourly
  cap), so `Medium` → `Critical` inside two minutes still acts.
- Staging promotes by one tier when a visit lingers (`Low` → `Medium`,
  `Medium` → `High`) and never promotes a visit the assessment vetoed: a
  courier who waited was being announced at as an intruder.
- `continuity_window_s` defaults to 45 s: the camera re-emits a live track
  about every 30-35 s, so a 20 s window split one visit into several
  encounters, re-greeted the visitor and reset the staging count.

**Retention.** The daily sweep that already purged the decision journal
after `journal_retention_days` (90) now purges, in one transaction, the
rest of guard's history past the same window: incidents (with their
`event_json`), assessments, actions, settled action intents, closed
encounters with their transitions, dead letters, signature visits and
evidence rows whose object is already deleted. Settled inbox rows go after
the stream's retention (the dedupe horizon), because a redelivery older
than that cannot arrive. Rows still in flight (`processing` inbox rows,
`pending`/`in_flight`/`retryable_failed` intents, open encounters) are
never touched. Before this every observation left an inbox row with its
full payload forever - up to thousands a day per camera.

The research behind this (alarm fatigue, TMA AVS-01 levels, OSHA's silent
robbery guidance, the EDPB position on face recognition of customers) is
in the 2026-10 audit report; the per-profile defaults it suggests are the
starting point for `[guard.schedule]`. They stay a starting point the owner
writes, not windows the service assumes from `profile`: an assumed `open`
window marks every unknown as a customer, and a restaurant that closed early
or a holiday would be guarded as if it were serving. Only the hours the
owner states lower vigilance; `profile` itself only frames the assessment
prompt.

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

`GET /health` (unfiltered) and the administrative API:
`POST /guard/mode` (`{mode, environmentId?}`; no id means every
environment), `GET|POST /guard/environments`,
`PATCH|DELETE /guard/environments/{id}`, `GET /guard/incidents?limit=N`,
`GET /guard/decisions?limit=N` (read-only decision journal),
`GET /guard/decisions/summary?from=&to=&nearMissMargin=`,
`POST /guard/decisions/{eventId}/feedback`,
`GET|POST /guard/expected-guests`, `DELETE /guard/expected-guests?id=N`,
`POST /guard/person/{id}/promote` (forwards the owner bearer token and device
fingerprint to argus-identity), `GET /guard/cameras`,
`PUT /guard/cameras/{id}`, `GET /guard/episodes?limit=&before=&environmentId=`,
`GET /guard/episodes/{id}` and `POST /guard/episodes/{id}/review` (see "Site,
camera context and episodes" below).
Every `/guard` route runs the full `DeviceFilter → ValidJsonFilter → JwtFilter
→ RoleFilter` chain; `/guard` is outside the sync table map, so the role checks
admit Owner and deny every other role. The listener terminates TLS with the
instance certificate and announces one `_argus-route._tcp` instance per logical
route; the app reaches it directly, with no proxy in front (Phase 3d step 1c
removed the gateway that used to relay `/guard`). Expected guests accept
`cameraId`, `personId`, `hostUserId`, `oneTime` and an explicit window;
one-time windows are consumed atomically on first match. The lookup takes
every active pass for the camera and the person into account — an open pass
(`personId` 0) or the visitor's own — and prefers the visitor's own. It used to
read only the newest active pass and then compare the person, so a newer pass
bound to someone else hid an older open one.

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
machinery), `quiet_hold`/`budget_hold` (`[guard.quiet_hours]`, default off;
since 2026-10 they hold the alert when on, see below) and `assessMs` are
journaled calibration inputs. Sustained tamper (`moved`/`covered`/`blurred` past
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

## Offline cameras and critical tamper (2026-10, WATCHDOG)

A camera that is unplugged, loses its video or its address used to be a
badge in the app and nothing more: the monitor's `unreachable` never counted
as tamper (Round 12), so an intruder who pulled a camera's cable was never
told. The sweep now treats a sustained `unreachable` like the other tamper
states, with its own window, `guard.offline_sustained_s` (240 s, the
monitor samples every 60 s, so four consecutive misses): a Wi-Fi blip, a
router reboot or a camera restart heals before it and says nothing, the
episode keys, recovery and "one notification per episode" are the Round 13
ones. Only explicit `unreachable` samples count: a camera that is disabled,
put in privacy mode by Argus or deleted is not sampled, its feed goes stale
and stale readings are skipped, so none of those alarms. The body says what
is likely and what it costs ("Argus no recibe imagen desde hace 4 min: puede
estar desenchufada, sin red o apagada. Esa zona no está vigilada.").

Severity follows the place, not the camera: when the environment's
effective mode is `away`, `armed` or `night` (nobody is expected to be
watching, or everyone sleeps), a covered, moved, blurred or offline camera
is `critical` (incident danger, journal, `urgency: critical`, plus "La
vigilancia está activa: revísalo cuanto antes."), which the call engine
rings through RESPONSE's plan (`guard_tamper` + `critical` is a
`guard_critical` candidate); at home it stays `high` / `active`, a
notification, never a call. A burglar covering or unplugging a camera on an
empty house is the classic move this catches; the same at 15:00 with the
family home is most likely a child or a cleaner.

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

## Owner settings

`src/feature/settings/guard-settings.cc` (`argus::guard-settings`) is the
catalog an owner may change through `argus.settings.v1.Settings`. Guard had no
gRPC server before; it now opens one only for this, on `[rpc] address`
(`127.0.0.1:7139` in the native template, `0.0.0.0:7139` in the deploy one;
empty means no listener), and registers `SettingsRpcService{.service = "guard"}` alone
on it. The only credential it accepts is `[rpc.callers] settings` (service
name `settings`, resolved by `GuardConfig::resolveRpc()` through
`settingsCallers`); any other `[rpc.callers]` entry is ignored, and an address
with an empty settings secret starts nothing and logs why.
`ensure_settings_owners` wires the slot as `guard rpc.callers settings rpc`,
like llm/tts/stt/vlm, so argus-settings' `[owners.guard]` target is
`argus-guard:7139` in compose and `127.0.0.1:7139` natively. A native config
written before the template had an address gets it filled from the template
by `ensure_settings_owners` (setup.sh and the native-stack sandbox), so the
owner page lists guard without a hand edit.

| Key | Level | Applies | Group | Range | Fallback |
|---|---|---|---|---|---|
| `guard.notify_level` | basic | live | response | 1-4 | 2 |
| `guard.announce_level` | basic | live | response | 1-5 | 3 |
| `guard.alarm_level` | basic | live | response | 1-5 | 4 |
| `guard.alarm_seconds` | basic | live | alarm | 1-60 | 6 |
| `guard.arm_siren` | basic | live | alarm | toggle | false |
| `guard.siren_seconds` | basic | live | alarm | 1-300 | 20 |
| `guard.greet_enabled` | basic | live | visitors | toggle | true |
| `guard.greet_known` | basic | live | visitors | toggle | false |
| `guard.expected_guests` | basic | live | visitors | toggle | true |
| `guard.quiet_hours.enabled` | basic | live | quiet | toggle | false |
| `guard.quiet_hours.start_hour` | basic | live | quiet | 0-23 | 22 |
| `guard.quiet_hours.end_hour` | basic | live | quiet | 0-23 | 7 |
| `guard.quiet_hours.daily_budget` | advanced | live | quiet | 1-500 | 30 |
| `guard.decision_mode` | advanced | live | decisions | shadow, enforce | shadow |
| `guard.belief.gate_scope` | advanced | live | decisions | notify, communication, all | notify |
| `guard.belief.threshold_critical` | advanced | live | decisions | -12-12 | 1 |
| `guard.belief.threshold_high` | advanced | live | decisions | -12-12 | 3 |
| `guard.belief.threshold_medium` | advanced | live | decisions | -12-12 | 5 |
| `guard.belief.threshold_low` | advanced | live | decisions | -12-12 | 7 |
| `guard.belief.detector_strong` | advanced | live | decisions | 0.3-0.99 | 0.75 |
| `guard.belief.detector_weak` | advanced | live | decisions | 0.05-0.9 | 0.35 |
| `guard.belief.zone_dwell_alert_ms` | advanced | live | decisions | 500-120000 | 3000 |
| `guard.belief.zone_dwell_monitor_ms` | advanced | live | decisions | 500-300000 | 12000 |
| `guard.belief_refresh_s` | advanced | live | decisions | 10-3600 | 300 |
| `guard.action_cooldown_s` | advanced | live | limits | 1-86400 | 120 |
| `guard.repeat_window_s` | advanced | live | limits | 60-604800 | 86400 |
| `guard.regroup_window_s` | advanced | live | limits | 0-86400 | 600 |
| `guard.max_actions_per_hour` | advanced | live | limits | 1-60 | 4 |
| `guard.max_dialogue_turns` | advanced | live | dialogue | 1-10 | 3 |
| `guard.greet_listen_seconds` | advanced | live | dialogue | 1-10 | 6 |
| `guard.greet_reply_enabled` | advanced | live | dialogue | toggle | true |
| `guard.cross_camera_window_s` | advanced | live | tracking | 5-600 | 60 |
| `guard.continuity_window_s` | advanced | live | tracking | 5-600 | 45 |
| `guard.signature_min_similarity` | advanced | live | tracking | 0.5-0.99 | 0.82 |
| `guard.loiter_checks` | advanced | live | tracking | 1-20 | 3 |
| `guard.staging` | advanced | live | tracking | toggle | true |
| `guard.encounter_timeout_s` | advanced | live | tracking | 30-3600 | 300 |
| `guard.tamper_sustained_s` | advanced | live | health | 30-3600 | 300 |
| `guard.offline_sustained_s` | advanced | live | health | 60-3600 | 240 |
| `guard.health_stale_s` | advanced | live | health | 30-3600 | 300 |
| `guard.journal_retention_days` | advanced | live | history | 1-3650 | 90 |

The levels are danger ranks (1 low, 2 medium, 3 high, 4 critical): a response
fires when the rank reaches its level. Announce and alarm accept 5, which no
rank reaches (never); notify stops at 4 so a critical danger always notifies.
Every minimum of a service key is 1 or more because `configIntOr` reads `0`
as "absent" and would run the fallback instead of the owner's value; the
belief thresholds use the presence-based belief readers, so they span the
belief score's own range, negatives included, and `regroup_window_s` reads
with a presence test so its `0` (grouping off) is honoured.

### How a change goes live

`GuardService` no longer holds a plain `Config` member. It holds a
`std::shared_ptr<const Config>` behind `configMutex_`; `currentConfig()`
copies the pointer under that lock, and every member function that read
`config_` takes one snapshot at its top (the encounter sweep takes it inside
its timer coroutine, per tick). Readers run on Drogon's I/O loops, the NATS
callback threads and the coroutines those spawn, with no common lock, so a
snapshot is the only race-free shape; a running observation also keeps one
coherent config from its first stage to its last instead of mixing values
across a change. The action authorizer is built from the same snapshot per
effect, so a level or `arm_siren` change reaches the next authorization.

The registry's `onChange` in `src/app/main.cc` calls
`guardService.refresh(GuardConfig::resolveService())`. `refresh` copies the
current snapshot, overwrites exactly the catalog's fields from the fresh
resolution, swaps the pointer under the lock, and then clears the
per-camera belief cache under `beliefMutex_`. Clearing the cache is what
makes the `[guard.belief]` keys live rather than "within
`belief_refresh_s`": the next observation re-resolves its camera's belief
config. A per-camera override (`[guard.belief.camera."<id>"]`) still wins over
the global key the catalog edits, exactly as it does at boot.

Everything outside the catalog is boot-bound even when a refresh runs: the
enabled flag, profile, default mode and schedule (the controller and
`GuardSchedule` copy them once), the NATS stream, subject and durable names,
the heartbeat interval, `max_observation_attempts` (the consumer's
`maxDeliver`), the retry backoff and lease, and the spoken texts, languages,
`max_announce_words` and `veto_scope` (the last two are also baked into
`GuardAssessment`'s own config). A hand edit of those keys followed by an
owner change does not leak them into the running service; they apply on
restart. The texts are left out of the catalog on purpose: they are
multi-variant, language-bound strings better edited with the templates.

`guard.enabled`, `guard.default_mode`, `guard.profile` and the schedule never
appear (the mode is the owner's HTTP `PATCH`), nor do targets, URLs,
credentials, database paths, consumer names or NATS subjects.

`guard.quiet_hours.start_hour`/`end_hour` used to go through
`configIntOr`, so an explicit `0` (midnight) silently became the fallback
(22 / 7). They now read with a presence test, so `0` is midnight and an
absent key is still 22 / 7.

### Shutdown

The settings server is a `shutdown_signal` drain registered before the guard
drain: the stop request shuts it down with a 500 ms deadline (no new owner
change can reach a stopping service), and `main` shuts it down again after
`run()` returns, which is a no-op when the drain already ran. Both happen
before `GuardService` is destroyed, so `onChange` never refreshes a dead
service.

### Tests

`guard-settings-test` builds the catalog through `SettingsRegistry`, rejects
plumbing fragments, pins every fallback against `resolveService()` and
`resolveBelief()` with an empty config, checks the midnight quiet hour, drives
a registry update into a constructed `GuardService` (null peers, disabled, no
NATS or camera) and checks the new snapshot, the untouched old snapshot and a
boot-bound field that a hand edit could not move, and runs a live gRPC server
where only the settings secret can List or Update.

## Site, camera context and episodes (2026-10)

The owner asked for a guard that does not flood the phone the way
"motion detected" products do, that understands a home, an office and a
restaurant differently, and that knows what each camera looks at. Four
pieces answer it, all additive to guard.db and all on the owner-only API.

**Principles adopted** (from the research recorded in the 2026-10 guard
report): an alert must be actionable, everything else is an indication
(ISA-18.2 / EEMUA 191 alarm rationalisation; their benchmark is about one
alarm per ten minutes at worst); alerts and routine detections are two
different products (Frigate's review items: *alerts* vs *detections*, one
item per activity, not per frame); a dedup key turns repeats of one
situation into updates of one incident (PagerDuty alert grouping); location
and schedule context belong to the camera (Nest activity zones, UniFi
Protect per-camera smart-detection zones and schedules, Ring motion
schedules); response rates fall to the perceived reliability of the alarm
(Bliss' cry-wolf studies), so precision and an honest "why" matter more
than recall of the trivial; urgency is a property of the message, so the
data carries an interruption level a push channel can map later (iOS
passive / active / time-sensitive / critical, Android channel importance).

**Site** (superseded by "Environments" below; `guard_site` is migrated
into the default environment and dropped). `guard_site` (one row, `id = 1`) held the profile (`home`,
`office`, `commercial`), the schedule (`schedule_enabled`, `asleep_hours`,
`open_hours`, `staffed_hours`, `closed_mode`) and `digest_hour`. Until the
owner edits it, guard reads `[guard] profile`, `[guard.schedule]` and
`guard.digest_hour` exactly as before (`guard_schedule::siteDefaults`);
`PATCH /guard/site` seeds the row from those values (`INSERT OR IGNORE`)
and then updates only the fields it carries, so two quick edits from an
optimistic UI cannot lose each other. The saga reads the row on every
observation (one primary-key read beside the existing `mode` read), so an
edit is live without a restart and without a cache to invalidate. The
profile still only frames the assessment prompt and chooses copy: the
service never assumes hours from it. The app offers per-profile presets,
but only hours the owner confirms lower vigilance — the 2026-10 posture
rule stands.

**Camera context.** `guard_camera_context` (one row per camera id) holds the
role (`entrance`, `perimeter`, `garage`, `living`, `kitchen`, `office`,
`register`, `storage`, `public_area`, `other`), `outdoor`, `public_area`
and optional `active_hours`. `guard_context::evaluate` (pure) turns it into
two facts the policy reads:
- *in use*: a work area (kitchen, office, register, storage) or a public
  area while the site is staffed or open, or the camera's own active hours
  while the effective mode is home (or a commercial site is closed and the
  mode is away). Never in night, armed or a manual away. An unknown person
  in an area in use is `Low`, `Medium` only inside an owner-drawn alert
  zone — the same treatment the public already had during open hours.
- *passer-by*: an outdoor public camera (street, shared path) outside any
  alert zone. A passer-by is `Low` even at night or away; three visits of the
  same identified person in the repeat window make it `Medium`. Lingering
  still promotes it one tier through staging, so someone who stops in front
  of the house is told; someone walking past is not.
Both make the deterrence ladder silent: Argus never talks to staff,
customers or the street. An unconfigured camera changes nothing.

Staging no longer promotes expected activity. Before this, a customer or a
waiter seen three times during open hours was promoted from `Low` to
`Medium` and notified once per encounter, which contradicted the posture
rule; now staging still holds the first checks but never promotes when the
public is present or the area is in use.

**Why, in words.** `guard_policy::explain` lists the reasons the policy
applied (`after_hours`, `nobody_home`, `armed`, `night`, `alert_zone`,
`several_strangers`, `repeat_visits`, `escalating`, and the lowering ones
`public_hours`, `staff_hours`, `area_in_use`, `passerby`, `expected_guest`,
`with_resident`, `with_guest`); the saga adds `weapon`, `lingering`,
`face_hidden` and `brief`. They are journaled per observation
(`guard_decision_journal.reasons`) and kept on the episode
(`guard_encounter.reasons`, the reasons of its highest-ranked observation).

**Notifications people read.** The deterministic English body
("Unrecognized person in the alert zone for 18s (strong detection, …)") is
replaced by `guard_copy::render`, a pure, table-driven es/en renderer of a
structured `GuardNotice`: who (`Persona desconocida`, `Alguien sin
identificar`, `3 personas desconocidas`, `… acompañada`), where (the camera
name in the title, the role and the owner's zone name in the body), why (at
most two raising reasons and the dwell) and what Argus is doing (speaker,
camera alarm, greeted with or without an answer, silent because of a
weapon, or watching). Example: "Persona desconocida · Jardín" / "En el
exterior, de noche, desde hace 18 s. Argus le está avisando por el
altavoz." A tier increase of the same episode reads as an update ("Sigue
en Jardín · riesgo crítico"). Tamper reads "Revisa la cámara …" with what is
wrong. Each recipient reads their own language: guard groups the notifiable
users by the `lang` of their identity record (`GetUser`, cached ten
minutes; `guard.notify_lang` when absent) and sends one `CreateNotifications`
per language, command id `<commandId>:<lang>` when there is more than one.
The batches, their titles and bodies are persisted in the action outbox
payload before the first send, so a replay sends the same words to the same
people (an old single-batch payload still replays). The belief phrases stay
in the journal summary for the owner's calibration view.

Notification `data` keeps every field it had and adds `cameraName`,
`episodeId`, `kind` (`guard_episode`, `guard_tamper`, `guard_digest`),
`phase` (`opened`, `escalated`, `daily`, `after_quiet`), `threadKey`
(`guard:episode:<id>`, the key a client or a future push channel collapses
on), `urgency` (`passive`, `active`, `time_sensitive`, `critical`),
`action`, `role`, `outdoor`, `subject`, `people`, `reasons` and `lang`.
There is deliberately no "resolved" notification: one per episode end would
double the volume.

**Episodes.** The encounter was already the stateful episode (observing →
verifying/escalating → closed, revisioned transitions, one notification
thread); it now also carries `subject`, `people`, `reasons`,
`reasons_rank`, `group_id`, `review_label` and `reviewed_at`.
`GET /guard/episodes` lists person episodes and camera-tamper incidents
newest first with state (`active`/`resolved`), resolution (`left`,
`recognized`, `recovered`), danger, whether and how often it notified,
whether Argus spoke or sounded the alarm, and the reasons.
`GET /guard/episodes/{id}` adds a condensed timeline (state changes,
decisions with identical consecutive verdicts folded into one entry with a
count, and every action including held and grouped notifications).
`POST /guard/episodes/{id}/review` stores the owner's label on the episode
and on every decision of it that notified, in one transaction — the same
calibration population the per-decision feedback feeds; nothing is retuned
live.

**Grouping, quiet hours and digests.**
- A medium-or-lower first alert of a new episode on a camera that already
  alerted at the same or a higher tier within `regroup_window_s` (600 s)
  joins that episode: journal `grouped`, action `grouped`, `group_id` set.
  The owner tunes the window live from Configuración; `0` turns grouping
  off. It is read with a presence test like the quiet hours: through
  `configInt64Or` an explicit `0` used to fall back to 600, so "0 disables"
  never worked.
  High and critical always alert on their own. The decision is persisted in
  the checkpoint, so a replay does not regroup differently.
- Quiet hours and the daily budget (`[guard.quiet_hours]`, still default
  off) now hold medium-or-lower alerts instead of only marking them:
  journal `held`, action `held` with `quiet_hours` or `daily_budget`. High
  and critical are never held. The hold is computed once and persisted in
  the checkpoint.
- The encounter sweep (every minute) sends at most two summaries a day:
  "Mientras descansabas" at `end_hour` when quiet hours are on (held alerts
  and routine activity of the night), and "Resumen de vigilancia" at the
  site's `digest_hour` (since the previous summary). Each is one passive
  notification through the same durable intent path (correlation
  `digest:quiet:<day>` / `digest:daily:<day>`), skipped when there is
  nothing to say, and recorded in `guard_state` only once it settled.

**Scenarios, measured** (`tests/unit/guard-scenario-test.cc`, default
config, fakes for the camera and the notification service):

| Scenario | Before (527bfa2f) | After |
|---|---|---|
| Home garden at night, one stranger, 4 observations | 1 alert "Jardín: person_night" / "Unrecognized person in the area for 18s (strong detection, lingering, steady track)", 1 spoken line | 1 alert "Persona desconocida · Jardín" / "En el exterior, de noche, desde hace 18 s. Argus le está avisando por el altavoz.", 1 spoken line |
| Restaurant kitchen while open, a cook seen 6 times | 1 alert "Cocina: person_day" / "Unidentified person in the area for 18s (…)" (staging promoted a customer-hours Low to Medium) | 0 alerts; counted in the daily summary |
| Office after hours, 3 observations | 1 alert "Oficina: person_day" / "Unrecognized person in the area for 18s (…)", 1 spoken line | 1 critical alert "Persona desconocida · Oficina" / "En la oficina, fuera de horario, desde hace 18 s. Argus le está avisando por el altavoz.", 1 spoken line |
| Street camera at night, 3 passers-by | 2 alerts "Calle: person_night" and 2 lines spoken to the street (the third was only stopped by the hourly cap) | 0 alerts, nothing said to the street |
| Entrance, medium visit inside quiet hours | 1 alert "Entrada: person_in_monitor_zone" | 0 alerts, held for the morning summary |

The before column is the same test file built against 527bfa2f (the parent
of the change) in a scratch worktree; the camera-context rows it inserts do
not exist there, so the old guard saw the same events with no context.

## Environments (2026-10, wave 2)

The owner's words: "Vigilancia" was one profile and one mode for the whole
system, but a camera lives in a place, and one install can watch a home, a
restaurant and an office at once. An environment is now the unit of
posture: guard judges every observation through the environment of its
camera.

**Research.** Serious products all separate the place from the system:
Ring and Google Home have *locations/homes*, each with its own mode
(Disarmed/Home/Away), its own mode schedule and its own devices; alarm
panels (Alarm.com, Qolsys IQ, DSC Neo) have *partitions* that arm
independently, with an "arm all" action across them (Alarm.com only offers
the multi-select when the partitions share a state); Verkada Alarms has
*sites/partitions* with their own arming schedules, smart schedules and
schedule exceptions, managed from one console; UniFi Protect's Alarm
Manager has arm profiles with schedules and a scope of cameras (and a
documented midnight gap in its 00:00-23:59 profiles, which our windows that
cross midnight avoid). Frigate and the camera-level products keep zones and
review policy per camera. The model below takes the location/partition shape
for posture (mode, hours, summaries) and keeps the camera-level context
(role, indoor/outdoor, public area, own hours) that 966ae0b0 introduced.

**Model.** `guard_environment` holds `name` (unique, case-insensitive),
`kind` (`home`, `office`, `commercial`, `restaurant`, `warehouse`,
`outdoor`), `is_default` (exactly one, enforced by a partial unique index),
the manual `mode` and `mode_updated_at`, the schedule (`schedule_enabled`,
`asleep_hours`, `open_hours`, `staffed_hours`, `closed_mode`), the summary
policy (`digest_hour`, `quiet_policy` = `inherit` | `custom` | `off`,
`quiet_start_hour`, `quiet_end_hour`). `guard_camera_context.environment_id`
places a camera; a camera without a row, or pointing at a removed
environment, belongs to the default. The kind frames the assessment prompt
and names the seeded default ("Casa", "Local", ...); like the old profile it
never assumes hours.

**Engine.** One query per observation (`EnvironmentRepository::forCamera`,
a primary-key join that also counts the environments) replaces the site and
`mode` reads. Per environment: posture and the in-use/passer-by evaluation,
the expected-guest lookup (a pass with `environment_id` 0 is valid
everywhere, as every pass was before), quiet hours (`inherit` follows the
owner's `guard.quiet_hours.*` settings, `custom` uses the environment's
hours, `off` never holds) and the daily budget (`firedSince` counts the
environment's own notified decisions). Regrouping, threads, staging and the
per-camera caps were already per camera. The decision journal and the
encounter carry `environment_id`, written when the decision is made, so
history and digests stay with the place where they happened even if a
camera moves later. Tamper episodes take the camera's current environment.

**Copy.** With one environment nothing changes. With several, every title
names the place: "Persona desconocida · Cocina (Trattoria)", "Sigue en
Jardín (Casa) · riesgo crítico", "Revisa la cámara Cocina (Trattoria)",
"Resumen de vigilancia · Trattoria". Notification `data` gains
`environmentId` and `environmentName` (empty with one environment).

**Summaries.** The sweep sends each environment its own "Mientras
descansabas" (at its quiet end) and "Resumen de vigilancia" (at its
`digest_hour`), counting only that environment's journal rows. The thread is
one per day and environment, `threadKey = guard:digest:<envId>:<YYYY-MM-DD>`,
shared by both summaries of that day. Correlations are
`digest:quiet:<envId>:<day>` / `digest:daily:<envId>:<day>`, state keys
`digest_*_day_<envId>`. A summary is not a camera effect: effects with
`cameraId` 0 no longer count against the per-camera hourly cap, which five
environments sharing a digest hour would otherwise have exhausted.

**Modes.** `POST /guard/mode {mode, environmentId?}` sets one environment,
or every environment when the id is absent (the voice tool and older clients
send no id). It answers the whole environment list, so an optimistic client
settles from it.

**API.** `GET /guard/environments` (Owner, Resident, Guard) lists every
environment with its config, live posture and the ids of the cameras placed
in it. `POST /guard/environments` (name and kind required), `PATCH
/guard/environments/{id}` (any field, PATCH semantics) and `DELETE
/guard/environments/{id}` are Owner-only; removing moves its cameras to the
default and retires the expected visits scoped to it, and the default cannot
be removed (409). `PUT /guard/cameras/{id}` takes an optional
`environmentId` (absent keeps the camera where it is; unknown is 404).
`GET /guard/episodes` takes `environmentId` and every row carries it.
Expected visits take an optional `environmentId`. `GET /guard/mode`,
`GET /guard/site` and `PATCH /guard/site` are gone.

**Residents and expected visits.** Recognised people stay global: a
resident's face is known in every environment, and identity has no notion
of membership per place, so "staff of the restaurant" is an open item rather
than a model guard can enforce today. Expected visits are scoped: a pass can
name an environment, so "the plumber comes at noon" for the home does not
make an unknown in the restaurant a guest.

**Migration.** `migrate()` adds the `environment_id` columns; after the
schema, `seedEnvironments(environmentSeed(config))` runs once (only when the
table is empty, in one transaction): the default environment takes its name
from the kind in `guard.notify_lang`, its kind/hours/closed mode/digest hour
from the old `guard_site` row when there is one (else the config seeds), its
mode from the stored `guard_state.mode` (else `guard.default_mode`), and
quiet policy `inherit`. Existing camera contexts, encounters and journal rows
are backfilled to it, the three digest state keys move to their `_<id>`
names (so the day's summary is not sent twice), and `guard_site` and the
`mode` key are dropped. An existing install behaves exactly as before.
`guard-migration-test` pins it on a legacy database carrying a single site.

**Measured** (`guard-scenario-test`, "three environments at the same
moment": a restaurant kitchen while open, a home garden while its residents
sleep and an office after hours, 13 interleaved observations; the before
column is the same events against a973774a in a scratch worktree, where one
site has to describe all three):

| Setup | Alerts | Spoken lines | What went wrong |
|---|---|---|---|
| Before, site = restaurant (open) | 0 | 0 | the stranger in the garden at night and the intruder in the closed office were treated as customers |
| Before, site = home (asleep) | 3 | 3 | the cook paged the owner and was spoken to through the speaker, at "night" |
| Before, site = office (closed) | 3 | 3 | the cook paged and was spoken to, "after hours" |
| After, three environments | 2 | 2 | none: "Persona desconocida · Jardín (Casa de campo)" and "Persona desconocida · Oficina (Oficina Centro)"; the kitchen stays in the Trattoria's summary |

**A compiler trap.** GCC 16 miscompiles a conditional expression inside the
operand of `co_await` (and a `co_await` inside either branch of one): the
repository's `setMode` evaluated the wrong branch and dereferenced an empty
optional, and the feature service's PATCH did the same with an optional
field. Build the input as a local before awaiting, and branch with `if`.

## Known arrivals for calls (2026-10, "Argus calls you")

An observation whose people are all recognized and whose primary person has
an identity (`personId > 0`) publishes `argus.guard.v1.known_seen`
(`{eventId, personId, cameraId, cameraName, environmentId, environmentName,
at}`) from the encounter stage of the saga, right after the encounter phase
commits. It is a plain core publish with no outbox and no change to any
verdict, notification or encounter: argus-notification's call engine turns
the first sighting after an absence into an opt-in "ha llegado Marta a la
Entrada" call or notification (`services/notification/CONTEXT.md`, "Argus
calls you"). A replayed observation may publish it again; the consumer keys
arrivals by person and time. Known people still never notify from guard
itself.

## Presence: who is home (2026-10, safety wave)

The owner's words: an intruder alert must know whether anyone is home, locally
and never by GPS. "Nobody home" calls the owner and the guards and offers the
siren; "people home, intruder outside" warns the people inside first and
quietly; "intruder inside with people home" never speaks through a camera in a
room where the family is; "night, everyone home" wakes people only on a clear
threat. Presence is the input those decisions read.

**Why guard owns it.** Presence is per user *per environment*, and guard owns
the environments. One of the three signals (a recognized face) is born in
guard's own saga, and the decisions that read presence (the response plan,
the deterrence ladder) are guard posture. A presence feature in
argus-notification would have needed the environments, the camera roles and
the known-seen feed from guard over the wire, only to hand presence back to
guard for the plan. argus-notification and argus-sync read it through the
SDK instead.

**What is stored: current state only.** `guard_presence` holds one row per
user and environment: `home` or `away`, the kind of signal behind it
(`lan_session`, `tunnel_session`, `app_activity`, `camera`, `timeout`), since
when, and the last home signal. A missing row means *unknown*. Every change
overwrites the row: there is no history, no address and no location, ever.
Rows go when the environment goes (`ON DELETE CASCADE`), when consent is
withdrawn or the account disabled, and after `[presence] retention_days`
(30) without a change. That retention is the one ONBOARD-CONSENT quotes in
the privacy notice.

**Signals.**
- *Home network vs tunnel.* `DeviceFilter` (lib/auth) classifies every request
  by where it arrived: `tunnel` on the `[remote] tunnel_port` listener,
  `loopback`, `lan` for private, link-local and ULA ranges (`[device]
  lan_networks` replaces the default list, e.g. for a Tailscale range),
  otherwise `external`. Only that class leaves the filter. `JwtFilter`
  forwards it to argus-auth's verdict (`ValidateTokenRequest.origin`), and the
  verdict core-publishes `argus.auth.v1.presence_signal` `{userId, sessionId,
  platform, origin, at}` for `lan`/`tunnel` when a session's origin changed or
  its `last_seen` advanced (at most once a minute per session). Guard never
  reads auth.db (rule 27). A phone (`android`/`ios`) on the LAN is a
  `lan_session`; a desktop or web session on the LAN is `app_activity`, a
  weaker sign (a desktop at home says less about its owner than a phone in a
  pocket). The LAN signal makes the person home in every environment with
  `lan_presence` (the server's own network; seeded on the default
  environment, editable through `PATCH /guard/environments/{id}`). Any
  session through the tunnel makes them away there.
- *A recognized face.* The presence feature subscribes to guard's own
  `argus.guard.v1.known_seen` (no saga coupling), asks identity which account
  the person is (`GetPerson.user_id`) and marks that user home in the
  camera's environment. `known_seen` gained `passerby`: a resident walking past
  on an outdoor public camera is not an arrival. A user who withdrew face
  consent has no `user_id` on their person, so a sighting cannot name them.
- *The away timeout.* A sweep every minute turns `home` into `away` with
  source `timeout` after `[presence] away_timeout_minutes` (45) without a
  home signal.

**The rules** (`presence_engine::apply`, pure). A home signal always wins
and keeps the first arrival as `since`. The tunnel means away, except within
`tunnel_grace_seconds` (90) of a home signal, because a phone leaving the
wifi races its own last LAN request. A timed-out `away` confirmed by the
tunnel becomes `tunnel_session` at once (the decision readers treat the two
differently), and a signal older than the row's last one changes nothing.
Two strengths of away exist on purpose: `tunnel_session` means the person's
device is provably outside; `timeout` only means silence, which a phone
asleep on the nightstand also produces. The response plan must never sound a
siren or speak into a room on a `timeout` away alone.

**Consent.** Nothing is stored for a user who has not consented
(`UserIdentity.privacy`, decided and presence, from identity; ONBOARD-CONSENT
owns the record). The verdict is cached five minutes and dropped by identity's
change feed: a user change whose `row.privacy.presence` is false, or whose
`isActive` is false, deletes the user's rows at once and publishes
`unknown`/`consent` for each environment. A change without `row.privacy` (a
rename) is not a consent change. Every ten minutes, and at boot,
`ListPrivacy` reconciles the table (covers a withdrawal that happened while
guard was down). An unreachable identity stores nothing: fail closed.

**Who reads it.**
- In guard: `PresenceRepository` and the helpers in
  `shared/vocabulary/presence-state.hxx` (`presence::stateOf`,
  `presence::overall`): shared because the response plan reads them too.
- Over gRPC: `argus.guard.v1.PresenceService/ListPresence` through
  `argus::clients::guard` (`GuardPresenceClient`), served on `[rpc] address`
  beside the settings service for the callers named `sync` and
  `notification` in `[rpc.callers]`. The setup, native-stack and deploy
  pairing fill `[guard] presence_credential` on their side.
- Live: `argus.guard.v1.presence_changed` `{userId, environmentId, state,
  source, since, overall}` on every state change. `overall` folds the
  environments: home anywhere is home, away everywhere is away, otherwise
  unknown.
- The app: `GET /guard/presence` (owner only, by `kGuardAccess` omission) gives
  `{people: [{userId, state, since, environments: [{environmentId, state,
  since}]}]}`: the coarse "en casa / fuera" of the people view, with no source.

**Code map.** `src/shared/vocabulary/presence-state.hxx`,
`src/shared/repositories/presence/` (`argus::guard-presence-repository`),
`src/feature/presence/` (`argus::guard-presence`: the pure engine, the
service with its NATS subscriptions and timers, the identity directory
adapter, the NATS publisher, the controller and its response DTO),
`src/app/rpc/presence-rpc-service` (`argus::guard-rpc`), `[presence]` in
`config.toml`. Tests: `guard-presence-repository-test` (storage, timeout,
retention, cascade), `guard-presence-test` (every signal, the grace, stale
signals, consent withdrawal, account disable, the consent sweep, the owner
view, the RPC with its caller check), `device-origin-test` (lib/auth) and the
presence cases in auth's `session-verdict-test`.

## Who is called, in what order, and how (2026-10, RESPONSE)

David (2026-10-04): per environment the Owner edits a recipient list in
Seguridad. By default Owner and Guards are called together, then the
Residents one by one, then the external emergency contacts; Guests get
nothing. A Resident may be lowered to notify. A Guard on duty cannot be taken
off intruder calls.

**Model.** `guard_response_recipient (environment_id, user_id, mode, step,
on_duty)` holds only what the Owner changed. A NULL `mode`/`step` means the
role default, so a user added later follows the defaults without a migration.
Defaults (`response_plan::members`):
- Owner and Guard: `call`, step 0.
- Resident: `call`, one step each after the last explicit step, in id order.
- Guest: `off`.
- Inactive users are never listed.
`guard_response_contact` holds up to ten external contacts (name, phone,
note, position). `guard_response_setting` holds the emergency number (dialled
by the phone; Argus cannot place calls) and the wait per step (15-300 s,
default 45, the length of a ring). The user list comes from identity's
`ListUsers` (`IdentityResponseDirectory`), never from identity's database.

**Duty.** A Guard is on duty when their `on_duty` toggle is set (their own
`POST /guard/environments/{id}/duty`, or the Owner's list) or while the
environment is staffed or open (`response_plan::staffedAt`). On duty they are
`mandatory`: called even if listed as notify, and the call engine skips their
own call switches (`services/notification/CONTEXT.md`, "Intruder response").
Setting them to `off` removes them; that is the Owner's explicit decision.

**The decision table** (`response_plan::build`, pure, per call-worthy
notification). It reads the plan members, PRESENCE's rows for the
environment, the camera context and the posture.

| Situation | Strategy | Who first | Notes |
|---|---|---|---|
| Panic, duress, or an episode turning worse (`escalated`) | `everyone` | every member at once | the actor (`excludeUserIds`) is never in the plan |
| Somebody home, intruder on an indoor camera | `everyone` | every member at once | guard also silences that camera's voice and siren |
| Night (night mode or the `night` reason), everyone with a presence row home, not critical | `night_quiet` | everyone, as notify | only a clear threat (critical) wakes people; a guard on duty still rings |
| Somebody home, intruder outside | `inside_first` | the people home, `discreet` | then the owner's order, one step later |
| Nobody known home, or tamper | `ordered` | the owner's steps | |

Notes on the table:
- "Somebody home" means a Home row. A user with no row (no consent, or
  never seen) is unknown and never counts as home.
- "Everyone home" needs at least one Home row and no Away row among the
  members.
- The siren is only *offered* (`offers: ["siren"]`) when every member is
  Away and at least one is positively away through the tunnel. A timeout
  alone could be a phone asleep on the nightstand (PRESENCE's caution).
  Guard itself never sounds anything because of this table; the deterrence
  ladder is unchanged except for the indoor rule below.

**Indoor speakers stay silent with the family inside.** At the context stage
the saga sets `checkpoint.familyInside`. It is true when the camera is
configured indoors (`outdoor = false`) and the environment's presence
`overall` is Home. It is persisted in the checkpoint, so a replay decides the
same. `guard_policy::deterrence` then returns no voice and no siren: a
speaker line or a siren in the room where the family is reveals them to the
intruder. An unconfigured camera changes nothing, because we do not know it
is indoors.

**What guard sends.** For a call-worthy notification (a time-sensitive or
critical `guard_episode`, a critical `guard_panic`, `guard_duress` or
`guard_tamper`), the batches carry only the plan's first step, and `data`
carries the plan as `response` (persisted in the outbox payload with the
batches, so a replay sends the same plan). The call engine reaches later steps
itself. Any other notification goes to every member not set to off, so a
Resident now receives guard notifications; before this, guard's roster was
owners and guards only. Without the identity directory, guard keeps the old
owner+guard roster (`legacyRecipients`).

**Feedback.** `ResponseVerdictFeed` subscribes to
`argus.notification.v1.response_verdict`. A verdict on a `guard_episode`
labels the episode, through the same `EpisodeRepository::review` the owner's
review uses: `false_alarm` gives `false_alarm`, `real` gives `useful`.

**API.** `GET /guard/environments/{id}/response`: Owner sees everyone;
Resident and Guard see only their own row plus the contacts and the emergency
number. `PUT /guard/environments/{id}/response` (Owner) replaces the list,
the contacts and the settings in one transaction, and refuses a user who is
not active (422 `RecipientUnknown`). `POST /guard/environments/{id}/duty
{onDuty}` is the Guard's own toggle. `kGuardAccess` matches `{id}` segments
now (`routeMatches`).

Tests: `guard-response-test` (defaults, overrides, every row of the table,
duty and staffed hours, the siren offer, panic actor exclusion, the JSON,
the DTO validation, the repository), `role-access-test` (the three routes).

## Panic and duress (2026-10, WATCHDOG)

The owner's plan ("SAFETY", point 4): a silent panic button in the app, and
an optional duress code that looks like disarming but silently raises a
critical alert. Both live in `src/feature/safety/` (`argus::guard-safety`)
because guard owns the posture they act on and the notify pipeline they
alert through.

**Panic.** `POST /guard/panic {environmentId?}` (every role, Guest included:
a babysitter in danger is exactly who needs it). It records a
`guard_safety_alert` row and raises it through
`GuardService::raiseSafetyAlert`: one critical notification, `data {kind:
guard_panic, urgency: critical, threadKey: guard:panic:<id>, episodeId: <alert
id>, cameraId: 0, actorUserId, actorName}`, sent through the normal notify
effect with `NotifyContent.excludeUserIds = {actor}`, so RESPONSE's plan and
the call engine ring everyone else and never the person who pressed it. The
actor gets one separate row, `kind: guard_panic_sent`, `urgency: passive`,
`silent: true` ("Aviso enviado"), so their screen confirms without any
sound. A second press within 60 s answers the same alert (`repeated`), so a
held finger or a retry never doubles the call. If the notification service
is down the press still answers (`sent: false`), the alert retries with
backoff and, after a restart, `resumePending` delivers rows younger than 15
minutes that never settled. The app makes it a 2-second hold
(`PANIC_HOLD_MS`), never a tap.

**Duress codes.** The owner switches them on for the household (`PATCH
/guard/safety {duressEnabled}`, Owner only). Then everyone who can disarm
(Owner, Resident) may set two codes (`PUT /guard/safety/pin {disarmPin,
duressPin}`: 4-8 digits, different; `DELETE` removes them). With codes set,
`POST /guard/mode {mode: home}` requires `pin`:

| PIN | Answer | Side effect |
|---|---|---|
| missing | 403 `PIN_REQUIRED` | none |
| wrong | 403 `PIN_INVALID` (5 in 15 min → 429 `PIN_LOCKED`) | none |
| normal code | the environment list, mode home | none |
| duress code | the same environment list, mode home | a `guard_duress` critical alert to everyone but the actor |

The duress answer is byte-for-byte the normal one and the mode really
changes, so the screen, the `/sync` frames and the environment state other
people see are those of an ordinary disarm. Arming (away/night/armed) never
asks. Switching the feature off deletes every stored code.

**Threat model.**
- *Who it is for:* someone forced to disarm in front of an intruder (a home
  invasion, a robbery at closing time). The attacker sees the app; the code
  must look ordinary and nothing on the actor's device may change.
- *What the attacker sees:* the same PIN prompt, the same answer and the same
  mode. The actor receives no notification, ring, call, `response_update` or
  hand-off frame (RESPONSE excludes `actorUserId` from the plan), and the
  alert is not a guard incident or episode: it lives in
  `guard_safety_alert`, which no list route returns, so "Vigilancia" on the
  actor's phone shows nothing new. Other members get a critical call; if the
  attacker holds *their* phone too, they see it, which is unavoidable.
- *Secrets:* codes are stored only as PBKDF2-HMAC-SHA256 (600 000
  iterations, 16-byte random salt, constant-time compare) in
  `guard_user_pin`; neither code is logged, journaled, synced or returned.
  Short PINs are brute-forceable offline from a stolen `guard.db`, so the
  hash slows that down rather than prevents it; online guessing is capped by
  the lockout. A lockout is per user and in memory, so a restart resets it;
  that is an accepted gap.
- *Not covered:* a household with a single member has nobody to alert (the
  external emergency contacts of RESPONSE are the only fallback); a duress
  phrase over voice is not built (an STT transcript cannot be matched
  against a hash reliably, and the voice tool cannot disarm a user who has
  codes, it gets `PIN_REQUIRED`); an attacker who knows the household uses
  codes can demand the "other" one.

Code: `src/feature/safety/` (controller, DTOs, `SafetyService` as the
`DisarmGate` that `GuardFeatureService::setMode` consults, `pin-hash`,
`pin-attempts`, three repositories, `GuardAlertSink`,
`NotificationActorNotifier`), `src/feature/guard/services/disarm-gate.hxx`,
tables `guard_user_pin`, `guard_safety_setting`, `guard_safety_alert`;
`tests/unit/guard-safety-test.cc`.

## Who is called, in what order, and how (2026-10, RESPONSE)

David (2026-10-04): per environment the Owner edits a recipient list in
Seguridad. By default Owner and Guards are called together, then the
Residents one by one, then the external emergency contacts; Guests get
nothing. A Resident may be lowered to notify. A Guard on duty cannot be taken
off intruder calls.

**Model.** `guard_response_recipient (environment_id, user_id, mode, step,
on_duty)` holds only what the Owner changed. A NULL `mode`/`step` means the
role default, so a user added later follows the defaults without a migration.
Defaults (`response_plan::members`):
- Owner and Guard: `call`, step 0.
- Resident: `call`, one step each after the last explicit step, in id order.
- Guest: `off`.
- Inactive users are never listed.
`guard_response_contact` holds up to ten external contacts (name, phone,
note, position). `guard_response_setting` holds the emergency number (dialled
by the phone; Argus cannot place calls) and the wait per step (15-300 s,
default 45, the length of a ring). The user list comes from identity's
`ListUsers` (`IdentityResponseDirectory`), never from identity's database.

**Duty.** A Guard is on duty when their `on_duty` toggle is set (their own
`POST /guard/environments/{id}/duty`, or the Owner's list) or while the
environment is staffed or open (`response_plan::staffedAt`). On duty they are
`mandatory`: called even if listed as notify, and the call engine skips their
own call switches (`services/notification/CONTEXT.md`, "Intruder response").
Setting them to `off` removes them; that is the Owner's explicit decision.

**The decision table** (`response_plan::build`, pure, per call-worthy
notification). It reads the plan members, PRESENCE's rows for the
environment, the camera context and the posture.

| Situation | Strategy | Who first | Notes |
|---|---|---|---|
| Panic, duress, or an episode turning worse (`escalated`) | `everyone` | every member at once | the actor (`excludeUserIds`) is never in the plan |
| Somebody home, intruder on an indoor camera | `everyone` | every member at once | guard also silences that camera's voice and siren |
| Night (night mode or the `night` reason), everyone with a presence row home, not critical | `night_quiet` | everyone, as notify | only a clear threat (critical) wakes people; a guard on duty still rings |
| Somebody home, intruder outside | `inside_first` | the people home, `discreet` | then the owner's order, one step later |
| Nobody known home, or tamper | `ordered` | the owner's steps | |

Notes on the table:
- "Somebody home" means a Home row. A user with no row (no consent, or
  never seen) is unknown and never counts as home.
- "Everyone home" needs at least one Home row and no Away row among the
  members.
- The siren is only *offered* (`offers: ["siren"]`) when every member is
  Away and at least one is positively away through the tunnel. A timeout
  alone could be a phone asleep on the nightstand (PRESENCE's caution).
  Guard itself never sounds anything because of this table; the deterrence
  ladder is unchanged except for the indoor rule below.

**Indoor speakers stay silent with the family inside.** At the context stage
the saga sets `checkpoint.familyInside`. It is true when the camera is
configured indoors (`outdoor = false`) and the environment's presence
`overall` is Home. It is persisted in the checkpoint, so a replay decides the
same. `guard_policy::deterrence` then returns no voice and no siren: a
speaker line or a siren in the room where the family is reveals them to the
intruder. An unconfigured camera changes nothing, because we do not know it
is indoors.

**What guard sends.** For a call-worthy notification (a time-sensitive or
critical `guard_episode`, a critical `guard_panic`, `guard_duress` or
`guard_tamper`), the batches carry only the plan's first step, and `data`
carries the plan as `response` (persisted in the outbox payload with the
batches, so a replay sends the same plan). The call engine reaches later steps
itself. Any other notification goes to every member not set to off, so a
Resident now receives guard notifications; before this, guard's roster was
owners and guards only. Without the identity directory, guard keeps the old
owner+guard roster (`legacyRecipients`).

**Feedback.** `ResponseVerdictFeed` subscribes to
`argus.notification.v1.response_verdict`. A verdict on a `guard_episode`
labels the episode, through the same `EpisodeRepository::review` the owner's
review uses: `false_alarm` gives `false_alarm`, `real` gives `useful`.

**API.** `GET /guard/environments/{id}/response`: Owner sees everyone;
Resident and Guard see only their own row plus the contacts and the emergency
number. `PUT /guard/environments/{id}/response` (Owner) replaces the list,
the contacts and the settings in one transaction, and refuses a user who is
not active (422 `RecipientUnknown`). `POST /guard/environments/{id}/duty
{onDuty}` is the Guard's own toggle. `kGuardAccess` matches `{id}` segments
now (`routeMatches`).

Tests: `guard-response-test` (defaults, overrides, every row of the table,
duty and staffed hours, the siren offer, panic actor exclusion, the JSON,
the DTO validation, the repository), `role-access-test` (the three routes).
