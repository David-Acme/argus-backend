# Phase 5 step 3 — the golden HTTP contracts, route by route

Plan row: `docs/history/plans/architecture-plan.md` "3 | Golden HTTP contracts
per route: exact envelope, 404 vs 502 `CAMERA_UNREACHABLE`, multipart intact,
filter matrix identical route by route".

## What the row asks for

Four claims, each of which has to hold **on every route** rather than on the
routes someone happened to try:

1. the body is exactly the `{status, info, errors}` envelope;
2. a missing row answers 404, and a camera the server cannot reach answers 502
   `CAMERA_UNREACHABLE` — not each other, not a 500;
3. a multipart body still reaches its handler with `ValidJsonFilter` in the
   chain;
4. the filter matrix is identical route by route: the same request with no
   token, a foreign device, a wrong role and a malformed body answers what the
   route's own filter chain says it should.

Answering them needed two instruments the tree did not have: a **static
census** that knows every route and its filters, and a **recorded replay**
that turns every route into a set of concrete requests and pins the answers.

## 1. The census: what routes exist, and which filters guard them

`scripts/check-routes.sh` + `scripts/lib/route_scan.py` read the controller
macros that declare a route — those of the eleven services and the one package
(`packages/lib/http`'s `/health`) — and compare them against
`scripts/lib/route-baseline.txt` (75 tab-separated declarations, one per
route, with method, filters, multipart and the file that declares it). The
scan enforces the filter order of rule 5, that `RoleFilter` never appears
without `JwtFilter`, that a multipart route never declares
`ValidJsonFilter`, and that the baseline and the tree agree in both
directions.

It also refuses to report a census it cannot complete: an occurrence of any of
Drogon's regex registration forms (`ADD_METHOD_VIA_REGEX`,
`WS_ADD_PATH_VIA_REGEX`, `registerHttpControllerViaRegex`,
`registerWebSocketControllerRegex`, `registerHandlerViaRegex`) aborts the scan
with `unclassified registration` and exit 1 instead of leaving those routes
invisible to the gate. No file in the tree uses one today, so the guard is a
tripwire for the day one appears rather than a description of the tree.

**Measured:** 75 declarations over 12 units, 56 behind `JwtFilter`, 2
multipart, 0 added, 0 removed, 0 forbidden. Both negative controls fail
precisely — deleting a baseline row reports `1 added` (the tree declares a
route the baseline lost), adding a bogus row reports `1 removed` (the baseline
declares a route no file has), each exit 1. A third control, an
`ADD_METHOD_VIA_REGEX` line dropped into a scanned `.cc`, reports

    check-routes: unclassified registration  <file>: ADD_METHOD_VIA_REGEX
    registers routes through a pattern this census cannot spell, so its routes
    would be invisible here

with exit 1 — the scan aborts rather than reporting a 75-row census that has
silently stopped covering the tree.

## 2. The replay: 326 probes over the eight booted units

`scripts/golden-http.py record|verify` turns each censused route into the
probes its own filter chain admits (a `no-token` probe only where `JwtFilter`
guards it, a `bad-json` probe only where `ValidJsonFilter` does, a role probe
per non-owner role where `RoleFilter` does), sends them at a live fleet and
stores the normalized answers in `scripts/fixtures/http/`.

**Measured:** `record: 326 probes over 8 units, 0 violations, 4 units
skipped`; per unit — camera 83, productivity 89, guard 55, identity 48,
notification 23, auth 21, shared 6, sync 1. The eight units are the seven
booted services plus the synthetic `shared` bucket, which is the one bucket
that is not a service: the census attributes `/health` to `packages/lib/http`,
so the replay expands that single row across the booted units that do not
declare a `/health` of their own (`plain-auth`, `plain-camera`, …), turning
one censused route into six recordings.

`census` plans against every unit the census declares and prints its own
counts:

    camera 83  productivity 89  guard 55  identity 48  notification 23
    auth 21  llm 3  stt 2  tts 5  vlm 2  sync 1  shared 1
    census: 333 probes over 12 units

321 of those are planned for the eight booted units, and 326 are recorded: the
five extra are the `shared` row, which the census counts once as a unit and a
recording expands to six probes.

