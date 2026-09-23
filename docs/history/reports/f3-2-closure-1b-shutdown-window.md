# 3a step 2 closure, item 1b — the shutdown window beyond the drains (S8b)

Scope: the units item 1's coverage sweep recorded as still reaching the
database from a thread no drain knows about, and the seam that now covers
them. `packages/lib/sqlite` gains the client the window is served with,
`packages/lib/runtime` the hook it is armed from, and two `camera` units gain
the drain pair item 1 asks for.

Item 1b was added by item 1's own sweep, not by the original list: item 1
narrowed the question to "a unit's own drain" and made the rest visible. This
unit answers it in two halves, because the sweep turned out to hold two
different populations.

## The window item 1 left

Item 1 gave a unit's own worker a drain: the term/int handler only requests the
stops, and Drogon quits once every registered drain reports drained. What a
drain cannot be is anything the unit does not own — and the sweep recorded
three kinds:

- **A coroutine suspended *across* the reset.** `services/sync`'s fan-out and
  the camera lease/retention sweeps run on the loop, and one of them can be
  parked inside a repository call when the reset happens.
- **A request-scoped service.** `guard`'s HTTP feature services hold no
  `LifecycleGuard`, so a handler in flight is not counted anywhere.
- **A thread nobody registered.** Those turned out to be benign one by one (see
  the table below), but the sweep could not say so before this unit measured
  them.

The crash class is item 1's, unchanged: `DbService::client()` is
`drogon::app().getDbClient()`, which reaches through `dbClientManagerPtr_`
without a guard, and `quit()` resets that manager. Every statement that arrives
afterwards is a null dereference.

The measurement that shapes the fix: for SQLite, `DbClientImpl` is
self-contained. Its `init()` calls `newConnection(nullptr)` — the loop argument
is ignored for sqlite — and each `Sqlite3Connection` runs its own
`EventLoopThread`, so a client built from `newSqlite3Client()` owns its loop and
its connection and needs nothing from the app's loops. `~DbClientManager()` is
not empty either: it calls `closeAll()` on every client it holds, and
`DbClientImpl::closeAll()` swaps `connections_` away and disconnects each one.
A caller that kept a `DbClientPtr` therefore survives the reset with a client
whose connections are gone, and `execSql` on such a client buffers the
statement into `sqlCmdBuffer_` with no ready connection to drain it — the hang
this unit had to avoid, and the reason the client it serves the window with is
built *after* the freeze rather than before it.

## The decision: a drain where a worker is owed, a client for the rest

Two different questions, answered by two different mechanisms:

| Question | Answer |
|---|---|
| Does this unit own a worker that can keep landing in the window? | `requestStop()`/`drained()` (D22), as item 1 defined it |
| Can a statement reach the database after the manager is gone? | `DbService::freezeClient(dbPath)` armed from `shutdown_signal::onQuit` (D23) |

The sweep's units, sorted by that first question:

| Unit | Own worker in the window? | Remedy |
|---|---|---|
| camera's operator frame path (`processFrame`, `rescan`) | **yes**, a frame cadence plus `camera_rescan_ms` | the drain pair; its enqueue leg is the object-event sink's own drain |
| camera's health monitor (`loadCameras`) | **yes**, `[health].interval_ms` | the drain pair |
| llm's detached stream thread and its tool chat | no database access at all | nothing owed |
| memory's background worker | its own `SqliteGraph` handle, never Drogon's manager | nothing owed |
| identity's raw-vec0 face threads | its own `VecDb` handle, same | nothing owed |
| guard's HTTP feature services | bounded by the lifetime of the request that made them | nothing owed; the freeze covers the tail |
| the sync fan-out, the lease/retention sweeps | a coroutine suspended across the reset, which no drain can report | the freeze |

`freezeClient(dbPath)` arms the second answer: the first `client()` after the
arming builds an independent `newSqlite3Client("filename=" + dbPath, 1)` that
Drogon's manager never holds, so it survives the reset with its own loop and
connection, and everything a suspended coroutine or a late handler issues lands
on it instead of on nothing. Arming itself borrows nothing and builds nothing —
the laziness is load-bearing, for the reason measured above.

The hook that arms it, `shutdown_signal::onQuit`, is the one piece of the
module item 1 did not have: it runs **after** the last drain reported drained
and **before** `drogon::app().quit()`, which is the only window in which a
service can still decide where the work to come goes. Hooks run once, in
registration order, on the loop thread, each inside the module's own catch, so
one that throws is logged and the rest still run.

## What changed

