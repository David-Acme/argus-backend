# Phase 2 step 6 — the two test-teardown families, and the suites run standalone

Scope: the row is "`./scripts/build-all.sh dev` → 0 errors, 0 warnings; package tests run
standalone" (`docs/history/plans/architecture-plan.md:822`), read against row 5's `Done` cell,
which hands this row two families it found and did not finish: **18 throwaway drogon sqlite clients
in 11 test files across 6 units**, and **the joinable-`std::thread`-on-unwinding shape**. Base
`c70791d`. The row's two halves are measurements — a gate log and a failure rate — so the ledger is
in **Verification**, and a number that is a reporting agent's rather than re-measured here says so.

## Pre-state, measured

- Gate: 0 first-party warnings, every target carrying `-Wall -Wextra` (re-measured at the end).
- `git grep newSqlite3Client -- '*test*.cc'`: 18 sites in 11 files / 6 units.
- `git grep "std::thread" -- '*/tests/*'` at `HEAD`: 30 case-local `std::thread runner(...)` sites
  plus 13 case-local worker threads in 3 files (`camera-action-rpc` 8, `object-event-outbox-sink` 2,
  `guard-retry-lifecycle` 3). The three fake servers under `packages/clients/*/tests/support/`, plus
  `services/vlm/tests/support/fake-vlm-server.hxx` and `services/tunnel/tests/tunnel-harness.hxx`,
  already stop and join in their destructors (`stop()`: close the listener or stop the loop, then join).
- After the row, the same question asked the other way round: `git grep -l 'drogon::app().run()'
  -- '*/tests/*'` returns **41 files**, the set carrying the canonical destructor comment is **the
  same 41** (`comm` empty in both directions) with exactly one such owner per file, and
  `git grep 'std::thread .*drogon::app().run()' -- '*/tests/*'` returns **nothing** — every in-test
  drogon boot is owned, which is a property a reader can re-measure rather than a count to trust.
  40 of the 41 owners are `class AppRunner`; the 41st is `packages/clients/vlm`'s `FakeVlmService`,
  which the row converted too (its destructor at `HEAD` was `quit(); if (joinable()) join();`).
  **The two files the row edits that are not owners are the defect-1-only ones** —
  `packages/lib/sqlite/tests/unit/identity-client-test.cc` and
  `services/gateway/tests/gateway-test.cc`, which hold a drogon sqlite client and never boot the
  app — so the touched set closes as **43 = 41 owners + 2 sqlite-only** (`diff` between
  `git status --short`'s 43 paths and the 41 `drogon::app().run()` files leaves exactly those two,
  and it leaves nothing on the other side).
- Stress baseline (480 runs per suite × 11 suites, 8 workers under load, each worker in its own cwd
  and its own `/tmp` through a mount namespace): **7 aborts in 5,280 runs, every one an EDEADLK
  self-join** — `identity-client` 1, `audit-sync-read` 3, `camera-notifier` 1,
  `productivity-controller` 2; the other seven suites 0/480.
- Assertion counts recorded before any edit (they must not move): device-credential 55,
  identity-client 3, camera-action-rpc 143, camera-controller 72, camera-talk-cutover 19,
  audit-sync-read 67, camera-notifier 82, gateway 320, notification-controller 54, push-intent 39,
  productivity-controller 163.

## The mechanism

A drogon sqlite connection runs on its own loop thread (`Sqlite3Connection::init()` →
`loopThread_.run()`); `Sqlite3Connection::execSql` queues a lambda capturing a strong `thisPtr` on
that loop. Releasing the client while such a lambda is still queued makes the last release happen on
that loop thread: `~Sqlite3Connection` → `~EventLoopThread` → `std::thread::join()` on itself →
`std::system_error("Resource deadlock avoided")` → SIGABRT with zero assertions. The release that
takes the count to zero must happen on another thread, i.e. no statement lambda pending.

The rest of the teardown path is already safe by construction: `DbClientImpl::~DbClientImpl` →
`closeAll()` swaps the connection list into a local and releases it on the calling thread;
`Sqlite3Connection::disconnect()` captures a **weak** pointer and keeps its strong reference as a
local on the calling thread, blocking on `f.get()` while the loop runs its lambda. The loop thread
holds nothing once no statement lambda is pending.

## The cure, proved

A sentinel queued from inside a statement callback. Callbacks run on the connection's own loop, and
trantor destroys each queued functor as it dequeues the next (`EventLoop::doRunInLoopFuncs`:
`Func func; while (funcs_.dequeue(func)) func();`), so the statement lambda's reference is gone
before the sentinel runs — after which only the calling thread holds the connection and the
destructor joins an idle loop thread from outside. Measured under load on two probes with an
`LD_PRELOAD` `pthread_join` watcher — `probe-client.cc` (31 lines) for the shape above and
`probe-drain.cc` (80 lines) for the drained one:

| shape | cycles | self-joins |
|---|---|---|
| release right after the last statement | 16,000 | 4 |
| release after the loop is drained | 80,000 | 0 |

With the unfixed rate, 80,000 cycles would have shown ≈20 (p ≈ 2·10⁻⁹).

`drain()` as handed to every agent (it adds no assertion by design; a timeout throws, which doctest
reports) — snippet from `/tmp/row6/BRIEF.md`.

Three shapes, chosen per site: **(a)** seeding-only fixtures write through the sqlite3 C API in the
unit's sibling idiom (the removal of the client removes the window); **(b)** fixtures that must keep a
client drain it immediately before releasing it; **(c)** clients the app owns through `addDbClient` /
`getDbClient` are left alone, since the manager releases them inside `quit()` on the app's own loop
thread while it holds a reference. Defect 2 gets an RAII owner (`AppRunner`, canonical shape at
`services/notification/tests/unit/push-intent-test.cc:52-80`).

## Wave D — the owner's stop path, measured per unit

The `AppRunner` destructor this row prescribed has a window, and closing it became wave D (the
cross-cutting bullet below states the window against the linked sources). The canonical destructor —
21 lines, `cmp`-identical in every copy after the wave — is:

```cpp
  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    // Drogon reports the app running before its main loop is looping, and a
    // loop that has not begun cannot be stopped: trantor's loop() clears the
    // quit flag again as it starts. Waiting for it to loop is what makes the
    // quit below take effect — detaching in that window left the app's thread
    // running past the end of the process, measured as SIGSEGV inside
    // EventLoop::loop() in 3 of 20 runs of a forced constructor throw.
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    // A boot that never reached the loop at all is left to the process: it
    // cannot be asked to stop, and joining it would block for ever.
    runner_.detach();
  }
```

The 30 s bound is not a wait for the usual case (that is ~1 ms) but the guarantee that nothing about
to loop is left detached; it is only ever paid on a path that is already failing. The `detach()`
branch stays because a boot that never reaches the loop cannot be asked to stop and joining it would
block for ever — what wave D changed is waiting before concluding that this is the case.

Forced-path A/B per unit: arm 1 = the pre-wave-D destructor plus a forced failure right after boot,
arm 2 = the wave-D destructor with the same injection; ≥20 sequential runs per arm, each from a
scratch cwd. `source` says who measured it; a report's numbers are a claim until re-run.

| unit | shape forced | arm 1 (pre-fix) | arm 2 (fixed) | source |
|---|---|---|---|---|
| `services/notification` (ack) | fixture ctor throws after `waitForBoot` | 17×rc=1, 3×rc=139 | 20×rc=1, 0 cores | agent |
| `services/notification` (delivery) | case-local `REQUIRE` after boot | 19×rc=1, 1×rc=139 | 20×rc=1, 0 cores | agent |
| `services/camera` | case-local `REQUIRE` after boot | 28×rc=1, 2×rc=139 (of 30) | 30×rc=1, 0 cores | agent |
| `services/llm` (consumer) | case-local `REQUIRE` after boot | 14×rc=1, 6×rc=139 | 20×rc=1 | agent |
| `packages/lib/cert` | case-local `REQUIRE` after boot | 17×rc=134, 3×rc=139 | 20×rc=134, 0 SIGSEGV, 0/20 detach | agent |
| `services/stt` | case-local `REQUIRE` after boot | 15×rc=1, 4×rc=139, 1×rc=134 | 20×rc=1, 0 signals, 0 cores | agent |

Two limits stated by the agents rather than papered over, and both stand as written: cert's arm-2
`rc=1` is unreachable in that unit's shape because the residual static `CertState::rotationThread`
(joined only by `CertService::shutdown()`, past the injection) aborts at exit in *both* arms, so the
wave-D signal there is 3/20 → 0/20 SIGSEGV plus 11/20 → 0/20 detach, not the exit code; and the
obvious isolation for it (a temporary `shutdown()` before the injection) destroys the very window it
was meant to clear, measured, so no isolated arm is claimed. Notification's fixture shape carries a
second, separate defect found on the way: a `sharedBoot()`-style function-local static cannot be
re-constructed after its constructor threw — the next case re-boots drogon in-process and dies
(`SIGSEGV`, or `Assertion !running_' failed` in `addDbClient`). It is reachable only through a
throwing fixture, needs a fixture-design decision rather than a destructor edit, and is recorded
here unfixed.

