# argus_clients_camera

The argus-camera sync-table read seen from the caller's side: a table pull in,
rows out.

## What this is

A module, not a service: one `argus_clients(NAME camera ...)`, a STATIC library
whose include root is `src/`, so a consumer writes
`<camera/camera-sync-client.hxx>` and links `argus::clients::camera`. It
compiles one proto (`argus/camera/v1/sync.proto`) and one source
(`src/camera/camera-sync-client.cc`), so the generated `SyncService` stubs
belong to this package and no consumer reaches
`argus.camera.v1.SyncService` without them. Three CMakeLists name it:
`argus-llm` (`services/llm/CMakeLists.txt:183`) and the `camera-rpc` module of
`services/camera` (`services/camera/CMakeLists.txt:143`, which links it for the
generated stub although no source of that service includes the client header)
link it, and `argus-sync`'s `sync-transport` module does too
(`services/sync/src/feature/transport/CMakeLists.txt:19`) — the leg that pages
this domain over `/sync` since sub-step 3a-1c. All three also add the package to
their own standalone tree by path (llm `:143`, camera `:129`, sync `:125`). The
two readers that really use this client are argus-sync's `CameraSyncGateway`
and argus-llm's catalog seed (`fetchCatalogSnapshot`); the sync repositories
themselves are argus-camera's since sub-step 3a-1b.

## Layout

- `src/camera/camera-sync-client.hxx` — `SyncIdentity` (`userId`, `role`,
  `device`, forwarded as x-argus-* metadata, presence required) and
  `CameraSyncClient` with `pullTable(request, identity)` and
  `listCatalog(identity)`, both returning `std::optional`; 4 files include it —
  the two suites under `tests/unit/`,
  `services/sync/src/feature/transport/infra/camera-sync-gateway.hxx` and
  `services/llm/src/main.cc`.
- Nothing else: `find packages/clients/camera -type f` returns CMakeLists.txt,
  the two sources, the two suites and this file.

## Rules

- Rule 25: the folder IS the module. One `argus_clients(NAME camera ...)` with
  an explicit source list, never `file(GLOB)`.
- The include prefix is load-bearing: `<camera/camera-sync-client.hxx>`.
- What a consumer sees: `CameraSyncClient`'s two reads and the `SyncIdentity`
  they forward. What it must not see: no stub, no channel, no URL, no retry
  policy. Two flagged deviations from §2.3 as measured — the answer type IS a
  protoc message (`std::optional<argus::camera::v1::PullTableResponse>`), so
  protobuf types do cross this surface; and the package has **no `details/`
  directory**, the channel, the deadline and the metadata living inline in
  `src/camera/camera-sync-client.cc`.
- One failure mode, and it is silent: nullopt. A refusal, an unreachable
  argus-camera and a call whose answer arrived empty are three different things
  that a caller cannot tell apart here (nullopt, nullopt, and a value with no
  rows). The suite pins both sides of that line.
- On the wire: every call carries `x-argus-user`, `x-argus-role` and
  `x-argus-device` (`argus::client::addCallerIdentity`, the device leg engaged
  even when empty, which the sibling suite pins); `listCatalog` sends a
  default-constructed `ListCatalogRequest` (measured server-side:
  `ByteSizeLong()` is 0); one deadline, `kPullTimeoutMs` = 5000 ms, a
  `constexpr` in the `.cc` and not exported. No credential and no fleet secret:
  the sync client calls `setDeadline` and `addCallerIdentity` only.
- Config: the endpoint is runtime config, never a constant —
  `camera.grpc_target`, read by argus-sync
  (`services/sync/src/config/sync-config.cc:54`, resolved once into
  `SyncUpstreams`; the source is constructed either way and an empty target
  fails the dial, which the pull turns into the 503
  `SyncErrors::CameraSyncUnavailable`) and by argus-llm
  (`services/llm/src/main.cc:65`, the read then `if (!cameraTarget.empty())`).
  Three files declare it: `services/sync/config.toml.example:42-43`,
  `argus-deploy/config.sync.toml.example:42-43` and
  `argus-deploy/config.llm.toml.example:74-75` (both `argus-camera:7036`).
- The channel is plaintext: `argus::client::makeChannel` is
  `InsecureChannelCredentials`, so what protects this edge is the network, not
  this package.

## Tests

- `tests/unit/client-caller-identity-test.cc` — the pre-existing suite; one
  case, two subcases: the x-argus-* keys travel by presence, including an empty
  device header.
- `tests/unit/camera-sync-client-test.cc` — the second, differently aimed
  suite; three cases: the pull's branch and cursor as the server sees them plus
  the row that comes back; the catalog read's empty request; the five-second
  deadline measured server-side on both methods, and a scripted NOT_FOUND and a
  dead target each pinned to nullopt.
- The CMakeLists registers one target per suite, both with
  `EXCLUDE_FROM_ALL FALSE` because the folder is pulled in
  `EXCLUDE_FROM_ALL`: the pre-existing `client-caller-identity-test` (lines
  21-35, `add_test` at 35) and `camera-sync-client-test`. Because this folder
  is added by three of the gate's projects, ctest collects each suite three
  times — once per project that pulls the package in.
