# argus-gateway — CONTEXT

## Why the gateway exists

The Argus backend was split from a single monolith into small services
(Fase 1 of the `migracion-microservicios` plan). The gateway is the first new
service of that split: a real Drogon application inside the monorepo (sibling
of `src/`) that progressively took over the public HTTP surface until the
monolith's build set was retired (F6-4).

## What it owns

- **Health**: `GET /health`, no auth, standard `ApiResponse` envelope
  `{status: 200 (int), info: {service, uptimeSeconds}, errors: null}`
  (F1-2 ruling; never depends on NATS being reachable).
- **Identity domain (F1-3)**: the identity HTTP surface (`/auth/*`,
  `/pairing`, `/invitation/*`, `/user`, `/portrait-preview/*`) served from
  the `argus_identity` static library — the exact sources the legacy backend
  links, so routes, filter chains (`DeviceFilter → ValidJsonFilter →
  JwtFilter → RoleFilter`) and DTO validation are identical by construction.
  Controllers are `HttpController<T, false>` registered explicitly in
  `src/identity/identity-registrar.cc`; the backend does the same explicit
  registration in `Application::run()` (static-lib auto-creation is dropped
  by the linker).
- **identity.db**: the identity domain reads ONE database resolved from
  `[identity] db` (default `database/identity.db`), injected both as the
  Drogon default client and as the `database.file` runtime override so
  `VecDb`/face-db write face embeddings there too. At boot the gateway
  applies F1-3a's `identity-schema.sql` (`[identity] schema`) — the 7
  identity tables plus the audit/portrait substrate the identity write paths
  touch (`audit_log`, `user_audit_log`, `user_action_log`, `user_portrait`,
  `stored_file`, `portrait_preview_capability`), carried verbatim from the
  retired monolith schema — and aborts if it fails; it never touches
  `argus.db` and never runs the backend migrations.
- **Device identity modes (F5-2, Ruling CH)**: `DeviceFilter` gained a
  `[device] identity_mode` gate (`ip` default, byte-identical legacy
  behavior | `credential`), shared with the legacy through `argus_identity`.
  In credential mode the fingerprint drops the source IP entirely —
  `HMAC(UA | SHA-256(secret))` — where `secret` is the per-device credential
  presented via the `X-Argus-Device-Credential` header and validated against
  the `device_credential` table in identity.db (only the SHA-256 is stored;
  the plaintext is minted at register/login and at desktop-challenge approval
  and shown once). Unknown, missing or oversized credentials degrade to an
  empty device hash, which downstream fails jwt-filter's session device match
  with the standard 401 `Device mismatch` — never a distinct error. A mode
  flip invalidates every existing session once (controlled re-login). The
  wire contract lives in `argus-contracts/identity/README.md`. The F5-1 rate
  limiter key (`DeviceFilter::deviceKey`) deliberately stays IP-based even in
  credential mode: the limiter evaluates pre-routing before any database
  access (Ruling CJ) and a client-presented credential would be an
  attacker-controlled key.
- **NATS event bus client**: connects to the shared event bus (`[nats]` in
  config) so later phases can fan sync-change events without touching the
  legacy backend. Connecting is optional: with no `nats.url` configured the
  gateway logs and continues.
- **FaceService**: config-gated on `[face] enabled` (absent section or
  `false` boots without face models). Enabled with models present it loads
  like the backend; with models missing it degrades to a warn and facial
  login stays disabled. It never touches alarm/siren paths (none exist in
  the identity surface).
- F1-4 will move the `/sync` listener ownership here.

## `/sync` surface (F1-4)

