# argus-contracts

Source of truth for Argus wire contracts: protobuf v1 skeletons, capability
package/plugin manifest schemas and the frozen sync wire values. Top-level
folder of the Argus monorepo (rooted at `backend/`), versioned via repo tags
(`contracts-v*`), not an independent repo; services (`backend/` C++20 + Drogon)
and the mobile app (`frontend/` React Native) coordinate against this folder.

## Layout

```
proto/argus/{common,camera,ai,productivity,notification,memory,sync}/v1/*.proto
manifests/package.schema.json   typed capability package manifest
manifests/plugin.schema.json    ed25519-signed plugin manifest
sync/fixtures/                  golden /sync frames
identity/                       device identity contract
buf.yaml  buf.gen.yaml          lint + C++ codegen
```

Wire contracts documented under `docs/architecture/`:
`wire-sync-tables.md`, `wire-device-identity.md`,
`wire-sync-golden-frames.md`, `wire-camera-media.md` and
`wire-nats-subjects.md`.

## Service-to-service auth and the camera action surface

- `sdk/grpc/grpc-server-identity.hxx` types the caller-credential edge:
  `x-argus-credential` matched against the receiver's configured caller set
  (`CallerCredential{service, secret}`); authority comes from the matched
  secret, never from declared user/role metadata. Comparison is
  constant-time.
- `proto/argus/camera/v1/actions.proto` (`CameraActionService`) is the only
  audible/physical surface: `Announce`, `Alarm`, `SetSiren`, `GetPersonCrop`,
  `Listen`, with `CommandOutcome` (`SUCCEEDED`, `DUPLICATE_SUCCEEDED`,
  `IN_FLIGHT`, `INDETERMINATE`, `REJECTED`, `RETRYABLE_FAILED`, `CONFLICT`).
  The thin wrapper is `argus::sdk-camera-actions`
  (`sdk/camera/camera-action-client.cc`).
- `command_id` is the idempotency key on the notification
  `CreateNotifications` fan-out and on every camera action command.

## Validation

```sh
buf lint
buf breaking --against '.git#branch=main'
```

If buf is not installed, protoc works too:

```sh
protoc --descriptor_set_out=/dev/null -I proto $(git ls-files 'proto/*.proto')
```

## Versioning policy

Packages `argus.<domain>.v1`. Additive changes only; retired fields are
`reserved` and replaced by new fields. Breaking changes move to `v2`. See
`AGENTS.md` for the frozen-contract policy.
