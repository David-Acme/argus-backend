# Phase 1 step 3 — merging `json` and `hash` into `text`

Scope: `packages/json` and `packages/hash` deleted, their files moved into
`packages/text`; 8 files moved, 21 `CMakeLists.txt` and 2 `AGENTS.md` rows
repointed, **zero** `.cc`/`.hxx` edited. Base `7cc5564`.

## What the three packages were

- `text` — `text-norm` only (2 files), no dependencies, no test.
- `json` — `json-util.hxx` (parse/serialize/fold helpers), `json-diff.{hxx,cc}`
  (`JsonDiff`: flat object diff plus snapshot), 3 files, `DEPENDS Drogon::Drogon`
  for jsoncpp.
- `hash` — `base64.{hxx,cc}`, `fnv-hash.hxx` (the FNV-1a checksum the migration
  tools use), `sha256.hxx` (self-contained SHA-256, no crypto dependency), 5
  files plus `sha256-test`, no dependencies.

Three packages, all leaf tier-1, all exposing the same include root `src`, with
their utilities already under `src/shared/utils/<utility>/`.

## Why this is a CMake-level change and nothing else

Because the three include roots are identical and the utility folders do not
collide, moving `json`'s and `hash`'s folders under `packages/text/src/shared/utils/`
leaves **every** `#include <shared/utils/...>` in the tree resolving unchanged.
Measured, not assumed: `git diff --name-only -- '*.cc' '*.hxx'` is empty. The
merge is therefore a target-level operation — one provider, one alias, one
include root — and the only files that had to change are the ones that named the
old packages.

## What the unit did

1. **Moved 8 files** with `git mv` (history preserved): `json-diff.{cc,hxx}`,
   `json-util.hxx`, `base64.{cc,hxx}`, `fnv-hash.hxx`, `sha256.hxx` and
   `sha256-test.cc` (into `packages/text/tests/unit/`).
2. **One module**: `packages/text/CMakeLists.txt` now lists all nine sources,
   keeps `DEPENDS Drogon::Drogon`, and wires `sha256-test` against `argus_text`.
3. **Deleted the two packages** — `packages/json/` and `packages/hash/`
   (their `CMakeLists` were two of the 21).
4. **Repointed 16 `CMakeLists`**: every `argus::json` and `argus::hash` link
   became `argus::text`, and every guarded `add_subdirectory` now owns
   `packages/text`. Where a target listed both aliases, the pair collapsed into
   one link.
5. **Collapsed the guards**: 20 `json`/`hash`/`text` guard blocks became 15 —
   `sync`, `camera`, `gateway`, `guard` and `llm` each guarded both packages
   adjacently, so the pair became one.
6. **Repointed the four migration tools** (`identity`, `camera`, `notification`,
   `productivity`), which reach the checksum header by include *path*
   (`.../packages/hash/src`), and the two `AGENTS.md` key-file rows.
7. **Removed the orphaned build output**: `build/dev/json` and
   `build/dev/hash` directories inside eleven project build trees, which no
   configure references any more.

## Verification

- **No source edit**: 0 `.cc`/`.hxx` in the diff, and `git diff -M` reports all
  eight renames at 100% similarity — the include paths survived the move
  exactly as designed, byte for byte.
- **No stragglers**: no `CMakeLists.txt` anywhere names `argus::json`,
  `argus::hash`, `packages/json` or `packages/hash`; `third_party/` aside, no
  file in the tree mentions the two dead package paths.
- **Every consumer owns its subtree**: each of the 17 files that links
  `argus::text` (16 repointed plus `packages/intent`, which already linked it)
  carries exactly one `if(NOT TARGET argus::text)` guard; the two feature
  modules under `services/guard/src/feature/` do not, because their project
  owns the subtree — the same rule the tree follows everywhere.
- **Coverage follows the package**: `sha256-test` now runs in 13 of the 18
  projects because it arrives with `packages/text` wherever that is linked.
  Six projects gained it in this build and none lost a suite: cert 11 → 12,
  socket 5 → 6, identity 11 → 12, intent 4 → 5, memory 13 → 14, tunnel 9 → 10.
- **Build**: `./scripts/build-all.sh dev` — 18/18 projects, all suites green,
  no first-party warnings. Tests per project: cert 12, socket 6, sqlite 2,
  identity 12, sync 14, memory 14, intent 5, gateway 20, camera 29,
  productivity 18, notification 23, guard 32, tts 7, stt 3, vlm 5, llm 21,
  voice 8, tunnel 10.

## Code review of the change

Reviewed: the 21 `CMakeLists` (each guard block's path and binary dir, each link
list for a surviving duplicate), the moved files (byte-identical, `git mv`),
`AGENTS.md`, and the four migration tools. Findings and their disposition:

- **A duplicate guard the script left behind**: in `packages/memory/CMakeLists.txt`
  the `json` and `text` guards were not adjacent — the `nats` guard sat between
  them — so the collapse produced two `if(NOT TARGET argus::text)` blocks. Benign
  at configure time (the first creates the target, the second's condition is then
  false) and the project built green with it, but it is noise: the second block
  is gone.
- `services/gateway/CMakeLists.txt` guards `packages/text` without linking
  `argus::text` in any target — it repointed a guard whose purpose is the
  standalone configure of sources that include the JSON headers through the
  packages it does link. Pre-existing shape, left as it was apart from the path.
- `services/notification` and `services/productivity` linked `argus::hash`
  while owning only the `json` subtree: the target arrived through
  `packages/sync`'s own guard. They now own `packages/text` themselves, so the
  accidental-dependency pattern is gone for both — it is a side effect of the
  merge, not a separate cleanup.
- The four migration tools take an include **path** into `packages/text/src`
  (and `packages/sqlite/src`) instead of linking the target. Pre-existing, and
  outside this unit — only the path was repointed, so the tools keep building.

## Deferred, and findings outside this unit

- **Namespaces inside the merged package**: the SHA-256 header keeps
  `namespace argus::hash`, `fnv-hash.hxx` keeps `Fnv1a` at global scope and
  `json-util.hxx` keeps `namespace json_util`. §4.2 rule 5 says a unit declares
  `argus::<unit>`, so `argus::hash` inside `lib/text` is drift — but renaming a
  namespace reaches into five consumer sources and their tests, and step 3 is a
  package merge. Phase 4's header normalization owns it.
- The migration tools' path-based reach into other packages' `src/` is exactly
  what the architecture forbids for new code; they are legacy tooling, and
  neither `check-deps.sh` nor the migrate-tool cleanup exists yet.
- The `lib/text` destination in §9.1 is the Phase 2 move; this step only made
  the folder contain everything that table says it should.
