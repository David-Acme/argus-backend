# Phase 1 step 8 — `packages/clients/health` dies, the stubs move to the gRPC package

Scope: the row is "Delete `packages/clients/health` (zero sources; standard health stubs →
`lib/grpc`)". What it deletes is a package holding **one file**, and what it moves is a generated
module over a vendored proto. The step's real content is the second half: a package with no sources
cannot simply vanish, so the stubs have to be declared *somewhere*, and where decides what they are
called. Base `bc6dccf`.

## What the package was

`find packages/clients/health -mindepth 1` returns exactly one path: `CMakeLists.txt`. No `src/`,
no `tests/`, no headers — the zero-source package §9.3 describes (`docs/history/plans/architecture-plan.md:1000`).
Its fifteen lines did three things:

```cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus_client_grpc_base)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../grpc
                     ${CMAKE_CURRENT_BINARY_DIR}/grpc)
endif()

if(NOT TARGET argus::client-health)
  argus_client_module(NAME health
      PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/proto
      PROTO grpc/health/v1/health.proto)
endif()
```

The proto is upstream's, vendored verbatim at `packages/contracts/proto/grpc/health/v1/health.proto`
(the header comment says so; Apache-2.0). The generated module had exactly two consumers, and both
are servers: `services/voice/src/feature/health/health-rpc-service.hxx` and
`services/camera/src/feature/health/health-rpc-service.hxx` implement
`grpc::health::v1::Health::CallbackService` — and that generated header is the package's only use
anywhere in the tree. Nothing calls the stubs, and nothing else names the package: a repo-wide grep
for `clients/health`, `client-health` and `client_health` across every file type finds the two
`CMakeLists` blocks, two link lines, the package's own file and the plan's two mentions in
`docs/history/`. No script, no Dockerfile, no `AGENTS.md` rule, no doc outside the history.

## The decision: where the stubs go, and what they are called

The row gives the destination — `lib/grpc` — and the plan had already measured it: §2.2 lists the
`lib/grpc` contents as "client-base, cq-bridge, server-identity **+ the standard grpc.health.v1
stubs**" and §9.1's row is `| grpc | 6 | lib/grpc | + standard health stubs |`, whose `6` is exactly
the count of files under `packages/grpc/src` today. So the destination is not a choice; what the row
does not say is **what the resulting target is called**.

Two options:

- **A — keep the name.** `packages/grpc` declares the module and the target stays
  `argus_client_health` / `argus::client-health`.
- **B — rename it.** `packages/grpc` declares it as a gRPC-runtime module:
  `argus_grpc_health` / `argus::grpc-health`.

§2.5's rule is "the namespace mirrors the folder", and under A the name would lie twice over: the
target would be declared by `packages/grpc` (not by a `clients/` package) and it would carry the
`client` word for something no client calls. B is what the row's own sentence implies — the stubs
are `lib/grpc`'s content, and their name should say so.

B needs the helper to be able to name a module something other than the client convention, so
`argus_client_module` grew an optional `GROUP`:

```cmake
# argus_client_module(NAME <name> [GROUP <group>] [PROTO_ROOT <dir>] PROTO <path>...
#                  [SOURCES ...] [INCLUDES ...] [DEPENDS ...])
#                  -> argus_<group>_<name> / argus::<group>-<name>
# GROUP defaults to client (the SDK convention); packages/grpc passes GROUP grpc
# for the standard health stubs, which are served rather than called.
```

`GROUP` **defaults to `client`**, and that default is what makes the change safe rather than merely
convenient: all ten existing callers pass no `GROUP`, so every one of them still declares
`argus_client_<domain>` / `argus::client-<domain>`, and the helper's blast radius is nil. Nothing
else in the function moves either — the codegen recipe, the ABI-bridge block
(`argus_client_grpc_bridge_entry`/`_exit` when the vendored stack is in play) and the client base
link are the same lines they were, with `${module}` and `${module_alias}` standing where the
hard-coded `argus_client_${ARG_NAME}` strings stood. The only other consequence is the build-tree
directory name, which now follows the group: `${CMAKE_CURRENT_BINARY_DIR}/argus-grpc-health/generated`.

So `packages/grpc/CMakeLists.txt` ends with:

```cmake
if(NOT TARGET argus::grpc-health)
  argus_client_module(NAME health
      GROUP grpc
      PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../contracts/proto
      PROTO grpc/health/v1/health.proto)
  set_target_properties(argus_grpc_health PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
```