| Area | Change |
|---|---|
| `packages/lib/runtime` | `onQuit(QuitHook)` — the hooks, the mutex, and `runQuitHooks()` between the drained verdict and `quit()`; new `shutdown-quit-hook-test.cc` target; the shared `tests/unit/app-runner.hxx` |
| `packages/lib/sqlite` | `DbService::freezeClient(dbPath)` plus the frozen branch of `client()`, with the private client built lazily on first use after the arming |
| `camera` operator | `requestStop()`/`drained()` on the in-flight counter; `rescan()` and `processFrame()` hold the shared guard; `runCamera` re-checks the stop once `grab()` resumes |
| `camera` health monitor | the same pair; `loadCameras()` holds the shared guard; the destructor requests the stop |
| `camera` shared | the guard itself, `in_flight::Guard` at `src/shared/utils/in-flight/` — the duplicate the two headers carried, in the one home rule 23 gives it |
| six boot sites | `shutdown_signal::onQuit(…) { DbService::freezeClient(dbPath); }` before `run()`: camera, sync, notification, productivity, gateway (the identity database it hosts) and guard |
| `camera` main | the two new drain registrations, beside the two sinks item 1 registered |
| tests | the module's quit hook; the sqlite freeze against a booted app; the operator's and monitor's drain verdicts |
| docs | `packages/lib/runtime/AGENTS.md`, `packages/lib/sqlite/AGENTS.md`, `services/camera/CONTEXT.md`, the root `AGENTS.md` rows, and the plan's D23, §4.6, row 1b |

## Evidence

**The two real services under a real `SIGTERM`.** Both binaries were built from
this tree, booted on their own database, and terminated 4 s after they logged
`Listening on` — no test harness, the ordinary `docker stop` path:

| Probe | Log | Result |
|---|---|---|
| `argus-sync` (the freeze and nothing else: it registers no drain) | `/tmp/argus-sync-probe/sync.log` | `SIGTERM signal received.`; `Shutdown signal: stop requested for 0 drain(s)`; `SQLite: /tmp/argus-sync-probe/sync.db is armed for the work that arrives after the app's clients are reset` **50.3 ms** after the signal; exit 0 |
| `argus-camera` (freeze + drains; its probe config disables object detection and the change funnel, so the health monitor is the one drain it registers) | `/tmp/argus-camera-probe/camera.log` | `SIGTERM signal received.`; `stop requested for 1 drain(s)`; the same armed line for `/tmp/argus-camera-probe/camera.db`, **50.2 ms** after the signal; exit 0 |

The two 50 ms figures are not latency to fix — they are the poll cadence
`pollAndQuit` runs at, which is the shape D22 asked for: the signal only
requests the stops, one tick later the drained verdict is in, and the hook that
arms the freeze runs between that verdict and `quit()`. Neither probe reached
the freeze through a path the drains own, which is the whole point of the seam:
in `sync` there was no drain at all, and in `camera` the armed database is the
one the drains had just finished with.

**The frozen client is a different client, and it is built after the reset.**
`identity-client-test` boots a Drogon app on a temporary database, arms the
freeze, calls `quit()`, and waits until the app's own client reports no
available connections — the manager's `closeAll()` having run, which
`!isRunning()` alone does not prove, since `quit()` flips that flag before it
queues the teardown. Only then does it take `DbService::client()` and assert it
is a different object with live connections that read and write the same file.
This is the case that would have caught the design the unit rejected (a client
built before the freeze: connections closed under it, statements buffered and
never run).

**The hook's own contract.** `shutdown-quit-hook-test` pins what the module
promises: the hooks run after the drained verdict and before `quit()`, once
each, in registration order, on the loop thread, and a hook that throws is
logged without stopping the ones behind it.

**Drogon 1.9.13, read rather than assumed** (the version the Conan graph
pins): `HttpAppFrameworkImpl::quit()` (`lib/src/HttpAppFrameworkImpl.cc:1034-1055`)
does `running_.exchange(false)` first and only then queues the lambda that
resets `dbClientManagerPtr_` — so the manager dies on the loop after the flag
says stopped; `DbClientImpl::hasAvailableConnections()` (`orm_lib/src/DbClientImpl.cc:492-496`)
is the `const noexcept` predicate the test waits on; `~DbClientManager` →
`closeAll()` swaps `connections_` away and disconnects each one, which is the
buffering the report's decision section measures.

**What the evidence does not cover.** No test drives a statement through the
frozen client from a coroutine suspended *across* the reset — the population
the seam exists for is exercised by construction (the sync probe boots, arms
and exits) rather than by a statement landing in the window. The probes ran
with NATS absent and the AI engines disabled, so they say nothing about a
machine under inference load. And the seam's boundary is exactly as wide as
D23 records it: a caller that hoisted `client()`'s result before the arming
holds the app's client across the reset, and no lock follows a pointer already
handed out.

