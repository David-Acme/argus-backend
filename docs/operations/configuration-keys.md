# Configuration keys

What each key of the configuration templates is for, and the optional keys a
template does not set. The templates carry no comments (rule 20); this page is
where their notes live. Per-project templates are `<project>/config.toml.example`,
the deploy stack's are `argus-deploy/config.<service>.toml.example`.

## `argus-deploy/config.camera.toml.example`

argus-deploy argus-camera configuration. Copy to config.camera.toml (gitignored) next to this file and fill the instance secrets (the same jwt/device values as every other service); the compose file bind-mounts it as argus-camera's config.toml.

| Key | Notes |
|---|---|
| `camera.host` / `camera.port` / `camera.plain` / `camera.min_protocol` | The app-facing listener (`0.0.0.0:7026`), TLS unless `plain = true`; `[cert] server_cert` / `server_key` are the instance certificate it serves, and the compose healthcheck asks it over `https`. |
| `mdns.enabled` / `mdns.name` | The LAN announcement: one `_argus-route._tcp` instance per logical route this service registers (`path=camera`, `path=media`, `path=zone`), each carrying its own SRV port and an `https="true"` TXT key. |
| `identity.target` | argus-identity's fleet-secret RPC listener (`argus-identity:7040`); the camera guard's face recognition and enrollment ride it. |
| `identity.rpc_secret` | Must match argus-identity's [identity] rpc_secret, or every identity call fails. |
| `grpc.caller_guard` | Capability credential for the guard -> camera edge (paired with guard's camera.actions_credential). |
| `identity.identify` | Camera guard recognition; enroll/capture stay off until you enable them. `auto_enroll`, `capture_clear_faces`, `min_face_box_px`, `identify_interval_ms`, `enroll_cooldown_ms`, `best_shot_ms` and `improve_margin` ride the same table. |
| `operator.zones_from_db` | Enabled zones live in camera.db (the app editor); false uses the JSON below only. |
| `actions.enabled` | Audible/device actions from argus-guard; off until you enable them. |
| `health.enabled` | Occlusion/blur/moved detection from periodic image stats. |
| `stt.remote_url` | Camera listen (guard agent) transcription through argus-stt. |
| `storage.mode` | Private object storage (RustFS) for detection evidence snapshots. provision-host.sh fills the endpoint and credentials. |

Optional keys the template does not set:

| Key | Example | Notes |
|---|---|---|
| `objects.classes` | `"person,bicycle,car,..." (COCO-80 default when empty)` |  |
| `operator.ignored_classes` | `"bird,cat"` |  |
| `operator.zones` | `'[{"cameraId":1,"kind":"alert","name":"door","points":[[0.0,0.0],[1.0,0.0],[1.0,1.0]]}]'` |  |

## `argus-deploy/config.auth.toml.example`

argus-deploy argus-auth configuration. Copy to config.auth.toml (gitignored) next to this file and fill the instance secrets (the same jwt/device values as every other service); the compose file bind-mounts it as argus-auth's config.toml and its `database/schema.sql` over the mounted data directory.

| Key | Notes |
|---|---|
| `auth.db` | The three session tables, moved here from identity.db as a copy in Phase 3b-2 with the `/auth` surface that writes them. |
| `auth.rpc_secret` | Must be the same value in every service's config: the RPC answers session verdicts for the fleet. Empty means loopback-only and ungated, and the service refuses to start when the listener is reachable beyond loopback without it. |
| `auth.context_cache_seconds` | How long a resolved user context may be reused before identity is asked again (0 disables the cache). A change on the identity feed drops the entry immediately, so this is a load valve, not the invalidation mechanism. |
| `identity.target` | The identity call behind a session verdict; `argus-identity:7040`, the fleet-secret RPC leg. |
| `identity.rpc_secret` | Must match argus-identity's [identity] rpc_secret, or every validation of a live session fails. |
| `mdns.enabled` / `mdns.name` | The LAN announcement: one `_argus-route._tcp` instance per logical route this service registers, each carrying its own SRV port and an `https="true"` TXT key. |

## `argus-deploy/config.identity.toml.example`

argus-deploy argus-identity configuration. Copy to config.identity.toml (gitignored) next to this file and fill the instance secrets (the same jwt/device values as every other service); the compose file bind-mounts it as argus-identity's config.toml, **read-write** because the pairing state persists there, and its `database/schema.sql` over the mounted data directory.

| Key | Notes |
|---|---|
| `identity.port` | The TLS HTTP surface (7044). The compose publishes it on every interface, so the app dials the `/user`, `/invitation`, `/portrait-preview` and `/pairing` prefixes directly. It is also the port the pairing and invitation answers publish, and the one the app writes into every later invitation QR. |
| `identity.rpc_secret` | Gates the cleartext `argus.identity.v1` gRPC listener on `[server] grpc_port` (7040). Same value in every service's [identity] rpc_secret, or every call fails. Empty means loopback-only and ungated, and the service refuses to start when the listener is reachable beyond loopback without it. |
| `identity.db` | `database/identity.db`, this owner's only database (rule 27). |
| `face.enabled` | Face detection + recognition in this process; the engine never leaves it and argus-camera only ships crops. |
| `storage.mode` | Private object storage (RustFS) for the portraits. provision-host.sh fills the endpoint and credentials. |
| `pairing.paired` | The QR pairing state the frontend's onboarding reads; persisted at runtime, which is why this config bind is not read-only. |
| `auth.target` | argus-auth's fleet-secret RPC listener: the session-verdict leg the shared `JwtFilter` asks before it trusts a token. |
| `sync.control_target` | The sync service's control plane; a role update rides `replaceRoleRooms` before the `AuthContextChanged` emit. |
| `jwt.secret` / `jwt.refresh_secret` | The same instance secrets as every other service. The shared `JwtService` constructor reads both and refuses to start on a missing, short or default-value one, and this service's routes verify the access token with `jwt.secret`; `argus-auth` remains the only minter and rotator. |
| `mdns.enabled` / `mdns.name` | The LAN announcement: one `_argus-route._tcp` instance per logical route, so the app discovers this service's port directly. |
| `mdns.address` | The LAN address the announcements carry: empty enumerates the host's usable interfaces, and a value that is not an IP address falls back to the interfaces with a warning. provision-host.sh detects the host address (`--mdns-address`, `ARGUS_MDNS_ADDRESS`, `ip route get 1.1.1.1`, `hostname -I`) and writes it into every deploy config that carries the key. |

Optional keys the template does not set:

| Key | Example | Notes |
|---|---|---|
| `auth.rpc_host` / `auth.rpc_port` | `"127.0.0.1"` / `7043` | The split alternative to `auth.target`; `auth.target` wins when it is non-empty. |
| `jwt.access_ttl_minutes` / `jwt.refresh_ttl_days` | `60` / `7` | Read by the shared `JwtService` constructor; this service never mints or rotates a token, so the defaults are inert here. |
| `memory.create_face_vec` | `true` | Whether the face vec0 index is built from the schema's JSON column (unset behaves as true). |
| `remote.hostname` | `""` | The extra DNS SAN `lib/cert` appends when it rotates the instance leaf; this service's own config carries the key, and a leaf rotated without it loses that SAN. |

## `argus-deploy/config.guard.toml.example`

argus-guard deploy configuration. Copy to config.guard.toml (gitignored).

- **`[guard.quiet_hours]`** — Quiet-hours demotion markers. Journal-only and default off: held rows still notify exactly as before, the journal only records what a future enforce mode would have held back.
- **`[guard.belief]`** — Deterministic belief engine: closed signal vocabulary with bounded integer weights summed into a score, compared against the asymmetric severity thresholds (critical 1 < high 3 < medium 5 < low 7). gate_scope selects which effect kinds enforce mode may suppress: "notify" (default), "communication" (notify + announce) or "all". Alarm and siren-arm additionally require no hard floor, so a hard floor always lets physical effects through regardless of scope.
- **`[guard.belief.camera."1"]`** (optional, not in the template) — Per-camera overrides: any leaf above may be redefined per camera id.

| Key | Notes |
|---|---|
| `guard.host` / `guard.port` / `guard.plain` / `guard.min_protocol` | The app-facing listener (`0.0.0.0:7039`), TLS unless `plain = true`; `[cert] server_cert` / `server_key` are the instance certificate it serves. This service has no gRPC listener, so there is no `[server]` block. |
| `mdns.enabled` / `mdns.name` | The LAN announcement: one `_argus-route._tcp` instance per logical route this service registers, each carrying its own SRV port and an `https="true"` TXT key. |
| `guard.decision_mode` | Belief-gate mode: "shadow" journals the verdict without enforcing it, "enforce" lets the belief gate suppress effects. Unknown values fall back to shadow. decision_mode governs the belief gate only; per-encounter notification threading applies in both modes. |
| `guard.tamper_sustained_s` | Sustained-tamper escalation window in seconds; a tamper-ish camera health state (moved, covered, blurred) held this long raises its own high-danger notification through the durable intent path. |
| `storage.mode` | Private object storage (RustFS) for incident evidence records. provision-host.sh fills the endpoint and credentials. |

Optional keys the template does not set:

| Key | Example | Notes |
|---|---|---|
| `guard.belief.camera."1".threshold_medium` | `6` |  |

## `argus-deploy/config.llm.toml.example`

argus-deploy argus-llm configuration. Copy to config.llm.toml (gitignored); the compose file bind-mounts it as the service's config.toml. Since f8-b4 this is the brain: the [llm] engine block drives both the wire and the tool loop, and the blocks below carry the hosted memory stack.

- **`[memory]`** — The compose file binds models/llm, models/memory and models/extract read-only. The hosted memory stack; the memory data dir mounts at /opt/argus/memory.
- **`[identity]`** — Read-only snapshot sources for the catalog replica boot fill.

| Key | Notes |
|---|---|
| `llm.gpu_layers` | The compose service ships without /dev/dri, so the engine runs CPU. |
| `memory.create_face_vec` | The face recognition index belongs to the face service. |
| `extract.gpu_layers` | CPU only in the container (no /dev/dri on this service). |
| `nats.url` | Empty skips the hosted catalog replica (the snapshot fill still runs). |

## `argus-deploy/config.notification.toml.example`

argus-deploy argus-notification configuration. Copy to config.notification.toml (gitignored) next to this file and fill the instance secrets (the same jwt/device values as every other service); the compose file bind-mounts it as argus-notification's config.toml.

| Key | Notes |
|---|---|
| `notification.host` / `notification.port` / `notification.plain` / `notification.min_protocol` | The app-facing listener (`0.0.0.0:7028`), TLS unless `plain = true`. It is the singular table: `[notifications]` below is the camera object policy and has no listener keys. `[cert] server_cert` / `server_key` are the certificate served. |
| `mdns.enabled` / `mdns.name` | The LAN announcement: one `_argus-route._tcp` instance per logical route this service registers, each carrying its own SRV port and an `https="true"` TXT key. |
| `grpc.caller_guard` | Capability credential for the guard -> notification edge (`CreateNotifications`), paired with guard's `notifications.credential`. |
| `grpc.caller_sync` | Capability credential for `PullNotifications`; provision-host.sh pairs it with argus-sync's `notifications.credential`, so both sides carry the same minted value. |
| `notifications.budget_per_hour` and the `silent_*` / `guard_heartbeat_timeout_s` / `fallback_*` keys | The camera object policy (Phase 3d step 1), as documented for `services/notification/config.toml.example` below. |
| `nats.url` | The bus the camera notifier subscribes on; the deploy endpoint is the `nats` service. |
| `[push] enabled` | Push intents through the tunnel transport; off in a LAN-only deployment. |

## `argus-deploy/config.productivity.toml.example`

argus-deploy argus-productivity configuration. Copy to config.productivity.toml (gitignored) next to this file and fill the instance secrets (the same jwt/device values as every other service); the compose file bind-mounts it as argus-productivity's config.toml.

| Key | Notes |
|---|---|
| `productivity.host` / `productivity.port` / `productivity.plain` / `productivity.min_protocol` | The app-facing listener (`0.0.0.0:7027`), TLS unless `plain = true`; `[cert] server_cert` / `server_key` are the instance certificate it serves. |
| `mdns.enabled` / `mdns.name` | The LAN announcement: one `_argus-route._tcp` instance per logical route this service registers, each carrying its own SRV port and an `https="true"` TXT key. |
| `identity.target` | argus-identity's fleet-secret RPC listener; the sync socket reads the people rows the projections need. |
| `identity.rpc_secret` | Must match argus-identity's [identity] rpc_secret, or every identity call fails. |

## `argus-deploy/config.relay.toml.example`

argus-deploy relay configuration (Fase 5). Copy to config.relay.toml (gitignored) next to this file; the compose file bind-mounts it as argus-relay's config.toml. The [tunnel] secret must be IDENTICAL to the client's and is injected at deploy time, never committed.

- **`[push]`** — Push intents to the home client; default off. The notification rows are the source of truth — the queue only accelerates delivery.
- **`[drogon.app]`** — Mirror of the legacy [drogon.app] block for the /health listener.

## `argus-deploy/config.settings.toml.example`

argus-settings deploy configuration; `provision-host.sh` fills it.

| Key | Notes |
|---|---|
| `settings.host` / `settings.port` / `settings.plain` / `settings.min_protocol` | The app-facing TLS listener (`0.0.0.0:7045`); `[cert]` carries the certificate served and `[mdns]` the `settings` route announcement. No database, no gRPC listener. |
| `settings.list_timeout_ms` / `settings.update_timeout_ms` | Per-owner deadlines: 1500 ms for each catalog read of `GET /settings` (owners are read in parallel, so one dead owner costs one deadline), 5000 ms for a forwarded update. Values outside 1..120000 fall back to these. |
| `owners.<owner>.target` / `owners.<owner>.credential` | One table per settings owner (`llm`, `voice`, `tts`, `stt`, `vlm`, `guard`, `camera`, `notification`, also the display order). An empty target leaves the owner out. Provisioning sets the target to `argus-<owner>:<grpc port>` and mints the credential into both this file and the owner's caller slot (`[rpc.callers] settings`, or `[grpc] caller_settings` for voice, camera and notification), but only for an owner whose config carries that slot. |

## `argus-deploy/config.stt.toml.example`

argus-deploy argus-stt configuration. Copy to config.stt.toml (gitignored); the compose file bind-mounts it as the service's config.toml.

## `argus-deploy/config.sync.toml.example`

argus-deploy argus-sync configuration. Copy to config.sync.toml (gitignored) next to this file and fill the instance secrets (the same jwt/device values as every other service); the compose file bind-mounts it as argus-sync's config.toml.

| Key | Notes |
|---|---|
| `sync.db` | `database/sync.db`, this service's own file (Phase 3c-2 split it out of identity's; `argus-migrate-sync` copies the rows across), applied from this owner's `database/schema.sql` at boot. |
| `sync.audit_retention_days` | The audit TTL: rows older than the window are compacted into the nearest newer old row of the same key and a client whose cursor is older is refused with 409 so it re-bootstraps. Values <= 0 keep every row by stopping the sweep; cursors behind what an earlier sweep already deleted stay refused. |
| `sync.control_secret` | Must match the same key in every producer service's config: the control RPC injects frames into any user's room, and the service refuses to start when this listener is reachable beyond loopback without it. |
| `voice.target` | The voice service the /sync forwarder relays voice:* frames and PCM to. |
| `identity.target` | argus-identity's fleet-secret RPC listener (`argus-identity:7040`): the user directory the socket resolves names through and the identity pull source beside it. |
| `identity.rpc_secret` | Must match argus-identity's [identity] rpc_secret, or the directory and the identity pull both fail. |
| `mdns.enabled` / `mdns.name` | The LAN announcement: one `_argus-route._tcp` instance per logical route this service registers, each carrying its own SRV port and an `https="true"` TXT key. |
| `notifications.credential` | Caller capability credential for the sync -> notification pull edge; must match argus-notification's [grpc] caller_sync, and provision-host.sh pairs the two. |

