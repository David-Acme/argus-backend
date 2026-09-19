# Phase 1 step 5 — deleting the dead artifacts

Scope: the row's three items, measured before anything was deleted — the "unused
presets", the gateway's leftover mounts and config in `argus-deploy`, and (named in
the row only to be spared) `user_action_log`. Two of the three turned out to be
smaller than the row drafted, the third does not exist as drafted, and what is
actually dead sits beside them: one preset, six mount-point directories in one
image, two ignore rules whose subjects the tree has retired, three entries missing
from ignore lists that decide what an image's build stage sees, a config key with no
reader, and a CI orchestrator test that has been red since step 2. Base `62b841c`.

## What the row asked for, and what measuring it found

**`user_action_log` is live**, exactly as the row says: its repository is
`packages/audit/src/shared/repositories/user-action-log/user-action-log-repository.cc`
(the statement beside it is `INSERT INTO user_action_log …`), its table is part of
the frozen sync vocabulary (`packages/contracts/sync-contract/src/shared/contracts/table-name.hxx:82,116`)
and its schema is `packages/identity/database/schema.sql:198`. Nothing here touches
it; §3.5's move to `sync` stays Phase 3a's.

**Unused presets: exactly one, and it was the gateway's.** All 18 projects carry a
tracked `CMakePresets.json`; 17 of them define only `dev`/`prod` configure and build
presets, and `scripts/build-all.sh:101-104` runs precisely those (`cmake --preset`,
`cmake --build --preset … -j 8`, and the extra-target build) — every one is used.
The exception was `services/gateway/CMakePresets.json`, the tree's only `testPresets`
block (`{"name": "dev", "configurePreset": "dev"}`): no file in the tree invokes
`ctest --preset`. Every `--preset` occurrence outside `third_party/` is a configure or
a build one, and the gate itself runs a bare `ctest --output-on-failure` inside
`build/dev` (`build-all.sh:108`). The block is deleted; the file now has the shape its
17 siblings have. Generated presets are not artifacts: Conan writes a
`CMakeUserPresets.json` beside each file (untracked, gitignored, ignored by every
`Dockerfile.dockerignore`), which is why `cmake --list-presets` in `services/gateway`
still offers a generated `conan-debug` next to `dev`/`prod`.

**The gateway's mounts and its config in `argus-deploy` are all live — there was
nothing to delete there.**

- The block mounts seven paths and each has a reader: `./config.gateway.toml` (the
  service's own config), `${ARGUS_CERTS_DIR}` → `/opt/argus/certs` (`[cert] dir`,
  `ca_cert`, `server_cert`…, plus `listener-config.cc`'s `certs/server.{pem,key}`
  default), `…/data/identity` → `…/database` (`[identity] db = "database/identity.db"`),
  `…/data/gateway` → `…/gateway` (`[gateway] db = "gateway/gateway.db"`),
  `packages/identity/database/schema.sql` → `database/schema.sql` (`[identity] schema`),
  `services/gateway/database/schema.sql` → `gateway/schema.sql` (`[gateway] schema`)
  and `${ARGUS_MODELS_DIR}` → `/opt/argus/models` (`[face] enabled` →
  `FaceService::init("models/face")`, `packages/identity/src/shared/services/face/face-service.cc:110`).
- All **85 keys** of `argus-deploy/config.gateway.toml.example` have a reader: 60 are
  read by their literal `"section.key"` in first-party code (e.g. `mdns.enabled` and
  friends in `services/gateway/src/shared/services/mdns/mdns-service.cc:105-115`,
  `identity.rpc_host`/`rpc_port` in the identity RPC server), 22 are `[drogon.app]`
  keys that Drogon's own loader consumes rather than first-party code, and the 3 under
  `[mdns.txt]` are read as a *table* — `ConfigService::getStringPairs("mdns.txt")`,
  mdns-service.cc:115 — which is exactly what a per-key grep reports as unread. Zero
  unread keys.
- The compose declares two named volumes and uses both (`camera-stream` at line 120,
  `nats-data` at line 729); there is no declared-but-unmounted volume left.

So the row's premise about tracked files under `argus-deploy` does not hold. What is
dead *in* that folder is untracked residue under `data/` — reported below, not deleted,
because it may hold rows the live databases do not.

## What was dead, and what was done

