# Phase 5 step 2 — the frozen `/sync` golden frames replay byte-identical

Plan row: `docs/history/plans/architecture-plan.md` "2 | Frozen `/sync` golden
frames replay byte-identical".

## What the row asks for

That the session recorded into `services/sync/tests/fixtures/sync/` on
2026-09-08 replays unchanged against the tree as it stands: the wire the
mobile app already speaks must not move. Answering it needed three things the
tree did not have — a comparison strict enough to mean "byte-identical", a
fleet the replay can run against, and a way to make that fleet reproducible
for anyone who wants to re-measure the claim.

## 1. The comparison was half a comparison

`golden-sync-test` already recorded and replayed the session in `record` and
`verify` modes. What it did *not* do was pin the request side, and its
incoming comparison could say *that* two frames differed but not *where* — a
normalized `Json::Value` inequality.

`verify` now:

- **canonicalizes before it compares**: both sides are rewritten with sorted
  keys and compact indentation, so the comparison is over the value, not over
  JsonCpp's key order or whitespace, and the canonical bytes are also hashed
  into the `OK` line (the hashes below are those);
- **diffs structurally** (`diffJson`): the union of both objects' keys, with
  an absent/absent branch, an array-length branch and a leaf branch, each
  reporting a JSON path —
  `sync-bootstrap.frames[0].json.info.person.created[0].status`, not "the
  frames differ";
- **pins the outgoing frames**: every request the replay sends is hashed and
  its byte length compared against the manifest's `direction: "out"` entries,
  so a request that grows, shrinks or reorders a key fails even when the
  response would have matched anyway;
- **declares what may differ** through `acceptedAdditions` in the manifest: a
  wire field that landed *after* the recording, with its exact value, the
  commit that landed it and the date. The replay passes only when every
  difference from the fixture is a declared addition, and fails on anything
  else. A re-recording leaves the block out entirely, so the next replay must
  match the new recording exactly.

## 2. The one drift the frozen fixture found

The first strict replay failed on exactly one path:

    sync-bootstrap.frames[0].json.info.person.created[0].status
      expected <absent>
      actual   "known"

`PersonStatus` (`services/identity/src/shared/vocabulary/person-status.hxx`,
`known`/`candidate`) entered the person projection in `f58167d7`
("f11-identity: camera-guard person rpc surface") on 2026-09-14 — six days
after the fixture's `recordedAtUtc` of 2026-09-08T05:20:26Z. The person row is
in the bootstrap projection by design and the field is read by the app, so
there were two bad answers and one good one: falsifying the fixture would have
erased the very evidence the row exists to keep, and removing the field would
have broken the app to satisfy a test. It is declared instead, dated, with its
landing commit, so the next reader sees a reviewed decision rather than an
unexplained diff.

## 3. The fleet the replay needs, and what was missing from it

The recording drives **two** sockets: `/sync` on 7025 and camera's `/media` on
7026 — the gateway-era assumption that `camera:subscribe` travels `/sync` was
simply wrong, and the harness now opens the second socket for the
`camera-subscribe` scenario. Reaching the fixture rows then surfaced a real
gap on the native path.

**Measured:** the notification leg answered `503 Notification sync
unavailable`. `argus::client::authorizeCaller` refuses an *empty* presented
credential and can only match a non-empty configured secret, and the native
configs had `sync [notifications] credential` and `notification [grpc]
caller_sync` both empty — the deploy path shared those secrets, the native
path never had. `scripts/lib/common.sh` and `scripts/setup.sh` now do, for the
three pairs the fleet needs (`sync→notification`, `guard→camera`,
`guard→notification`):

- `fill_deploy_pair` → `fill_config_pair`, now that both paths call it, with
  guards that return early when either config or either key is absent;
- the four caller keys (`camera actions_credential`, `notifications
  credential`, `grpc caller_guard`, `grpc caller_sync`) added to
  `adopt_wiring_keys`, because a config written before this change has no
  `[grpc]` table at all and a naive fill would have silently done nothing —
  caught by testing the helper against a stale copy before trusting it.

## 4. Making the green claim reproducible

A replay that only the author can run is not evidence. Three files carry the
recipe:

- **`scripts/native-stack.sh`** — boots the six request-serving services
  natively, each with its own database under `build/native-stack/`
  (gitignored), from copies of the developer's own configs with the paths made
  absolute, mDNS off, camera pointed at the repo's own `go2rtc` binary and an
  empty sandbox config, and a `certs` symlink to the repo's PKI. `up`, `down`,
  `restart`, `kill`, `status`, `logs`, `prepare` and `env`; `up` refuses a
  port it does not own and gates on each service's HTTPS `/health` only (the
  gRPC legs are exercised by the replay itself, and probing them over HTTPS
  burned 45 s per port for a `000`).
- **`scripts/seed-golden.py`** — writes the fixture rows into each owner's own
  database under the sandbox and mints the recorder session (HS256 from the
  service's own config, device hash from the fingerprint secret, token written
  0600). **This file already existed**: a gateway-era tool that seeded the
  monolithic `argus.db` plus `identity.db` from `--argus/--identity/--config`.
  That database was deleted with the gateway in Phase 3d step 1, and no
  document, script or service references the tool (`grep` over `docs/`,
  `services/`, `packages/`, `scripts/` and the READMEs), so it is replaced
  rather than kept: same fixture rows, same minting, now one database per
  owner and a sandbox directory as the single argument. It is 224 lines
  against the old 163, and the difference is the six-owner fan-out and the
  argument handling.
