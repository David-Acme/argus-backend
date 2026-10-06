# argus-auth — context

## Origin

Extracted in f7-4 from the filters and the JWT service. A pure move: 13
files, zero content edits — the include prefixes of the day (`filter/…`,
`shared/services/jwt/…`) were preserved inside `src/`, so not one of the
`#include` lines in the fleet changed then. Phase 2 step 2 dropped those
prefixes: the package is flat (`src/auth/<file>`, the private
`identity-access.hxx` under `src/auth/details/`) and every consumer's include
was repointed (`<auth/jwt-filter.hxx>`).

The extraction had to wait for f7-3. Until the auth RPC landed, the
filters read identity's repositories directly, so the package would have
depended on `argus-identity` while `argus-identity` depends on this
package for its own routes — a genuine cycle. It is last in the module
sequence for that reason.

## Why it depends on two wires and on no service

JwtFilter verifies the token SIGNATURE locally (cheap, no I/O) and then asks
`argus.auth.v1.ValidateToken`, which is authoritative for everything stateful:
the user row and its status, the refresh-token session, its expiry and its
device binding. DeviceFilter calls `CheckDeviceCredential` on the same leg in
credential mode. The user directory is the second, read-only edge
(`argus.identity.v1.GetUser`, behind `filterIdentityClient()`). So the
package's heavy edges are `argus::clients::auth` and
`argus::clients::identity` — generated contracts, not domain libraries, and
neither of them a database.

The verdict moved off the identity listener in 3b: before that, both filters
rode the identity client and `services/auth` did not exist. It owns the
session tables and answers the verdict now.

Two consequences worth keeping in mind:

- The package carries no AI or database closure. `argus-tts` links it and
  stays ncnn-free; that is a standing gate (`nm -C | grep -c 'ncnn::'`).
- An unreachable `argus-auth` fails closed: no authenticated request is
  admitted, and the client resolves a target rather than silently falling
  back to a local database. The refusal is 503 `AuthUnavailable`, not 401: a
  401 tells the app its token is bad, and the app answers it by refreshing
  and then clearing the session and its local projection, so restarting the
  auth container used to sign every user out. Only a verdict that says
  invalid is a 401. A missing verdict covers every RPC failure, a wrong fleet
  secret included, because none of them is the user's fault.
- In `credential` identity mode a failed `CheckDeviceCredential` RPC is the
  same outage: `AuthClient::checkDeviceCredential` answers `nullopt` for a
  transport failure (and `false` only for a credential auth says is not
  active), and `DeviceFilter` refuses with 503 `AuthUnavailable` instead of
  continuing with an empty device hash that ended in a device-mismatch 401.
- With `trust_forwarded_for`, the client address is the right-most
  `X-Forwarded-For` hop that is not itself a trusted proxy - the address the
  nearest trusted proxy actually saw. The left-most entry is whatever the
  client chose to send, and in `ip` mode the device hash is built from it,
  so a stolen token could be replayed with the victim's address claimed.
- The header is read at all only when the immediate peer is in
  `device.trusted_proxy_ips`, a comma-separated list of exact addresses or
  CIDRs (`details/proxy-allowlist`, IPv4, IPv6 and IPv4-mapped peers), and
  the list is empty by default. Loopback used to be trusted implicitly, which
  let anything that reached a service through a local port (the tunnel client
  dials the remote listener on loopback) choose its own address. No
  first-party component writes the header, so the deploy templates leave the
  list empty and `trust_forwarded_for` false.
- `JwtContext.sessionId` is the session (refresh-token family) the verdict
  names; argus-sync tags each socket with it, and argus-auth uses it as the
  caller's current session. `role_access::kSessionAccess` opens
  `GET /auth/sessions`, `DELETE /auth/sessions` and
  `DELETE /auth/sessions/{id}` to every role, route by route like
  `kGuardAccess`: the service scopes every query to the caller's own
  sessions. Four more rows are `kOwnerOnly`: `GET /auth/users/sessions`,
  `GET` and `DELETE /auth/users/{id}/sessions` and
  `DELETE /auth/users/{id}/sessions/{id}`, the owner's view of every user's
  sessions. `routeMatches` compares a pattern segment by segment, one `{id}`
  per segment, so a placeholder never spans a `/` and an empty segment never
  matches.

