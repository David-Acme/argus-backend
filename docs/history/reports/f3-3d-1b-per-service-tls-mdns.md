# Phase 3d step 1b — every app-facing service terminates its own TLS and announces its routes

Each of the eight services the app talks to now opens its own TLS listener with
the one instance certificate and announces one `_argus-route._tcp` mDNS
instance per logical route it registers, each carrying its own SRV port and the
`path` / `https` TXT keys. The single-instance `MdnsService` that only the
gateway used becomes a multi-instance responder, and the route vocabulary moves
into a new tier-2 contract so that a service announcing its routes and a client
resolving them spell the same strings.

This is the second half of Phase 3d step 1. Step 1c deletes `services/gateway`,
its proxy and `contracts/gateway`; that deletion is only safe once every service
serves the app on its own certificate, which is what this unit lands. The
gateway keeps its public listener and, additionally, its legacy `_argus._tcp`
record, so the deployed app's first-match discovery is untouched through the
transition.

## What existed before

- **One TLS listener in the tree that the app could reach.** `ListenerConfig::resolveTls`
  read the gateway's own keys (`gateway.host`, `gateway.port`, `gateway.plain`,
  `gateway.min_protocol`) plus the global `cert.server_cert` / `cert.server_key`
  and was the only resolver that produced a certificate-bearing listener. The
  three other services that terminated TLS — auth, identity and sync — each
  hand-rolled the same nine lines in their own config class
  (`AuthConfig::resolveListener`, `IdentityConfig::resolveListener`,
  `SyncConfig::resolveListener`), with their own default host, their own port
  default and their own copy of `certs/server.pem`.
- **Four app-facing services had no TLS at all.** Camera (7026), productivity
  (7027), notification (7028) and guard (7039) resolved `ListenerConfig::resolve(port)`
  — `server.host` (default `127.0.0.1`), `server.port`, no certificate — and the
  compose publish was `127.0.0.1:<port>:<port>`. The gateway reached them over
  plain HTTP on host loopback; the app never did.
- **Discovery was one record for one port.** `MdnsService` was a single-instance
  responder that read `mdns.service_type`, `mdns.port` and the whole `[mdns.txt]`
  map from config, and only the gateway constructed one. The app's two clients
  (React Native and tauri) browse `_argus._tcp.`, take the first resolved
  instance and dial `https://<ip>:<srv port>` with `https` hardcoded — they
  never read TXT, so nothing in the fleet could have announced a second surface
  to them anyway.
- **A config template that could not be adopted correctly.** `scripts/lib/common.sh`'s
  `adopt_template_key` funnelled the template's value through
  `replace_toml_value`, which always wraps its value in quotes: adopting a bare
  `enabled = true` wrote `enabled = "true"`, which `ConfigService::getBool`
  reads as the default `false`. The wiring keys had this since
  `device.trust_forwarded_for` was added; this unit's `mdns enabled` line would
  have inherited it and silently disabled advertising on every config written
  before the key existed.

## The decision, as built

### The route vocabulary is a contract

`packages/contracts/routes/` is the new tier-2 package
(`argus::contracts::routes`, header-only through `argus_contracts`), holding
`src/routes/service-discovery.hxx`: the service type `_argus-route._tcp` and the
TXT keys `path` and `https`. A contract declares what crosses a boundary and
nothing else, so it carries no resolver, no builder and no config read — it is
the spelling a service announces with and a client resolves by, and the client
that will resolve it (Phase 5 step 6) reads the same header. Its own
`route-discovery-test` pins the spellings.

### The join between routes and instances lives in `lib/http`

A tier-1 package may not consume a contract, and `lib/mdns` is tier 1, so
`MdnsService` takes the service type, the path and the TXT pairs as parameters
and never learns where they came from. The package that knows both the
registered routes and the listener's TLS state is `lib/http` (tier 2), and
that is where the join sits:

