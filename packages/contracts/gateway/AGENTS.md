# argus_contracts_gateway

The gateway's own refusals: the two camera-stream answers, the remote gate
and the unreachable route.

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

- `src/gateway/gateway-errors.hxx` — the four definitions in `GatewayErrors`:
  the two camera-stream answers (503 each), the gate (`RemoteNotAllowed`
  403) and `RouteUnreachable` (500). 3 files
  include it.

## Rules

- The gate carries its own code, not `Forbidden` alone:
  a client distinguishes "this path is LAN-only" from "you are not allowed" by
  the code, and the app switches on it. The 429 that answered a rate-limited
  remote attempt left with the limiter itself (Phase 3b-2 moved it into
  `argus-auth`, whose `TooManyAttempts` refuses), so this catalog holds no
  throttling vocabulary.
- `ServiceUnavailable` answers 503 here, as it does in every other catalog in
  the tree. That pairing is the convention a reviewer checks first.
- Rule 25: the folder IS the module. One `argus_contracts(NAME gateway ...)`
  with an explicit source list, never `file(GLOB)`.

## Tests

- `tests/unit/gateway-contract-catalog-test.cc` — the four refusals as a
  pinned table, each entry's wire legality, and that no two say the same
  thing.
