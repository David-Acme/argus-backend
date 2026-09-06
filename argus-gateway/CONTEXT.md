# argus-gateway — CONTEXT

## Why the gateway exists

The Argus backend is being split from a single monolith into small services
(Fase 1 of the `migracion-microservicios` plan). The gateway is the first new
service of that split: a real Drogon application inside the monorepo (sibling
of `src/`) that progressively takes over the public HTTP surface while the
legacy backend keeps running untouched on its own listener.

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
  `stored_file`, `portrait_preview_capability`), all copied verbatim from
  `database/schema.sql` — and aborts if it fails; it never touches `argus.db`
  and never runs the backend migrations.
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

## What proxies to legacy

- **`/sync` relay (F1-4)**: the gateway owns the `/sync` WebSocket end to end.
  It serves the sync protocol natively (`sync`, `sync_audit_log`,
  `sync_user_audit_log`, identity rooms, `initial_info`) from
  `argus_sync`: the non-identity sync tables read the legacy `argus.db`
  through a read-only SQLite connection (`file:...?mode=ro`, enabled
  process-wide by `DbService::enableUriFilenames()`; `[legacy] db`, default
  `database/argus.db`) — Ruling G's identity-owned tables (`user`, `person`,
  `user_invitation`) and Ruling S's audit pages (`sync_audit_log`/
  `sync_user_audit_log` over the identity.db `audit_log`/`user_audit_log`
  tables) read the default client instead, so post-cutover rows replay to the
  app. It relays every `camera:*`/`voice:*` frame (text and
  binary) byte-transparently to the legacy's internal `/sync`
  (`[legacy] sync_url`, empty disables the relay) as the client itself — same
  `Authorization` header and `User-Agent`, so the legacy device-hash filter
  still binds the session. The relay never forwards a client-supplied
  `X-Forwarded-For`: it synthesizes it from the observed TCP peer address of
  the client connection (the gateway is the only one that sees the client;
  the device hash is `HMAC(User-Agent|IP)`), and the legacy needs
  `[device] trust_forwarded_for = true` with the gateway as trusted proxy.
  While the legacy session is still connecting, frames are buffered up to a
  256-frame cap; binary frames past the cap are dropped (transient PCM,
  stale on replay), text overflow answers the standard 503 envelope.
  Legacy frames coming back are filtered to the relayed protocol only
  (`camera:*` / `voice:*` types); module emits and `initial_info` of the relay
  session are dropped — the gateway emits those itself.
- **Sync-change fan-out**: subscribes the tail-only wildcard
  `argus.*.v1.change` and re-emits through the same `RoomManager` rooms
  (`moduleRoom`, `userRoom`, `replaceRoleRooms`, `disconnectUser`) exactly as
  the legacy `SocketService` would, marshalled into the Drogon loop. It never
  publishes — only the legacy installs the event bus
  (`SocketService::setEventBus`). Payload contract: `argus-contracts/subjects.md`.

## Build wiring (decisions)

- The gateway is added from the root project with
  `add_subdirectory(argus-gateway EXCLUDE_FROM_ALL)`, so
  `cmake --build --preset dev` still builds ONLY the backend. Build it with
  the root build preset `cmake --build --preset gateway` (targets
  `argus-gateway` + `gateway-test`), or with `--target`.
- `argus_common` was moved into `src/shared/CMakeLists.txt` (same target,
  same sources, same flags) so both the root project and the gateway consume
  one definition without duplicating the source list. `argus_identity`
  (F1-3) follows the same pattern in `src/identity/CMakeLists.txt`.
- The gateway also builds standalone: it reuses `../src/shared`,
  `../src/identity`, `../third_party/sqlite-vec` and `../third_party/ncnn`
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

## Cutover (F1-5): public TLS listener + reverse proxy to the legacy

The gateway takes the public listener the app has always connected to and
proxies everything else to the legacy backend on its internal plain listener
(config-gated port, e.g. 7025). The app keeps working without any update.

- **Listener**: `ListenerConfig::resolve()` reads `[gateway] host` (default
  `0.0.0.0`), `port` (7024), `plain` (TLS unless `true` — local-test option),
  `min_protocol` (TLSv1.2 → `[[listeners]] ssl_conf [["MinProtocol", ...]]`)
  and `[cert] server_cert/server_key`; `main.cc` injects the built
  `[[listeners]]` array into `loadConfigJson()` (no static `addListener`).
- **Reverse proxy**: `gateway_proxy::SimpleReverseProxy` (official Drogon
  example vendored into `src/proxy/`, `setPassThrough(true)` both directions —
  headers and multipart bodies are never mutated) registered as the plugin
  `gateway_proxy::SimpleReverseProxy` with `backends: [legacy.proxy_url]`.
  Two additive behaviors over the official pattern: path exclusions
  (`gatewayNativePaths()`, segment-boundary match) pass through to the normal
  routing chain, and the proxy synthesizes `X-Forwarded-For` from the observed
  TCP peer address (dropping any client-supplied value) so the legacy device
  hash still binds the real client — same rule the `/sync` relay applies.
  **Link lesson**: the plugin self-registers through a `DrObject<T>` template
  static that nothing references by name, so the `gateway-core` static lib
  must be linked `WHOLE_ARCHIVE` into the executable or Drogon logs
  "Plugin ... undefined!" and every proxied request 404s.
