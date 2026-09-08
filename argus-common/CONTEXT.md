# CONTEXT.md — why this folder exists

## Origin

The Argus backend (C++20/Drogon monolith) is being migrated to microservices
(`docs/migracion-microservicios.md`). `src/` disappears entirely over the
course of that migration; every folder at the repo root becomes a service or
a package. argus-common (step f7-1b) is the first package extracted this
way: the shared foundation library that `src/shared/CMakeLists.txt` used to
compile as `argus_common` moved into its own top-level folder, unchanged in
behavior.

## What moved and what didn't

Exactly what `src/shared/CMakeLists.txt` compiled into `argus_common`:
`enums.hxx`, `access/role-access.hxx`, `validation/*`,
`exceptions/response-exception.hxx`, `wrapper/api-response`,
`wrapper/thread-budget`, `wrapper/blocking-task`, `wrapper/cancellation`,
`wrapper/hardware-profile` (its own module, `add_subdirectory`'d),
`wrapper/nats`, `services/config-service`, `utils/schema-runner`,
`dtos/socket-emit`, plus three pure-C++ sync contract headers,
`contracts/{sync-operation,sync-filter,syncable}.hxx`.

The sync contracts moved alongside the rest, not later, because they are
interfaces (`Syncable`, `SyncFilter`, `SyncOperation`), not wire contracts:
`argus-contracts/` stays strictly protos + generated stubs + the thin SDK
wrapper layer, so a C++-only interface with no `.proto` counterpart belongs
in the C++ foundation package, not there. The rest of `src/shared/contracts`
(`camera-*`, `identity-change-sink.hxx`, `push-intent-sink.hxx`,
`tool-contracts.hxx`, `user-*`) are domain seams owned by later steps and
stayed in `src/shared/contracts`.

Everything else in `src/shared` (repositories, schemas, the other
`services/*`, the rest of `wrapper/*`, `vocabulary`) stays where it is;
those are later steps of the same arc.

## The include-prefix invariant

1205 `#include <shared/...>` (also `feature/`, `filter/`, `config/`)
statements across ~450 files had to keep resolving unchanged, so the moved
files kept their `shared/...` relative path: `src/shared/wrapper/nats/
nats-bus.cc` became `argus-common/src/shared/wrapper/nats/nats-bus.cc`, and
`argus_common`'s PUBLIC include root became `argus-common/src` (was
`src/shared/../` = the whole `src/` tree). `argus_identity` and `argus_sync`
still export the old `src/` root for the files that stayed there, so both
roots coexist without anything being duplicated.

That story has two exceptions, both found empirically (by actually building,
not just by inspecting `#include` lines) while doing the move — everything
below governs every later step of this arc, and both should be checked with
a real build before any future file leaves `src/shared`, not just a grep of
its own includes:

### 1. The moved-file side: a moved file may only include moved files, or files already in the destination

If a file about to move includes another `src/shared/...` file by path, that
included file must be moving too (or already have moved). `enums.hxx`,
`role-access.hxx`, `validation/*` are all self-contained within the moved
set. The one violation found this step: `dtos/socket-emit/
socket-emit-dto.hxx` included `shared/contracts/sync-operation.hxx`, which
was going to stay behind — resolved by moving the three sync-contract
headers alongside it (see above), not by adding a second include root back
onto `argus_common`.

**Quoted-include sub-case.** A quoted `#include "sibling.hxx"` resolves
relative to the including file's own directory, independent of any `-I`
search path — so it breaks even when the target is reachable through a
consumer's include root, the moment the two files stop being physical
siblings. `src/shared/contracts/camera-sync-source.hxx` (an explicitly
staying file) did `#include "syncable.hxx"`; once `syncable.hxx` moved out
of `src/shared/contracts/`, that quoted include stopped resolving even
though every target that compiles `camera-sync-source.hxx` links
`argus_common` and would otherwise have picked it up fine through the
angle-bracket path. Fixed by matching the file's own existing convention for
its other cross-directory reference (`<filter/jwt/jwt-filter.hxx>`):
`#include <shared/contracts/syncable.hxx>`. Grep every staying file for a
quoted include of anything that's about to move before moving it.

### 2. The consumer side: a target that leaned on argus_common's old whole-tree include root must declare that root itself

Before this step, `argus_common`'s PUBLIC include directory was the entire
`src/` tree (`target_include_directories(argus_common PUBLIC
${CMAKE_CURRENT_SOURCE_DIR}/..)` from `src/shared/`), not just the files it
actually compiled. Any target that linked bare `argus_common` — without also
linking `argus_identity`/`argus_sync`, which export that same root on their
own account — got free, undeclared access to every `shared/*`/`config/*`/
`filter/*` header in the monolith, whether or not that header had anything
to do with `argus_common`'s own sources. Narrowing `argus_common`'s root to
`argus-common/src` (the point of this step) closes that back door, and every
consumer that depended on it needs its own, explicit fix — this is a
per-consumer audit, not something a file move can paper over.

All twelve service binaries (`argus-llm`, `argus-stt`, `argus-tts`,
`argus-vlm`, `argus-memory`, `argus-tunnel-client`, `argus-tunnel-relay`,
`argus-camera`, `argus-gateway`, `argus-productivity`, `argus-notification`,
`argus-voice`) already declare their own `src/` include (e.g.
`argus-llm/CMakeLists.txt` puts `${CMAKE_CURRENT_SOURCE_DIR}/../src` PUBLIC
on `llm-core`) — they never depended on `argus_common`'s root for those
headers, so they were unaffected.