- `ListenerConfig::resolveServiceTls(section, defaultPort)` — the app-facing
  listener contract: `<section>.host` (default `0.0.0.0`), `<section>.port`,
  `<section>.plain` (absent means TLS on), `<section>.min_protocol`
  (`TLSv1.2`), with the certificate from the global `[cert]`
  (`certs/server.pem` / `certs/server.key`). The gateway-shaped `resolveTls` is
  gone and the three hand-rolled resolvers collapsed into this one, so eight
  services now share a single definition of "my app-facing listener".
- `logicalRoutes()` / `logicalRoutesFrom(patterns)` — the first path segment of
  every handler Drogon reports, deduplicated and sorted, with `/health`, `/`
  and any pattern that is not `/`-rooted skipped. The derivation is a pure
  function of the pattern list, which is what makes it testable without a live
  Drogon app.
- `routeAnnouncements(input)` / `routeAnnouncementsFor(routes, input)` — one
  `MdnsInstance` per route: `serviceType = routes::kServiceType`, `path = route`,
  `port = input.port`, and TXT `path=<route>` plus `https="true"` when
  `input.tls`.

`packages/lib/http/tests/unit/logical-routes-test.cc` is new and pins all three:
the derivation (including that `/`, `//`, `""` and a non-slash pattern yield
nothing), the instance shape per route, and that a plain listener announces
without the `https` key.

### The responder becomes multi-instance

`MdnsService` now takes `std::vector<MdnsInstance>` and answers each with its own
PTR/SRV/TXT record set over the host's A/AAAA records. Two details are load
bearing:

- **The instance label is composed, not configured.** A route's instance is
  `<mdns.name>-<path>` and a pathless instance is the bare name, with dots in
  the configured name folded to dashes. A DNS label is 63 octets, so the path
  suffix is kept whole and the name is truncated to fit it — a long name would
  otherwise truncate the route away and make two routes of the same service
  collide on the LAN.
- **Advertise, then report liveness — never delivery.** `initialize()` stays
  best-effort and answers `true` when advertising is off, when nothing is
  advertisable or when no socket opens; the announce and the goodbye are
  multicast without a checked result. The eight call sites therefore call it
  without inspecting the return, and the package documents `isAdvertising()` as
  liveness rather than delivery.

`mdns.service_type`, `mdns.port` and `[mdns.txt]` are no longer read by the
package. `mdns.port` still matters, but as the *identity* service's pairing port
(the port its pairing answers publish), which is that service's own config read
and not the responder's.

### The eight services

Every app-facing service resolves its listener through `resolveServiceTls` on
its own section and announces its own routes from a beginning advice, so the
announcement is built after the routes are registered:

```cpp
std::unique_ptr<MdnsService> mdnsService;
drogon::app().registerBeginningAdvice([&mdnsService, &listener]() {
  mdnsService = std::make_unique<MdnsService>(
      routeAnnouncements({.port = listener.port, .tls = listener.tls}));
  mdnsService->initialize();
});
```

| Service | Section / port | Routes it announces (from its own handlers) |
|---|---|---|
| argus-auth | `[auth]` 7042 | `auth` |
| argus-identity | `[identity]` 7044 | `invitation`, `pairing`, `portrait-preview`, `user` |
| argus-sync | `[sync]` 7025 | `sync` |
| argus-camera | `[camera]` 7026 | `camera`, `media`, `zone` |
| argus-productivity | `[productivity]` 7027 | `calendar-event`, `calendar-event-share`, `project`, `project-member`, `project-task` |
| argus-notification | `[notification]` 7028 | `notification`, `notification-token` |
| argus-guard | `[guard]` 7039 | `guard` |
| argus-gateway | `[gateway]` 7024 | every pattern it proxies (`auth`, `camera`, `camera-stream`, `guard`, `invitation`, `media`, `notification`, `notification-token`, `pairing`, `portrait-preview`, `project`, `project-member`, `project-task`, `user`, `zone`, …), **plus** the legacy `_argus._tcp` record |

