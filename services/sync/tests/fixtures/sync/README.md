# Frozen `/sync` golden frames

These fixtures are a byte-level recording of one session against a real
Argus fleet: the `/sync` WebSocket and the camera `/media` socket. They were
captured on 2026-09-08 (`recordedAtUtc` in `manifest.json`) and pin Phase 5
step 2 of `docs/history/plans/architecture-plan.md`: the wire the mobile app
already speaks must not move.

## What the replay checks

`services/sync/tests/e2e/golden-sync-test.cc` drives the same frames against
a running stack and compares two things:

- **Incoming frames** — each response is normalized (ids, timestamps,
  secrets masked, `camera:closed` dropped, see the `normalization` block in
  `manifest.json`), canonicalized (sorted keys, compact) and compared with
  the fixture *structurally*, not textually. A difference is reported with
  its JSON path and fails the run.
- **Outgoing frames** — every request the harness sends is hashed and its
  byte length compared with the `direction: "out"` pins in the manifest, so
  the request side cannot drift either.

The run fails on any difference that `manifest.acceptedAdditions` does not
declare. An accepted addition is a wire field that landed *after* the
recording: it names the exact path, the exact value, the commit and the
date. Today there is one — `person.status`, added by `f58167d7` on
2026-09-14. Adding a second entry is a deliberate act with a reviewable
reason; a re-recording clears the block by construction.

## Reproducing the run

```bash
# 1. Build the services and the harness.
./scripts/build-all.sh dev --only sync

# 2. Boot a sandbox fleet: six native services, each with its own
#    databases under build/native-stack/ (gitignored). The compose stack
#    must be up for nats, rustfs and the voice/stt/tts/vlm legs.
./scripts/native-stack.sh up

# 3. Seed the fixture rows and mint a recorder session.
python3 scripts/seed-golden.py

# 4. Point the harness at the sandbox (this also mints a fresh session,
#    because a replay consumes the refresh token it uses).
eval "$(./scripts/native-stack.sh env)"

# 5. Replay.
services/sync/build/dev/tests/golden-sync-test verify
```

In credential identity mode (the default) the replay presents the owner's
device credential on the refresh call and on both WebSocket upgrades:
`native-stack.sh env` exports it as `ARGUS_TEST_DEVICE_CREDENTIAL` beside the
refresh token. Without it argus-auth refuses the session and the replay fails.

The replay skips in one case only: no `ARGUS_TEST_REFRESH_TOKEN`, that is no
sandbox at all. It then prints a `SKIPPED:` line and exits 77, which ctest
reports as Skipped (`SKIP_RETURN_CODE`, label `e2e`). Once the token is set,
an unreachable backend, a refused session, a malformed URL or a socket that
does not open is a `FAIL:` and exit 1.

State of the recording: it predates the heartbeat frame (`Heartbeat = 11`,
pushed after `InitialInfo`), which the replay reads as one frame too many in
every later scenario, so a replay today reports differences from the second
scenario on until the set is re-recorded. The first scenario differs by one
field only, the user's `context` (`InitialInfo.context`, the role, its
capabilities and the module list, plus the Owner's `ownerCatalog` whose host
measurements the normalization drops).

A passing run prints one `OK <scenario>` line per scenario and ends with
`PASS: golden /sync contract matches fixtures`. Eight scenarios: the initial
info frame, the sync bootstrap, both audit-log and both user-audit-log legs,
the camera media subscription and the unknown-type error.

`native-stack.sh` also carries the tools the durability and isolation drills
need: `kill <service>` stops one service, `restart <service>` brings it
back, `logs <service>` tails its log, `status` shows the fleet and `down`
stops everything the sandbox started.

## Skipping

Without `ARGUS_TEST_REFRESH_TOKEN` the harness prints `SKIP:` and exits 0,
so the suite stays green on a machine with no fleet running. `ctest`
therefore never fails for a missing backend — only for a contract that
moved.

## Recording a new set

```bash
services/sync/build/dev/tests/golden-sync-test record
```

`record` rewrites every fixture and the manifest from the live stack,
including the request pins, and leaves `acceptedAdditions` out of the new
manifest — so the next replay must match the new recording exactly. Record
only against a stack seeded by `scripts/seed-golden.py`: the fixture rows
are what the normalized comparison expects.