- **`services/sync/tests/fixtures/sync/README.md`** — what the fixtures are,
  what the replay checks, the five-command recipe, the drill subcommands and
  what a re-recording does.

## Evidence

Taken on this tree, under the build lock.

**Four green replays.** Three consecutive runs plus a cold one — `down`, then
`rm -rf build/native-stack`, `up`, `seed-golden.py`, `eval "$(native-stack.sh
env)"`, run — each 8/8 scenarios with **bit-identical canonical hashes**:

| scenario | bytes | canonical sha256 (first 12) |
|---|---|---|
| initial-info | 127 | `5927baf1e60a` |
| sync-bootstrap | 4648 | `de29ac295515` (+1 accepted addition) |
| sync-audit-log-watermark | 209 | `0f94ad17b486` |
| sync-audit-log-page | 343 | `ab3d82e448fa` |
| sync-user-audit-log-watermark | 214 | `8004d16879f9` |
| sync-user-audit-log-page | 355 | `0f02d517336d` |
| camera-subscribe | 180 | `2fcd09b4a8a1` |
| unknown-type-error | 164 | `cff4164a807b` |

The cold run printed `PASS: golden /sync contract matches fixtures`, exit 0.
The voice leg ran in the same session (`voice:start`, a binary frame,
`voice:assistant`, `voice:stop`, `voice:done`) against the compose stack's
`stt`/`tts`/`vlm`/`voice`.

**Three negative controls, each run through the promoted tooling and each
reverted afterwards.** A gate that has never failed is not known to work:

1. The declared addition's value flipped from `"known"` to `"unknown"` →
   `MISMATCH sync-bootstrap: 1 difference(s)`, at exactly that path, `FAIL`,
   exit 1.
2. A live `UPDATE camera SET name='Golden Cam X'` in camera's **own** database
   → `MISMATCH … camera.created[0].name`, `expected "Golden Cam"` / `actual
   "Golden Cam X"`, `FAIL`, exit 1.
3. An outgoing pin's `byteLength` lowered by one → `MISMATCH sync-bootstrap:
   request bytes drifted`, naming `expected: 1575 bytes` against `actual: 1576
   bytes` with the hashes equal — the request-side pin answers on its own.

After the reverts, a fourth run printed the same eight hashes and `PASS`.

**Every pin reproduces from the committed bytes.** A reader with only the
fixtures and the manifest can re-derive all sixteen: hashing each `.raw.json`
frame's `raw` text and each fixture's `request` text and comparing against the
manifest's `sha256`/`byteLength` gives 7/7 outgoing and 9/9 incoming text
frames, 0 mismatched. Nothing in the manifest was typed by hand.

**Gates.** `check-comments.sh`: 1402 files checked, 0 comments (the two new
scripts and the README are comment-free; the README is prose).
`check-deps.sh`: 135 declarations, 911 edges, 0 forbidden, 0 cycles, 0
unresolved. `build-all.sh dev --only sync`: 0 warnings, 49/49 ctest passed —
`golden-sync-test` among them, skipping without `ARGUS_TEST_REFRESH_TOKEN` and
exiting 0, which is what keeps ctest green on a machine with no fleet.

## Two defects in the tooling itself, found by measuring rather than reading

- `native-stack.sh env` printed bare `ARGUS_TEST_…=…` assignments, so the
  documented `eval "$(… env)"` set shell variables that never reached the
  harness: it fell back to its compiled defaults, sent `PATCH
  /auth/refresh-token` to **sync** instead of auth, and reported `404 Path not
  found` as a `SKIP`. The give-away was `curl` answering 200 to the same
  request. It now prints `export` lines and the token value inline, and mints
  a fresh session first, because a replay consumes the refresh token it uses.
- The launcher `( cd … && setsid nohup … & )` left **one subshell per
  service** alive holding the script's stdout, so `up | tail` never saw EOF
  and the command appeared to hang with the fleet already healthy. It now
  `exec`s `setsid --fork` inside the subshell, and `down` waits for each
  service's drain instead of sleeping a second.

## Deliberate choices

- **`camera:closed` is excluded from the normalized comparison** — the go2rtc
  relay emits it on its own retry schedule, so its arrival is
  timing-dependent. It stays in the raw fixtures. One green run collected two
  `camera-subscribe` frames and the next one, proving the exclusion is what
  makes that scenario stable rather than luck.
- **`verify` is env-driven, not manifest-driven.** The manifest's recorded
  `baseUrl` is the gateway-era 7024; the replay takes its endpoints from the
  environment, which is also what lets the sandbox move the fleet without a
  re-recording.
- **The sandbox keeps mDNS off** and reaches every service by its configured
  port. The mDNS-discovered path is step 6's subject, not this one's.

## Files

| File | |
|---|---|
| `services/sync/tests/e2e/golden-sync-test.cc` | canonical compare, structural diff, request pins, `acceptedAdditions`, two sockets |
| `services/sync/tests/fixtures/sync/manifest.json` | `acceptedAdditions` block + its `normalization` line (12 insertions, 1 deletion; the fixtures themselves are untouched) |
| `services/sync/tests/fixtures/sync/README.md` | new — what the fixtures are and how to replay them |
| `scripts/native-stack.sh` | new — the sandbox fleet |
| `scripts/seed-golden.py` | rewritten — the fixture rows, one database per owner |
| `scripts/lib/common.sh`, `scripts/setup.sh` | the native path's shared caller secrets |
