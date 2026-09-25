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
  blocking callable on a detached thread and resumes the coroutine, the
  `camera-sync-source` pattern. A blocking SDK call goes through this, never
  straight into a Drogon handler.
- `src/runtime/cancellation-token.hxx` — `CancellationToken`: a shared atomic
  flag a long operation polls; `cancel`/`reset`/`cancelled`.
- `src/runtime/ai-init.hxx` — `ai_init::llamaMutex()`: the single mutex that
  serialises access to the shared LLM/Vision context (rule 13d).
- `src/runtime/thread-budget.{cc,hxx}` — `ThreadBudget`: `hardwareThreads`,
  `computeThreads`, `batchThreads`, `heavyThreads`, `lightThreads`,
  `inferenceSlots`, `ttsThreads`, `extractionThreads`, `extractionSlots`,
  `queueWorkers`. The single source of thread counts; no AI service hardcodes
  one.
- `src/runtime/hardware-profile.{cc,hxx}` — `HardwareProfile` (cores, ISA,
  RAM, Vulkan device and VRAM, video accel, `CapabilityTier`) and
  `HardwareProbe::get()`/`describe()`, plus the derived answers the services
  ask for (`detectorInputSize`, `analysisFps`, `vlmEnabled`, `toolsEnabled`,
  `llmGpuLayers`, `vlmGpuLayers`). `docs/operations/hardware-tiers.md` is the
  document this implements.
- `src/runtime/shutdown-signal.{cc,hxx}` — `shutdown_signal::onStop(Drain)`:
  the process-wide stop sequence. A service registers each drain it owns
  (`drainOf(unit, name)` adapts a `requestStop()`/`drained()` pair), the
  module's term/int handler only *requests* stops, and Drogon quits once every
  registered drain reports drained — or after a 10 s deadline, naming the
  drains that never finished. `onQuit(hook)` registers what has to happen
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

`tests/unit/shutdown-signal-test.cc` — the deferred quit: the hook stops a
drain registered after the stop without waiting for it, asks every registered
drain once, holds `app().isRunning()` while one is still draining, and quits
once all of them report drained.

`tests/unit/shutdown-quit-hook-test.cc` — the quit hook runs only after the
last drain reported drained (not before), exactly once, on the loop thread,
with the app still running — the window the sqlite freeze is armed from.

`tests/unit/app-runner.hxx` — the shared `AppRunner` the two suites above boot
a throwaway Drogon app with, plus `waitForBoot` and `waitUntil`.
