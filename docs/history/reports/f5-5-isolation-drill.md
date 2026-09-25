# Phase 5 step 5 — the isolation drill

Plan row: `docs/history/plans/architecture-plan.md` "5 | Isolation drill:
killing an AI service does not take down `auth`, `sync` or `camera`".

## What the row asks for

One claim — the AI tier can die without taking `auth`, `sync` or `camera`
with it — and the sentence has three parts that have to be measured
separately:

1. the three keep serving, at the latency and with the answers they had
   before the tier went;
2. the legs that *do* reach an AI service refuse **in their own words**,
   bounded, instead of hanging until the client gives up;
3. the refusal is a refusal, not a casualty: the socket that asked stays
   open and keeps answering, and the process that answered it is the same
   process afterwards.

The row says "an AI service"; the drill takes the whole tier down at once,
because all of it gone is the hardest form of the same claim and the only
form whose blast radius is the full set of AI clients.

## 1. The spine, as the code states it

**Isolation is structural, not defensive.** Every service is its own
process, owns its own SQLite file (root rule 27) and terminates TLS on its
own listener; the AI capacity is reached only through
`argus::clients::{llm,stt,tts,vlm,voice}` (rule 25's tier 3) and never
in-process. There is no shared connection, no shared database and no
background thread of an AI service living inside the trio — so "killing an
AI service" can only reach the trio through the two legs that call one, or
through a bug in how a refusal is handled.

**The camera's speech leg.** `CameraControlFeatureService::speak`
(`camera-control-feature-service.cc:79` in
`services/camera/src/feature/camera-control/services/`)
synthesizes through `TtsClient` inside a `BlockingTask`, and catches
`std::exception` into `DriverResult::failure("Text-to-speech unavailable: " +
e.what())`. The words come from the client that failed:
`packages/clients/tts/src/tts/tts-remote.cc:382` throws
`argus-tts unreachable at <baseUrl>`. The route answers the camera's own
envelope — 502 `CAMERA_UNREACHABLE` — because the camera never dialled a
camera: the refusal is about the speech leg it could not reach.

**The voice leg.** `VoiceGrpcRelay::forwardText`
(`services/sync/src/feature/transport/infra/voice-grpc-relay.cc:184`) answers
`voice:start` by probing the gRPC channel —
`BlockingTask{client_->waitConnected(kConnectProbeTimeoutMs)}`, 500 ms
(`:19`) — and throws `ResponseException(503, SyncErrors::VoiceUnavailable)`
when the probe fails (`:216`). `SyncSocket::handleNewMessage`'s catch
(`services/sync/src/feature/transport/controllers/sync-socket.cc:51`)
converts it into a per-frame answer through `sendSocketFrameError`
(`packages/contracts/sync/src/sync/sync-forwarder.hxx:25`):
`{type:"voice:start_error", status:503, error:"Voice unavailable"}`. A
frame is refused; the connection is not.

**The reading the outage has to leave alone.** `/camera/1/status` answers
502 `CAMERA_UNREACHABLE` in this sandbox both before and after the outage —
the seed leaves the camera's credential columns empty, so the camera is
never dialled and the answer is the server's own `no credentials
configured`. That is exactly what makes it a good witness: the drill demands
the *same* status before and during the outage rather than a 200 it would
have to manufacture.

## 2. The instrument

`scripts/isolation-drill.py`, verbs `tier`, `talk`, `voice`, `all`, `state`.
It takes its tier from `docker ps` (image name minus the `argus-` prefix,
filtered to `AI_SERVICES`), stops those containers by id, and proves every
published port refuses before it measures anything else. Then, for each of
`auth`, `sync` and `camera`: `/health`, the unit's own read route with the
reading it had before, and the latency bound. Then the sync socket:
`sync_audit_log {findLast:true}` must answer `operation=2` with a positive
integer `watermarkId` (the `option='user'` field is the request's own
default — no audit emitter sets it — so it is printed, never asserted), the
process must be the same pid afterwards, the frozen `/sync` replay must print
its `PASS:` line, the HTTP contract census must print the **same tally as the
control run before the outage**, and an audited logout must still commit on
its first publish, reach `sync.user_action_log` exactly once and store no
duplicate message id. Then the two AI-touching legs, each against a control
taken while the tier was still up, and a liveness check after each socket
that asked — the socket that hangs up is the one that used to kill `sync`
(§4), so every hang-up is followed by `/health` and a pid comparison.

The instrument refuses to pass vacuously, and every refusal was reproduced
before it was trusted: a tier that is not running stops the drill
(`require_tier_up`), a container that publishes no port the drill can probe
is its own failure rather than an empty loop, a replay exits 0 on a `SKIP:`
line and no longer counts as a pass, a census without a tally is a failure,
the outage's census is compared against a control rather than against zero,
the pre-outage reads must be answers a live caller gets (`auth` 200,
`camera` 200/502), and a request that never got an answer fails its check
instead of crashing the drill.

## 3. Measured, on one freshly booted sandbox

Fresh boot (`scripts/native-stack.sh down`, `rm -rf build/native-stack`,
`up`, `scripts/seed-golden.py`), 2026-09-25, one run of `python3
scripts/isolation-drill.py all`: **59 checks, 59 passed, 0 failed.**

The tier as the drill found it: `stt` on 7030, `tts` on 7029, `vlm` on
7031, `voice` on 7034-7035 — five published ports on four containers, every
one of them answering before the stop and refusing after it.

Before the outage: the census 326 probes / 0 failing / 0 stale / 0
unverified; `auth` `/health` 200 in 7 ms, `/auth/status` 200 in 7 ms;
`sync` `/health` 200 in 7 ms; `camera` `/health` 200 in 6 ms,
`/camera/1/status` 502 in 7 ms; the talk control 502 `The talk channel needs
the vendor cloud password` in **4.06 s** (the device path's own refusal,
speech not blamed); the voice control produced no refusal frame in 4.0 s and
the same socket then answered the sync contract in 2 ms; and `sync` was the
same pid [1753792] after that socket hung up.

With the tier down — all four containers stopped, and every published port
(7030, 7029, 7031, 7034, 7035) refusing connections:

| Leg | Measured |
|---|---|
| `auth` `/health` | 200 in 7 ms |
| `auth` `/auth/status` | 200 → 200 in 8 ms |
| `sync` `/health` | 200 in 6 ms |
| `camera` `/health` | 200 in 6 ms |
| `camera` `/camera/1/status` | 502 → 502 in 8 ms |
| `sync` socket contract | `operation=2 option='user' watermarkId=1` in **46 ms** |
| `sync` process | pid [1753792] → [1753792], `/health` 200 in 10 ms |
| frozen `/sync` replay | `PASS: golden /sync contract matches fixtures` |
| HTTP census | 326 / 0 / 0 / 0 — equal to the control |
| audited logout | outbox `sent` on the first publish, `attempts=1`, one `user delete` row in `sync.user_action_log`, `ip_address=''`, 0 duplicate message ids |
| talk (camera) | 502 `Text-to-speech unavailable: argus-tts unreachable at 127.0.0.1:7029` in **7 ms**, and the camera is still pid [1753611] |
| voice (sync) | `voice:start_error` 503 `Voice unavailable` in **504 ms**, twice on one socket (506 ms), the socket still answering the sync contract in 2 ms afterwards, and `sync` still pid [1753792] |

The tier came back, and the drill says so: `stt`, `tts`, `vlm`, `voice`
reachable again on every published port.

The two numbers that matter most are the talk leg's 7 ms against the
control's 4.06 s, and the voice leg's 504 ms against its 500 ms probe
window: both refusals are the server's own, taken at its own bound, not a
client timeout and not a hang. And every pid is unchanged across a socket
that connected, asked, was answered and hung up — which is the regression
guard for the defect below.

## 4. The crash the drill found

The first run of the drill **killed `argus-sync`** — and the chain of
evidence went through the drill's own failures: the contract probe got
`operation=0 option='user' watermarkId=None` (the fix for that is an
instrument defect, below), the frozen replay then printed `SKIP: could not
open /sync WebSocket`, and the census answered `GET /health unreachable:
[Errno 111] Connection refused`. The service was gone, and the last thing
it had done was answer a client that then hung up.

The trigger was isolated by hand (ask `{"type":"sync_audit_log",
"payload":{"findLast":true}}`, then close) and the death pinned with gdb to
a SIGSEGV in `SynchronizedService::syncAuditLog`
(`services/sync/src/feature/transport/services/synchronized-service.cc:307`).

**Cause.** Drogon's WebSocket context is a `shared_ptr<void>`;
`getContextRef<T>()` hands back a raw reference *into* it and
`clearContext()` resets the shared_ptr
(`WebSocketConnection.h:194-208`). `SyncService::handleDisconnect` called
`conn->clearContext()`, while a suspended `handleMessage` coroutine still
held `const auto& ctx = conn->getContextRef<JwtContext>()` across an
`co_await`. The disconnect freed the context under the suspended frame: a
use-after-free that landed as a SIGSEGV the moment the coroutine resumed.

**Fix.** `handleDisconnect` no longer clears the context
(`services/sync/src/feature/transport/services/sync-service.cc:107`) — the
connection object dies with the socket, so the reset bought nothing and
cost the process. The context is set once per connection before any message
can be dispatched and is never replaced, so the reference a suspended frame
holds stays valid for the connection's lifetime; no first-party code resets
a WS context any more. `sync` rebuilt at 49/49 tests, 0 warnings; the
frozen replay passes end to end.

**Verified** by the drill's own liveness checks, which now fail if the
process that answered is not the process that kept serving: `sync`'s pid is
compared after the control socket's hang-up, after the contract probe and
after the drill socket's hang-up, and `camera`'s after the refused talk —
four checks, all green in the run above. The pattern itself survives at two
sites (`sync-service.cc:14` and `:72`, both binding a context reference
before an `co_await`); it is safe only while nothing clears or replaces the
context, so a future change that re-mints one has to re-read it after the
suspension instead.

This is a defect the step-2 and step-3 instruments would only have caught
by accident: a client that disconnects mid-request is not something the
recorded fixtures do.

## 5. Defects found while building the instrument

Twelve, each of them a way the row's claim could have been *stated* without
being measured. The first seven were found by building the drill; the rest
by an adversarial read of the instrument and of this report before the
commit.

1. **The contract probe matched the wrong frame.** The socket's first frame
   is `InitialInfo` (`operation=0`); the predicate accepted any JSON object,
   so the check read the greeting as the answer. Now the shared
   `audit_answer` predicate requires `operation == 2`, no `status` field and
   a positive integer `watermarkId`, and the failure detail names what
   arrived.
2. **The replay passed on `SKIP:`.** `run_replay` checked only the exit
   code, and the replay exits 0 when it cannot open the socket at all —
   which is how a dead `sync` produced `ok the frozen /sync replay still
   passes with the tier down`. `replay_passed` now requires the `PASS:`
   line.
3. **The census had no control.** The first run blamed the tier for a red
   census that was really the crash above. The census now runs once before
   the stop, must be green, and the outage's census must print the *same
   tally* — a red control is reported as a red control and explicitly not
   attributed to the outage.
4. **The tier's "up" half was assumed.** The drill proved the ports
   refused after the stop but never that they answered before it. It now
   probes every published port first.
5. **The voice control's second check could not fail.** It required only
   that *a* frame had arrived, and with the tier up the only frame is
   `InitialInfo` — so it passed on a socket that had silently swallowed
   `voice:start`. It now asserts what a control is for: the same socket
   still serves the sync contract after the request.
6. **Dead tokens after the census.** `golden-http.py verify` clears the
   seeded sessions in its `finally`, so every header minted before a census
   is worthless after it — in the `all` path the talk and voice legs would
   have run with a 401 and reported a false failure. The drill re-mints
   them after the census.
7. **The step-4 instrument crashed on the step-5 drill.** `assert_one_row`
   formatted `len(arrived)` before testing it, so the drill died with
   `TypeError: object of type 'NoneType' has no len()` instead of reporting
   a row that never arrived — a failure that reads as a tooling crash, not
   as a durability defect.
8. **Nothing checked that a process survived the hang-up.** The drill's
   loudest failure mode — a crashed `sync` after a socket closed — could
   have printed `59/59 green` if the crash happened after the last socket
   check. Every socket close is now followed by `/health` and a pid
   comparison (`hold_after`), which is the regression guard for §4.
9. **The audited-write check could not fail.** `logout_action` only returns
   once an outbox row is `sent`, so re-asserting `status == "sent"` was a
   tautology. It now asserts what the wait does not: `attempts == 1` (the
   producer did not retry — the write path ends at the broker's PubAck) and
   the row's message-id shape (`auth-action:`). The same tautology sat in
   two checks of step 4's `durability-drill.py` (`drill_outage` and
   `drill_frozen`); they got the same predicate, and that drill was re-run
   with it — **79 checks, 79 passed** on this sandbox after the isolation
   run, with the frozen axis redelivering at 60.1 s and the broker axis
   draining in order, each action once, `num_ack_pending: 0`.
10. **`option == "user"` was asserted as if it meant something.** The socket
    DTO defaults `option` to `TableName::User` and no audit emitter sets
    it, so the conjunct was true by construction. It is now printed in the
    detail and asserted nowhere.
11. **The pre-outage reading was compared only with itself.** If the
    "before" read had been a 401 or a 404, the outage's identical answer
    would have passed. The baseline now requires an answer a live caller
    gets (`auth` 200, `camera` 200/502) before the outage repeats it.
12. **A container that publishes nothing would have passed.** The port
    parser matched only `127.0.0.1:…->`, so a wildcard or registry-prefixed
    publish parsed as an empty list — and `all()` over an empty set is
    `True`. Parsing is host-aware now, wildcards resolve to `127.0.0.1`,
    ranges expand, and "every AI container publishes a port the drill can
    probe" is its own check.

Four smaller repairs in the same round: the tier is marked stopped *before*
`docker stop` and the stop moved inside the `try`, so a docker failure can no
longer leave the AI tier down; a request to a service that is not there
(`SystemExit` from the HTTP helper, a socket that will not upgrade) now fails
its own check instead of aborting the drill; `assert_one_row`'s wording
("reached the audit log after the restart") is neutral, because the isolation
drill restarts nothing; and `measured["health"]` — stored and never read —
is gone.

Two corrections to the draft of this report, from the same adversarial read:
the wedged durable is not step 4's (its report and the ledger record no such
behaviour, and a broker restart drains cleanly in the re-run above), and the
crash was pinned once with gdb rather than "three times" — the count had no
artifact and the pid checks are the durable evidence.

## 6. What the drill deliberately does not assert

- **`argus-llm` is not in this sandbox.** The tier is taken from `docker
  ps`, so the measured tier is the four AI containers that were running
  (`stt`, `tts`, `vlm`, `voice`); an AI service that is already stopped is
  not part of the measurement, and an empty tier stops the drill rather
  than passing.
- **Recovery quality.** It proves the tier answers again on every published
  port, not that a synthesis or a transcription is correct after the
  restart — that is the AI services' own suites.
- **Partial outages.** One AI service down is not drilled: the claim is
  about the tier's absence, and the legs' refusal paths do not depend on
  which AI service went.
- **The frontend.** The app painting persisted data first and then reacting
  to live sync is the frontend's claim, measured there; the drill's
  contribution is that the backend answers throughout.
- **The durable's re-attach under a broker restart.** Step 4's `broker`
  axis covers it and it drains cleanly there (the queue waits, then drains
  in order on the reconnected consumer); it is not re-measured here.
- **`FATAL` log lines.** `trantor`'s `LOG_SYSERR` builds a kFatal-level
  logger and does not abort (`Logger.cc:235-247`), so the
  `Transport endpoint is not connected (errno=107) … shutdownWrite` lines a
  peer's close produces are noise, not a signal. The drill reads processes
  and ports, never log levels.

## Gates

`check-comments` 1418 files checked / 0 comments (the drill is the tree's new
file); `check-deps` 135 declarations, 911 edges, 0 forbidden, 0 cycles,
0 unresolved; `check-routes` 75 declarations over 12 units, 56 behind
`JwtFilter`, 2 multipart, 0 added / 0 removed / 0 forbidden; and
`build-all.sh dev` exit 0 — 15 projects, 458 ctest cases, 0 failed, 0
`warning:` / 0 `error:` on a first-party path, `check-tidy` at 553 TUs /
2875 findings over 45 checks against the 2901 baseline (9 checks below it,
identical to step 4: the only C++ line this step removes carried no finding
of its own, so the counts do not move). The review round that this report
closes is Python-only, so those C++ numbers stand; only `check-comments`
was re-run over it.

## How to run it

```bash
# the AI tier, then the sandbox
docker compose -f argus-deploy/docker-compose.yml up -d argus-tts argus-stt \
    argus-vlm argus-voice
./scripts/native-stack.sh up
python3 scripts/seed-golden.py --stack-dir build/native-stack

python3 scripts/isolation-drill.py all        # 59 checks
python3 scripts/isolation-drill.py state      # what is up, without drilling
```

`all` leaves the tier running (`start_tier` in a `finally`), and the drill
refuses to start at all when no AI container is up, because a drill that
has nothing to take down measures nothing.

## Files

| File | Role |
|---|---|
| `scripts/isolation-drill.py` | the drill: tier discovery, sync socket client, the four legs, the liveness checks |
| `services/sync/src/feature/transport/services/sync-service.cc` | the crash fix (`handleDisconnect`) |
| `scripts/durability-drill.py` | `assert_one_row`'s crash on a missing row; `first_try_ack`, the strengthened acknowledgment checks |
| `docs/history/reports/f5-3-golden-http-contracts.md` | the census this drill re-runs as a control |
| `docs/history/reports/f5-4-durability-drills.md` | the sandbox, the golden seed, the four durability axes |
