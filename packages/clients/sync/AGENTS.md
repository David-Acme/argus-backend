# argus_clients_sync

The `argus.sync.v1.SyncControlService` surface seen from the caller's side: the
fleet secret every call carries, the frozen frame the two row-carrying calls
put on the wire, and the ack that decides the answer.

## What this is

A module, not a service: one `argus_clients(NAME sync ...)`, a STATIC library
whose include root is `src/`, so a consumer writes `<sync/sync-client.hxx>` and
links `argus::clients::sync` (D8's imperative leg, §3.5). It is the only
CMakeLists that **compiles** `argus/sync/v1/contracts.proto` — the contract
package is an INTERFACE target and ships no translation unit — so the control
schema's `SyncFrame` symbols are generated here, in
`argus-clients-sync/generated/`. Four link lines across three trees, each of
which adds the folder to its own standalone build first:
`services/identity/CMakeLists.txt`, `services/sync/CMakeLists.txt` with its
`sync-control-rpc` module (`services/sync/src/app/rpc/CMakeLists.txt`), and
this package's own suite. One file outside the package builds the client at
boot and installs it through `sync_control::setSink` —
`services/identity/src/app/main.cc`; the emit-only call site
(`user-feature-service`) reaches the same sink through
`<sync/sync-control-sink.hxx>`, the interface that keeps a domain owner from
linking the transport. argus-auth left the leg in 2026-10: a logout and a
session revocation now travel its durable change outbox
(`argus.auth.v1.session`), closing only the revoked session's sockets.

## Layout

- `src/sync/sync-client.hxx` — `SyncClientConfig` (the target and the fleet
  secret, a struct for the reason `NotificationClientConfig` is one: two bare
  strings at a call site can be handed over the wrong way round) and
  `SyncClient` with its three methods
  (`replaceRoleRooms`, `disconnectUser`, `emitToUser`), all `[[nodiscard]]`
  `virtual bool`, all `const`. No local DTO: the frame type is
  `SocketEmitDto` and the role input is `sync_change::RoleRoomChange`, both
  from `contracts/sync`, because the caller already holds them.
- `src/sync/sync-client.cc` — the channel, the deadline, the fleet secret, and
  the one mapping the leg needs (`toFrame`).
- Nothing else: `find packages/clients/sync -type f` returns CMakeLists.txt,
  the two sources, the suite and this file. No `details/` directory.

## Rules

- Rule 25: the folder IS the module. One `argus_clients(NAME sync ...)` with an
  explicit source list, never `file(GLOB)`.
- The include prefix is load-bearing: `<sync/sync-client.hxx>`. It shares the
  `sync/` prefix with `contracts/sync`'s headers and does not collide with
  them: a consumer that links both gets `<sync/sync-change.hxx>` from the
  contract and `<sync/sync-client.hxx>` from here.
- Two protos are listed, not one. `argus/sync/v1/sync.proto` imports
  `argus/sync/v1/contracts.proto`, and the macro generates per file, so the
  imported file has to be compiled in the same call — otherwise `SyncFrame`,
  `SyncOperation` and `TableName` are missing at compile time.
- What a consumer sees: three methods returning `bool`, the config struct it
  constructs the client with, and the two contract types it passes in. What it
  must not see: no stub, no channel, no URL, no retry policy. Two flagged
  deviations, both measured: §2.3's `details/` split is absent, the same one
  `clients/identity` records, and the constructor takes a config struct where
  `IdentityClient` takes two strings — the shape `NotificationClientConfig`
  already uses for its target-and-credential pair, chosen because two bare
  `std::string` parameters of that kind are what rule 16's
  `bugprone-easily-swappable-parameters` counts, and this unit may not raise
  the baseline.
- The refusals stay local, and only where the caller can prove them: all three
  methods refuse a non-positive `userId` (there is no such room) without
  opening a socket. Role *names* are **not** validated here — `RoleRoomChange`
  carries the frozen spellings the caller derived through
  `userRoleToString`, and the consumer's own `userRoleFromString` is the
  authority on what a name means.
- On the wire: `SyncClientConfig.credential`, the caller's own credential,
  rides every call as `x-argus-credential` (`addPeerCredential`); only when it
  is empty does the legacy `fleetSecret` ride as `x-argus-fleet` (2026-10-05
  audit, #25). Sync answers each caller only for its own methods. No caller identity
  rides this leg — the control calls are the installation's own, not an
  actor's, and §3.5 puts no user context on them. One deadline,
  `kCallTimeoutMs` = 5000 ms, a `constexpr` in the `.cc`. The channel is
  plaintext (`makeChannel` is `InsecureChannelCredentials`).
- The frame mapping is the contract of this leg: `SocketEmitDto.operation` and
  `.option` cross as the **numbers** their enums carry — `contracts.proto`
  mirrors `sync-operation.hxx` and `table-name.hxx` value for value, and the
  suite pins that equality enumerator by enumerator — while `.obj` crosses as
  JSON text in `info`. That is not the spelling the WS envelope uses, and the
  difference is the point: the envelope spells the same triple
  `{operation, option, info}` with `option` as the table's *name* and `info` as
  the *object*, so the service converts both back before the frame is rendered
  (`sync_change`'s payload builders, at the far end). One corner differs and is
  recorded rather than hidden: a null `obj` serialises as `"{}"` through
  `json_util::toString`, where the socket's own envelope would write
  `"info": null`; none of the four measured call sites builds such a frame,
  since each one fills a row.
- The answer is the ack, not the transport: `false` covers an unreachable
  service, a timeout and sync's own refusal alike, and that is deliberate —
  §3.5's control operations are best-effort by nature (the durable outbox of
  §3.5 is the change leg's, not this one's). `ControlAck.reason` is written by
  the server for diagnosability on the wire; this client's surface is the
  bool.
- Config, since sub-step 3a-1c: the target and the credential arrive in
  `SyncClientConfig`, resolved from `sync.control_target` and
  `sync.control_credential` (the legacy `sync.control_secret` beside it on an
  older install) — the caller's `[sync]` block
  (`services/identity/config.toml.example:65-67`) and, on the answering side,
  `services/sync/config.toml.example:37`. The deploy stack pairs them under
  the same keys (`argus-deploy/config.identity.toml.example`,
  sync's own at `argus-deploy/config.sync.toml.example:37`), spelling the target
  `argus-sync:7041` from the compose containers and `127.0.0.1:7041` in the
  project templates, and an empty target leaves the leg uninstalled
  rather than failing a call.

## Tests

- `tests/unit/sync-client-test.cc` — five cases: the local refusals with a live
  server as their control (every ack it serves is a success, so `true` coming
  back would prove the client dialled, and the call counters prove none did),
  the fleet secret plus the frame's frozen mapping for both row-carrying calls,
  the server's refusal answering `false` while all three calls demonstrably
  landed, an unreachable target answering `false` instead of throwing, and the
  two enums against their proto constants enumerator by enumerator. The rows
  the two row-carrying calls carry are distinct — the AuthContextChanged one
  whose table is `User` (0) and the per-user audit one whose table is
  `UserAuditLog` (17) — so a dropped `set_operation` or `set_table` fails
  instead of hiding behind proto3's default 0.
- Two things it does not exercise, both measured: the 5000 ms deadline (the
  unreachable case is bounded by port 1's refusal, not by the deadline) and the
  empty-`fleetSecret` branch of `addFleetSecret`, which no case drives.
- The CMakeLists registers it as `sync-client-test`, with `EXCLUDE_FROM_ALL
  FALSE` because the folder is pulled in `EXCLUDE_FROM_ALL`, and guards the
  target so a second consumer still registers exactly one test. It registers in
  exactly the projects whose trees add it — auth, identity and sync.
