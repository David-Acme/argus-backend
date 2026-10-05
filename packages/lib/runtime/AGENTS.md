# argus-runtime

The process-runtime primitives every service but the purely request-shaped
ones needs: the off-loop blocking task, cooperative cancellation, the shared
AI-engine mutex, the adaptive thread budget and the hardware profile the AI
services size their pools and offload from.

## What this is

A PACKAGE, not a service: no routes, no `main`, no database. Eleven units
outside `packages/lib` link it — the `vlm` client and ten services (auth,
camera, guard, identity, llm, notification, stt, sync, tts, vlm). Two of
its targets are the reason the profile lives in a package rather than in
service-private code: it is compiled twice, once ncnn-free for the services
that must not drag an inference runtime in and once with the Vulkan probe, and
both variants have to answer the same numbers.

## Layout

- `src/runtime/blocking-task.hxx` — `BlockingTask<T>`: an awaitable that runs a
  blocking callable off the loop and resumes the coroutine on the Drogon loop
  it was awaited from (see "Where a blocking task resumes"). A blocking SDK call goes through this, never straight into a Drogon
  handler. `BlockingTask<T>(fn)` runs on the light lane; an inference call
  passes `BlockingLane::Heavy`; work that must run one at a time in arrival
  order passes a `BlockingStrand&` instead of waiting for a turn on a worker.
- `src/runtime/blocking-pool.{hxx,cc}` — the two process-wide lanes behind it
  (`blocking_pool::lane`, `submit`, `statsOf`), the `ElasticPool` they are made
  of and `BlockingStrand`. See "The blocking lanes" below.
- `src/runtime/cancellation-token.hxx` — `CancellationToken`: a shared atomic
  flag a long operation polls; `cancel`/`reset`/`cancelled`.
- `src/runtime/wake-signal.hxx` — `WakeSignal`: `notify`/`waitFor`, a
  condition variable with its own pending flag, so a notify that lands while
  the worker is busy wakes its next wait instead of being lost. The outbox
  drains sleep on it.
- `src/runtime/ai-init.hxx` — `ai_init::llamaMutex()`: the single mutex that
  serialises access to the shared LLM/Vision context (rule 13d).
- `src/runtime/cpu-limits.hxx` — `cpu_limits`: the CPU count a process may
  actually use, the tightest of the online processors, its
  `sched_getaffinity` set and its cgroup quota (v2 `cpu.max`, v1
  `cpu.cfs_quota_us`/`cpu.cfs_period_us`, walking every ancestor of the
  `/proc/self/cgroup` path and keeping the tightest, rounded up). Pure parsers
  plus an injectable file reader, so the suite feeds it cgroup trees instead of
  the host's.
- `src/runtime/thread-budget.{cc,hxx}` — `ThreadBudget`: `hardwareThreads`,
  `computeThreads`, `batchThreads`, `heavyThreads`, `lightThreads`,
  `inferenceSlots`, `ttsThreads`, `extractionThreads`, `extractionSlots`,
  `queueWorkers`. The single source of thread counts; no AI service hardcodes
  one. `hardwareThreads()` is `cpu_limits::probeEffectiveThreads()`, read once:
  inside a container it is the compose `cpus:` limit (1.5 → 2), not the host's
  cores, which `std::thread::hardware_concurrency()` reported and CFS then
  throttled in 100 ms slices.
- `src/runtime/hardware-profile.{cc,hxx}` — `HardwareProfile` (cores, ISA,
  RAM, Vulkan device and VRAM, video accel, `CapabilityTier`;
  `logicalThreads` and `physicalCores` describe the host, `effectiveThreads`
  the budget this process may use — the tier stays a property of the
  hardware, so a 2-CPU container on an 8-core host does not turn the VLM off) and
  `HardwareProbe::get()`/`describe()`, plus the derived answers the services
  ask for (`detectorInputSize`, `analysisFps`, `vlmEnabled`, `toolsEnabled`,
  `llmGpuLayers`, `vlmGpuLayers`). `docs/operations/hardware-tiers.md` is the
  document this implements.
