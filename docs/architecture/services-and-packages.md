# Services and packages

Argus is a set of independent processes (services) plus reusable libraries
(packages). Services own listeners and data; packages are compiled into their
consumers. The gateway is the only public entry point.

## Runtime services

| Service | Role | Listeners | Owns data |
|---|---|---|---|
| `argus-gateway` | Public TLS API, WebSocket relay, identity host | HTTPS 7024 | `identity.db` |
| `argus-camera` | Camera/zone data, go2rtc streaming, object events | HTTP 7026, gRPC 7036 | `camera.db` |
| `argus-productivity` | Reminders, projects, calendar | HTTP 7027 | `productivity.db` |
| `argus-notification` | Notifications and push tokens | HTTP 7028 | `notification.db` |
| `argus-tts` | Supertonic ONNX speech synthesis | HTTP 7029 | — |
| `argus-stt` | sherpa-onnx speech recognition | HTTP 7030 | — |
| `argus-vlm` | LFM2.5-VL image understanding | HTTP 7031 | — |
| `argus-llm` | LFM2.5 chat, intent router, tool loop, hosted memory | HTTP 7032 | `memory.db` |
| `argus-voice` | Voice-session orchestration over gRPC | gRPC 7034, health 7035 | — |
| `argus-tunnel` | Byte-transparent client and relay transport | per config | — |

Every service binds loopback or the deployment's private network; the gateway
proxies the public surface. Core NATS (`4222`) carries change events; typed
gRPC covers camera sync, voice sessions and identity operations.

## Standalone packages

These own a Conan/CMake graph and build on their own:

| Package | Responsibility |
|---|---|
| `argus-common` | Shared enums, contracts, validation, config, responses, wrappers |
| `argus-contracts` | Protobuf contracts and typed internal SDKs |
| `argus-cert` | Instance CA and certificate issuance/rotation |
| `argus-socket` | Room/socket emission (`SocketService`) |
| `argus-sqlite` | Database client access and vec0 (`DbService`, `VecDb`) |
| `argus-identity` | Users, persons, invitations, portraits, face stack |
| `argus-sync` | Sync engine and notifications |
| `argus-memory` | Semantic-graph memory (hosted by `argus-llm`) |
| `argus-intent` | fastText intent router (hosted by `argus-llm`) |

## Direct-import packages

`argus-auth`, `argus-audio`, `argus-audit`, `argus-room`, `argus-phrase`,
`argus-llm-client`, `argus-stt-client`, `argus-tts-client`. These have no
Conan graph of their own: the service that links them provides the build
context. They are declared once in their folder and linked by target name.

## Dependency direction

- Services depend on packages and on internal clients, never on another
  service's source.
- Cross-service calls go through `packages/argus-contracts` generated SDKs or
  the shared wire clients.
- A package never depends on a service.

## Third-party dependencies

Vendored sources stay shared in `third_party/`; every project declares which
of them it compiles. The consumer map and pins live in
[build-model.md](build-model.md#third-party-map).