| Artifact | Proof it is dead | Action |
|---|---|---|
| `services/gateway/CMakePresets.json` `testPresets` | no `ctest --preset` caller in the tree; the gate runs a bare `ctest` | deleted (the file's only unused block) |
| `services/gateway/Dockerfile` runtime `mkdir -p` entries `third_party/go2rtc camera stream productivity notification memory` | the image's own compose block mounts none of them; `argus-deploy/CONTEXT.md:446` states the gateway links no go2rtc code and "neither mounts nor spawns go2rtc" (go2rtc is the camera's: `Go2rtcManager`, `[camera] go2rtc_bin`/`go2rtc_config`); `camera`, `stream`, `productivity`, `notification` and `memory` are the *other* services' data directories, owned by their own images | deleted; the list is now `certs database gateway models` — exactly the directories the gateway's own compose block mounts into, file mounts aside. The same rule reproduces guard's existing `database guard` |
| `argus-deploy/config.memory.toml` in the ten `Dockerfile.dockerignore` | the file was deleted in the f8 wave and does not exist; the rule ignored nothing | replaced by `argus-deploy/config.guard.toml` |
| `argus-deploy/config.guard.toml` missing from those ten lists | the file exists on disk (`-rw-------`, 4815 B), `argus-deploy/.gitignore` ignores it, and every Dockerfile does `COPY . .` from the repo root (`context: ..`) — so it was copied into every image's build stage | added to the ten, and to guard's new list below |
| `argus-deploy/.env` missing from every list | same build context; host-local only (`ARGUS_UID/GID/DATA_DIR/CERTS_DIR/MODELS_DIR/GO2RTC_DIR`, no secrets) and gitignored, and the ignore lists' rule is that the deploy folder's local files stay out of the build context | added to the ten lists and to the root `.dockerignore` |
| `argus-deploy/data` missing from the eleven lists | 15 MB of untracked deploy state — the live databases the services open and the rustfs object tree — sitting under the build context every image `COPY . .` reads. The intent is already declared three lines away: the root `.dockerignore:5` lists it, but a `Dockerfile.dockerignore` replaces that fallback, so the eleven services were the ones not getting it | added to the eleven |
| `docker/runtime` in the eleven lists | the path does not exist and tracks nothing: the `docker/` tree was retired in `b8233d0` ("retire the legacy compose stack, rustfs provisioning and legacy config"), and no tracked file or script names it — the rule is named by these lists alone | deleted |
| `services/guard/Dockerfile.dockerignore` did not exist | guard is the only one of the eleven services without one, so its `COPY . .` fell back to the root `.dockerignore`, which ignores neither `certs/` nor `third_party/go2rtc` nor the deploy configs — the build stage took the whole repository | created, identical to the corrected ten |
| `scripts/build-all-test.sh` `--only common` | `packages/common` was deleted in step 2, so the script — the CI workflow's "Test build orchestrator" step — fails today with `[error] unknown project for --only: common` and exit 1 | `--only cert`, a project with no extra targets, which is what the two cases measure (conan alone under `--install-only`, configure+build alone under `--no-tests`) |
| `services/productivity/config.toml.example` `[identity] db = "database/identity.db"` | unread: the service reads `productivity.db`/`productivity.schema` (`services/productivity/src/productivity/productivity-config.cc:6-19`) and its identity access filter reads `identity.target`/`identity.rpc_secret` (`packages/auth/src/filter/identity-access.cc:14,35`); the `db` key is a leftover of the pre-split direct-DB access, and the deploy example carries `target`/`rpc_secret` instead | deleted |

## Verification

- **The CI orchestrator test passes again**: `./scripts/build-all-test.sh` exited 1
  with the `common` error before the change and now prints `build-all tests passed`
  (exit 0). It is the "Test build orchestrator" step of `.github/workflows/ci.yml`,
  which runs it before "Build and test standalone projects" — so a CI step that had
  been red since step 2 is green again.
- **The edited preset file is what CMake reads**: it parses, and
  `cmake --list-presets` in `services/gateway` lists `dev` and `prod`; the gateway
  then configured and built from that file inside the gate below.
- **The eleven ignore lists are one file**: after the edits all ten plus guard's new
  one are byte-identical (md5 `4608df190d07e800136a50173a6a5282`), the two dead rules
  are gone and the three additions are present in each; the root `.dockerignore`
  gained `argus-deploy/.env` and `argus-deploy/config.*.toml` as the fallback for any
  Dockerfile that has no list of its own. `bash -n` passes on the edited script.
- **The mount-versus-mkdir audit** is what justifies the gateway's new list and what
  produces the table in Findings: for each image, the directories its compose block
  mounts into, plus the directories its own config and compiled-in defaults name,
  against the directories its `mkdir` creates.
- **What no gate checks**: no gate builds an image (CI runs the two shell scripts
  only) and Docker is not exercised here, so every Dockerfile and compose claim in
  this report is a static one — a directory audit and a config-reading audit, not a
  container run. The full gate below exercises the C++ tree, which this step does not
  change.

## The gate

`./scripts/build-all.sh dev` is green — exit 0 — on all 18 projects with 288 reached
tests, and every configure warning in the run is third-party (ccache's absence and
the `CMAKE_CXX_STANDARD` notices from ncnn, glslang, llama.cpp and openfst): no
first-party warning anywhere. This step changes no source, so the per-project counts
are step 4's counts, and the run reproduces them project by project:

| project | tests |  | project | tests |
|---|--:|---|---|--:|
| `cert` | 15 |  | `productivity` | 23 |
| `socket` | 8 |  | `notification` | 28 |
| `sqlite` | 2 |  | `guard` | 36 |
| `identity` | 15 |  | `tts` | 9 |
| `sync` | 19 |  | `stt` | 3 |
| `memory` | 16 |  | `vlm` | 5 |
| `intent` | 4 |  | `llm` | 23 |
| `gateway` | 25 |  | `voice` | 13 |
| `camera` | 34 |  | `tunnel` | 10 |
|  |  |  | **total** | **288** |

