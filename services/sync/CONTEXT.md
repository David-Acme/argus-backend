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
- **The audit trail and the journal.** Four tables, one schema file
  (`database/schema.sql`): `audit_log` (module/global field diffs),
  `user_audit_log` (recipient-scoped diffs), `user_action_log` (the §3.5 action
  journal) and `notification_delivery_inbox` (durable delivery receipts).
  This service is their only writer. The two audit tables are convergent by
  construction — a redelivery merges into the row's `(record, table, UTC day)`
  key and advances its id, the same compaction the daily job runs — while the
  journal is append-only and non-convergent, so `user_action_log.msg_id` holds
  the producer's `Nats-Msg-Id` under a partial unique index and the insert is
  `INSERT OR IGNORE`: a redelivered journal row is ignored instead of doubled.
- **The fan-out.** One NATS **durable JetStream consumer per change stream**
  (`change_feed::defaults()`: `argus-sync-camera` on `ARGUS_CAMERA`,
  `argus-sync-notification`, `argus-sync-productivity`, `argus-sync-identity`
  and `argus-sync-identity-action`, the last two both on
  `ARGUS_IDENTITY_CHANGE`), plus the delivery consumer. Each attaches with
  `deliverAll = false` — deliver-new, so the first boot after this landed
  cannot replay a week of already-recorded changes into duplicate audit rows —
  settles `durable_delivery`'s three dispositions (ack, nak on a throw, term on
  a payload it will never parse) and retries its attach every 5 s while its
  stream does not exist yet. An audit frame persists
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
  silently.

## Where the state lives, today

Identity's file. The four tables' DDL moved here verbatim (they were
`packages/identity`'s schema), and `[sync] db` still points at
`database/identity.db`; Phase 3c-2 splits them into `sync.db` and the key
changes with it. Two consequences are accepted for now: the audit tables' `REFERENCES user(id)` foreign keys
target a table this owner does not declare, and the deploy binds identity's
data directory into this container. Both are the transitory price of moving the
writer before splitting the file; rule 27's shape resumes in 3c-2.

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
columns. A reset is not the remedy here: until Phase 3c-2 this file is
identity's, and resetting it would take users, persons and faces with it. The
rows that predate the column keep `msg_id = ''`, which the partial index does
not constrain.

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
newer value. The action journal is the fifth feed and keeps 256 in flight: it
is keyed by `msg_id` and inserted verbatim, so order buys it nothing, the same
reason the delivery consumer keeps 256. The serial queue above stays: those
two rely on it.

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

The delivery leg keeps the gateway's shape with one deliberate change: the
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
implementation and the catalog replica payload moved into
`packages/identity`'s own feature tree. Productivity's and notification's sinks
are untouched — they never needed a sixth slot, and identity needed a namespace
of its own.

Two properties that shape the code: identity holds *interfaces*, never the
client, so `packages/identity` stays linkable into a process with no control
channel (its suites); and each host's `main.cc` constructs and installs the
funnel at boot. The gateway's `main.cc` still installs the identity sink — the
identity RPC listener it serves is what publishes — while the socket fan-out,
the delivery consumer and the `RoomManager` lifecycle moved here.

## Why the voice relay came along

The relay is a `SyncForwarder`: it exists only to serve the `/sync` socket. Had
it stayed in the gateway it would have been dead code (nothing there holds a
socket any more) and a dead app feature in the window between this sub-step and
the gateway's deletion in Phase 3d. It moved with its socket, and its two
refusals became `SyncErrors::VoiceUnavailable`, which is why the gateway's
catalog lost its four sync-adjacent definitions and the `[voice]` block moved
to `services/sync/config.toml.example` and the deploy example.

## The endpoint, and what the app must do

`/sync` is TLS on `7025`, published on all interfaces: the app dials the socket
directly because a WebSocket upgrade cannot ride the gateway's reverse proxy.
The endpoint is configuration until Phase 3d step 5 moves discovery, so the
frontend's switch to 7025 is one coordinated change — the same commit that
makes the surface complete, which is why this sub-step moves the relay, the
audit writers and the fan-out together rather than in pieces.

## What this service must not do

No auth surface of its own, no route other than `/health` and the WS upgrade,
no second writer for any table it owns, no reading of another service's
database (the three pull sources call their own gRPC contracts), no AI capacity
and no in-process emit that the control plane cannot also perform.

## Compose volume

`config.sync.toml` read-only as `config.toml`; the certs directory read-only;
identity's data directory read-write as `database/` (the transitory file);
`services/sync/database/schema.sql` read-only beside it. State lives on the
host and is bind-mounted, so updating is a rebuild plus `docker compose up -d`.
