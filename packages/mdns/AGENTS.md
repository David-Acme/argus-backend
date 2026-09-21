# argus-mdns

The announcement that makes the appliance findable on the LAN: one mDNS
responder over the vendored `mdns.h`, driven by the `mdns.*` config keys.

## What this is

A PACKAGE, not a service: no routes, no `main`, no database, nothing on the
wire except multicast UDP. It has exactly one consumer today — the gateway,
which advertises on boot and shuts the responder down on exit. Extracting it
from `services/gateway` changed no behaviour: the announcement was already
gateway-only (`argus::mdns` is a source module, not a service's private code),
so the move is what makes that fact visible in the build graph.

The keys are a contract with two other services, not private tuning:

- `mdns.name` is the hostname advertised, and `packages/cert` puts it in the
  instance certificate's SAN, so a rename without reissuing breaks TLS;
- `mdns.port` is what `identity` answers pairing and invitation requests with,
  so the announcement and the pairing port must agree.

## Layout

- `src/mdns/mdns-service.hxx` — `MdnsService`: `initialize`, `isAdvertising`,
  `shutdown`, `health`. The public header carries `jsoncpp` and nothing else.
- `src/mdns/mdns-service.cc` — the socket set, the PTR/SRV/A/AAAA/TXT records,
  the responder loop and the goodbye packet.

## Rules

- Rule 25: the folder IS the module. One `argus_module(NAME mdns ...)`
  declaration, explicit source lists, never `file(GLOB)`.
- `mdns::mdns` is PRIVATE: the vendored header is this package's business, so a
  consumer cannot include `mdns.h` by accident and does not inherit its
  SYSTEM-include workaround.
- The announcement is best-effort by design. `initialize()` answers `true` when
  advertising is disabled, when no interface resolves and when no socket opens,
  because a home appliance must boot and serve even when the network is not
  there to be announced on. Only a caller that must know asks
  `isAdvertising()`.
- The responder owns its sockets and its thread; `shutdown()` is the only way
  they are released, and it is idempotent.