- `src/runtime/shutdown-signal.{cc,hxx}` — `shutdown_signal::onStop(Drain)`:
  the process-wide stop sequence. A service registers each drain it owns
  (`drainOf(unit, name)` adapts a `requestStop()`/`drained()` pair), the
  module's term/int handler only *requests* stops, and Drogon quits once every
  registered drain reports drained — or after the deadline (15 s by default,
  `setDeadline` to change it), naming the drains that never finished. The
  default sits below the compose `stop_grace_period: 20s`, so the quit hooks
  and the database freeze run before Docker's SIGKILL. A second SIGTERM/SIGINT
  while the first is still draining exits at once with `kForcedExitCode` (130):
  an operator who asks twice is not kept waiting on a stuck drain. `onQuit(hook)` registers what has to happen
  *between* that last drain and the quit itself.

## Rules

- Rule 25: the folder IS the module. `argus_lib(NAME runtime ...)` for the
  primitives, `argus_module(NAME hardware-profile)` and
  `argus_module(NAME hardware-profile-gpu)` for the two profile variants —
  explicit source lists, never `file(GLOB)`. The include root is `src/`, so
  consumers write `<runtime/thread-budget.hxx>` and
  `<runtime/hardware-profile.hxx>`.
- The two profile targets differ in ONE way: the GPU variant links the ncnn
  target and probes Vulkan, the other is compiled with `ARGUS_NO_NCNN_GPU`.
  A consumer that does not need Vulkan must link the ncnn-free variant —
  `argus-tts`'s synthesis module links it, which is what keeps the binary's
  `nm -C | grep -c 'ncnn::'` at zero.
- A budget is a function of the host, never a constant: the clamps live in
  `thread-budget.cc` and the suite asserts each budget stays inside its own
  bounds and moves monotonically with the hardware count. A new budget gets a
  clamp and a row there before it gets a consumer.
- Nothing here opens a socket, reads config or starts a thread of its own at
  load time. These are values and small types a service's boot wires up.
