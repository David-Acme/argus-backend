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
  `ARGUS_IDENTITY_CHANGE`, `argus-sync-auth-action` on
  `ARGUS_AUTH_CHANGE` and `argus-sync-auth-session` on
  `ARGUS_AUTH_SESSION`), plus the delivery consumer. Each attaches with
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
  Each caller presents its own credential (`[rpc.callers]` `identity` and
  `notification`, paired with their `[sync] control_credential`), checked by
  `argus::client::FleetCallerGate`; see "Control callers" below.
- **Sessions.** Every socket keeps the `JwtContext` its upgrade produced, and
  that context now carries the session id the auth verdict returned, so a
  socket is tagged with its session for its whole life. argus-auth queues its
  session changes on `argus.auth.v1.session` (stream `ARGUS_AUTH_SESSION`),
  read by a seventh durable, `argus-sync-auth-session`, ordered like the other
  change feeds and handled by the same change dispatcher. A
  `disconnect_session` change (`user` and `session`, beside the per-user
  `disconnect`) walks that user's room on every IO loop, sends the frame only to
  the sockets of that session and then closes them (1008,
  `session_revoked`); the user's other sockets stay. `sessionsChanged` is an
  ordinary user emit. Measured on the sandbox: the revoked session's socket
  had its frame and its close 58 ms after the revocation was sent, while the
  user's two other sockets received `sessionsChanged` and stayed open. A
  socket opened through an auth that predates the session id carries an empty
  one and cannot be closed by session, so auth is upgraded first.
- **The voice leg.** `SyncForwarder` is the socket's `voice:*` + raw PCM path;
  the service installs `VoiceGrpcRelay` when `[voice] target` is set and leaves
  the forwarder null otherwise, which answers 503
  (`SyncErrors::VoiceUnavailable`) at `voice:start` instead of dropping frames
  silently. `voice:start` awaits the directory and the connect probe; a socket
  that closes during either is taken out of the session map before the stream
  exists, so the relay checks `closing` (and a stream a second start already
  opened) after the awaits instead of starting a stream nobody would finish —
  the observer and the session hold each other until the stream closes.
  `VoiceStart.identity` carries the socket's user, role and, since the passive
  voiceprints, the `device_hash` its `JwtFilter` bound, so argus-identity can
  tell a holder's own phone from a device several accounts share.
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
- **A resumed call.** `voice:start` with `{"resume": true}` (a JSON `true`;
  anything else is a new call) is sent as `VoiceStart.resume`: the app
  reconnected after losing its socket mid-call, and argus-voice continues
  without greeting again.
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

## Realtime calls: `POST /rtc/token` (2026-10-04)

Calls move to WebRTC through a self-hosted LiveKit SFU (`services/voice/CONTEXT.md`,
"Realtime calls over WebRTC"). The client needs a short-lived LiveKit room
token, and this service mints it: `POST /rtc/token`, filters `DeviceFilter →
ValidJsonFilter → JwtFilter → RoleFilter`, every role (`role_access::kRtcAccess`,
like the `/sync` voice leg, which has no role gate either).

**Why here and not in argus-voice or a new service.** The token is signaling,
and signaling is what the plan leaves on this service's surface. Minting
needs the authenticated, device-bound session (`JwtContext`: user, role,
session id, device hash), which this listener already establishes for
`/sync`; the call identity needs the directory lookup the voice relay already
does; revocation needs the `argus.auth.v1.session` feed, which this service
already consumes; and argus-voice must stay a pure gRPC service without
filters or an app-facing listener. A service of its own would duplicate the
TLS listener, the filter chain and the session consumer for one route. The
cost is the one exception to "no route but `/health` and the upgrade" in this
service's AGENTS.md, stated there.

**What it does, in order.** Validates the body (`callId` absent, `rtc-<32
hex>` or `call-<digits>`; `resume` a boolean; `mode` `duplex`/`half`),
refuses with 503 `RTC_UNAVAILABLE` when `[rtc]` is not configured (or still
holds a `CHANGE_ME` placeholder, or a secret under 32 bytes), when no voice
target exists, or when the session predates session ids; mints `rtc-<32 hex>`
for a new call; for `call-<id>` claims the call from argus-notification
(`CallService.ClaimCall`, idempotent per session) and maps TAKEN, EXPIRED and
NOT_FOUND to 409 `CALL_TAKEN`, 410 `CALL_EXPIRED`, 404 `CALL_NOT_FOUND`; then
asks argus-voice to join (`VoiceService.JoinRoom`, light blocking lane, 6 s
deadline) with the agent's own token, and only then mints the caller's token
and answers. A 200 therefore means the agent is already in the room.

- Room `u<userId>.<callId>`, so a resume can only name a room of the caller's
  own user id; participant identity `user:<userId>:<sessionId>`; agent
  `argus-voice` with `kind: agent`.
- Grants: the user may join exactly that room, publish only the microphone,
  subscribe and send data; the agent also updates its own attributes (the
  `lk.agent.state` the apps read). No admin grant leaves this service.
- `url` is `[rtc] public_url` or `wss://<hostname of the request's Host>:<[rtc]
  public_port>` (7046, the TLS front). The Host is checked to be a hostname or
  a bracketed IPv6 literal; anything else falls back to `argus.local`. The
  apps rewrite the host to the one they pinned and keep the port.
