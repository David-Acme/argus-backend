# F3-1c — `services/sync`: the surface, the state and the producers' wire

Row 1 of Phase 3a, sub-step **c**. Row 1 reads: *"Extract `services/sync` from
the gateway: `/sync`, rooms, fan-out, audit persistence, the action journal
(`user_action_log`, §3.5), control RPC. Create `contracts/sync` … and
`clients/sync`; delete `packages/sync`, `packages/socket`, `packages/room`,
`packages/audit`"* (`docs/history/plans/architecture-plan.md:834`). Sub-steps
a1, a2 and b are done (`5687dac`, `b8b2bdc`, `ef67014`) and their reports are
`f3-1a-sync-contract-vocabulary.md`, `f3-1a-sync-control-wire.md` and
`f3-1b-sync-repositories-home.md`.

This report is written **before** the code, per the standing protocol. The
pre-state and the design below are measured; the "as executed", "gaps" and
"verified" sections are filled in when the sub-step closes.

## The correction this sub-step opens with

In the previous session I told the user that c would split like a1 did —
**c1** the extraction and the deletions, **c2** identity's writes onto the
wire. Measurement refutes the split, and a1's own report already contains the
refutation (`f3-1a-sync-contract-vocabulary.md:105-126`, the C2 finding):

> Both cannot hold: identity is a package linked into the gateway process, so
> once the journal's code is a service's, the six sites cannot call it, and
> 3a-1's own deletion clause removes the code they call. The move therefore
> happens in 3a-1 (sub-step c).

The three legs are one act:

1. `packages/audit`'s deletion removes `SyncAuditService`,
   `UserActionLogService` and the two log services — the exact code identity's
   **eleven** sites call (6 journal + 5 audit publishes, measured below).
2. Identity's **four imperative frames** (`replaceRoleRooms`, two
   `disconnectUser`, one `emitUser`) reach live connections through
   `packages/room`'s `thread_local` rooms, which exist in the process that
   holds the `/sync` sockets. Once the sockets are services/sync's, an
   in-process emit in the gateway reaches an empty room set: not a compile
   error but a silent functional break. Deleting `packages/room` without
   repointing them first is therefore impossible, and repointing them is the
   same act as moving the sockets.
3. Sub-step c is consequently **one unit of work, one commit**.

The c/d split that holds is the one a1's table records, and it is a split by
*artefact*, not by *leg*:

| Sub-step | Scope |
|---|---|
| **c** | `services/sync` created as a binary that composes the existing packages; the gateway's sync surface, fan-out, delivery consumer and RoomManager lifecycle removed; identity's audit, journal and imperative writes onto the wire; `packages/{socket,room,audit}` deleted; the gate's array 18 → 17 |
| **d** | `packages/sync`'s engine, forwarder vocabulary and pull-source contracts relocated into `services/sync`; `packages/sync` deleted; `memory` repointed; the doc sweep; the gate; the review |

The reason c does not also move `packages/sync`'s engine in: c is the sub-step
that flips who serves `/sync` and who persists the audit trail, and its risk is
in behaviour, not in file placement. Leaving the WebSocket protocol code
literally untouched — a package the new binary links — means the protocol
cannot regress by accident in the same commit that changes the endpoint. d
then moves code that no longer has any behaviour left to break.

## Pre-state, measured

### The four packages this sub-step ends

| Package | Files | Consumers | What it is |
|---|---|---|---|
| `packages/socket` | 4 (`CMakeLists.txt`, `.gitignore`, `socket-service.{hxx,cc}`) | gateway, identity, camera, notification, productivity, guard, memory | `SocketService` (`emitModule`/`emitUser`/`emitUsers`/`replaceRoleRooms`/`disconnectUser`) + the dead `setEventBus`/`publishChange` NATS leg |
| `packages/room` | 3 (`CMakeLists.txt`, `room-manager.{hxx,cc}`) | `packages/socket` only, plus three vestigial lifecycles | `RoomManager`, file-level `thread_local` rooms per module/user |
| `packages/audit` | 25 | gateway, identity, `packages/sync`, `packages/memory` | 3 repositories (audit-log, user-audit-log, user-action-log), 3 schemas, 4 services (audit-log, user-audit-log, user-action-log, sync-audit) |
| `packages/sync`'s **surface** | 10 code files + 3 pull-source contracts | gateway (`/sync`), camera (forwarder vocabulary), memory (transitively) | `SyncSocket`, `SyncService`, `SynchronizedService`, the DTO, the forwarder, the event repository + schema, `person-event` schema |

`packages/sync` itself survives sub-step c (d deletes it): what leaves it in c
is nothing — c links it. Its three pull-source contracts stay in
`src/shared/contracts/` and are implemented inside the new service.

### The gateway's sync surface

`services/gateway/src/sync/` holds 26 files. They divide four ways:

