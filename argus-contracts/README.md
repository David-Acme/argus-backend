# argus-contracts

Source of truth for Argus wire contracts: protobuf v1 skeletons, capability
package/plugin manifest schemas and the frozen sync wire values. Sibling repo
of `backend/` (C++20 + Drogon) and `frontend/` (React Native); services and the
mobile app coordinate against this repo.

## Layout

```
proto/argus/{common,camera,ai,productivity,notification,memory,sync}/v1/*.proto
manifests/package.schema.json   typed capability package manifest
manifests/plugin.schema.json    ed25519-signed plugin manifest
sync/README.md                  frozen SyncOperation 0-7 + TableName 0-23 + SYNC_LIMIT=200
sync/fixtures/                  golden /sync frames
buf.yaml  buf.gen.yaml          lint + C++ codegen
```

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