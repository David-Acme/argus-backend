# Phase 3d step 1c — the gateway is gone

Every app-facing service is now the public API for its own routes: the app
resolves a route over mDNS and dials the service that owns it, on that
service's own TLS listener and its own instance certificate. `services/gateway`
leaves the tree (213 of its 217 files deleted, the four the gate kept renamed
into `lib/auth`), `packages/contracts/gateway` with it, and the
two things the gateway was the only home for — the single public door and the
LAN gate in front of the enrollment paths — are re-expressed per service:
`RemoteGate` refuses `/pairing` and `/auth/register` on the second, remote
listener of argus-auth and argus-identity, and nothing else proxies anything.

This is the last part of Phase 3d step 1. Step 1a moved the camera-object
notification policy off the gateway's own database, step 1b gave every
app-facing service a TLS listener and a per-route mDNS announcement, and this
unit deletes what was left once nothing needed it.

## What existed before

- **One public port, one proxy.** `services/gateway` held the tree's only
  certificate-bearing listener (7024) and reverse-proxied `/auth`,
  `/identity`, `/camera`, `/zone`, `/productivity`, `/notification`,
  `/invitation`, `/pairing`, `/portrait-preview`, `/user` and the rest to the
  services that had come out of the monolith, over plain HTTP on host
  loopback. It also byte-relayed `/camera-stream` and the `/sync` upgrade.
- **One remote door.** The tunnel's byte-transparent stream terminated at the
  gateway, whose `[remote]` handling was the instance's only gate against a
  remote client reaching the pairing or enrollment path.
- **`contracts/gateway`** carried `GatewayErrors`, the one refusals catalog
  that existed only for the proxy's own failures.
- **`argus-deploy/config.gateway.toml.example`**, 85 keys, and the `gateway`
  compose service on `network_mode: host` — the only host-networked service in
  the stack, which is what let it see real client addresses.
- **`services/gateway/tools/probe-captures/`** — 195 tracked `.txt`
  transcripts of the proxy's HTTP answers (63 KB) and the
  `probe-identity-matrix.sh` driver that wrote them, the frozen evidence of the
  proxied contract's byte shape.

## The deletion

222 paths leave their old home, all staged in one change — 218 deletions and
four renames:

| Owner | Files | |
|---|---|---|
| `services/gateway/` | 217 | 213 deleted, 4 renamed |
| `packages/contracts/gateway/` | 4 | deleted |
| `argus-deploy/` | 1 | deleted |

The gateway's 217 are `tools/` (196: the 195 recorded probe transcripts under
`probe-captures/` and the `probe-identity-matrix.sh` driver that wrote them),
`src/{main.cc,proxy,server,sync}` (13 files), `tests/gateway-test.cc`,
`CMakeLists.txt`, `Dockerfile`, `Dockerfile.dockerignore`, `.gitignore`,
`config.toml.example`, `AGENTS.md` and `CONTEXT.md`.

The four renames are the LAN gate itself, and git scores them where they
belong: `src/server/remote-config.{cc,hxx}` →
`packages/lib/auth/src/auth/` at R094/R100 and `remote-gate.{cc,hxx}` at
R074/R071. The gate keeps its body and changes owner — its `[remote]` reading
is per service now instead of the one public door's, which is the 26–29% the
`remote-gate` pair gave up.

Follow-through in the build and provisioning surface:

- `scripts/build-all.sh` loses its `services/gateway` entry — the list is 17
  projects.
- `scripts/setup.sh` loses its `services/gateway` config loop and points
  `ensure_instance_certs` at `services/identity/config.toml` (the `mdns.name`
  SAN source moved with the service that reads it).
- `scripts/lib/comment_scan.py` drops the `/tools/probe-captures/` exclusion:
  the folder it existed for is gone, and an exclusion that outlives its
  subject is a hole in rule 20's gate.
- The `gateway` compose service is deleted, and `print_summary`'s
  `docker compose logs -f gateway` becomes `docker compose logs -f`.
- `scripts/seed-golden.py`'s usage example names `config.auth.toml` (it mints
  its recorder session from the fleet JWT secrets, which auth carries).

## What replaced each thing the gateway did