- **The surface** (moves): `sync-registrar.{hxx,cc}` (`/sync` route +
  `WS_PATH_ADD`), `sync-fan-out.{hxx,cc}` (the `argus.*.v1.change` router),
  `camera-fan-out.{hxx,cc}`, `user-change-fan-out.{hxx,cc}`,
  `notification-delivery-consumer.{hxx,cc}`, `notification-row-json.hxx`,
  `camera-sync-source.{hxx,cc}`, `productivity-sync-source.{hxx,cc}`,
  `notification-sync-source.{hxx,cc}`.
- **The relays** (stay until Phase 3d): `camera-stream-socket.{hxx,cc}`,
  `camera-stream-relay.{hxx,cc}`, `voice-grpc-relay.{hxx,cc}`. They implement
  `SyncForwarder` and carry frames between the app and camera/voice; they are
  not sync state.
- **`camera-notifier.{hxx,cc}`** (stays): the `argus.camera.v1.object-detected`
  subscriber that creates notifications over `argus.notification.v1`. It is a
  camera-to-notification edge, not sync.
- **`fallback/`** (stays): the degraded-mode records.

State that moves with the surface: `src/shared/repositories/delivery-inbox/`
(3 files, the delivery consumer's receipts) and
`src/shared/services/socket/nats-identity-change-sink.{hxx,cc}` — the identity
change sink `main.cc` installs for argus-memory's catalog replica, which is
identity's producer leg and belongs beside it.

### `packages/identity`'s sites, re-measured

Eleven call sites across five files, in four features:

| Leg | Sites |
|---|---|
| journal (`UserActionLogService::record`) | `user-feature-service.cc:134`, `portrait-preview-service.cc:137`, `invitation-feature-service.cc:151`, `auth-service.cc:309`, `auth-service.cc:528`, `auth-service.cc:633` |
| audit (`SyncAuditService::publishUsers`/`publishModule`) | `user-feature-service.cc:70`, `invitation-feature-service.cc:122`, `auth-service.cc:297`, `auth-service.cc:555`, `identity-rpc.cc` (`identity-rpc.hxx:114` holds the member) |
| imperative (`SocketService`) | `user-feature-service.cc:56` (`replaceRoleRooms`), `:97` (`disconnectUser`), `:128` (`emitUser`), `auth-service.cc:526` (`disconnectUser`) |
| emit (`SocketService`) | `invitation-feature-service.cc:145` (`emitModule(UserInvitation)`), `auth-service.cc:291` (`emitModule(User)`) |

### The tables

`audit_log`, `user_audit_log`, `user_action_log` and
`notification_delivery_inbox` live in **identity.db**
(`packages/identity/database/schema.sql:176-208,219-238` plus indexes at
`:277-283`). They do not move in this sub-step: row 3c-2 owns the split into
`sync.db` with row-count and checksum verification. A fresh empty table in a
new database would regress the client's monotonic watermark — the
SynchronizeAuditLog paging is by SQLite row id, and a client whose watermark is
above a restarted sequence would never see rows again.

So after c, `services/sync` is the **single writer** of these four tables
while they still physically live in identity.db. That is the transitory state
the plan's ordering already implies, and it is the one rule-27 exception this
sub-step creates. The DDL follows its owner (rule 26): the four tables'
`CREATE TABLE` and indexes move to `services/sync/database/schema.sql` and
leave `packages/identity/database/schema.sql`, and services/sync applies its
own schema to `identity.db` at boot, exactly as the gateway does today
(`main.cc:449`). When 3c-2 lands, the DDL does not move again — only the rows
do — and the config key changes from `[sync] db = "database/identity.db"` to
`sync.db`.

### The port

`/sync` is a WebSocket route and the gateway's reverse proxy cannot relay a
WebSocket upgrade: `reverse-proxy.cc` forwards with
`HttpClient::sendRequest`/`setPassThrough(true)` — no 101 tunnelling, no
CONNECT, no splicing — and `/sync` is in `gatewayNativePaths()`
(`proxy-config.cc:13`), so it never reaches the proxy. There is no upgrade code
anywhere in the tree; Drogon performs the handshake behind `WS_PATH_ADD`.

`services/sync` therefore terminates `/sync` on **its own TLS listener**: 7025
(the retired legacy internal listener), the same `argus.local` certificate
every service uses, with 7041 for the gRPC control plane (the +10 convention
cannot hold — 7035 is voice's). `/sync` leaves `gatewayNativePaths()`, so a
client still dialing 7024/sync gets the gateway's unmatched-route envelope
instead of a socket that no longer exists — a loud failure, not a silent one.

### Test instances that move

| Suite | Today | Destination |
|---|---|---|
| `gateway-test.cc`'s sync/fan-out/registrar cases | gateway | split out into `services/sync/tests/unit/`; the relay and notifier cases stay |
| `audit-sync-read-test.cc` | gateway | `services/sync/tests/unit/` |
| `notification-delivery-inbox-test.cc` | gateway | `services/sync/tests/unit/` |
| `notification-delivery-live-test.cc` | gateway | `services/sync/tests/unit/` |
| `event-sync-empty-test.cc` | `packages/sync` | `services/sync/tests/unit/` |
| `golden-sync-test.cc` + 17 fixtures | `packages/sync/tests/e2e` | `services/sync/tests/e2e/` — the frozen golden frames move with the surface they pin |

### Who links what, and how it changes

`argus::socket` (`SocketService`) has seven consumers; `argus::room` three
(the socket package, the gateway's lifecycle, camera's vestigial one);
`argus::audit` four; `argus_sync` three (gateway, camera for the forwarder
vocabulary, and `packages/memory` transitively).

After c: every consumer above loses its link except camera, which keeps
`argus_sync` for `SyncForwarder` exactly as it does today. `packages/memory`
links `argus::audit` for one stated reason — the `<nats/...>` include roots
`catalog-replica.cc` compiles against, which reach it only through
`argus::audit`'s PUBLIC chain to `argus::socket`, and from there to
`argus::lib::nats` (`packages/memory/CMakeLists.txt:107-113`) — so c repoints
it to `argus::lib::nats` directly and drops the audit/socket/room links. That
repoint is c's because the targets die in c.

## Design

### The service

`services/sync`, composed on `services/gateway/src/main.cc`'s order: config →
database paths → schema → listener → advices → exception handler → NATS block
→ RPC server → boot advice → `run()`.

```
services/sync/
├── AGENTS.md  CONTEXT.md  CMakeLists.txt  CMakePresets.json
├── config.toml.example  .gitignore
├── database/schema.sql                 the four tables' DDL (transitory home)
├── Dockerfile  Dockerfile.dockerignore
├── src/
│   ├── app/
│   │   ├── main.cc
│   │   └── rpc/sync-control-rpc-service.{hxx,cc}   argus.sync.v1.SyncControlService
│   ├── config/sync-config.{hxx,cc}               typed config (D20)
│   ├── feature/
│   │   ├── transport/                            the /sync surface
│   │   │   ├── controllers/sync-socket.{hxx,cc}
│   │   │   ├── dtos/synchronized-dto.hxx
│   │   │   ├── services/{sync-service,synchronized-service}.{hxx,cc}
│   │   │   └── infra/{camera,productivity,notification}-sync-source.{hxx,cc}
│   │   └── fanout/
│   │       ├── services/{sync-fan-out,camera-fan-out,user-change-fan-out,
│   │       │             notification-delivery-consumer}.{hxx,cc}
│   │       ├── infra/notification-row-json.hxx
│   │       └── repositories/delivery-inbox/
│   └── shared/
│       ├── repositories/{audit-log,user-audit-log,user-action-log}/
│       ├── schemas/{audit-log,user-audit-log,user-action-log}/
│       └── services/{audit-log,user-audit-log,user-action-log,room,socket}/
└── tests/{unit,e2e,fixtures}
```

Two features, not four: `transport` owns the socket, the engine and the three
pull-source adapters; `fanout` owns everything a change event or a delivery
does on arrival. The audit repositories, schemas and services are `shared/`
because both features read them (the paging legs read, the fan-out writes) —
rule 23's 2+ rule, applied inside the service. `socket`/`room` keep their
existing class names and behaviour; only their home changes.

### The producers' wire

Identity's writes stop being in-process calls. Three new pieces in
`contracts/sync`:

1. **`emit` on the user change sink.** `UserChangeSink` gains
   `emitModule(TableName, const SocketEmitDto&)` — the one method identity
   needs that the two existing consumers do not — and the `identitySink()`
   slot beside `productivitySink()`/`notificationSink()`. Productivity and
   notification's sinks implement it as a module-room emit; nothing about
   their existing behaviour changes.
2. **The action journal's own event.** `user-action-event.hxx` carries
   `UserActionEvent{recordId, tableName, action, before, after, userId, ipAddress}`
   with `toJson()`/`fromJson()`, modelled on `user-audit-event.hxx`. Identity
   publishes it on the **additive** subject `argus.identity.v1.user-action`
   (§3.5), through a `UserActionSink` slot installed at boot. The subject
   gains its row in `docs/architecture/wire-nats-subjects.md` — the authority —
   in this sub-step, because the plan's row 2 lists the declaration beside the
   *outbox*, and the outbox is a durability refinement of a publish that c
   already performs. Publishing on an undeclared subject and declaring it
   later is the worse order.
3. **The control sink.** The four imperative frames go through
   `clients/sync`'s `SyncClient` (a2). Identity holds the interface
   (`SyncControlSink`, three methods), not the client: the client is
   constructed and installed in each host's `main.cc`, exactly as the identity
   change sink is today, so `packages/identity` stays linkable into a process
   that has no control channel (its suites).

The gateway's `NatsIdentityChangeSink` moves to `packages/identity`'s own
feature tree: it is identity's producer leg to `argus.identity.v1.change`, and
`packages/identity` is where the domain lives.

### What the gateway keeps

The relays, the notifier, the identity surface, the proxy, the health extra and
the deploy record. What it loses: the `/sync` route, the fan-out, the delivery
consumer and its inbox, the RoomManager lifecycle, and `/sync` from
`gatewayNativePaths()`.

### What `services/sync` must not do

No auth surface of its own (its filter chain is `DeviceFilter` + `JwtFilter`,
the two the `/sync` route uses today), no HTTP route other than `/health` and
the WS upgrade, no reading of another service's database (its three pull
sources call `argus.camera.v1`/`argus.productivity.v1`/`argus.notification.v1`
through their clients, as they do today), and no second writer for any table it
owns.

## Open items carried into the execution

- `event`/`person_event` have **no `CREATE TABLE` anywhere in the tree**; the
  `EventRepository` and both schemas compile and are read by the `/sync`
  `Event`/`PersonEvent` paging legs. The missing DDL is not invented here; it
  is carried and reported.
- `notification_delivery_inbox`'s DDL moves to services/sync's schema with the
  audit tables' (same owner, same transitory database), and its consumer's
  destination is the `fanout` feature.
- The NATS bus becomes load-bearing for audit and journal persistence: a
  deployment with no `[nats].url` loses live fan-out and audit writes. The
  producers' durable outbox is row 2's; this sub-step documents the
  dependency.
- The frontend must dial the new endpoint. The plan's own risk row already
  declares the coordination, and 3d step 5 moves discovery; until then the
  endpoint is configuration.

## As executed

### The service, as it landed

Two features, three module targets under `src/`, plus the control RPC in
`app/rpc/`:

```
services/sync/
├── AGENTS.md  CONTEXT.md  CMakeLists.txt
├── config.toml.example  .gitignore
├── database/schema.sql                 the four tables' DDL and five indexes
├── Dockerfile  Dockerfile.dockerignore
├── src/
│   ├── app/main.cc                     composition, control listener, NATS legs
│   ├── app/rpc/sync-control-rpc-service.{hxx,cc}
│   ├── config/sync-config.{hxx,cc}     argus::sync-config
│   ├── feature/transport/infra/        argus::sync-transport
│   │   ├── {camera,notification,productivity}-sync-source.{hxx,cc}
│   │   ├── sync-socket-registrar.{hxx,cc}
│   │   └── voice-grpc-relay.{hxx,cc}
│   ├── feature/fanout/                 argus::sync-fanout
│   │   ├── services/{sync,audit}-fan-out.{hxx,cc}
│   │   ├── services/notification-delivery-consumer.{hxx,cc}
│   │   └── repositories/delivery-inbox/
│   └── shared/infra/notification-row-json.hxx
└── tests/{unit,e2e,fixtures}
```

Three deviations from the design's tree, each forced by a measurement:

1. **The engine stays in `packages/sync`.** The design's `transport/` was to
   hold `controllers/sync-socket`, `dtos/synchronized-dto` and
   `services/{sync,synchronized}-service`. It does not, and that is the c/d
   split working as intended: c composes the package, d inlines it. What
   `transport/` holds is the part the service owns *now* — the registrar, the
   three pull sources and the voice relay.
2. **`notification-row-json.hxx` is `shared/`, not `fanout/infra/`.** Two
   features render it: `fanout`'s delivery consumer writes the receipt from the
   row, and `transport`'s notification source serves it to `/sync`. Rule 23's
   2+ rule, measured rather than assumed.
3. **`camera-fan-out` and `user-change-fan-out` are one `sync-fan-out`.** The
   two files were routers over the same subject with the same parse; merged,
   the routing decision is `planEvent` on a parsed `Event`, which is also what
   the control RPC hands to `dispatchEvent`.

No `CMakePresets.json`: the design's line is dropped, and the service
configures through the root graph exactly as the other ten do.

### The engine's three lines

`packages/sync` changed behaviour in exactly one place. The design contemplated
a fourth pull-source contract plus `setActionLogSource` on `SyncService` for the
`user_action_log` read leg. The execution deviated to something smaller and
local: `SynchronizedService` gained a `UserActionLogRepository
userActionLogRepository_` member, `repoFor` gained `case
TableName::UserActionLog`, and `SynchronizedDto` gained
`std::optional<SynchronizedBodyDto> userActionLog` parsed from
`"user_action_log"`. The wire body field is the one the app already sends; the
engine needed no new interface, no new source slot and no new
`*SyncUnavailable` refusal, and the read leg cannot be "unconfigured" because
the table lives in the file the service already opens.

A fourth line followed from a measurement: `UserActionLogRepository::find` and
`findLast` read through `DbService::readOnlyClient()`, a slot the library's own
comment says "the host installs at boot" and that **no host installs** — only
test suites do (`services/sync/tests/unit/*`, `gateway-test.cc`; measured
across the tree). Uninstalled, the repository answers empty by design, so the
leg added here would have been dark in production while its suite passed. The
two methods now resolve `DbService::client()`, the client this service writes
through, which is what the journal's sibling repositories already do:
`audit-sync-read-test`'s first case is named "audit sync reads resolve to the
default client, not the read-only one" and asserts exactly that id. The
`event`/`person_event` repository keeps the slot, and with it the empty answer
the missing DDL already produces.

### The producers' wire, as built

The design had `UserChangeSink` gain `emitModule` and an `identitySink()` slot
beside `productivitySink()`/`notificationSink()`. The execution deviated, and
the deviation is the better shape: identity's outbound wire is **one
five-method `IdentityChangeSink`** in `contracts/sync`
(`publishCatalog`, `emitModule`, `publishModuleAudit`, `publishUsersAudit`,
`publishAction`), with `NatsIdentityChangeSink` implemented in
`packages/identity/src/feature/api/user/services/`. Productivity's and
notification's sinks were not touched: they never needed a sixth method, and
identity's needs a slot of its own because its catalog payload is identity's
vocabulary, not the generic emit triple.

The four imperative frames go through `SyncControlSink`
(`contracts/sync/sync-control-sink.hxx`, three methods) whose only
implementation is `clients/sync`'s `SyncClient`; identity holds the interface,
each host's `main.cc` constructs and installs the client, so
`packages/identity` stays linkable into a process with no control channel — its
suites link it exactly as before.

`SyncAuditService` did not move: the facade was **merged into the fan-out**.
`audit_fan_out::{handleAuditChange, handleActionJournal}` parse the frame,
persist it through the moved `AuditLogService`/`UserAuditLogService` (or, for
the journal, straight through `UserActionLogRepository`) and dispatch the
DB-assigned row, which is Ruling Y in code. `UserActionLogService` was deleted
with its two remaining callers, because a write-only insert with no reader of
its own is not a service.

The new subject `argus.identity.v1.user-action` is declared in
`packages/lib/nats`'s `nats-subject.hxx` beside the change subjects, six lines,
and documented in `docs/architecture/wire-nats-subjects.md`.

### Deletions

`packages/socket` (4 files, including the dead `setEventBus`/`publishChange`
NATS leg), `packages/room` (3), `packages/audit` (its remaining 10 files), and
the gateway's ten moved surface files. Three `PROJECT_IS_TOP_LEVEL` blocks lost
their dead package guards (`packages/identity`, `services/guard`,
`packages/sync`), and `packages/lib/auth` lost `identity-change-sink.hxx`, whose
one consumer was the gateway's sink.

The gateway's `src/sync/` keeps what is not sync: `camera-notifier`,
`camera-stream-socket`, `camera-stream-relay` and `fallback/`.

The gate's project array is 17: `packages/socket` and `packages/sync` leave it
(sync is composed by its service, and d deletes the package), `services/sync`
enters, and `scripts/setup.sh`'s `ensure_local_config` list gains it.
`check-deps.py`'s `IN_TRANSIT` lost three of its seven entries; the gate answers
58 declarations, 450 edges, 0 forbidden, 0 cycles, 0 unresolved. The gateway
lost the two link lines and the two guard blocks of
`argus::clients::{camera,productivity}` as well: the sync sources were their
only users in that process, so c left them dead (measured: no reference under
`services/gateway/src` or `services/gateway/tests`).

### The tables, the schema and the config keys

The four tables' DDL and their five indexes moved to
`services/sync/database/schema.sql` (rule 26: the owner names the file) and left
identity's. The file they are applied to is still `database/identity.db`, and
`SyncDbConfig`'s header says why in one line. `[sync] db` and `[sync] schema`
are the two keys, and the DDL does not move again in 3c-2 — only the rows do.

The control listener is a `GrpcListenerConfig::resolve(7041)`, i.e. it reads
`server.host` and `server.grpc_port` from the shared listener helper rather than
inventing `[sync] control_host`/`control_port` keys. Its guard's fatal message
says `[server] host`, because that is the key a wrong value lives in: an earlier
draft of this sub-step printed `[sync] control_host`, a key nothing reads.

### The voice leg

The plan recorded mid-flight was `.forwarder = nullptr` with the relay left in
the gateway until 3d. That was superseded, and the reason is worth keeping: the
relay is a `SyncForwarder`, so nothing but a `/sync` socket can use it — in the
gateway it would have been dead code from this commit onward, plus a dead app
feature in the window before the gateway's deletion. It moved into
`transport/infra/`, its two refusals became `SyncErrors::VoiceUnavailable`
(503), the gateway lost its `argus::clients::voice` link and the four
sync-adjacent error definitions that only its own test named, and `[voice]
target` moved to the service's and the deploy's sync examples. The two
frozen-wire test cases moved with it into
`services/sync/tests/unit/voice-leg-test.cc`.

### Tests

Five suites changed home and one is new:

| Suite | From | To |
|---|---|---|
| sync surface, fan-out routing | `services/gateway/tests/gateway-test.cc` | `services/sync/tests/unit/sync-surface-test.cc` |
| audit read path | `gateway/tests/audit-sync-read-test.cc` | `services/sync/tests/unit/` |
| delivery inbox | `gateway/tests/notification-delivery-inbox-test.cc` | `services/sync/tests/unit/` |
| delivery live (opt-in) | `gateway/tests/notification-delivery-live-test.cc` | `services/sync/tests/unit/` |
| event paging, empty result | `packages/sync/tests/unit/` | `services/sync/tests/unit/` |
| golden transcript + eight golden pairs (16 fixture files) and the manifest | `packages/sync/tests/` | `services/sync/tests/` |
| voice leg, config + frozen JSON | `services/gateway/tests/gateway-test.cc` | `services/sync/tests/unit/voice-leg-test.cc` (new) |

Three expectations had to change with the code, each for a measured reason:
the sync catalog tripwire went 6 → 7 (the new refusal); identity's migration
test went 21 indexes → 16 with an absence check for the four moved tables
(`moved.empty()` over `audit_log`, `user_audit_log`, `user_action_log`,
`notification_delivery_inbox`), because the DDL left that schema; and the
delivery-inbox suite seeds a bare `user` table, because the audit tables'
`REFERENCES user(id)` target a table this service does not declare.

One more line changed with the endpoint: the golden suite's default
`ARGUS_TEST_BASE_URL` is now `https://127.0.0.1:7025`, because `/sync` is no
longer on 7024 and a default that can only ever skip is worse than none. The
recorded `baseUrl` inside the moved manifest stays `…:7024` — it is provenance
of the original recording, written but never read back by the suite.

### Config and deploy

`services/sync/{config.toml.example,.gitignore,CMakeLists.txt,Dockerfile,
Dockerfile.dockerignore,database/schema.sql}` are the service files, the
`argus-sync:` block in `argus-deploy/docker-compose.yml` is the stack entry, and
both examples carry the new `[voice]` block. Two measurements worth recording:

- **7025 is the only non-loopback publish.** Camera, productivity and
  notification publish loopback-bound pairs and the gateway uses host
  networking; `/sync` is published as `${SYNC_PORT:-7025}:7025` on every
  interface, deliberately, because the app dials the socket directly. The
  control port is `127.0.0.1:${SYNC_CONTROL_PORT:-7041}:7041`.
- **`argus-deploy/config.sync.toml` needs no new provisioning code.**
  `ensure_deploy_configs` copies `config.*.toml.example` by glob, so the sync
  example is picked up by the same loop that produces the others, and
  `ensure_data_tree` deliberately gains no `sync` directory: the compose binds
  identity's data directory into this container, which is the transitory file
  the tables live in.

The delivery consumer's durable name is `argus-sync-delivery`, not the
gateway's `argus-gateway-delivery`: a new durable starts with its own cursor
instead of inheriting the gateway's unacknowledged backlog position, and the
old one ages out with the gateway.

### Defects found and fixed while closing the sub-step

The build gate found one break that no suite could: `llm-wire-test` compiles
`tool-executor.cc` (which includes `<auth/role-access.hxx>`) but only linked
`argus::contracts::auth`. It compiled at HEAD because `memory-core` linked
`argus::audit` PUBLIC, whose chain reached `argus::lib::auth`'s include root;
deleting `packages/audit` in this sub-step removed that transitive path. The
target now links `argus::lib::auth` explicitly, exactly as `llm-core` and
`llm-tool-runtime-test` already did. This is the kind of dependency the
extraction was supposed to expose, and it is why the full orchestrator — not a
per-project build — is the gate.

A code review of the new code found five more, all fixed here:

- `AuditFanOut` was a namespace of free functions that built its three
  dependencies (`AuditLogService`, `UserAuditLogService`,
  `UserActionLogRepository`) as locals on every event. It is a class with
  private members now, one instance owned by `main.cc` and handed to both
  subscribers, the same shape `NotificationDeliveryConsumer` uses.
- `sync_fan_out::dispatchEvent` built a `RoomManager` per event. It is now one
  file-scope `const RoomManager`, the stateless handle over the room module's
  `thread_local` registries that the engine's `SyncService` holds for the
  process's life.
- `finishAck` took four positional parameters; it takes an `AckInput` struct.
- Four comments sat on single statements (`main.cc` twice, `audit-fan-out.cc`
  twice). They are gone; what they said is in the class comment, this report
  and `CONTEXT.md`.
- Two comments named the gateway as the writer or reader of frames it no longer
  touches (`contracts/sync`'s `UserAuditEvent`, identity's
  `nats-identity-change-sink`), and `voice-grpc-relay.cc` discarded an unused
  parameter with a C-style cast.

Two more, from the same review's notes: the two config examples carried an
`[identity] target`/`rpc_secret` block nothing in this service reads (it is the
gateway's key pair, copied with the file) — removed from both; and
`packages/identity`'s `nats-identity-change-sink.cc` was in no `SOURCES` list
and included no header for the type it publishes, which was fixed when the file
was compiled for the first time.

Earlier in the sub-step: `services/sync`'s standalone configure block had no
`argus::lib::cert` guard, so `--only sync` failed to configure while every
sibling service carried one; and two comments in `services/camera` still spoke
of module rooms after the rooms became one fan-out's, plus one in
`contracts/sync` that named `RoleRoomReplaceInput` through the deleted package
it used to live in.

### Frontend coordination, as measured

The app's socket URL is built from the paired port in
`frontend/src/shared/constants/net.constant.ts` (`ARGUS_DEFAULT_PORT = 7024`)
with `SYNC_WS_PATH = '/sync'` (`frontend/src/shared/constants/sync.constant.ts`),
so a paired installation now dials a port that no longer serves the route: the
gateway answers its unmatched-route envelope, which is the loud failure the
design wanted. The frontend needs a sync endpoint of its own — a `SYNC_PORT`
constant or the pairing record carrying it — and 3d step 5 is where discovery
moves. No frontend file was touched from here.

### Verification

The measurements are in `## Gaps carried forward` and `## Verified` below.

## Verified

One full orchestrator run over the final tree, `./scripts/build-all.sh dev`,
exit 0. Its two gates ran before and after the seventeen projects, and the
numbers below are read from that run's own log — not from any earlier one.

**The seventeen projects, in the order the orchestrator builds them**

| Project | Suites | Result |
|---|---|---|
| `packages/lib/cert` | 2 | 100% passed, 0 failed |
| `packages/lib/sqlite` | 2 | 100% passed, 0 failed |
| `packages/identity` | 23 | 100% passed, 0 failed |
| `packages/memory` | 19 | 100% passed, 0 failed |
| `packages/intent` | 4 | 100% passed, 0 failed |
| `services/gateway` | 33 | 100% passed, 0 failed |
| `services/sync` | 40 | 100% passed, 0 failed |
| `services/camera` | 49 | 100% passed, 0 failed |
| `services/productivity` | 30 | 100% passed, 0 failed |
| `services/notification` | 34 | 100% passed, 0 failed |
| `services/guard` | 50 | 100% passed, 0 failed |
| `services/tts` | 20 | 100% passed, 0 failed |
| `services/stt` | 6 | 100% passed, 0 failed |
| `services/vlm` | 7 | 100% passed, 0 failed |
| `services/llm` | 32 | 100% passed, 0 failed |
| `services/voice` | 25 | 100% passed, 0 failed |
| `services/tunnel` | 12 | 100% passed, 0 failed |

388 suites, none failed. `services/sync`'s 40 include the moved engine's own
and the service's new `sync-control-rpc`, `notification-delivery` and
`fanout` suites; `golden-sync-test` and `notification-delivery-live-test` ran
in their CI no-op shape (0.01 s and 0.02 s), as the pre-state recorded.

**Errors and warnings.** Zero first-party diagnostics of either kind. The log
carries 21 `warning:` lines and every one is a third-party configure notice
from outside the tree's own sources: seven `ncnn`, seven `ncnn/glslang`, three
`llama.cpp/ggml` and one `openfst` CMAKE_CXX_STANDARD notice, plus three
`ccache not found` lines from llama.cpp's ggml. `grep -ci "error:"` on the log
returns 0.

**The dependency gate** (`scripts/check-deps.sh`, run by the orchestrator
first):

```
58 declarations, 451 edges, 0 forbidden, 0 cycles, 0 unresolved, 63 edges deferred to phase 3
```

The edge count is 451, one more than the 450 the previous run recorded: the
extraction left `llm-wire-test` reaching `argus::lib::auth` transitively through
the engine, and the new `llm-core` link is that edge written down (see the
"Two link-line corrections" item above). No forbidden edge, no cycle and no
new deferral.

**The tidy gate** (`scripts/check-tidy.sh`, run by the orchestrator at the end
of a full run):

```
clang-tidy 22.1.8
482 TUs, 3141 findings over 45 checks, baseline 3141
worst file services/guard/src/feature/guard/guard-repository.cc (104 findings)
```

Five lines of `scripts/lib/tidy-baseline.txt` moved, and the move is one
translation unit up with four check counts down:

| Line | Before | After |
|---|---|---|
| `tus` | 479 | 482 |
| `modernize-use-nodiscard` | 790 | 780 |
| `modernize-use-designated-initializers` | 305 | 302 |
| `bugprone-suspicious-stringview-data-usage` | 302 | 301 |
| `bugprone-unchecked-optional-access` | 225 | 224 |

The unit count rose because the engine's TUs now compile into
`services/sync`'s tree and are scanned through its compile database — the
orchestrator's `--only` runs skip the scan by design, so those units are
covered exactly once, by the full run. Total findings fell 3156 → 3141. The
four count reductions are the `[[nodiscard]]` additions and the two
`.data()` → `std::string(...)` conversions this sub-step made when the ratchet
first rose (above), landed here as baseline movement in the right direction.
No check stands above its baseline.

**Hygiene.** `packages/sync/build/` held 3.2 G of `dev` and 249 M of `prod`
residue whose `compile_commands.json` named files deleted from a package the
orchestrator no longer builds; the tidy scanner reported it as 14 entries it
could not resolve. The tree is deleted, `packages/sync/` holds only its
`CMakeLists.txt` and `src/`, and the residue is not part of this commit.

**Documentation reconciliation, verified against code rather than against the
mover's report.** Every claim below was re-read at its file before editing:

- `AGENTS.md`'s project-identity line no longer says the gateway serves
  `/sync`; it names argus-sync's own TLS listener on 7025 and the gateway's
  public API on 7024.
- The two owner counts fell from eighteen to seventeen:
  `docs/architecture/system-overview.md:47` and
  `docs/operations/build-and-test.md:6`, `:67`.
- The four moved client `AGENTS.md` files (`camera`, `notification`,
  `productivity`, `voice`) name argus-sync's `sync-transport` module for the
  `/sync` leg, with the include path inside its `infra/`, the read site in
  `services/sync/src/config/sync-config.cc` and the two config examples per
  key. Each link line, include list and config read was opened before the
  edit; the counts of includers were recounted, not copied.
- `services/voice/CONTEXT.md` said four times that the gateway relays or
  validates the voice wire. It now says the forwarder is argus-sync's
  (`voice-grpc-relay.cc`) and that role validation happened once on the `/sync`
  edge, which is argus-sync's filter chain since this sub-step. The matching
  comment at `services/voice/src/feature/voice/voice-rpc-service.cc:79` was
  changed to agree.
- `packages/clients/sync/AGENTS.md` names the two sides of the control wire —
  `services/gateway/CMakeLists.txt:146` and `packages/identity/CMakeLists.txt:217`
  as callers, `services/sync/CMakeLists.txt:152` and `src/app/rpc/CMakeLists.txt:8`
  as the server — and the caller's and answerer's config blocks.

Deleted package names now appear only where the history is deliberate: the
plan, the reports under `docs/history/reports/`, the architecture docs' own
"pre-migration spelling" paragraphs and `docs/history/project-log.md`'s
Phase 3a entry.

## Gaps carried forward

None of these is an unfinished piece of sub-step c; each is either the
transitory state the plan's own ordering implies or a finding an earlier report
already records and this one does not silently drop.

**Created or exposed by this sub-step**

1. **Two owners open identity's file.** `services/sync` reads and writes
   `database/identity.db` while `packages/identity` still declares and migrates
   it. Phase 3c-2 splits the four rows into `sync.db` with row-count and
   checksum verification; until then rule 27's exception is declared in exactly
   one key (`[sync] db`), one comment beside it and `services/sync/AGENTS.md`
   rule 3.
2. **NATS is load-bearing for audit and journal persistence.** A deployment
   with no `[nats].url` loses the live fan-out *and* the audit writes, because
   the write is the fan-out's first act. The producers' durable outbox that
   removes the dependency is row 2's.
3. **The app dials 7024 until the frontend ships a sync endpoint.**
   `frontend/src/shared/constants/net.constant.ts` carries the paired port and
   `sync.constant.ts` the path; the pairing record gaining a sync port and 3d
   step 5's discovery move are the frontend's own unit of work, and no frontend
   file was touched from here.
4. **`event` and `person_event` still have no `CREATE TABLE` anywhere in the
   tree** while `EventRepository` and both schemas compile and serve the
   `Event`/`PersonEvent` paging legs of `/sync`. Carried from the pre-state; the
   missing DDL is not invented here.
5. **`RoomManager::shutdown()` is declared, defined and never called.**
   `services/sync/src/app/main.cc` holds the lifecycle object for the process's
   life and never tears it down — the same shape the gateway had at HEAD
   (`services/gateway/src/main.cc`), so the move changed no behaviour.
6. **`gateway` still links `argus::clients::sync`** — measured as live, unlike
   the two that were removed: `main.cc` builds the `SyncClient` that carries
   the identity surface's four imperative frames to the sync service's control
   RPC.

**Carried, unchanged from earlier reports**

- `DbService::setCameraClient` has no caller anywhere in the tree (measured
  again here: only its declaration and definition remain).
- Five `services/llm/CMakeLists.txt` `add_test` entries carry no `TIMEOUT`
  property (`llm-wire`, `llm-tool-parse`, `llm-tool-runtime`, the two
  `encounter-closed` suites), so a hung suite hangs the run.
- The golden-frame e2e suite is a CI no-op in the orchestrator — it needs a
  fixture environment the gate does not build.
- `packages/lib/cert`'s rotation thread is file-static state, not an instance's.
- The boot-readiness race family, `NotificationSchema::toJson()` versus the
  socket's own rendering, the llm/stt fake `recv()` without a timeout, vlm's
  dead-wire control, the detached stream threads at
  `services/llm/.../llm-controller.cc` and `services/tts/.../tts-controller.cc`,
  and the stray `/tmp/argus-a1-tree` worktree pinned at `5687dac`.


