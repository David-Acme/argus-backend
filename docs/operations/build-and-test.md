# Build and test

## Orchestrator

```bash
./scripts/build-all.sh dev                 # Debug + ctest for all 20 projects
./scripts/build-all.sh prod                # Release + ctest
./scripts/build-all.sh prod --no-tests     # Release only (image build)
./scripts/build-all.sh dev --only argus-camera
./scripts/build-all.sh dev --install-only  # conan install only
```

Per project the script runs `conan install . --output-folder=build/<profile>`,
`cmake --preset <profile>`, `cmake --build --preset <profile> -j 8`, the
owner CLI targets (`argus-migrate-*`, `argus-vulkan-probe`) and `ctest`.

## Working inside one project

```bash
cd services/argus-camera
conan install . --output-folder=build/dev -s build_type=Debug --build=missing
cmake --preset dev
cmake --build --preset dev -j 8
ctest --test-dir build/dev --output-on-failure
```

## Gates

- 0 errors and 0 warnings in Argus code under `-Wall -Wextra`; third-party
  build output and CMake configure chatter are not part of the gate.
- `ctest` green for the affected project; the full orchestrator whenever
  shared build infrastructure changes.
- `./scripts/build-all-test.sh` mocks Conan/CMake/CTest and locks the
  orchestrator flags that CI depends on.
- The image build is the integration gate: build every service image with
  `COMPOSE_PARALLEL_LIMIT=1 docker compose -f argus-deploy/docker-compose.yml
  --profile tunnel --profile identity-init build`.

## Stability protocol

A single green run proves nothing about timing-sensitive suites. After any
change to timing, retry, networking or lifecycle code — and before certifying
a round — repeat the affected binaries: 20 consecutive runs per binary, plus
50 per ordering (`--order-by=name`, `--order-by=name --reverse`, random seeds)
for any case that ever flaked. A skipped test is not a pass; an assertion
count that varies with execution order is a coverage hole, not stability.
Every test binary must clean up its own TempDb files; a `.db*` file left in a
build directory after a run is a failure to investigate, whatever the exit
code says.

## Current scale

20 projects in `dev`. Per-suite test and assertion counts move with the
suites — read them from `ctest -N` inside each project.