`src/test/unit/CMakeLists.txt` is the one place that did: 22 targets in that
tree compile `shared/`, `config/` and `filter/` sources by raw path (a
pre-existing rule-25 violation — file lists instead of module links, not
something this step fixes) and relied on `argus_common` for the whole `src/`
root along the way. Fixed with one directory-scoped, explicitly transitional
line at the top of that file:

```cmake
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../..)
```

Transitional and scoped to that one file on purpose: the unit suites are
distributed into each service's own `tests/` in a later step of this arc,
and `src/` disappears at the end of it, so this line disappears with it — it
is not a general-purpose escape hatch and should not be copied elsewhere.
One target in that file needed one more fix beyond the directory-wide root:
`memory-remote-adapter-test` compiles `config-service.cc` by raw path and
includes `config-service.hxx`, both of which really did move into
`argus-common` (they are not just an old-back-door dependency), so it also
carries an explicit `${CMAKE_SOURCE_DIR}/argus-common/src` include.

## Step f7-1c: config, sqlite-stmt, json-util, json-diff, s3 storage

Five more foundation pieces moved in, closing the last raw-path duplication
of files that already lived entirely inside `argus_common`'s consumers:
`src/config/{app-config.cc,app-config.hxx,service.hxx}`,
`src/shared/wrapper/sqlite-stmt/sqlite-stmt.hxx`,
`src/shared/utils/json-util/json-util.hxx`,
`src/shared/utils/json-diff/{json-diff.cc,json-diff.hxx}`, and
`src/shared/services/storage/{s3-storage-service.cc,s3-storage-service.hxx,
s3-signing.hxx}`. Left behind on purpose: `src/shared/services/sqlite/`
(`db-service`, `VecDb`, which still reach into domain repositories — a
design problem, not a move), `src/shared/utils/text-match/` and
`text-norm/` (single-domain, belong to argus-memory later), and
`src/shared/services/storage/private-portrait-service.{cc,hxx}` (reached
identity repositories; moved to `argus-identity/` in f7-2d).

`app-config.cc` was compiled by raw path in 16 places; `s3-storage-service.cc`
in 1; `json-diff.cc` in 2. All 19 raw-path entries were deleted since every
one of those targets already linked `argus_common` (directly, or
transitively through `argus_identity`/`argus_sync`) — the library now
supplies the object instead. Comments that described why a file was raw-path
compiled (e.g. "the centralized response config") were trimmed or reworded
alongside the deleted line rather than left describing code that is no
longer there.

**New dependency: OpenSSL.** `s3-signing.hxx`/`s3-storage-service.cc` are
the first `argus_common` sources to need OpenSSL. Not every root/standalone
build path that `add_subdirectory(argus-common)`s finds OpenSSL first (e.g.
`argus-voice`, `argus-productivity`, `argus-notification`, `argus-gateway`
standalone blocks don't), so `argus-common/CMakeLists.txt` now does its own
guarded `find_package(OpenSSL REQUIRED)` (`if(NOT TARGET OpenSSL::SSL)`)
instead of assuming a caller already ran it, and links it PRIVATE since
nothing in the public headers exposes an OpenSSL type.

**Another quoted-include break, same shape as the one in step f7-1b.**
`src/shared/services/storage/private-portrait-service.cc` (at the time
still in `src/`; it moved to `argus-identity/` in f7-2d)
did `#include "s3-storage-service.hxx"`; once that header moved out of
`src/shared/services/storage/`, the quoted form stopped resolving even
though `argus_identity` (which compiles this file) links `argus_common` and
would otherwise pick it up fine. Fixed the same way as before, matching the
file's own convention for its other cross-directory references
(`<shared/repositories/...>`): `#include
<shared/services/storage/s3-storage-service.hxx>`.

**A consumer-side break the file-list audit didn't surface on its own:**
`tools/migrate-{identity,productivity,camera,notification}/CMakeLists.txt`
each compile a `*-migration.cc` that does `#include
<shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>`, resolved before this step
through their own `${SRC_ROOT}` include (not through `argus_common`, which
none of them link). Moving `sqlite-stmt.hxx` out of `src/` broke all four the
same way the six-service standalone-build gap did, but for the root-tree
build itself, not just standalone. Fixed by adding
`${CMAKE_SOURCE_DIR}/argus-common/src` to each of their `PUBLIC` include
directories, the same pattern `memory-remote-adapter-test` already used for
`config-service.hxx` in f7-1b. This is the reason invariant #2 above says
"per-consumer audit, not something a file move can paper over" — a target
list built from `grep`ing raw-path `CMakeLists.txt` entries misses consumers
that only reach a moved file through an include, never a compiled source of
their own.

## Known regression (deferred, not introduced by this step)

Six services — `argus-tts`, `argus-stt`, `argus-vlm`, `argus-llm`,
`argus-memory`, `argus-tunnel` — link `argus_common` but carry no
standalone-build fallback
block (`if(NOT TARGET argus_common) add_subdirectory(...) endif()`), unlike
`argus-voice`, `argus-camera`, `argus-productivity`, `argus-notification`
and `argus-gateway`, which all repoint that block at `../argus-common` as of
this step. Building any of the six outside the root tree (e.g. `cmake -S
argus-llm -B build-standalone`) will fail to find `argus_common`. This was
already a known gap before this step and stays deferred to a later one; it
is unrelated to the include-root issues above, which are about the root-tree
build.
