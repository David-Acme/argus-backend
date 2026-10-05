# argus_clients_identity

The `argus.identity.v1` surface seen from the caller's side: the fleet secret
every call carries, the owner token a promotion adds, the profile legs a read
maps back, and the sync pull that reaches the same service.

## What this is

A module, not a service: one `argus_clients(NAME identity ...)`, a STATIC
library whose include root is `src/`, so a consumer writes
`<identity/identity-client.hxx>` and links `argus::clients::identity`. It
compiles three protos (`argus/identity/v1/identity.proto`,
`argus/identity/v1/sync.proto` and `argus/identity/v1/voiceprint.proto`) and
three sources (`src/identity/identity-client.cc`,
`src/identity/identity-sync-client.cc`, `src/identity/voiceprint-client.cc`).
Ten owner trees link it — `argus_lib_auth`
(`packages/lib/auth/CMakeLists.txt:65`), `argus-auth` (`services/auth:98`)
with its `auth-auth` (`src/feature/auth:19`) and `auth-session`
(`src/feature/session:20`) modules and its two client-reaching suites
(`tests:15,34`), the argus-notification tree (`services/notification:355`, its
`camera-notifier-test`) with its `notification-camera-notification` module
(`src/feature/camera-notification:12`), the `argus-guard`
executable (`services/guard:108`) with its `guard` module
(`src/feature/guard:25`), `argus-llm`
(`services/llm:167`), `argus_identity-rpc`
(`services/identity/src/app/rpc/CMakeLists.txt:18`) with its
`identity-sync-rpc-test` suite (`services/identity/tests:53`), `sync-transport`
(`services/sync/src/feature/transport:20`) and `voice-core`
(`services/voice:88`) — plus `argus-camera`, whose `operator` module links it
for the known-person matcher (`src/feature/operator:21`), and
`services/productivity`, which links it only from
its `productivity-controller-test` target (`services/productivity:196`). All
ten of those trees also add the package to their standalone build by path (the
nine services above and `packages/lib/auth`). The auth library is the one that
spreads it furthest: two of its files include the header
(`details/identity-access.cc`, `user-directory-identity.cc`; the matching
`.hxx` forward-declares `IdentityClient` only), so a service that links
`argus::lib::auth` reaches the identity RPC through this package without a
link line of its own (argus-notification is one: it links the library and
names the package itself, in its own tree and in its `camera-notification`
module). 27 C++ files include the
header: the in-package suite, two in `packages/lib/auth`, five in
`services/auth`, two in `services/camera`, eleven in `services/guard`, one in
`services/llm`, three in `services/notification` and two in `services/voice`.

## Layout

- `src/identity/identity-client.hxx` — the input structs `UpdateUserNameInput`,
  `RegisterUserInput`, `EnrollPersonInput`, `TagPersonInput` and
  `PromotePersonInput`, the one local DTO `PersonProfile`, and `IdentityClient`
  with its twelve methods (`updateUserName`, `registerUser`, `getUser`,
  `listPersons`, `identifyPerson`, `enrollPerson`, `touchPerson`,
  `promotePerson`, `tagPerson`, `personTags`, `getPerson`,
  `listNotifiableUsers`); 27 files include it.
- `src/identity/identity-sync-client.hxx` — `IdentitySyncClient`, the second
  stub (`argus.identity.v1.SyncService`), one method `pullTable`. Its
  production consumer is `services/sync`'s `identity-sync-gateway`,
  constructed at `services/sync/src/app/main.cc:80`; identity's own
  `identity-sync-rpc-test.cc` drives it directly against the served method.
