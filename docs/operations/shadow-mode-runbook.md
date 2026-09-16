# Shadow-mode runbook

How to run argus-guard in shadow mode for one to two weeks, read what it
measured, and later switch the belief gate to enforce. No source changes are
needed for any step below — only configuration and SQL reads.

## 1. Turn on shadow mode

Shadow is the default. In the guard service config confirm:

```toml
[guard]
decision_mode = "shadow"

[guard.belief]
gate_scope = "notify"
```

`decision_mode` governs the belief gate only. In shadow the gate never
suppresses anything; per-encounter notification threading still applies, so
expect fewer repeat notifications than before Round 6 — that is intended.
`arm_siren` stays `false`; nothing audible changes.

Restart (or deploy) the guard service. No other service needs a change.

## 2. What to expect in the journal

Every observation that reaches the effects stage writes one row to
`guard_decision_journal` in `guard.db`:

- `legacy_would_notify = 1` means the old severity rule would have notified.
- `belief_would_notify = 1` means the belief score cleared its severity
  threshold (`critical 1 < high 3 < medium 5 < low 7`, see `[guard.belief]`).
- `did_notify = 1` means the notification actually went out.
- `suppression_reason` is `none`, `belief_gate`, `budget`, `legacy_silent`
  or `thread_suppressed`; `suppressed_kinds` lists the skipped effect kinds.
- `belief_score` with `belief_signals` names the evidence for the score.
- `dispatch_attempts` counts how many times a notification send was initiated
  for the row. The counter is bumped before the notification RPC, so a crash
  or hang during the RPC — after the journal row exists but before the
  service commits — still flags the row. Attempts that never reach the
  service are counted too; that is the conservative watch metric.
  `did_notify = 0` with `dispatch_attempts > 0` and
  `legacy_would_notify = 1` is the ambiguous population: a send was initiated
  but no successful settlement was recorded (crash or hang during the RPC,
  lost settlement, exhausted retries, dead-lettering). These rows fail open — a later same-tier
  observation still notifies — and the summary exposes them as
  `ambiguousNotifications` so calibration can count them separately instead
  of mistaking them for suppressions. Review them against footage like any
  divergent row; they are uncertainty, not decisions.

After one day you should see hundreds to low thousands of rows depending on
camera traffic, almost all with `did_notify = legacy_would_notify` and
`suppression_reason = none`, plus `thread_suppressed` rows wherever one
encounter produced several same-tier observations. After one week the
`scoreHistogram` (below) should show two humps: high scores on notified rows,
low scores on `belief_would_notify = 0` rows.

Fallback (guard down) suppressions never appear here by design — they carry
no belief score. They are counted on the gateway `/health` endpoint under
`notifications_fallback` and logged with their reason. Since Round 12 every
fallback drop also lands a durable row in `gateway_fallback_event`
(`gateway.db`, gateway-owned): query `WHERE created_at BETWEEN <outage
start> AND <outage end>` grouped by `reason` (`non_hard_signal`,
`drop_known`, `drop_weak_score`, `drop_short_dwell`, `budget_silent`) to
reconstruct exact fallback volume for a past outage window — the
process-lifetime counters alone cannot do that across a restart. Treat the
journal as the complete record of guard decisions, the fallback table as
the complete record of gateway decisions while guard was absent.
Fallback rows older than `notifications.fallback_retention_days`
(default 90) are purged on the per-minute digest tick.

Upgrade note for existing deploys: the gateway now keeps its fallback
record in `data/gateway/gateway.db` (new `data/gateway` volume in
docker-compose, created automatically by `scripts/provision-host.sh` for
new hosts). If that directory is absent the gateway still boots and
serves — it logs an error naming the directory and records nothing until
it exists. The one manual step on an already provisioned host, no
re-provisioning needed:
`mkdir -p data/gateway && chmod 700 data/gateway` (same ownership as
`data/identity`), then restart the gateway; no migration, no re-pairing.

## 3. Read the summary endpoint

`GET /guard/decisions/summary?from=<unix>&to=<unix>` (Owner only) returns
SQL-computed aggregates — no raw rows needed for calibration:

- `totalRows`, `fired`, `legacyWould`, `beliefWould`: notifications that
  fired vs would have fired under each rule. `legacyWould - beliefWould`
  is the suppression volume.
- `scoreHistogram`: per severity and exact score, row counts split by
  `didNotify` and `beliefWould`. To sweep a candidate threshold T for one
  severity, sum `rows` with `score >= T` — no re-read required.
- `byCameraDay` / `byCameraHour`: event and notification counts per camera.
- `signals`: how often each belief signal fired.

`GET /guard/decisions` pages the raw rows with filters (`camera_id`,
`severity`, `decision_mode`, `suppression_reason`, `divergent_only`,
`from`/`to`) and a stable `nextCursor` (`createdAt` + `eventId`).

