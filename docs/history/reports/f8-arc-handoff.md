# F8 handoff — state of the arc and what remains

> Written 2026-09-10 as a handoff for an AI session taking over the F8 arc on
> the Argus backend. Working directory: `/home/acme/Desktop/argus/backend`
> (git, branch `master`). The approved plan lives at
> `/home/acme/.claude/plans/humming-percolating-horizon.md` (Spanish; written
> as user communication — everything committed to the repo is English).

## 1. Project context

Argus is a C++20 backend (Linux, CMake + Ninja + Conan 2) reorganized into
`packages/` (libraries compiled into service processes) and `services/`
(processes) plus `third_party/` (git submodules: llama.cpp, ncnn,
sherpa-onnx, sqlite-vec, fastText, kaldi-native-fbank …), `scripts/`,
`argus-deploy/` (Dockerfile + docker-compose), `cmake/argus-module.cmake`,
`docs/`. Quality standards are in `AGENTS.md`; the long decision log is
`docs/CONTEXT.md` (rulings A–CL). Standing rules that override defaults:

- **Rule 20**: comments minimal, small "what it does" only; the "why" goes to
  CONTEXT.md. Applies to all code, tests, CMakeLists, conanfiles, presets.
- **All-English** code, comments, commits, docs (the only Spanish artifacts
  are user-facing plan/handoff communications like this file).
- Test naming hyphenated (`user-service-test.cc`).
- Rule 24 (no dead code), rule 25 (declare once in own folder), rule 2 (param
  structs for 3+ param functions), rule 19 (no raw C arrays).
- Never `git add -A` / `git add .` — explicit paths only.
- Every work step ends in a commit with trailer
  `Co-Authored-By: Claude Code <noreply@anthropic.com>`.
- The user's standing directive: run the whole F8 plan
  (humming-percolating-horizon.md) and **continue until finished**
  ("continua hasta acabar").

## 2. F8 plan shape (twelve-ish commits, A → C4)

1. **A — reorganize** (DONE, earlier sessions): repair preexisting defects
   (Dockerfile paths, `labs/intent-probe` target, CI trigger), delete `labs/`
   and dead code, `git mv` everything into `packages/`/`services/`, move docs
   to `docs/`. Gates: the full test network stayed green through the moves.
2. **B — the brain** (DONE, earlier sessions): B1 measured tool-calling
   accuracy of the active LFM2.5-1.2B-Instruct-QAD model (gate passed); B2
   moved `tool-registry`/`tool-validator`/`tool-executor` into
   `packages/argus-llm-client`; B3 turned `argus-memory` from a service into a
   package (commit "f8-b3"); B4 wired the memory tool-calling loop inside
   `LlmController::chatStream` in argus-llm (Ruling BT wire freeze and Ruling
   CA voice-session isolation respected).
3. **C — build independence** (the current arc):
   - **C1** (commit `92d5102`): test-gate inversion. `if(ARGUS_ROOT_PROJECT)`
     test registration became unconditional in every project; the two
     `CMAKE_SOURCE_DIR STREQUAL` guards in gateway/tunnel were removed. Tests
     went from 40 to 54 in the root build (tunnel's 7 and gateway's 4 suites
     joined). Root regression 54/54 verified before and after.
   - **C2** (commits `10f85de` … `e4b2355`, `b78283b`, plus `f2aec2e`
     cleanup): every standalone project got `conanfile.txt` +
     `CMakePresets.json` (dev AND prod) and builds/tests from its own folder.
   - **C3** (commits `a23c772`, `0a02b54`, `7718562`): setup.sh split per
     owner; per-project local configs.
   - **C4** (in progress): `319e700` added the orchestrator; the cut of the
     root build trio is the NEXT step (see §5).

## 3. C2 — what was done and the hard-won lessons (per commit)

Pilot: `10f85de` argus-common (conanfile + dev/prod CMakePresets.json template
— this file is the canonical preset template every project copied).
Then, in order: contracts `53bdaed`, identity `65d5ba3`, cert `64c7381`,
socket `1206eea`, sqlite `13e15b8`, sync `d187c3b`, tts `522f446`,
stt `814c13d`, vlm `a541495`, memory `f7dfc02`, llm `9938b09`,
voice `e2dec98`, tunnel `9df7967`, camera `dd5b8b1`, productivity `8d497b0`,
notification `13a26d4`, gateway `e4b2355`, intent `b78283b`.
Cleanup: `850b423` declared the vendored sqlite-vec target in its own folder
(`third_party/sqlite-vec/CMakeLists.txt`, compiled with
`SQLITE_CORE SQLITE_VEC_STATIC`); `f2aec2e` deleted the dead
`ARGUS_SQLITE3_TARGET` variable in argus-memory.

