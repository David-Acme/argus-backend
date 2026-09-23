# 3a step 2 closure, item 1 — the drain is stopped before Drogon quits (S8)

Scope: the nine units that own a drain over a durable outbox — the four change
producers (`camera`, `notification`, `productivity`, and the identity package
the gateway hosts) plus `guard` — and `packages/lib/runtime`, which gains the
shutdown hook they all register with.

S8 was the only item in step 2's closure that is a crash in normal operation,
which is why it went first after item 0.

## The crash

`drogon::app().quit()` runs on the loop thread. It stops the IO loops and
**resets the database client manager**, while `DbService::client()` is
`drogon::app().getDbClient()` (`packages/lib/sqlite/src/sqlite/db-service.hxx:11`),
which dereferences that manager without a guard
(`HttpAppFrameworkImpl::getDbClient` → `dbClientManagerPtr_->…`).

The five drains do not live on the loop. Each sink owns a `std::thread` that
polls its outbox and publishes, and that thread is joined only by the sink's
destructor — after `main()`'s `drogon::app().run()` has returned, i.e. after
the manager is already gone. `guard`'s drain is its coroutines, which do live
on the loop but suspend across repository calls, so a coroutine resumed by the
loop's own shutdown can still reach the manager.

The window is therefore not one retry cadence: it is the whole shutdown,
started by the ordinary `SIGTERM` a `docker stop` sends, and every drain tick
landing in it is a null dereference. Recorded in 3a-2c (C6), sharpened in
3a-2d, and guard's variant recorded in 3a-2h.

## The decision: the quit is deferred, not the drain

`packages/lib/runtime/src/runtime/shutdown-signal.{hxx,cc}` inverts the order.
A service registers each drain it owns at boot:

```cpp
shutdown_signal::onStop(shutdown_signal::drainOf(*changeSink, "camera-change"));
```

`onStop` takes a `Drain` — a `name`, a `requestStop()` and a `drained()` — and
`drainOf` adapts a unit that has those two members. The module's term/int
handler (`installHandlers()`, installed by the first registration) now only
**requests** stops; it never quits. The Drogon loop keeps running, and a
50 ms poll re-arms itself until every registered drain reports drained, at
which point `drogon::app().quit()` follows. A 10-second deadline bounds the
wait: an expired deadline logs every drain that never reported, by name, and
quits anyway.

The registration therefore precedes the work it guards: a sink is registered
**before** its `reconcile()` starts the worker. A drain that registers after
the stop was already requested cannot be waited for — the quit is already in
flight — so `onStop` stops it at once and logs that the quit will not wait for
it. That ordering is what makes the interrupted-boot case safe rather than
merely refused: the signal lands on the main thread while it is still inside
the boot path, so the drain would otherwise start a worker into the window
`quit()` opens.

Every one of the six registrations therefore sits immediately before
`drogon::app().run()`, not inside a beginning advice — `run()` installs Drogon's
own `sigaction`, and a `SIGTERM` that lands in the stretch between that install
and a later registration would be answered by Drogon's **default** handler,
which quits with no drain wait at all. `TERMFunction` dispatches to whatever
handler is stored **at signal time** (`getTermSignalHandler()()`), so a
registration only has to precede the signal; it is the default-handler window,
not the dispatch, that the early position closes.

`drained()` per unit means "this unit will touch the database no more":

| Unit | `requestStop()` | `drained()` |
|---|---|---|
| the four change sinks + camera's object-event sink | sets `stopping_`, wakes the worker | `exited_ \|\| !workerStarted_` |
| `GuardService` | clears `alive`, stops the retry pump, stops the timers | `active == 0` |

`exited_` is written as the **last** statement of `flushLoop()`, so it is
exactly "the worker body has finished touching the database"; a sink that was
never reconciled reports drained, because it has no worker to wait for.
`active` is the guard's `LifecycleGuard` count, and every async body of
`GuardService` holds one across its suspends (`guard-service.cc:331, 375, 384,
432, 811, 861, 923, 959, 1025, 2377, 2704`), so `active == 0` is exactly "no
coroutine is suspended mid-await".

### Alternatives rejected

- **An `alive` check after every `co_await`** (68 of them in guard): invasive,
  and miss-prone in exactly the way the item describes — one forgotten check
  is the same crash.
- **Pinning a client inside `DbService::client()`** so the pointer outlives
  the manager: turns a crash into a hang, and breaks the per-test-case
  `addDbClient` the suites rely on.
- **A bounded join plus `detach()` in the sink destructors**: the detached
  thread keeps publishing into a database whose connections are being closed
  — a use-after-free instead of a null dereference.
- **Registering the drain and quitting from `main()` after `run()` returns**:
  the manager is destroyed *inside* `quit()`, before `run()` returns, so the
  window is already open.

## What changed