## `argus-deploy/config.tts.toml.example`

argus-deploy argus-tts configuration. Copy to config.tts.toml (gitignored); the compose file bind-mounts it as the service's config.toml.

## `argus-deploy/config.tunnel.toml.example`

argus-deploy tunnel-client configuration (Fase 5). Copy to config.tunnel.toml (gitignored) next to this file; the compose file bind-mounts it as argus-tunnel-client's config.toml.

| Key | Notes |
|---|---|
| `tunnel.reconnect_wait_ms` | Home-host client knobs. |
| `tunnel.stream_idle_seconds` | Link knobs shared with the relay. |

## `argus-deploy/config.vlm.toml.example`

argus-deploy argus-vlm configuration. Copy to config.vlm.toml (gitignored); the compose file bind-mounts it as the service's config.toml.

| Key | Notes |
|---|---|
| `vision.gpu_layers` | 999 offloads every layer; a GPU-less host degrades to CPU, -1 skips the attempt. |

## `argus-deploy/config.voice.toml.example`

argus-deploy argus-voice configuration. Copy to config.voice.toml (gitignored) next to this file.

- **`[identity]`** — argus-identity's fleet-secret RPC listener; empty disables the spoken-name persist.

| Key | Notes |
|---|---|
| `identity.target` | The identity RPC behind the spoken-name lookup (`argus-identity:7040`). |
| `identity.rpc_secret` | Must match argus-identity's [identity] rpc_secret, or the spoken-name persist fails. |