65 of the census's 75 declarations carry at least one probe; the ten without
one belong to `llm`, `stt`, `tts` and `vlm` — built, since
`scripts/build-all.sh dev` builds all fifteen projects, but absent from the
sandbox roster, so the harness skips them by name rather than silently;
`scripts/fixtures/http/README.md` records what that leaves uncovered.

The answers split: 200 ×33, 204 ×1, 400 ×31, 401 ×109, 403 ×59, 404 ×33,
405 ×2, 409 ×1, 422 ×49, 502 ×8.

## 3. The four claims, each measured

| Claim | Measurement |
|---|---|
| exact envelope | every JSON answer is exactly `{status, info, errors}` — 0 violations over 326 probes, one declared deviation (§4) |
| 404 vs 502 | the `owner-missing` probes answer 404 ×11 / 422 ×4. The four are body-addressed: the entity a path id would name is named in the JSON body there (`PATCH /camera/{1}/preset` wants `id`, `/ptz` wants `x`/`y`, `/project-member/{1}` wants `access`, `POST /camera/{1}/talk` wants `text`), the probe sends `{}`, and the DTO refuses before a lookup can miss — 422 with the field's own message. All 8 5xx in the whole run are camera-control answers, and all 8 carry `CAMERA_UNREACHABLE` |
| multipart intact | `POST /auth/login` and `POST /auth/register` answer 422 (empty), 401 (unrecognized face) and 409 — never 400, so `ValidJsonFilter` never saw them |
| filter matrix | 54 probes with no token answer 401, and 54 more answer 401 while carrying the owner's token from another device's fingerprint; 31 malformed bodies answer 400, 59 role refusals answer 403, the 2 WebSocket GETs answer 405 |

The multipart probes send multipart bodies on purpose — a deterministic
128×128 gradient PNG and an empty part — so the claim is about the filter
chain and not about a probe shaped like the JSON one beside it.

The 502's body carries the layer's own message, which is what rule 6 asks
for:

    {"errors": {"code": "CAMERA_UNREACHABLE",
                "message": "no credentials configured"},
     "info": null, "status": 502}

The prose is the server's, not the camera's: it is built in this tree, at
`services/camera/src/shared/services/tapo/tapo-client.cc:85`, when the client
was constructed with no credential candidate at all. A candidate is built only
from a non-empty credential (`camera-driver/tapo-driver.cc:20` and `:24`), and
the seeded camera row carries an empty `password` and an empty
`cloud_password`, so the list is empty and the failure is local: the camera is
never dialled. A reachable camera that refused its credentials would answer
the same code with the attempt's own error text.

## 4. The one declared deviation

Guard serves `/health` from its own handler in
`services/guard/src/app/main.cc` (`{"status":"ok","service":"argus-guard"}`)
and never registers the shared
`HealthController`; every other unit answers the envelope. The manifest
declares it once, with its reason, so the deviation is a reviewed entry rather
than a hole in the invariant.

The census is what surfaced the duplicate: it attributes `/health` to
`packages/lib/http`, so the replay expanded that one row across every booted
unit — including guard, which declares its own `/health` in the census.
Expansion now skips a unit that declares the row itself, and guard's `/health`
is probed once, as guard's route.

## 5. The one field that is an observation, not a contract

A later run of the replay, on a stack that had been up for a while, failed on
three probes of a single route:

    drift: notification GET /notification/delivery-summary [role-resident]
      response.json.info.latencyMsP50: 0 -> 5
      response.json.info.probeOk: False -> True

`/notification/delivery-summary` reports the push channel's **last self-test**
— `probeOk`, `probeMs` and three latency percentiles — and that test runs on
its own schedule, so a stack that has not probed yet answers `0`/`false` and a
stack that has probed answers a measurement. The fixture had recorded the
first state, which is why it passed on a fresh stack and failed on a warm one:
it was pinning a stopwatch.

The manifest now carries `volatileFields`: one entry, listing those five
fields for that route with its reason, and the comparison drops them from both
sides. The counters beside them (`acked`, `pending`, `sent`, `unacked`,
`unackedOld`) stay pinned, so the route's contract is still checked — only the
observation is exempt, and the exemption is visible in the manifest rather
than buried in the harness.

## 6. What a run writes