**Final C2 state (verified)**: 19 standalone projects (9 packages: common,
contracts, cert, socket, sqlite, identity, sync, memory, intent; 10 services:
gateway, camera, productivity, notification, tts, stt, vlm, llm, voice,
tunnel). Each has `conanfile.txt` + `CMakePresets.json` (dev Debug + prod
Release, mirroring `packages/argus-common/CMakePresets.json`: Ninja,
`build/<dev|prod>`, toolchain at
`build/<p>/build/<Type>/generators/conan_toolchain.cmake`, jobs 8). Each
configures, builds and passes ctest standalone in both presets. Per-project
standalone ctest counts (dev): common 6, contracts 1, cert 11, socket 7,
sqlite 7, identity 11, sync 13, memory 11, intent 8, gateway 18, camera 20,
productivity 16, notification 16, tts 7, stt 7, vlm 8, llm 15, voice 11,
tunnel 13. Root regression stayed 54/54 throughout.

**Lessons (apply these whenever touching standalone CMake):**

- `cmake_minimum_required(VERSION 3.25)` + `project(argus-X VERSION 1.0.0
  LANGUAGES CXX)` + the 3 `CMAKE_CXX_STANDARD` set() lines are required in
  every standalone project header (CMP0000 otherwise).
- The top-level `include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/
  argus-module.cmake)` is MANDATORY in every project — even those not using
  `argus_module()` — because `include_guard(GLOBAL)` makes sibling includes
  no-ops and `ARGUS_CMAKE_DIR` must be seeded in the top scope. Place it at
  the top of the file.
- Ordering inside a service CMakeLists: (a) unguarded find_package lines go
  ABOVE the sibling add_subdirectory block; (b) the llama.cpp/ncnn/sqlite-vec
  vendored blocks go BEFORE the sibling package add_subdirectory calls — the
  packages read `ARGUS_LLAMA_TARGETS`, `ARGUS_NCNN_TARGET`,
  `ARGUS_SQLITE_VEC_TARGET`, `ARGUS_SHERPA_ONNX_TARGET` from the parent scope
  at configure time; (c) llama.cpp cache vars must be set before memory's own
  llama block would win the cache.
- A package's own `if(NOT TARGET Drogon::Drogon)` closure blocks are inert
  under a parent that already found Drogon — the PARENT must add the full
  transitive closure explicitly (llm had to add argus-phrase, sqlite-vec,
  argus-sqlite itself; voice added argus-audio and the three client packages).
- **cq-bridge gotcha**: in a standalone build, packages/argus-contracts'
  substrate finds the vendored gRPC at `~/.local/argus-thirdparty/grpc` and
  builds the real cq bridge, whose entry symbol interposes libgrpc's own
  callbacks → infinite mutual recursion (`bridgeCq` ↔
  `grpc_call_run_cq_cb`, segfault). Every service that adds argus-contracts
  as a subdirectory creates empty INTERFACE stand-ins
  `argus_sdk_grpc_bridge_entry`/`argus_sdk_grpc_bridge_exit`
  (`if(NOT TARGET ...)`) BEFORE contracts runs. contracts itself only does
  this under `PROJECT_IS_TOP_LEVEL`; identity/sync carry their own guarded
  stand-ins, but a parent adding contracts first makes the real bridge win.
- `${CMAKE_SOURCE_DIR}` occurrences (valid only in the root tree) were all
  converted to `CMAKE_CURRENT_SOURCE_DIR`-relative paths: from
  `packages/argus-X` the backend root is `../../`; from `services/argus-X`
  also `../../`; from `tools/<tool>` under a project it is `../../../..`
  (four levels — an earlier brief wrongly said three; camera's agent caught
  it). All 9 occurrences from the plan are fixed; `grep -rn
  "CMAKE_SOURCE_DIR"` in non-build files is clean.
- Root-discovered tools (`services/argus-*/tools/*`,
  `packages/argus-*/tools/*`) are globbed by the root CMakeLists. Standalone,
  each service must `add_subdirectory(tools/migrate-<name>)` itself (guarded
  `PROJECT_IS_TOP_LEVEL AND NOT TARGET <name>-migration`), plus the
  identity-migration ride-along for services that pull argus-identity's test
  suites, plus `tools/vulkan-probe` for camera (`PROJECT_IS_TOP_LEVEL AND
  TARGET ncnn`).
