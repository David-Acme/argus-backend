# argus_contracts_routes

The discovery vocabulary: how a service announces a route, and how a client
reads the announcement back.

## What this is

A CONTRACT, and a header-only one: three constants, no enum, no
dependencies. The include root is `src/`, so a consumer writes
`<routes/service-discovery.hxx>` and links `argus::contracts::routes`.

It is the C++ half of a wire that is **not** protobuf: the announcements
travel as DNS-SD records over mDNS, and the app parses them with the
platform's own resolver. What the two sides must agree on is therefore a
service type, an instance-label spelling and two TXT keys — nothing else.

## Layout

- `src/routes/service-discovery.hxx` —
  - `kServiceType = "_argus-route._tcp"`, the DNS-SD service every advertising
    service registers one instance of per logical route;
  - `kTxtPath`, the TXT key carrying the route's leading path segment
    (`camera`, `reminder`, …), or the empty string for the root route;
  - `kTxtHttps`, the TXT key that is present exactly when the announced
    listener terminates TLS, so a client knows to dial `https://`.

## Rules

- **One instance per route, not per host.** A service that owns two routes
  announces two instances under the same service type; the instance label is
  the service name joined to the route segment, which keeps the root route's
  label the bare service name. SRV carries host and port, so two instances of
  one host may name the same port.
- **Ports and paths are the only facts on the wire.** Discovery answers where
  a route lives, never what it means: a client still reads the route table
  (`packages/contracts/routes`, the frontend's `contracts` route table) for
  the paths it may call. Discovery supplies an endpoint per route; the manual
  server entry stays available as the fallback when nothing answers.
- **A path TXT value is a single leading segment**, never a full path and never
  a wildcard. A route whose path has no second segment announces the empty
  string, and a client treats that as the root.
- **TLS is announced, never assumed.** Presence of `kTxtHttps` means the
  announced port speaks TLS with the instance certificate; its absence means a
  cleartext listener that is not meant to be dialled off-host.
- Rule 25: the folder IS the module. One `argus_contracts(NAME routes ...)`
  with an explicit source list, never `file(GLOB)`.
- Tier 2: this package depends on nothing, and nothing in tier 1 may consume
  it — `packages/lib/mdns` takes the service type as a parameter for exactly
  that reason. The one reader is `packages/lib/http`, itself tier 2, in
  `src/http/route-announcements.cc:17-21`; the services call that function and
  never spell a constant of this package.

## Tests

- `tests/unit/route-discovery-test.cc` — the spellings are pinned verbatim
  (they are what an already-shipped client parses), and the service type is
  checked to be a well-formed DNS-SD name.