## Route access is denied unless a table names it (2026-10 audit)

The role check used to read the path as it came and to fall back on a generic
`kAuthAccess` rule (any `/auth` path, decided by method alone, `GET` open to
every role). Drogon routes case-insensitively, so `GET /auth/Users/sessions`
reached the owner-only handler while `RoleFilter` saw an unknown `/auth` path
and let a guest through. Since the audit:

- `hasHttpAccess` lowercases the path and strips trailing slashes before any
  table is consulted (`normalizedPath`). A trailing slash is not a no-op for
  Drogon: `/x/` reaches the handler of `/x/{1}` with an empty id. So a path
  that had a trailing slash must be allowed both as `/x` and as its `{id}`
  child (`/x/0`); a role that holds only one of the two is refused.
- `kAuthAccess` is gone. Every `/auth` route behind `RoleFilter` is listed in
  `kSessionAccess`, and an `/auth` path that no row names is refused to every
  role but the Owner. The `/auth` routes without `RoleFilter` (status, me,
  logout, the QR approval and its details) never consult the table.
- Dispatch is by whole first segment (`firstSegment`): `auth`, `rtc`,
  `privacy`, `visitor`/`visitor-crop`, `sync` and `guard` each answer from
  their route table; then `kRouteOverrides` (rows that narrow a table, today
  `GET /notification/delivery-summary`, owner-only); then the camera actions;
  then `tableFromPath`, which also matches a whole first segment, so `/user`
  no longer answers for `/users-x`.
- `permissionForMethod` returns `std::nullopt` for every method it does not
  name, `PUT` included, and a table route with no permission is refused. A
  `PUT` reaches a non-owner only through an explicit row (`/privacy/me`,
  `/guard/safety/pin`).
- `role_access::hasAppAction(role, AppAction)` is the one answer for the
  actions the assistant can ask the app to perform (`ShowCamera`,
  `OpenScreen`, `SetGuardMode`). `SetGuardMode` is decided by the same
  `POST /guard/mode` row the HTTP route uses, so the tool descriptor and the
  route cannot drift; argus-llm and argus-voice are meant to ask it instead of
  carrying their own table/permission pair.

## The filters after the audit

- `JwtFilter` refuses (500 `DeviceContextMissing`) when no `DeviceContext` is
  on the request: a route that forgot `DeviceFilter` would otherwise lose the
  device binding silently. `scripts/check-routes.sh` enforces the same rule
  at build time (`JwtFilter` implies `DeviceFilter`) and keeps an explicit
  allowlist of the routes that run without `JwtFilter`.
- The token is read from `Authorization: Bearer` only. `?token=` is accepted
  on a WebSocket upgrade (`Upgrade: websocket`) and nowhere else, and the
  `authorization` cookie is no longer read: query strings end up in proxy and
  access logs, and no first-party client uses a cookie.
- `JwtFilter` holds a `JwtService{JwtRole::Verifier}`, which loads
  `jwt.secret` alone; the refresh secret stays in `argus-auth`, the only
  issuer. An issuer refuses to start when `jwt.secret` equals
  `jwt.refresh_secret`, every minted token carries `typ` (`access` or
  `refresh`), `verifyAccess` requires `typ = access`, and `verifyRefresh`
  refuses `typ = access` (a refresh token minted before the claim existed,
  with no `typ`, still verifies, so nobody is signed out by the upgrade; an
  access token without `typ` is refused and the app refreshes once).
- `device.fingerprint_secret` is mandatory (32 characters or more) and no
  longer falls back to `jwt.secret`. `DeviceFilter::requireFingerprintSecret()`
  is the boot check; argus-auth calls it before it listens, and a service that
  skips it refuses every request that needs a fingerprint instead.
- `device.identity_mode` is `credential` unless the key says `ip` exactly:
  a missing or unknown value is the stronger mode.
