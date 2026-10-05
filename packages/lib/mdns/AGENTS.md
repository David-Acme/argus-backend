# argus-mdns

The announcement that makes the appliance findable on the LAN: one mDNS
responder over the vendored `mdns.h`, driven by the `mdns.*` config keys and by
the instances the caller hands it.

## What this is

A PACKAGE, not a service: no routes, no `main`, no database, nothing on the
wire except multicast UDP. It answers for a list of `MdnsInstance` values, one
record set each (PTR/SRV/TXT plus the host's A/AAAA), so a consumer announces
as many names and ports as it serves.

Its consumers are every app-facing service, and they reach it through
`packages/lib/http`'s `routeAnnouncements()`, not directly: that helper walks
the routes the service registered and builds one `_argus-route._tcp` instance
per logical route (TXT `path=<segment>`, `https="true"` when the listener
terminates TLS).

The tier rules are why the join lives in `lib/http`: this package is tier 1 and
the route vocabulary is `packages/contracts/routes` (tier 2), so `MdnsService`
takes the service type, the path and the TXT pairs as parameters and never
learns where they came from.

The keys are a contract with the rest of the installation, not private tuning:

- `mdns.name` is the hostname advertised, and `packages/lib/cert` puts it in the
  instance certificate's SAN (with `argus.local`, which is the name the app
  dials), so a rename without reissuing breaks TLS;
- `mdns.address` is the address the A/AAAA records carry, and an empty value
  enumerates the host's own interfaces. A bridge-networked container's
  interface IP is unreachable from the LAN, so `scripts/provision-host.sh`
  detects the host's LAN address (`--mdns-address` / `ARGUS_MDNS_ADDRESS`, else
  `ip route get 1.1.1.1`, else `hostname -I`) and writes it into every deploy
  config that carries the key. A value that is not an IP address is logged and
  dropped in favour of the interfaces;
- the SRV port is not a key any more. `mdns.port` is gone: a per-route
  announcement carries the route's own port, and `identity` answers pairing and
  invitation requests with `IdentityConfig::resolveAnnouncedPort()`, its own
  listener port, so the announcement and the pairing port agree by construction.
- `mdns.enabled` gates advertising only. `initialize()` is called either way and
  answers `true` when it is off.

## Layout

- `src/mdns/mdns-service.hxx` — `MdnsService`: `initialize`, `isAdvertising`,
  `shutdown`, `health`. The public header carries `jsoncpp` and nothing else.
- `src/mdns/mdns-service.cc` — the socket set, the PTR/SRV/A/AAAA/TXT records,
  the responder loop and the goodbye packet.
- `src/mdns/link-filter.hxx` — `mdns_link`: whether a query's source is on the
  local link (one of the host's interface subnets, IPv4 link-local, IPv6
  `fe80::/10` or loopback), read from `getifaddrs` with the netmasks.

## Rules

- Rule 25: the folder IS the module. One `argus_lib(NAME mdns ...)`
  declaration, explicit source lists, never `file(GLOB)`.
- `mdns::mdns` is PRIVATE: the vendored header is this package's business, so a
  consumer cannot include `mdns.h` by accident and does not inherit its
  SYSTEM-include workaround.
- The announcement is best-effort by design. `initialize()` answers `true` when
  advertising is disabled, when no interface resolves and when no socket opens,
  because a home appliance must boot and serve even when the network is not
  there to be announced on. Only a caller that must know asks
  `isAdvertising()`, and what it reports is liveness — the sockets open and the
  responder thread running — never delivery: the announce and the goodbye are
  multicast without a checked result, so nothing here can say the LAN saw them.
- The instance label is composed, not configured: `<name>-<path>` for a route
  and the bare name for an instance with no path, where `<name>` is
  `mdns.name` with dots folded to dashes. A DNS label is 63 octets, so the
  path suffix is kept whole and the name is truncated to fit it — otherwise a
  long name would truncate the route away and two routes of the same service
  would collide on the LAN.
- The responder owns its sockets and its thread; `shutdown()` joins the thread,
  sends the goodbye packet and closes the sockets, and it is idempotent. The
  destructor calls it, so a service that never calls it still leaves the LAN
  cleanly.
- The config is read once, in the constructor: `mdns.enabled`, `mdns.name` and
  `mdns.address`. Nothing re-reads it later, so a runtime
  `setBool("mdns.enabled", ...)` does not start or stop advertising.
- The service type is the caller's, and an instance that carries none is not
  advertisable: it is skipped rather than defaulted, and a list with nothing
  advertisable leaves advertising off while `initialize()` still answers
  `true`.
- A question from outside the local link is ignored (RFC 6762 §11): a unicast
  answer to it would make the responder an amplifier and leak the route list
  to anyone who can reach 5353/udp — a native install on a host with a public
  address. The interface subnets are re-read at most every 30 s when an
  unknown source arrives, so a DHCP change is picked up; the drops are logged
  once per thousand. `tests/unit/mdns-link-filter-test.cc` pins the rule.