| Area | Change |
|---|---|
| `packages/lib/runtime` | new `shutdown-signal.{hxx,cc}` (registry, signal handlers, the deferred-quit poll), added to the `argus_lib(NAME runtime …)` source list; new `shutdown-signal-test.cc` target |
| five sinks | `requestStop()`/`drained()` added; `exited_` added; the destructor's inline stop became `requestStop()` |
| `GuardService` | `requestStop()`/`drained()` added |
| six boot sites | `shutdown_signal::onStop(shutdown_signal::drainOf(…, "…"))` immediately before `drogon::app().run()`, ahead of the `reconcile()` (or `start()`, in guard) that the beginning advice still runs: camera (two drains), gateway, notification, productivity, guard |
| five sink suites + guard's retry suite | the drain is pinned: not drained while the worker runs, drained after `requestStop()`, idempotent second request, and — in guard — an in-flight observation parked inside the notification call, so the not-drained verdict is deterministic rather than a race |
| `ObservationRetryPump` | `stop()` sets an atomic flag instead of clearing the `std::function` the loop's tick reads: the stop now arrives from the signal path, and the pump's `tick_` is loop-thread-only |
| docs | `packages/lib/runtime/AGENTS.md` (layout bullet, the registration rule, the test), and a paragraph in each owner's `CONTEXT.md` where its drain is already described (camera, notification, productivity, guard, identity, gateway) |

## Evidence

- **The five touched services, rebuilt and tested** (`dev`, `--only`): guard 54
  tests, camera 51, gateway 31, notification 39, productivity 35 — 210 tests, 0
  failures and no warning or error in any of the five logs. Guard's
  `guard-retry-lifecycle-test` runs its drain block against the gated
  notification fake (18.46 s for the whole suite).
- **The drain predicates are pinned by tests, not by inspection.** The module's
  own suite boots a Drogon app, asks a drain that is still finishing, holds
  `app().isRunning()` while it does, and quits once it reports — and separately
  registers a drain *after* the stop, which is stopped without being waited
  for. The five sink suites and guard's suite assert the in-flight verdict is
  false and the post-stop verdict is true.
- **A real `SIGTERM`, on a real binary, with a real worker.** The notification
  service was booted from a throwaway config in `/tmp` (its own database, its
  own ports, `[nats] url` pointed at a dead port so the sink exists and its
  worker thread runs while no broker answers), then signalled:

  ```
  13:40:53.455606 WARN  SIGTERM signal received. - HttpAppFrameworkImpl.cc:172
  13:40:53.455648 INFO  Shutdown signal: stop requested for 1 drain(s) - shutdown-signal.cc:148
  ```

  42 µs apart: Drogon's own `TERMFunction` dispatches to the handler the module
  stored, and that handler stops the drain — the one thing the suites cannot
  show, because they call `requestStop()` directly instead of signalling the
  process.
- **How long the drain actually held the quit.** The same run probed the
  listener (Drogon stops it *inside* `quit()`, so the listener only closes once
  the module has decided to quit) and the process: TERM → listener closed at
  **52 ms** → exit code 0 at **428 ms**. So every drain had reported drained by
  the first 50 ms poll tick, and the remaining tail is Drogon's io pool, the
  gRPC server and the sink/bus threads being joined — not the drain. No
  deadline line and no "still running" line appear in the log. (An earlier run
  of the same shape, with the bus mid-retry, exited at 3.6 s: the tail *after*
  the decided quit varies with what the sink and bus threads are doing when
  they are joined — the drain's own contribution is the 52 ms the probe
  measures.)
- **The other side of the window, measured.** With `[nats] url` empty no sink
  is built, no drain is registered, so the module never installs a handler and
  Drogon's **default** one quits: exit 8.7 ms after TERM, with no drain line in
  the log. That is exactly the F-2 window — a service whose drain exists but
  registers late would shut down like this, which is why every registration
  sits before `run()`.
- The happy path logs nothing when it quits, by design: only the deadline
  branch speaks. The probe measures the listener for that reason.
