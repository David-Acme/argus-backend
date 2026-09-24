# Configuration keys

What each key of the configuration templates is for, and the optional keys a
template does not set. The templates carry no comments (rule 20); this page is
where their notes live. Per-project templates are `<project>/config.toml.example`,
the deploy stack's are `argus-deploy/config.<service>.toml.example`.

## `argus-deploy/config.camera.toml.example`

argus-deploy argus-camera configuration. Copy to config.camera.toml (gitignored) next to this file and fill the instance secrets (the same jwt/device values as config.gateway.toml); the compose file bind-mounts it as argus-camera's config.toml.

| Key | Notes |
|---|---|
| `identity.rpc_secret` | Must match the gateway's [identity] rpc_secret, or every request 401s. |
| `grpc.caller_guard` | Capability credential for the guard -> camera edge (paired with guard's camera.actions_credential). |
| `grpc.identify` | Camera guard recognition; enroll/capture stay off until you enable them. |
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

## `argus-deploy/config.gateway.toml.example`

argus-deploy gateway configuration. Copy to config.gateway.toml (gitignored) next to this file and fill the instance secrets; the compose file bind-mounts it as the gateway's config.toml. The container runs host-networked (transitional Ruling O exception), so 127.0.0.1 reaches the service backends.

| Key | Notes |
|---|---|
| `gateway.db` | Gateway-owned fallback record; identity and sync state stay in their owners. database/schema.sql stays the identity schema; the gateway schema mounts separately because this container hosts both databases. |
| `identity.rpc_host` | Cleartext listener: bind loopback or the pinned bridge address only. |
| `identity.rpc_secret` | Required whenever rpc_host is not loopback; same value in every service. |
| `sync.control_target` | The sync service's control plane (unary gRPC); empty disables the imperative leg, leaving role changes and disconnects undelivered. |
| `sync.control_secret` | Required whenever the control target is set; same value in every service. |
| `storage.mode` | Private object storage (RustFS). provision-host.sh fills the [storage.s3] keys with the generated bucket and application credentials; the endpoint is the host loopback publish because the gateway runs host-networked. |

## `argus-deploy/config.guard.toml.example`

argus-guard deploy configuration. Copy to config.guard.toml (gitignored).

- **`[guard.quiet_hours]`** — Quiet-hours demotion markers. Journal-only and default off: held rows still notify exactly as before, the journal only records what a future enforce mode would have held back.
- **`[guard.belief]`** — Deterministic belief engine: closed signal vocabulary with bounded integer weights summed into a score, compared against the asymmetric severity thresholds (critical 1 < high 3 < medium 5 < low 7). gate_scope selects which effect kinds enforce mode may suppress: "notify" (default), "communication" (notify + announce) or "all". Alarm and siren-arm additionally require no hard floor, so a hard floor always lets physical effects through regardless of scope.
- **`[guard.belief.camera."1"]`** (optional, not in the template) — Per-camera overrides: any leaf above may be redefined per camera id.

| Key | Notes |
|---|---|
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

argus-deploy argus-notification configuration. Copy to config.notification.toml (gitignored) next to this file and fill the instance secrets (the same jwt/device values as config.gateway.toml); the compose file bind-mounts it as argus-notification's config.toml.

| Key | Notes |
|---|---|
| `identity.rpc_secret` | Must match the gateway's [identity] rpc_secret, or every request 401s. |
| `grpc.caller_guard` | Capability credentials, each paired with its single caller. |

## `argus-deploy/config.productivity.toml.example`

argus-deploy argus-productivity configuration. Copy to config.productivity.toml (gitignored) next to this file and fill the instance secrets (the same jwt/device values as config.gateway.toml); the compose file bind-mounts it as argus-productivity's config.toml.

| Key | Notes |
|---|---|
| `identity.rpc_secret` | Must match the gateway's [identity] rpc_secret, or every request 401s. |

## `argus-deploy/config.relay.toml.example`

argus-deploy relay configuration (Fase 5). Copy to config.relay.toml (gitignored) next to this file; the compose file bind-mounts it as argus-relay's config.toml. The [tunnel] secret must be IDENTICAL to the client's and is injected at deploy time, never committed.

- **`[push]`** — Push intents to the home client; default off. The notification rows are the source of truth — the queue only accelerates delivery.
- **`[drogon.app]`** — Mirror of the legacy [drogon.app] block for the /health listener.

## `argus-deploy/config.stt.toml.example`

argus-deploy argus-stt configuration. Copy to config.stt.toml (gitignored); the compose file bind-mounts it as the service's config.toml.

## `argus-deploy/config.sync.toml.example`

argus-deploy argus-sync configuration. Copy to config.sync.toml (gitignored) next to this file and fill the instance secrets (the same jwt/device values as config.gateway.toml); the compose file bind-mounts it as argus-sync's config.toml.

| Key | Notes |
|---|---|
| `sync.db` | The five sync-owned tables still live in identity's file (Phase 3c splits them into sync.db); the schema mount below is this service's own. |
| `sync.audit_retention_days` | The audit TTL: rows older than the window are compacted into the nearest newer old row of the same key and a client whose cursor is older is refused with 409 so it re-bootstraps. Values <= 0 keep every row by stopping the sweep; cursors behind what an earlier sweep already deleted stay refused. |
| `sync.control_secret` | Must match the same key in every producer service's config: the control RPC injects frames into any user's room, and the service refuses to start when this listener is reachable beyond loopback without it. |
| `voice.target` | The voice service the /sync forwarder relays voice:* frames and PCM to. |
| `notifications.credential` | Caller capability credential for the sync -> notification pull edge; must match argus-notification's [grpc] caller_gateway. |

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

- **`[identity]`** — Gateway internal identity RPC; empty disables the spoken-name persist.

| Key | Notes |
|---|---|
| `identity.rpc_secret` | Must match the gateway's [identity] rpc_secret, or every request 401s. |

## `packages/memory/config.toml.example`

argus-memory package keys (f8-b3). The package has no process and no listener: the host service (argus-llm, the brain) carries these blocks in its own config.toml. Copy the block, not the file.

- **`[identity]`** — Read-only snapshot sources for the catalog replica boot fill.

| Key | Notes |
|---|---|
| `memory.catalog_person_table` | Catalog replicas fed by the change subjects. |
| `memory.create_face_vec` | The face recognition index belongs to the face service. |

## `services/camera/config.toml.example`

argus-camera configuration. Copy to config.toml (gitignored) to run.

- **`[drogon.app]`** — The listener and db_clients are built by the service itself.
- **`[tts]`** — With remote_url empty every /camera/{id}/talk call answers 502 CAMERA_UNREACHABLE.
- **`[nats]`** — Empty disables the change funnel (events drop with a warning).
- **`[identity]`** (optional, not in the template) — identity.db points at the gateway's; the sync socket reads its user table. target is the gateway's internal identity RPC listener.

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
| `identity.db` | `"database/identity.db"` |  |
| `identity.target` | `"127.0.0.1:7040"` |  |
| `identity.rpc_secret` | `""` |  |
| `identity.identify` | `false` |  |
| `identity.auto_enroll` | `false` |  |
| `identity.capture_clear_faces` | `true` |  |
| `identity.min_face_box_px` | `48` |  |
| `identity.identify_interval_ms` | `2000` |  |
| `identity.enroll_cooldown_ms` | `600000` |  |
| `objects.classes` | `"person,bicycle,car,..." (COCO-80 default when empty)` |  |
| `operator.ignored_classes` | `"bird,cat"` |  |
| `operator.zones` | `'[{"cameraId":1,"kind":"alert","name":"door","points":[[0.0,0.0],[1.0,0.0],[1.0,1.0]]}]'` |  |

## `services/gateway/config.toml.example`

argus-gateway configuration. Copy to config.toml (gitignored) to run. The gateway owns the public TLS 7024 listener and forwards each extracted domain to its service backend through the proxy route table.

- **`[drogon.app]`** — The listeners, db_clients and plugins are built by the gateway itself.

| Key | Notes |
|---|---|
| `gateway.db` | Gateway-owned fallback record; identity and sync state stay in their owners. |
| `sync.control_target` | The sync service's control plane (unary gRPC); empty disables the imperative leg, leaving role changes and disconnects undelivered. |
| `sync.control_secret` | Fleet secret every control call carries; same value in every service. |
| `notifications.credential` | Caller capability credential for the gateway -> notification edge. |
| `notifications.guard_heartbeat_timeout_s` | Freshness window for the argus-guard heartbeat: while a heartbeat is newer than this, the raw camera notifier yields to guard (seconds). |
| `notifications.fallback_min_score_median` | Degraded fallback sanity gate for hard signals while guard is absent. Absent wire keys fail open; a matched known identity never pages. |
| `notifications.fallback_retention_days` | Fallback-record retention in days, matching the guard decision journal. Values <= 0 keep every row. |

## `services/guard/config.toml.example`

argus-guard configuration. Copy to config.toml (gitignored) to run.

- **`[guard.quiet_hours]`** — Quiet-hours demotion markers. Journal-only and default off: held rows still notify exactly as before, the journal only records what a future enforce mode would have held back. Low and medium events inside the overnight window, or past the daily fired budget, are marked quiet_hold/budget_hold.
- **`[guard.belief]`** — Deterministic belief engine: closed signal vocabulary with bounded integer weights summed into a score, compared against the asymmetric severity thresholds (critical 1 < high 3 < medium 5 < low 7). gate_scope selects which effect kinds enforce mode may suppress: "notify" (default), "communication" (notify + announce) or "all". Alarm and siren-arm additionally require no hard floor, so a hard floor always lets physical effects through regardless of scope.
- **`[guard.belief.camera."1"]`** (optional, not in the template) — Per-camera overrides: any leaf above may be redefined per camera id.

| Key | Notes |
|---|---|
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

## `services/notification/config.toml.example`

argus-notification configuration. Copy to config.toml (gitignored) to run.

| Key | Notes |
|---|---|
| `notifications.ack_window_s` | Display-confirmation window in seconds; sent deliveries older than this without an ack surface as unacknowledged-old in the delivery summary. |
| `notifications.selftest_interval_s` | Synthetic delivery-probe period in seconds; <= 0 disables the probe. Each probe creates one user-0 row, publishes it and records the settle outcome. |
| `grpc.caller_guard` | Caller capability credentials, each shared only with its single caller. |

## `services/productivity/config.toml.example`

argus-productivity configuration. Copy to config.toml (gitignored) to run.

## `services/stt/config.toml.example`

argus-stt configuration. Copy to config.toml (gitignored) to run.

- **`[drogon.app]`** — The body limit covers long PCM turns.

## `services/sync/config.toml.example`

argus-sync configuration. Copy to config.toml (gitignored) to run.

| Key | Notes |
|---|---|
| `sync.host` | The TLS listener /sync terminates on: clients dial it directly, because a WebSocket upgrade cannot ride the gateway's reverse proxy. |
| `sync.audit_retention_days` | The audit TTL: rows older than the window are compacted into the nearest newer old row of the same key and a client whose cursor is older is refused with 409 so it re-bootstraps. Values <= 0 keep every row by stopping the sweep; cursors behind what an earlier sweep already deleted stay refused. |
| `sync.control_secret` | Fleet secret for the control RPC. Required whenever its listener is reachable beyond loopback: an unauthenticated caller injects frames into any user's room. |
| `voice.target` | The voice service the /sync forwarder relays voice:* frames and PCM to. |
| `notifications.credential` | Caller capability credential for the sync -> notification pull edge. |

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
