# Build speed plan, 2026-10-19

The owner asked to stop the builds from being the bottleneck: what only needs
compiling once (llama.cpp first) must compile once, and every other repeated
cost must go, without touching the gates (0 errors, 0 warnings, comments,
deps, routes, tidy) or the per-service standalone build the images use.

## 1. What was measured (2026-10-19, `wt/backend-batch2`, 15 build dirs)

| Cost | Evidence | Size |
|---|---|---|
| Parallelism capped at ~5 jobs on 16 cores | `build-all.sh` divides MemAvailable by `MEM_PER_JOB_MB=4096`; measured Debug peaks are 348 MB (`call-time-test.cc`), 524 MB (`llm-service.cc`), 648 MB (`llm-turn-flow-test.cc`), 1,384 MB (`llm/src/app/main.cc`) | ~3x wall time on every compile phase |
| Projects and tests run strictly in sequence | one `cmake --build` per project in a loop; `ctest` without `-j` | the link tail of each project idles the cores; 16 test suites serial |
| Vulkan shader generation per build dir | `.ninja_log`: `llama-build` custom steps 41.8 CPU-min of 318 (13 %); `mul_mm.comp.cpp` alone 44–86 s; glslc output is not ccache-able | repeated for llm and vlm, dev and prod, in every worktree |
| llama.cpp built twice per profile | `services/llm` (MTMD OFF) and `services/vlm` (MTMD ON) each `add_subdirectory(third_party/llama.cpp)` | 335 + 379 objects per profile |
| ncnn (with Vulkan/glslang) and sherpa-onnx built twice | ncnn in camera and identity (5.7 CPU-min in identity dev); sherpa-onnx in identity and stt | per profile, per worktree |
| doctest runner compiled into every test | 415 files define `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`; the runner alone costs 2.1 s (-O0 -g) / 4.1 s (-O3) per TU | 14–28 CPU-min per profile |
| Package tests built and run once per consuming service | `packages/lib/runtime/tests/*` compiled 15 times; 5,619 of 7,530 TU entries are package/third-party/generated (1,892 unique) | package tests 21.9 CPU-min (6.9 %), and their ctest runs 15x |
| Drogon in every TU that includes it | `#include <drogon/drogon.h>` alone: 2.8 s | no PCH anywhere in the tree |
| Links | 38.4 CPU-min (12 %); 375 executables, full `-g` DWARF through mold | Debug only |
| ccache limits | `ccache -s`: 1.2 / 5.0 GiB, 1,313 cleanups, 80.5 % hits; the 15G size lives only in the CMake launcher env, so every other invocation (`ccache -s`, Conan builds) cleans to 5G | evictions between worktrees |
| Images | Dockerfiles cache only `~/.conan2`; `COPY . .` precedes the build, so any change rebuilds llama.cpp/ncnn/sherpa and every package from zero, without ccache | each image, each time |
| Tidy | `check-tidy.sh` re-analyses all ~1,313 TUs on each full run | no result cache |

Note: the `.ninja_log` durations of the 17-10 run are inflated by memory
pressure (`call-time-test.cc` logs 111 s, compiles in 3–9 s alone), which is
itself a sign that the run thrashed while the job count was low.

## 2. Units, in order (cheap and safe first)

Every unit records the before/after wall time of `./scripts/build-all.sh dev`
with a warm and a cold ccache, and leaves all gates green. Any unit that
changes the number of translation units in the compile databases re-measures
`scripts/lib/tidy-baseline.txt` in the same commit (the tidy gate fails on a
lower TU count otherwise).

### B1. Job count from measured memory, and parallel tests
- Compile jobs: `MEM_PER_JOB_MB` 4096 → 1536 (measured peak 1.4 GB, Release
  re-measured before fixing the value), capped at `nproc`. The link pool in
  `argus-module.cmake` stays memory-derived.
- A `heavy` Ninja pool (`JOB_POOL_COMPILE`) for the few TUs above 1 GB, named
  in their own module's CMakeLists, so 16 light compiles never meet 16 heavy ones.
- `ctest -j "$JOBS"`; tests that bind fixed ports or shared files get
  `RESOURCE_LOCK`, found by running each suite at `-j16` three times.