Measured across **every table of all seven services' databases**, comparing a
fresh stack before and after a full `record`.
`scripts/db-snapshot.py` walks `sqlite_master` and digests each table's rows,
so a table added later is covered without editing the instrument, and the
measurement is reproducible:

```bash
python3 scripts/db-snapshot.py --stack-dir build/native-stack --out /tmp/before.json
python3 scripts/golden-http.py record --stack-dir build/native-stack
python3 scripts/db-snapshot.py --stack-dir build/native-stack --out /tmp/after.json
python3 scripts/db-snapshot.py --diff /tmp/before.json /tmp/after.json
```

| Table | Effect |
|---|---|
| `auth.device_login_challenge` | 0 → 1: `POST /auth/device-login` leaves an unexpired pending challenge |
| `auth.change_outbox` | 0 → 1 `sent`: the logout's user action, published to NATS |
| `sync.user_action_log` | 0 → 1: that action's recipient audit row |
| `auth.refresh_token` | 3 → 1: the run's own sessions — logout invalidates the user's rows and the harness re-mints one |
| `identity.user` | 3 → 1: the two transient probe users, removed again by the run's own `--roles-clear` |
| `auth.sqlite_sequence`, `sync.sqlite_sequence` | the AUTOINCREMENT high-water marks of the tables above: they gain a row the first time such a table is written |

Every other table — person, camera, zone, reminder, project, notification,
invitation, the guard tables — is byte-identical before and after. The three
additive rows grow by one per run.

## 7. What the measurements taught the harness

The first four were found by running the harness; items 5 to 9 by an
adversarial read of it, each reproduced as a live failure before it was
fixed:

1. **A 204 is not "no body to check".** `PATCH /auth/logout` answers 204 with
   an empty body, and the envelope invariant had no branch for it, so the
   recorder accepted it silently. It now requires an empty body on 204.
2. **The recorder poisoned its own session.** The logout probe invalidates
   every refresh token of the recorded user, so every probe after it recorded
   401. The harness now re-mints after the routes that kill a session, and
   the re-mint exchanges each refresh token back into an **access** token —
   the first version handed the harness refresh tokens and every later request
   failed signature verification.
3. **Random material needs masking.** `POST /auth/device-login` answers a
   random `challengeId`; the normalizer now masks any 64-character hex value,
   so the fixture pins the shape and not the dice.
4. **One censused row can be probed twice on the same unit.** The census
   attributes `/health` to `packages/lib/http`, so the replay expanded that
   row across every booted unit — including guard, which declares its own
   `/health` in the census. The expansion now skips a unit that declares the
   row itself, so guard's `/health` is probed once, as guard's route (§4).
5. **A masked key has to keep the shape of what it held.** Masking used to
   substitute the string `<masked>` for the value, so a whole subtree under a
   masked key collapsed into one scalar and nothing inside it was compared.
   The mask now walks the value and masks the leaves, so
   `{"errors": {"code": …, "message": …}}` under a masked key is still
   compared field by field.
6. **The server's own refusal text is part of the contract.** Masking by key
   name also hid `errors.fields`, which is the validation message list: a
   message about `startsAt` sat under a key ending in `At` and was masked on
   both sides. The mask suffix applies outside the `errors` subtree only, so a
   validation message is pinned — and the fixtures show it never echoes what
   the client sent (`token must not be empty`, not the token).
7. **The 502 exemption was as wide as "a 5xx on a camera route".** It is now
   scoped to the camera-control routes and made conditional on the body: a 502
   there must carry `CAMERA_UNREACHABLE`, and a 5xx anywhere else is a
   failure.
8. **Three checks that could not fail.** `other-device` had no expected
   status, so a token from another device being *accepted* with a 200 would
   have been recorded and re-verified green — the fixture would have pinned
   the hole as the contract; it is now `expect: 401`. A multipart 400 was
   detected by searching the answer's message text instead of its status, so
   a 400 with any other message passed; it reads the status now. And a
   boolean compared equal to a number, because Python's `==` says `True == 1`,
   so the comparison checks types before values.
