# Argus Backend Monorepo

Argus is a local-first home security and assistant platform written in C++20.
The backend is a set of independently buildable services plus shared packages;
there is no root CMake project or monolithic backend executable.

## Layout

- `services/<name>/` — deployable processes, each configuring against the
  one root Conan graph
- `packages/lib/` — reusable libraries compiled into their consumers
- `packages/contracts/` — protobuf and wire contracts, imported directly
- `packages/clients/` — internal gRPC and HTTP clients for those wires
- `packages/<name>/` — the packages not yet in a group (`identity`,
  `memory`, `sync`, …)
- `third_party/` — pinned source dependencies maintained as Git submodules
- `argus-deploy/` — the multi-service Compose stack; every service builds its
  own image
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
./scripts/build-all.sh dev --only camera
```

One `conanfile.txt` at the repository root is the whole tree's dependency
manifest, resolved once by `scripts/build-all.sh`; each orchestrated project
owns its `CMakeLists.txt` and configures against the toolchain that install
produced. See [build-and-test.md](docs/operations/build-and-test.md) for
working inside one project.

## Runtime

The gateway is the only public entry point. Camera, productivity,
notification, TTS, STT, VLM, LLM, voice, guard and tunnel capacities run as separate
processes behind it, each from its own image. See
`argus-deploy/docker-compose.yml` for the complete topology and
`docs/operations/deployment-docker.md` for the image build.

Internal versioning uses repository tags: `contracts-v*` for contracts and
`service-v*` for service releases.
