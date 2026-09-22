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

Sub-step 3a-1c created this binary and flipped who serves `/sync`. It composes
the engine package (`packages/sync`, `argus_sync`) as it stands; sub-step 3a-1d
inlines the engine here and deletes the package. The split is deliberate: c
changes *behaviour* (who persists, who serves, who reaches live connections),
d changes only file placement, so the WebSocket protocol cannot regress by
accident in the commit that changes the endpoint.

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
  This service is their only writer.
- **The fan-out.** Two NATS subscribers (`sync_fan_out::subscribeChangeFanOut`,
  `subscribeActionJournal`) and the delivery consumer. An audit frame persists
  first and fans out the DB-assigned row (Ruling Y); a journal row is inserted
  verbatim; an emit frame becomes a room emit; an identity catalog frame belongs
  to the memory replicas and is not re-emitted.
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
changes with it. Two consequences are accepted for now and are the reason the
config comment exists: the audit tables' `REFERENCES user(id)` foreign keys
target a table this owner does not declare, and the deploy binds identity's
data directory into this container. Both are the transitory price of moving the
writer before splitting the file; rule 27's shape resumes in 3c-2.

The schema is applied at every boot (`registerBeginningAdvice` →
`DbService::runScriptFile`) and `applyPragmas()` follows it, so a fresh
container is self-sufficient. `/health` needs the TLS certs, which the compose
mounts read-only.

## How a change reaches a client

Producers never write these tables. A domain service publishes on its own
subject (`argus.<domain>.v1.change`); identity's journal rows travel
`argus.identity.v1.user-action`, a subject of their own because the journal is
inserted verbatim rather than parsed as an emit. This service subscribes,
persists, and fans out. The imperative frames — a role change, a disconnect, an
emit to one user — reach the same dispatcher through the control RPC instead.
Subscriptions register before the bus connects and the bus re-attaches them on
every reconnect, so a broker that is down at boot costs no handler. The one
object the two subscribers share is the `AuditFanOut` `main.cc` owns; the
fan-out itself is a free-function namespace because dispatching holds no state
of its own.

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
