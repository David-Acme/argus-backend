# Phase 2 step 3 — one root `conanfile.txt`, no per-project manifests or presets

Scope: the row is "One root `conanfile.txt`; delete per-package `conanfile.txt` and presets"
(`docs/history/plans/architecture-plan.md:818`), read against §2.6's build model and **D10**
("Packages carry no `conanfile.txt`. One general `conanfile.txt` at the repository root resolves the
dependency graph; each project imports only what it uses"). Base `baeaf60`. The step deletes **36
tracked files**, cleans **29 ignore files**, changes **no source file**, and repairs one thing the
deletion exposed: eight `CMakeLists.txt` carrying stand-in bridge targets that the new single graph
made actively harmful.

## Measured first: the union is the manifest

The risk of one manifest is divergence — a project pinning a version its neighbours do not, or an
option that only one project sets. That was measured before anything was deleted, from the eighteen
tracked manifests at `HEAD`:

| require | declared by | version(s) in the tree |
|---|---|---|
| `cnats` | 18 of 18 | `3.13.0` only |
| `doctest` | 18 | `2.4.12` only |
| `drogon` | 18 | `1.9.13` only |
| `sqlite3` | 18 | `[>=3.45.0 <4]` only |
| `tomlplusplus` | 18 | `3.3.0` only |
| `nlohmann_json` | 16 | `3.11.3` only |
| `jwt-cpp` | 13 | `0.7.2` only |
| `opencv` | 9 | `4.13.0` only |
| `onnxruntime` | 5 | `1.24.4` only |
| `mdns` | 4 | `1.4.3` only |

Ten distinct requires, each spelled identically everywhere it appears — no divergence to resolve.
The options union is the same shape: `sqlite3/*:enable_fts5=True` and `drogon/*:with_sqlite=True`
(all 18), `opencv/*:with_{cuda,eigen,ffmpeg,gtk,protobuf,vulkan,wayland}=False` (the 9), and
`onnxruntime/*:with_cuda=False` (the 5) — ten distinct. The root manifest carries exactly those ten
requires and ten options and nothing else, plus `CMakeDeps`+`CMakeToolchain` and `cmake_layout`;
the union and the manifest are the same set, which is what makes the deletion a merge rather than a
rewrite. `git ls-tree -r HEAD` counted **18 `conanfile.txt`** and **18 `CMakePresets.json`** before
the step, one pair per gate project.

## The build model the row lands

`scripts/build-all.sh` no longer runs Conan inside each project. It resolves the graph **once**:

```
conan install <root> --output-folder=build/<profile> -s build_type=<Debug|Release> --build=missing
```

and then configures each project with the flags the deleted preset used to hold —
`-DCMAKE_TOOLCHAIN_FILE=build/<profile>/build/<Debug|Release>/generators/conan_toolchain.cmake`,
`-DCMAKE_PREFIX_PATH=<same generators dir>`, `-DCMAKE_CXX_STANDARD=20` — builds with
`cmake --build build/<profile> -j 8`, builds the four owner CLI targets, and runs `ctest`.

The install produces **254 files** in that one generators directory and **40** `*[Cc]onfig.cmake`
package entries, i.e. one graph for the whole tree instead of eighteen, and every project's
`find_package` resolves against the same one. One behaviour was normalized in passing:
`CMAKE_EXPORT_COMPILE_COMMANDS=ON` appeared in **18 dev presets but only 2 prod presets** at `HEAD`
(the other sixteen prod presets simply lacked the key), so `compile_commands.json` existed for a
Debug build and vanished for a Release one; the orchestrator now passes it in both profiles, which
is 18/18 either way.

## The presets themselves

A preset carried five things: the generator (`Ninja`), `binaryDir` (`build/dev` / `build/prod`), the
three flag groups above, and — in the build preset — `jobs: 8`. The orchestrator already passed the
generator and `-j 8` explicitly at `HEAD` (`cmake --preset dev`, `cmake --build --preset dev -j 8`),
so what the presets really owned was `binaryDir` and the flags, and the rewritten script spells all
of it. Nothing else in the tree read a preset: `git grep preset -- '.github/**'` is empty, the two CI
steps call the two shell scripts, and no script outside the orchestrator named one.

**The probe that decided the root `.gitignore`.** The first draft of the root ignore file carried a
`/CMakeUserPresets.json` line on the assumption that Conan writes one. It was measured instead, in
`/tmp/argus/preset-probe`: a bare `conan install .` with this manifest writes its generators at
`build/Debug/generators/conan_toolchain.cmake` and produces **no `CMakeUserPresets.json` anywhere**.
The line would have been an ignore entry for a file nothing creates, so it was dropped: the root
`.gitignore` is one line, `/build/`.

One tooling file assumed the preset layout and still holds: `.zed/settings.json` points clangd at
`--compile-commands-dir=build/dev`, the same binary directory the orchestrator still produces per
project, so the deletion does not move it.

## Deleted

- **36 tracked files, `git rm`, staged**: the eighteen `conanfile.txt` and the eighteen
  `CMakePresets.json` (18 + 18, `+0 −1035` lines). The commit's own diff shows 35 deletions plus one
  `R090` pair — git's rename detection matches `services/gateway/conanfile.txt` to the new root
  `conanfile.txt`, which is 90% the same text because that project already declared most of the
  union; `git show --no-renames` gives the 36 deletions and 3 additions the step really staged.
- **29 ignore files, 47 lines**: the eighteen project `.gitignore` files lost two lines each
  (`CMakeUserPresets.json` and `CMakePresets.json.bak`, 36 lines) and the eleven
  `services/*/Dockerfile.dockerignore` lost `**/CMakeUserPresets.json` (11 lines). Nothing else in
  those files moved — 0 matches of either spelling remain anywhere in the live tree.

## The step's one real defect: eight stand-in blocks

Row 2c added `argus_grpc_absl_bridge()` and guarded its call sites with `if(TARGET
argus_client_grpc_bridge_entry)`. Nine `CMakeLists.txt` in the tree name that target; eight of them
carried an older `if(NOT TARGET argus_client_grpc_bridge_entry) add_library(… INTERFACE)` block, a
pre-2c stand-in that **creates the target** so the real bridge is skipped:

```
if(NOT TARGET argus_client_grpc_bridge_entry)
  add_library(argus_client_grpc_bridge_entry INTERFACE)
  add_library(argus_client_grpc_bridge_exit INTERFACE)
endif()
```

That was harmless while each project had its own graph — a tree with one abseil flavour must not
build the bridge at all (the recursion hazard 2c recorded). The single root graph removes the
premise: `onnxruntime`'s protobuf pulls a Conan abseil into **every** tree, so all eighteen are
two-flavour and the real bridge is required everywhere. The stand-ins pre-empt it, and the failure
is a link error in the first project that links a generated client:

```
undefined reference to `grpc_call_run_cq_cb(grpc_call const*, absl::lts_20260107::AnyInvocable<void ()>&&)`
undefined reference to `grpc_call_run_in_event_engine(...)`
```

Diagnosed by measurement, not by reading the guard: cert's build tree held **no `*bridge*.o`**, its
`libargus_clients_identity.a` had four members instead of six, and the failing link line mixed the
Conan abseil archive with the vendored `libgrpc++.so`. The eight blocks (with the comment line
justifying each) were removed — the ninth name, `packages/lib/grpc/CMakeLists.txt:27`, is the
legitimate `argus_grpc_absl_bridge()` call site and is untouched. Two of the eight had a second
defect in the same region: `services/notification/CMakeLists.txt` carried the mis-indented
`argus::contracts::{auth,sync}` blocks, repaired with it.

Measured after, on cert alone: the entry and exit objects are present in the build tree, the client
archive carries both bridge members, the binary shows `T … lts_20260107` beside `U … lts_20260526`
(the two flavours meeting at the bridge), the flavour counts are 20,771 / 41 symbol mentions, and
`./scripts/build-all.sh dev --only cert` is exit 0 with 22/22 tests.

## The documentation sweep

The row deletes a build mechanism that eleven documents taught, so the sweep is part of the step:
**25 files**. By hand — `README.md`, `docs/README.md`, `docs/architecture/build-model.md`,
`docs/architecture/system-overview.md`, `docs/architecture/services-and-packages.md`,
`docs/operations/build-and-test.md`, `docs/operations/configuration.md`,
`packages/contracts/CONTEXT.md`, `packages/identity/CONTEXT.md`, `packages/identity/AGENTS.md`,
`services/tunnel/CONTEXT.md`, `services/voice/AGENTS.md`, the root `AGENTS.md`, and the re-wrapped
paragraphs in `packages/lib/cert/AGENTS.md` and `packages/lib/sqlite/AGENTS.md`; by two subagents
against a fixed recipe and then verified against `git diff` — the nine service `AGENTS.md`
(`camera`, `gateway`, `llm`, `notification`, `productivity`, `stt`, `tts`, `tunnel`, `vlm`), each a
13-line edit (8 deletions, 5 insertions), and `packages/memory/AGENTS.md`. The stale classes were
four: "each with its own Conan graph"/"their Conan graphs" (now one graph), a build recipe that
starts with `conan install .` inside a project (now the root install plus explicit flags), file
lists that still named the two deleted files, and counts — `docs/README.md` said "the 19 standalone
projects" where the orchestrator builds **18**. Two doc claims were checked and left alone because
they are true and were never about presets: `docs/architecture/build-model.md:8` and
`docs/operations/build-and-test.md` describe the new model ("the per-project presets are gone"),
which is the sentence the sweep wrote, not a leftover.

**CI needs no change**, measured rather than assumed: `.github/workflows/ci.yml:33` keys the Conan
cache on `hashFiles('**/conanfile.txt')`, which still matches — it is now a one-file glob, and the
key's value changes with the manifest rather than going undefined; `:45` and `:48` call
`build-all-test.sh` and `build-all.sh dev`, both already rewritten; and no workflow mentions a
preset at all.

**Neither do the eleven images.** None of them drives Conan or CMake itself: every Dockerfile's
build stage is `COPY . .` plus `conan profile detect` plus
`./scripts/build-all.sh prod --no-tests --only <name>`, so the deletion reaches the images through
the script they already call, and the root manifest travels inside the copy. The gateway image is
the only one that builds twice (`--only gateway` then `--only identity`, for the migrate tool), and
the two calls now share one graph — the identical `conan install` is a cache hit inside the same
`/root/.conan2` buildkit cache mount. This is static, not a container run: nothing in the tree or in
CI builds an image.

## Verified

- `./scripts/build-all.sh dev` — **exit 0**, **18/18** projects, **18** `100% tests passed, 0 tests
  failed` summaries, no `Not Run`, no `***Failed` and no `warning:` line in the run.
- The reached-test ledger is **428** — unchanged from 2d and 2c, which is the expected result for a
  step that compiles nothing new: the ten client suites still register in the same trees, and the
  bridge repair changes which library implements two symbols, not how many tests exist. Per
  project: camera 53, guard 47, gateway 41, notification 39, productivity 34, llm 32, sync 29,
  voice 25, memory 22, identity 22, cert 22, tts 18, socket 13, tunnel 12, vlm 7, stt 6, intent 4,
  sqlite 2.
- `./scripts/build-all-test.sh` passes, locking one `conan install` per run, the exact root command
  line, the exact configure line, and failing if `--preset` appears anywhere.
- The deleted set is exactly 36 files (`18 conanfile.txt + 18 CMakePresets.json`), the ignore sweep
  exactly 47 lines over 29 files, and the only mentions of a preset left in the live tree are the
  sentence in `build-and-test.md` that records the deletion.