- The default LAN list no longer carries `172.16.0.0/12`: those are Docker's
  bridges, and a tunnelled request that reaches a service through a published
  port arrives from one. Installations behind the userland proxy name their
  bridge gateway in `device.lan_networks` themselves.
- `DeviceFilter::networkPrefix` (an address cut to a given prefix, IPv4 and
  IPv6, IPv4-mapped folded to IPv4) and `networkFingerprint` (HMAC of the
  origin class and the /24 or /64 network, `tunnel` for the tunnel listener)
  are what argus-auth binds an `ip`-mode refresh to and what its rate gate
  groups IPv6 clients by.
- `RemoteConfig` also reads `[remote] allow_qr_login` and
  `[remote] tunnel_profile`. `requireTunnelListener` refuses a configuration
  that expects the tunnel (`tunnel_profile`, `enabled` or `allow_qr_login`)
  while `tunnel_port` is 0, because then every tunnelled request is taken
  for a LAN one.

## The two targets

`filterAuthClient()` resolves `auth.target` (`auth.rpc_host` /
`auth.rpc_port` as the fallback, default `127.0.0.1:7043`) and
`filterIdentityClient()` resolves `identity.target` the same way
(`identity.rpc_host` / `identity.rpc_port`, default `127.0.0.1:7040`). Each
presents the service's own credential, `auth.credential` and
`identity.credential`, falling back to the legacy `auth.rpc_secret` /
`identity.rpc_secret` only while that credential is empty (2026-10-05 audit,
#25). `installLocalAuthClient` lets the session authority itself (argus-auth)
answer its own filters in process instead of over its own listener; while one
is installed `filterAuthClient()` returns it. Each client is cached per
resolved target, credential and secret (the `voice-engine-seam` precedent), so a
config change picks up a new client and tests can point the chain at a dead
port to prove fail-closed.

The call itself rides `BlockingTask`, which moves the blocking stub call
off the event loop — the `camera-sync-source` pattern. It costs a detached
thread per validation; the SDK channel base (step 5) is where that should
become an async stub over the CQ bridge.

## Device context: presence is not emptiness

`DeviceFilter` always inserts a `DeviceContext`; in credential mode the
hash is EMPTY when the credential is missing, oversized or unknown. The
session check must therefore key on the context being PRESENT, not on the
hash being non-empty — otherwise a credential-less request passes a
device-bound session. f7-3 shipped that bug briefly and the
device-credential suite caught it; the contract now carries presence
(proto field presence) separately from the value.

## The user directory

`IdentityUserDirectory` (rule 27) exposes the identity domain's read-only
user row over the same cached client: `findById` calls
`argus.identity.v1.GetUser` and returns the DB-free `DirectoryUser`
(`auth/user-directory.hxx` in this package). Consumers install
it where they own a seam — argus-sync builds one at boot
(`services/sync/src/app/main.cc:82`) and hands it to its socket and to the
voice relay, while argus-productivity holds it as a private member in its two
share-carrying feature services — so no service opens
another domain's database for a user row.

## What does NOT live here

Anything stateful. No repositories, no schemas, no `DbService`. The
identity domain (users, persons, invitations, portraits) is
`services/identity/`; the RPC surface that serves this package is
`services/identity/src/app/rpc/`. The sessions and the device credentials
behind a verdict are `services/auth/`'s.

## Module gating (2026-10, the modules plan)

`docs/history/plans/modules-and-welcome-plan.md` makes surveillance and
productivity selectable modules. The gate lives here because `RoleFilter` is
the one place every app-facing route already passes, so the filter chain of
rule 5 is unchanged and `scripts/check-routes.sh` needs no new row.

- **The map is data beside the route tables.** `role_access::kModuleRoutes`
  maps a whole first path segment to a module: `camera`, `zone`, `media`,
  `guard`, `visitor`, `visitor-settings` and `visitor-crop` to
  `surveillance`; `project`, `project-task`, `project-member`,
  `calendar-event` and `calendar-event-share` to `productivity`.
  `moduleOfPath` normalizes the path the way `hasHttpAccess` does (case,
  trailing slash), so `/CAMERA/` is gated like `/camera`. A segment the map
  does not name is core and is never gated, whatever the enabled set says.
- **`RoleFilter` asks the gate first.** A route of a disabled module is
  refused with 403 `MODULE_DISABLED` to every role, the Owner included,
  before the role check: the enabled set is not secret (`GET /modules`
  answers it to every role), and the app needs the reason to hide the screen
  instead of reporting a permission problem. The `/media` WebSocket runs
  without `RoleFilter`, so argus-camera's socket asks the same gate on
  connect.
- **`/modules` routes** are in `kModuleAccess`: `GET /modules` for every
  role, the six `POST /modules/{id}/…` actions Owner-only.

### The cache, and why it fails open

`moduleGate()` is one `ModuleGate` per process. A module it has no word on is
**enabled**. The state reaches it in three ways, in this order of authority:

1. **The boot read** (`settingsBootRead`): `argus.settings.v1.Modules/
   ModuleStates` on `[modules] target` with `[modules] credential`, retried
   every 5 s up to 12 times off the event loop, so a service that boots before
   argus-settings still learns the set within a minute.
2. **The durable feed**: each service binds its own durable
   (`argus-<service>-modules`, deliver-all at creation, ordered) on
   `argus.settings.v1.module`. A message whose top level carries
   `modules: [{id, enabled, lifecycle?}]` is applied, a module counting as
   enabled only when `enabled` is true and its `lifecycle`, when present, is
   `active` (`disabled`, `not_installed` and `uninstalled_data_kept` all
   gate); one with `settled: false`, or with
   any other shape (the per-job progress frames), is acked and ignored; a
   malformed enabled set is terminated. Messages and the boot read carry a
   `version`; anything older than the last applied version is ignored, so the
   boot read and a replayed backlog cannot undo a newer state whatever order
   they arrive in.
3. **The last known state**: every applied set is written atomically
   (`.part` then rename) to `[modules] state_file`, default
   `database/module-state.json` in the service's working directory, and read
   back synchronously by `install` before the listener opens.

The decision is **fail open**, for three reasons. The gate is a product
switch, not a security boundary: the role table still decides who may do
what, and a disabled module's data is kept, not hidden. A service that cannot
reach argus-settings at boot must not lock the household out (the plan's own
requirement), and core routes are never in the map at all. And an upgraded
installation has no module state anywhere: argus-settings seeds its enabled
set by adoption and publishes `settled: false` until it has (every component
owner answered), which this cache treats as "no word", so surveillance and
productivity stay enabled on an upgraded install until the Owner changes
them. When a state is known, the last one wins over the default: a service
restarted while argus-settings is down keeps a disabled module disabled from
its file.

No repository, no schema and no `DbService` call came with it: the state file
is a cache this package can rebuild from the feed at any time, which is what
the package's no-database rule protects.

### What a service does with it

`module_gate::install({.service, .bus})` in `main.cc` is the whole wiring: it
restores the file, registers the start in a beginning advice and the drain
with `shutdown_signal`. A service that owns background work reads
`moduleGate().enabled(role_access::kSurveillanceModule)` (argus-camera's
operator and health monitor, argus-guard's evaluation) and may register
`moduleGate().onChange` (argus-camera closes its open views when surveillance
is disabled). argus-camera, argus-guard, argus-identity (`/visitor*`) and
argus-productivity install it; a service whose routes are all core needs
nothing, its `RoleFilter` simply finds no module.

## Roles per module, capabilities and the unknown role (2026-10, the context plan)

`docs/history/plans/context-roles-tools-quality-plan.md` (sections 2 and 3).
What a user can use now is one answer computed in one place, and every
decision below reads the same tables.

### The role vocabulary grows; the database stores text

`UserRole` stays an enum, but identity stores `user.role` and
`user_invitation.role` as plain text with no CHECK (a module that brings a role
would otherwise force a table rebuild per module; the boot rebuild that drops
the old CHECKs is `services/identity/CONTEXT.md`, "Role storage"). The enum is
therefore the only gate:

- `parseUserRole(name)` is the input validator: it answers nothing for a name
  outside the enum (`"unknown"` included), and every DTO that takes a role
  uses it.
- `userRoleFromString(name)` is the reader of a stored or received name and
  answers `UserRole::Unknown` for anything else. `Unknown` is not a role: it
  has no `roleBit` (`roleBit(Unknown) == 0`), no row in `kTableAccess`, no
  capability and no app action; `ModuleSnapshot::roleActive(Unknown)` is false.
  The places that used to default to Guest (`userRoleFromString`, the llm chat
  DTO, the llm controller, the llm rpc server, the guard directory, every
  `UserRole role{...}` member) now default to `Unknown`, and the voice wire's
  `VoiceIdentity.role` is `optional` so that a missing role is not the Owner
  (`VOICE_ROLE_OWNER = 0`); `VOICE_ROLE_UNKNOWN = 4` is what an `Unknown` role
  travels as.

### Which module brings a role

`modules.json` names the roles a module brings (`"roles": ["guard"]` on
`surveillance`; a role belongs to at most one module and `owner` to none). The
list travels on `ModuleStates`, on every enabled-set message of the feed and in
the last known state file, so every service's gate knows it. `ModuleFlag`
(`module-snapshot.hxx`) carries `{id, enabled, lifecycle, dataPurgedAt, roles,
name, summary, intro}` and `ModuleSnapshot` answers `enabled(module)` (a module
with no word is enabled, `core` always), `moduleOfRole(role)`, `roleActive(role)`
and `activeModules()`. A role whose module is off is **inactive**; every
other role, the Owner always, is active. `moduleGate().snapshot()` is the copy
a decision reads, `moduleGate().roleActive(role)` the shortcut, and
`moduleGate().onStateChange(fn)` fires once per `apply` that changed any field
of any module (argus-sync turns it into `ContextUpdate`).

### Inactive role: the baseline

An inactive role keeps the user and costs the household nothing, but the server
grants core only, and "core only" means the **baseline**, a small fixed set
every role holds in core: its own profile, sessions, privacy, notifications and
push tokens, calls, the heartbeat, the module list and the request for one,
the panic button and its own reminders. In the tables it is
`kBaselineBit` on a route row (`kEveryRoleBaseline`, `kNonOwnerBaseline`,
`kResidentGuardGuestBaseline`) and `kBaselineTables` (`user`, `audit_log`,
`user_audit_log`, `notification`, `notification_token`, `reminder`,
`reminder_detail`). `roleGranted(mask, role, roleActive)` and
`hasAccess({.., .roleActive})` apply it; `readsUserDirectory(role, roleActive)`
is false for an inactive Guard (its own row only) and `moduleTables(role,
modules)` leaves the directory and every non-baseline table out.

`RoleFilter` decides with `routeVerdict`, in this order: the route's module is
off (403 `MODULE_DISABLED`, for every role); the role is inactive and the route
is beyond the baseline (403 `ROLE_INACTIVE`, new `AuthErrors::RoleInactive`, so
the app can show the calm screen instead of a permission problem); the role
table says no (403 `FORBIDDEN`). `POST /guard/panic` is core:
`moduleOfRoute(path, method)` knows `kCoreRoutes` and never gates it, and it
carries the baseline bit, so panic works with surveillance off and for an
inactive guard. `GET /guard/safety`, the safety state of the caller, is core the
same way: a panic or duress alert already raised is never cancelled by a module
change, so what reads and answers it stays reachable with surveillance off and
for an inactive Guard (supervisor decision of 2026-10-06). The answer itself
rides the notification routes (`GET /notification/responses`, `GET` and `PATCH
/notification/responses/{id}`, `PATCH /notification/ack` and `read`), which are
the `notification` table, a baseline table, and the call it rings
(`POST /rtc/token`, `calls.join`). Whether the caller was a recipient of the
alert is the notification service's own check (404 for anyone else:
`call-engine-test`). The capability that opens the path is `safety.respond`, in
the baseline, independent of `guard.read` and of every module. What stays
surveillance is enabling, configuring and entering duress (`PATCH
/guard/safety`, `PUT` and `DELETE /guard/safety/pin`), the modes and every
other `/guard` route.

`role_access::hasAppAction({.role, .action, .modules})` is the module-aware twin
of `hasAppAction(role, action)`: the app actions the assistant may trigger
(`ShowCamera`, `OpenScreen`, `SetGuardMode`) answered from `camera.view`,
`notifications.read` and `guard.mode.set`, so a module that is off or an
inactive role refuses them.

Who may be put into an inactive role is identity's call, not this package's:
an invitation into one is refused 409 `ROLE_INACTIVE`, a role change into one
is allowed (`services/identity/CONTEXT.md`, "Role storage"). Any service that
asks "is this role active under the current modules" uses
`moduleGate().roleActive(role)`, or `moduleGate().current()->roleActive(role)`
when it also needs `moduleOfRole`.

### Capabilities

`capability.hxx` holds `kCapabilities` (`{id, module, roles}`) and
`capabilitiesFor({.role, .modules})`: the capability ids the role holds whose
module is active, in table order; an inactive role keeps only the entries
whose mask carries the baseline bit, `Unknown` gets none.
`hasCapability({.role, .modules, .capability})` answers one. The app reads the
list in its context (`useCapabilities()`), argus-llm filters its tools by it;
the server stays the authority (routes, sync pulls, the tools' own checks).
`tests/unit/capability-test.cc` keeps the list from drifting from the routes:
for every role and every combination of the two modules it asserts that each
capability's probe route or table answers exactly what the capability says.

| Capability | Module | Roles besides the Owner (who holds all but `modules.request`) |
|---|---|---|
| `profile.read` `sessions.manage` `privacy.own` `notifications.read` `notifications.register` `calls.join` `heartbeat.read` `modules.read` `safety.panic` `safety.read` `safety.respond` `reminders.read` `reminders.write` | core | every role; **baseline** |
| `modules.request` | core | Resident, Guard, Guest; baseline |
| `assistant.voice` | core | Resident, Guard, Guest |
| `directory.read` | core | Guard |
| `people.read` | core | Resident, Guard |
| `people.write` `memory.manage` | core | Resident |
| `users.manage` `invitations.manage` `privacy.household` `settings.manage` `modules.manage` `activity.read` | core | Owner only |
| `camera.view` | surveillance | Resident, Guard, Guest |
| `camera.talk` `zones.read` `events.read` `guard.read` | surveillance | Resident, Guard |
| `camera.manage` `zones.write` `guard.mode.set` `guard.guests.write` `safety.duress` | surveillance | Resident |
| `response.duty` `visitors.read` | surveillance | Guard |
| `guard.admin` `visitors.manage` `presence.read` | surveillance | Owner only |
| `agenda.read` `agenda.write` `projects.read` `projects.write` | productivity | Resident |

`safety.duress` covers the duress switch and the safety PINs; the panic button
is the core `safety.panic`, the caller's safety state the core `safety.read` and
answering a raised alert the core `safety.respond`. `reminders.read` and `reminders.write` are held by
every role, the Owner included, for rows whose target user is the caller only
(owner decision of 2026-10-06): the tables give `reminder` and
`reminder_detail` to Guard and Guest as to Resident, the service query keeps
the rows to `JwtContext.sub` and answers 404 for another user's, and nothing
in the sync projections widens it for the Owner: they are in no module room
and no global audit list (`isOwnRowTable`), their changes reach the target's
own room alone, and `kModuleRoutes` never names `reminder` so the routes are
never module-gated and the tables keep syncing with productivity off.

### Tables of a module

`kTableModules` maps the tables of a gated module (`camera`, `camera_stream`,
`zone`, `event` to surveillance; `calendar_event`, `calendar_event_share`,
`project`, `project_member`, `project_task` to productivity) and
`tableReadable(role, table, modules)` is what a sync pull asks: the role
reads it, the table's module is active and the role is not inactive. A pull of
an inactive module's table answers `null`, like a table the role may not read;
the rows stay in their owners' databases.
