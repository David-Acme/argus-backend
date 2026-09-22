# argus_contracts_gateway

The gateway's own refusals: the two camera-stream answers, the remote gate,
the rate limiter and the unreachable route.

## What this is

A CONTRACT, not a service and not a library: one `argus_contracts`
declaration, an INTERFACE target with no translation unit. The include root is
`src/`, so a consumer writes `<gateway/gateway-errors.hxx>` and links
`argus::contracts::gateway`. `services/gateway` is the only consumer, and the
only owner: these are the refusals the gateway gives on its own behalf, before
or without a backend, and no other service has a reason to name them.

No `.proto` answers to this domain. The gateway is a proxy: what crosses its
wire is the envelope it forwards, not a message it defines.

## Layout

- `src/gateway/gateway-errors.hxx` — the five definitions in `GatewayErrors`:
  the two camera-stream answers (503 each), the two gates (`RemoteNotAllowed`
  403, `TooManyRemoteAttempts` 429) and `RouteUnreachable` (500). 3 files
  include it.

## Rules

- The two gates carry their own code, not `Forbidden`/`TooManyRequests` alone:
  a client distinguishes "this path is LAN-only" from "you are not allowed" by
  the code, and the app switches on it.
- `ServiceUnavailable` answers 503 here, as it does in every other catalog in
  the tree. That pairing is the convention a reviewer checks first.
- Rule 25: the folder IS the module. One `argus_contracts(NAME gateway ...)`
  with an explicit source list, never `file(GLOB)`.

## Tests

- `tests/unit/gateway-contract-catalog-test.cc` — the five refusals as a
  pinned table, each entry's wire legality, and that no two say the same
  thing.
