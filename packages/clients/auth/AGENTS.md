# argus_clients_auth

The `argus.auth.v1` surface seen from the caller's side: two calls, the fleet
secret both carry, and the session verdict one of them maps back.

## What this is

A module, not a service: one `argus_clients(NAME auth ...)`, a STATIC library
whose include root is `src/`, so a consumer writes `<auth/auth-client.hxx>` and
links `argus::clients::auth`. It compiles one proto
(`argus/auth/v1/auth.proto`) and one source (`src/auth/auth-client.cc`).

Two calls, and they are the whole edge between a filter and the service that
owns sessions: `validateToken`, the question every authenticated request asks,
and `checkDeviceCredential`, the one the device credential mode adds. Until
Phase 3b-3 repoints `packages/lib/auth` they are asked of `argus.identity.v1`;
after it, of this client. Neither is an HTTP route: the `/auth/*` surface is the
app's, this is the fleet's.

`argus.auth.v1` declares its own `SessionUser` rather than importing
`identity.proto`'s `UserIdentity`, because a client package may not depend on
another client package (rule 25's tiers) and two generated copies of one message
would collide in a single binary. The two messages carry the same fields; the
auth end maps identity's answer into its own vocabulary, so a change on the
identity side never reshapes this wire.

## Layout

- `src/auth/auth-client.hxx` — `AuthClientConfig` (`target`, `fleetSecret`),
  `ValidateSessionInput` (`accessToken`, `deviceHash`, `hasDeviceContext`), and
  `AuthClient` with its two methods. `validateToken` answers the whole
  `ValidateTokenResponse` — the caller reads `valid()`, `reason()` and the
  session's user; `checkDeviceCredential` answers a `bool`.
- Nothing else: CMakeLists.txt, the two sources, the suite and this file. The
  channel, deadline and fleet secret ride inline in the `.cc`.

## Rules

- Rule 25: the folder IS the module. One `argus_clients(NAME auth ...)` with an
  explicit source list, never `file(GLOB)`.
- The include prefix is load-bearing: `<auth/auth-client.hxx>`. It is the same
  prefix `packages/contracts/auth` uses (`<auth/user-role.hxx>`), which is fine
  because the file names differ; a header named the same in both packages would
  not be.
- Neither call refuses anything locally: an empty token or secret hash is the
  service's to judge, and a filter that invented a refusal would answer a
  different question than the one the service answers.
- On the wire: the constructor's fleet secret rides every call as
  `x-argus-fleet` (`addFleetSecret`, skipped when empty, so an unset secret
  sends no header at all); `validateToken` engages the request's device leg
  whenever `hasDeviceContext` is set, empty hash included. One deadline,
  `kCallTimeoutMs` = 5000 ms, a `constexpr` in the `.cc`. The channel is
  plaintext (`makeChannel` is `InsecureChannelCredentials`), like every other
  gRPC leg in the fleet.
- Config: this package resolves nothing itself — the constructor takes the
  target and the fleet secret and the consumer reads them. Phase 3b-3 gives
  `packages/lib/auth` the consumer that needs it (`details/auth-access.cc`,
  beside the identity one), resolving `auth.target`, or `auth.rpc_host` and
  `auth.rpc_port` with `127.0.0.1:7043` as the fallback, plus `auth.rpc_secret`.
  Until then the only code that builds an `AuthClient` is this package's test:
  `argus-auth` links the target for the generated stub, which is what
  implements the server side.

## Tests

- `tests/unit/auth-grpc-client-test.cc` — three cases: the verdict, its reason,
  its expiry and the session's user mapped back field by field; the fleet secret
  and the device leg a call presents, with the credential answer following the
  server's `active`; and a listener that was shut down before the call, which
  answers neither nullopt nor false by accident — the client returns exactly
  that. The suite is registered `EXCLUDE_FROM_ALL FALSE` because the folder is
  pulled in `EXCLUDE_FROM_ALL`, and ctest collects it once per project that
  pulls this package in.
