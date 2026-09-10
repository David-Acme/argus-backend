# Argus Backend Monorepo

Argus is a local-first home security and assistant platform written in C++20.
The backend is a set of independently buildable services plus shared packages;
there is no root CMake project or monolithic backend executable.

## Layout

- `services/argus-<name>/` — deployable processes, each with its own Conan
  graph and CMake presets
- `packages/argus-<name>/` — reusable libraries compiled into their consumers
- `packages/argus-contracts/` — protobuf contracts and internal gRPC SDKs
- `third_party/` — pinned source dependencies maintained as Git submodules
- `argus-deploy/` — the multi-service Compose stack and shared image
- `scripts/` — provisioning, setup and the standalone build orchestrator
- `models/` — shared runtime model locations; large artifacts are gitignored
- `docs/` — architecture, migration history and operational guidance

## Build

Provision local configuration, models and dependencies, then build and test all
projects:

```bash
./scripts/setup.sh dev
```

Build an already provisioned checkout:

```bash
./scripts/build-all.sh dev
./scripts/build-all.sh prod --no-tests
./scripts/build-all.sh dev --only argus-camera
```

Every orchestrated project owns `CMakeLists.txt`, `conanfile.txt` and
`CMakePresets.json`. To work directly inside one, run its Conan install, CMake
preset and CTest commands from that project folder.

## Runtime

The gateway is the only public entry point. Camera, productivity,
notification, TTS, STT, VLM, LLM, voice and tunnel capacities run as separate
processes behind it. See `argus-deploy/docker-compose.yml` for the complete
topology and `docs/backend.md` for local commands.

Internal versioning uses repository tags: `contracts-v*` for contracts and
`service-v*` for service releases.