- Token lifetime `[rtc] token_ttl_seconds` (60 since 2026-10-05, clamped
  60-3600; it was 600): the token only has to be valid at connect; LiveKit
  refreshes a connected participant's token itself. LiveKit checks `exp` only
  at connect, so the lifetime is exactly how long a stolen or revoked token
  can still open a call. `expiresAt` is the `exp`.
- The room is created by this service (`RoomService.CreateRoom`, idempotent,
  so a resume finds the room it named) before argus-voice is asked to join,
  and LiveKit runs with `room.auto_create: false`
  (`argus-deploy/livekit.yaml.example`). With auto-create on, a revoked
  device could recreate a deleted room just by reconnecting with a token that
  had not expired yet; now only this service, after the whole checklist
  above, can bring a room into existence. CreateRoom needs the `roomCreate`
  grant, which only the admin token of `LiveKitRoomClient` carries; neither
  the agent's nor the caller's token gets it. A refused CreateRoom answers
  503 `RTC_UNAVAILABLE` and argus-voice is never asked to join.
- `rtc` is announced over mDNS with this service's other routes (it is a
  route segment of this listener); the LiveKit front is not announced.

**Revocation.** `sync_fan_out::onSessionEnd` is a listener the fan-out calls
after it has closed the sockets of a `disconnect_session` (one session) or a
`disconnect` (account disabled), on both transports (the NATS feed and the
control RPC). `RtcSessionRevoker` lists the rooms (`RoomService.ListRooms`
over LiveKit's Twirp API, a 60 s admin token), and for each room of that user
removes the session's participant (`RemoveParticipant`), or deletes the room
when the whole account went (`DeleteRoom`, which also sends the agent away).
It runs on the main loop, off the ordered feed's critical path. A failed
`ListRooms` is retried after 0.5 s, 1.5 s and 4 s before the revocation gives
up with a warning (before, one failure left every call of the revoked session
running).

Before the removal the user is told (David's request, 2026-10-04): the
revoker first revokes the participant's publish permissions
(`UpdateParticipant`, so mic and data stop at the SFU at once), then asks
argus-voice to say goodbye (`VoiceService.Farewell` with the cause the auth
session frame carries in `info.cause`, `accountDisabled` for an account
disconnect), and removes the participant or deletes the room when it
answers. The whole goodbye shares one 2.4 s budget from the revocation; with
argus-voice down or no line cached the removal follows at once. When the
revoked participant is the only human in the room (the agent aside), the
room is deleted instead of the participant removed: `RemoveParticipant` does
not stop a reconnect with the same token (nor with the one LiveKit refreshed
for it), while a deleted room cannot be reopened because nothing but this
service creates rooms. DeleteRoom
needs LiveKit's `roomCreate` grant (it answered 401 with `roomAdmin` alone,
measured). On the PCM path `RoomManager` asks the voice relay through a
`SocketFarewell` hook before it closes a revoked socket: when that socket has
a live voice call, the relay sends `VoiceFarewell`, ignores the socket's
frames, and the auth frame plus the close follow 2.3 s later
(`closeAfterFarewell`); a socket without a call closes as before. Stateless on
purpose: a restart of this service loses nothing, and LiveKit is the source of
truth for who is in which room. The app sees `Disconnected` with reason
`PARTICIPANT_REMOVED`, the agent ends the call as `revoked`.

The LiveKit key pair is `[rtc] api_key` / `api_secret`, generated per
installation by `setup.sh` (native) and `provision-host.sh` (deploy, which
also writes the 0600 `argus-deploy/livekit-keys.yaml` LiveKit reads through
`key_file`). It never reaches a client, a log or argus-voice.

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

**The four change feeds and the auth session feed are ordered consumers** (closure item 5b): each holds
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

Each event is written in one transaction (2026-10): a module audit's merge,
and every recipient row of a user audit, commit together or not at all, and
the Log frames go out only after the commit, so a client never receives an id
that was rolled back. A failure in the middle (the merge's delete, the fifth
recipient's insert) leaves the table as it was and the nak's redelivery
applies the event whole; before, the merge inserted and deleted in separate
autocommits and a failure between them left an older, redundant row behind
the merged one. The retention round (every survivor's rewrite, the removals
and the frontier) is one transaction for the same reason.
`tests/unit/change-feed-consumer-test.cc` forces the insert and the delete to
fail with a temporary trigger and pins that nothing is written.

**A message the fan-out cannot route is terminated, not retried** (2026-10).
An unknown `option` or `table_name` used to fall back to `user` and broadcast
into the user module room, which every Owner and Guard socket holds; it is now
refused, as is a `users` element that is not an integer, a role that is not a
string, an audit priority outside 0-2 and any jsoncpp type error inside the
handler. Those used to nak and hold the ordered feed for the full 151 s
described above, for a message no retry could ever write. Non-positive user
ids are dropped from a user emit (`userRoom(-999)` wrapped to the user module
room).

**Retention folds a row only into its immediate successor** (2026-10). The
sweep used to pair an old row with the nearest newer *old* row of the same
record, skipping a recent row between them whenever event timestamps ran out
of step with ids (a producer replay, a host that booted before NTP), so a
client whose cursor sat between the two applied the recent value and then the
older one folded on top of it. The pair is now the next row by id, and only
when both are old; the daily merge likewise merges only into the record's
newest row, and only when that row falls on the same UTC day.
`tests/unit/audit-compaction-test.cc` pins the old-recent-old case.

**The cursor queries seek instead of sorting.** Paging `audit_log` after an
id reads the primary key (`+table_name` keeps the planner off the
`(table_name, event_timestamp)` index that forced a sort of every row of the
role's tables per page and per `findLast`), `user_audit_log` pages through
`(user_id, id)` and `user_action_log` through `(created_at, id)`; the indexes
are additive and appear at the next boot.

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

## Grants: a scoped pull instead of a frame per row (2026-10)

A member added to a project while offline never received the project's
tasks: `Synchronize` pages creations by `created_at`, those tasks were created
before the member's cursor, and the grant pushed them only as live frames
(d07e59cd) that an offline socket never saw. The same held for a calendar
event shared with someone offline, and on revocation for the rows a removed
member kept.

The fix follows what local-first sync engines do for permission changes —
WatermelonDB's sync guide (a granted record must be reported as created, a
revoked one as deleted), PowerSync (a newly applicable bucket is downloaded
from its beginning, a removed one is dropped from the client) and Electric's
shape move-in/move-out — without breaking rule 18's creation-only stream:

- **The grant is the durable signal.** A `project_member` /
  `calendar_event_share` row naming the user is created at grant time, so it
  reaches an offline member through the ordinary creation pull and an online
  one as a live `Add`. Its tombstone reaches them through the deleted leg,
  whose scope includes the user's own grant rows.
- **The client pulls the scope.** On a grant for itself the app records the
  parent id (persisted, so a crash resumes it) and sends a scoped page:
  `{"project": {"requiredCreate": true, "scope": [14]}, "project_task":
  {"requiredCreate": true, "scope": [14]}}`. `SynchronizedBodyDto` accepts
  `scope` only with `requiredCreate`, only on `project`, `project_task` and
  `calendar_event` (a 422 otherwise) and at most `SyncLimits::kMaxScopeIds`
  (50) positive ids; `SyncFilter::scopeIds` carries it to argus-productivity
  as `TablePull.scope_ids`, whose RPC refuses the same cases, and the
  repositories page `id IN (...)` / `project_id IN (...)` by `(created_at,
  id)` from zero (`sync_query::buildScopedQuery`), still under the caller's
  owner-or-member scope — a scope never widens what a user may read.
- **Revocation is local.** When a grant row of the user's own disappears, the
  app drops every parent it neither owns nor holds a grant for, with its
  children (tasks, other grant rows). The sweep runs after each pull and after
  a live batch that deleted a grant row.
- **A grant costs one row, not one per child.** The project-member and
  calendar-event-share services no longer emit the parent and every task to
  the member (d07e59cd did, O(tasks) outbox rows per grant and per revoke);
  they emit the grant row to the owner and the member, and the member's app
  pulls what it now may read. That scales with many projects and members, and
  a member who is offline gets exactly what an online one gets.

Measured on the sandbox (throwaway residents 9301 owner / 9302 member, project
with three tasks): an incremental pull after the grant returned the membership
row and no task or project (the old gap), the scoped pull returned the project
and the three tasks, `scope` on `camera` answered `sync_error` 422, the revoke
reached the member's open socket as one `project_member` Delete, an offline
pull of the deleted leg returned that tombstone, and a scoped pull after the
revoke returned nothing.

The fresh bootstrap also pulls every grant's scope once more (the projection
paging may pass a parent's old rows before it meets a grant created
mid-bootstrap); that one-time re-download is the price of not missing them.

## Dead man's switch: the heartbeat (2026-10, WATCHDOG)

The owner's plan ("SAFETY", point 3): a phone must learn that Argus stopped
answering even though a stopped Argus can send nothing. The only thing that
can raise that alarm is the phone itself, so Argus sends a heartbeat and the
phone keeps a local notification scheduled `graceSeconds` ahead of the last
one it received; when the heartbeats stop, the notification fires.

**What is sent.** `SyncOperation::Heartbeat = 11` (option `user`, `10` is
RESPONSE's `response_update`), with `info {at, intervalSeconds,
graceSeconds, socketGraceSeconds, armed, presence, presenceSince, guard,
guardSeenAt}`, built by the pure `heartbeat::render`
(`src/feature/heartbeat/services/heartbeat-policy.cc`):

- on every socket: one right after `InitialInfo`, and one answering each
  `{type:"heartbeat"}` the client sends every `intervalSeconds` (60). The
  client asks instead of the server ticking a timer per socket: the answer
  proves the whole path (TLS listener, loop, directory lookup) is alive, the
  existing "a request unanswered for 10 s recycles the socket" rule already
  covers a hung server, and there is no per-connection state to keep;
- to a user's room the moment their presence changes
  (`argus.guard.v1.presence_changed`, PRESENCE's feed, `overall` field), so a
  phone that leaves home arms within a second instead of a minute;
- `GET /sync/heartbeat` (every role, `kSyncAccess`, own row only): the same
  payload for the app's background task, which has no socket;
- a push intent `type: heartbeat` (data only: empty title and body, `data
  {kind: heartbeat, ...the payload}`) to every armed user every
  `push_interval_seconds` (900), only when `[push] enabled`. It rides the
  existing `argus.notification.v1.push_intent` subject toward argus-relay;
  the device leg (APNs/FCM) does not exist yet (`services/tunnel/CONTEXT.md`),
  so today it is a hook the app already handles.

**Armed only while away.** `armed = presence == "away"`; `home` and
`unknown` (no consent, no row, presence service down, sync just booted and
the directory not read yet) never arm, so a missed reschedule cannot fire at
home and a server stopped on purpose while people are home raises nothing.
Presence is cached in `PresenceBoard`, filled from the core-NATS feed and
re-read from guard's `PresenceService.ListPresence` every `refill_seconds`
(300) off the loop, because the feed is at-most-once
(`argus::clients::guard`, `[guard] presence_target` / `presence_credential`,
the caller `sync` in guard's `[rpc.callers]`; an empty target skips the
refill and leaves the feed alone).

**Guard liveness.** The payload also carries `guard` (`alive` / `stale` /
`unknown`) from `argus.guard.v1.heartbeat`: sync answering while guard is
down is still "nobody is watching", which the app words differently.

**The numbers** (`[heartbeat]`, clamped in `SyncConfig::resolveHeartbeat`):

| Key | Default | Range | Why |
|---|---|---|---|
| `interval_seconds` | 60 | 15-300 | socket heartbeat cadence |
| `grace_minutes` | 45 | 15-720 | phone alarm delay; three push/background chances at 15 min, so one missed wake-up never alarms |
| `socket_grace_seconds` | 180 | 60-3600 | desktop/web: socket down this long shows the banner; a Wi-Fi blip or a restart (~10 s) does not |
| `guard_stale_seconds` | 90 | 30-3600 | guard heartbeat older than this is `stale` |
| `push_interval_seconds` | 900 | 300-3600 | iOS budgets background pushes at two or three an hour |
| `refill_seconds` | 300 | 30-3600 | presence re-read |

**Phone background limits (research).** iOS delivers background
(`content-available`) pushes at Apple's discretion, "not more than two or
three per hour", from a device-wide budget, and never to an app the user
force-quit; `BGAppRefreshTask` runs when the system decides. Android's
WorkManager (what `expo-background-task` uses) has a 15-minute floor and is
deferred to Doze maintenance windows; high-priority FCM data messages are
the only reliable wake-up. Hence: socket heartbeats while the app is open,
a 15-minute background task that calls `GET /sync/heartbeat`, data pushes
when the relay leg exists, and a 45-minute grace that tolerates two missed
wake-ups. A force-quit iOS app hears nothing and may alarm after 45 minutes
away; the notification says it has not heard from Argus since a time and
opens the app, which settles it at once. Sources: Apple, "Pushing background
updates to your App"; Expo `expo-background-task`; Android "Optimize for Doze
and App Standby".

Code: `src/feature/heartbeat/` (`argus::sync-heartbeat`: policy, board,
service, feed, `HeartbeatController`), the transport port
`feature/transport/infra/heartbeat-source.hxx`, `tests/unit/heartbeat-test.cc`.

## Audit of 2026-10-05: what changed and why

`docs/history/reports/cloud-audit-2026-10-05.md` findings 10, 26 (the sync
half), 52 (the sync half), 53, 54, 55, 56, 77, 78, 79 and 80, plus the
performance notes that name this service.

**The audit watermark (#10).** The compaction frontier is global per audit
table, but `findLast` answered the last id of the caller's own scope. A Guest
whose tables saw no change for longer than the window got a watermark below
the frontier, paged with it, received `ReplicaTooOld`, re-bootstrapped, got
the same watermark and looped. `watermarkId` is now
`max(last id in scope, frontier)`: every row of the caller's scope at or
below the frontier is already folded into a survivor the bootstrap delivered,
so starting after the frontier loses nothing. `lastSyncRecord` still names
the real last row. When the scope has no row at all the watermark stays
absent, as before, and `afterId = 0` remains the legal empty baseline. With
a frontier of 0 (a fresh install, the golden fixtures) the answer is byte for
byte what it was.

**Ordering ties (#77).** The four timestamp-paged audit queries of each table
order by `event_timestamp, id`, so a page boundary inside one millisecond can
neither skip nor repeat a row; the id-paged legs already sorted by id.

**Compaction pages by cursor.** A sweep keeps one cursor per table: each round
asks for pairs whose older row is above the last older id of the previous
round (`o.id > ?`, a primary-key seek), so a sweep reads the table once
instead of once per 200 pairs, and the round's "is this row folded away"
test is a hash set. Every row at or below the cursor is settled by the round
that passed it: a survivor below the cursor that still had an old successor
would have been one of that round's pairs.

**User audit in one round trip.** `UserAuditLogService::createMany` writes an
event's recipients with three statements whatever their number: one select
of each recipient's newest row of the record for that UTC day
(`user_id IN (...)`), one multi-row `INSERT ... RETURNING id, user_id`, one
delete of the superseded rows. The merge rule, the per-recipient row and the
per-recipient `Log` frame are unchanged (rule 21), and the whole event still
commits in one transaction.

**The `event` table is gone from this service (#80, #52).** Its repository
read `DbService::readOnlyClient()`, which nothing installs in production, so
the `event` leg of a `Synchronize` always answered empty, and its tombstone
paging had the same-second gap #52 describes. No owner serves `event` today
(camera keeps its own notion of events and never published this table), so
the repository and the two schemas were removed instead of fixed, and
`event` is answered with the node it always produced: `created: []`,
`deleted: []`, and `lastSyncDate: {}` when a `findLast` was asked. The wire
shape is unchanged; if `event` ever gets an owner it is paged through that
owner's client like the other domains.

**Parallel pulls.** A `Synchronize` frame naming several tables pulls them
four at a time (`kPullWidth`) instead of one after the other, so a bootstrap
costs about its slowest pulls rather than their sum; four keeps a frame from
holding more than four threads of the light blocking lane at once. The pulls
share no reference into a suspended frame (rule 18): their state is one
`shared_ptr` batch. Errors keep their old precedence: the first failing table
in field order is the refusal the client gets. Page rows are moved into the
answer instead of copied.

**One frame at a time per socket, and a rate limit (#56).** Every socket has
a `FrameLane`: its text frames run in arrival order, one after the other,
instead of one `async_run` each, so a client can no longer start thousands
of pulls at once, two frames of one socket never race on its `JwtContext`,
and the answers come back in the order they were asked. A token bucket of 120
frames refilled at 20 a second, and at most 64 frames waiting, bound what one
socket can queue; a frame beyond either answers
`{type:"<type>_error", status: 429}` with `SyncErrors::TooManyFrames`
(`TOO_MANY_REQUESTS`, an additive refusal: the app already treats any
`*_error` it does not know as a failed request). PCM stays on its own path.
Serial frames cost a heartbeat the time of a slow page ahead of it, which the
parallel pulls keep well inside the app's 10 s window.

**The socket follows its account without waiting for a frame (#53).** Before,
`refreshContext` corrected a socket only when its client sent something, and a
disabled account got 401 on each frame with the socket still open. Three
paths now:

- identity's catalog row for a `user` (`kind: identity`, on
  `argus.identity.v1.change`, written in the same transaction as the change,
  so it is durable even when the best-effort control RPC failed) makes this
  service forget the cached user and revalidate that user's sockets at once;
- every 60 s a sweep revalidates every connected user (one directory lookup
  per user, then one revalidation job per socket, which hits the cache);
- a frame that meets `UserAccountDisabled` now also closes its socket.

A revalidation runs inside the socket's lane, like a frame: a role change
moves the socket's module rooms and updates its context; a disabled or
deleted account leaves every room at once and the socket is closed (1008,
`account_disabled`), and the reconnect meets `ACCOUNT_DISABLED` at the
verdict. Identity being unreachable changes nothing.

**A short user cache.** `CachedUserDirectory` keeps a `GetUser` answer for
10 s (never an `Unavailable`), so the per-frame `refreshContext`, the voice
leg and `/rtc/token` stop costing one identity RPC each. The identity
catalog feed evicts a user the moment identity commits a change to them, and
an answer that was in flight across an eviction is not stored.

**Control actions only from their own feed (#26).** `disconnect_session` is
accepted only on `argus.auth.v1.session` (argus-auth's session outbox);
`disconnect` and `replace_role_rooms` are produced only by the control RPC
(identity calls it) and are refused on every NATS feed. A frame on the wrong
feed is terminated with a warning. Before, any producer able to publish on
its own change subject could have moved a socket into Owner rooms. The
emits, the audit diffs and the journal are unchanged.

**A payload that is not a JSON object is terminated (#78).**
`json_util::fromString` answers a null value for a body it cannot parse, but
jsoncpp still threw for one case, nesting past its stack limit, which is why
the first fix wrapped the call in a `try`. That left two holes: every other
caller of `fromString` (the socket, the other services' consumers) could
still throw on such a body, and a body that parses to something other than an
object (an array, a number, a string) reached the fan-out, where
`Json::Value::get` on an array throws, the handler nakked and the ordered
feed was held for its retries. Now `fromString` and `isValid` catch jsoncpp's
exception themselves and answer null / false (`packages/lib/text`,
`json-diff-test` pins the 5000-deep body), and both consumers check the parsed
value: anything that is not an object is terminated before the fan-out reads
a field. `change-feed-consumer-test` sends the deep body, arrays, numbers and
strings on every subject and expects `Term`; without the object check the
array cases throw. The socket path's `try` around the same call is gone with
the throw it guarded; `SocketFrameDto::fromJson` already refuses a
non-object.

**Shutdown drains (#79).** Next to the database freeze, `main.cc` registers a
drain for the socket lanes (the revalidation timer stops and every lane is
closed: a frame or a revalidation that arrives afterwards, on an existing
socket or one opened during the stop, answers `{type:"<type>_error",
status: 503}` with `SyncErrors::SyncStopping` instead of joining a queue, so
a chatty client can no longer hold the drain open until the quit gives up;
what was already queued still runs; drained when no lane is running), each change-feed consumer and the delivery consumer (they
unsubscribe; drained when every received message is settled, counted from
receipt so a queued one counts too), the heartbeat feed (drained when no
presence refill is in flight), the voice relay (every stream is finished;
drained when every stream has closed), the retention sweep and the control
RPC (a `Shutdown` with a 2 s deadline on its own thread, through
`argus::client::GrpcServerDrain` from `packages/lib/grpc`, the copy
notification and productivity share). A delivery settled
before the quit is not redelivered into a new UTC day, where it would have
merged into a second audit row.

**Presence refills never overwrite a newer event (#55).** The board numbers
every feed event it applies. A refill takes the number before it reads guard's
list and then applies by difference: a user the feed updated after that mark
keeps the feed's state, a user missing from the list leaves the board (they
withdrew consent or were deleted), and only the users whose state changed get
a frame. A `since` comparison was not used: guard's snapshot reports the
earliest `since` of the winning state across environments while the feed
carries the changed row's, so the two clocks are not comparable and the rule
would have refused the very refill that recovers a lost event.

**Guard and Guest see a camera without its connection details (#76).**
argus-camera already answers the `camera` pull of a Guard or a Guest with
`ip ""`, `port 0`, `username ""`, `cloudUsername ""` and `config "{}"`; the
live leg and the audit pages used to undo that by sending every socket of the
camera module room the full row and the full diff. The camera table now has
two module rooms: `moduleRoom(camera)` for the roles
`role_access::readsCameraConnection` admits (Owner, Resident) and
`reducedModuleRoom(camera)` (500 + the table) for the rest; `moduleRoomFor`
picks one at connect and on a role change (`moduleRoomsOf`, shared by the
connect and `replaceRoleRooms`). The fan-out sends a camera `Add` or `Log`
twice: as it came to the full room, and through
`camera_projection::reduceRow` / `reduceDiff`
(`packages/contracts/camera/src/camera/camera-row-projection.hxx`, the one
definition of the reduced field set) to the reduced room; a `Delete` carries
only the id and goes unchanged. `SynchronizeAuditLog` pages served to a role
outside `readsCameraConnection` drop the same fields (and any `config.*`
path) from every `camera` row's `changes`; the row and its id stay, so the
cursor is unchanged. `sync-surface-test` and `audit-sync-read-test` pin both.

**At most two calls per user at once (#54).** `/rtc/token` lists LiveKit's
rooms before it claims, creates or joins anything and counts the rooms named
with the caller's prefix (`u<userId>.`), the room it is asked for excluded so
a resume always passes, plus the caller's requests still in flight in this
process (two concurrent requests cannot both take the last slot). At
`[rtc] max_concurrent_calls` (2 by default, clamped 1-8) it answers 429
`TOO_MANY_REQUESTS` (`RtcErrors::TooManyCalls`, "Too many calls at once"),
and argus-voice is never asked to join, so a Guest in a loop can no longer
start one agent (LLM, STT and TTS) after another. LiveKit is the source of
truth for what is open: a room is deleted when its call ends or its last
human is revoked, and LiveKit closes an empty room by itself. A
`ListRooms` that fails answers 503 `RTC_UNAVAILABLE`, the answer the
`CreateRoom` that follows would have given. Without the LiveKit admin client
(`rooms` unset, a test or a half-configured install) no limit is applied.

**Not changed here.** The `person` pull's index belongs to identity's schema
(`person(created_at, id) WHERE user_id IS NOT NULL`). What remains of #54 is
argus-voice's: when a participant joins a room the agent should ask
argus-auth for the verdict of the session its identity names
(`user:<userId>:<sessionId>`) and leave a revoked one at once, instead of
relying on this service's revoker alone; and the agent's voice role is fixed
when it joins, so a role change mid-call takes effect only on the next call.
The revoker already deletes the room when the revoked participant was the
only human, which is what keeps a 60 s token from reopening it.

## Control callers (2026-10-05 audit, #25)

One `sync.control_secret` used to open the control RPC to any container
holding it, so a compromised camera could emit `AuthContextChanged` or a fake
`CallIncoming` to any user. Two peers call it, each with its own credential
in `[rpc.callers]`, paired with its `[sync] control_credential` by
`ensure_fleet_callers` (`scripts/lib/common.sh`):

| Method | identity | notification |
|---|---|---|
| `ReplaceRoleRooms`, `DisconnectUser` | yes | no |
| `EmitToUser` | any frame | `CallIncoming`, `CallCancel`, `ResponseUpdate` only |

The table is `src/app/rpc/sync-control-callers.hxx`; the operation check runs
before the request is validated, so a refused frame never reaches the
dispatcher. A known caller outside its rights gets `PERMISSION_DENIED`, a
missing or unknown credential `UNAUTHENTICATED`. The fleet secret of an older
install is accepted only while a caller is unpaired, and only for what that
caller may do; one WARN marks the first such call. `setup.sh` and
`provision-host.sh` pair both callers idempotently, after which the secret is
refused and may be deleted. `sync-control-callers-test` pins all of it.

## Selectable modules: `ModuleUpdate = 12` (2026-10, the modules plan)

`docs/history/plans/modules-and-welcome-plan.md` lets the Owner install,
disable, uninstall and purge `surveillance` and `productivity`. argus-settings
owns the state and publishes it on the durable subject
`argus.settings.v1.module` (stream `ARGUS_SETTINGS_MODULE`); this service
binds its own ordered durable `argus-sync-settings-module` beside the change
feeds and turns each event into frames. Nothing is persisted here.

**Rooms.** Every socket now also joins its **role room** (`400 + role`) and
the **connected room** (`450`) on connect. A role change moves the role room
with the module rooms (`roleRoomsOf`); the connected room is never left while
the socket lives.

**What the app receives** — always `{operation: 12, option: "user", info}`,
`option` carrying no meaning (dispatch on `operation`):

| Event on the subject | Frame | Room |
|---|---|---|
| top-level `module: {…}` (progress, lifecycle, a job's end; throttled by the producer) | `info` = the bare module JSON of the plan, unchanged | Owner role room |
| the same event when that module's `dataPurgedAt` is newer than the last one this process saw (integer or ISO string) | `info` = `{id, enabled, lifecycle, dataPurgedAt}` | every connected socket |
| top-level `modules: [{id, enabled, lifecycle?}]` (the enabled set changed) | `info` = `{modules: [{id, enabled, lifecycle?}]}`, every other field dropped | every connected socket |
| `settled: false` | an enabled set is not forwarded (a progress module still is) | — |

One event may carry both `module` and `modules`. The purge watermark lives in
memory, so after a restart the first purged module event reaches every socket
once more; the app compares `dataPurgedAt` with its own record and drops the
module's local tables only when it is newer, so the repeat is harmless. A
device that was offline learns it from `GET /modules` (plan, "Lifecycle").

**What the app must do**: paint its cached modules, call `GET /modules` on
open and after every reconnect (frames are live only: the durable starts at
new messages and nothing is replayed to a socket), then follow the frames;
treat any `lifecycle` other than `active` as not usable; drop a module's local
tables on a newer `dataPurgedAt`. After a purge the owners' pulls simply hold
no rows for those tables: no tombstones are sent for purged rows.

**Only the settings feed may speak it.** `parseEvent` refuses operation 12 on
every change subject and over the control RPC (`EmitToUser` included), so no
producer and no control caller can forge an enabled set into every socket; a
message on the settings subject that carries neither shape is terminated.
`tests/unit/sync-surface-test.cc` pins the rooms, the trimming, the purge
watermark and the refusal; `change-feed-consumer-test.cc` pins the feed and
its routing.

## Module change history: summary and purge (2026-10, the modules plan)

A module purge must not leave its rows' old values in argus-sync: the audit
diffs carry previous field values. argus-sync is therefore the last data owner
of `surveillance` and `productivity` in `modules.json` (`dataOwners` run in
order, so the owners have already deleted their rows when sync runs).
`src/feature/module-data/` (`argus::sync-module-data`) maps a module to its
synced tables (`surveillance`: `camera`, `camera_stream`, `zone`;
`productivity`: `project`, `project_task`, `project_member`,
`calendar_event`, `calendar_event_share`) and, in one transaction, deletes
their `audit_log`, `user_audit_log` and `user_action_log` rows and their
`audit_compaction_state` frontier. The summary is one item,
`change_history`, with an estimate of its bytes. `main.cc` serves it through
a `SettingsRpcService` (empty catalog) on the control listener (7041) when
`[rpc.callers] settings` is paired; that entry is taken out of the control
gate's caller table, so it never admits a control call.

Audit cursors survive the gap: clients page by id, and deleted ids are simply
not returned. Devices drop the module's local tables on the newer
`dataPurgedAt` of the `ModuleUpdate` frame. A change event an owner had
already published before its purge and that argus-sync consumes after its own
purge would write one audit row again; the window is the few seconds between
the owner's purge and sync's, and the next purge removes it.
`tests/unit/sync-module-data-test.cc` pins the table scope, the rollback and
the idempotent retry.

## Activity history: `GET /sync/activity` (2026-10, the roles-and-tools wave)

`user_action_log` is the household's action journal (who read, changed, created
or deleted what), and with modules it needs to be read by module, by person and
by time. Three additive changes carry it:

- **A `module` column** (`TEXT NOT NULL DEFAULT ''`). The writer sets it:
  `AuditFanOut::insertAction` takes the event's own `module` (a module action
  names the module it concerns) or derives it from the table through
  `shared/vocabulary/module-tables.hxx` (`camera`, `camera_stream`, `zone` are
  `surveillance`; `project`, `project_task`, `project_member`, `calendar_event`,
  `calendar_event_share` are `productivity`; everything else is `core`). The
  same header answers `SyncModuleData`'s tables of a module, so purge and
  activity cannot disagree. `UserActionLogRepository::migrateLegacySchema` adds
  the column to an existing table before `schema.sql` runs (the indexes need
  it), and `backfillModules` fills the rows that still have `''` once the
  schema is applied (an old journal, or rows copied by `argus-migrate-sync`,
  whose column check tolerates a source without `module`).
- **A table subject for actions that touch no table.** `UserActionEvent`
  (`argus::contracts::sync`) gains `module` and `subject`; `subject` is
  `"module"` today and is the text stored in `table_name`, because `TableName`
  is the frozen, wire-visible list of synced tables and a module is not one of
  them. A `table_name` that is neither a known table nor `module` makes the
  event malformed (Term), where it used to become `user` silently. argus-settings
  publishes its module actions on `argus.settings.v1.user-action` (stream
  `ARGUS_SETTINGS_ACTION`), consumed by the durable `argus-sync-settings-action`
  and written by the same `handleActionPayload` as the identity and auth ones.
- **Three composite indexes** for the filters, each ending in `(created_at, id)`
  so the newest-first order never needs a temporary b-tree:
  `(module, created_at, id)`, `(user_id, created_at, id)` and
  `(table_name, created_at, id)`; the date range and the unfiltered page use the
  existing `(created_at, id)` index. `sync-activity-test` reads the query plan
  of each shape and refuses a temp b-tree (rule 22).

`GET /sync/activity` (`src/feature/activity/`, argus-sync's TLS listener,
`DeviceFilter -> JwtFilter -> RoleFilter`) is Owner only: `kSyncAccess` names
only `/sync/heartbeat`, so every other role is refused by the table, and the
Owner passes. Query: `module`, `userId`, `action` (`create`, `read`, `update`,
`delete`), `table` (a table name, or `module`), `from` and `to` (unix seconds,
inclusive), `limit` (default 50, clamped to 200) and `cursor`. Answer:
`{ items: [{ id, userId, recordId, table, module, action, oldData, newData,
ipAddress, createdAt }], nextCursor }`, newest first. The cursor is
`<createdAt>-<id>` and the page is a keyset (`created_at < ? OR (created_at = ?
AND id < ?)`), so the volume never costs an offset scan; the service reads one
row more than the limit to know whether there is a next page, and a cursor that
this endpoint did not issue is a 422. Names are not joined (sync cannot read
identity); the app maps `userId` through its own user table. Module actions are
rows with `table: "module"`, `recordId: 0` and the module id in `module`.

## Live user context: `InitialInfo.context` and `ContextUpdate = 13` (2026-10, the context plan)

`docs/history/plans/context-roles-tools-quality-plan.md`, section 1. The app
reads what a user may use from the socket alone: no `GET /modules` polling,
actions stay HTTP. argus-sync builds the context from its own module gate
(`module_gate::install({.service = "sync"})`: the boot read over
`Modules/ModuleStates` with `[modules] target/credential`, the durable
`argus-sync-modules` and the last-known-state file, like every service) and the
user's role.

**The context** (`src/shared/services/context/user-context.{hxx,cc}`):

```json
{ "userId": 7, "role": "guard", "roleActive": false,
  "capabilities": ["safety.panic", "reminders.read", ...],
  "roles": [ { "id": "owner", "module": "core", "active": true },
             { "id": "guard", "module": "surveillance", "active": false }, ... ],
  "modules": [ { "id": "surveillance", "kind": "available", "name": { "es", "en" },
                 "summary": { "es", "en" },
                 "intro": { "es": { "what", "examples": [..] }, "en": { ... } },
                 "roles": ["guard"], "enabled": false, "lifecycle": "disabled",
                 "dataPurgedAt": null }, ... ],
  "ownerCatalog": [ ...the Owner's module JSON, only for the Owner... ] }
```

`InitialInfo` carries it as `info.context` beside `id`, `role` and `isActive`;
`ContextUpdate` (`context_update`, `{operation: 13, option: "user", info}`) sends
the bare context. Texts come in both languages so the app picks one without a
round trip and a cached context paints in either. `kind` (`core`, `available` or
`coming_soon`, from the catalog) lets the app refuse to ask the Owner for a
module that cannot be enabled yet. `roles` lists the roles the
household can assign, so a picker offers only those of an active module. A role
this build does not know gets `role: "unknown"`, `roleActive: false` and no
capabilities, and joins no module room. An inactive-role user keeps the socket,
the user room and the context.

**When it is sent.** `UserContextService::modulesChanged()` (the gate's
`onStateChange`) reaches every connected socket, one frame each, whenever any
field of any module changes; `userChanged(userId, role)` reaches one user's
sockets after a role change, from the control RPC's `replace_role_rooms` and
from a revalidation that finds the role changed. Delivery runs on each socket's
own loop (`RoomManager::forEachConnection`, `forEachUserConnection`), moves the
socket's role and module rooms to what its role and the active modules allow
(`reconcileRoleRooms`: module rooms of an inactive module are left, the others
joined, user and connected rooms untouched) and then sends the frame, so a
module that goes off stops its live frames and one that comes back resumes
them. `ModuleUpdate = 12` is unchanged and keeps carrying install progress to
the Owner.

**The Owner's catalog** is not kept here: argus-sync asks `Modules/OwnerCatalog`
(1.5 s) when an Owner connects and once per change, and leaves `ownerCatalog`
out when settings does not answer; the app keeps its last one.

**Only this module speaks it.** `parseEvent` refuses operation 13 on every
feed and over the control RPC, like 12.

**Sync pulls and the audit** read the same gate: `tableReadable` answers `null`
for a table of an inactive module or beyond an inactive role's baseline, and
`auditTablesFor(role, modules)` leaves those tables, and every own-row table,
out of the global audit pages. `reminder` and `reminder_detail` (own-row, core)
never get a module room or a global audit page for any role, the Owner
included: a reminder change reaches the target's own room only.
`tests/unit/sync-context-test.cc` pins the context, the frames, the room moves,
the pulls and the refusal.
