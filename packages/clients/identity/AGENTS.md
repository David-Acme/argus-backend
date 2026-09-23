# argus_clients_identity

The `argus.identity.v1` surface seen from the caller's side: the fleet secret
every call carries, the owner token a promotion adds, and the profile legs a
read maps back.

## What this is

A module, not a service: one `argus_clients(NAME identity ...)`, a STATIC
library whose include root is `src/`, so a consumer writes
`<identity/identity-client.hxx>` and links `argus::clients::identity`. It
compiles one proto (`argus/identity/v1/identity.proto`) and one source
(`src/identity/identity-client.cc`). Eight packages link it — `gateway-core`
(`services/gateway/CMakeLists.txt:171`), `argus-guard` (`services/guard:140`),
`argus_guard` (`services/guard/src/feature/guard/CMakeLists.txt:19`),
`argus_guard-api` (`services/guard/src/feature/api/guard/CMakeLists.txt:19`),
`argus-llm` (`services/llm:210`), `argus_voice-core` (`services/voice:100`),
`argus_lib_auth` (`packages/lib/auth:68`) and `argus_identity` itself
(`packages/identity:222`). Six of those CMakeLists also add the package to their
own standalone tree by path. The auth library is the one that spreads it
furthest: four of its files include the header (`details/identity-access.cc`,
`jwt-filter.cc`, `device-filter.cc`, `user-directory-identity.cc`; the matching
`.hxx` forward-declares `IdentityClient` only), so a service that links
`argus::lib::auth` — argus-camera and argus-notification among them — reaches
the identity RPC through this package without a link line of its own (measured:
neither `CMakeLists.txt` names `argus::clients::identity`). 21 C++ files include the
header: the in-package suite, four in `packages/lib/auth`, two in
`services/camera`, eleven in `services/guard`, one in `services/llm` and two in
`services/voice`.

## Layout

- `src/identity/identity-client.hxx` — the input structs `UpdateUserNameInput`,
  `ValidateTokenInput`, `EnrollPersonInput`, `TagPersonInput` and
  `PromotePersonInput`, the one local DTO `PersonProfile`, and `IdentityClient`
  with its thirteen methods (`updateUserName`, `validateToken`, `getUser`,
  `listPersons`, `checkDeviceCredential`, `identifyPerson`, `enrollPerson`,
  `touchPerson`, `promotePerson`, `tagPerson`, `personTags`, `getPerson`,
  `listNotifiableUsers`); 21 files include it.
- Nothing else: `find packages/clients/identity -type f` returns
  CMakeLists.txt, the two sources, the suite and this file. No `details/`
  directory: the channel, deadline, fleet secret and metadata ride inline in the
  `.cc`.

## Rules

- Rule 25: the folder IS the module. One `argus_clients(NAME identity ...)` with
  an explicit source list, never `file(GLOB)`.
- The include prefix is load-bearing: `<identity/identity-client.hxx>`.
- What a consumer sees: thirteen methods returning `std::optional` or `bool`,
  and `PersonProfile` as the one local DTO — `getPerson` always maps personId,
  name, alias, observation, role and tags, maps `userId` only when the wire
  carries `has_user_id()`, and answers nullopt when the answer has no `person`
  at all. What it must not see: no stub, no channel, no URL, no retry policy.
  Two flagged deviations as measured: most answer types ARE protoc messages, so
  protobuf types cross this surface, and §2.3's `details/` split is absent.
- The refusals stay local: `getUser`, `touchPerson`, `tagPerson`, `personTags`
  and `getPerson` refuse a non-positive id; `identifyPerson` and `enrollPerson`
  refuse an empty image; `promotePerson` refuses a non-positive id or an empty
  accessToken. None of them opens a socket.
- On the wire: the constructor's fleet secret rides every call as
  `x-argus-fleet` (`addFleetSecret`, skipped when empty, so an unset secret
  sends no header at all); `x-argus-user` and `x-argus-role` ride only
  `updateUserName`; `promotePerson` adds `authorization: Bearer <accessToken>`
  and `x-argus-device` only when the deviceHash is non-empty, while
  `validateToken` engages the request's device leg whenever `hasDeviceContext`
  is set, empty hash included. One deadline, `kCallTimeoutMs` = 5000 ms, a
  `constexpr` in the `.cc`. The channel is plaintext (`makeChannel` is
  `InsecureChannelCredentials`).
- Config: `identity.target` in nine files — the six
  `argus-deploy/config.{camera,guard,llm,notification,productivity,voice}.toml`
  and `services/{notification,productivity,voice}/config.toml` (all `:7040`) —
  and `identity.rpc_secret` in the seven argus-deploy trees. The auth library
  falls back to `identity.rpc_host` / `identity.rpc_port`
  (`services/gateway/config.toml:57-58` and
  `argus-deploy/config.gateway.toml:56-57`, both 7040 — the gateway is the one
  consumer that never sets `identity.target`), and `resolveTarget` defaults to
  `127.0.0.1:7040` when the port is not positive.
- The service links its own client (`packages/identity/CMakeLists.txt:189`): the
  wire vocabulary is shared between the two ends, not copied.

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
  `EXCLUDE_FROM_ALL FALSE` because the folder is pulled in
  `EXCLUDE_FROM_ALL`. Because that folder is added by five of the gate's
  projects, ctest collects the suite five times — once per project that pulls
  the package in.