- **`/sync` socket (F1-4)**: the gateway owns the `/sync` WebSocket end to end.
  It serves the sync protocol natively (`sync`, `sync_audit_log`,
  `sync_user_audit_log`, identity rooms, `initial_info`) from
  `argus_sync`: the productivity/notification sync tables read their
  named read-only clients (`[productivity] db`, `[notifications] db`, opened
  `file:...?mode=ro`, enabled process-wide by
  `DbService::enableUriFilenames()`), and since F6-5 the camera sync tables
  pull from argus-camera over the `argus.camera.v1.SyncService` gRPC leg
  (`[camera] grpc_target`) — Ruling G's identity-owned tables
  (`user`, `person`, `user_invitation`) and Ruling S's audit pages
  (`sync_audit_log`/`sync_user_audit_log` over the identity.db
  `audit_log`/`user_audit_log` tables) read the default client instead, so
  post-cutover rows replay to the app. Because it serves the bootstrap
  itself, a gateway whose config omits `[productivity] db` or
  `[notifications] db` resolves those sync reads to the default identity
  client (which has no such tables) and the whole bootstrap throws
  `sync_error` — a scratch/deploy config must always set both keys. With
  argus.db retired (F6-4) no read-only client is installed for the `event`
  domain (permanent orphan, Ruling DH) and `EventRepository` answers the
  empty shape — `DbService::readOnlyClient()` returns nullptr when nothing
  is installed instead of falling back to a database lacking the tables.
  It relays every `camera:*`/`voice:*` frame (text and
  binary) byte-transparently to the service `/sync`/gRPC legs — `camera:*`
  to argus-camera (`[camera] sync_url`), `voice:*` to argus-voice
  (`[voice] target`) — as the client itself — same
  `Authorization` header and `User-Agent`, so the device-hash filter
  still binds the session. The relay never forwards a client-supplied
  `X-Forwarded-For`: it synthesizes it from the observed TCP peer address of
  the client connection (the gateway is the only one that sees the client;
  the device hash is `HMAC(User-Agent|IP)`).
  While the relay session is still connecting, frames are buffered up to a
  256-frame cap; binary frames past the cap are dropped (transient PCM,
  stale on replay), text overflow answers the standard 503 envelope.
  Relay frames coming back are filtered to the relayed protocol only
  (`camera:*` / `voice:*` types); module emits and `initial_info` of the relay
  session are dropped — the gateway emits those itself. An unconfigured leg
  answers its frames with the 503 unconfigured-relay envelope.
- **Sync-change fan-out**: subscribes the tail-only wildcard
  `argus.*.v1.change` and re-emits through the same `RoomManager` rooms
  (`moduleRoom`, `userRoom`, `replaceRoleRooms`, `disconnectUser`) exactly as
  the retired `SocketService` did, marshalled into the Drogon loop. It never
  publishes — no gateway component installs the event-bus publisher
  (`SocketService::setEventBus` stays a no-op slot here). Payload contract:
  `argus-contracts/subjects.md`.

## Build wiring (decisions)

- The gateway is added from the root project with
  `add_subdirectory(argus-gateway EXCLUDE_FROM_ALL)`, so
  `cmake --build --preset dev` still builds ONLY the backend. Build it with
  the root build preset `cmake --build --preset gateway` (targets
  `argus-gateway` + `gateway-test`), or with `--target`.
- `argus_common` was moved into `src/shared/CMakeLists.txt` (same target,
  same sources, same flags) so both the root project and the gateway consume
  one definition without duplicating the source list. `argus_identity`
  (F1-3, moved to `argus-identity/` in f7-2d) follows the same pattern in
  `argus-identity/CMakeLists.txt`.
- The gateway also builds standalone: it reuses the root module folders
  (`../src/{cert,mdns,room,sqlite,socket,audit,filter,sync}`),
  `../argus-identity`, `../third_party/sqlite-vec` and `../third_party/ncnn`
  via `add_subdirectory`, and its own `conanfile.txt` (Drogon 1.9.13, cnats
  3.13.0, tomlplusplus 3.3.0, doctest 2.4.12, jwt-cpp 0.7.2,
  nlohmann_json 3.11.3, mdns 1.4.3, opencv 4.13.0 — same versions as the
  root, same Drogon/sqlite3 options for Conan cache reuse; re-run
  `conan install` after pulling: the identity deps were added in F1-3).
  Its tests register into ctest only in the standalone tree, keeping the
  root ctest at its 7 backend suites.
- Canonical build shape: `cmake --build --preset gateway` (root tree) is the
  canonical way to produce deployable gateway binaries. The standalone tree
  compiles `argus_common` WITHOUT ncnn, so HardwareProfile-derived behavior
  (`CapabilityTier`, the vulkan fields, `deriveTier` in
  `src/shared/wrapper/hardware-profile/hardware-profile.cc`) differs between
  the two shapes; the first gateway consumer of HardwareProfile must know
  this and must not ship binaries from the standalone tree.
