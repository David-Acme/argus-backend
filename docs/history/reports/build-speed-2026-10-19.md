# Build speed, 2026-10-19

Unit: `perf/build-speed`, on `17145f2b` (`integ/backend-batch-2` before its
pause WIP). Plan: `docs/history/plans/build-speed-plan.md`. Machine: Ryzen 7
5825U, 16 threads, 30 GB, the laptop dedicated to the build (owner,
2026-10-19).

## Measured

| Scenario | Before | After |
|---|---|---|
| Job count derived by `build-all.sh` | 5 (4096 MiB per job, 4096 MiB reserve) | 16 (1536 MiB per job, 1024 MiB reserve) |
| Cold llm + vlm + camera + identity, empty ccache, no vendored prefix | 1238 s (447 + 113 + 334 + 344, one at a time) | 826 s (first vendored builds included, before Release dependencies and 16 jobs) |
| All 15 projects in a NEW worktree, warm caches | not measured whole; every tree regenerated the Vulkan shaders (about 425 s of glslc edges in llm, again in vlm) | 259 s, ccache 95.3 % hits, llama.cpp and ncnn reused |
| No-op `build-all.sh dev --no-tests`, all 15 projects | | 36 s |
| clang-tidy gate, whole tree | 674 s (no cache) | 58 s (cached), identical counts |
| Full `build-all.sh dev` after an edit: build + 432 tests of 15 projects + tidy | | 520 s, every suite green, 0 warnings |
| Full `build-all.sh prod --no-tests`, first Release vendored builds included | | 1051 s, 0 first-party warnings |
| `argus-llm` dev binary | 1015 MB | 93 MB |
| `llm-settings-test` dev binary | 87 MB | 35 MB |
| Package tests | built and run once per consuming service (`lib/runtime`'s 15 times) | 132 tests, once, 7–8 s at `-j16` |

The cold "after" figure predates two later changes (Release Conan dependencies
for `dev`, 16 jobs) and is therefore an upper bound.

## Findings on the way

- A Release `libllama.so` linked into Debug services made mold extract archive
  members that weakly define `std::string` members (turn engines → GLiNER → all
  of onnxruntime): test binaries grew from 87 MB to 1 GB. The vendored build now
  follows the consumer's build type.
- Debug Conan dependencies put about 1 GB of DWARF into every test executable
  (identity tests 1.4 GB before this unit). `dev` now links the Release
  dependency packages `prod` already has in the cache (`-s build_type=Release
  -s &:build_type=Debug`), so nothing new is compiled.
- Removing the per-test doctest runner exposed three tests that compiled only
  through doctest's own includes (`<vector>`, `<algorithm>`, `<unistd.h>` ×3).
- `ctest -j` exposed two races: `guard-safety-test` searched the random 80-hex
  hash for the substring `4821` (≈0.1 % failure; it now checks no `$` field is
  the PIN), and the voice tests shared one `voice-test-config.toml` in the build
  directory (now one file per process).
- Without doctest's implementation in the same TU, GCC 16 at `-O3` inlined
  `tone()` into `pause-limiter-test` and raised a false `-Warray-bounds` on
  `vector::insert` of a whole vector; the test appends through a small
  `append(std::span)` helper instead (0 warnings, both profiles).
- llm and vlm first produced two llama keys because only one of them enables C
  and passed `CMAKE_C_FLAGS_DEBUG`; both now pass the configuration's C++ flags
  for C too, and every configure drops the cached `llama_LIBRARY`/`GGML_*`
  paths so a new key never links a deleted prefix.
- PCH rejected: 1555 targets, median one TU per target; the 75 targets with ≥ 8
  TUs would each carry a GCC PCH of hundreds of MB (≈ 15 GB per profile per
  worktree) and a force-included header hides missing includes, the defect class
  this unit just found three of.
- No test needs serialising: the heaviest model test peaks at 1.2 GB
  (`tts-pocket-test`), all model tests together near 10 GB.

## Tidy gate

The gate fails on this commit **and** on the untouched `17145f2b`, with the
same 2298 findings over the same risen checks (largest:
`modernize-use-designated-initializers` 221 vs 158,
`bugprone-unchecked-optional-access` 73 vs 52). This is debt from the batch
commits after its last ratchet (`29e1ea37`), left to the consolidated test
phase; this unit adds one TU (`doctest-main.cc`) and no finding. Removing the
doctest define from a test leaves its findings unchanged (checked on
`identity-grpc-client-test.cc`).

## Not done here

macOS C++ portability (sockets, epoll, CPU affinity, Darwin probes) and the
MLX engine are units M1–M8 of `docs/history/plans/mac-mlx-plan.md`.
