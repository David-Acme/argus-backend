# Configuration

## Per-project config.toml

Every runtime owner reads its own `config.toml`, generated from the adjacent
committed `config.toml.example` by `setup.sh` (0600, gitignored). There is no
root monolithic config; secrets never live in the repository.

Service templates expose only their own domain plus `[server]` (and `[nats]`
where used):

| Project | Notable sections |
|---|---|
| gateway | `[gateway]`, `[identity]`, `[sync]`, `[nats]`, `[cert]`, `[mdns]` |
| camera | `[server]`, `[database]`, `[camera]`, `[streaming]`, `[objects]`, `[tapo]`, `[tts]` |
| productivity | `[server]`, `[database]` |
| notification | `[server]`, `[database]` |
| tts / stt / vlm / llm / voice | their engine section plus `[server]` |
| memory (hosted) | `[memory]`, `[extract]` |

## Secrets

`setup.sh` injects only the keys each template declares: JWT access/refresh
secrets, the device fingerprint secret and the identity RPC secret, all
generated locally. Values are never printed, logged or committed, and the app
receives tokens only.

## Runtime writes

`ConfigService` reads the TOML and supports `setBool/setString/setInt/...`,
which update memory and persist a surgical line edit that preserves comments
(for example `[pairing] paired` after a successful pairing). Use it for
one-time flags that must survive restarts without a database table.

## Conan substrate

`cmake/argus-module.cmake` resolves the protobuf/gRPC stack: Debian
pkg-config packages when present (image build), otherwise the vendored
`~/.local/argus-thirdparty/grpc` fallback (Arch hosts). `sqlite3` is a direct
require in every project's `conanfile.txt`, with `enable_fts5=True` and
Drogon's `with_sqlite=True`.