- Controller registration: gateway controllers MUST be declared as
  `HttpController<T, false>` and registered explicitly in `main.cc` with
  `app().registerController(std::make_shared<T>())` before `run()`. Drogon's
  automatic registration is dropped by the linker for controllers whose
  object code lives in a static library (`gateway-core`), which silently
  yields 404s; explicit registration avoids it.
- Run from a directory containing `config.toml` (copy
  `config.toml.example`); `config.toml` is gitignored.

## Cutover (F1-5): public TLS listener + reverse proxy

The gateway takes the public listener the app has always connected to and
forwards each extracted domain to its service backend through the proxy route
table. The app keeps working without any update.

- **Listener**: `ListenerConfig::resolve()` reads `[gateway] host` (default
  `0.0.0.0`), `port` (7024), `plain` (TLS unless `true` — local-test option),
  `min_protocol` (TLSv1.2 → `[[listeners]] ssl_conf [["MinProtocol", ...]]`)
  and `[cert] server_cert/server_key`; `main.cc` injects the built
  `[[listeners]]` array into `loadConfigJson()` (no static `addListener`).
- **Reverse proxy**: `gateway_proxy::SimpleReverseProxy` (official Drogon
  example vendored into `src/proxy/`, `setPassThrough(true)` both directions —
  headers and multipart bodies are never mutated) registered as the plugin
  `gateway_proxy::SimpleReverseProxy` with the built route table (one entry
  per configured `[camera]`/`[productivity]`/`[notifications]` `proxy_url`;
  the plugin registers only when at least one route exists).
  Two additive behaviors over the official pattern: path exclusions
  (`gatewayNativePaths()`, segment-boundary match) pass through to the normal
  routing chain, and the proxy synthesizes `X-Forwarded-For` from the observed
  TCP peer address (dropping any client-supplied value) so the backend device
  hash still binds the real client — same rule the `/sync` relay applies.
  **Fallback**: a request no route claims also passes through to the normal
  routing chain, which answers unknown paths with the frozen NOT_FOUND
  envelope (`setCustomErrorHandler` → `AppConfig::get404Response()`) —
  byte-identical to what the retired monolith served.
  **Link lesson**: the plugin self-registers through a `DrObject<T>` template
  static that nothing references by name, so the `gateway-core` static lib
  must be linked `WHOLE_ARCHIVE` into the executable or Drogon logs
  "Plugin ... undefined!" and every proxied request 404s.
- **Proxy exclusion table (Ruling I — who serves what)**: gateway-native and
  therefore NEVER proxied: `/auth/*` (identity), `/pairing`, `/invitation/*`,
  `/user`, `/portrait-preview/*`, `/sync` (native WS + relay), `/health`.
  The extracted domains go through the route table:
  `/camera/*` + `/zone/*` (up to 8 segments) → argus-camera,
  `/calendar-event*` + `/project*` (up to 8 segments) → argus-productivity,
  `/notification*` (up to 2 segments) → argus-notification. Only what no
  route claims falls through to the gateway's own routing chain, which answers
  unknown paths with the NOT_FOUND envelope. Coverage is enforced at boot
  (`requireExclusionCoverage`): every registered gateway route must be inside
  the exclusion set or startup aborts. No path is
  served by both sides (verified live: `/health` → gateway envelope through
  the gateway, 404 envelope direct; `/no-such-route` → the same NOT_FOUND
  envelope the monolith served).
- **TLS trust chain (Ruling K)**: the gateway points at the SAME `certs/`
  directory as the legacy (transitional shared path) and runs
  `CertService::init()` + `MdnsService` with the same `[cert]`/`[mdns]` keys
  as the legacy; verified live with the shared CA (`openssl s_client`
  verify OK, `issuer=CN=Argus Instance CA`). In the cutover runtime mDNS is
  advertised by the gateway only (legacy `mdns.enabled=false`).
- **Cutover config requirements (historical, config-only)**: internal plain
  listener (loopback bind only — the monolith trusted X-Forwarded-For for the
  device hash, so a routable internal bind is spoofable), `[identity] db`
  (Ruling H read-only identity client: the monolith's `UserRepository::findById`
  and `RefreshTokenRepository::findByAccessToken` resolved to identity.db when
  the key was configured — this covered the JWT filter's per-request reads AND
  the proxied project-member/calendar-event-share target-user checks, which is
  why gateway-only users were not rejected against stale argus.db),
  `[device] trust_forwarded_for = true` so the proxied `/sync` relay and
  reverse-proxy X-Forwarded-For were trusted (the gateway is a 127.0.0.1 peer
  and always trusted). Moot since the monolith build set was retired (F6-4).