The gateway's legacy record is built as the `[mdns.txt]` map it used to carry
(`path=/`, `https=true`, `wss=true`), so the transitional record is
byte-identical to what discovery returned before this unit and the deployed app
sees no change. `routeAnnouncements` appends it first, so a first-match client
resolving `_argus._tcp` still lands on the public port.

### The deploy surface

The compose stack mounts `${ARGUS_CERTS_DIR:-../certs}` read-only at
`/opt/argus/certs` in each app-facing service and its healthcheck became
`curl -kfs https://127.0.0.1:<port>/health`. Each deploy template gained the
service's listener section (`host`, `port`, `plain = false`, `min_protocol`),
`[cert]`, and `[mdns] enabled/name`; the `[server] port` that used to be the
HTTP listener became the explicit `server.grpc_port` of the internal wire. The
camera template's `[identity]` block lost eight recognition keys it never
carried the reader for and `[streaming] relay_chunk_bytes` went with them
(nothing in the tree reads it).

### Provisioning

`ensure_instance_certs` now runs **after** `ensure_deploy_configs` in
`scripts/provision-host.sh`: the certificate's SAN is built from the operator's
`mdns.name`, and the file that name is read from did not exist yet on a fresh
host, so a first provision always minted `Argus` regardless of the configured
name.

`adopt_wiring_keys` gained the `mdns enabled` line every other app-facing
wiring key already had, and the adoption path was fixed to write a value the way
the template spells it. `toml_literal()` reads a key's raw right-hand side
(quotes intact), `replace_toml_value` takes a `literal` mode as its fifth
argument (default `quoted`, so all ten existing call sites keep their behaviour
byte for byte), and `adopt_template_key` now:

- adopts a key the config lacks, written as the template spells it (`enabled = true`,
  not `enabled = "true"`);