### B2. ccache configuration
- `~/.config/ccache/ccache.conf` written by `setup.sh`: `max_size = 40G`,
  `compression = true`, `sloppiness = pch_defines,time_macros,include_file_mtime,include_file_ctime`,
  `base_dir = $HOME`, `hash_dir = false`. The launcher keeps its env for the
  images, where no config file exists.
- Find what the 1,583 "unsupported compiler option" calls are (ccache debug
  log on one full configure) and fix it if it is ours.

### B3. llama.cpp, ncnn and sherpa-onnx built once per (tag, options, build type)
- One script, `scripts/build-thirdparty.sh`, builds each vendored engine into
  `~/.local/argus-thirdparty/<name>-<tag>-<options-hash>/` (the same home gRPC
  already uses) and installs its CMake config. The hash covers the submodule
  SHA, the option set, the compiler and the build type, so a stale prefix is
  never reused.
- llama.cpp is built ONCE with `LLAMA_BUILD_MTMD=ON`; llm links `llama` only,
  vlm links `llama` and `mtmd`. AGENTS.md §15 and both CONTEXT files change in
  the same commit (each service no longer sets its own `LLAMA_BUILD_MTMD`).
- The engines are always built Release, also for the dev profile: a Debug
  ggml is not debugged by anyone and makes every model-backed test slower.
- Services `find_package(llama CONFIG)` first and fall back to the current
  `add_subdirectory` when the prefix is missing, so a fresh checkout and the
  images still build.
- Same treatment for ncnn (camera, identity) and sherpa-onnx (identity, stt).
- Expected: the 41.8 shader CPU-min and the four duplicate engine builds go to
  zero after the first build on the machine, across every worktree.

### B4. One doctest runner
- `packages/lib/test-support` gains an OBJECT module `argus::lib::doctest-main`
  holding the only `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`; the 415 test files
  drop the define and link it. Files with a custom `main()` keep
  `DOCTEST_CONFIG_IMPLEMENT` and do not link it.

### B5. Package tests registered once
- A package's tests are added only when the package is built as its own
  project or by the aggregator (B8), never when a service pulls the package in
  with `add_subdirectory`: an `ARGUS_PACKAGE_TESTS` switch, ON in the package
  projects, OFF by default inside services.
- `build-all.sh` gains one `packages` project that builds and tests every
  package once (cert and sqlite already are projects; the rest join).

### B6. Debug info and PCH
- Debug: `-gsplit-dwarf` and `-Wl,--gdb-index` (mold supports it), so links
  stop reading every DWARF byte. The images (Release) are unaffected.
- `target_precompile_headers` for the Drogon, gRPC/protobuf and doctest
  headers on one INTERFACE target per service, reused with `REUSE_FROM`, with
  the ccache sloppiness from B2 and `-fpch-preprocess` on GCC. Kept only if
  the measured gain is real on both profiles.

### B7. Images
- Dockerfiles: a `--mount=type=cache,target=/root/.cache/ccache` on every
  build step (`CCACHE_MAXSIZE` from the env), and the engines of B3 built in
  their own stage from `COPY third_party/<engine>` only, so a source change
  never rebuilds llama.cpp, ncnn or sherpa-onnx.

### B8. Tidy cache, then the aggregator
- `check-tidy.sh` through a content-hash cache (ctcache, or our own key of
  preprocessed TU + `.clang-tidy` + clang-tidy version); the TU-count rule
  keeps counting cached TUs as scanned.
- Last and optional: a root aggregator (`cmake/superbuild`) that adds all
  services to one Ninja graph for `build-all.sh` full runs: every package
  compiled once, no per-project barrier. Measured cost of entry: two target
  names collide (`notification-delivery-live-test` in notification and sync,
  `onnxruntime` in identity and stt) and the `PROJECT_IS_TOP_LEVEL` blocks must
  move their `find_package` calls up. `--only` and the images keep the
  standalone projects.

## 3. Acceptance

- `build-all.sh dev` full, cold ccache and warm ccache, before and after,
  recorded in `docs/history/reports/build-speed-2026-10.md`.
- A one-file change in a package and a one-file change in a service:
  rebuild + test time recorded.
- A second worktree building from zero after the first: engines not rebuilt.
- All gates green; the image of llm and vlm still builds and boots.
