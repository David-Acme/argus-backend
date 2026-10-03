# argus-sync — CONTEXT

## Why the sync service exists

Phase 3a row 1 of `docs/history/plans/architecture-plan.md`: the gateway's sync
surface — `/sync`, the rooms, the fan-out, the audit persistence, the action
journal and the control RPC — becomes its own service, with `contracts/sync`
and `clients/sync` created around it. The model the app relies on is the reason
the surface is a service at all: the client paints persisted local data first
and then reacts to live sync, so the transport, the audit trail and the room
routing are the product's state machine, not a gateway feature. A service owns
them the way a service owns a domain.

Sub-step 3a-1c created this binary and flipped who serves `/sync`. It composed
the engine as it then stood, still a package (`packages/sync`, `argus_sync`);
sub-step 3a-1d inlined it here — its features, repositories, schemas and
services are modules of this service (rule 23) — and deleted the package. The
split was deliberate: c changed *behaviour* (who persists, who serves, who
reaches live connections), d changed only file placement, so the WebSocket
protocol could not regress by accident in the commit that changed the endpoint.

## What it owns

- **The WebSocket surface.** `/sync`, TLS, port 7025, filter chain
  `DeviceFilter` + `JwtFilter` — device binding is enforced on this transport
  exactly as on HTTP, and there is no third filter: the route has no roles to
  check. Requests are `{type, payload}`; responses are
  `{operation, option, info}` (`SocketEmitDto`); errors are
  `{type:"<type>_error", status, error}`.
- **The rooms.** `RoomManager` (per module and per user, file-level
  `thread_local`) and the lifecycle object `main.cc` holds for as long as the
  process runs. Live frames reach connections through it; the audit *rows*
  never do — a client reads the journal through its own `Synchronize` page.
  A socket joins the module rooms of `role_access::moduleTables(role)`, not
  of every readable table: Resident and Guest read only their own `user`
  row (rule 7b), and the identity enrollment's `Add` for a new user is a
  module emit, so joining them to the `user` room broadcast every new
  user's row to them. Their own row arrives through the scoped pull and
  their own user room; the global audit page uses the same table set.