- leaves a key the config already carries alone, with one exception: a value
  that is the *quoted mirror* of the template's literal
  (`enabled = "true"` against a template's `enabled = true`) is repaired, because
  that shape is exactly what the old quoting bug wrote, and `getBool`/`getInt`
  read it as the default. A value an operator chose is never touched, and a
  string-valued template key cannot reach the repair arm at all — its literal
  already carries its quotes.

`toml_value()` is now a wrapper over `toml_literal()` (strip a matching
quote pair, print) rather than a second copy of the same awk scanner, so the
file has one TOML scalar reader instead of two.

## Consequences recorded, not hidden

- **The announced address is not yet reachable from the LAN.** Six of the eight
  services still publish on host loopback, so the address the announcement
  carries (the container's bridge IP) cannot be dialled from the LAN; argus-sync
  (7025) and the host-networked gateway are the two exceptions. This is
  coherent with the plan — the app keeps following the gateway's `_argus._tcp`
  record through this unit — and Phase 3d step 1c flips the six publishes and
  settles the advertised address. Phase 5 step 6 verifies discovery against a
  real client. Recorded in `argus-deploy/CONTEXT.md` next to the port map.
- **A dev run of a TLS service cannot resolve both its config and its
  certificate from one working directory.** `ConfigService::load("config.toml")`
  and the `certs/server.pem` default are both cwd-relative, while the dev PKI
  lives only at `$ROOT/certs` (`scripts/setup.sh` calls `ensure_instance_certs`
  once, for the gateway). Every service's `.gitignore` already lists `certs/`,
  so a per-service `certs -> ../../certs` link is the obvious fix, but it is a
  `setup.sh` change this unit did not make: it widens an existing gap from four
  services to eight rather than creating it. Native development is unaffected
  for `plain = true` sections and for any service run with a certificate path in
  its own config.
- **The gateway's legacy record is a transitional duplicate.** Both the app's
  clients take the first resolved instance, so until 1c deletes the gateway the
  app finds the public port and not the per-route records — which is the point,
  and also means this unit's announcements are unobserved by the app until
  then.
- **An existing per-installation `config.camera.toml` keeps its `[grpc]` copy
  of the eight recognition keys.** They were never read from that table (`[grpc]
  caller_guard` is the only key the camera reads there), so the running
  behaviour of such an installation is unchanged and the defaults apply; the
  keys are documented under `identity.identify` and its seven siblings now.
  Nothing rewrites a gitignored config in place, and no migration helper was
  added for a table move whose old location was inert.
- **A deploy config written before this unit still spells four upstreams plain.**
  `ensure_deploy_configs` copies a template only when the file is absent and
  `adopt_wiring_keys` only fills keys the file lacks, so a per-installation
  `argus-deploy/config.gateway.toml` that predates this unit keeps
  `http://127.0.0.1:{7026,7027,7028,7039}` and `ws://127.0.0.1:7026/media` for
  the four services this unit made TLS-only, and the gateway's proxy to them
  fails until those five lines are edited (`https://` / `wss://`, the spelling
  the deploy template already carries) or the file is regenerated. The same
  holds for a native `services/gateway/config.toml`. No migration
  helper was written: 1c deletes the gateway and the keys with it, so a helper
  would be dead code in the next unit. Both host files are gitignored and were
  left untouched. The same key's description in
  `docs/architecture/wire-camera-media.md` was stale in both halves — it read
  `ws://argus-camera:7026/media`, a host the deploy template had already
  stopped using — and now reads `wss://127.0.0.1:7026/media`.
- **The camera's native template still leaves `[identity]` out.** Its
  documentation already presents the block as optional ("not in the template"),
  and `identity.identify` defaults to off, so the template stays as it was; the
  deploy template is the one that carries the keys because the deployed guard
  is the configuration that uses them.
- **`mdns.name` remains a two-way contract.** `packages/lib/cert` puts it in the
  instance certificate's SAN and every service advertises it, so renaming
  without reissuing still breaks TLS; the announcement now also embeds it in
  eight instance labels rather than one.

## Review

Three fresh adversarial reviewers read the finished unit against the tree,
without the implementation's own reasoning. Each finding was re-verified against
the source before anything changed; what follows is what that verification
concluded.

Fixed here:

- **`mdns.enabled` could not be adopted as a boolean.** The finding above: the
  new `adopt_wiring_keys` line wrote `enabled = "true"`, which
  `ConfigService::getBool` reads as `false`, so mDNS would have been silently
  disabled on every config that predated the key. Fixed with `toml_literal` +
  the literal write mode + the quoted-mirror repair, and proven by exercising
  the helpers on scratch configs: a real operator value survives, a
  `CHANGE_ME` string placeholder is untouched, a quoted mirror is repaired, and
  the ten existing `replace_toml_value` call sites keep writing quoted strings.
- **The certificate was minted before the config that names it existed.**
  `provision-host.sh` called `ensure_instance_certs` ahead of
  `ensure_deploy_configs`, so a fresh host's SAN fell back to `Argus`. Order
  swapped.
- **The camera's recognition keys were documented in the wrong table.**
  `argus-deploy/config.camera.toml.example` carried the eight keys under
  `[grpc]` while `operator_config::resolveIdentity()` reads
  `identity.identify`, `identity.auto_enroll`, `identity.capture_clear_faces`,
  `identity.min_face_box_px`, `identity.identify_interval_ms`,
  `identity.enroll_cooldown_ms`, `identity.best_shot_ms` and
  `identity.improve_margin`. Moved into `[identity]`, next to the target they
  ride, and `docs/operations/configuration-keys.md` now names them there.
  `relay_chunk_bytes` — which no translation unit in the tree reads — left both
  camera templates.
- **`mdns-service-test.cc` did not cover what the label composition decides.**
  The health projection's `instance` field (the composed
  `<name>-<path>.<type>` label, with and without a path) and the 63-octet clamp
  are now asserted, including that two routes of an over-long name stay
  distinct.
- **`packages/lib/mdns/AGENTS.md` claimed more for `isAdvertising()` than it
  delivers** ("only a caller that must know asks"), which invited exactly the
  dead `if (!mdnsService->initialize()) LOG_WARN` the eight call sites carried.
  The bullet now separates liveness from delivery, and the composed-label rule
  is documented beside it.
- **`argus-deploy/CONTEXT.md` said "mDNS advertised here only"** about the
  gateway, which this unit made false, and its port map omitted 7025 (sync, the
  one service that already published on all interfaces) and 7044 (the identity
  HTTP surface the gateway proxies to). Both fixed, plus a paragraph stating
  the announcement/publish mismatch above.
- **The root `AGENTS.md` counted the contracts wrong** ("nine of the ten")
  and predated `routes`. It now says ten of eleven and names `routes` among the
  `argus_contracts` packages.

Refuted or recorded as questions, not acted on:

- One reviewer read the announcement as happening before the listener bind and
  therefore as advertising a port that is not yet accepting. Both happen in the
  same process microseconds apart, mDNS resolution on a client takes orders of
  magnitude longer, and the responder keeps answering for the life of the
  process — recorded, not changed.
- The `wss://<discovered host>:<port>/sync` question — whether the app's
  WebSocket upgrade survives being dialled at a per-route discovered address —
  was marked unverified by the reviewer and stays unverified here: the app still
  dials the gateway's public port, so nothing in this unit can exercise it. It
  is Phase 5 step 6's check.
- `MdnsService::health()`'s `instances` projection is consumed by no service's
  `/health` today. Kept: it is the only way to see the composed label without a
  packet capture, and it is what the new test asserts.
- The sync → notification pull credential is still unprovisioned
  (`fill_deploy_pair` mints `grpc.caller_gateway` from the gateway's key and
  nothing for `argus-deploy/config.sync.toml`). Carried from step 1a's report;
  it is 1c's item, and 1c also owns the caller-label rename that goes with it.

### Found by the closing gate, not by a reviewer

The first full run failed `check-tidy`: five checks had risen, seven findings
net. Every one of them sat in code this unit wrote.

- `logical-routes.cc` used the iterator-pair `std::find` and `std::sort`; both
  are the ranges forms now (`std::ranges::find` against `routes.end()`,
  `std::ranges::sort`).
- `mdns-service.cc` built the per-instance records in an index loop over
  positionally initialized `mdns_string_t{ptr, size}`; the loop is a range-for,
  the four constructions name their fields, the service-type deduplication uses
  `std::ranges::find`, and the two new private helpers are `[[nodiscard]]`.
- `instanceLabel()` and `instanceNameFor()` took two and three adjacent
  `const std::string&` and tripped the swappable-parameter ratchet. Both take an
  input struct now — `InstanceLabelInput`, `InstanceNameInput` — which is the
  shape this file already used for `NameEqualsInput` and rule 2's shape for a
  three-parameter call, with designated initializers at both call sites and the
  one in `MdnsService::health()`.

The remaining findings in those files are not this unit's: each is a
pre-existing site whose line number moved, checked by running the gate's own
check set (`cppcoreguidelines-owning-memory`, `modernize-*`, `performance-*`,
`bugprone-*`) over every translation unit this unit touched and finding no hit
on an added line. `scripts/lib/tidy-baseline.txt` was then re-recorded on this
verified tree, as rule 19 asks: `tus` 538 → 543, the same 45 checks and 2940 →
2932 findings, after a per-check diff of the two files that found nothing risen
and exactly four counts lower — `bugprone-easily-swappable-parameters` 52 → 51
and `modernize-use-nodiscard` 724 → 723 (the input structs and the two
`[[nodiscard]]` helpers), `modernize-use-designated-initializers` 241 → 238
(the four record constructions that now name their fields), and
`modernize-use-scoped-lock` 279 → 276, which had been that far below its floor
since the previous unit deleted the gateway's notifier and three
`std::lock_guard`s with it. The other two checks that had risen,
`modernize-use-ranges` and `modernize-loop-convert`, sit exactly back at their
floors.

The new files were formatted with the tree's `.clang-format` before that. Two
notes for whoever formats next: the style's `FixNamespaceComments` (an LLVM
default the file does not turn off) appends `} // namespace` to every anonymous
namespace, which rule 20 forbids — the four it wrote were removed and
`check-comments` is what caught them — and the local clang-format disagrees
with a handful of pre-existing lines (`mdns-service.cc:1`, `:193`,
`mdns-service-test.cc:2`), so it is applied to new files wholesale and to
edited line ranges otherwise, never to a file at large.

