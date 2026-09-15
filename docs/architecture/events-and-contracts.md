# Events and contracts

## NATS event bus

- `NatsBus` wraps cnats; subscriptions may register before `connect()` and
  activate once the connection is up.
- Subject convention: `argus.<domain>.v1.<event>`, lowercase segments and
  `snake_case` event names. Subjects are frozen once published; wildcards are
  subscription-only.
- Concrete subjects required by `/sync`: `argus.sync.v1.change`,
  `argus.camera.v1.change`, `argus.camera.v1.object_detected`,
  `argus.productivity.v1.change`, `argus.notification.v1.change`.
- Durable JetStream legs (PubAck settlement, inbox receipts, at-least-once):
  `argus.camera.v1.object_detected` → guard (`ARGUS_CAMERA`),
  `argus.guard.v1.encounter_closed` → argus-llm (`ARGUS_GUARD`),
  `argus.notification.v1.delivery` → gateway (`ARGUS_NOTIFICATION`).
  Same id plus same canonical fingerprint is a replay; same id plus a
  different fingerprint is a conflict that is never dispatched.
- Readiness and health signals: `argus.guard.v1.heartbeat` (the gateway's raw
  camera notifier yields while fresh) and `argus.camera.v1.health`.
- The gateway subscribes the frozen wildcard `argus.*.v1.change` and routes
  by concrete subject; camera audit diffs are persisted before fan-out.

The full subject contract lives in `wire-nats-subjects.md`.

## Typed gRPC contracts

- `packages/argus-contracts/proto/argus/<domain>/v1/` holds the protobuf
  schemas; `argus_sdk_module()` wraps the generated stubs so consumers never
  see protobuf directly.
- Camera sync (`argus.camera.v1.SyncService`, port 7036) serves the camera
  sync tables with the frozen `/sync` semantics.
- Camera actions (`argus.camera.v1.CameraActionService`, port 7036) serve the
  guard-only audible/physical surface (`Announce`, `Alarm`, `SetSiren`,
  `GetPersonCrop`, `Listen`): fleet-secret gated, idempotent by `command_id`,
  siren arming as an expiring lease. The gateway never calls it.
- Voice (`argus.voice.v1.VoiceService`, port 7034) serves voice sessions.
- Identity (`argus.identity.v1`) exposes gateway-owned operations such as the
  spoken-name update, plus the fleet-gated person surface (`IdentifyPerson`,
  `EnrollPerson`, `TouchPerson`, `TagPerson`, `PromotePerson`,
  `ListNotifiableUsers`) that feeds camera→guard identity.
- Notification (`argus.notification.v1`, port 7038) fans out creates by
  idempotent `command_id` and serves the user-scoped sync page.
- Caller authority (`sdk/grpc/grpc-server-identity.hxx`): service-to-service
  calls present `x-argus-credential`; the receiver matches it against its
  configured caller set and the authority comes from the matched secret, never
  from declared `x-argus-user`/`x-argus-role` metadata.
- All listeners share `grpc.health.v1.Health`.

## Stability rules

Wire contracts are frozen: public paths, the `{status, info, errors}`
envelope, error codes and `SyncOperation` values 0–6 do not change. New
capabilities are additive; retired protobuf fields are `reserved`.