- **Acceptance evidence**: two-process run (gateway TLS 7024 + legacy
  internal 7025) — proxied matrix byte-identical to direct-legacy captures
  (statuses and bodies; only CORS header order differs), 404-vs-502
  `CAMERA_UNREACHABLE` distinction preserved through the proxy, gateway-minted
  JWT accepted by the legacy `JwtFilter` (Ruling H), golden-sync e2e PASS
  pointed at the gateway (bootstrap, audit pages, `camera:subscribe` →
  `camera:ready` + binary frame through the relay), voice relay
  `voice:start` → PCM → `voice:assistant`/`voice:done` live. Full matrix in
  `.superpowers/sdd/migracion-microservicios/task-f1-5-report.md`.

## Camera cutover (F2-2): routing table, composite relay, camera funnel

- **Proxy route table**: `SimpleReverseProxy` reads `[camera] proxy_url` and
  builds route targets (`prefixes`, `maxSegments`, `backend`); resolution
  (`segmentPrefixMatch`/`segmentCount`/`matchRoute`) is public for the test
  suite. Since F6-2 `/camera` and `/zone` go to argus-camera at every
  segment depth (cap 8): CRUD plus
  `/camera/{id}/ptz|preset|settings|status|presets|capabilities|talk`. Only
  deeper paths than the cap and foreign prefixes fall through to the
  gateway-native routing chain.
- **Composite `/sync` relay**: `SyncRelay` holds one upstream per protocol
  family — the seven `camera:*` frame types relay to argus-camera
  (`[camera] sync_url`), and since F6-3 `voice:*` frames relay to argus-voice
  over argus.voice.v1 (`[voice] target`, see the F6-3 section); an empty key
  makes that leg answer the 503 unconfigured-relay envelope. Same client
  credentials and XFF rule on both legs.
- **Camera change funnel (`camera_fan_out`)**: the NATS subscription is the
  wildcard `argus.*.v1.change`; the concrete subject routes the payload —
  `argus.camera.v1.change` goes to `camera_fan_out::handleCameraChange`,
  anything else to the sync fan-out parser. An audit-kind payload
  (`CameraAuditEvent`, Ruling Y) is inserted into identity.db `audit_log`
  via `AuditLogService::create` first, then the DB-assigned row is fanned out
  as a `Log` event; a plain change payload fans out directly. The funnel
  handler runs on the Drogon IO loop (RoomManager is thread-local).
- **Named camera client (Ruling Z, superseded F6-5)**: `[camera] db` opened
  mode=ro as the named camera client is gone; the camera/camera_stream/zone
  sync reads pull from argus-camera over the `argus.camera.v1.SyncService`
  gRPC leg (`[camera] grpc_target`), identity tables stay on the default
  client.

## Camera object_detected consumer (F2-3): budget, silent hours, digest

- **`camera_notifier`** subscribes `argus.camera.v1.object_detected` (F2-3)
  next to the camera change fan-out. Events marshal from the cnats
  dispatcher into the Drogon IO loop before touching policy or database —
  same discipline as the change funnel. The subscription is an ephemeral
  core-NATS consumer: no replay after a gateway restart, so events published
  while it is down are lost (the JetStream stream retains them for
  inspection only).
- **This subject is NOT a sync change**: payloads never reach `/sync`; the
  consumer turns them into `notification` rows via the existing
  NotificationService (type `camera`) for active owner/guard users only.
- **NotificationService compiles into argus_sync** (moved from the legacy
  target list): its repository/schema live there and it links argus_identity
  for SocketService — the gateway gets it through the argus_sync
  dependency it already had.
- **Ruling AD budget**: 6 notifications per camera per rolling hour
  (`[notifications] budget_per_hour`), silent local-hour window
  (`silent_start`/`silent_end`, both -1 off, wrapping supported). Suppressed
  events count per class; a cumulative digest flushes every minute once the
  window or the silent window closes. Event payloads are data, never
  commands — no notification path can arm or trigger any audible device.