## Verification

- `./scripts/check-comments.sh` — 1346 files, 0 comments.
- `./scripts/check-deps.sh` — 80 declarations, 678 edges, 0 forbidden, 0
  cycles, 0 unresolved, 23 edges deferred to phase 3. The new
  `lib/http -> argus::contracts::routes` and `lib/http -> argus::lib::mdns`
  edges are accepted; no tier-1 package gained a contract dependency.
- `./scripts/build-all.sh dev --only camera` — 51/51 ctest tests, including the
  two suites this unit added or changed (`logical-routes-test`,
  `mdns-service-test`) and `route-discovery-test` from the new contract.
- The full orchestrator (`./scripts/build-all.sh dev`) — **exit 0**: 18/18
  projects, 485 test executions, 0 failures, 0 first-party warnings and 0
  errors; `check-comments` 1346 files / 0 comments; `check-deps` 80
  declarations / 678 edges / 0 forbidden / 0 cycles / 0 unresolved / 23 edges
  deferred to phase 3; `check-tidy` 543 TUs, 2932 findings over 45 checks
  against the 2940 baseline, 4 checks below it, none risen — the five that had
  risen are back at or under their floors and the tree reads twelve findings
  lighter than the run that rejected it.

## Files

New: `packages/contracts/routes/{CMakeLists.txt,AGENTS.md,src/routes/service-discovery.hxx,tests/unit/route-discovery-test.cc}`;
`packages/lib/http/src/http/logical-routes.{hxx,cc}`,
`packages/lib/http/src/http/route-announcements.{hxx,cc}`,
`packages/lib/http/tests/unit/logical-routes-test.cc`.