and `packages/clients/health/` — file and directory — is gone, which is the row's actual verb.
`packages/clients/` is now the 10 directories §9.1 records ("10 live dirs").

`EXCLUDE_FROM_ALL` is on the target rather than on the caller's `add_subdirectory` because the
package is now reached from eight places — the nine `CMakeLists.txt` that add `packages/grpc`, minus
the deleted package itself: `contracts/response-contract`, `contracts/tts-contract`, and the client
packages `notification`, `productivity`, `voice`, `identity`, `camera` and `camera-actions` — and
only two of them serve health. The stubs are therefore declared everywhere the runtime is, and
generated nowhere they are not used.

## Where the module actually lands

`packages/grpc` is a guarded subtree, added by whichever consumer needs it first, so "the gRPC
package" is a different build directory in every project. The generated stubs follow the first
instantiation: building `services/voice`, the health headers land under
`build/dev/tts-client/grpc/argus-grpc-health/generated/grpc/health/v1/` (the `tts-client` guard at
`services/voice/CMakeLists.txt:43` runs before `clients/voice`'s at `:61`); building
`services/camera` they land under `build/dev/clients/identity/grpc/`, because `clients/identity` is
added before `clients/camera`. In both cases the module's object files sit in the same directory's
`CMakeFiles/argus_grpc_health.dir`.

That changes no source line. `argus_client_module` adds `${gen_root}` to the module's
`SYSTEM PUBLIC` include directories and the file's path inside it comes from the proto's own path
(`grpc/health/v1/health.proto` → `grpc/health/v1/health.grpc.pb.h`), so
`#include <grpc/health/v1/health.grpc.pb.h>` resolves exactly as before, wherever the package was
added. **0 `.cc`/`.hxx` changed** — the second step in a row where the whole diff is
`CMakeLists.txt` files.

## The consumer repointing

Two files, four lines:

| consumer | guard | link |
|---|---|---|
| `services/voice` | `:65-70` — `if(NOT TARGET argus::grpc-health)` + `add_subdirectory(…/packages/grpc ${CMAKE_BINARY_DIR}/grpc EXCLUDE_FROM_ALL)` | `:99` `argus::client-health` → `argus::grpc-health` |
| `services/camera` | `:155-160` — same block | `:178` `argus::client-health` → `argus::grpc-health` |

Both blocks keep the shape they had (guard, `add_subdirectory`, `EXCLUDE_FROM_ALL`) and gain a
comment naming the package that now declares the stubs.

**The guard never fires in the standard configure, and that is recorded rather than hidden.** Both
services add the generated client they link (`clients/voice` at `:61`, `clients/camera` at `:151`)
*before* this block, and every client package's first act is the same
`if(NOT TARGET argus_client_grpc_base) add_subdirectory(…/grpc …)` — which now declares the health
stubs on the way past. So by the time the health guard is evaluated, the target exists.

It stays because it is not the dead guard f1-6 flagged. Those named packages (`packages/socket`,
`packages/room`) that no source of the project reached; this one names the stubs the service's own
`health-rpc-service.{hxx,cc}` includes, so it states a real dependency instead of inheriting it from
a sibling's transitive graph. Phase 2's DAG verification (step 4) is where "which project owns which
guarded subtree" stops being per-consumer folklore and the question is settled once — the same
finding step 7 recorded for `services/guard`'s `argus::runtime`.

## Verification

- **The phase gate is green**: `./scripts/build-all.sh dev` exits 0, "[setup] All selected projects
  built and tested (profile: dev)". It is the second run. The first also exited 0 — same 18 projects,
  same warning classes, 0 errors — but the machine restarted before its log could be read and `/tmp`
  is a tmpfs, so the log is gone; the run reported here is the one that survived, and every number
  below comes from it.
- **The reached-test ledger is unchanged at 288**, project by project in `build-all.sh`'s order:
  cert 15, socket 8, sqlite 2, identity 15, sync 19, memory 16, intent 4, gateway 25, camera 34,
  productivity 23, notification 28, guard 36, tts 9, stt 3, vlm 5, llm 23, voice 13, tunnel 10 — the
  numbers step 4 recorded and step 7 reproduced. Arithmetic again, not luck: the deleted package
  declared no tests, so removing it could only cost a suite if some project had been building them,
  and the two projects that ever added the subtree added it under `EXCLUDE_FROM_ALL`.
- **The stubs are built by exactly the two projects that link them**, which is the
  `EXCLUDE_FROM_ALL` claim measured rather than assumed: `find . -name libargus_grpc_health.a`
  returns two paths, `services/voice/build/dev/tts-client/grpc/` and
  `services/camera/build/dev/clients/identity/grpc/`. The other six packages that add `packages/grpc`
  declare the module and never build it. The directory names are each project's *first* instantiation
  of the guarded package (`tts-client` and `identity`), not the one the health guard names — which is
  what "where the module actually lands" above describes.
- **No first-party warning**: 23 `Warning:` lines in the whole log, every one third-party — ncnn (8)
  and glslang (8) `CMAKE_CXX_STANDARD` notices, llama.cpp/ggml (3), conan's `ccache not found` note
  (3), openfst (1, from `services/stt/build/dev/_deps`). The same classes and the same total step 7's
  run recorded. (A case-sensitive `grep 'warning:'` reports zero and is wrong: the prefix is
  `-- Warning:`, capital W.)
- **No file names the old targets**: a grep for `clients/health`, `client-health` and
  `client_health` across every file type outside `build/`, `third_party/` and `docs/history/` returns
  one line — the comment in `packages/grpc/CMakeLists.txt` recording what the stubs were. The new
  name appears only in the three `CMakeLists.txt` that declare and use it.
- **Every `add_subdirectory` path resolves on disk**: 288 calls, no missing targets, two left
  variable-shaped (`"${module_dir}"` in `services/tts`, `…/packages/clients/${CLIENT}` in
  `services/camera`). The check substitutes `${CMAKE_CURRENT_SOURCE_DIR}`,
  `${CMAKE_CURRENT_FUNCTION_LIST_DIR}`, `${CMAKE_CURRENT_LIST_DIR}` and `${ARGUS_CMAKE_DIR}` before
  testing; an earlier version of it reported nine false "missing" paths, because a variable-shaped
  argument it could not resolve still had the call's closing paren glued to it.
- **Nothing stale named health survives in a build tree**: `services/voice/build/dev/clients/health`
  and `services/camera/build/dev/clients/health` — 8.1 MB each, holding a `libargus_client_health.a`,
  an `argus-client-health/generated` stub tree and `CMakeFiles/argus_client_health.dir` — were
  removed **after** the gate ran, so no tracked file changed with them and the green run above still
  describes the tree being committed.

## Findings outside this unit

- **The health stubs still link the client base.** `argus_client_module` builds every module on
  `argus_client_grpc_base` — `packages/grpc/src/grpc/grpc-client-base.{cc,hxx}`, the channel
  credentials, deadlines and `x-argus-*` caller metadata — which is machinery a *caller* uses.
  Nothing on the serving side touches it: both health services include exactly
  `<grpc/health/v1/health.grpc.pb.h>` and `<grpcpp/grpcpp.h>` and nothing from the base, so
  `argus_grpc_health` carries a client-side object into two server binaries. Recorded rather than
  fixed, because the value of this step is that it is mechanical — the link recipe is the client
  recipe, unchanged, and the gate proves the whole tree behaves. The `GROUP` argument it introduces
  is the natural place to settle it later (`if(ARG_GROUP STREQUAL "client")` around the base and the
  bridge), and Phase 2 step 1, the move into `lib/grpc`, is where the module's dependencies are
  reviewed as a whole.
- **`packages/grpc` still declares no target of its own.** Rule 25 says the folder is the module, and
  this folder's six source files back three OBJECT libraries (`argus_client_grpc_base`,
  `argus_client_grpc_bridge_entry`, `argus_client_grpc_bridge_exit`) plus, since this step, one
  generated module — there is no `argus_grpc` / `argus::grpc` for the folder itself. That predates
  this step (the folder became the substrate's home when step 1 gave it a `CMakeLists`), §9.1 keeps
  the package alive (`grpc | 6 | lib/grpc`), and step 12's rule 23–27 rewrite is where the question is
  asked — the same class of finding step 7 recorded for the hardware profile.
- **§9.1 needs no correction.** The `grpc` row already forecasts "+ standard health stubs" and the
  `clients/*` row already says "`health` dies; `auth` + `sync` are new" — this step is that sentence
  executed, with the `lib/` prefix deferred to Phase 2's rename exactly as rows 3, 4 and 7 deferred
  theirs. The baseline stays frozen at its measurement date; the deltas live in the Done rows.
- **`AGENTS.md` needed no change.** No rule, Key Files row or example names health, and the helper's
  two examples (`AGENTS.md:605`, `:610`) both call it without a `GROUP` — the new argument is
  documented where the helper is (`cmake/argus-module.cmake:229-234`). Rule 25's rewrite is step 12.
