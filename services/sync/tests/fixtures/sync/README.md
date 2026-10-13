# Frozen `/sync` golden frames

These fixtures are a byte-level recording of one session against a real
Argus fleet: the `/sync` WebSocket and the camera `/media` socket. They were
re-recorded on 2026-10-06 (`recordedAtUtc` in `manifest.json`) and pin Phase 5
step 2 of `docs/history/plans/architecture-plan.md`: the wire the mobile app
already speaks must not move.

## What the replay checks

`services/sync/tests/e2e/golden-sync-test.cc` drives the same frames against
a running stack and compares two things:

- **Incoming frames** — each response is normalized (ids, timestamps,
  secrets and the household zone masked, `camera:closed` dropped, see the
  `normalization` block in `manifest.json`), canonicalized (sorted keys, compact) and compared with
  the fixture *structurally*, not textually. A difference is reported with
  its JSON path and fails the run.
- **Outgoing frames** — every request the harness sends is hashed and its
  byte length compared with the `direction: "out"` pins in the manifest, so
  the request side cannot drift either.

The run fails on any difference that `manifest.acceptedAdditions` does not
declare. An accepted addition is a wire field that landed *after* the
recording: it names the exact path, the exact value, the commit and the
date. Today there are none: the 2026-10-06 re-recording folded in `person.status`
(`f58167d7`, 2026-09-14). Adding an entry is a deliberate act with a reviewable
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

State of the recording: it was made on a freshly reset sandbox seeded by
`scripts/seed-golden.py`, and a replay on a freshly reset and seeded sandbox passes
all eight scenarios. A replay on a sandbox a replay already used does not: the first
one leaves notification rows behind. Beside the first recording the `initial-info`
scenario now holds the user's `context` (frame 0) and the heartbeat frame that
follows it (frame 1), and the Owner's `ownerCatalog` leaves out each module's
`hardware`, `installedBytes`, `components` and `hasData`, which the answering host
and the settings engine's own refresh schedule decide.

A passing run prints one `OK <scenario>` line per scenario and ends with
`PASS: golden /sync contract matches fixtures`. Eight scenarios: the initial
info frame, the sync bootstrap, both audit-log and both user-audit-log legs,
the camera media subscription and the unknown-type error.

`native-stack.sh` also carries the tools the durability and isolation drills
need: `kill <service>` stops one service, `restart <service>` brings it
back, `logs <service>` tails its log, `status` shows the fleet and `down`
stops everything the sandbox started.

## The remote link scenario

`info.context.remote` (the home's cloud link, `services/sync/CONTEXT.md`,
"Remote access in the context") is present only on a stack where a tunnel
client reports one, so the normalized comparison drops it from every frame's
context (`remoteField` in the manifest's `normalization` block) and the
recorded `initial-info` stays valid on a stack with or without remote access.

The field is pinned by a scenario of its own instead, `remote-link.json`. Its
expected frames were not recorded: each one is the shape the contract
(`packages/contracts/routes/src/routes/remote-link.hxx`) gives the state the
step publishes. After the eight scenarios, with the `/sync` socket still open,
the harness connects to the broker as the home's tunnel client, publishes each
step's frames on `argus.tunnel.v1.link` and reads the `ContextUpdate` frames
(operation 13, option `user`) that carry `remote`:

- `reset` publishes `down` and drains whatever argus-sync sends for 7 seconds,
  so the steps after it start outside the 5-second window;
- `up` expects one frame with the published state at once;
- `back-inside-the-window` publishes `down` and then `up` within the window and
  expects no frame, because the state is back where the last frame left it;
- `down` expects one frame;
- `trailing` publishes `connecting` and then `up` within the window and expects
  one frame with the latest state, `up`, at least 4 seconds after the previous
  one: the 5-second throttle.

It runs only when `ARGUS_TEST_NATS_URL`, `ARGUS_TEST_TUNNEL_NKEY` and
`ARGUS_TEST_TUNNEL_SEED_FILE` are all set: the broker lets only the `tunnel`
row publish that subject. The seed is `${ARGUS_DATA_DIR}/nats/seeds/tunnel.seed`
on a provisioned host, and the nkey is the public key `provision-host.sh`
derives from it. Otherwise it prints `SKIPPED remote-link:` and the run goes on
with the other scenarios and the same exit code. In `verify` a step that
differs prints a `MISMATCH remote-link` line and fails the run. In `record` it
prints its lines and writes nothing.

Point it at the sandbox broker only. Its frames reach every app connected to
that stack's argus-sync. A tunnel client running on the same stack publishes
its own state every 30 seconds with its own home id and breaks the steps, so
stop it first. The scenario takes about 20 seconds. It leaves argus-sync
holding its home id with the link `up`. Every new `InitialInfo` carries that
until a real tunnel client reports or argus-sync restarts; 90 seconds later
the link reads `down`. A later replay is not affected, because the field is
out of the normalized comparison. Unit tests that run on every build, with no
stack, cover the rest of the contract: the 90-second rule, the four checks on
a frame, the refusal log, `unknown` getting nothing and the Owner's catalog on
these frames (`services/sync/tests/unit/sync-remote-test.cc` and
`sync-context-test.cc`).

## Skipping

Without `ARGUS_TEST_REFRESH_TOKEN` the harness prints `SKIPPED:` and exits 77,
which ctest reports as Skipped, so the suite stays green on a machine with no
fleet running. `ctest` therefore never fails for a missing backend, only for
a contract that moved. The remote link scenario skips on its own (above)
without changing the exit code.

## Recording a new set

```bash
services/sync/build/dev/tests/golden-sync-test record
```

`record` rewrites every fixture and the manifest from the live stack,
including the request pins, and leaves `acceptedAdditions` out of the new
manifest — so the next replay must match the new recording exactly. Record
only against a stack seeded by `scripts/seed-golden.py`: the fixture rows
are what the normalized comparison expects.