- **Proxy exclusion table (Ruling I — who serves what)**: gateway-native and
  therefore NEVER proxied: `/auth/*` (identity), `/pairing`, `/invitation/*`,
  `/user`, `/portrait-preview/*`, `/sync` (native WS + relay), `/health`.
  Everything else (legacy-owned: `/camera/*`, `/zone/*`, `/notification/*`,
  `/calendar-event*`, `/project*`, and any unknown path) is forwarded verbatim to the legacy. Coverage is enforced
  at boot (`requireExclusionCoverage`): every registered gateway route must
  be inside the exclusion set or startup aborts. No path is served by both
  sides (verified live: `/health` → gateway envelope through the gateway,
  legacy 404 envelope direct; `/no-such-route` → legacy envelope through the
  proxy).
- **TLS trust chain (Ruling K)**: the gateway points at the SAME `certs/`
  directory as the legacy (transitional shared path) and runs
  `CertService::init()` + `MdnsService` with the same `[cert]`/`[mdns]` keys
  as the legacy; verified live with the shared CA (`openssl s_client`
  verify OK, `issuer=CN=Argus Instance CA`). In the cutover runtime mDNS is
  advertised by the gateway only (legacy `mdns.enabled=false`).
- **Legacy config requirements (documented, config-only)**: internal plain
  listener (loopback bind only — the legacy trusts X-Forwarded-For for the
  device hash, so a routable internal bind is spoofable), `[identity] db`
  (Ruling H read-only identity client: legacy `UserRepository::findById` and
  `RefreshTokenRepository::findByAccessToken` resolve to identity.db when the
  key is configured — this covers the JWT filter's per-request reads AND the
  proxied project-member/calendar-event-share target-user checks, which is
  why gateway-only users are not rejected against stale argus.db),
  `[device] trust_forwarded_for = true` so the proxied `/sync` relay and
  reverse-proxy X-Forwarded-For are trusted (the gateway is a 127.0.0.1 peer
  and always trusted).
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
  suite. `/camera` and `/zone` with at most 2 segments go to argus-camera;
  `/camera/{id}/ptz|preset|settings|status|presets|capabilities|talk` and
  everything else fall through to the legacy. Default backends unchanged.
- **Composite `/sync` relay**: `SyncRelay` holds one upstream per protocol
  family — `voice:*` frames relay to the legacy (`[legacy] sync_url`), the
  seven `camera:*` frame types relay to argus-camera (`[camera] sync_url`,
  empty keys keep the old single-legacy behavior). Same client credentials
  and XFF rule on both legs.
- **Camera change funnel (`camera_fan_out`)**: the NATS subscription is the
  wildcard `argus.*.v1.change`; the concrete subject routes the payload —
  `argus.camera.v1.change` goes to `camera_fan_out::handleCameraChange`,
  anything else to the sync fan-out parser. An audit-kind payload
  (`CameraAuditEvent`, Ruling Y) is inserted into identity.db `audit_log`
  via `AuditLogService::create` first, then the DB-assigned row is fanned out
  as a `Log` event; a plain change payload fans out directly. The funnel
  handler runs on the Drogon IO loop (RoomManager is thread-local).
- **Named camera client (Ruling Z)**: `[camera] db` opens mode=ro as the
  named camera client (`DbService::setCameraClient`); camera/camera_stream/
  zone sync reads resolve to it, legacy non-camera tables keep the read-only
  argus.db client, identity tables stay on the default client.

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
- **Emission caveat**: the gateway never installs the `SocketService` event
  bus, so pre-cutover the camera notifier's `createAndEmitMany` push was
  already a no-op here (empty local user rooms, no bus to publish on). The
  F3-2 sink seam turns that into an explicit warn instead of a silent no-op;
  app-visible behavior is unchanged. Live camera-notification delivery to
  the app happens through the next `/sync` pull from notification.db.

## Productivity + notification cutover (F3-2): routing, funnels, named clients

- **Proxy route table (Ruling AP)**: `[productivity] proxy_url` routes
  `/calendar-event`, `/calendar-event-share`, `/project`, `/project-member`,
  `/project-task` (every method, subpaths up to 8 segments) to
  argus-productivity; `[notifications] proxy_url` routes `/notification`,
  `/notification-token` (2 segments) to argus-notification. Paths are
  relayed identical (no rewrite); exclusion coverage is enforced at boot
  like every route target. Everything not routed to a F3 service still falls
  through to the legacy default backend.
- **User change funnel (Ruling AQ/Y)**: the `argus.*.v1.change` wildcard now
  also routes `argus.productivity.v1.change` /
  `argus.notification.v1.change` payloads to
  `user_change_fan_out::handleUserChange`, which inserts each user-scoped
  audit row VERBATIM into identity.db `user_audit_log` via
  `DbService::client()` (identity client) BEFORE fanning the row out as a
  `Log` sync event. The F3 services never persist audit rows locally — the
  gateway is the only writer. Daily compaction of `user_audit_log` stays
  gateway-side.
- **Named clients (Ruling AQ)**: `[productivity] db` and
  `[notifications] db` open read-only named clients
  (`DbService::setProductivityClient` / `setNotificationClient`); the 7
  productivity sync tables and the notification/notification_token sync
  reads resolve to them. Personal-table scoping (`isPersonalTable` +
  `ctx.sub`) and role checks stay gateway-side; `/sync` pull pages for the
  moved tables are byte-identical with the legacy (golden-sync evidence).
- **WAL discipline (Ruling AR)**: the gateway opens the F3 databases
  read-only with `busy_timeout` and never runs DDL against them —
  schema/DDL belong to the F3 services and the migrate tools.
- **camera-notifier retarget (Ruling AR)**: the camera notifier keeps
  running gateway-side but writes its rows through
  `NotificationService` → `DbService::notificationClient()`
  (notification.db), not the legacy argus.db copy.
- **Legacy stays up, goes quiet (Ruling AS)**: the legacy keeps its whole
  notification/productivity code; the routes are simply unreachable through
  the gateway because the route table never sends them there.
