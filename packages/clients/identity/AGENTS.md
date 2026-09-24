# argus_clients_identity

The `argus.identity.v1` surface seen from the caller's side: the fleet secret
every call carries, the owner token a promotion adds, the profile legs a read
maps back, and the sync pull that reaches the same service.

## What this is

A module, not a service: one `argus_clients(NAME identity ...)`, a STATIC
library whose include root is `src/`, so a consumer writes
`<identity/identity-client.hxx>` and links `argus::clients::identity`. It
compiles two protos (`argus/identity/v1/identity.proto` and
`argus/identity/v1/sync.proto`) and two sources
(`src/identity/identity-client.cc`, `src/identity/identity-sync-client.cc`).
Nine owner trees link it — `argus_lib_auth`
(`packages/lib/auth/CMakeLists.txt:57`), `argus-auth` (`services/auth:98`)
with its `auth-auth` (`src/feature/auth:19`) and `auth-session`
(`src/feature/session:20`) modules and its two client-reaching suites
(`tests:15,34`), `gateway-core` (`services/gateway:104`), the `argus-guard`
executable (`services/guard:116`) with its `guard` (`src/feature/guard:21`) and
`guard-api` (`src/feature/api/guard:19`) modules, `argus-llm`
(`services/llm:183`), `argus_identity-rpc`
(`services/identity/src/app/rpc/CMakeLists.txt:18`) with its
`identity-sync-rpc-test` suite (`services/identity/tests:53`), `sync-transport`
(`services/sync/src/feature/transport:20`) and `voice-core`
(`services/voice:88`) — plus `services/productivity`, which links it only from
its `productivity-controller-test` target (`services/productivity:244`). Nine
of those trees also add the package to their standalone build by path (the
eight services above and `packages/lib/auth`). The auth library is the one that
spreads it furthest: two of its files include the header
(`details/identity-access.cc`, `user-directory-identity.cc`; the matching
`.hxx` forward-declares `IdentityClient` only), so a service that links
`argus::lib::auth` — argus-camera and argus-notification among them — reaches
the identity RPC through this package without a link line of its own (measured:
neither CMakeLists names `argus::clients::identity`). 26 C++ files include the
header: the in-package suite, two in `packages/lib/auth`, five in
`services/auth`, two in `services/camera`, two in `services/gateway`, eleven in
`services/guard`, one in `services/llm` and two in `services/voice`.

## Layout

- `src/identity/identity-client.hxx` — the input structs `UpdateUserNameInput`,
  `RegisterUserInput`, `EnrollPersonInput`, `TagPersonInput` and
  `PromotePersonInput`, the one local DTO `PersonProfile`, and `IdentityClient`
  with its twelve methods (`updateUserName`, `registerUser`, `getUser`,
  `listPersons`, `identifyPerson`, `enrollPerson`, `touchPerson`,
  `promotePerson`, `tagPerson`, `personTags`, `getPerson`,
  `listNotifiableUsers`); 26 files include it.
- `src/identity/identity-sync-client.hxx` — `IdentitySyncClient`, the second
  stub (`argus.identity.v1.SyncService`), one method `pullTable`. Its one
  consumer is `services/sync`'s `identity-sync-gateway`.
- Nothing else: the folder is CMakeLists.txt, the four sources, the suite and
  this file. No `details/` directory: the channel, deadline, fleet secret and
  metadata ride inline in the `.cc` files.

## Rules

- Rule 25: the folder IS the module. One `argus_clients(NAME identity ...)` with
  an explicit source list, never `file(GLOB)`.
- The include prefix is load-bearing: `<identity/identity-client.hxx>`.
- Two stubs, two clients, neither wrapping the other: the identity surface
  (`IdentityService`) and the sync pull (`SyncService`). A consumer that needs
  both holds both.
- What a consumer sees: twelve methods returning `std::optional` or `bool`,
  and `PersonProfile` as the one local DTO — `getPerson` always maps personId,
  name, alias, observation, role and tags, maps `userId` only when the wire
  carries `has_user_id()`, and answers nullopt when the answer has no `person`
  at all. What it must not see: no stub, no channel, no URL, no retry policy.
  Two flagged deviations as measured: most answer types ARE protoc messages, so
  protobuf types cross this surface, and §2.3's `details/` split is absent.
- The refusals stay local: `getUser`, `touchPerson`, `tagPerson`, `personTags`
  and `getPerson` refuse a non-positive id; `identifyPerson`, `enrollPerson` and
  `registerUser` refuse an empty image; `promotePerson` refuses a non-positive
  id or an empty accessToken. None of them opens a socket.
- The session and device-credential legs are NOT here: `validateToken` and
  `checkDeviceCredential` belong to `packages/clients/auth`, since argus-auth
  owns the session tables (3b).
- On the wire: the constructor's fleet secret rides every call as
  `x-argus-fleet` (`addFleetSecret`, skipped when empty, so an unset secret
  sends no header at all); `x-argus-user` and `x-argus-role` ride only
  `updateUserName`; `promotePerson` adds `authorization: Bearer <accessToken>`
  and `x-argus-device` only when the deviceHash is non-empty. One deadline per
  call, `kCallTimeoutMs` = 5000 ms (the sync pull's `kPullTimeoutMs` is the
  same), a `constexpr` in the `.cc`. The channel is plaintext (`makeChannel` is
  `InsecureChannelCredentials`).
- Config: `identity.target` in the eight `argus-deploy/config.{auth,camera,gateway,guard,llm,productivity,sync,voice}.toml.example`
  trees (`argus-identity:7040` from the compose containers, `127.0.0.1:7040`
  in the host-networked gateway) with
  `identity.rpc_secret` beside it in each, and in six project templates
  (`services/{auth,gateway,guard,productivity,sync,voice}/config.toml.example`,
  all `127.0.0.1:7040`). The auth library and argus-auth also accept
  `identity.rpc_host` / `identity.rpc_port`
  (`packages/lib/auth/src/auth/details/identity-access.cc:17-18`,
  `services/auth/src/config/auth-config.cc:74-77`), defaulting to
  `127.0.0.1:7040` — no template spells those two keys any more. The gateway
  reads neither: it takes `identity.target` straight from its config and warns
  instead of dialling when it is empty (`services/gateway/src/main.cc:330`).
- The service builds and links its own client (`services/identity/CMakeLists.txt:103`
  adds the folder by path; its `argus_identity-rpc` module links it at
  `src/app/rpc/CMakeLists.txt:18`): the wire vocabulary is shared between the
  two ends, not copied.

## Tests

- `tests/unit/identity-grpc-client-test.cc` — three cases: the local refusals
  with a live server as their control (every answer it serves is a success, so a
  value coming back would prove the client dialled); the fleet secret, the Bearer
  token and the device binding a promotion presents; the optional legs of a
  read's answer (`has_person`, `has_user_id`) deciding the profile. The target
  carries `grpc` in its name because `packages/lib/sqlite` already registers a
  suite called `identity-client-test`; ctest allows the duplicate, and the
  rename keeps the ledger readable.
- The CMakeLists registers it as `identity-grpc-client-test`, with
  `EXCLUDE_FROM_ALL FALSE` because six of the pulls that reach it are
  `EXCLUDE_FROM_ALL` (gateway, guard, llm, productivity, sync, voice; auth and
  identity pull it plainly). Because eight of the gate's projects add the
  folder, ctest collects the suite eight times — once per project that pulls
  the package in.