**The full gate.** `./scripts/build-all.sh dev` exits 0 on all 17 projects,
**425** tests, no first-party compiler warning; the module's two new suites run
where they are declared. The three orchestrator gates pass with it:
`check-comments` 1233 files and 0 comments, `check-deps` 481 edges with 0
forbidden and 0 cycles, `check-tidy` 498 TUs and 3122 findings over 45 checks
against a 3141 baseline, two checks below it and none risen. The first run of
this gate is worth recording because it failed and why: two counts had risen by
exactly one each (`modernize-use-designated-initializers` 303/302,
`modernize-use-nodiscard` 781/780), both from the test case this unit added —
positional initializers in its `Dependencies` aggregate and two getters without
`[[nodiscard]]`. The ratchet was not widened; the test was written the way the
tree is, and the operator's pre-existing `running()` gained the `[[nodiscard]]`
its own `drained()` already had, which is what puts two checks *below* baseline
rather than at it.

## The adversarial review, and what it changed

A fresh agent reviewed the unit against the code, the vendored Drogon and
trantor sources, and the plan; its findings below are its own numbering, most
severe first. Five findings were fixed in this unit, three more were found by
the same verification pass in my own work, and the rest are recorded as
boundaries rather than repaired.

| # | Finding | Verdict | Action |
|---|---|---|---|
| 1 | `runCamera()` never re-checks the stop between `grab()` resuming and `processFrame()`: three cameras parked in `grab()` read as `drained() == true`, the freeze is armed, `quit()` queues its teardown, and the resume in that same loop iteration still starts a worker that touches the database | CONFIRMED (the missing check is structural; the interleaving is probabilistic) | fixed at the call site, where the review placed it: the loop breaks on `!running_.load() \|\| stop->load()` after `grab()` and drops the frame in hand |
| 2 | `requestStop()` took `camerasMutex_`, which a worker can hold behind `stateMutex_` in a DB write or a gRPC call — so the stop was not non-blocking, and on Drogon's raw signal handler (empty `sa_mask`) a non-recursive lock already held by the landing thread deadlocks a shutdown that the module's deadline cannot fire out of | CONFIRMED (the chain; the review's claim that `rescan()`'s own query sits under that lock was false — the query is `:166-171`, the lock `:178`) | fixed: the stop is one `running_` store and nothing else, and the per-camera stop tokens stay the rescan path's own |
| 3 | `EvidenceUploader::uploadDetection` starts inside the guard but its `camera_evidence` INSERT runs after an S3 upload finishes, and the retention sweep is a `runAfter`/`runEvery` chain — neither is covered by any drain | CONFIRMED | recorded: that work is the freeze's population (D23), not a drain's, and the sink drain covers the flush worker only |
| 4 | `supervise()` has a check-then-dispatch window: a stop between its check and the worker's `rescan()` leaves fresh camera loops whose stop tokens were never signalled | CONFIRMED (benign today: each new loop re-checks `running_` and returns) | recorded |
| 5 | `start()` re-arms `running_` after a stop was already requested, so a `SIGTERM` inside the boot window starts loops against a dying loop | CONFIRMED (needs a module-level "stop already requested", which the module does not expose) | recorded |
| 6 | `CONTEXT.md` described the mechanism wrongly: the guard as "exactly the three places that query `camera.db`" (it spans the whole frame, inference included), "neither stop waits on a worker" (false for the operator), and the sink drain as covering the evidence leg | CONFIRMED | fixed: the bullet rewritten to the three DB-touching places, and to what the freeze covers and the drains do not |
| 7a | `running()` without `[[nodiscard]]` beside a `drained()` that has it | CONFIRMED | fixed |
| 7b | `loadCameras()` lost its `const` only because the guard's counter is a non-const reference | CONFIRMED | recorded |
| 7c | the `InFlight` struct duplicated verbatim in the operator's and the monitor's headers | CONFIRMED | fixed: one `in_flight::Guard` at `services/camera/src/shared/utils/in-flight/` (rule 23's 2+ rule), included by both |
| 7d | `NatsObjectEventSink::syncHook` is never assigned — a dead hook | CONFIRMED (pre-existing, outside this unit) | recorded |

Found by this unit's own verification while fixing the above, and fixed here:

| Finding | Action |
|---|---|
| the freeze could be armed between `client()`'s flag read and its `getDbClient()` call — the window the seam exists to close | fixed: the flag and that dereference share one reader/writer lock (`seamMutex`); the plan's D23 and §4.6 record the residual boundary |
| the freeze test proved `!isRunning()`, which `quit()` flips *before* the manager goes — it did not prove the reset | fixed: the case now waits for the app's client to report no available connections |
| `AppRunner`'s teardown joined a `run()` that had never stopped, which can hang a suite instead of failing it | fixed: bounded wait, then `quit()`, then detach rather than join forever |

What the review checked and found correct is worth recording too, because it is
the half that did not change: the counter's `acq_rel` pairing (a `load` that
reads 0 has observed the last `fetch_sub`, so the write behind it is visible),
the monitor's leg being genuinely sealed (its only `execSqlSync` is the only
thing its guard covers, and its loop re-checks `running_` per camera), no guard
living across a suspension point in any of the three sites, ownership and
lifetime of the two units against the module's registrations, and rule 20
holding across all seven changed files.