## `services/identity/config.toml.example`

argus-identity configuration. Copy to config.toml (gitignored) to run.

- **`[server]`** — The gRPC listener: `host` is loopback by default, and `grpc_port` must match the port the peers' `identity.target` names.
- **`[drogon.app]`** — The HTTP listener and db_clients are built by the service itself.
- **`[cert]`** — The instance CA and server certificate; the HTTPS surface needs both.
- **`[face]`** — The face engine runs in this process: the vec0 index lives in identity.db and no crop leaves the host.
- **`[storage]`** — Private object storage for the portraits; `mode = "s3"` with the `[storage.s3]` keys.

| Key | Notes |
|---|---|
| `identity.port` | The TLS HTTP surface (7044): the app-facing listener the `/user`, `/invitation`, `/portrait-preview` and `/pairing` prefixes serve on. It is also the port the pairing and invitation answers publish, and the one the app writes into every later invitation QR. |
| `identity.db` / `identity.schema` | `database/identity.db` and this owner's `database/schema.sql`, applied at boot. |
| `identity.rpc_secret` | Gates the gRPC listener; empty is legal only while `[server] host` is loopback, and the service refuses to start otherwise. |
| `auth.target` / `auth.rpc_secret` | The session-verdict leg the filter chain asks (`argus-auth:7043`). |
| `sync.control_target` / `sync.control_secret` | The sync control plane: a role update rides `replaceRoleRooms` before the `AuthContextChanged` emit. |
| `pairing.paired` | The QR pairing state the frontend's onboarding reads; `ConfigService::setBool` persists it, so this file must stay writable. |
| `nats.url` | The broker the change feed publishes to; empty disables the publish. |
| `jwt.secret` / `jwt.refresh_secret` | The shared `JwtService` constructor reads both and refuses to start on a missing, short or default-value one; the routes verify the access token with `jwt.secret`, and `argus-auth` stays the only minter. |
| `mdns.enabled` / `mdns.name` | The LAN announcement: one `_argus-route._tcp` instance per logical route, so the app discovers this service's port directly. |
| `mdns.address` | The LAN address the announcements carry: empty enumerates the host's usable interfaces, and a value that is not an IP address falls back to the interfaces with a warning. provision-host.sh detects the host address (`--mdns-address`, `ARGUS_MDNS_ADDRESS`, `ip route get 1.1.1.1`, `hostname -I`) and writes it into every deploy config that carries the key. |