## Code review of the change

- **The row's premise failed, and the report says so instead of forcing it.** Two of
  the three items were drafts from the reconnaissance that did not survive
  measurement: the gateway's mounts and config are all live (above), and the "unused
  presets" reduce to one block. Nothing was deleted to make the row's words true.
- **The replacement `--only` project had to be a project.** `packages/text` — the
  merge target of `json` and `hash` — looks like `common`'s successor but is not one of
  `build-all.sh`'s 18 projects (it is a subtree its consumers build), so the test could
  not be repointed at it. Both surviving cases need a project with no extra targets
  (the assertions count two `cmake` calls), which leaves `cert`, `socket`, `sqlite`,
  `sync`, `memory` and `intent`; `cert` is the first and smallest. The script's own
  unknown-project case is unaffected and still asserts the failure.
- **Several of the actions are additions, not deletions, and are flagged as such**:
  the `gateway` directory in the gateway's `mkdir` (the deploy config writes
  `gateway/gateway.db` and `gateway/schema.sql` there, and the compose bind is what
  mounted it until now), guard's new `Dockerfile.dockerignore` (a different service
  than the row names), the two root-`.dockerignore` patterns, and the
  `argus-deploy/data` entry in the eleven lists. The alternative for the gateway was
  `certs database models` — its compiled-in defaults alone — but guard's file, the
  only modern one in the tree, creates its own deploy data directory too, and the
  gateway's image should not depend on Docker creating a bind target for its own
  database.
- **`.env` is framed for what it is**: host-local paths, not secrets. The reason to
  ignore it is the same as the ten config files beside it — the deploy folder's local
  state has no business in an image — and the report does not claim a leak it did not
  find. The guard config *is* secret-bearing, and that one was a real exposure.
- **What a reviewer should push on**: the nine Dockerfiles left alone (Findings) and
  the probe captures (Findings) are the two places where this step stops short of its
  own headline, both with the reasoning recorded.

## Findings outside this unit

- **Nine Dockerfiles carry the same monolith `mkdir` list** — camera, productivity,
  notification, tts, stt, vlm, llm, voice and tunnel each create all nine directories.
  Running the audit rule above (the directories the image's compose block mounts into,
  plus the directories its own config and compiled-in defaults name) over those nine
  gives, per image, the deletion candidates below — the gateway's row is the one this
  step acted on, and the same rule produced exactly its six:

  | image | neither mounted nor named by it | kept by the rule |
  |---|---|---|
  | gateway *(done)* | `third_party/go2rtc camera stream productivity notification memory` | `certs database gateway models` |
  | camera | `certs productivity notification memory` | `camera database models stream third_party/go2rtc` |
  | productivity | `certs models third_party/go2rtc camera stream notification memory` | `database productivity` |
  | notification | `certs models third_party/go2rtc camera stream productivity memory` | `database notification` |
  | guard *(already correct)* | — | `database guard` |
  | tts / stt / vlm / voice | `certs database third_party/go2rtc camera stream productivity notification memory` | `models` |
  | llm | `certs third_party/go2rtc camera stream productivity notification` | `database memory models` |
  | tunnel | `certs models third_party/go2rtc camera stream productivity notification memory` | `database` |

  Deferred, not done here: the keep-set is a per-service decision that needs the
  package-level defaults folded in first — `certs/` comes from
  `packages/config/src/server/listener-config.cc:37-38`, `models/` from `FaceService`
  and the model loaders, `database/` from the DB-owning packages — and no gate builds
  an image, so the change needs its own unit and its own static audit.
- **`services/gateway/tools/probe-captures/` is referenced by nothing**: 196 files,
  816 KB of recorded HTTP probe transcripts (the F1-3b review baselines), no build, no
  source and no document naming them, and the generator `services/gateway/tools/probe-identity-matrix.sh`
  takes its output directory as an argument, so nothing regenerates or consumes this
  copy. Left in place: it is a record, not a build artifact, and deleting a record is
  the owner's call. The two honest options are deleting them (git keeps them) or
  moving them under `docs/history/`.
- **`argus-deploy/data/` holds untracked residue**: a 0-byte `schema.sql`
  (root-owned, 10 September, referenced by no compose revision or script), a 0-byte
  `camera.db` (the camera service creates its own at boot; `scripts/provision-host.sh`
  makes the directories), and a flat 864 KB `identity.db` with its `-shm` beside the
  live copy in `data/identity/` — `migrate_identity_dir()` only folds the flat file in
  when the target directory is empty, so it is stranded. Not deleted: it may hold rows
  the live database does not.
- **Stale prose, for step 11**: `argus-deploy/CONTEXT.md:180-186` still describes each
  database as living "on its own dedicated named volume" with `argus-cutover-*-db`
  names that no longer exist (the compose declares only `camera-stream` and
  `nats-data`), and `:408-409` claims the gateway's `[productivity] db` /
  `[notifications] db` point at those mounts, while the example carries
  `proxy_url`/`grpc_target` for both and no `db` key at all.