**TLS, routes and discovery** came in 1b; this unit makes them reachable.
Seven app-facing publishes flip from `127.0.0.1:<port>:<port>` to
`<port>:<port>` (auth 7042, identity 7044, sync 7025, camera 7026,
productivity 7027, notification 7028, guard 7039), and their internal RPC
publishes stay loopback-only. A container announcing its own address announces
its bridge interface, which no LAN client can dial, so `mdns.address` is a new
key: `MdnsService` resolves it as an IPv4 or IPv6 literal before it falls back
to enumerating interfaces, warns and falls back when the value is not an IP,
and `scripts/provision-host.sh` fills it for every deploy config that carries
it — `--mdns-address`, else `ARGUS_MDNS_ADDRESS`, else `ip route get 1.1.1.1`'s
`src`, else `hostname -I`'s first address, else a warning that the containers
will stay undiscoverable.

**The LAN gate** is now `packages/lib/auth`'s `remote-config.{hxx,cc}` and
`remote-gate.{hxx,cc}`, the first feature the tier-4 library gained since the
split:

- `RemoteConfig{tunnelPort, enabled}` resolves `[remote]` per service, and
  `requestIsRemote` classifies by **local port** — a request that arrived on
  the listener whose port is `[remote] tunnel_port` is remote, whatever its
  peer address says. `remote_ctx`, the attribute and the device-filter branch
  that fed it, is gone from the tree.
- `appendRemoteListener` adds that listener beside the service's own, mirroring
  its TLS posture and certificate, and `requireDistinctTunnelPort` refuses a
  configuration where `tunnel_port` collides with the service's public port
  before any listener is built.
- `RemoteGate::check` is a pre-routing advice in argus-auth's and
  argus-identity's `main.cc`: a remote request for `/pairing` or
  `/auth/register` gets `403 REMOTE_NOT_ALLOWED` with CORS applied, unless
  `[remote] enabled` is set.
- The refusal is a catalog entry — `AuthErrors::RemoteNotAllowed` — which is
  where `contracts/gateway`'s vocabulary went instead of moving wholesale.

**The pairing port** identity answers with is its own listener port:
`IdentityConfig::resolveAnnouncedPort()` replaces the gateway-era `mdns.port`
read, so the port a pairing or invitation answer names is the port of the
instance the app paired through. `mdns.port` is read by no code and declared by
no config anywhere in the tree.

**The notification caller credential** is renamed with its caller:
argus-notification's `[grpc] caller_sync` replaces `caller_gateway`, and
`ensure_deploy_configs`'s third `fill_deploy_pair` repoints from
`config.gateway.toml`'s `[notifications] credential` to `config.sync.toml`'s
`[grpc] caller_sync` — the sync → notification pull was the only reader of that
pair, and step 1a had already recorded that the gateway's own delivery leg
presented a credential notification never accepted.

## Consequences recorded, not hidden

- **The tunnel's forwarding target names no live listener.** Its
  `server.gateway_host`/`server.gateway_port` pointed at the gateway's remote
  listener (`127.0.0.1:7024` in `service-config.cc`'s clamp and in
  `services/tunnel/config.toml.example`, `127.0.0.1:7034` in
  `argus-deploy/config.tunnel.toml.example`). Repointing the client at the
  service whose remote listener it wants is the tunnel's own unit of work
  (D19 defers it), and `argus-deploy/CONTEXT.md` says so next to the
  remote-listener section.
- **A remote client now reaches two doors, not one.** The app's other routes
  are reachable remotely only if the tunnel is pointed at a service that
  serves them; the remote gate is per service by construction, which is the
  price of deleting the single door. Phase 5 step 6 verifies the remote path
  against a real client.
- **Device addressing.** With the gateway gone, no service is host-networked,
  so `device.trust_forwarded_for` buys no real client address in a container:
  under `identity_mode = "ip"` the peer is Docker's bridge forwarding address
  and every device fingerprints alike. `identity_mode = "credential"` is the
  address-independent path, and `argus-deploy/CONTEXT.md` records the
  consequence where the network section explains the trusted-proxy rule.
- **The 195 probe captures are gone with their driver.** They recorded what
  the gateway's proxy answered (63 KB of tracked text, the 816 KB the f1-5
  audit measured being disk blocks); no build, source or document read them,
  and the f1-5 audit had already reported them as a record rather than an
  artifact whose deletion was the owner's call.
- **The frontend still dials 7024.** `frontend/src/shared/constants/net.constant.ts`
  carries `ARGUS_DEFAULT_PORT = 7024` and its clients take the first
  `_argus._tcp` instance they resolve; the coordinated switch is the
  frontend's, tracked by the plan (Phase 5 step 6), and the backend keeps the
  mDNS route vocabulary stable so that switch is a port change and not a
  protocol change.