Optional keys the template does not set:

| Key | Example | Notes |
|---|---|---|
| `auth.rpc_host` / `auth.rpc_port` | `"127.0.0.1"` / `7043` | The split alternative to `auth.target`; `auth.target` wins when it is non-empty. |
| `jwt.access_ttl_minutes` / `jwt.refresh_ttl_days` | `60` / `7` | Read by the shared `JwtService` constructor; this service never mints or rotates a token, so the defaults are inert here. |
| `memory.create_face_vec` | `true` | Whether the face vec0 index is built from the schema's JSON column (unset behaves as true). |
| `remote.hostname` | `""` | The extra DNS SAN `lib/cert` appends when it rotates the instance leaf; this service's own config carries the key, and a leaf rotated without it loses that SAN. |

## `services/camera/config.toml.example`

argus-camera configuration. Copy to config.toml (gitignored) to run.

- **`[drogon.app]`** — The listener and db_clients are built by the service itself.
- **`[camera]`** — The app-facing TLS listener (`host`, `port`, `plain`, `min_protocol`); `[cert]` carries the instance certificate it serves and `[mdns]` (`enabled`, `name`) the per-route announcement.
- **`[tts]`** — With remote_url empty every /camera/{id}/talk call answers 502 CAMERA_UNREACHABLE.
- **`[nats]`** — Empty disables the change funnel (events drop with a warning).
- **`[identity]`** (optional, not in the template) — argus-identity's fleet-secret RPC listener: the guard matcher identifies and enrolls a person through it, and camera.db is the only database this service opens.