- **The audit trail and the journal.** Five tables, one schema file
  (`database/schema.sql`): `audit_log` (module/global field diffs),
  `user_audit_log` (recipient-scoped diffs), `user_action_log` (the §3.5 action
  journal), `notification_delivery_inbox` (durable delivery receipts) and
  `audit_compaction_state` (the retention frontier, one row per audit table).
  This service is their only writer. The two audit tables are convergent by
  construction — a redelivery merges into the row's `(record, table, UTC day)`
  key and advances its id, so a burst of changes to one record settles on a
  single row per day — while the
  journal is append-only and non-convergent, so `user_action_log.msg_id` holds
  the producer's `Nats-Msg-Id` — `identity-action:` plus 32 hex the producer
  mints at enqueue, opaque so it survives the split (the journal's row ids now
  come from this file, not from the producer's) — under a partial unique index
  and the insert is `INSERT OR IGNORE`: a redelivered journal row is ignored
  instead of doubled. Rows the producer enqueued before the minting flush under
  the row-derived `identity-action:<id>` the older builds published, which the
  index accepts just the same.
- **The retention window (D15).** `sync` compacts audit rows older than
  `[sync] audit_retention_days` (90 days, `audit_retention::kDefaultDays`). A
  sweep runs ~30 s after boot and every 24 h: it pairs each old row with the
  nearest newer *old* row of the same key, folds the older diff into the newer
  one (the chained `JsonDiff::compareChanges` the daily coalesce uses, applied
  in ascending id order; only `changes` is written, so the survivor keeps its
  own priority and timestamp and the folded row's are dropped with it), deletes
  the older row and advances the frontier — the highest deleted id —
  monotonically through `audit_compaction_state`. Pairwise merging is what makes the sweep always
  progress: a window of the oldest rows would clog with lone survivors and
  never reach the long-lived records compaction exists for. Bounded work per
  round (200 pairs, the sync page size), repeated until a round deletes
  nothing. A **recent** row (younger than the cutoff) is never a candidate and
  never a partner, so a live write is out of the sweep's reach and a sweep can
  suspend as often as it needs to; only a producer's replay of an event older
  than the window can land a row the sweep may select, which costs that round a
  retry and nothing else. The summary carries its fold under an id above every
  row it supersedes, so a replica that paged past the survivor is unaffected; a
  cursor *older* than the frontier is refused as the window's declared boundary
  — it would still reach each record's current value, on merged diffs rather
  than on each individual change — and the audit legs answer
  `SyncErrors::ReplicaTooOld` (409, frozen `CONFLICT`), which the app handles by
  re-bootstrapping with a full `Synchronize` — `afterId = 0` stays the legal
  empty baseline so a fresh client still fills history. The window is read once
  at boot (`start` ignores a second call, so nothing can re-arm the sweep with
  a different window). The semantics are declared in
  `contracts/sync` (`audit-retention.hxx` + the refusal) and in
  `docs/architecture/wire-sync-tables.md`.
- **The fan-out.** One NATS **durable JetStream consumer per change stream**
  (`change_feed::defaults()`: `argus-sync-camera` on `ARGUS_CAMERA`,
  `argus-sync-notification`, `argus-sync-productivity`, `argus-sync-identity`
  and `argus-sync-identity-action`, the last two both on
  `ARGUS_IDENTITY_CHANGE`, and `argus-sync-auth-action` on
  `ARGUS_AUTH_CHANGE`), plus the delivery consumer. Each attaches with
  `deliverAll = false` — deliver-new, so the first boot after this landed
  cannot replay a week of already-recorded changes into duplicate audit rows —
  settles `durable_delivery`'s three dispositions (ack, nak on a throw, term on
  a payload it will never parse) and retries its attach every 5 s while its
  stream does not exist yet. Deliver-new is only safe because every producer's
  outbox worker creates its stream as soon as its bus connects, not on its
  first publish: when the stream was created by the first change itself, that
  change landed before this durable existed and never reached the audit tables
  or the clients (measured on a fresh sandbox: the first calendar event of a
  new install was missing from every open app until its next bootstrap). An audit frame persists
  first and fans out the DB-assigned row (Ruling Y); a journal row is inserted
  verbatim, keyed by its `Nats-Msg-Id` so a redelivery is ignored; an emit
  frame becomes a room emit; an identity catalog frame belongs
  to the memory replicas and is acked, not re-emitted.
- **The control plane.** `argus.sync.v1.SyncControlService` on 7041, served
  from `src/app/rpc/`: `ReplaceRoleRooms`, `DisconnectUser`, `EmitToUser`. The
  three operations change no row, so they cannot travel as a change event; each
  typed frame is rebuilt into the change feed's envelope and handed to the same
  dispatcher the NATS leg uses, so the two transports cannot diverge.
- **The voice leg.** `SyncForwarder` is the socket's `voice:*` + raw PCM path;
  the service installs `VoiceGrpcRelay` when `[voice] target` is set and leaves
  the forwarder null otherwise, which answers 503
  (`SyncErrors::VoiceUnavailable`) at `voice:start` instead of dropping frames
  silently. `voice:start` awaits the directory and the connect probe; a socket
  that closes during either is taken out of the session map before the stream
  exists, so the relay checks `closing` (and a stream a second start already
  opened) after the awaits instead of starting a stream nobody would finish —
  the observer and the session hold each other until the stream closes.
- **One stream, many calls.** The gRPC stream outlives a call: `voice:stop`
  ends the session in argus-voice but leaves the stream open, so a later
  `voice:start` on the same socket is sent as a new `VoiceStart` on that
  stream (argus-voice starts a new session once the previous one stopped).
  The relay used to drop it because a stream existed, so every call after the
  first on one socket - the retry button, a second call from the dashboard -
  started nothing and the app waited on a silent call.
- **Frames that race the start.** The app sends its first `voice:context`
  notes right after `voice:start`, while the relay is still awaiting the
  directory and the connect probe. The relay marks the session `starting`
  before those awaits and queues what arrives meanwhile (context, action
  results, mute, skip, stop; at most 16) to flush, in order, right after the
  start frame; before, the first note of a call (the camera names) was
  dropped and the model never knew the cameras. PCM is not queued: the call
  greets first, so the first few hundred milliseconds of microphone carry no
  turn. The session's stream, its `starting` flag and the queue sit behind
  one mutex, because `forwardText` resumes on Drogon's main loop after its
  awaits while the stream observer and the binary path run on the socket's
  loop.
- **A voice failure is the call's, not the socket's.** When the voice stream
  closes with an error the relay sends `voice:start_error` (503, `Voice
  unavailable`) - the app's router hands every `voice:*_error` to its voice
  error listeners - and forgets the stream, so the next `voice:start` dials
  again. It used to shut the whole `/sync` socket down, which also dropped
  sync and every live emit until the app reconnected.
- **The voice start mode.** `voice:start` may carry `{"mode":"duplex"}`; the
  relay reads it with `VoiceGrpcRelay::startModeOf` and sends it as
  `VoiceStart.mode` (`VOICE_MODE_DUPLEX`). Anything else - no payload, a
  payload that is not an object, a missing, non-string or unknown `mode`,
  even `"DUPLEX"` - is `VOICE_MODE_HALF_DUPLEX`, the proto default, so the
  current app, which sends `voice:start` with no payload, starts exactly the
  session it always did. The parse is strict on purpose: a typo must fall
  back to the frozen behaviour, never into a mode the app cannot handle.
  `renderServerFrame` renders the two duplex frames the same way as the
  others: `VoiceTurn` as `{"type":"voice:turn","payload":{"id":N}}` and
  `VoiceInterrupted` as `{"type":"voice:interrupted","payload":{"id":N}}`.
  `voice:assistant` gains `payload.turnId` only when the frame carries a
  non-zero `turn_id`, which only a duplex session sets, so a half-duplex
  `voice:assistant` stays `{"text":...}` byte for byte.

## Where the state lives

Its own file, `database/sync.db`, resolved from `[sync] db` — the deploy binds
this service's own data directory in, and no container sees another owner's
database. The four identity tables' DDL moved here verbatim (they were
`services/identity`'s schema) and `audit_compaction_state` is this service's
own; between Phase 3c-1 and Phase 3c-2 those five tables were applied onto
identity's `identity.db`, which is why `argus-migrate-sync` exists and why the
audit tables' `REFERENCES user(id)` clauses are gone: a foreign key into a
table this owner does not declare cannot even be prepared in its own file, so
the split trades the cross-owner cascade for rule 27's shape — a deleted user
leaves the audit rows that recorded them, which is what an audit trail is for.
The copy is row-count and checksum verified over exactly the keys the run
copied, so a re-run against a live target neither copies nor re-verifies rows
it already moved, and the documented rollback is the same tool with the source
and target swapped (`sync-init` forward, `sync-rollback` back).

The schema is applied at every boot (`registerBeginningAdvice` →
`DbService::runScriptFile`) and `applyPragmas()` follows it, so a fresh
container is self-sufficient. `/health` needs the TLS certs, which the compose
mounts read-only.

`user_action_log.msg_id` and its partial unique index are the one additive
change to a table that existed without them. `CREATE TABLE IF NOT EXISTS` does
not widen a live table and the index over the new column is refused by a
database that predates it, so the beginning advice first runs
`AuditFanOut::migrateLegacySchema()`: when the table exists without the column
it adds it (`ALTER TABLE ... ADD COLUMN msg_id TEXT NOT NULL DEFAULT ''`), the
same guarded shape camera, notification and guard use for their additive
columns. The rows that predate the column keep `msg_id = ''`, which the partial
index does not constrain.

The consumers attach inside that same advice, after the migration, the schema
and the pragmas. Two reasons, both measured: drogon creates its IO loops only
inside `run()` (`getIOLoop` is null before), and a durable with a backlog
delivers its first message within a millisecond of the bind, so a bind before
`run()` crashed the process on the first delivery — and, the message never
acked, on every restart after it. The advice is also the first moment the
schema exists, so a backlog never meets a missing table.

## How a change reaches a client

Producers never write these tables. A domain service publishes on its own
subject (`argus.<domain>.v1.change`); identity's journal rows travel
`argus.identity.v1.user-action`, a subject of their own because the journal is
inserted verbatim rather than parsed as an emit. This service subscribes,
persists, and fans out. The imperative frames — a role change, a disconnect, an
emit to one user — reach the same dispatcher through the control RPC instead.
The subscriptions are durables, and a durable here is a broker-side object the
bus creates once and then binds to, so a teardown unsubscribes the socket
without deleting the consumer: a change published while this service was down
drains from the stored cursor when it returns, and an in-process reconnect
re-attaches through the bus's pending list. Deliver-new is therefore safe in
both directions — it decides where the cursor starts, and only at creation: the
broker refuses to change an existing consumer's policy and the attach keeps
failing with that reason in the log, so a feed whose policy changes takes a new
durable name. A stream that does not exist yet at boot (a producer that has
never run) is what the consumer's own 5 s retry timer covers.
`tests/unit/change-feed-live-test.cc` pins the whole property against a real
broker: it detaches the consumer, publishes, re-attaches, and requires the
change to be applied.

**Each feed is applied one message at a time, in arrival order.**
`durable_delivery::handler` queues every delivery of its feed on IOLoop 0 and
runs the next only after the previous one has settled. The audit writers merge
into a `(record, table, UTC day)` row through a read, a delete and an insert
that are separate statements, so two changes to one record applied together
both read the same row and both insert: measured, a 20-change burst for one
record left 17 rows and a merged diff ending at the third value instead of the
twentieth. A durable makes that burst the normal case — the backlog a restart
drains is exactly a run of changes queued while nobody applied them — and
`tests/unit/change-feed-consumer-test.cc` pins the burst to one row carrying
the first previous and the last current value. Room emits need no loop of
their own: `RoomManager` posts every emit onto each IO loop itself. The one
object the consumers share is the `AuditFanOut` `main.cc` owns; the fan-out
itself is a free-function namespace because dispatching holds no state of its
own.

**The four change feeds are ordered consumers** (closure item 5b): each holds
one unacknowledged message at a time (`NatsBus::kOrderedMaxAckPending`, the
feed's `maxAckPending` in `change_feed::defaults()`), so the broker delivers
nothing behind a message until it is acked or given up on. A message that is
nak'd, whose ack never arrived or whose ack window expired is therefore
redelivered before anything after it — measured on the dev broker, a nak'd `a`
of `a b c` is applied `a a b c`, where 256 in flight gives `a b c a` — and the
audit merge can no longer fold an older diff into a row that already holds a
newer value. The two action journals are the fifth and sixth feeds and keep
256 in flight: they are keyed by `msg_id` and inserted verbatim, so order buys
them nothing, the same reason the delivery consumer keeps 256. The serial
queue above stays: the journals and the delivery consumer rely on it.

Three costs come with it, each chosen over a wrong value on a client:

- **Throughput is one round trip per message** — measured at about a thousand
  a second on a local broker with an empty apply, so a day's backlog drains in
  seconds.
- **A failing message holds its feed.** A nak waits before the redelivery,
  1 s doubling to 30 s (the bus's backoff, not the handler's), and each feed
  allows 10 deliveries; the nak of the last one does not wait. A message that
  can never be written therefore stalls its domain's live changes for about two
  and a half minutes (151 s); the bus logs an error with that last nak, the
  broker drops the message, and the feed moves on. Before item 5 such a message
  was lost on the first failure. A last delivery that ends by an expired ack
  window instead of a nak is dropped without that log.
- **A delivery lost while its subscription survives waits out the 60 s ack
  window** — a message in flight when the connection drops and cnats reconnects
  on the same subscription. A process that stops uncleanly does not wait: its
  successor re-attaches through a fresh deliver subject and the broker
  redelivers the in-flight message at once, ahead of the rest.

The merge itself inserts the merged row before it deletes the one it replaces,
and finds the row to merge into newest first. A failure between the two
statements therefore leaves an older, redundant row behind the merged one —
a client pages both in id order and ends on the merged value — instead of
deleting the day's history, which is what the old delete-then-insert order did.
`tests/unit/change-feed-consumer-test.cc` forces each statement to fail with a
temporary trigger and pins both outcomes.

**NATS is load-bearing for audit and journal persistence.** A deployment with
no `[nats] url` loses live fan-out *and* the audit writes, because the writes
are the fan-out's first act. The producers' durable outbox that removes this
dependency is row 2's; until it lands the dependency is documented rather than
hidden.

The delivery leg kept the gateway's shape with one deliberate change: the
durable name is `argus-sync-delivery`, not `argus-gateway-delivery`, so the new
consumer starts with its own cursor instead of inheriting the gateway's
unacknowledged backlog position. Receipts come first — the inbox insert wins
the dispatch lease, a `dispatched` row drops redeliveries, a `received` row
replays them, and a conflicting fingerprint is never dispatched.

## The producers' wire, as built

The design in the report had `UserChangeSink` gain `emitModule` plus an
`identitySink()` slot. The execution deviated, and the deviation is the better
shape: identity's outbound wire is **one five-method `IdentityChangeSink`**
(`publishCatalog`, `emitModule`, `publishModuleAudit`, `publishUsersAudit`,
`publishAction`) declared in `contracts/sync`, with the `NatsIdentityChangeSink`
implementation and the catalog replica payload living in the identity owner's
own feature tree (`services/identity/src/feature/user/services/`).
Productivity's and notification's sinks are untouched — they never needed a
sixth slot, and identity needed a namespace of its own.

Two properties that shape the code: a domain's features hold *interfaces*,
never the client — that is what the sink declarations in `contracts/sync` are —
so identity's features stay linkable into a process with no control channel
(its suites); and each host's `main.cc` constructs and installs the funnel at
boot. `services/identity`'s `main.cc` is now one of those hosts: the identity
RPC listener that publishes belongs to argus-identity, while the socket
fan-out, the delivery consumer and the `RoomManager` lifecycle moved here.

## Why the voice relay came along

The relay is a `SyncForwarder`: it exists only to serve the `/sync` socket. Had
it stayed in the gateway it would have been dead code (nothing there held a
socket any more) and a dead app feature in the window between this sub-step and
the gateway's deletion in Phase 3d. It moved with its socket, and its two
refusals became `SyncErrors::VoiceUnavailable`; the `[voice]` block moved to
`services/sync/config.toml.example` and the deploy example.

## The endpoint, and what the app must do

`/sync` is TLS on `7025`, published on all interfaces: the app dials the socket
directly. The endpoint is configuration until Phase 3d step 5 moves discovery,
so the frontend's switch to 7025 is one coordinated change — the same commit
that makes the surface complete, which is why this sub-step moves the relay,
the audit writers and the fan-out together rather than in pieces.

## What this service must not do

No auth surface of its own, no route other than `/health` and the WS upgrade,
no second writer for any table it owns, no reading of another service's
database (the four pull sources call their own gRPC contracts), no AI capacity
and no in-process emit that the control plane cannot also perform.

## Compose volume

`config.sync.toml` read-only as `config.toml`; the certs directory read-only;
this service's own data directory read-write as `database/` (`sync.db`);
`services/sync/database/schema.sql` read-only beside it. State lives on the
host and is bind-mounted, so updating is a rebuild plus `docker compose up -d`.
The `sync-init` profile runs `argus-migrate-sync` over those two directories
with the stack stopped: it is the only writer of `sync.db` besides this service
and reads `identity.db` read-only through an `ATTACH`. An upgrade from an
install whose audit rows still live in `identity.db` must run it *before* this
service starts: without the profile the service applies its schema to an empty
`sync.db` and serves an empty audit history, which every client can only answer
with a full re-bootstrap. `sync-rollback` is the mirror (`--profile
sync-rollback`): the same tool with the paths swapped, so it mounts `sync/`
read-only and identity's directory writable. The forward service cannot be run
backwards — identity's directory is read-only there, so a swapped invocation
could not create its target — and the rollback mounts `sync/` read-only for the
same reason in the other direction.

## Voice app actions and context

`voice:action` (server to app) carries `{id, name, arguments}` with the
arguments parsed into an object (anything unparsable becomes `{}`).
`voice:context` (app to server) takes `{kind: "note" | "cameraEvent" |
"situation", text, camera}`, maps any other kind to a note and cuts each
string at 300 characters (the situation's text at 900, since it carries the
guard mode, the day's agenda and recent camera events together) before it
reaches argus-voice. Cuts land on a UTF-8 character boundary: a byte cut in
the middle of "á" made the protobuf string invalid, and the receiving side
refuses to parse such a frame.

`voice:action_result` (app to server) reports what the app did with an
action: `{id, ok, detail}`, `id` a number or a numeric string, `ok` true only
for a JSON `true`, `detail` cut at 160 characters. `voice:mute`
`{muted: true}` tells argus-voice the microphone is muted, so it drops the
half-said utterance instead of finishing it after the unmute; any other
payload is unmute.