- The trio camera/productivity/notification diverged from root's third_party
  cache vars (no `NCNN_VULKAN ON`); all now replicate root's flags, and the
  inline sqlite-vec static-lib copies were replaced with the vendored folder
  (rule 25).
- Two preexisting latent defects found and fixed during C2:
  `camera-sync-rpc-service.cc` had a non-compiling `co_await fill({...})`
  (never compiled in the root build; fixed with `FillInput{...}` CTAD), and
  the Ruling CL violation — `argus-tunnel` linking `argus_common` dragged
  `SQLite::SQLite3` — fixed by making argus-common's sqlite link PRIVATE
  (schema-runner.hxx only forward-declares `struct sqlite3`; verified no
  consumer relied on the transitive include; commit `9df7967`).

## 4. C3 — what was done

- `a23c772`: `scripts/lib/common.sh` — the shared helpers (`log`, `warn`,
  `err`, `need_cmd`, `sha256_file`, `sudo_if_needed`, `toml_value`,
  `ensure_toml_value`) moved verbatim out of setup.sh; setup.sh sources it.
  A `download_to` helper was deliberately NOT factored (the curl flag sets
  differ per model function).
- `0a02b54`: per-owner provisioning. `scripts/provision.sh` created in
  services/argus-tts, argus-stt, argus-llm, argus-vlm, argus-voice,
  argus-camera (camera+go2rtc), and packages/argus-memory (memory+extract),
  packages/argus-identity (face). Function bodies moved byte-identical; ROOT
  resolves 3 levels up via BASH_SOURCE (models/ stays a shared root folder).
  Root `setup.sh` `main()` delegates in order tts, llm, vlm, stt, face, vad,
  memory(+extract), camera(+go2rtc); `--camera` (CAMERA_ONLY) calls only the
  camera provision script. Also fixed a preexisting bug: `SKIP_BUILD` was
  hardcoded to 0 (the documented `SKIP_BUILD=1` never skipped anything).
- `7718562`: per-project local configs. `ensure_project_config <dir>` in
  scripts/lib copies `<dir>/config.toml.example` → `<dir>/config.toml`
  (chmod 600) and injects only the secret keys each template actually has
  (jwt secret / refresh_secret / fingerprint_secret hex-48, identity
  rpc_secret hex-32). Root `config.toml.example` DELETED (git rm);
  `migrate_legacy_overlay` deleted (dead: its target was the root config).
  ci.yml:39 now copies all per-project examples; Dockerfile build-stage cp
  removed and line 70 now `COPY services/argus-gateway/config.toml.example
  /opt/argus/config.toml`. Real `docker build` gate passed (full recompile,
  523 s build stage). Per-project `config.toml` files are gitignored by the
  bare `config.toml` pattern. docker-compose uses its own
  `argus-deploy/config.<svc>.toml` mounts and is unaffected.

## 5. C4 — state and the exact next step

- DONE (`319e700`): `scripts/build-all.sh` — the orchestrator. Loops the 19
  standalone projects (list hardcoded in PROJECTS), per project:
  `conan install . --output-folder=build/<p> -s build_type=<Type>
  --build=missing`, `cmake --preset <p>`, `cmake --build --preset <p> -j 8`,
  `ctest --output-on-failure` in `build/<p>`. Flags: `dev|prod`,
  `--no-tests`, `--only <name>`. Proven: dev loop 19/19 projects, 206 tests
  all passed, 434 conan cache hits / zero package rebuilds; prod loop 19/19
  projects all "100% tests passed" (re-verified after an interrupted run).
- **NEXT (not started, one implementer step)**: the cut. Delete the root
  `CMakeLists.txt`, `CMakePresets.json`, `conanfile.txt` (`git rm`) and
  rewire:
  - `scripts/setup.sh` `build_project()` → call
    `"$ROOT/scripts/build-all.sh" "$PROFILE"`; `SKIP_BUILD` needs a new
    `--install-only` flag in build-all.sh (stops after conan install).
    Update setup.sh's header comment block and the final log line.
  - `.github/workflows/ci.yml`: replace the root conan install + cmake
    preset + build + ctest steps with `./scripts/build-all.sh dev`; cache
    key `hashFiles('conanfile.txt')` → `hashFiles('**/conanfile.txt')`;
    keep the "Generate runtime configs" step.
  - `argus-deploy/Dockerfile`: build stage lines ~31-38 (root presets,
    `-DARGUS_BUILD_LABS=OFF -DARGUS_SYSTEM_PROTOBUF=ON`, the explicit
    17-target list) → `./scripts/build-all.sh prod --no-tests`. The 17
    `COPY --from` runtime paths already match the standalone build-tree
    layout (verified in C2/C3; spot-check 3 anyway).
  - Docs: update factual descriptions of the root build at AGENTS.md:615-619,
    docs/backend.md:269-273, docs/CONTEXT.md:30,66 to the per-project
    reality; leave historical/dated entries alone.
  - Gates: `bash -n`; `SKIP_BUILD=1 ./scripts/setup.sh prod`; full
    `flock /tmp/f7-review.lock bash -c './scripts/build-all.sh dev'`;
    `./scripts/build-all.sh prod --no-tests`; real
    `docker build -f argus-deploy/Dockerfile .` under the flock; EOF newline
    on every touched file (`tail -c1 | od -An -tx1` = `0a`);
    `git diff --check`; explicit-path commit "f8-c4: cut the root build
    trio" with the trailer.
  - The root `build/` directory becomes obsolete but is untracked — leave it
    (do not delete).