| Key | Notes |
|---|---|
| `operator.zones_from_db` | Enabled zones live in camera.db (the app editor); false uses the JSON below only. |
| `actions.enabled` | Audible/device actions from argus-guard; off until you enable them. The siren is a lease the camera must be able to release by itself: enabling arm_siren requires a hardware-side alarm timeout or watchdog, because a crashed camera process cannot execute its own sweeper. |
| `grpc.caller_guard` | Caller capability credential for the guard -> camera edge; generated per installation and shared only with argus-guard. |
| `health.enabled` | Occlusion/blur/moved detection from periodic image stats. |
| `stt.remote_url` | Camera listen (guard agent) transcription through argus-stt. |

Optional keys the template does not set:

| Key | Example | Notes |
|---|---|---|
| `identity.target` | `"127.0.0.1:7040"` | The identity RPC the guard matcher calls; unset leaves recognition off. |
| `identity.rpc_secret` | `""` | Must match argus-identity's [identity] rpc_secret. |
| `identity.identify` | `false` |  |
| `identity.auto_enroll` | `false` |  |
| `identity.capture_clear_faces` | `true` |  |
| `identity.min_face_box_px` | `48` |  |
| `identity.identify_interval_ms` | `2000` |  |
| `identity.enroll_cooldown_ms` | `600000` |  |
| `objects.classes` | `"person,bicycle,car,..." (COCO-80 default when empty)` |  |
| `operator.ignored_classes` | `"bird,cat"` |  |
| `operator.zones` | `'[{"cameraId":1,"kind":"alert","name":"door","points":[[0.0,0.0],[1.0,0.0],[1.0,1.0]]}]'` |  |