- **A stale `argus-deploy/config.gateway.toml` on an upgraded host** is inert
  (the config files are gitignored and regenerated; the compose has no gateway
  service), and the deploy `CONTEXT.md` and the shadow-mode runbook both say a
  host that still carries the old gateway data directory may delete it.

## Documentation swept

Every gateway reference in first-party prose was rewritten, deleted or
reframed as history — 43 tracked Markdown files across `docs/`,
`argus-deploy/`, `services/` and `packages/` (the plan's own step rows among
them), by five parallel sweeps whose edits were each verified against the
tree. The mechanical ones (healthcheck rows, port maps, template
lists, link tables) were the easy half; the interesting ones were claims the
deletion made false:

- `services/auth/AGENTS.md`, `services/identity/AGENTS.md`,
  `docs/operations/deployment-docker.md`: "the compose publishes both ports on
  `127.0.0.1` only … reached by the gateway's loopback proxy" became the real
  shape (public port on the LAN, RPC loopback-only, `RemoteGate` in front).
- `packages/lib/mdns/AGENTS.md`: the legacy `_argus._tcp` paragraph deleted
  (nothing keeps that record any more), `mdns.port` replaced by
  `mdns.address`, and the key list corrected.
- `packages/lib/cert/AGENTS.md`: the cert package now has one owner
  (argus-identity), not two.
- `packages/clients/notification/AGENTS.md`,
  `packages/clients/{identity,sync}/AGENTS.md`,
  `packages/contracts/sync/AGENTS.md`: link-line tables, includer counts and
  caller lists recounted against the tree.
- `services/identity/CONTEXT.md`: the pairing-port section rewritten around
  `resolveAnnouncedPort()`, and the project count corrected to seventeen.
