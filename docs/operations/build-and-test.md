# Build and test

## Orchestrator

```bash
./scripts/build-all.sh dev                 # Debug + ctest for all 15 projects
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
`cmake --build build/<profile> -j <jobs>`, builds the owner CLI targets
(`argus-migrate-*`, `argus-vulkan-probe`) and runs `ctest`.

`<jobs>` is the first of: `--jobs N` on the command line, the
`CMAKE_BUILD_PARALLEL_LEVEL` environment variable, the memory the host has
free at start:

```
jobs = clamp(min(MemAvailable - 4 GiB, cgroup headroom) / 4 GiB, 1, nproc)
```

The 4 GiB reserve is the floor the build gate enforces before it admits a
capped job, and it applies to the host's figure. The 4 GiB per job is the
worst job measured in this tree: the `dev` `argus-llm` link at 3349 MiB (the
heaviest compile measured is `ggml-vulkan.cpp` at 1799 MiB). The budget is
set by a *link* on purpose — a build's tail runs nothing but links, so a
budget below the heaviest link would overrun the cgroup on the first tail.

The second term is the headroom of the process's own cgroup v2 directory
(`memory.max - memory.current`, read from `/proc/self/cgroup`), which is what
honours a capped `systemd-run` scope or a `docker run --memory`; `/proc/meminfo`
alone would not, because inside a container it reports the host's memory, not
the container's limit. The same count is handed to `conan install` as
`-c tools.build:jobs=<jobs>`, so a cold dependency cache cannot compile at
`nproc` either. The script prints what it chose before it builds:

```
[setup] build jobs: 3 (memory-derived: MemAvailable 20021 MiB - reserve 4096 MiB, cgroup headroom 13307 MiB, over 4096 MiB per job, 16 cpus)
```

That line is from a `heavy-gate.sh 13` run: the host had 20021 MiB free and the
scope allowed 13307 MiB, so the scope set the count.

## Link pool

`<jobs>` bounds the whole build, but a build's tail runs nothing but links, and
a link is much larger than the average compile. The shared helper
`cmake/argus-module.cmake`, which every project includes before its first
target, sets `CMAKE_JOB_POOLS "link=N"` and `CMAKE_JOB_POOL_LINK`, so ninja runs
at most `N` link steps at once while compilation keeps the full `-j <jobs>`.

```
N = max(1, floor(cap_mb / budget_mb))
```

The budget follows the linker the helper picked: 3072 MiB where mold is in use
and 4096 MiB for the default linker. It is the per-link anonymous peak of the
tree's heaviest binary, `argus-identity`, rounded up — 2764 MiB under mold and
3455 MiB under `ld` (the orchestrator's `MEM_PER_JOB_MB` stays 4096, a compile
being a different job). `cap_mb` is read in order from the
`ARGUS_BUILD_MEMORY_CAP_MB` variable or
environment, the process's own cgroup v2 `memory.max` (what a capped
`systemd-run` scope or a `docker run --memory` sets), then, with no cap at all,
`MemAvailable` minus the same 4 GiB reserve the orchestrator holds back.
`MemTotal` is never used: on a 30 GiB host it would allow ten links, and ten GNU
`ld` beside eight compile workers is the shape that froze the machine on
2026-10-03. A 18432 MiB cap gives `N = 6` with mold and 4 without, a 16384 MiB
cap 5 and 4, a 14336 MiB cap 4 and 3.
`ARGUS_LINK_POOLS` overrides
`N`, and an already-set `CMAKE_JOB_POOLS` is left alone. The configure prints
what it chose and from where:

```
-- argus linker: mold (/usr/bin/mold), link budget 3072 MiB
-- argus link pool: link=6 (memory cap 18432 MiB)
-- argus link pool: link=5 (MemAvailable 20746 MiB - reserve 4096 MiB)
```

The generated `build.ninja` then binds `pool = link` on every link edge and on
no compile edge, with the depth in `CMakeFiles/rules.ninja`. A pool exists only
for the Ninja generator, so the helper sets it only when `CMAKE_GENERATOR`
matches `Ninja`. `scripts/build-pool-test.sh` configures a probe through the
helper and fails unless the generated file carries the pool on its link edges.

## Working inside one project

The root graph has to exist first; `--install-only` is just that step.
`<jobs>` below is the count the orchestrator printed, or what `--jobs N`
asked for; it is never a fixed number.

```bash
./scripts/build-all.sh dev --install-only   # once for the whole tree
cd services/camera
cmake -S . -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=../../build/dev/build/Debug/generators/conan_toolchain.cmake \
  -DCMAKE_PREFIX_PATH=../../build/dev/build/Debug/generators \
  -DCMAKE_CXX_STANDARD=20 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/dev -j <jobs>
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
- Evaluation tests (ctest label `eval`: `fast-tier-eval`, `llm-tier-eval`,
  `llm-tier-smoke`, `eval-sealed-hash`, `decider-eval-test`, `slot-eval-test`,
  `conversation-eval-test`, `stt-wer-eval`) measure what Argus understands
  against versioned gates and exit 77, reported as skipped, when the model, the
  command or the clips they need are absent; the LLM tier also skips in a debug
  build, which decodes twenty times slower
  (`docs/operations/voice-quality-eval.md`). The decider, slot and conversation
  harnesses (`services/llm/tests/eval/*.py`) score any process that speaks
  their line protocol; the sealed set is pinned by sha256 and is never opened by
  whoever tunes a decider.
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

A skip is counted, never silent. ctest reports as Skipped a doctest binary whose
summary says `0 failed` and at least one `skipped` (every `doctest::skip` on a
missing broker, model or environment variable: `argus_service` sets
`SKIP_REGULAR_EXPRESSION` once per project, and the expression demands `0
failed` because one that also matched a failure would hide it), a test that
exits 77 (`SKIP_RETURN_CODE`: the evaluations and `golden-sync-test`, which
fails instead once a native stack is announced), and the script tests, which
print a `SKIPPED:` line and exit 77 when a tool they need (`script(1)`,
clang-tidy) is absent. A test that needs a file the repository carries
(`models/intent/intent.bin`) fails without it. The final sweep counts each
project's Skipped tests and the `SKIPPED:` lines of the scripts.

## Current scale

15 projects in `dev`. Per-suite test and assertion counts move with the
suites — read them from `ctest -N` inside each project.