## `services/guard/config.toml.example`

argus-guard configuration. Copy to config.toml (gitignored) to run.

- **`[guard.quiet_hours]`** — Quiet-hours demotion markers. Journal-only and default off: held rows still notify exactly as before, the journal only records what a future enforce mode would have held back. Low and medium events inside the overnight window, or past the daily fired budget, are marked quiet_hold/budget_hold.
- **`[guard.belief]`** — Deterministic belief engine: closed signal vocabulary with bounded integer weights summed into a score, compared against the asymmetric severity thresholds (critical 1 < high 3 < medium 5 < low 7). gate_scope selects which effect kinds enforce mode may suppress: "notify" (default), "communication" (notify + announce) or "all". Alarm and siren-arm additionally require no hard floor, so a hard floor always lets physical effects through regardless of scope.
- **`[guard.belief.camera."1"]`** (optional, not in the template) — Per-camera overrides: any leaf above may be redefined per camera id.

| Key | Notes |
|---|---|
| `guard.host` / `guard.port` / `guard.plain` / `guard.min_protocol` | The app-facing TLS listener (`0.0.0.0:7039`); `[cert]` carries the certificate served and `[mdns]` (`enabled`, `name`) the per-route announcement. This service has no gRPC listener. |
| `guard.max_dialogue_turns` | Bounded dialogue turns and poison-message handling. |
| `guard.decision_mode` | Belief-gate mode: "shadow" journals the verdict without enforcing it, "enforce" lets the belief gate suppress effects. Unknown values fall back to shadow. |
| `guard.health_stale_s` | Freshness window for camera health readings used by the belief gate. |
| `guard.belief_refresh_s` | Belief-config refresh window in seconds; per-camera configs are cached and re-resolved after this long. |
| `guard.journal_retention_days` | Decision-journal retention in days; journal rows are decision metadata, not private evidence, so they are kept longer than evidence (90 days). Values <= 0 keep every row. |
| `guard.tamper_sustained_s` | Sustained-tamper escalation window in seconds; a tamper-ish camera health state (moved, covered, blurred) held this long raises its own high-danger notification through the durable intent path. |
| `camera.actions_credential` | Caller capability credential for the guard -> camera edge. |
| `notifications.credential` | Caller capability credential for the guard -> notification edge. |

