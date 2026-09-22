# Sub-step 3a-2c — the notification durable change outbox

The second of the four producer outboxes `architecture-plan.md:835` (Phase 3a,
step 2) asks for: the notification domain's audit publish now writes a durable
row before it is published, and a drain thread settles it on the JetStream
PubAck. The unit is also where the `UserChangeSink` contract stops asking a
service for events it never emits.

## Pre-state, measured

The decision report counted this producer as **1** call site and noted that its
two `emit*` overrides are dead (`f3-2a-producer-outbox-decision.md:185`). Both
hold, with one refinement: the single site is a **loop**, so one
`PATCH /notification/read` enqueues one audit event per row it actually
changed.

| Site | At `HEAD` |
|---|---|
| `NotificationService::markAsRead` (`notification-service.cc:123-141`) | `repository_.markAsRead(userId, ids)` returns the rows that moved, then one `co_await sink.publishAudit(...)` per change |
| `NatsNotificationChangeSink::publishAudit` (`:36`) | dedups recipients, builds a `UserAuditEvent`, then `bus_->publish(nats_subject::kNotificationChange, …)` — core NATS, return value only feeding a `LOG_WARN` |
| `NatsNotificationChangeSink::emitUser` / `emitUsers` (`:20`, `:28`) | `bus_->publish(sync_change::userEmitPayload(...))`, fire and forget |
| `user_change::setNotificationSink` (`main.cc:157`) | installed only when NATS connected; a missing sink drops the audit with a `LOG_WARN` (`notification-service.cc:129-133`) |