- **Emission (F3-2 fix round)**: the gateway binds the legacy
  `SocketUserChangeSink` into the notification slot
  (`sync/user-change-sink.cc`, installed at boot next to the room-manager
  lifecycle), so camera-notifier deliveries dispatch the `/sync` Add frame
  into the user rooms the sync socket joined — the same pre-cutover
  `SocketService` push. `SocketService::publishChange` stays a no-op here by
  design: the gateway CONSUMES `kSyncChange` and must never publish it (it
  would loop through its own sync fan-out). The productivity sink slot is
  deliberately unset — the gateway performs no productivity writes (every
  route is proxied), so binding it would be dead code.

## Productivity + notification cutover (F3-2): routing, funnels, named clients

- **Proxy route table (Ruling AP)**: `[productivity] proxy_url` routes
  `/calendar-event`, `/calendar-event-share`, `/project`, `/project-member`,
  `/project-task` (every method, subpaths up to 8 segments) to
  argus-productivity; `[notifications] proxy_url` routes `/notification`,
  `/notification-token` (2 segments) to argus-notification. Paths are
  relayed identical (no rewrite); exclusion coverage is enforced at boot
  like every route target. Everything not routed falls through to the
  gateway-native routing chain.
- **User change funnel (Ruling AQ/Y)**: the `argus.*.v1.change` wildcard now
  also routes `argus.productivity.v1.change` /
  `argus.notification.v1.change` payloads to
  `user_change_fan_out::handleUserChange`, which inserts each user-scoped
  audit row VERBATIM into identity.db `user_audit_log` via
  `DbService::client()` (identity client) BEFORE fanning the row out as a
  `Log` sync event. The F3 services never persist audit rows locally — the
  gateway is the only writer. Daily compaction of `user_audit_log` stays
  gateway-side.
- **Named clients (Ruling AQ)**: `[productivity] db` opens
  `mode=ro` (`DbService::setProductivityClient`) and `[notifications] db`
  opens READ-WRITE with WAL + `busy_timeout`
  (`setNotificationClient` — Ruling AR requires rw: the camera notifier
  writes notification rows through it). The 7 productivity sync tables and
  the notification/notification_token sync reads resolve to them; an absent
  db key keeps the F2-2 precedent — the reads fall back to the default
  client and the boot logs a warn. Personal-table scoping
  (`isPersonalTable` + `ctx.sub`) and role checks stay gateway-side;
  `/sync` pull pages for the moved tables are byte-identical with the
  monolith's (golden-sync evidence).
- **WAL discipline (Ruling AR)**: the gateway opens the productivity
  database read-only and the notification database read-write, both with
  `busy_timeout`, and never runs DDL against either — schema/DDL belong to
  the F3 services and the migrate tools.
- **camera-notifier retarget (Ruling AR)**: the camera notifier keeps
  running gateway-side but writes its rows through
  `NotificationService` → `DbService::notificationClient()`
  (notification.db), not the retired argus.db copy.
- **Ruling AS (legacy stays up, goes quiet)**: historical — the retired
  monolith kept its whole notification/productivity code with the routes
  unreachable through the gateway; the build set died in F6-4.

## Remote listener + LAN-only bootstrap + refresh-token limiter (F5-1)

- **Remote classification (Ruling CG)**: `[remote] tunnel_port` (default 0 =
  disabled = the single-listener shape, zero behavior change) adds a SECOND
  listener mirroring the public one's TLS posture (same host, certs, min
  protocol; appended by `appendRemoteListener` in
  `argus-gateway/src/server/listener-config.cc`). A request is
  remote-classified when its connection's LOCAL port equals `tunnel_port`
  (`requestIsRemote`, remote-config.cc — TLS is end-to-end through the
  tunnel relay, so the observed local port is the only honest signal; the
  relay cannot inject XFF and the peer address is the tunnel client, not
  the device). The mechanism is one pre-routing advice in
  `argus-gateway/src/main.cc` (`RemoteGate::check`), the only pre-filter
  hook the gateway has, which also covers the `/sync` WebSocket upgrade
  path (Drogon runs pre-routing advices for WS requests too): remote is
  marked with the `remote_ctx` request attribute on EVERY request, and the
  whole gateway surface works unchanged remotely except the two bootstrap
  routes. `network.lan_cidrs` from the blueprint is deliberately NOT
  introduced: behind the byte-transparent relay every remote peer is the
  tunnel, so CIDR matching is meaningless — the deviation from the
  blueprint is this listener marking.