## 4. What `belief_would_notify = 0 while legacy_would_notify = 1` means

The severity rule wanted a notification and the belief evidence did not
support it (weak detector median, short dwell, unstable track, no analysable
face, or degraded camera health — see `belief_signals`). In shadow mode the
user still got notified (`did_notify = 1`); the row records what enforce
mode would have done. Before trusting a suppression, check `belief_signals`:
a real intrusion suppressed by a broken camera (health signal) or a blind
zone (unobservable identity) is a sensor problem, not a calibration win.

## 5. Switching to enforce: procedure and preconditions

Do this only after the shadow data review, and only for `gate_scope`
`notify` first.

Preconditions:

1. At least one full week of shadow rows with normal camera traffic.
2. Review every row with `legacy_would_notify = 1` and
   `belief_would_notify = 0`: confirm each is a false alarm from the
   operator's own judgement (walk the corresponding camera footage or
   incident evidence), not a real event the belief engine missed.
3. `scoreHistogram` shows a clean separation per severity you intend to
   enforce; if notified and suppressed scores overlap heavily, do not
   proceed — retune thresholds in `[guard.belief]` and collect another week.
4. Decide the scope deliberately: `notify` pages the owner less;
   `communication` additionally silences announcements; `all` may silence
   alarm and siren-arm, but a hard floor always lets physical effects
   through regardless of scope.

Procedure:

1. Set `decision_mode = "enforce"` (keep `gate_scope = "notify"` initially)
   in the guard config and restart the guard service.
2. Watch `guard_decision_journal` for `suppression_reason = "belief_gate"`
   rows and confirm `did_notify = 0` on exactly those rows.
3. Keep the first enforce week under daily review; revert to `"shadow"` on
   the first missed real event — reverting is instant and lossless, the
   journal keeps measuring either way.

Retention: journal rows older than `guard.journal_retention_days`
(default 90) are purged by the daily retention sweep. Keep the default
until Round 9 calibration is done.

## 6. Quiet log, near misses and labels (Round 11)

Every suppression now lands in the journal, including early-watch staging
(`suppression_reason = "staging"`, previously unrecorded) and sustained
tamper escalations. Each `GET /guard/decisions` row carries a `summary`
field with the belief signals in the same human phrasing as notification
bodies, plus `noveltyScore`, `repeatVisits`, `quietHold`, `budgetHold`,
`assessMs` and `feedbackLabel`.

Near misses: pass `near_miss_margin=N` to `GET /guard/decisions` or
`/guard/decisions/summary` to surface rows that scored within N points
below their notify threshold (`belief_would_notify = 0` and
`belief_score >= belief_threshold - N`). Review these like divergences;
they are the retrospective path for a suppression that should not have
happened. `summary.nearMisses` counts them without paging rows.

Labels: `POST /guard/decisions/{eventId}/feedback {"label":
"useful|false_alarm|not_now"}` stores the resident verdict on the row.
Labels never retune live thresholds; they are collected so a later round
can calibrate offline against a held-out set. Implicit signals come free:
display confirmations (`acked_at`, see below) and the existing read flags.

## 7. Delivery proof (Round 11)

`GET /notification/delivery-summary` reports `pending`, `unacked`,
`unackedOld` (sent but unconfirmed past `notifications.ack_window_s`,
default 24h), `sent`, `acked`, dispatch-to-settle latency
(`latencyMsP50/P95/Max`, millisecond legs), and the synthetic probe
(`probeAt/probeOk/probeMs`, every `notifications.selftest_interval_s`,
default 300s, user-0 rows invisible to every device). Reference points:
under ~3s feels live, over ~30s needs an explanation in the incident
record, anything older than the ack window is followed up, never assumed
received. Clients confirm display with `PATCH /notification/ack
{"notification_ids": [...]}`; only sent rows flip, repeats return 0.

## 8. Tamper, quiet hours and baselines (Round 11)

A tamper-ish health state (`moved`, `covered`, `blurred`) held
past `guard.tamper_sustained_s` (default 300s) notifies once per episode
as a high-danger `camera_tamper` incident; it stays quiet while the
condition persists and re-arms after recovery. Camera health never raises
observation danger (Round 13).

`[guard.quiet_hours]` (`enabled = false` default) only marks rows
(`quiet_hold`, `budget_hold` over `daily_budget`, default 30): held rows
still notify exactly as before. `noveltyScore` (1/(1+ema) per
camera-hour-of-week, 0 cold) and `repeatVisits` (unknown-signature
clusters) are journaled inputs for later calibration, never live inputs.
`assessMsP50/P95` and `detectionHealth.thermalBucket`
(nominal/warm/hot/unknown, coarse, not a throttling claim) expose inference
and platform health in the summary.