Optional keys the template does not set:

| Key | Example | Notes |
|---|---|---|
| `guard.belief.camera."1".threshold_medium` | `6` |  |

## `services/llm/config.toml.example`

argus-llm configuration. Copy to config.toml (gitignored) to run.

| Key | Notes |
|---|---|
| `intent.model_file` | An absent file makes the router abstain; every turn stays on tool calling. |
| `memory.observe_camera_events` | Camera events become system episodes for the owner. |
| `memory.catalog_person_table` | Catalog replicas fed by the change subjects. |
| `memory.create_face_vec` | The face recognition index belongs to the face service. |
| `memory.schema_file` | The memory graph schema this service applies (`database/schema.sql`). |
| `extract.model_path` | The off-turn extractor artifact; absent leaves the lexicon tier alone. |

The catalog boot fill is fed by the `[identity] target`/`rpc_secret` and `[camera] grpc_target` clients the deploy config carries, never by a database path: the replica owns no other owner's file (rule 27).

## `services/notification/config.toml.example`

argus-notification configuration. Copy to config.toml (gitignored) to run.

| Key | Notes |
|---|---|
| `notification.host` / `notification.port` / `notification.plain` / `notification.min_protocol` | The app-facing TLS listener (`0.0.0.0:7028`); `[cert]` carries the certificate served and `[mdns]` (`enabled`, `name`) the per-route announcement. The singular table is the listener: `[notifications]` below it is the policy. |
| `notifications.ack_window_s` | Display-confirmation window in seconds; sent deliveries older than this without an ack surface as unacknowledged-old in the delivery summary. |
| `notifications.selftest_interval_s` | Synthetic delivery-probe period in seconds; <= 0 disables the probe. Each probe creates one user-0 row, publishes it and records the settle outcome. |
| `notifications.budget_per_hour` | Camera notification budget per camera per rolling hour; the digest reports what the budget held back. |
| `notifications.silent_start` / `silent_end` | Silent hours (local hour, `-1`/`-1` disables); a window that wraps midnight is honoured, and notifications inside it are counted for the digest. |
| `notifications.guard_heartbeat_timeout_s` | Freshness window for the argus-guard heartbeat: while a heartbeat is newer than this, the raw camera notifier yields to guard (seconds). |
| `notifications.fallback_min_score_median` | Degraded fallback sanity gate for hard signals while guard is absent. Absent wire keys fail open; a matched known identity never pages. |
| `notifications.fallback_min_dwell_ms` | Minimum dwell a hard-signal track must show before the fallback forwards it (milliseconds). |
| `notifications.fallback_suppress_known` | Whether a matched known identity suppresses a hard signal in the fallback path. |
| `notifications.fallback_retention_days` | Fallback-record retention in days, matching the guard decision journal. Values <= 0 keep every row. |
| `identity.target` | The identity roster the camera notifier resolves its recipients from. Empty keeps the fallback record but reaches no recipient. |
| `identity.rpc_secret` | Fleet secret the roster call carries; same value in every service. |
| `nats.url` | The event bus the camera notifier subscribes on (`argus.camera.v1.object_detected`, `argus.guard.v1.heartbeat`); empty leaves the bus disabled and camera notifications off. |
| `grpc.caller_guard` | Capability credential for the guard -> notification edge (`CreateNotifications`). |
| `grpc.caller_sync` | Capability credential for `PullNotifications`, which argus-sync presents from its own `notifications.credential`; provision-host.sh pairs the two. |

## `services/auth/config.toml.example`

argus-auth configuration. Copy to config.toml (gitignored) to run.