- A unit that owns a worker thread writing to the database registers it with
  `shutdown_signal::onStop` at boot, **before `drogon::app().run()`** — which
  is also before the call that starts the worker (`reconcile()` in the change
  sinks, `start()` in guard). The registration is
  what stores the module's term/int handlers, so registering inside a beginning
  advice instead leaves a window in which Drogon's own handler quits with no
  drain wait at all. `requestStop()`
  must be non-blocking (it runs on the loop thread, and the module calls it
  from a signal handler's own path), and `drained()` must mean "this unit's own
  worker will touch the database no more" — a loop-side request handler of the
  service is not the drain's to report. A registration that lands after the
  stop was already requested is stopped at once and logged, but the quit does
  not wait for it — which is why the registration comes first. The `Drain`
  holds its unit by reference, so the unit outlives shutdown: a boot-time
  object, never a local. Never call
  `drogon::app().quit()` from a shutdown path of your own: the module is the
  only caller, and it quits after the drains, not before. (A test suite ending
  its own throwaway app is not that path.)
- `onQuit` is the seam for work that must run after the last drain and before
  `app().quit()` — quitting is what resets Drogon's database client manager,
  and `DbService::client()` dereferences it unguarded, so this is the only
  window where a service can hand the statements still to come to a client of
  its own (the `argus-sqlite` freeze). Its hooks run **once**, in registration
  order, on the loop thread, each inside the module's own catch — one that
  throws is logged and the rest still run. A hook is therefore quick and never
  waits on a worker the drains were asked to stop, and it is registered at
  boot beside the drains, not from a request path.

## Tests

`tests/unit/thread-budget-test.cc` — the clamp bounds, monotonicity in the
hardware count, and the extraction queue as the one queue work is raised for.

`tests/unit/cpu-limits-test.cc` — `cpu.max` and cfs parsing (`max`, `-1`,
garbage), the tightest of online/affinity/quota, a container's own cgroup v2
root, a nested v2 path whose ancestor is tighter, an unlimited tree, a v1
container read at its mount root, and the probe never exceeding the online
count.

`tests/unit/shutdown-deadline-test.cc` — the default deadline, a second signal
forcing the exit (in a forked child), a single one only recording the request,
and a drain that never finishes abandoned at a configured 200 ms deadline.

`tests/unit/shutdown-signal-test.cc` — the deferred quit: the hook stops a
drain registered after the stop without waiting for it, asks every registered
drain once, holds `app().isRunning()` while one is still draining, and quits
once all of them report drained.

`tests/unit/shutdown-quit-hook-test.cc` — the quit hook runs only after the
last drain reported drained (not before), exactly once, on the loop thread,
with the app still running — the window the sqlite freeze is armed from.

`tests/unit/app-runner.hxx` — the shared `AppRunner` the two suites above boot
a throwaway Drogon app with, plus `waitForBoot` and `waitUntil`.

## The blocking lanes

`BlockingTask` used to start a detached `std::thread` per call. Nothing bounded
it: a burst of 2000 slow calls (20 ms each) peaked at 1000 live threads in the
stress suite, and each call paid a thread start (15.4 µs per empty job against
3.9 µs on a reused worker, 20 000 jobs). It now hands the callable to one of two
`ElasticPool`s, created on first use (nothing starts at load time):

- **Light** (the default): blocking RPC and IO — the auth verdict every request
  waits on, the identity directory, the camera actions, the sync pulls. Core
  `ThreadBudget::lightThreads()` workers stay; extra workers up to
  `ThreadBudget::blockingLightThreads()` (`clamp(hw * 4, 16, 64)`) start the
  moment a job finds no idle worker and retire after 30 s idle. These calls
  wait on the network, not on a core, so the lane grows instead of queueing
  until its cap; past it, jobs queue in arrival order.
- **Heavy**: AI inference (`chatAsync`, `chatStreamAsync`, `describeAsync`,
  `transcribeAsync`, `synthesizeAsync`, face identify/extract, the extractor,
  the LLM controller). One core worker, up to
  `ThreadBudget::blockingHeavyThreads()` (`clamp(hw, 4, 16)`). The engines
  serialise themselves (rule 13d's mutexes, the face semaphore), so this lane
  only caps how many callers may wait on them. Its point is isolation: a
  minute-long generation never holds a worker the auth verdict needs. Measured
  (`blocking-pool-stress-test`, 32 jobs of 300 ms saturating the long lane,
  200 short jobs at 2 ms intervals): one shared pool of 8 made a short job wait
  p50 1001 ms / p99 1191 ms; the two lanes kept it at p50 0.01 ms / p99 0.03 ms
  with 9 threads.

**Deadlock rule.** A job must never block waiting for another job of the same
lane: with a bounded lane, enough such jobs hold every worker while the job
they wait for sits in the queue. Identity's session notices did exactly that
(a ticket per call, each worker waiting for its turn); they now post to a
`BlockingStrand`, which keeps the arrival order without holding a worker while
it waits. Waiting on something outside the pool (a database commit callback,
another service's reply) is fine. The lanes are leaked-by-design statics:
they are never destroyed (a union whose destructor does nothing), so a worker
or a strand still running during static destruction never reaches a freed
pool; workers own the shared state, and a worker still inside a call at exit
is not joined.

**Bounded admission.** Each lane carries a queue cap (`maxQueued`: light
`blockingLightThreads() * 64`, heavy `blockingHeavyThreads() * 16`). `submit`
and `BlockingTask(fn, lane)` keep the old behaviour and always queue, so no
caller changes meaning. A caller that can answer "busy" asks for it:
`trySubmit` returns false past the cap, `BlockingTask(fn, lane,
BlockingAdmission::RejectWhenFull)` (or with a strand) throws `BlockingLaneFull`
into the awaiting coroutine — a controller maps it to 429, a gRPC handler to
`RESOURCE_EXHAUSTED`. `BlockingStrand(lane, maxQueued)` bounds a strand the
same way through `tryPost`. `stats()` reports `queued`, `rejected` and
`oldestQueuedAge` (the wait of the job at the head), which is what a health
endpoint should expose. A strand whose next hand-off to the pool fails (no
thread could start) runs the job on the worker it is already on instead of
staying marked scheduled forever; a `post` whose first hand-off fails takes
its job back out and rethrows.

## Where a blocking task resumes

`await_suspend` captures the loop of the awaiting thread. If it is Drogon's
main loop or one of its IO loops, the coroutine resumes there — a handler that
awaited on IO loop 3 continues on IO loop 3, which is what `services/sync`'s
`RoomManager` (a `thread_local` room table per IO loop) needs. Any other origin
(a cnats thread, a DB client loop, a worker) resumes on the main loop, as every
await used to. If the chosen loop is not running — before `app().run()`, or
after `quit()` — the coroutine resumes inline on the worker, because a functor
queued on a stopped loop never runs and the coroutine would leak. A quit that
lands between the check and the queueing can still strand one frame; that
window is the stop sequence's own and the drains close it.