Touched: `packages/lib/mdns/{AGENTS.md,src/mdns/mdns-service.{hxx,cc},tests/unit/mdns-service-test.cc}`,
`packages/lib/http/{AGENTS.md,CMakeLists.txt,src/http/listener-config.{hxx,cc}}`,
`packages/contracts/{AGENTS.md,CONTEXT.md}`, `AGENTS.md`,
`scripts/{lib/common.sh,provision-host.sh}`,
`argus-deploy/{docker-compose.yml,CONTEXT.md,config.{auth,camera,gateway,guard,identity,notification,productivity,sync}.toml.example}`,
`docs/operations/configuration-keys.md`,
`docs/architecture/wire-camera-media.md`,
`services/{auth,identity,sync,camera,productivity,notification,guard,gateway}` —
each service's `main.cc` (listener + announcement), its `config.toml.example`,
its `CONTEXT.md`, and where the resolver lived,
`services/auth/src/config/auth-config.cc`,
`services/identity/src/config/identity-config.cc`,
`services/sync/src/config/sync-config.cc`,
`services/camera/tests/unit/camera-config-test.cc`,
`services/gateway/tests/gateway-test.cc`,
`scripts/lib/tidy-baseline.txt` — the tidy floor re-recorded on the tree the
gate accepted (`tus` 538 → 543, 2940 → 2932 findings, nothing risen).