One more pre-existing defect surfaced by the same probing and is recorded rather than fixed:
`packages/identity/tests/unit/identity-migration-test.cc`'s `makeFixture` (`:81`) builds its scratch
directory at a **fixed** path and `remove_all`s it, so two instances running at once destroy each
other's fixture — measured by the unit's agent as 16 failures in 16 runs at 8 concurrent pairs (and
the suite's assertion count drops to 111 when it collides). It is a test-hygiene defect of exactly
the family this row is about, but it is not one of the eleven suites under stress and the gate's
ctest is sequential, so no measurement in this row is taken through it. It is the reason this report
does not claim the tree is safe under a parallel `ctest -j`.

Verified by me against the code, not taken from the reports: **the whole tree's destructor copies
were swept mechanically** (`/tmp/row6/dtor-sweep.sh`: anchor on each `detach();`, walk back to the
nearest preceding destructor signature, normalise only the class name and the `runner_`/`thread_`
member, compare against the brief's block) → **41 of 41 byte-identical, canonical block 21 lines**,
and the set of files carrying the canonical comment is exactly the set carrying a `detach()`
(41 = 41) with one `detach()` each and no `} else` before one, so no owner kept the old stop path;
no leftover injection anywhere (`git grep -n "1 == 2" -- '*/tests/*'` empty); and an
assertion-line instrument (`/tmp/row6/assert-count.sh`: assertion-macro lines at `HEAD` vs the
working tree, per touched file) covers all **43** modified files and, re-run after every agent
stopped and again after this row's own last edit, leaves **all 43 at Δ0**. Mid-row the same
instrument read 37 at Δ0 and the four that read
Δ+1 were that moment's four live injections (identity, productivity, stt, tts), all since reverted;
its own first version proved nothing and was rewritten (it took the first `~Name()` in each file,
which in a file with a second RAII class is the wrong block, and its `sed` normalised the mistake
away). Suites re-run by me
after wave D: camera's thirteen all `rc=0` with the six touched counts unmoved
(143/72/19/11/25/19); notification's nine all `rc=0` with counts unmoved (41/24/54/37/51/39/119/16),
and — correcting that unit's report — `ARGUS_NATS_URL=nats://127.0.0.1:4222` makes
`notification-delivery-live-test` exercise **21 assertions** at `rc=0` (a `nats-server` *is* running;
the agent looked for the binary on `PATH` and found none), so that copy is exercised at runtime and
not by compilation alone; llm's consumer 1 case / 25 assertions and live 1 case / 22 assertions, three
runs each, all `rc=0`; cert `rc=0` ×4 with 69/76/71/76 (the pre-existing variance).


## Cross-cutting decisions

Three decisions were taken for the whole row and applied uniformly, because a per-unit
variation would have made the eleven suites incomparable:

- **The seed helpers throw; they do not assert.** Each converted helper replaces a drogon
  call that threw (`execSqlSync` propagates `SqlError`), so a throw is the faithful
  conversion, and it keeps every suite's assertion count identical to its recorded
  pre-state — which is this row's instrument for proving that no test body was weakened.
  The migration suites spell the same helper with `REQUIRE_MESSAGE`
  (`identity-migration-test.cc:46-52`, `camera-migration-test.cc:39-46`,
  `productivity-migration-test.cc:39-46`, `guard-migration-test.cc:112-118`) because they
  were authored that way, not because they were converted. Five units were converted; all
  five use the throwing form. This was a course correction mid-row: the first brief carried
  "no assertion count may move" as an instrument, two agents flagged the resulting
  divergence from the sibling idiom, and the divergence was resolved in favour of the
  instrument and the semantics, not the spelling.
- **The `drain` guard throws too** (a timeout is teardown plumbing, not fixture
  construction, and the tree's teardown owners report failures the same way).
- **A drain covers the failing path only when the released client is the sole reference at
  release time.** The abort needs the release that takes the count to zero to happen on the
  connection's own loop thread; a release on any other thread is safe even with a statement
  lambda still queued, because a queued lambda holds a reference of its own — it decrements,
  it does not destroy. So where a `DbService` slot also holds the client (the read-only
  client, the legacy client, the replacement client, the case's seeded client), a local
  release can never be the last one and every failing path is safe by construction. The
  exception measured in this row is `push-intent-test.cc`'s client, which nothing else holds:
  there the drain had to move into a scope owner so an unwinding path drains too, or a
  failing `REQUIRE` would still end in an unreported abort. Each unit's report states, per
  site, which of the two it is.
- **The owner's stop path has a window, and it is closed by a bounded wait (wave D).** The
  `AppRunner` destructor this row prescribed — `quit()`+join when `getLoop()->isRunning()`, else
  `detach()` — detaches a loop that is *about to start* whenever it runs in the window between
  drogon's own flag (`HttpAppFrameworkImpl.cc:643`) and trantor's loop (`:689`), because
  `EventLoop::isRunning()` is `looping_ && !quit_` (`trantor/net/EventLoop.h:272-274`, and
  `loop()` clears `quit_` at `EventLoop.cc:209`) while `quit()` is ignored unless the loop is
  looping (`:1036`). Measured on `services/notification` with a forced constructor throw after
  `waitForBoot`: **17 of 20 runs exit 1, 3 exit 139 SIGSEGV** with `loop-running=0` and a core dump
  inside `EventLoop::loop()`; with the bounded wait, **20/20 exit 1**. How often the window is
  entered is measured per unit by each one's own probe rather than inferred: notification's
  *pre-fix* arm recorded 3 entries of 20 (ack) and 1 of 20 (delivery), every one of them a crash,
  while the units that instrumented the decision rather than its outcome counted 2/20 and 6/20
  (gateway), 9/20 (productivity), 12/20 (tts) and 11/20 then 9/20 (cert) — every one of those clean
  with the wait in place. (Notification's arm-2 probe prints the state *after* the wait, so it
  cannot measure entries at all; an earlier version of this report read "8 of those 20" out of it,
  which no primary source supports, and that claim is retracted here.) The window is not exotic — `waitForBoot` polls drogon's flag, so a failure just after
  boot decides inside it, which is precisely the path this row exists to make clean — so the fix was
  extended to every owner in the row (wave D) rather than left in the two fixtures where it was
  first measured. Per-unit sections written before wave D describe the destructor as it stood before
  this revision.
- **The failure-injection proof must use `REQUIRE`, not `CHECK`.** The brief handed the
  agents `CHECK(1 == 2);` and that recipe is wrong: doctest's `CHECK` records a failure and
  keeps going — its `checkIfShouldThrow` only unwinds for `REQUIRE` — so it never reaches the
  unwinding path the fix exists for (measured by the camera unit: a pristine suite with an
  injected `CHECK` reports 144 assertions, exit 1, no signal). Two agents found this
  independently and ran the discriminating `REQUIRE(1 == 2)` instead, which on the unfixed
  shape gives `CRASHED: SIGABRT`, exit 134, with the assertions after it never run. A brief
  defect that the verification caught, recorded so the next row's brief does not repeat it.
- **The `SQLITE_CONFIG_MULTITHREAD is not supported!` line is expected.** A fixture that
  seeds through the C API before drogon initialises prints one FATAL line from
  `Sqlite3Connection.cc:97`. The tree's precedent is `guard-migration-test`, which prints it
  identically and is green (76/76 at `HEAD`), so the line is not a regression and no
  assertion count moves because of it.

## Per unit

### `packages/identity` — verified

One file changed, `device-credential-test.cc` (+114/−48). `seedIdentityDb` (six statements)
was a seeding-only client inside the function that called it, so shape (a): the client is
gone and the six statements go through `DbHandle`/`openFile`/`exec` in
`identity-migration-test.cc:16-53`'s idiom. The case-local `std::thread runner` became
`AppRunner runner;` and the trailing `quit()`/`join()` pair was deleted. Two client sites were
classified shape (c) and left alone (the `addDbClient` config and `DbService::client()`),
verified against drogon's source rather than assumed. `identity-migration-test.cc` needed
nothing.

Verified by me, not taken on report: `git diff` read; `device-credential-test` run three
times from scratch cwds → rc=0, `2 | 2 passed`, `55 | 55 passed`, exactly the recorded
pre-state; the shape (c) sites re-read in the source.

### `services/gateway` — verified

Five files touched. `audit-sync-read-test.cc`: `seedAuditTables` and `createCameraTable`
became C-API helpers (with `PRAGMA busy_timeout = 5000` and `journal_mode = WAL` where the
camera client — read-write, at `:539` — may hold the file), a `drain` before each handover,
and the case-local runner became `AppRunner`. `gateway-test.cc`: the `writable` client is
seeded through the C API and `drain(readOnly)` runs before `setReadOnlyClient(nullptr)`.
`notification-delivery-{inbox,live}-test.cc`: case-local runners became `AppRunner` and the
explicit `quit()`/`join()` pairs were deleted. `camera-notifier-test.cc`: the replacement
client is drained before its release, and `SharedBoot`'s declared-then-assigned
`std::thread runner` became `std::optional<AppRunner> runner` emplaced **after** both
`addDbClient` calls — so a constructor that throws after the app's thread exists unwinds into
a member whose destructor stops the app instead of destroying a joinable thread. The
destructor calls `runner.reset()` rather than leaving the optional to be destroyed after the
body, which preserves the fixture's original order: stop the app, then remove the db files.
The agent's stated limit: the constructor-throw path is argued from the code, not measured
(forcing it means a synthetic throw in a function-local static whose failed initialisation
would be retried).

Verified by me: all five diffs read; all five suites run from scratch cwds →
`audit-sync-read-test` 1/1 and 67/67, `camera-notifier-test` 16/16 and 82/82,
`gateway-test` 39/39 and 320/320, `notification-delivery-inbox-test` 1/1 and 44/44,
`notification-delivery-live-test` 1/1 and 0/0 — every count identical to the recorded
pre-state, rc=0 throughout.

### `packages/lib/sqlite` — verified

One file changed, `identity-client-test.cc` (+93/-7). `seedDb` seeding replaces the two
`execSqlSync` calls of a throwaway client with the sqlite3 C API, and the read-only client the
case hands to the service is drained before its release — `drain(readOnly)` ahead of
`setIdentityClient(nullptr)` and `readOnly.reset()`, so both references go away with the
connection's loop empty. The helper keeps the **default rollback journal on purpose**: the
read-only client opens the same file afterwards, and a WAL database needs write access for
its `-shm` — the brief's warning about WAL applies exactly here, and the code says why.

Verified by me: diff read; `identity-client-test` run three times from scratch cwds → rc=0,
`1 | 1 passed`, `3 | 3 passed`, the recorded pre-state. The conversion to `REQUIRE_MESSAGE`
was applied, measured (`3 → 6` assertions, green) and then reverted on the row's argument
above; the report records both forms with their measurements.

An accuracy note the agent raised and I confirmed: `device-credential-test` is **not** in
`packages/lib/sqlite`'s build tree, so the reverse of the cross-package coupling holds — a
service's tree contains its dependency packages' suites, not the other way round.

### `services/productivity` — verified

Two files changed. `productivity-controller-test.cc`: both seeders (`seedIdentityDb`,
`seedProductivityDb`) moved to the C-API helpers, and the handover site drains before it
releases — `drain(productivityDb)` then `productivityDb.reset()`, with the comment naming why
(the service's reference is already gone, so the next release is the last one). The six
`std::remove` calls now run before the app's quit; accepted, with the evidence that 20/20 runs
leave no database behind. `productivity-sync-rpc-test.cc`: the case-local runner became
`AppRunner`.

The `REQUIRE_MESSAGE` conversion was implemented and built here too (`EXIT=0`, 0 warnings,
ctest 34/34) before the belay landed, then reverted with the Edit tool; the report labels the
+11 it would have added as arithmetic, since the converted form was never run standalone.

Verified by me: both diffs read; four suites run from scratch cwds →
`productivity-controller-test` 1/1 and 163/163, `productivity-sync-rpc-test` 1/1 and 30/30,
`productivity-migration-test` 9/9 and 202/202, `productivity-schema-test` 3/3 and 18/18 —
the recorded pre-state throughout, rc=0.

### `services/camera` — verified

Six files changed. All three throwaway clients were **shape (a)** — created inside a
seeding-only function called before `app().run()`, never handed to a service, so there was
nothing to hand over and nothing to drain — and each now seeds through the C API with per-file
helpers in `camera-migration-test.cc:16-57`'s idiom, SQL text byte-identical. Six case-local
runners became `AppRunner`; the ten case-local worker threads got a `Worker` owner whose
destructor joins, **with the body's release callback run before the join** — a worker parked
on a stub gate can only finish once released, so a bare join would block for ever, which is
the case the brief warns about and the owner is shaped around it. Where the body's own release
would have run it was deleted so it fires exactly once (a second `arrive_and_wait` on a
completed barrier would open a new phase and hang).

Verified by me: the diffs read (including the `Worker` owner); **all thirteen** suites of the
unit run from scratch cwds → rc=0 throughout, with the three recorded counts unmoved
(`camera-action-rpc-test` 2/2 and 143/143, `camera-controller-test` 1/1 and 72/72,
`camera-talk-cutover-test` 1/1 and 19/19).

Two residuals the agent reported rather than hid, both accepted and recorded here: a
`std::thread` **constructor** throwing while the other worker sits parked would hang its
owner (a resource-exhaustion path, not an assertion path — the family this row removes is the
assertion one), and this unit's teardown ordering is unchanged — its `std::remove` counts are
identical at `HEAD` and in the working tree (6, 7 and 4 in the three suites), the camera diff adds
none, so nothing about the leftover files changed here and only the pre-existing gitignored
`go2rtc.yaml` remains behind (written at boot by `go2rtc-manager.cc:306`).

### `services/notification` — verified

Seven `AppRunner` copies, all of them in `services/notification/tests/unit/*.cc`: this unit's `tests/` tree is nine `.cc` files with no headers at all, so every copy lives in the file that uses it and all seven name the member `runner_`. Two are fixture-shaped — `notification-ack-test.cc:54` and `notification-no-nats-test.cc:81`, the `SharedBoot` class held in a `std::optional<AppRunner> runner` released by `~SharedBoot` and reached through the `sharedBoot()` function-local static — and both already carried the brief's destructor verbatim, so the wave's action on them is "none". Five are case-local and were replaced: `notification-controller-test.cc:181` (`const AppRunner app;` in the single `TEST_CASE`), `notification-delivery-test.cc:51`, `notification-delivery-live-test.cc:28` (the case returns before the runner is reached when `ARGUS_NATS_URL` is unset), `notification-rpc-test.cc:86` and `push-intent-test.cc:58`. Nothing was renamed, no copies were merged, no shared header was introduced. `notification-migration-test.cc` and `notification-schema-test.cc` contain no drogon reference at all — no boot to stop — and were not touched. At `HEAD` only `push-intent-test.cc` has an `AppRunner`; the other six copies are this row's own uncommitted work, so the destructor change has to be read inside a larger uncommitted delta (`push-intent-test.cc` also carries the row's `drain()` helper and extra includes) rather than as a `git diff` hunk of its own.

The applied body is exact: the block extracted from `~AppRunner()` to its closing brace diffs empty against the brief's block written to `/tmp/row6-notif-scratch/expected-dtor.txt` (md5 `a683d59bf7758d3434f17154d1e02610`) in all seven files, and the five pre-fix bodies had been byte-identical to one another (md5 `215210c25cfa345065654cc0672aefa0`), so one Edit per file replaced exactly that block and nothing else. No include moved: every one of the five already used `std::this_thread::sleep_for` and `std::chrono::milliseconds` in its own `waitForBoot`, and all five include `<thread>`. The unit compiles with the project's `-Wall -Wextra` and no compiler warning (`grep -cE "warning:" build-final.log` → 0), and the only ctest failures across its three build logs are sibling units' live injections caught mid-build, with all nine notification suites passing in those same runs.

The A/B forces both shapes this unit has — arm 1 = the pre-fix destructor plus the injection, arm 2 = the wave-D destructor plus the same injection, 20 sequential runs per arm from a scratch cwd. Fixture shape (`notification-ack-test.cc`'s `SharedBoot` constructor throwing immediately after `waitForBoot`): arm 1 **17×rc=1, 3×rc=139**; arm 2 **20×rc=1, 0 cores** — the pre-fix arm's own `loop-running=` instrument put 3 of its 20 runs inside the window and all 3 of those are the crashes, the 17 runs that read `loop-running=1` exiting 1. Case-local shape (`notification-delivery-test.cc`, `REQUIRE(1 == 2)` right after the boot `REQUIRE`): arm 1 **19×rc=1, 1×rc=139** (the single window entry is the single crash); arm 2 **20×rc=1, 0 cores**. With the fixture shape the *full-suite* runs crash in the second case in both arms (arm 1 with cores in 20 of 20, arm 2 all 20 of 20), because the `sharedBoot()` static cannot be re-constructed after its constructor threw: the next case re-boots drogon in-process and dies (`SIGSEGV`, or `Assertion !running_' failed` in `HttpAppFrameworkImpl::addDbClient`). That is why the arms quoted above were re-run selecting only the first case; the re-boot crash is a separate defect, reachable only through a throwing fixture and needing a fixture-design decision rather than a destructor edit, and it is recorded here **unfixed**.

Counts: nine suites, with every count where it stood before the wave — `notification-ack` 5/41, `notification-no-nats` 2/24, `notification-controller` 1/54, `notification-delivery` 1/37, `notification-rpc` 1/51, `push-intent` 4/39, `notification-migration` 9/119, `notification-schema` 3/16 — and `notification-delivery-live` 1 case, rc=0, whose recorded 0 assertions are the case returning before the runner is reached while `ARGUS_NATS_URL` is unset. That last suite is where this unit's report is corrected rather than trusted: the agent concluded from `command -v nats-server` finding no binary on `PATH` that the copy is verified "by text and by compilation only". A `nats-server` *is* running (`pgrep -af nats` → `nats-server -js -m 8222 -sd /data`, listening on 127.0.0.1:4222), and with `ARGUS_NATS_URL=nats://127.0.0.1:4222` the suite exercises **21 assertions** at rc=0 — so that copy is exercised at runtime, not compiled only.

Verified by me, not taken on report: the seven blocks `cmp`-identical to the canonical 21 lines with no injection left anywhere in the unit; the nine suites re-run by hand, rc=0 with every count unmoved; `notification-delivery-live` run with `ARGUS_NATS_URL=nats://127.0.0.1:4222` → 21 assertions at rc=0 (my run is the one that corrects the agent's reading); `notification-ack` and `notification-delivery` ten runs each, all rc=0; and the agent's 180 post-revert runs (9 suites × 20, all rc=0, no core) read as the sweep they are, with its report carrying no PENDING left.

### Wave B (defect 2 only) — `services/guard`, `services/llm`, `services/stt`, `services/tts`

#### `services/guard` — verified

Twelve files, +444/−68, and defect 1 is absent (`git grep newSqlite3Client -- services/guard/tests` returns nothing at `HEAD`). The 20 `std::thread` lines at `HEAD` classify into four shapes, each fixed with the canonical owner and nothing else: **six inline case runners** (`std::thread runner([] { app().run(); });` plus a trailing `quit(); runner.join();`) → `AppRunner runner;` with the pair deleted; **four `SharedBoot` fixtures** whose member was assigned in the constructor body and could then throw → `std::optional<AppRunner> runner` emplaced after the last `addDbClient`, with the two `throw`s below the emplace so a constructor that throws unwinds into an owner that stops the app (member order still stops the app before `TempDb` deletes its files); **two pre-existing local `AppRunner` destructors** whose `quit(); if (joinable) join();` could block for ever (now guarded, with the `detach` fallback); **three case-local workers** → `std::jthread`, including the move-assigned one (`jthread`'s move-assign joins the previously held thread, and the default-constructed one is not joinable).

Verified by me, not taken on report: all twelve diffs read site by site, including that each `emplace()` sits after the last configuration call and each `AppRunner runner;` sits *before* the `REQUIRE`s that can fire during boot; `git diff -U0 | grep -c` on `CHECK`/`REQUIRE` = 0, so no assertion statement moved and a count change would need a compile error; `git grep "1 == 2" -- services/guard/tests` empty; the twelve suites run by me from scratch cwds → rc=0 with `belief-scope` 6/42, `decision-journal` 16/170, `dialogue-service` 1/216, `dlq-crash` 1/12, `migration` 1/76, `notify-thread` 23/169, `outbox-adopt` 4/35, `retry-lifecycle` 1/28, `saga` 1/123, and `assessment-live` / `dlq-service-live` / `vlm-client-live` reporting 0 assertions each; the injection A/B read from the raw logs (pre-fix shape + `REQUIRE` → `CRASHED: SIGABRT`, core dumped; fixed shape + the same `REQUIRE` → `Status: FAILURE!`, no signal); and the jthread bodies read to confirm there is no wait gate — each is one `sync_wait(service.handle(...))` inside a try/catch, so the destructor's join cannot block on the unwinding path.

The agent's declared limits, corroborated here rather than hidden: **three of the twelve fixes are exercised by no run available in this environment** (`assessment-live` and `vlm-client-live` gate on `ARGUS_VLM_TEST_URL`/`ARGUS_VLM_TEST_IMAGE`, `dlq-service-live` on `ARGUS_NATS_URL`; with the variables unset the case returns before the runner exists — my own runs of those three report 0 assertions, which is the corroboration), so those diffs are verified by reading only; the `jthread` sites are prophylactic today (no failing assertion sits between creation and join); and this unit's pre-state counts had never been recorded anywhere, so the invariance rests on the no-assertion-line property plus the agent's out-of-tree reconstruction from `HEAD` (12/12 equal), which I did not reproduce.

Two residuals, recorded and not buried: the six inline runners now stop the app at scope exit, i.e. after locals declared below them die, where the old trailing pair ran before those locals (`retry-lifecycle` is the affected suite — green 20/20, and a case about teardown, so nothing hangs); and one project build failed once with `49 - vlm-client-test (BAD_COMMAND)` because the `packages/clients/vlm` agent was relinking that binary inside guard's tree during ctest — interference, not a defect, and the re-run came back clean (49/49, `0 Not Run`).

#### `services/llm` — verified

Three files, one case-local `AppRunner` each: `services/llm/tests/unit/encounter-closed-consumer-test.cc`, `.../encounter-closed-live-test.cc` and `.../llm-wire-test.cc`. The unit has no support header and no fixture class — `tests/` is five suites, a `bench/` tool and a `fixtures/` directory — so wave D's whole change in this unit is the destructor body in the three copies, verbatim from the brief, with the class name, the member name, the class-level comment and the rest of each class untouched (each file already included `<thread>` and `<chrono>`; the compile is the check). Per site: the consumer's copy is a plain `AppRunner runner;` in the case body (`:44`/`:49`, used at `:184`); the live suite's is the same shape (`:46`/`:51`, used at `:246`); the wire suite's sits in a `std::optional<AppRunner>` (`:285`/`:290`, emplaced at `:369`, `runner.reset()` at `:524`) so that the stop still precedes the engine's teardown. `git grep` returns exactly those three `class AppRunner` lines, and no `std::thread` anywhere else in the unit's tests beyond each copy's comment line and its `std::thread runner_;`, so there is no second owner to keep in step.

The A/B runs on `encounter-closed-consumer-test.cc` with a temporary `REQUIRE(1 == 2);` immediately after the boot `REQUIRE`, the same injected source in both arms, each run from a fresh `mktemp -d` cwd. Arm 1, the pre-wave-D destructor: **14×exit=1, 6×exit=139**, each of the six dying by SIGSEGV *after* doctest had already printed the failure — no `terminate`, no `CRASHED` line of its own, the process simply never getting past static destruction (the core from the same shape shows the app thread inside `HttpAppFrameworkImpl::run()` → `trantor::EventLoop::loop()` while the main thread is in `exit()` → `~HttpAppFrameworkImpl` → `~EventLoopThreadPool` → `~EventLoopThread`). Arm 2, the wave-D destructor with the same injection: **20×exit=1, no signal** — all twenty reported the failure rather than crashing, which is the branch this wave exists for. The injection was then removed with the Edit tool.

The wire suite's pre-fix count was never measurable, and the unit's report says so plainly instead of asserting it: the pre-wave-D binary (md5 `aa070b43fb9c7fe58528126cf7f01ad3`) was launched for the "before" number and never reached its listener — at 5:57 elapsed its log still ended at `register_device: registered device CPU (AMD Ryzen 7 5825U with Radeon Graphics)`, i.e. still loading the model against a 22 s nominal load — so it was killed there and no before-count exists. What stands in its place is by construction: the file's assertion-macro diff is empty, so no count can have moved. Its own standalone run afterwards gives the count the report was missing: **1 case / 181 assertions, exit 0, 1324 s** (22 min, with `stt-wire-test` and `tts-wire-test` running concurrently; progress was checked without touching the process — a loopback socket pair ESTABLISHED under the test's own pid at 11:51 elapsed, a new local port at 21:29, request legs completing rather than a stall). The first attempt at that run is worth keeping too: exit=1 at 66 s, `llm-wire-test.cc:67` `REQUIRE(::connect(fd, …) == 0)` with `values: REQUIRE(-1 == 0)` and `assertions: 6 | 5 passed | 1 failed` — drogon's `running_` set before the listener accepts, one of the four measured instances of the boot-readiness family this row records and not this row's code. That failing `REQUIRE` reported and exited 1 with no signal, where under the pre-D owner the same unwinding failure was the SIGABRT shape.

Counts: `encounter-closed-consumer-test` 1 case / 25 assertions and `encounter-closed-live-test` (with `ARGUS_NATS_URL`) 1 case / 22 assertions, each unchanged across the edit, and the agent's 20-run repeats of both were 20/20 rc=0 at 25 and 22 assertions respectively. Residuals recorded and not fixed in this unit: `services/llm/src/controllers/llm-controller.cc:247` detaches a production producer thread (`std::thread([job] { runStreamJob(job); }).detach();`, production code outside this row); and none of the five `add_test`s in `services/llm/CMakeLists.txt` carries a `TIMEOUT` property, measured as a stalled suite holding the whole ctest run open — the wire suite's own ctest leg ran 1443.43 s inside the project's set — with CMake changes out of scope.

Verified by me: the diff read clean — no assertion line in it, the destructor's key lines appearing once per file, no injection left anywhere in the unit; the consumer suite at 1 case / 25 assertions and the live suite at 1 case / 22 assertions, three runs each from fresh binaries, all rc=0; and the A/B's raw files recount to exactly the arm figures above (14×exit=1 with 6×exit=139, then 20×exit=1) and predate the wave-D diff (21:35 and 21:38 against the 21:46 diff, with BRIEF-D written 21:31 — timeline consistent). The 181-assertion wire run is now mine as well as the agent's: after the gate, from a scratch directory on the quiet machine, the post-wave-D binary gives **1 case / 181 assertions / 0 failed, rc=0 in 90 s** — the agent's count reproduced independently, and the third measurement of this report's load effect (90 s quiet against the agent's 1324 s at load 88).

#### `services/stt` — verified

One file, one site: `services/stt/tests/unit/stt-wire-test.cc` — the unit's test tree is exactly that file, one `TEST_CASE` — held a case-local `std::thread runner([] { drogon::app().run(); });` with a trailing `drogon::app().quit(); runner.join();`. Wave A/B replaced it with `AppRunner runner;` (the class at `:194`, the use now `:269`) and deleted the pair; wave D replaced the destructor's stop path (`:199-219`, the brief's body verbatim — the file's `199-219` diffs empty against the brief's block and both are 950 bytes) and nothing else, which shifted the declaration by the destructor's seven extra lines and no more. This unit has no fixture class of its own, no support header, no worker thread and no second copy to keep in step, and defect 1 does not occur here (`git grep newSqlite3Client` over the tree is empty).

Arm 1 is a **single** 20-run loop, `/tmp/row6/stt-logs/ab-pre-summary.txt` (binary `services/stt/build/dev/stt-wire-test`, scratch cwd recorded in the file): **15×rc=1, 4×rc=139** (runs 01, 04, 07, 11), **1×rc=134** (run 16) — five signal deaths of 20 — and every one of the 20 printed the same correct doctest verdict (`assertions: 3 | 2 passed | 1 failed`) before the death, so the assertion was reported in all 20 and the death came after it, in teardown. The cores put the crashing thread where the mechanism predicts: the app's own thread inside `EventLoop::loop()` → `doRunInLoopFuncs()` → the `registerHandler` lambda queued during boot → `DrClassMap::getSingleInstance<HealthController>()`, with the SIGABRT twin reaching the `DrClassMap.h:92` singleton assert. Arm 2, the same injected `REQUIRE(1 == 2)` after the boot `REQUIRE` with the wave-D destructor: **20/20 rc=1, zero signals, zero cores** (`coredumpctl list` for the arm's window → 0, against 5 cores in arm 1).

The touched suite's green count is 1 case / **122 assertions** and it did not move — 122 before the destructor replacement, 122 after, and 122 in the project's own ctest run (whose set passed 6/6 with no first-party warning). Its 20-run green loop was run twice. The first loop, on the loaded machine: 13 of 20 rc=0 at 122 assertions each; the other 7 rc=1, every one the pre-existing boot flake — `stt-wire-test.cc:77` `REQUIRE(::connect(…) == 0)` with `values: REQUIRE(-1 == 0)` and `assertions: 6 | 5 passed | 1 failed`, dying in ~23 s against the green runs' 164-214 s — with load average 84.9-91.5 through those runs, and from run 14 on, the moment the leaked spinners were killed and the load fell, every run green and fast. The second loop, on the quiet machine (load 9.96-14.32): **20/20 rc=0 at 122 assertions each**. No run in either loop died by signal and no core came from them.

That flake is the finding this unit hands over, unfixed. Its mechanism was read off the syscalls: `waitForBoot` polls drogon's `running_` (`HttpAppFrameworkImpl.cc:643`), which is set 46 lines before `getLoop()->loop()` (`:689`), so the socket is bound — the suite prints a real ephemeral port, `getListeners()` returns it — while `listen()` has not yet run, and the first request can be refused. A diagnostic-only `LD_PRELOAD` probe (outside the repo, nothing in the unit changed) that logs `bind`/`listen`/`connect` and widens `listen()` by 300 ms proved it at syscall level: the case thread's `connect fd=11 port=45647 ret=-1 errno=111` while the boot thread is still `listen-enter` on fd=10 — ECONNREFUSED with the ordering visible, `rc=1` and no signal. It is recorded unfixed because it is not one of this row's two defects, because the `waitForBoot(isRunning)` idiom is tree-wide and changing it here would diverge this unit from every sibling and alter a shared readiness helper unilaterally, and because papering over a race with a sleep is forbidden. What the row's fix did change is legibility: the flake now reports as a clean failing assertion instead of aborting.

Verified by me: I read the primary `/tmp/row6/stt-logs/ab-green-summary.txt` myself and derived the failure sequence per run (rc=1 at 01, 02, 04, 06, 08, 10, 12; rc=0 at 03, 05, 07, 09, 11, 13), which corrected an earlier draft of this section whose first-thirteen count was both wrong and arithmetically impossible against runs alternating with slow green ones; I read arm 1's primary itself (`/tmp/row6/stt-logs/ab-pre-summary.txt`) and it carries five signal deaths of 20 (rc=139 at 01, 04, 07, 11 and rc=134 at 16), and I resolved a mis-attribution an earlier draft of this section inherited — it claimed stt had a second arm-1 loop in `d-arm1.md`, which is tts's primary (tts's wave-D report cites it as its own and reproduces its lines verbatim), an exhaustive search of `/tmp/row6` finding exactly two files with five `rc=139` lines, tts's and gateway's; stt has one arm-1 loop, the table's figure; and the diff's hunks sit at 191, 269 and 430, so the `::connect` helper's lines (60-90) are untouched by it — the flake is not this row's code. Endgame's own list still carried this unit's arm-2 loop and its green arm as in flight when it was written, so those two loops' figures above are the agent's records, not my runs.

#### `services/tts` — verified

One file, one site: `services/tts/tests/unit/tts-wire-test.cc`. The case-local `std::thread runner([] { drogon::app().run(); });` with its trailing `drogon::app().quit(); runner.join();` became `AppRunner runner;` — the class at `:215`, the declaration in the case body at `:297`, created after the last configuration call (`addListener`, `:295`) and before the boot `REQUIRE` (`:298`) — with the pair deleted, and wave D replaced the destructor's stop path with the brief's body and nothing else. The unit's other thread site, `tts-rpc-test.cc:103`'s `std::jthread canceller`, is not an `AppRunner` and was not touched: its own destructor requests stop and joins and the lambda is bounded by a 1 s deadline. Defect 1 does not occur in this unit at all. `git grep` for `fprintf|1 == 2|decision-loops` over the unit's tests is empty in the final state, which is how the two temporary instruments are confirmed removed.

The A/B uses the unit's case-local shape: a temporary `REQUIRE(1 == 2);` immediately after the boot `REQUIRE`, the same injection in both arms, 20 sequential runs per arm from the same scratch cwd, reverted with the Edit tool afterwards. Arm 1, the pre-fix destructor: **5 of 20 exited 139 (SIGSEGV) with core dumped, 15 exited 1** (`/tmp/row6/d-arm1.md`, whose 20 lines I read: rc=139 at runs 1, 2, 5, 6 and 8), every run reporting the injected failure first. No arm-1 run printed a `terminate` line — the crash is not the joinable-thread destroy but the detached loop running past the end of the process — and the core's crashing thread is the `AppRunner` lambda's thread inside `EventLoop::loop()` → `doRunInLoopFuncs()` → the queued `registerHandler` lambda → `DrClassMap::getSingleInstance<HealthController>()` walking drogon's already-destroyed static registry, the same signature this binary shows earlier the same day and the older SIGABRT form of which goes back to 2026-09-06. Arm 2, the wave-D destructor with the same injection: **20/20 rc=1, no signal, no core**.

Arm 2 carried two temporary `fprintf` lines printed per run, `decision-loops` and `looping-after-wait`, and the label needs stating plainly because it is a misnomer: `decision-loops` samples `isRunning()` **once, before the wait** (`? 1 : 0`), so `0` means the loop was *not yet looping at the decision point* — inside the window, exactly the state in which arm 1's destructor detaches — and not "zero wait iterations". Read that way, **12 of the 20 arm-2 runs decided inside the window**, and in all 20 the loop was looping after the wait (`looping-after-wait=1`), so `quit()`+`join()` took effect: arm 2's clean result is the wait closing the window, not the runs avoiding it.

Counts: the touched suite is 1 case / **92 assertions** before the edit (`base-wire-2.log`, `base-wire-3.log`) and the same after it (`d-wire-green-1.log`), and its 20-run repeat loop is **20/20 rc=0 at 92 assertions each** (Endgame recorded that loop at 13 of 20 green with 92/92 when last read — it was still in flight there and closed at 20/20 in the agent's final record). `tts-rpc-test`, untouched, is 11 cases / 47 assertions across three runs, all rc=0; the project's ctest set is 18/18, exit=0, with 0 warning lines. The pre-existing readiness flake also fires in this suite (`tts-wire-test.cc:113`, `REQUIRE(-1 == 0)` on `connect`) — measured in 3 of ~7 earlier standalone runs under load ~90 and not in the 20 repeats — and is recorded, not fixed: the `waitForBoot(isRunning)` idiom is tree-wide and this row does not change a shared readiness helper unilaterally. One production-side residual is recorded and not fixed: `services/tts/src/feature/synthesis/api/http/controller/tts-controller.cc:112` `std::thread([job] { runStreamJob(job); }).detach();` — a detached stream thread in the tts service's controller, production code outside the test tree and outside this row's scope.

Verified by me: this file's single destructor block is one of the 41 the row's mechanical sweep found byte-identical to the canonical 21-line body, with no injection left in the unit; I read the repeat loop's raw file while it ran (13 of 20 green at 92 assertions each at the moment I read it) and resolved the `decision-loops` label against the instrumented code the agent quoted on request, so the 12-of-20 entry figure stands as printed — the name nearly cost a correct claim; and the counts on both sides of the edit are equal at 92 in the logs the unit's report cites. The A/B arms and the 20-run loop are the agent's runs, not mine — of their primaries I read `d-arm1.md` in full and its 20 lines give the arm-1 distribution above.

### Wave C — `packages/lib/cert`, `packages/clients/vlm`

#### `packages/clients/vlm` — verified

Defect 1 does not occur here (confirmed by grep, not assumed). Defect 2 has exactly one site,
addendum shape 2: `FakeVlmService`, the suite's static fixture, owned the app's run thread and
its destructor was `quit(); if (joinable) join();`. The pinned Drogon 1.9.13 source confirms
why that can block for ever — `HttpAppFrameworkImpl::quit()` is a no-op unless the loop is
looping (`:1036`: `if (getLoop()->isRunning() && running_.exchange(false))`). It now has the
canonical destructor, with the class otherwise untouched, and no second owner added.

Verified by me: diff read; suite run three times from scratch cwds → `3 | 3 passed`,
`23 | 23 passed`, the baseline count unmoved, rc=0; and the build-host claim re-measured
independently (only `services/guard/CMakeLists.txt:121` adds the package; camera's ctest file
does not list it; and the two binaries on disk read 15:17 `services/camera/build/dev/vlm/` — the
stale leftover — and 22:32 `services/guard/build/dev/clients/vlm/`, rebuilt by this row's own
builds, against the 16:18 re-home).

**A third, pre-existing defect the agent found and measured, left unfixed and recorded:** under
heavy load the suite fails with an uncaught `drogon::HttpException("Bad server address")`. An
interleaved A/B of two byte-distinct builds (the pre-fix destructor vs the canonical one) gives
**6/40 pre-fix vs 11/40 post-fix** with the identical message — Fisher exact, two-sided
**p = 0.27** — so the edit did not introduce the flake; the rate is load-dependent and no single
number is "the" rate (the same fixed binary measured 9/20 in the 20-run repeat block at another
moment, and 6/40 vs 11/40 is the narrower claim the A/B was run for). Every failing run was
`rc=1`, none a signal. Mechanism: the fixture's readiness check waits on drogon's `running_`
flag (`HttpAppFrameworkImpl.cc:643`) rather than on the listener accepting (`startListening`
runs only after `loop()`, `:689`), so the first request can be issued inside the pre-loop window
and be refused — the same window wave D fixes on the *owner's* side. Fixing it means changing
the fixture's boot protocol — a different family from the two this row removes — so it is
recorded here as a finding for a future row, not patched in this one.

**That family has four measured instances, not one**, which is what makes it a finding rather
than a quirk of a single fixture: `vlm-client-test` (`Bad server address`, above),
`llm-wire-test` (its first standalone attempt died at 66 s with `ECONNREFUSED`, and the run that
succeeded took 1324 s for its 181 assertions), **`stt-wire-test`** (`REQUIRE(::connect(fd, …) ==
0)` with `values: REQUIRE(-1 == 0)` at `stt-wire-test.cc:77`), and the `waitForBoot`-style
helpers the notification and gateway fixtures were found to share. The stt instance is the most
instructive because its 20-run green-arm loop caught the load correlation in a single sequence:
**7 of the first 13 runs failed** (rc=1, ~23 s, `6 | 5 passed | 1 failed` — the suite dies at
its first request), the first two back to back and then alternating with slow green runs
(~200 s), and **from run 14 on — the moment the 73 leaked spinners were killed — every run is
green and fast (23 s)**. The connect helper is
not this row's code: its lines (60-90) are untouched by the diff, whose hunks are at 191, 269
and 430. The pattern lives in eight files (`llm-wire-test`, `stt-wire-test`, `tts-wire-test`,
`vlm-wire-test`, the three fake servers and `cert-san-test`), and the test binds an **ephemeral**
port (measured: `stt-wire-test` listening on `127.0.0.1:39699`), so no daemon can be squatting
it — the race is the bind-before-listen window, not a port collision. It is also the second
reason this row's gate must run on a quiet machine: an unreleased load generator is enough to
turn it into a failure.

#### `packages/lib/cert` — verified

One file, +39/−3; defect 1 is absent (no sqlite client in this suite). One defect-2 site: the case-local `std::thread runner([] { app().run(); });` at `cert-san-test.cc:358`, joinable across `waitForBoot`, `rotateServerCertificate` and the `FAIL`s inside `servedPeer`. It becomes the canonical `AppRunner`, created after the last configuration call (`addListener`) and **before** the boot `REQUIRE` — which is the point of the shape — with the trailing `quit(); runner.join();` deleted, so `CertService::shutdown()` now runs with the app still up and the app stops at scope exit instead.

Verified by me: the diff read; eight runs from scratch cwds → rc=0 every time. **This suite's assertion count is not a usable instrument**: it varied across my own eight runs (76, 76, 69, 76, 76, 76, 71, 71), and the agent's claim that the instability is pre-existing is confirmed against a binary whose source I diffed as **byte-identical to `HEAD`** — its eight runs gave the same range (76, 69, 69, 74, 74, 76, 76, 76). The invariant here is therefore the diff, which adds no assertion statement (the single `CHECK`/`REQUIRE` match in it is a comment) and leaves the support set unchanged.

The residual, worth a future row: with a fatal `REQUIRE` the failure is now reported at once, but the process still exits 134, because the **production** `CertState gState` (`cert-service.cc:57`) holds `std::thread rotationThread` (`:54`), joined only by `CertService::shutdown()` (`:374-378`) — which the unwinding skips — and `CertState` has no destructor, so the implicit one destroys a joinable thread at exit. I re-ran the agent's arms myself: pristine source + `REQUIRE` → **rc=134**, fixed source + `REQUIRE` → **rc=134**, fixed source + an eight-line test-side RAII guard → **rc=1 with no signal**. Since the fixed source no longer contains a case-local thread, the remaining 134 can only be the static, which is what makes the guard's rc=1 the proof of the diagnosis. The guard is proven, not applied: the durable fix belongs in `cert-service.cc`, and this row is confined to the test tree. On the green path nothing aborts, because `shutdown()` runs.

### The completeness check — `services/vlm`, converted last

The row closed its census as a **property rather than a list**: `git grep -l 'drogon::app().run()' --
'*/tests/*'` returns **41 files**, the set of files carrying the canonical owner is the **same 41**
(`comm` empty in both directions, one owner per file), and
`git grep 'std::thread .*drogon::app().run()' -- '*/tests/*'` returns **nothing**. Asked that way,
two files answered that no earlier sweep had looked at: `services/vlm`'s
`vision-remote-adapter-test.cc` (`:151` at `HEAD`) and `vlm-wire-test.cc` (`:250`), each a bare
case-local `std::thread runner([] { drogon::app().run(); });` with a trailing `quit(); runner.join();`.
The row-5 report's own hand-over sentence names this family as row 6's — "a failing `REQUIRE` in a
body holding a joinable `std::thread runner` destroys it while unwinding and `std::terminate()`
fires … so it joins the 18 throwaway-client sites as row 6's family" — and names `llm-wire-test` as
an instance of it, which this row did convert; leaving these two would have been a gap in a family
the row claims to close, in a project the gate builds and runs (`vlm` is one of the eighteen).

Both take the shape `llm-wire-test` already carries, because in each the stop sits **before**
something that must follow it: `std::optional<AppRunner> runner;` and `runner.emplace()` where the
bare thread stood, and `runner.reset()` exactly where the explicit pair stood — in `vlm-wire-test`
ahead of `vlm->shutdownEngine()` and `llama_backend_free()` (the order that teardown always had), in
`vision-remote-adapter-test` ahead of the `FakeVlmServer downServer(503)` half of the case, so the app
stops where it always stopped rather than at the end of the body. `<optional>` added to both; the
diffs are `+43/−3` and `+45/−3`.

Verified by me: `git diff -U0` on both files contains **0** assertion-macro lines — the only removals
are the bare thread, the `quit()` and the `join()` — so no count can have moved, and this unit's
pre-state counts were never recorded anywhere, which makes that property (not a count) the
invariant, as in `services/guard`; the assertion-line instrument reads both at **Δ0**; each file
compiles clean under `-fsyntax-only` with its own compile-database command line; clang-tidy over both
TUs with the gate's check set gives **0 findings inside the diff's hunks** (rule 19, no new triple);
the whole-tree destructor sweep re-run afterwards reads **41 of 41 byte-identical** to the canonical
block with the marker set exactly the `detach()` set (41 = 41); and both suites are green from scratch
cwds — **`vision-remote-adapter-test` 1 case / 33 assertions rc=0** and **`vlm-wire-test` 1 case / 95
assertions rc=0**, both after a targeted rebuild of their own targets.

## Verification

### The suite inventory is measured, not assumed

`packages/clients/*` are **not** projects of `scripts/build-all.sh` (its `PROJECTS` list is
the 7 packages and 11 services of the section 2.3 layout), so the ten client packages have no
build tree of their own: they are compiled and tested inside the services that reach them.
Measured for the one client package this row touched: `git grep "clients/vlm" --
'**/CMakeLists.txt'` hits only `services/guard/CMakeLists.txt:121`, guard's generated
`CTestTestfile.cmake` lists `subdirs("clients/vlm")`, and camera's lists none — **and the
`services/camera/build/dev/vlm/vlm-client-test` binary on disk is a stale leftover** from
before the section 2.3 re-home (`9bdee84`, 16:18) that the agent used as its pre-fix A/B
control precisely because it predates the edit. A stale binary in a build tree is a trap for
verification: `find`-based inventory says camera, the truth is guard, and a measurement taken
against that binary would have measured pre-row code. Hence the ordering in this row's own
verification: the full gate rebuilds every touched target, and the stress measurement runs
after it, never before.

This is also why a `--only <service>` build's ctest set contains other units' suites —
`--only gateway` runs `packages/lib/sqlite`'s `identity-client-test` from its own build tree —
which is interference to expect during the row, not a failure.

### Stress, before and after

The harness runs each suite in a working directory of its own (the suites name their databases
relative to the cwd) and its own `/tmp` through a mount namespace, eight workers at a time
under eight spinners, and greps each failing run for the interposer's `SELF-JOIN` line, so a
failure is attributed rather than guessed. One correction to that method, found by audit of the
pre-state logs: the **baseline** run passed `LD_PRELOAD=/tmp/joinwatch/joinwatch.so` while the
built interposer is at `/var/tmp/joinwatch.so`, so `ld.so` rejected it on every run
(`cannot be preloaded … ignored`) and **zero** baseline logs carry a `SELF-JOIN` line — the seven
baseline aborts are attributed by their own `Resource deadlock avoided` text and cores, which is
sufficient for a crash but is not the watcher. The after-run and the detector control use the
correct path, so their attribution is the interposer's.

**The detector was re-armed and re-proved the day the row closed**, because a zero from an
instrument that no longer fires proves nothing: the unfixed shape run again under the watcher
gave **2 aborts in 160 processes × 50 cycles = 8,000 cycles** (`plain-16-3`, `plain-17-4`),
each with `[joinwatch] SELF-JOIN self=…(EventLoopThread) target=…(EventLoopThread)` and
`pthread_join` in the frames — the recorded rate is 4 in 16,000, so 2 in 8,000 is the same
rate.

### The harness was loading the machine itself — and the load mattered

Found by audit while the wave-D agents ran, and it changes how the numbers above must be read:
the harness launched its spinners as `setsid bash -c 'while :; do :; done'` but cleaned up with
`pkill -f 'setsid bash -c while'`. **`setsid` execs**, so the child's argv is
`bash -c while :; do :; done` — the word `setsid` never appears in it, the pattern matched
nothing, and every spinner outlived the run. **73 orphans, ppid 1, 5h32m old, ~36% CPU each
(~26 cores)** were found at 22:42 on a 16-core machine and killed by hand; load average was
**88-90**, falling to 36 within minutes of the kill.

Three consequences, in order of importance:

1. Load is a driver of this defect — it is a release-while-callback-pending race — so the
   **pre-state baseline (7 aborts in 5,280 runs) and the 2/8,000 detector control were both
   measured on the inflated machine**, not on the machine the harness describes (eight spinners).
   They stay valid as *rates* but they are not the after-run's condition.
2. Therefore the after-run's zero only means something **together with a control taken at the
   after-run's own load**: the unfixed shape is re-run in the same set-up (same `SPINNERS`) at the
   endgame, and if it still aborts there, the zero is a property of the fix rather than of a quiet
   machine.
3. It is also part of the environment the `vlm-client-test` flake and the starved `llm-wire-test`
   (1324 s for 1 case / 181 assertions) were measured in — the flake's own attribution (the
   fixture declaring ready on drogon's pre-loop flag) stands, and its load-sensitivity is now
   partly explained. The same suite against the same source on the quiet machine, in the gate's
   own ctest run, takes **79.92 s** — a factor of ~17 — which is what "starved" means here as a
   number rather than an adjective.

The fix is in the harness, not in the tree: the launch records `SPIN_PIDS+=($!)` (kill by pid,
robust even though `setsid` execs) and the tail's sweep is anchored to the child's exact argv
(`^bash -c while :; do :; done$`), which cannot match this script or the shell that launched it —
a trap this session already fell into once, when an unanchored `pkill -f` matched the invoking
shell's own command line and killed it. **Proved, not assumed**: `/tmp/row6/test-spinner-kill.sh`
launches three spinners the same way, reports `alive before cleanup: 3` — the anchored pattern
does see the real argv, the very thing the old one failed to match — and `alive after: 0`, with
`orphans system-wide: 0`.

Baseline (before any edit): **7 aborts in 5,280 runs** — `identity-client` 1, `audit-sync-read`
3, `camera-notifier` 1, `productivity-controller` 2, the other seven suites 0/480 — every one
of them an EDEADLK self-join.

After:

**0 failures in 5,280 runs** — the same eleven suites, the same 480 runs each, the same harness,
with the watcher actually armed this time (`PRELOAD=/var/tmp/joinwatch.so`, the location the harness
accepts and refuses to run without): 23, 4, 68, 11, 9, 18, 69, 7, 18, 8 and 27 s per suite for
`device-credential`, `identity-client`, `camera-action-rpc`, `camera-controller`,
`camera-talk-cutover`, `audit-sync-read`, `camera-notifier`, `gateway`, `notification-controller`,
`push-intent` and `productivity-controller`, `failures: 0` on every line, and `failures.txt` left
empty rather than carrying an unexplained abort.

Two honest qualifications belong with that zero, because the arithmetic alone would overstate it.
First, **the after-run sat on a quieter machine than the baseline**: its eleven suites took 262 s of
suite time against the baseline's 486 s, which is why its per-suite times are lower across the board
(23 s against 49 s, 4 s against 10 s, 68 s against 103 s). The ambient load differed even though both
arms carry the harness's own eight spinners, so "0 vs 7" is not a load-matched comparison by itself —
the baseline was taken while the wave-D agents were still working, and its own `PRELOAD` was the
`/tmp` one that never loaded, so its seven aborts had to be attributed by their abort text and cores
rather than by the watcher. Second, "0 observed at this depth" is not "0 in general"; nothing here
claims the defect is impossible, and the depth is stated with each number.

That is what makes the **control** load-bearing rather than decorative, and it is why it is run at
this run's own load with the same interposer: the unfixed shape — the probe binary that reproduced
defect 1 before any edit — aborted **14 times in 8,000 cycles** (160 processes × 50 cycles), every
one of the fourteen carrying `[joinwatch] SELF-JOIN`, `Resource deadlock avoided` and `pthread_join`
in its log, i.e. exactly the failure this row removes. The instrument that reports a zero above is
therefore demonstrably able to report a non-zero here, at this load, with this `PRELOAD`.

Two numbers about that control are stated rather than smoothed. Its count is **14, not the 16 the
script prints**, because the script counts every `FAIL-*.log` in its output directory and two of
those are leftovers from 20:47 — the fourteen are the ones timestamped 00:15:50-54, and the run's own
`FAILED run` lines name the same fourteen. And 14 in 8,000 is **seven times** the previously recorded
rate (4 in 16,000), which is what a load-driven race does when the load is higher: it is why the
control was re-run at this run's load instead of quoted from the earlier measurement.

The deeper runs close the measurement's second clause. The four suites that produced every one of the
baseline's seven aborts were run **5,600 times each — 22,400 runs — with 0 failures**: `identity-client`
42 s, `audit-sync-read` 202 s, `camera-notifier` 888 s, `productivity-controller` 852 s, same harness,
same eight spinners, watcher armed. At the baseline's own per-suite rates (1, 3, 1 and 2 in 480) the
same depth would be expected to show tens of aborts, so the zero is not an artefact of running too
shallowly — it is the depth chosen to make that objection fail.

What the after-side says as a whole, with its limit attached: a defect that reproduced at 4 in 16,000
cycles on a probe and at 7 in 5,280 runs of the harness does not reproduce in 5,280 runs (0), nor in
22,400 deeper runs of the four suites that carried it (0), while the detector is proved live at the
same load in the same session (14 in 8,000). Nothing here claims the rate is zero in general; the
arithmetic supports "not observed at these depths, under this load, with this watcher".

### The gate

`./scripts/build-all.sh dev`, run after the row's last edit, captured rather than eyeballed
(`/tmp/row6/gate-final/`): **exit=0 in 612 s**, **18/18 projects** built and tested, each with its
own `100% tests passed, 0 tests failed` — 2, 13, 2, 22, 29, 22, 4, 41, 50, 34, 39, 49, 18, 6, 7, 32,
23, 12 tests for `cert`, `socket`, `sqlite`, `identity`, `sync`, `memory`, `intent`, `gateway`,
`camera`, `productivity`, `notification`, `guard`, `tts`, `stt`, `vlm`, `llm`, `voice`, `tunnel`.
**0 errors and 0 compiler warnings**: the log carries 21 `Warning:` lines and every one is a
CMake/Conan notice about a *third-party* tree — `ncnn`, `glslang`, `llama.cpp/ggml` and `openfst`
restating their C++ standard, plus `ccache not found` — the same twenty-one row 5 recorded, and
`grep -c 'warning:'` over the log (the compiler's own form) is **0**.

`check-tidy` closes at **480 TUs, 3156 findings over 45 checks, baseline 3157, 1 check below it**,
with no `risen:` and no `unread:` — that is, the row's own edits leave the ratchet **one finding
below** the baseline rather than at it: it removed one pre-existing `lock_guard` line and converted
two `lock_guard`s this row's own work had added to `scoped_lock`, and a third edit moved a
`push-intent-test` constructor parameter to by-value. `check-deps` prints the same line as row 5,
word for word (`54 declarations, 424 edges, 0 forbidden, 0 cycles, 0 unresolved, 110 edges deferred
to phase 3`), so no forbidden edge or cycle entered the tree with these test edits.

The gate is also what makes the measurements *below* trustworthy: it rebuilds every touched target
in this tree, and the stale-binary trap (a `services/camera/build/dev/vlm/vlm-client-test` left over
from before the re-home) is the proof that measuring an older binary here measures pre-row code.
The stress harness and this report's counts therefore run **after** the gate, never before.

**Fresher than their sources, measured over every registered command rather than spot-checked**: the
twelve touched projects' ctest sets name **302 test commands** (`ctest -N -V` in each project's
`build/dev`), and every binary whose basename matches one of the row's 43 files is newer than that
file — `0` stale, which is the property that makes the twelve standalone runs above evidence about
*this* row's code. Two false leads came out of that check and both are worth keeping, because each
is the kind of thing this report would otherwise have believed: the `camera/build/dev/vlm/`
`vlm-client-test` is genuinely old (15:17) but **is not in camera's test set** — `ctest -N` there
says 50 tests and does not list it, and the live registration is guard's
`build/dev/clients/vlm/vlm-client-test` at 22:32 against a 22:30 source; and a basename collision
(`notification-delivery-live-test.cc` exists in both `services/gateway/tests/` and
`services/notification/tests/unit/`) paired gateway's 22:13 source with notification's 21:43 binary
and printed a staleness that does not exist — each binary is fresh against its own source
(21:43 > 21:40 and 22:16 > 22:13). A filename-keyed instrument over a tree with repeated filenames
reports what it paired, not what is true.

**Standalone** (`--only <project>`, the row's second clause), one project at a time, each with the
gate's own instrument and log:

| project | `--only` verdict | ctest | elapsed |
|---|---|---|---|
| `cert` | `exit=0` | 2/2 | 7 s |
| `identity` | `exit=0` | 22/22 | 9 s |
| `sqlite` | `exit=0` | 2/2 | 2 s |
| `guard` | `exit=0` | 49/49 | 30 s |
| `camera` | `exit=0` | 50/50 | 12 s |
| `productivity` | `exit=0` | 34/34 | 10 s |
| `notification` | `exit=0` | 39/39 | 13 s |
| `gateway` | `exit=0` | 41/41 | 11 s |
| `tts` | `exit=0` | 18/18 | 12 s |
| `stt` | `exit=0` | 6/6 | 17 s |
| `vlm` | `exit=0` | 7/7 | 32 s |
| `llm` | `exit=0` | 32/32 | 95 s |

Every count is the gate's own count for that project, arrived at independently: the twelve runs are
serial, each rebuilt and tested on its own, and all twelve report **0 errors**; the thirteen
`Warning:` lines across them are the same third-party notices the gate carries (`ncnn`, its
`glslang`, `llama.cpp/ggml`, `openfst`, `ccache not found`), none from a compiler. The property this
establishes is narrower than "the suite passes" — it is that each touched project's set passes when
it is the *only* project built and tested, with its own ctest invocation, rather than as one step
in an eighteen-project sequence.

**The capture failed twice before it worked, and both failures are worth recording** because the
second was silent. Round 1 piped each run through `| tee "$OUT/$name/summary.txt"`; every summary
was absent afterwards, and my first diagnosis (that `gate.sh` wipes its output directory) is false —
it only runs `mkdir -p`. The real cause is that `$OUT/$name` is created *by the instrument*, after
the parent shell has already opened the redirect: bash refuses to run a simple command whose
redirection fails, so round 1's `tee` failed (its stderr line was inside the pipeline's own
`tail -60` and never reached me) and round 2 — which redirected instead of teeing — **ran no build
at all while exiting 0**, printing the same `NO SUMMARY` table as round 1. Only round 3, with
`mkdir -p "$OUT/$name"` before the redirect, produced summaries; it is the run tabulated above.
Round 1's evidence survives as each run's own `gate.log` (`/tmp/row6/only-r1/`) and says the same
thing — twelve runs, `All selected projects built and tested (profile: dev)` in each, zero failure
markers, the same counts — so the two independent rounds agree. The lesson is one this report
already applies elsewhere: a capture whose path is created by the thing it captures can fail in a
way that leaves the run *absent* rather than red, and "no summary" is not a verdict.

`packages/clients/vlm` is not a project of `scripts/build-all.sh`, so the client package this row
touched is exercised standalone through `--only guard` — the only project whose `CMakeLists` adds it
(`services/guard/CMakeLists.txt:121`) — and through the `vlm-client-test` in that project's set.

## What this report does not verify

The row is a teardown change in 43 files across thirteen units, and the honest boundary is worth
stating as plainly as the measurements:

- **The wave-D A/B arms are the reporting agents' runs, not mine**, except where a section says
  otherwise (tts's `d-arm1.md` I read in full, and the cert arms I re-ran myself — pristine source
  + `REQUIRE` → rc=134, fixed + `REQUIRE` → rc=134, fixed + a test-side RAII guard → rc=1, which is
  what makes the residual diagnosis a proof rather than a story). For the others I read primaries
  where they existed (stt's arm-1 summary, tts's arm-1 and arm-2 files, gateway's arm logs),
  re-derived the counts, and re-ran the suites; I did not re-run every arm on every unit.
- **Three of `services/guard`'s twelve fixes are exercised by no run available in this
  environment** (two live suites gate on `ARGUS_VLM_TEST_URL` / `ARGUS_VLM_TEST_IMAGE`, one on
  `ARGUS_NATS_URL`), so those diffs are verified by reading and compilation only. This unit's
  pre-state counts were also never recorded anywhere, so its invariance rests on the
  no-assertion-line property and on the agent's out-of-tree reconstruction from `HEAD`, which I did
  not reproduce.
- **Two hang paths are argued from the code, not measured**: `services/camera`'s `Worker` owner
  facing a throwing `std::thread` constructor while another worker is parked, and
  `services/gateway`'s `SharedBoot` constructor-throw path (forcing it means a synthetic throw in a
  function-local static whose failed initialisation would be retried). Both are the agents' own
  stated limits, and both are resource-exhaustion paths rather than the assertion path this row
  removes.
- **The post-fix shapes are not given a rate.** The measurements support "0 observed at this
  depth", not "0 in general": 5,280 runs for the eleven-suite harness and 22,400 for the deep runs
  on the four ex-failing suites, with the detector re-proved in the same harness. The arithmetic
  the rates support is stated where it is used; nothing here claims the defect is impossible.
- **The tidy ratchet's baseline is row 5's, not re-derived here**: this row measures that no check's
  count rose and that no TU came back unread.
- **The boot-readiness race family is recorded, not fixed**, in four instances with mechanisms read
  off syscalls and primaries — one of them (`vlm-wire-test`'s and `llm-wire-test`'s own `connect`
  helper) inside this row's own touched files, whose helper lines the diff does not touch.
- **Six pre-existing defects are carried forward unfixed** and named in their sections: the
  notification `sharedBoot`-static re-boot crash; guard/tts's and llm's production detached stream
  threads; cert's production static `rotationThread`; identity-migration's fixed scratch path (the
  reason this report does not claim the tree is safe under a parallel `ctest -j`); the
  clients/vlm fake's readiness race; and the five `services/llm` `add_test`s with no `TIMEOUT`.
- **The `--only` runs cover the twelve touched projects, not all eighteen.** The untouched six
  (`socket`, `sync`, `memory`, `intent`, `voice`, `tunnel`) are covered by the gate's own run, which
  is a different property (sequential in one build) from a project built and tested on its own.
