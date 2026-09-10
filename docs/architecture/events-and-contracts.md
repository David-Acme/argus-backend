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
- The gateway subscribes the frozen wildcard `argus.*.v1.change` and routes
  by concrete subject; camera audit diffs are persisted before fan-out.

The full subject contract lives in `wire-nats-subjects.md`.

## Typed gRPC contracts

- `packages/argus-contracts/proto/argus/<domain>/v1/` holds the protobuf
  schemas; `argus_sdk_module()` wraps the generated stubs so consumers never
  see protobuf directly.
- Camera sync (`argus.camera.v1.SyncService`, port 7036) serves the camera
  sync tables with the frozen `/sync` semantics.
- Voice (`argus.voice.v1.VoiceService`, port 7034) serves voice sessions.
- Identity (`argus.identity.v1`) exposes gateway-owned operations such as the
  spoken-name update.
- All listeners share `grpc.health.v1.Health`.

## Stability rules

Wire contracts are frozen: public paths, the `{status, info, errors}`
envelope, error codes and `SyncOperation` values 0–6 do not change. New
capabilities are additive; retired protobuf fields are `reserved`.