- **The full gate, with the review's fixes in the tree.** `./scripts/build-all.sh
  dev` exit 0: **17/17 projects, 413 tests, no failure** (cert 2, sqlite 2,
  identity 26, memory 20, intent 4, gateway 31, sync 43, camera 51, productivity
  35, notification 39, guard 54, tts 21, stt 7, vlm 8, llm 33, voice 25, tunnel
  12) and **no compiler warning or error** anywhere in the log;
  `scripts/check-comments.sh` 1230 files, 0 comments; `scripts/check-deps.sh` 64
  declarations, 481 edges, 0 forbidden, 0 cycles, 0 unresolved;
  `scripts/check-tidy.sh` 497 TUs, **3125 findings over 45 checks against the
  3141 baseline**, with `modernize-use-scoped-lock` the single check below it —
  the 16 findings the unit took off are all there, because the two checks the
  review had pushed up (`bugprone-empty-catch` 12 → 11 and
  `bugprone-use-after-move` 4 → 3, both fixed above) are back at their baseline
  values, and no other check has moved. Two residues are left in place
  deliberately, both pre-existing and both inside the baseline:
  `guard-service.cc:1094` (`'state' used after it was moved`) and the three
  empty catches at `guard-retry-lifecycle-test.cc:440/447/476` — giving those
  catches a body would change what the cases assert, which is not this unit's
  call to make.

## The adversarial review, and what it changed

Two read-only agents ran against the change: one adversarial review of the
hook, and one sweep for every first-party thread that can reach SQLite. Each
finding below was re-verified against the tree before it was acted on.

| # | Finding | Verdict | Action |
|---|---|---|---|
| F-1 | Camera's operator frame path (`co_await BlockingTask` at `camera-operator-service.cc:526` → `processFrame` → `sink->publish`, `:909`) and its rescan (`:160`) and the health monitor's `loadCameras` (`camera-health-monitor.cc:106`) call `DbService::client()` from threads the hook does not know, and their `stop()`s run after `run()` returned | confirmed | recorded, not fixed here — same crash class, different owner: closure item 1b |
| F-2 | A SIGTERM between Drogon's `sigaction` install (`HttpAppFrameworkImpl.cc:573-591`, inside `run()`) and the beginning advice that registered the drains calls Drogon's **default** handler (`:744`), which quits with no drain wait; the advice still runs first, so the worker starts into the reset | confirmed | **fixed** — every registration moved out of its beginning advice to before `drogon::app().run()`: camera (two), gateway, notification, productivity, guard |
| F-3 | A drain registered after the stop is stopped but not waited for | by design | kept: the registration precedes what it guards, so a "late" drain is stopped before its worker starts — that ordering is what makes the interrupted boot safe |
| F-4 | `requestStop()` runs in signal context and takes the registry mutex, allocates, and calls each drain's stop | confirmed, accepted | recorded: Drogon's own `TERMFunction` does a `LOG_WARN` (`:172`) before dispatching to the stored handler, so the framework has already taken a lock and an allocation on this path; the module's exposure is now a vector copy (registration happens before `run()`, so the boot window is gone) |
| F-5 | `ObservationRetryPump::stop()` cleared the `std::function` the loop's tick reads, from the signal thread | confirmed, introduced by this change | **fixed** — the pump stops through an atomic flag and never writes `tick_` off the loop thread |
| F-6 | `drained()` does not mean "no database access from anywhere in the unit": guard's HTTP feature services (`guard-feature-service.cc:31-54`) hold no `LifecycleGuard`, and the sinks' enqueue side is loop-side | confirmed | recorded: `drained()` is worker-scoped, and the docs now say so; the loop-side half rides on Drogon draining its functor queue before the reset, and a coroutine suspended *across* the reset is out of the drain's reach — closure item 1b |
| F-7 | A sink that never reconciled reports drained | by design | kept, and it is load-bearing (NATS not configured is a clean shutdown); the ordering fix is what keeps it honest |
| F-9 | The deadline branch, a throwing `requestStop()` and a throwing `drained()` are untested | partly confirmed | `drained()` is now wrapped like `requestStop()` (`drainedOrLog`, treated as not-drained); the deadline branch stays untested and recorded |
| F-10 | On deadline expiry the quit lands under a running worker | confirmed | recorded: that is the documented bound, and closing it means making each drain's stop interruptible |
| F-11 | `pollAndQuit` called each drain's `drained()` while holding the registry mutex | confirmed | **fixed** — the registry is copied under the lock (`snapshot()`) and the callbacks run outside it |
| F-12 | The registry never forgets a drain, so a unit destroyed early leaves a dangling entry | latent | the guard suite stopped registering its stack-local service; the module's own test owns registration, and `AGENTS.md` states the unit must be boot-lifetime |
| F-13 | The sinks notify without holding the wait mutex | latent | recorded: every wait in `flushLoop` is a timed `wait_for`, so a lost wakeup costs up to one `retryMs` (500 ms) and never a hang — and the probed shutdown shows the notify landing, 52 ms from signal to a decided quit |
| coverage | llm (detached stream thread; tool chat), memory's worker, identity's raw-vec0 face threads, the sync fan-out and the lease/retention sweeps are all DB work a SIGTERM does not stop | confirmed | recorded — closure item 1b |

## What this unit does not change

- Services without a drain (llm, stt, tts, vlm, voice, sync, tunnel, intent,
  memory) keep Drogon's default signal path untouched: they never call the
  module, so nothing about their shutdown moves. Their own DB work is item 1b's
  subject, not this unit's.
- The 10-second deadline is a bound, not a guarantee: a drain that never
  reports drained is logged and outlived. Closing that residual means making
  each drain's stop interruptible, which is a per-unit change and is recorded
  as such in the plan.
- The drain is worker-scoped. A loop-side handler of the owning service that is
  suspended across the reset is not in its reach; what protects the ordinary
  case is that Drogon runs the whole functor queue — the queued handlers —
  before the teardown lambda that resets the manager.
- Nothing here changes the outbox format, the wire, the streams or the
  published payloads: the registration is boot wiring and a predicate.
