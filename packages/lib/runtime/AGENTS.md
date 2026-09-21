# argus-runtime

The process-runtime primitives every service but the purely request-shaped
ones needs: the off-loop blocking task, cooperative cancellation, the shared
AI-engine mutex, the adaptive thread budget and the hardware profile the AI
services size their pools and offload from.

## What this is

A PACKAGE, not a service: no routes, no `main`, no database. Nine units
outside `packages/lib` link it — identity, memory and seven services. Two of
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

## Tests

`tests/unit/thread-budget-test.cc` — the clamp bounds, monotonicity in the
hardware count, and the extraction queue as the one queue work is raised for.