**The two emit overrides have no caller, measured rather than carried over.**
`grep -rn 'emitUser\?s\?('` over `packages` and `services` finds production
callers only in `services/productivity` — **seven**: five `emitUsers`
(`calendar-event-share:30`, `project-task:46`, `project-member:30`,
`project:29`, `calendar-event:30`) and two `emitUser`
(`calendar-event-share:58`, `project-member:58`). The only other callers in the
tree are the two test doubles, each of which implements `emitUser` by
delegating to its own `emitUsers` (`productivity-controller-test.cc:226`, and
notification's at HEAD `:56`), and this service's own suites pin that deadness
with assertions (`notification-controller-test.cc:294`
`CHECK(sink.emits.empty())`, `notification-delivery-test.cc:256`
`CHECK(changeSink.emits == 0)`): the
notification domain emits audit diffs and nothing else. A notification row
reaching a user's room travels the delivery leg
(`argus.notification.v1.delivery` → sync's inbox), not this subject.

So a broker outage, a restart or a rejected publish loses a mark-as-read diff
that clients never see: `sync`'s fan-out is the only writer of
`user_audit_log`, and it learns of the change only from this publish.

## The design

**The contract splits, additively.** `publishAudit` moves to a new base,
`AuditSink`, and `UserChangeSink` becomes `AuditSink` plus the two emits
(`packages/contracts/sync/src/sync/user-change-sink.hxx`).
`user_change::notificationSink()` is typed `const AuditSink*`, so the
notification sink implements one virtual instead of three — two of which had no
caller. Productivity is untouched: it keeps `UserChangeSink`, all three
virtuals, and the same `setProductivitySink`. `publishAudit` gains
`[[nodiscard]]` at the same time, matching the camera sink's
`emitModule`/`publishAudit` after 3a-2b; every call site in the tree already
`co_await`s it (measured: eight, seven in productivity and the one in
notification's `markAsRead`).

**The outbox is the camera one, copied, and the plan says so.** §3.6:
"each one builds the payload from `contracts/sync`, writes it to its own outbox
in its own database, and publishes to the frozen subject through `lib/nats`.
None of that is shared code, which is exactly why `socket`, `room` and `sync`
die instead of becoming packages". The module is therefore
`services/notification/src/shared/repositories/change-outbox/` — the same six
files, the same dispositions (`Enqueued`/`Replay`/`Conflict`/`Failed`), the same
status-guarded CAS — and the table is `change_outbox`, the **same name in every
producer's database**, deliberately against this service's `notification_*`
prefix: it is the same thing in every producer and an operator should find it
under the same name in all four.

**The event id names the transition.** `"notification-change:" +
sha256Hex(table | recordId | payload).substr(0, 32)`, with the payload the
`UserAuditEvent` JSON the publish already built — the amendment 3a-2b measured
(`f3-2a-producer-outbox-decision.md`, "Amendment"), applied here from the
start rather than rediscovered. 32 hex digits plus the 20-character prefix is
52 characters, inside the JetStream `Nats-Msg-Id` header budget.

**The sink keeps its payload logic and drops its publish.** Recipient dedup,
the flat diff, the timestamp and the event's field order are unchanged; what
changes is that the JSON is enqueued (retrying a failed write three times, then
logging that the change is lost) instead of published, and a worker
publishes from the table with `publishWithMsgId`, marking a row sent only on
the PubAck. A payload past 256 KiB is refused at the door for the reason 3a-2b
measured: one row the broker refuses for ever parks every change behind it.

**A pass drains a batch, not a single row.** Reading a page of notifications
back marks every row as read, so one `PATCH /notification/read` is one burst of
audits, not one audit: at one row per 50 ms tick a 500-row page would trail the
client by half a minute, and the client is the one that has to see its own
change. `flushLoop` therefore reads up to `kDrainBatch` (64) pending rows in
**one** statement, publishes them oldest-first through `flush(row)` and waits
`kProgressMs` (50) while anything settled, `config_.retryMs` (500 in production)
when nothing did. One read per pass rather than one per row: settlement is per
row by construction — the PubAck is the whole point — but the read is not, and
the batch is where rule 21's "one multi-row statement over N single statements
in a loop" applies. The ordering guarantee is untouched: the batch is read
oldest-first, so a refusal still stops the pass at that row and the changes
behind it wait. Camera's sink reads and settles one row per tick and is the
older shape; the divergence is recorded as S3 below.

## The stream the change leg needs, and why it is its own

**A publish here is a JetStream publish, and the change subject had no stream.**
`NatsBus::publishWithMsgId` calls `js_Publish` and "never degrades to core NATS"
(`nats-bus.hxx:53-56`), so the first version of this drain could never settle a
row in production: `js_Publish` on a subject no stream matches is refused, the
attempt counter climbs for ever, and every later change waits behind the first.

The gap was invisible in camera because a stream already covered it:
`NatsObjectEventSink::ensureStream` declares `{"argus.camera.v1.change",
kCameraObjectDetected}` on `ARGUS_CAMERA`, so 3a-2b's change sink had a stream
to publish into. Nothing in the tree covered `argus.notification.v1.change`:
`ARGUS_NOTIFICATION` is created by `NatsNotificationDeliverySink::ensureStream`
over `{config_.subject}` — the delivery subject alone — and no other `ensureStream`
call in any unit names the change subject. Notification's change rows would have
parked behind a publish that cannot be acked.

**The change leg gets its own stream, `ARGUS_NOTIFICATION_CHANGE`, and the name
lives in the service.** Sharing `ARGUS_NOTIFICATION` is not merely coupled, it is
refused: `NatsBus::reconcileStream` (`nats-bus.cc:560-622`) compares the subject
sets it is given with the ones the stream carries and rejects a mismatch
("carries different subjects; refusing to repurpose it"), so the sink that
declared `{delivery}` first would lock out the one declaring `{delivery, change}`
— and both legs must stay independently configurable, because the delivery
sink's subject and stream are `Config` fields its own live test drives per run.
The change stream's name therefore sits in `NatsNotificationChangeSink`'s own
`Config`, exactly where camera keeps its change stream's fallback, and not in
`packages/lib/nats/src/nats/nats-subject.hxx`: the constants there are read by
*other* units (argus-sync reads `kNotificationDeliveryStream` to consume), and
nothing outside notification names this stream. Adding a constant for a reader
that does not exist is structure ahead of its consumer (rule 24).

**The stream is ensured in the drain, and a refusal arms the ensure again.**
`flush` ensures it while the bus is connected, before it publishes, and a
publish the broker refuses — which is also how a stream that disappeared under
the running process looks — clears the latch so the next tick ensures it again.
There is no boot-time ensure beside `reconcile()`: the drain attempts it within
one `retryMs` of the first change and then again after every reconnect or
refusal, so a second call site would only duplicate the mechanism. A broker
whose JetStream store is wiped while the service runs therefore stalls the feed
for one `retryMs`, not for the life of the process.

**The live leg pins every path.** The suite's opt-in leg now asserts (a) the
drain settles a row published against a stream the *test* created, reading the
message back off the broker, (b) a sink pointed at a stream that does not
exist yet creates it and settles: the row leaving `pending` is the proof, and
`streamInfo` reads the created stream's subject set back, and (c) a backlog of
100 changes — more than a single batch holds — drains inside three seconds,
which is the batch loop's own leg. The stranded leg keeps
its wildcard subject, where `reconcileStream` refuses to repurpose the existing
stream and the invalid publish subject leaves the row pending with a counted
attempt.

**(c) is a bound that was measured to fail on its own regression.** The first
version of it — a 3 s bound on a burst enqueued while the sink was already
reconciled — passed with `kDrainBatch` set back to 1 (measured: 44/44), because
every `enqueue` calls `wake_.notify_all()` and a backlog still being written
wakes the drain far ahead of its 50 ms tick, so a one-row pass keeps up. The
leg now writes the 100 rows **before** `reconcile()`: with nothing left to
notify it, a one-row pass pays its tick per row and the same bound fails at
**5.223 s** (measured, with `kDrainBatch = 1`), while the batched drain settles
them in well under a second. A pin that cannot fail on the change it guards is
not a pin, and this one was rewritten until it could.

## The boot wiring the sink also needed

The sink was first installed inside the branch that ran only when the broker
**accepted the connection at boot**. With NATS down at start-up — the case a
durable outbox exists for — `changeSink` stayed null, `user_change::setNotificationSink`
was never called, and `markAsRead` logged its "no sink installed" WARN and
dropped every audit for the whole life of the process, reconnecting or not. The
sink is now constructed and installed whenever NATS is *configured*, connected
or not, which is what `services/camera/src/main.cc:157-159` already did and what
the sink's own drain is built for: it holds the bus, waits out a disconnected
one, and ensures its stream on the first tick after the reconnect. The delivery
sink keeps its own unconditional construction, which it already had.

## The review, and what it changed

Two reviews ran over this unit — one on correctness, one on conventions — and
every finding was verified against the code before it was applied or recorded
as a step-level item below; none was left implicit. Two of them proposed a fix
that measurement refused, and the record says so.

| # | Finding | Disposition |
|---|---|---|
| A1 | `streamReady_` latched for the process's life, so a wiped JetStream store stalled the feed for ever, and this report claimed a re-ensure after every reconnect that the latch made impossible | **Applied.** The refusal path clears the latch, the report paragraph is corrected, and the bullet that described the latch as a limitation is replaced by S2 |
| A2 | The drain's per-row step (`flushOnce`, now `flush`) discarded `markSent`'s result and returned true anyway: a row the broker stored but the database did not settle republished at the progress cadence for ever and never reached the stuck log | **Applied.** The result is honoured, following the one checked shape in the tree — `services/camera/src/operator/nats-object-event-sink.cc:161-166` reads it to decide its post-mark hooks. This sink reads it to hold the row at the head: a failed settle logs and returns false, so the row drops to the retry cadence instead of being republished at the progress cadence |
| A3 | The same-millisecond block claims to pin the `rowid ASC` tiebreak, but the two rows it compares are settled as they are read, so the clause is unobservable there | **Applied.** The comment now says what the block proves; the clause's unobservability is recorded in it |
| B1 | The change stream has no durable consumer — `sync` reads the wildcard over core NATS — so durability stops at the broker: a change published while `argus-sync` restarts is never replayed | **Documented** as S4: it holds for every change subject in the tree, so the fix is a sync-side consumer design, not a notification one |
| B2 | The drain was paced at 20 rows/s even with a backlog | **Applied**, as the batch above |
| B3 | An unreachable stream is re-ensured and re-logged every tick, unthrottled; the WARN comes from `NatsBus::reconcileStream` (`nats-bus.cc:573`), so throttling it is a `lib/nats` change with camera and guard consumers | **Documented** as a known item; no half-mitigation inside one producer |
| B4 | No purge of `sent` rows, so the table grows with the feed | **Documented** as S5, a retention decision that must be the same for all four producers |
| B5 | The enqueue retry path is unreachable under test — `trantor::EventLoop::getEventLoopOfCurrentThread()` is null under `drogon::sync_wait` — while production, where the call arrives on the loop, is unaffected | **Documented** as a known gap in coverage |
| B6 | The 256 KiB door is unreachable from notification's only producer, whose diff is two scalars | **Documented**: inherited defence-in-depth, as in camera |
| B7 | The enqueue is not in the domain write's transaction | **Already recorded** as S1, re-confirmed at step level |

The review also verified, against the code, that durability ordering holds (no
row is ever marked sent without a stored message, the CAS is status-guarded and
oldest-first ordering never overtakes), that the contract split leaves every
implementer complete (productivity's `NatsProductivityChangeSink` still derives
from `UserChangeSink` with all three virtuals), and that no dead code was left
behind in this service.

### The conventions review

It read the same snapshot and ran the project's own gates over it.

| # | Finding | Disposition |
|---|---|---|
| C1 | **Rule 20**: five statement-level comments inside the sink's functions, and two more over namespace-scope constants | **Applied.** The production source now carries no comment inside a function body, which is what this service's other production files do (measured: `nats-notification-delivery-sink.cc` and `notification-service.cc` have none), and the reasoning they carried moved to `CONTEXT.md`, where rule 20 sends it |
| C2 | **Rule 20**: eighteen statement-level comments across the two new test files | **Documented** as S7. The idiom is tree-wide (measured: 57 of 132 `*-test.cc` files carry one, 518 lines), so stripping only this unit's suites would leave them unlike every other one in the tree; the sweep is step-level, not one unit's |
| C3 | The live leg's expected payload raced the drain — reading the outbox after `publishAudit` on an already-reconciled sink can find it empty | **Applied, with the proposed fix refused.** Recomputing the expectation from the DTO as camera does is impossible here: `UserAuditEvent` embeds `nowMs()`, so no test-side recomputation can match. The payload is read off the outbox **before** `reconcile()` instead — deterministic, and it keeps the assertion meaning what it says |
| C4 | The burst leg could not fail if `kDrainBatch` were reverted to 1 | **Applied**, after the first attempt at it was itself measured not to fail — see the burst paragraph above |
| C5 | A1 and A2 have no regression test | **Documented**: no seam exists to write one against. `NatsBus` exposes no delete-stream call, and the outbox is a concrete member (rule 4's convention, not an injectable interface), so both are unverifiable by construction from this unit |
| C6 | A drain tick can outlive Drogon's database manager — the read precedes any shutdown guard, and `app().quit()` releases `dbClientManagerPtr_` | **Documented** as S8: inherited verbatim from camera's drain (same read-first order), not reproduced, and the fix is shared |
| C7 | **Rule 21**: a 64-row pass issued up to 128 statements | **Applied.** `pendingBatch(limit)` replaces `nextPending()` in the repository, so a pass reads its batch in **one** statement; the 25 test call sites read a batch of one through two helpers |
| C8 | **Rule 23**'s 2+ rule against the module's home: `shared/repositories/change-outbox/` has one production reader | **Documented** as an observation: it mirrors camera and the step's per-producer shape, and it becomes visible when `src/notification/` turns into `feature/` in Phase 4 |
| C9 | Two documents still described the emit leg this unit deleted (`wire-nats-subjects.md:32`, `CONTEXT.md:151`) | **Applied.** Both reworded: the domain publishes audit diffs only, and its rows reach users on the delivery leg |
| C10 | The report's S2 attributed the latched `streamReady_` to camera's change sink | **Already corrected** before the review landed — it names the object-event sink for the latch and the change sink for the discarded `markSent` |

It also reproduced this unit's flagship figures independently on the same tree
(43/43 live assertions against the local broker, 36/36 under `ctest`,
`check-deps.sh` at 469 edges) and confirmed, file by file, rule 27 (the outbox
uses this service's own default client, the correct divergence from camera's),
rules 1/2/3/19 in the new module, rule 25 (`argus_module` in the module's own
`CMakeLists.txt`, parent discovery, no glob), the additive contract split's
completeness across the tree, and the boot wiring's install-when-configured
shape. It also measured the module's files against camera's: byte-identical
after name normalization but for the key prefix, the log labels and the module
name. Re-measured here, that enumeration was one short and one edit stale: the
files also differ in the DB accessor (`cameraClient()` against this service's
`client()`, the rule 27 divergence the same paragraph records) and in the
batched read this unit applied afterwards, which is what `change-outbox-query.hxx`,
the repository header and `change-outbox-repository.cc` are the only files to
carry. Every other line of the six files is camera's, character for character.

## Evidence

| Command | Result |
|---|---|
| `./scripts/build-all.sh dev --only notification` | exit 0, **0 warnings**, **36/36 tests passed** (re-run after the conventions review's fixes) |
| `./scripts/check-deps.sh` | 62 declarations, **469 edges, 0 forbidden, 0 cycles, 0 unresolved** (464 before this unit: the new module and its links) |
| `ARGUS_NATS_URL=nats://127.0.0.1:4222 ./notification-change-outbox-sink-test` | **44/44 assertions**, including the broker round trip, the stream the sink creates itself, the 100-change backlog under its 3 s bound, and the stranded-row leg |
| the same leg with `kDrainBatch = 1` | **43/44 — the bound fails at 5.223 s**, which is the burst leg's proof that it pins the batch it guards (C4) |
| `./scripts/build-all.sh dev` (full, 17 projects) | exit 0, **0 warnings**, every project's tests green (no suite reports a failure), `check-tidy: 487 TUs, 3139 findings over 45 checks, baseline 3141; 2 checks below it` — no check risen, and more TUs scanned than the baseline's 481, so the floor is satisfied |

The live leg's log is itself evidence of the two refusal paths working: the
oversized payload is refused at the door with its byte count, `reconcileStream`
refuses to repurpose the test's stream for the wildcard subject, and the row
behind it is reported unpublished after its first attempt.

## What this unit deliberately does not do

- **No second drain mechanism for this service's own reconciler rhythm.** The
  delivery path settles inline and sweeps on a 60 s `runEvery`
  (`notification-rpc-service.cc:103`), and reusing it was the cheaper-looking
  option; it is not taken because that reconciler returns early when no
  delivery sink is installed, so a change drain riding it would silently stop
  in exactly the configuration where the change path is still live. The sink
  owns its own worker, as camera's does.
- **No transactional enqueue** — the step-wide S1, below.
- **No shared fix for what the two reviewed defects turn out to be in camera
  too.** The review found two defects here that camera carries in two different
  files: its object-event sink latches `streamReady_` for the process's life
  (`services/camera/src/operator/nats-object-event-sink.cc:140-141`, the sink
  that owns `ARGUS_CAMERA`), so a wiped JetStream store stalls its feed until a
  restart — and its change sink discards `markSent`'s result after a publish
  the broker accepted (`services/camera/src/camera/nats-camera-change-sink.cc:138`),
  so a row stored but not settled republishes at the progress cadence for ever
  and never reaches the stuck log. Camera's change sink does not ensure a
  stream at all, which is why only the second defect is in that file. Both are
  fixed **here**, which makes this drain the checked shape and camera's the
  older one; the two fixes belong in those two files and are named as the
  step-wide item S2 below rather than carried out from a unit that does not own
  them.
- **No removal of the `UserChangeSink` emits themselves.** Productivity's seven
  call sites are live; the split is what lets a service stop paying for an
  interface it does not use.

## The items this unit leaves at step level

**S1 — the transactional enqueue.** The plan's risk table
(`architecture-plan.md:903`) says: "A producer that loses
its outbox write loses a change forever — nothing in Phase 3a step 2 is
optional: the outbox row and the NATS publish travel in one transaction".
3a-2b deferred that (its report's S1) and this unit does the same, with the
same residue: the enqueue follows the domain write instead of joining its
transaction, so a crash in the window loses one change, and the bounded retry
plus the loud log are the mitigation. It is named here as a **step-wide item
for all four producers** rather than closed in one of them, because the fix is
one mechanism (a transactional enqueue around each producer's domain write,
which is 11 autocommit repository methods in camera alone) and four
half-versions of it would be worse than one visible deferral. It is also the
one place where the plan's words and the implementation differ, so the
divergence belongs next to the step, not inside one unit's report.

**S2 — camera still has the two defects this unit fixed, in two files.** A1 and
A2 above are properties of the copied code, not of notification:
`services/camera/src/operator/nats-object-event-sink.cc:140-141` latches its
`streamReady_` exactly as this sink did (so a wiped JetStream store stalls the
camera object feed, and the change subject it also covers, until a restart),
and `services/camera/src/camera/nats-camera-change-sink.cc:138` discards
`markSent`'s result exactly as this sink did. That file also carries the two
statement comments rule 20 forbids (`:105-107`, `:143-144`), copied into this
unit's sink and deleted from it under C1. This unit's files are now the checked
shape and camera's are the ones to bring up to it; doing it from here would
mean editing a service this step does not own, and 3a-2d/3a-2e will touch
neither file. It belongs to whoever closes 3a step 2.

**S3 — one drain cadence for all four producers.** Camera reads and settles one
row per 50 ms tick; notification reads a batch of 64 in one statement and
settles each row of it (C7). The mechanism is identical and the divergence is
the batch bound and the batched read, so it converges when S2's file is
touched. Guard's `guard_encounter_outbox` drains on its own rhythm too and
should be read at the same time.

**S4 — a change subject has no durable consumer.** The stream exists so the
publish can be acked (`js_Publish` has no core-NATS fallback), and `sync` reads
the wildcard over core NATS, so an event published while `argus-sync` restarts
is stored by the broker and never read. Making the change feed durable
end-to-end is a sync-side design (a durable consumer on each change stream,
reading by `Nats-Msg-Id` with the outbox as the reconciliation point), which is
step-level work and affects all four subjects at once.

**S5 — no retention policy for settled rows.** `sent` rows stay in
`change_outbox` for ever in both producers that have one. A daily purge of rows
older than the stream's own retention (7 days here) is the obvious shape, but
it must be the same decision in all four databases or an operator will find
four different lifetimes under the same table name.

**Known and accepted, with no action planned.** B3 (an unreachable stream
re-ensures and re-logs every tick, from `lib/nats`), B5 (the enqueue retry path
cannot be exercised under `drogon::sync_wait`), B6 (the 256 KiB door is
unreachable from notification's only producer), B7 (S1, above) and C8 (the
module's `shared/` home, which Phase 4 revisits) are recorded here so a later
reader finds them measured rather than rediscovered.

**S7 — the test suites' statement comments.** Rule 20 as written reaches into
test bodies, where the tree has used explanatory comments for a long time:
measured with `grep -rlE '^ {4,}//' --include='*-test.cc' packages services`,
57 of 132 `*-test.cc` files carry a comment indented into a function body, 518
such lines in all, including this unit's two suites (C2) and camera's. Deleting
only this unit's would make its suites the tree's odd ones out and leave the
rule violated in 55 other files, so the question is a sweep's, not a unit's:
settle it once, with the rule-20 reading the project wants for tests, at the
close of 3a step 2. (The review reported 92/132 on a wider reading that counts
any comment a helper class or namespace carries; the 4-space form above is the
reading rule 20 actually names — a comment attached to a statement — and the
one the sweep should use.)

**S8 — a drain tick can outlive Drogon's database manager.** `flush` reads the
outbox before any shutdown guard, and the worker ticks until the sink's
destructor, which runs after `app().run()` returns — while `quit()` has already
released `dbClientManagerPtr_` and `DbService::client()` is `app().getDbClient()`.
Read against the vendored framework: `HttpAppFrameworkImpl::quit()` queues a
loop callback that does `dbClientManagerPtr_.reset()` (`HttpAppFrameworkImpl.cc:1046`),
and `getDbClient()` is `dbClientManagerPtr_->getDbClient(name)` with no guard
(`:899-901`), so a tick landing in that window dereferences a released manager.
The window is small and was not reproduced; it is inherited verbatim from
camera's drain, whose read has the same order, so the fix — hold the
`DbClientPtr` in the drain so the client outlives the manager, or stop the
worker before the framework releases its members (C6) — belongs to both.

**S6 — the tidy baseline is two findings above the tree, and its `tus` floor is
stale.** `scripts/lib/tidy-baseline.txt` records `tus 481` and 3141 findings,
which predates 3a-2b: the tree it measures now is 487 TUs and 3139 findings,
so two checks sit below the baseline and the gate passes (measured, this unit's
final run). The two-finding drop is *not* attributable to this unit — the three
findings this unit fixed lived in files it also created, which the baseline
never counted — so rule 19's "bring the count down in the same change" does not
name this change. The write belongs where the per-check counts are attributable
and the step's files are all in: **at the close of 3a step 2**, once, with
`./scripts/check-tidy.sh --write-baseline` on the frozen tree.
