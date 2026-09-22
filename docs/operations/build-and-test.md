# Build and test

## Orchestrator

```bash
./scripts/build-all.sh dev                 # Debug + ctest for all 17 projects
./scripts/build-all.sh prod                # Release + ctest
./scripts/build-all.sh prod --no-tests     # Release only (image build)
./scripts/build-all.sh dev --only camera
./scripts/build-all.sh dev --install-only  # conan install only
```

The script resolves the root graph once — `conan install <root>
--output-folder=build/<profile> -s build_type=<Debug|Release> --build=missing`
— then, per project, configures with the toolchain that install produced
(`cmake -S . -B build/<profile> -G Ninja` with `CMAKE_TOOLCHAIN_FILE` and
`CMAKE_PREFIX_PATH` pointing into
`build/<profile>/build/<Debug|Release>/generators`), builds with
`cmake --build build/<profile> -j 8`, builds the owner CLI targets
(`argus-migrate-*`, `argus-vulkan-probe`) and runs `ctest`.

## Working inside one project

The root graph has to exist first; `--install-only` is just that step.

```bash
./scripts/build-all.sh dev --install-only   # once for the whole tree
cd services/camera
cmake -S . -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=../../build/dev/build/Debug/generators/conan_toolchain.cmake \
  -DCMAKE_PREFIX_PATH=../../build/dev/build/Debug/generators \
  -DCMAKE_CXX_STANDARD=20 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/dev -j 8
ctest --test-dir build/dev --output-on-failure
```

Those three flag groups — the toolchain, the dependency prefix and the
language level — are what each project's deleted `CMakePresets.json` used to
carry.

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

17 projects in `dev`. Per-suite test and assertion counts move with the
suites — read them from `ctest -N` inside each project.