| Key | Notes |
|---|---|
| `auth.db` | The session database this service alone opens. |
| `auth.host` | The HTTP listener's bind address; the `/auth` surface serves on it. `auth.port`, `auth.plain` and `auth.min_protocol` complete the listener and `[cert]` carries the instance certificate it serves — TLS unless `plain = true`. |
| `auth.rpc_secret` | Fleet secret for the session-verdict RPC. Required whenever its listener is reachable beyond loopback: an unauthenticated caller could read any session's verdict. Empty means loopback-only and ungated. |
| `auth.context_cache_seconds` | How long a resolved user context may be reused before identity is asked again (0 disables the cache); a change on the identity feed drops the entry immediately. |
| `mdns.enabled` / `mdns.name` | The LAN announcement: one `_argus-route._tcp` instance per logical route this service registers, each carrying its own SRV port and an `https="true"` TXT key. |
| `identity.target` | The identity RPC behind a session verdict. An empty value falls back to `identity.rpc_host` / `identity.rpc_port`, which default to `127.0.0.1:7040`, so the leg is always dialled. |

## `services/productivity/config.toml.example`

argus-productivity configuration. Copy to config.toml (gitignored) to run.

- **`[productivity]`** — The app-facing TLS listener (`host`, `port`, `plain`, `min_protocol`); `[cert]` carries the instance certificate it serves and `[mdns]` (`enabled`, `name`) the per-route announcement.

## `services/settings/config.toml.example`

argus-settings configuration. `scripts/setup.sh` copies it to `config.toml`
and wires every owner whose `config.toml` carries a settings caller slot: the
credential on both sides and the target `127.0.0.1:<grpc port>` (tts, stt,
vlm and llm need a non-empty `[rpc] address` for a target). The keys are the
deploy template's, with `auth.target` on loopback.

## `services/stt/config.toml.example`

argus-stt configuration. Copy to config.toml (gitignored) to run.

- **`[drogon.app]`** — The body limit covers long PCM turns.

## `services/sync/config.toml.example`

argus-sync configuration. Copy to config.toml (gitignored) to run.

| Key | Notes |
|---|---|
| `sync.host` | The TLS listener /sync terminates on: clients dial it directly, because a WebSocket upgrade cannot ride a reverse proxy. |
| `sync.audit_retention_days` | The audit TTL: rows older than the window are compacted into the nearest newer old row of the same key and a client whose cursor is older is refused with 409 so it re-bootstraps. Values <= 0 keep every row by stopping the sweep; cursors behind what an earlier sweep already deleted stay refused. |
| `sync.control_secret` | Fleet secret for the control RPC. Required whenever its listener is reachable beyond loopback: an unauthenticated caller injects frames into any user's room. |
| `voice.target` | The voice service the /sync forwarder relays voice:* frames and PCM to. |
| `notifications.credential` | Caller capability credential for the sync -> notification pull edge. |
| `mdns.enabled` / `mdns.name` | The LAN announcement: one `_argus-route._tcp` instance per logical route this service registers, each carrying its own SRV port and an `https="true"` TXT key. |

## `services/tts/config.toml.example`

argus-tts configuration. Copy to config.toml (gitignored) to run.

## `services/tunnel/config.toml.example`

argus-tunnel configuration. Copy to config.toml (gitignored) to run. One file serves both binaries: argus-relay reads the [server] block's relay keys on the US host, argus-tunnel-client reads the client keys on the home host.

- **`[push]`** — Relay-only push-intent gate; default off.

| Key | Notes |
|---|---|
| `tunnel.secret` | Empty refuses to start (both binaries). |
| `tunnel.reconnect_wait_ms` | Client-only knobs. |
| `tunnel.stream_idle_seconds` | Shared link knobs. |
| `tunnel.socket_snd_buf` | SO_SNDBUF in bytes for every tunnel socket (0 = kernel-managed). |
| `server.relay_host` | Client dial targets. |
| `server.health_host` | Defaults differ per binary; the home host's copy sets health_port = 7104. |

## `services/vlm/config.toml.example`

argus-vlm configuration. Copy to config.toml (gitignored) to run.

- **`[drogon.app]`** — The body limit covers base64 JPEG frames.

## `services/voice/config.toml.example`

argus-voice configuration. Copy to config.toml (gitignored) to run. Pure gRPC voice service; no database, the only model is the in-process silero VAD.

- **`[identity]`** — Empty disables the spoken-name persist (logged, session continues).

| Key | Notes |
|---|---|
| `stt.language` | Fallback when the stream identity carries VOICE_LANGUAGE_SYSTEM. |
