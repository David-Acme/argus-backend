# Build and test

## Orchestrator

```bash
./scripts/build-all.sh dev                 # Debug + ctest for all 19 projects
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

## Current scale

19 projects and 206 CTest tests in `dev`. Gateway doctest assertions
367/78/48/20 and tunnel 106/50/19/28/2/26/24.
