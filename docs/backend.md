# Argus Backend

Argus is a local-first C++20 platform for home security and assistance. The
backend runs as independent domain and AI services behind one public gateway.
Media, speech, vision, language-model and biometric processing stay on the
host; the optional tunnel transports encrypted bytes without processing user
data.

## Architecture

| Owner | Process or package | Primary responsibility |
|---|---|---|
| Gateway | `argus-gateway` | Public TLS API, WebSocket relay, identity host and routing |
| Camera | `argus-camera` | Camera/zone data, go2rtc media, YOLO26n object events |
| Productivity | `argus-productivity` | Reminders, projects and calendar data |
| Notification | `argus-notification` | Notifications and device push tokens |
| TTS | `argus-tts` | Supertonic ONNX speech synthesis |
| STT | `argus-stt` | sherpa-onnx speech recognition |
| VLM | `argus-vlm` | LFM2.5-VL image understanding |
| LLM | `argus-llm` | LFM2.5 chat, intent routing, tool execution and hosted memory |
| Voice | `argus-voice` | Pure-gRPC voice-session orchestration |
| Tunnel | `argus-tunnel-client`, `argus-tunnel-relay` | Byte-transparent remote transport |
| Contracts | `argus-contracts` | Versioned protobuf contracts and typed internal SDKs |

The gateway is the only public surface. Internal services bind to loopback or
the deployment's private network. Core NATS carries change and object events;
typed gRPC contracts cover health, sync, voice and identity operations.

SQLite ownership is split by domain. Identity, camera, productivity,
notification and memory each own their data; the retired `argus.db` is not a
runtime database. Cross-owner access is read-only or goes through a typed
service contract, as recorded in each owner's `CONTEXT.md`.

## Build model

The repository root intentionally has no `CMakeLists.txt`,
`CMakePresets.json` or `conanfile.txt`. Nineteen standalone owner projects
each carry their own Conan graph and `dev`/`prod` CMake presets.

Build and test all projects:

```bash
./scripts/build-all.sh dev
./scripts/build-all.sh prod
```

Useful scoped modes:

```bash
./scripts/build-all.sh dev --only argus-camera
./scripts/build-all.sh prod --no-tests
./scripts/build-all.sh dev --install-only
```

Build one project directly:

```bash
cd services/argus-camera
conan install . --output-folder=build/dev \
  -s build_type=Debug --build=missing
cmake --preset dev
cmake --build --preset dev -j 8
ctest --test-dir build/dev --output-on-failure
```

`scripts/setup.sh` installs host dependencies, creates ignored local configs,
provisions model artifacts and delegates the build to `build-all.sh`:

```bash
./scripts/setup.sh dev
./scripts/setup.sh prod
SKIP_BUILD=1 ./scripts/setup.sh prod
./scripts/setup.sh camera
```

## Local runtime

Each process reads its own ignored `config.toml`, generated from the adjacent
`config.toml.example`. The main default listeners are:

| Service | Listener |
|---|---|
| Gateway | HTTPS `7024` |
| Camera | HTTP `7026`, gRPC `7036` |
| Productivity | HTTP `7027` |
| Notification | HTTP `7028` |
| TTS | HTTP `7029` |
| STT | HTTP `7030` |
| VLM | HTTP `7031` |
| LLM | HTTP `7032` |
| Voice | gRPC `7034`, health HTTP `7035` |

Standalone binaries are produced inside their owner folder, for example:

```bash
./services/argus-gateway/build/dev/argus-gateway
./services/argus-camera/build/dev/argus-camera
./services/argus-llm/build/dev/argus-llm
```

Start dependencies before consumers. For the complete topology and health
ordering, prefer Compose rather than launching every process by hand.

## Container deployment

The deployment uses one multi-stage image containing all runtime binaries.
The build stage calls the same standalone Release orchestrator as local work:

```bash
docker build -f argus-deploy/Dockerfile -t argus-cutover:local .
cd argus-deploy
ARGUS_UID="$(id -u)" ARGUS_GID="$(id -g)" docker compose up -d
```

Models are not baked into the image. They are provisioned on the host and
mounted read-only. Service data and generated secrets are also excluded from
Git. Never print or commit a real `config.toml`, certificate key or database.

## Camera object detection

`argus-camera` implements object detection with YOLO26n through ncnn. When
`[objects].enabled` is true, the service:

1. obtains JPEG frames from its managed go2rtc instance;
2. decodes and letterboxes frames to the configured detector size;
3. runs the local `models/objects/yolo26n` ncnn graph, with Vulkan-to-CPU
   fallback;
4. tracks configured classes and evaluates zones, cooldown and presence
   rules; and
5. publishes `object_detected` events to NATS.

Provision only the detector and go2rtc artifacts with:

```bash
./scripts/setup.sh camera
```

The operator is read-only toward camera hardware. It never arms an alarm or
siren. Overlay output is disabled by default and is intended only for local
diagnostics.

## API invariants

Public JSON responses use the envelope:

```json
{
  "status": 200,
  "info": {},
  "errors": null
}
```

Authentication uses device-bound HS256 access and refresh tokens. Public
routes preserve their pre-migration contracts; the gateway proxies them to
the owning service. Health endpoints must remain fast and available even when
NATS, models, cameras or downstream services are degraded.

## Engineering rules

- C++20 with `.hxx` headers, `.cc` sources and hyphenated `*-test.cc` tests.
- Zero warnings in Argus-owned code under `-Wall -Wextra`.
- Raw SQL, no ORM; migrations belong to the database owner.
- Parameter structs for functions with three or more parameters.
- Smart ownership and bounded queues; no raw owning pointers or unbounded
  transport buffers.
- English-only source, comments, documentation and commits.
- Never trigger camera alarms or sirens during development or tests.

The binding rules live in the root `AGENTS.md`; service-specific constraints
live in each owner folder's `AGENTS.md` and architectural decisions in its
`CONTEXT.md`.
