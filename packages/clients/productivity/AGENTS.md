# argus_clients_productivity

The argus-productivity sync wire seen from the caller's side: one table branch
per call, its rows and tombstones back.

## What this is

A module, not a service: one `argus_clients(NAME productivity ...)`, a STATIC
library whose include root is `src/`, so a consumer writes
`<productivity/productivity-sync-client.hxx>` and links
`argus::clients::productivity`. 59 lines of source (`.hxx` 29, `.cc` 30)
behind a 33-line CMakeLists. Three link lines in two CMakeLists:
`gateway-core` (PUBLIC, `services/gateway/CMakeLists.txt:184`), and
`argus-productivity` with its `productivity-sync-rpc-test` in
`services/productivity` (`:190`, `:281`). The gateway is the caller; the
service that owns the contract links the package for its own server side, so
`argus/productivity/v1/sync.proto` is compiled here and in no other
CMakeLists of the tree.

Three files include the header: the gateway's
`services/gateway/src/sync/productivity-sync-source.hxx`, the RPC test above
and this package's own suite.

## Layout

- `src/productivity/productivity-sync-client.hxx` — the whole surface:
  `ProductivitySyncClient(std::string target)`, deleted copy, and
  `pullTable(request, identity)` returning
  `std::optional<argus::productivity::v1::PullTableResponse>`; 3 files include
  it.
- `src/productivity/productivity-sync-client.cc` — the deadline (5000 ms,
  `kPullTimeoutMs`) and the nullopt.
- `tests/unit/productivity-sync-client-test.cc` — the suite below.

## Rules

- Rule 25: the folder IS the module. One `argus_clients(NAME productivity ...)`
  with an explicit source list, never `file(GLOB)`.
- The include prefix is load-bearing:
  `<productivity/productivity-sync-client.hxx>`.
- What a consumer sees: one virtual, `std::optional<PullTableResponse>` — rows
  under `created`, tombstones under `deleted`, the cursor row under
  `last_created`, in the branch the request asked for. What it must not see: no
  `StubInterface`, no `grpc::Channel`, no channel target, no deadline, no
  generated service class, no retry policy, and no branch of its own — the
  table is the caller's choice and the client adds nothing to it.
- An empty optional is the only failure value: `pullTable` maps **every**
  non-OK status to `nullopt`, so a refusal the receiver chose and an
  unreachable receiver are the same answer. There is no outcome enum here, and
  no way for a caller to tell them apart — deliberate for a pull the gateway
  can serve stale, and the point where this client differs most from
  `clients/notification`. The receiver agrees:
  `services/productivity/src/feature/sync/productivity-sync-rpc-service.cc`
  refuses `TABLE_NOT_SET` with INVALID_ARGUMENT at line 228, so a caller that
  sends no branch gets nothing back.
- The identity is the substrate's: `x-argus-user`, `x-argus-role` and
  `x-argus-device`. A caller must engage `identity.device`, because the
  receiver reads its caller through
  `argus::client::callerUserId`, which requires all three legs — the gate is
  `if (!(user && role && device))` at
  `packages/lib/grpc/src/grpc/grpc-server-identity.hxx:101` — and answers
  UNAUTHENTICATED "identity metadata missing or invalid" without them. There is
  no capability credential on this edge at all: the client has no credential
  field and never sends `x-argus-credential`, and the receiver never asks for
  one.
- The endpoint is runtime config: `productivity.grpc_target`, read at
  `services/gateway/src/main.cc:238`. Only
  `argus-deploy/config.gateway.toml.example:76` declares it
  (`127.0.0.1:7037`); the gateway's own tree declares the endpoint
  of no leg but the camera's (`services/gateway/config.toml.example:71-73` has
  proxy_url and db only), so a gateway started from that tree logs
  "productivity leg -> unconfigured source (503)" and never builds the client.
  With no target the client is not constructed at all rather than pointed
  somewhere default.
- §2.3 gives a client a `details/` for channel, credentials, retry and envelope
  parsing. This package has **no `details/` directory**: there is no retry, no
  credential and no envelope — 30 lines of `.cc` hold the deadline and the
  nullopt beside the surface. A known, flagged deviation.

## Tests

- `tests/unit/productivity-sync-client-test.cc` — two cases against a real
  in-process receiver. The first pins the wire: two different calls carry two
  different branches (`kProjectTask` with `required_create` and a `start_id` of
  40, then `kReminder` with `required_deleted`), the four `x-argus-*` values,
  `x-argus-credential` absent, and the parsed rows — title, `sort_order` 1.5
  through `doctest::Approx`, a tombstone's `deleted_at` 900, `last_created` id
  88. The second pins the failure value: NOT_FOUND, PERMISSION_DENIED,
  UNAVAILABLE and an unreachable target all answer an empty optional, while the
  same body under OK comes back with the row — so the empty results are the
  status's doing and not rows the client dropped.