- `src/identity/voiceprint-client.hxx` — `VoiceprintClient`, the third stub
  (`argus.identity.v1.VoiceprintService`): `identify`, `identifyWithin` (the
  same call under the caller's own deadline,
  `VoiceprintIdentifyInput{sample, timeoutMs}`, for a voice turn that cannot
  wait 5 s), `observeTurn` (`VoiceTurnObservation{sample, userId,
  deviceHash, callKey, timeoutMs}`: the same answer as `identify`, and
  identity learns the holder's voice from the turn) and `closeCall`
  (`VoiceCallClose{callKey, timeoutMs}`, true when identity had the call
  open), with `VoiceClipView` (a `std::span<const int16_t>` and its sample
  rate — the caller's samples are copied once, into the wire bytes),
  configured by `VoiceprintClientConfig{target, fleetSecret}`. argus-voice
  (calls) is its consumer; guard's visitor dialogues can use `identify`.
- Nothing else: the folder is CMakeLists.txt, the six sources, the two suites
  and this file. No `details/` directory: the channel, deadline, fleet secret and
  metadata ride inline in the `.cc` files.

## Rules

- Rule 25: the folder IS the module. One `argus_clients(NAME identity ...)` with
  an explicit source list, never `file(GLOB)`.
- The include prefix is load-bearing: `<identity/identity-client.hxx>`.
- Three stubs, three clients, none wrapping another: the identity surface
  (`IdentityService`), the sync pull (`SyncService`) and the voiceprint
  surface (`VoiceprintService`). A consumer that needs more than one holds
  each.
- The voiceprint wire: a clip is mono 16-bit little-endian PCM at its own
  rate (8-48 kHz), encoded byte by byte so the host's endianness never
  leaks. Every call carries the fleet secret only: the caller's account and
  device in `observeTurn` are the ones argus-sync's filters bound to the
  call's socket, vouched for by the fleet. `observeTurn` refuses locally a
  clip with no samples or an out-of-range rate, a missing user, an empty or
  oversized device hash and a call key that is empty or over 128 characters;
  `closeCall` an empty key. One deadline per call, chosen by the caller.
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
- On the wire: the caller's own credential rides every call as
  `x-argus-credential` (`addPeerCredential` over the
  `argus::client::PeerCredential` the constructor takes, `IdentitySyncClientConfig`
  and `VoiceprintClientConfig` carry); only when that credential is empty does
  the legacy fleet secret ride instead as `x-argus-fleet`, and an unset pair
  sends no header at all (2026-10-05 audit, #25). The string-only constructor
  is that legacy form; `x-argus-user` and `x-argus-role` ride only
  `updateUserName`; `promotePerson` adds `authorization: Bearer <accessToken>`
  and `x-argus-device` only when the deviceHash is non-empty. One deadline per
  call, `kCallTimeoutMs` = 5000 ms (the sync pull's `kPullTimeoutMs` is the
  same), a `constexpr` in the `.cc`. The channel is plaintext (`makeChannel` is
  `InsecureChannelCredentials`).
- Config: `identity.target` in the eight
  `argus-deploy/config.{auth,camera,guard,llm,notification,productivity,sync,voice}.toml.example`
  trees (`argus-identity:7040` from the compose containers, `127.0.0.1:7040`
  in a native-dev tree) with the caller's own
  `identity.credential` beside it in each (paired with identity's
  `[rpc.callers]` entry for that caller; an older install's
  `identity.rpc_secret` is still read as the legacy fallback), and in six project templates
  (`services/{auth,guard,notification,productivity,sync,voice}/config.toml.example`,
  all `127.0.0.1:7040`). The auth library and argus-auth also accept
  `identity.rpc_host` / `identity.rpc_port`
  (`packages/lib/auth/src/auth/details/identity-access.cc:17-18`,
  `services/auth/src/config/auth-config.cc:58-61`), defaulting to
  `127.0.0.1:7040` — no template spells those two keys any more. The other
  callers read neither: argus-notification takes `identity.target` straight
  from its config and warns instead of dialling when it is empty
  (`services/notification/src/app/main.cc:148`).
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
- `tests/unit/voiceprint-client-test.cc` — two cases: the local refusals
  (no call reaches the scripted server until a valid one does) and a call
  turn's wire (caller, device, call key, the little-endian samples and their
  rate, the fleet secret without a bearer) followed by its close. Registered
  as `identity-voiceprint-client-test`.
- The CMakeLists registers it as `identity-grpc-client-test`, with
  `EXCLUDE_FROM_ALL FALSE` because six of the pulls that reach it are
  `EXCLUDE_FROM_ALL` (guard, llm, notification, productivity, sync, voice; auth
  and identity pull it plainly). Because eight of the gate's projects add the
  folder, ctest collects the suite eight times — once per project that pulls
  the package in.