9. **A partial fleet must not look green.** `verify` used to skip a unit it
   had recordings for but could not reach, and report success; it now counts
   those probes as `unverified`, prints them and fails. `record` refuses to
   run at all when a unit it has recordings for is absent (the recordings
   would silently shrink), and when a recording's unit is not in the census
   any more. The census's planning had a defect of the same kind: it planned
   with no id table, so it counted `owner-missing` probes for routes whose id
   is not a path slot at all — probes that no run sends and that inflated its
   headline count. It now plans with the fixture ids a run uses, which is why
   its 333 and the run's 326 reconcile (§2).

## 8. Coupling with step 2

The frozen `/sync` replay still passes byte-identically after a full HTTP
replay has run against the same fleet: `ctest -R golden-sync` → `Passed`, with
`PASS: golden /sync contract matches fixtures` and the manifest's one declared
`person.status` addition. The reason the two do not interfere is measurable:
the HTTP replay's only sync-side write is `sync.user_action_log`, and that
table appears zero times in the bootstrap request the sync fixture records.

## Evidence

Taken on this tree, under the build lock.

**Green pair.** `record: 326 probes over 8 units, 0 violations`; then, on a
second fresh stack, `verify: 326 probes checked, 0 failing, 0 stale, 0 probes
unverified`. Repeated after §5's change, after the manifest block landed and
after §7's nine harness fixes, each on its own fresh stack, with the same
numbers.

**The recorder is deterministic.** Measured twice over:

- Three recordings under the pre-`errors` masking harness, each on its own
  fresh stack, produced byte-identical **unit** fixtures: all eight files hash
  equal (`md5sum` compared) every time. `manifest.json` differed between them,
  and only as the harness's own summary grew — first the 204 invariant and the
  hex64 normalization, then the `volatileFields` block.
- Two recordings under the harness this report describes — the re-recording
  that §6 and §7 changed, and a second one on another fresh stack — are
  byte-identical in **all nine** files, `manifest.json` included, so the
  earlier differences were versions of the harness and not run-to-run noise.

`verify` leaves the fixtures untouched (hashes before and after are equal).

**Negative controls.**

1. One recorded status flipped (`camera GET /camera/{1}/capabilities [owner]`
   200 → 201) → `verify: 326 probes checked, 1 failing, 0 stale`, naming
   exactly `drift: camera GET /camera/{1}/capabilities [owner]
   response.status: 201 -> 200`. Restored, and the hash matches the pre-tamper
   one.
2. A fixture file for a unit no route mentions
   (`scripts/fixtures/http/nosuchunit.json`) → `stale: nosuchunit has
   fixtures but no route in the census`, `1 stale`, exit 1. Removed after.
3. The census, in both directions (§1), plus the regex tripwire (§1) and the
   seed guard: on a stack without fixture rows, `record` stops with the list
   of absent slots and the command that would seed them, instead of recording
   401s.
4. A unit with recordings that the run cannot reach. Stopping camera and
   verifying with its config still in place stops the run with `POST /camera
   unreachable: [Errno 111] Connection refused`, exit 1 — and taking camera
   out of the roster altogether makes the harness's own seed step refuse
   (`missing …/camera/config.toml; run scripts/native-stack.sh up`, exit 1).
   Calling `verify` with camera's base withheld, so the branch itself runs,
   reports

       stale: shared GET /health [plain-camera] no longer in the census
       unverified: camera is not booted, so its 83 recordings were not checked
       verify: 242 probes checked, 0 failing, 1 stale, 83 probes unverified

   and exits 1 — where the same run without those two counts used to report a
   clean pass. The `stale` line is the shared bucket's own consequence: its
   `/health` recording for a camera base no longer has a probe in this run's
   plan, so it is named rather than silently kept.
5. The two normalization defects of §7, each reproduced before it was fixed.
   The shape collapse: with masking on, a masked key holding
   `{"token": …, "expiresAt": …}` and one holding
   `{"somethingElse": {"deep": […]}}` both normalized to the string
   `"<masked>"`, so a structural change inside a masked key compared equal to
   no change at all; they differ now, while a masked object still compares
   equal to itself. The masked `errors.fields`: a fixture whose `startsAt`
   validation message had been changed still passed, and fails now.

