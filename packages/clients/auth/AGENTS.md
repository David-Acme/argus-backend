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
and `checkDeviceCredential`, the one the device credential mode adds. The
identity listener answered both until 3b-3 moved the verdict to `argus-auth`,
and it answers neither now. Neither is an HTTP route: the `/auth/*` surface is
the app's, this is the fleet's.

Four trees link it — `packages/lib/auth` (the filters, through
`filterAuthClient()`), `services/auth` (the stub that implements the server
side, plus its suites), `services/identity` (whose RPC service asks the same
verdict for its own callers) and this package's own suite.

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
- On the wire: `AuthClientConfig.credential`, the caller's own credential,
  rides every call as `x-argus-credential` (`addPeerCredential`); only when it
  is empty does the legacy `fleetSecret` ride as `x-argus-fleet`, and an unset
  pair sends no header at all (2026-10-05 audit, #25); `validateToken` engages the request's device leg
  whenever `hasDeviceContext` is set, empty hash included. One deadline,
  `kCallTimeoutMs` = 5000 ms, a `constexpr` in the `.cc`. The channel is
  plaintext (`makeChannel` is `InsecureChannelCredentials`), like every other
  gRPC leg in the fleet.
- Config: this package resolves nothing itself — the constructor takes an
  `AuthClientConfig` and the consumer reads it. The resolver lives with the
  first consumer, `filterAuthClient()` in
  `packages/lib/auth/src/auth/auth-access.cc`, reading `auth.target` (or
  `auth.rpc_host` / `auth.rpc_port`, `127.0.0.1:7043` as the fallback) plus
  `auth.credential` (paired with argus-auth's `[rpc.callers]` entry for the
  caller) and the legacy `auth.rpc_secret`; `argus-identity` holds the same helper's client beside its
  own RPC service.

## Tests

- `tests/unit/auth-grpc-client-test.cc` — three cases: the verdict, its reason,
  its expiry and the session's user mapped back field by field; the fleet secret
  and the device leg a call presents, with the credential answer following the
  server's `active`; and a listener that was shut down before the call, which
  answers neither nullopt nor false by accident — the client returns exactly
  that. The suite is registered `EXCLUDE_FROM_ALL FALSE` because the folder is
  pulled in `EXCLUDE_FROM_ALL`, and ctest collects it once per project that
  pulls this package in.
