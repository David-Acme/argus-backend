# Phase 4 step 1 — the tts feature gets the reference interior

`services/tts`'s feature folder is now the shape rule 23 defines: `controllers/`,
`dtos/`, `services/` and `infra/` under `feature/synthesis/`, with no `api/`
level and no `domain/`. Six files moved as git renames, so the change itself is
twelve include lines, three CMake source paths and the two documents that named
the old paths.

## What existed before

- `feature/synthesis/api/http/{controller,dto}` — the pre-migration `api/`
  level, which rule 23 names as a spelling to delete rather than carry
  ("there is no `feature/api/<resource>/` level").
- `feature/synthesis/domain/` — the facade under a name the reference interior
  does not use; rule 23's feature interior is
  `{controllers, dtos, repositories, schemas, services, infra}`.
- Everything else in the service was already the reference shape:
  `src/app/{main.cc,rpc/}` for composition, and `infra/supertonic/` where the
  plan says it stays.

## The move

| From | To | git |
|---|---|---|
| `feature/synthesis/api/http/controller/tts-controller.{hxx,cc}` | `feature/synthesis/controllers/` | R100 / R097 |
| `feature/synthesis/api/http/dto/synthesize-dto.{hxx,cc}` | `feature/synthesis/dtos/` | R100 / R084 |
| `feature/synthesis/domain/tts-service.{hxx,cc}` | `feature/synthesis/services/` | R100 / R100 |

The two files below 100% carry the include rewrite; the other four are pure
moves.

References updated with them:

- **Includes, twelve lines across six files** — `src/app/main.cc:6,10`,
  `src/app/rpc/tts-rpc-server.hxx:4`,
  `controllers/tts-controller.cc:4,8`, `dtos/synthesize-dto.hxx:4`,
  `tests/unit/tts-rpc-test.cc:12`, `tests/unit/tts-wire-test.cc:4,11`. The
  three in-directory `"…"` includes stayed with their headers.
- **`feature/synthesis/CMakeLists.txt`** — the two targets' `SOURCES` lists
  (three paths). Their names (`argus::tts-synthesis`,
  `argus::tts-synthesis-http`) and the top-level `argus_service()` links are by
  name (rule 25) and did not change, so no consumer was touched.
- **`services/tts/AGENTS.md`** — the `## Layout` block, the include guidance,
  and a `src/shared/` sentence that described a directory the service does not
  have; **the root `AGENTS.md`** key-files row for the service, whose
  `(domain/ → services/ in Phase 4 step 1)` parenthetical is now discharged.

## The row's last clause was already vacuous

Phase 4 step 1 also asked for an "empty `src/shared/services/`" to go.
`services/tts/src` holds only `app/` and `feature/`, and nothing under
`services/tts/src/shared` was ever tracked (`git log --all --
services/tts/src/shared` is empty; the parent directory's own mtime predates
this unit), so there was nothing to remove.

`repositories/` and `schemas/` are absent, and rightly: the service owns no
database at all — no `database/` directory, no `schema.sql`, no
`DbClient`/`execSqlCoro`/`DbService` reference and no `[database]` key in its
template.

## Review

A read-only adversarial reviewer swept the tree independently and its findings
are data rather than instructions: each was measured again before it was acted
on or dismissed. Three findings, all pre-existing facts rather than defects this
change introduced, and two of them acted on here:

- **The root `.gitignore` did not list `go2rtc.yaml`.** The file at the
  repository root is the native-run artifact of
  `[camera] go2rtc_config = "go2rtc.yaml"`, holds RTSP credentials, is untracked
  and was **not** ignored (the root `.gitignore` held `/build/` only, while
  `services/camera/.gitignore` lists it for its own copy and every
  `Dockerfile.dockerignore` excludes it). Now ignored: rule 17b says a commit
  never carries an instance secret, and staging by hand with an `:(exclude)`
  pattern was the only thing standing between the two.
- **`services/tts/AGENTS.md` described a `src/shared/` that does not exist.**
  Rewritten to say the folder does not exist yet and that rule 23's 2+ rule
  earns it — which is also why the plan row's clause above was vacuous.
- **The plan still reads as if the step were unstarted.** That is this unit's
  own closing edit, made below.

Checks the review ran and reported, each of which I re-derived where it is
cheap: no stale spelling outside `docs/history/`; every
`#include <feature/synthesis/…>` (nine sites) resolves to a file on disk, and
of 3478 slash-bearing includes tree-wide the 70 unresolved paths are all
third-party or generated protobuf; the two CMake `SOURCES` lists name exactly
the feature's seven `.cc` files, once each; no empty or orphan directory; the
docs match the tree; the diff adds no comment (`git diff HEAD -U0` adds 18
lines, none a comment, and `check-comments` agrees); and rule 23's "Today,
against that target" paragraph is still accurate about this service — `src/app/`
in exactly `auth`, `identity`, `sync`, `tts`; `src/config/` in exactly `auth`,
`identity`, `sync`; `feature/` in nine services. Its corroboration: the tts
compile database and object files are at the new paths, and the two suites are
green (92 and 47 assertions). One non-finding it recorded: the build directory
still holds object files for the old paths, which is gitignored output the
CMake regeneration cannot include.

## Verification

- `./scripts/build-all.sh dev --only tts` — 29/29 tests, 0 failures, exit 0,
  and not one `warning:` line. The two pre-build gates ran with it:
  `check-comments` 1328 files checked, 0 comments, and `check-deps` 79
  declarations, 670 edges, 0 forbidden, 0 cycles, 0 unresolved, 23 deferred —
  both identical to the counts the gateway deletion left, which is what a
  paths-only change must produce.
- `scripts/check-tidy.sh` over the whole tree — the gate a `--only` run skips
  deliberately: 537 translation units, 2902 findings over 45 checks against
  the 2902 baseline, exit 0, no check risen. The move renames files and moves
  no code, so the totals are the gateway deletion's, unchanged.
- The move's completeness, measured directly: seven `.cc` files in the feature,
  the same seven named once each across the two target lists, no path left
  under an `api/` segment, no empty directory under `src/` or `tests/`.

## Files

13 changed (+18/−19): `AGENTS.md`, `.gitignore`, `services/tts/AGENTS.md`, the
six renames, `services/tts/src/app/main.cc`,
`services/tts/src/app/rpc/tts-rpc-server.hxx`,
`services/tts/src/feature/synthesis/CMakeLists.txt` and the two test
translation units.