**The warm-stack failure reproduced and then disappeared** (§5): the run that
found the self-test drift reported `3 failing` on a warm stack; after the
`volatileFields` entry landed, the same stack, the same fixtures and the same
command reported `326 probes checked, 0 failing, 0 stale`. A later run against
the long-lived sandbox stack reported those same numbers and left the fixtures
byte-identical.

Two limits on that evidence, measured rather than assumed. The fields move
only once the self-test has *written* its row: `notification_selftest` holds
one row written by a successful probe (`last_at`, `last_ok`, `last_ms`), a
failed one logs and writes nothing, and the prober only starts at all when a
delivery sink is installed (`runAfter(0.0)` then every
`notifications.selftest_interval_s`, 300 s by default). On the sandbox stack
the table is empty, so that stack answers `0`/`false` on every run and a
replay against it exercises the exemption's absence, not its effect. The stack
that produced the drift above
was one where the probe had succeeded — where the fixture, and any run against
it, would otherwise have pinned `probeOk: true` and a latency that changes
every 300 s.

**Gates.** `check-comments.sh`: 1416 files checked, 0 comments.
`check-deps.sh`: 135 declarations, 911 edges, 0 forbidden, 0 cycles, 0
unresolved. `check-routes.sh`: §1, and it now runs from `build-all.sh` before
anything is built. `build-all.sh dev`: exit 0, every project's ctest green
(458 registered cases), and `check-tidy` at 553 translation units / 2875
findings over 45 checks against the 2901 baseline, 9 checks below it. The
tidy numbers repeat step 2's — the file list says why: this step's diff
touches no `.cc` or `.hxx` file, so no first-party translation unit can have
moved. The comment count does grow, from 1402 at the end of step 2 to 1416
here, which is exactly the fourteen files the scanner reads and this step
adds: five scripts and the nine fixture JSON files, each comment-free (the
fixtures' README and this report are Markdown, which the scanner skips by
suffix).

## Findings outside the row

- **A lock taken around a stack run outlives the run.** `flock FILE
  native-stack.sh up` hands the lock's descriptor to every service it starts,
  and `flock` keeps a lock alive for as long as any process holds its file
  description — so the lock stayed held after the drill that took it had
  exited, and the *next* command waiting for it never ran. Found by measuring:
  the second drill sat for three minutes with an empty log while the services
  from the first still held the lock. The sandbox's `usage()` now says to wrap
  a stack run in `flock -o`, which keeps the lock in the `flock` process
  instead of in the services.
- **`services/guard/src/app/main.cc` was reachable only with the fleet's
  models present.** Booting identity without a `models/` directory in its
  working directory logs `FaceService: failed to load detector model` and then
  either aborts or wedges before listening, ignoring SIGTERM — which reads as
  a durability bug and is not one. The sandbox now symlinks the repo's
  `models/` into identity's directory, and the shutdown is clean.
- **`AGENTS.md` carried a stale line reference** for guard's `/health`
  (`main.cc:103-115`, actually 60-72). Corrected.
- **`argus-deploy/config.gateway.toml` is an orphan**: a gitignored, 0600
  instance config for a service deleted in Phase 3d step 1. It is not tracked
  and not shipped; it is left in place rather than deleted from under its
  owner.

## Files

| File | |
|---|---|
| `scripts/check-routes.sh`, `scripts/lib/route_scan.py`, `scripts/lib/route-baseline.txt` | new — the static census and its gate, including the regex-form tripwire |
| `scripts/golden-http.py` | new — the replay: record, verify, census, probe planning, normalization, invariants |
| `scripts/fixtures/http/` | new — the recorded contracts, the manifest and the recipe |
| `scripts/db-snapshot.py` | new — the write-set instrument: per-table row counts and digests, and their diff |
| `scripts/seed-golden.py` | the fixture rows, the three role sessions and the transient probe users the replay needs |
| `scripts/native-stack.sh` | guard in the roster, the identity `models` symlink, an absolute `ARGUS_STACK_DIR`, the lock note |
| `scripts/build-all.sh` | `check-routes.sh` runs before anything is built |
| `scripts/lib/comment_scan.py` | `route-baseline.txt` added to the hash-comment names |
| `packages/lib/http/AGENTS.md`, `AGENTS.md` | the guard line reference, and rule 5's measured gate |
| `docs/history/plans/architecture-plan.md` | the row this report closes |