- `docs/architecture/*` and `docs/README.md`: the gateway row, the "one public
  port" claims, the `18 projects` counts and the diagram in
  `wire-camera-media.md` (the app dials argus-camera's `/media` directly).

Four falsities that predate this unit surfaced during the sweep's verification
and were corrected in the same change, each measured before editing:

- **The root `AGENTS.md` listed `qr-code-generator/1.8.0` as a dependency.**
  It is absent from `conanfile.txt` and no `qrcodegen` user survives anywhere
  in the tree — the last one died in `24955467` (f6-4-1) with the legacy build
  set, and the line outlived it by several phases.
- **`services/camera/CONTEXT.md` described a config-gated named identity DB
  client** (`[identity] db`, read-only) backing the sync socket's user reads.
  No such key is declared by any camera config and no camera translation unit
  opens `identity.db`; what exists is caller credentials
  (`[grpc] caller_guard`) and the user metadata the sync pull carries, with the
  operator's known-person matcher reaching argus-identity over
  `[identity] target`.
- **`services/auth/CONTEXT.md` carried an open item about the config
  generator** minting independent per-project secrets. `ensure_shared_configs`
  has shared one value for `jwt secret`, `jwt refresh_secret`,
  `device fingerprint_secret` and `identity rpc_secret` since `31b8a793`, so
  the item is closed; `auth.rpc_secret` is filled by the deploy path
  (`ensure_deploy_configs`) and left empty by the native one, which is the
  value that works on a loopback listener.
- **Counts and measurements in five package `AGENTS.md` files** (identity
  client link lines, notification client line counts, the sync contract's
  fan-in) were stale by 9, 4 and 9 units respectively; all three were measured
  again and rewritten.

## Review

Five adversarial reviewers swept the tree's prose in parallel, and their
findings are data rather than instructions: every edit they made was re-checked
against the file, and every claim they flagged was measured before it was
acted on or dismissed. What survived that check and changed this unit:

- the four falsities above (root dependency list, camera's named client, the
  auth open item, the stale counts);
- `services/identity/CONTEXT.md`'s project count and `mdns.address`-as-SAN
  wording;
- `docs/architecture/wire-nats-subjects.md`'s domain list, rewritten after a
  first edit of mine named the proto packages instead of the live domains
  (`nats-subject.hxx` is the authority);
- `services/sync/AGENTS.md` and two sibling sentences that still had a
  WebSocket "cannot ride the gateway's reverse proxy" clause in the present
  tense.

Two reviewer observations were verified and deliberately left: the
`_argus._tcp` fixture in `packages/lib/mdns/tests/unit/mdns-service-test.cc` is
arbitrary fixture data for the package's multi-instance responder, not a live
record; and `services/sync/tests/fixtures/sync/manifest.json`'s recorded
`baseUrl` (`https://127.0.0.1:7024`) is provenance from the 2026-09-08
recording — verify mode compares only `frames`, and record mode rewrites it.

One reviewer flag was verified and left as a contract question for the
frontend's phase: `packages/lib/auth/src/auth/role-access.hxx` still maps
`/camera-stream` to `TableName::CameraStream`, a path no service serves since
the gateway's byte relay died. It is part of the frozen permission map the
frontend holds a copy of, so removing it is a coordinated contract change, not
a cleanup.

## Verification

- `scripts/check-comments.sh` — 1328 files checked, 0 comments (the count
  moves with the deleted gateway sources and the dropped capture exclusion).
- `scripts/check-deps.sh` — 79 declarations, 670 edges, 0 forbidden, 0 cycles,
  0 unresolved, 23 deferred to phase 3. The deleted `contracts/gateway` is one
  declaration and eight edges fewer than 3d-1b's 80/678, and the new
  `lib/auth → argus::lib::http` edge is a tier-4 → tier-2 edge, which rule 25
  permits.
- `./scripts/build-all.sh dev` — the run that closed this unit is green and
  complete: **17/17 projects, 467 tests, 0 failures, exit 0**, the last word
  being `All selected projects built and tested (profile: dev)`, with **not one
  `warning:` line in the whole log**. Its first pass is what caught the
  baseline shortfall: the deleted translation units put the scan at 537 TUs
  against the 543 the old file recorded, which rule 19 makes a failure ("a
  check that cannot be run is not a check that passed"), so the baseline was
  re-recorded and the run repeated — the clean pass reports `check-tidy: 537
  TUs, 2902 findings over 45 checks, baseline 2902`, nothing risen.
- `scripts/check-tidy.sh --write-baseline` — re-recorded on the built tree:
  **537 TUs, 2902 findings over the same 45 checks** (`tool 22.1.8`). The
  per-check diff against the 3d-1b file has no check above the old baseline
  and nine below it, every movement downward: `tus` 543→537,
  `modernize-use-nodiscard` 723→721, `modernize-use-scoped-lock` 276→273,
  `modernize-use-designated-initializers` 238→223,
  `modernize-avoid-c-arrays` 81→80, `modernize-use-emplace` 46→45,
  `modernize-use-starts-ends-with` 44→40,
  `bugprone-throwing-static-initialization` 36→34,
  `performance-move-const-arg` 14→13, `bugprone-exception-escape` 5→4. Thirty
  findings and six translation units fewer: the nine units the deletion takes
  out (`services/gateway`'s eight `.cc` files, its own test and `main.cc` among
  them, plus `contracts/gateway`'s catalog test) against the three this unit
  adds (`lib/auth`'s `remote-config.cc`, `remote-gate.cc` and their test), and
  the findings those units carried.

## Files

- **Deleted**: 218 (213 under `services/gateway/`, 4 under
  `packages/contracts/gateway/`, `argus-deploy/config.gateway.toml.example`).
- **Renamed**: 4, out of `services/gateway/src/server/` into
  `packages/lib/auth/src/auth/`.
- **Added**: `packages/lib/auth/tests/unit/remote-gate-test.cc`,
  `services/identity/Dockerfile.dockerignore` (the one service that had none
  until now, matching its twelve siblings), and this report.
- **Changed**: `packages/lib/auth/{CMakeLists.txt,AGENTS.md}`,
  `packages/contracts/auth/src/auth/auth-errors.hxx`,
  `packages/lib/mdns/src/mdns/mdns-service.cc`,
  `packages/lib/mdns/tests/unit/mdns-service-test.cc`,
  `services/{auth,identity}/src/app/main.cc`,
  `services/auth/config.toml.example`, `services/identity/config.toml.example`,
  `services/identity/src/config/identity-config.{hxx,cc}`,
  `services/identity/src/feature/{invitation,pairing}/…`,
  `services/notification/{config.toml.example,src/feature/rpc/…,tests/…}`,
  the eleven other service templates and the twelve sibling
  `Dockerfile.dockerignore` files (each one's enumerated
  `argus-deploy/config.<service>.toml` block, which named the gateway's file
  too, collapses into `argus-deploy/config.*.toml`),
  `argus-deploy/docker-compose.yml`, `argus-deploy/config.*.toml.example`,
  `scripts/{build-all.sh,setup.sh,provision-host.sh,seed-golden.py}`,
  `scripts/lib/{common.sh,comment_scan.py}`, and the prose listed above.