- Note: `-DARGUS_SYSTEM_PROTOBUF=ON` in the Dockerfile is a root-build cache
  var (Debian pkg-config protobuf stack in cmake/argus-module.cmake). The
  root trio deletion makes it moot; standalone projects resolve gRPC via the
  vendored fallback at `~/.local/argus-thirdparty/grpc` (see §6).

## 6. Build/test environment facts (for reproducing gates)

- Host: Arch Linux LTS, GCC 16 (conan profile `default`, gnu20,
  libstdc++11). Docker available; CI is GitHub Actions ubuntu-24.04.
- Commands (from `/home/acme/Desktop/argus/backend`): standalone per project
  `conan install . --output-folder=build/dev -s build_type=Debug
  --build=missing` then `cmake --preset dev`, `cmake --build --preset dev
  -j 8`, `ctest` in `build/dev` (same for prod/Release); everything at once
  `scripts/build-all.sh [dev|prod]`. Until C4-2 lands, the root build still
  exists: `flock /tmp/f7-review.lock bash -c 'cmake --build --preset dev -j 8'`
  then `cd build/dev && ctest` = 54/54 (prod also 54).
- Warning gate: 0 C++ warnings/errors outside third_party; third_party cmake
  configure chatter (ncnn, kaldi-native-fbank, openfst, ggml,
  vulkan-shaders-gen, conan target messages like `absl::strerror`) is
  expected noise, not a failure.
- conan substrate: root options `drogon/*:with_sqlite=True`,
  `sqlite3/*:enable_fts5=True`; every project's conanfile carries `sqlite3`
  as a DIRECT require (a transitive-only entry generates empty include dirs).
  Protobuf/gRPC: `cmake/argus-module.cmake` prefers the Debian pkg-config
  stack (ARGUS_SYSTEM_PROTOBUF, used in Docker), else falls back to
  `~/.local/argus-thirdparty/grpc` (Arch host) — that fallback triggers the
  cq-bridge story in §3.
- Models live in the shared root `models/` (13 GB, gitignored); each
  provision.sh owns its subfolder. `packages/argus-intent` gates its model
  with a sha256 pin at configure time.

## 7. Remaining after C4-2 (final F8 acceptance checks)

Per the plan's verification section:
- Real `docker build` (done in C3-3's gate; redo after C4-2's Dockerfile
  change — part of C4-2's gates).
- Suite-count sanity: gateway 367/78/48/20 doctest cases, tunnel
  106/2/50/19/28/26/24 (F7-era reference numbers; re-verify after the cut).
- md5 of the 3 databases unchanged by the arc; `argus.db` absent;
  `git diff --check`; EOF `0a`; `setAlarm` 0 occurrences in services;
  100% English in code/commits/docs.
- Open user decisions (do not act without asking): the
  RemoteVisionServiceAdapter question (Ruling BQ) and the GLiNER evaluation.
- Known cosmetic leftover: the obsolete untracked root `build/` directory.

## 8. Working style expected by the user

- Sequential implementer dispatches (shared tree + conan cache + git index —
  no parallel mutators), one step per agent, each step ends in a commit.
- Agent briefs quote the AGENTS.md rule text verbatim, restate rules fresh,
  reference prior commits by hash, and demand explicit-path staging.
- Gates after every step: dev+prod 0 errors 0 warnings (outside third_party),
  full ctest, `git diff --check`, EOF `0a` (git diff --check misses EOF
  newlines; gate on `tail -c1`).
- Ask, never assume, on architecture/domain-ownership decisions; ground the
  question in code first.
- Ultracode is ON in the current session (workflow orchestration preferred
  for substantive tasks where parallelism helps; the build/test pipeline
  itself is sequential by nature).