- **LAN-only gate (Ruling CG)**: `/pairing` and `/auth/register` answer
  `403 REMOTE_NOT_ALLOWED` (frozen `{status, info, errors}` envelope, new
  `AppConfig::ERROR_CODE_REMOTE_NOT_ALLOWED` +
  `getRemoteNotAllowedResponse()`) when the request is remote-classified and
  `[remote] enabled = false` (default). `enabled = true` lets them pass
  byte-identical to LAN behavior. Short-circuit responses bypass the
  post-handling advice, so the gate applies CORS itself to keep the
  response headers identical to every other gateway response.
- **Refresh-token rate limiting (Ruling CJ)**: the same gate rate-limits
  `PATCH /auth/refresh-token` — the scope Ruling CJ bound the limiter to,
  not the whole remote pre-auth surface (`/pairing` is bootstrap and 409s
  after pairing and `/auth/register` is LAN-only gated above, but
  `POST /auth/login`, `POST /auth/device-login` + its challenge poll and
  `POST /invitation/resolve` stay remote-reachable pre-auth routes that
  mint sessions with no limiter — still ledgered (F5-2 kept its scope to
  the device credential identity and left the limiter surface untouched).
  `RefreshRateLimiter`
  (`argus-gateway/src/server/refresh-rate-limiter.cc`) keeps a
  sliding-window counter (at most `max_requests` admissions per
  `window_seconds`) and locks a key out for `lockout_seconds` after
  `lockout_threshold` consecutive 4xx handler outcomes; the outcome is fed
  back by a second post-handling advice (`RemoteGate::recordOutcome`).
  429 uses the frozen envelope (`AppConfig::get429Response`, code
  `TOO_MANY_REQUESTS`) and is emitted in pre-routing — BEFORE routing,
  filters and any database access. `[rate_limit] enabled = false` (default)
  never rejects.
- **Key and honest limits**: the limiter key is the device fingerprint hash
  (`DeviceFilter::deviceKey` = HMAC(UA|IP) with the fingerprint secret) —
  the same key DeviceFilter stores for the request — falling back to the
  peer IP if the fingerprint secret is unconfigured. The gateway must keep
  `device.trust_forwarded_for` off: with it enabled the IP half of the key
  comes from the client-supplied `X-Forwarded-For` header, so a remote
  attacker controls the key (unlimited fresh keys, lockout defeat). Behind
  the tunnel all remote clients share the relay's local hop, so per-key
  collapses to per (User-Agent, relay-observed peer); UA rotation mints
  fresh keys. This is defense-in-depth for the bootstrap route, not an
  internet-grade WAF — the device credential identity (F5-2, Ruling CH)
  landed but the limiter key deliberately stays fingerprint-based: a
  client-presented credential would be an attacker-controlled key and the
  limiter evaluates pre-DB. All state is in-memory per
  gateway process: a restart clears every counter and lockout, and the
  tracked-key set is bounded (4096) so rotated fingerprints cannot grow it
  forever; once 4096 live keys are tracked, a new-key insert first evicts
  expired entries and, while the map stays full, rejects every further new
  key — attacker-rotated or a legitimate first-seen device — with 429
  until tracked entries expire (self-healing within `window_seconds`).

## Leaf SAN for the public relay hostname (F5-3, Ruling CI)

- **`[remote] hostname` appends a DNS SAN**: `CertService::instanceSans()`
  (`src/shared/services/cert/cert-service.cc`) reads
  `ConfigService::getString("remote.hostname")` after the always-present
  base list (`argus.local`, `localhost`, `127.0.0.1`, `::1`, `[mdns] name`
  when set, host hostname) and pushes a non-empty value as the LAST SAN
  entry; the leaf keeps its pre-existing SAN-only extension shape. The key
  is empty by default and ABSENT means zero change: the SAN list (and with
  it the certificate content shape) is byte-identical to a build without
  the key — rotation is only ever re-signed with a fresh key/serial, so
  the honest statement is the SAN list is unchanged, verified in
  `cert-san-test` against the exact base list.
- **Rotation picks the SAN up and hot reloads**: every leaf regeneration
  (startup near-expiry rotation or the periodic rotation loop) rebuilds
  the SAN string from the live config, and `rotateServerCertificate()`
  calls `drogon::app().reloadSSLFiles()` when the gateway is running —
  the listener serves the regenerated leaf (new fingerprint + the
  `remote.hostname` SAN) without a restart. Proven in-process in
  `src/test/unit/cert-san-test.cc`: a live TLS client sees the served
  fingerprint change and the hostname SAN appear after the rotation call
  returns.
- **App coordination (blueprint "la app configura servidor manual además
  de mDNS")**: the app configures `remote.hostname` as its manual server
  for remote access (in addition to mDNS discovery on the LAN) and
  validates it against the instance CA, so the value must be in the leaf's
  SANs or the TLS handshake fails. The templates ship `hostname = ""`;
  setting it is the instance owner's deployment choice once the tunnel
  relay hostname is known (Fase 5 argus-tunnel).
- **Known gap, documented not fixed**: the leaf carries no EKU/keyUsage
  extensions (pre-existing SAN-only leaf shape, consistent with Ruling CH
  keeping client-cert issuance out of scope for the app).

## Push-intent publisher (F5-5, Ruling CK)

- **`[push] enabled` gate (default off)**: when the key is absent or false the
  gateway installs no `NatsPushIntentSink`, `push_intent::getSink()` returns
  null and `NotificationService::createAndEmitMany` skips the per-row publish
  — structurally zero behavior change.
- **Why the gateway carries the publisher**: the brief's "argus-notification
  publishes" is topologically superseded — since the F3-2 cutover the only
  producer of notification rows is the gateway's `camera-notifier` writing
  through `NotificationService` into notification.db, so the row (and with it
  the intent publish, which fires strictly after the row is persisted) lives
  in the gateway process. The adjudication is forced by the cutover, not a
  convenience.
- **argus-notification remains the policy owner**: it owns the notification
  HTTP surface and the delivery policy; the gateway only hosts the publish
  seam. Intents are best-effort at-most-once (fire-and-forget NATS publish),
  display-only, and never carry alarm/siren semantics — see
  `argus-contracts/subjects.md`.

## Voice cutover (F6-3): argus.voice.v1 leg, typed identity, UpdateUser RPC

- **`[voice] target` leg**: with `voice.target` set, `voice:*` frames no longer
  relay to the old WS leg — `VoiceGrpcRelay` speaks `argus.voice.v1`
  VoiceService bidi to argus-voice (default 127.0.0.1:7034): text frames map
  to VoiceStart/VoiceStop/VoiceSkip, PCM binary frames to the `pcm` oneof
  field of `ClientFrame`. An empty `target` answers voice frames with the
  503 unconfigured-relay envelope.
  The frozen mobile app `/sync` contract is untouched: the gateway still
  renders every app frame as JSON — `voice:stt`, `voice:assistant`,
  `voice:event`, `voice:done`, plus TTS binary chunks — so byte-identity is a
  gateway concern, not argus-voice's.
- **Typed identity on VoiceStart**: the gateway resolves the session's user in
  identity.db and sends `identity{user_id, name, lang, role}` inside the first
  proto message, so argus-voice never reads a database — it greets from the
  typed fields and answers in the user's language.
- **`argus.identity.v1.IdentityService` listener**: the gateway hosts
  UpdateUser on `[identity] rpc_host`/`rpc_port` (default loopback 7040;
  compose binds 0.0.0.0 because argus-voice is on the bridge network). When a
  user says their name mid-session, argus-voice writes the spoken name back
  through this RPC; the gateway persists it via `UserRepository` and
  re-fans-out `argus.sync.v1.change` on NATS (the same user-change fan-out
  every other identity write already uses) so every connected device sees
  the renamed user. Role rides the `x-argus-role` metadata. UpdateUser
  enforces row scoping: the `x-argus-user` metadata must carry the request's
  `user_id`, otherwise the RPC answers UNAUTHENTICATED.
- **The service implementation moved out in f7-3**: `IdentityRpcService` now
  lives in `argus-identity/src/feature/rpc/identity-rpc.cc` — the surface
  belongs to the identity service; the gateway only constructs it and binds
  the listener, so it travels with the folder at the standalone extraction.
  The same listener gained `ValidateToken` and `CheckDeviceCredential`,
  which the whole fleet's filter chain calls instead of reading identity.db:
  the gateway is the only process that still touches those rows.
