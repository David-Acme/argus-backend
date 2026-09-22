# Phase 2 step 5 — rules 16 and 19, and section 2.4's forbidden edges, measured by machines

Scope: the row is "Add `.clang-tidy` (rules 16/19) and `scripts/check-deps.sh` (forbidden edges);
wire both into `build-all.sh` (§4.13)" (`docs/history/plans/architecture-plan.md:821`), read against
§4.13's enforcement table and §2.4's rule 7 ("the mechanical check reads `target_link_libraries`").
Base `9bdee84`. The row's two halves are one gate each, and neither could be written before it was
measured: §2.4's table is checkable by machine, and rules 16/19 describe a property the tree
violates 3,157 times today. No phase is done until `./scripts/build-all.sh dev` is clean; the ledger
is in **Verified**.

## The plan's clang-tidy check set cannot be a clean sheet, and the measurement is why

`cppcoreguidelines-owning-memory`, `modernize-*`, `performance-*` and `bugprone-*` over the tree's
first-party translation units produce **8,363 distinct findings**, and **5,206 of them — 62%, a
family six times the size of the next one — are `modernize-use-trailing-return-type`**. That check
is not about any rule in the plan's list (no owning raw pointers, no C-style casts, no `typedef`, no
`NULL`, no C arrays, no `std::bind`, no `printf`): it asks for `auto f() -> T` where the tree writes
`T f()`, which `.clang-format`'s LLVM base and all 488 first-party headers already decide the other
way. A gate that fails on day one over a style the project has settled is a gate that gets turned
off, so `.clang-tidy` carries the plan's set **minus that one check**, with the reason written into
the file, and enforcement is a **ratchet**: the counts are recorded, a rise fails, and the record
comes down as the findings behind it are fixed — which is rule 19's own sentence ("modernizing
existing code is a normal part of any task that touches it") given a number.

## `.clang-tidy`

```yaml
Checks: >
  -*, cppcoreguidelines-owning-memory, modernize-*, performance-*, bugprone-*,
  -modernize-use-trailing-return-type
HeaderFilterRegex: '/(packages|services)/.*\.hxx$'
FormatStyle: file
WarningsAsErrors: ''
```

The check list is the plan's; the exclusion and its reasoning are the file's longest comment. The
header filter names first-party headers for an editor's clangd/clang-tidy; the scan passes its own
filter rooted at the tree, which is the same set here because the tree's headers are all `.hxx`.
`FormatStyle: file` matters for the fixer rather than the checker: a tidy fix that restyles a file
is noise on top of the fix.

## `scripts/check-tidy.sh` and `scripts/lib/tidy_scan.py`

`check-tidy.sh` is the front door `build-all.sh` calls; the work is in `scripts/lib/tidy_scan.py`:

- **Translation units.** Every project's `compile_commands.json` (globbed at the three depths the
  tree's build directories sit at) is read, and the union of its first-party entries is the work
  list — no `third_party/`, no `/build/`, no duplicates, and no entry whose file is not in the tree.
  480 TUs today, across the eighteen projects, the on-demand tools included.
- **One tidy per TU**, in eight threads, against that TU's own database, with
  `--checks=` taken from `.clang-tidy` itself: an editor and the gate cannot disagree about what is
  being checked because there is one list. Findings are deduplicated by file, line, check and
  message — a header's finding is reported again by every translation unit that includes it, and the
  tree's headers are included widely — so a finding costs what it is worth, not what it was seen
  from; the gate ends by printing the single worst file.
- **The comparison** is `scripts/lib/tidy-baseline.txt`: the tool it was measured with, the TU
  count, and one count per check. The gate fails when a count rises above its baseline (a check the
  baseline does not name has a baseline of zero), when the scan sees fewer TUs than the baseline
  records ("a check that cannot be run is not a check that passed"), and when clang-tidy could not
  analyse a TU at all (`unread:`) — a scan that quietly skipped a file would otherwise look clean.
  `--top N` prints the files carrying the most findings, which is where a cleanup would start.

**The one list is the file's list, and that was measured rather than assumed.** `Checks:` is a YAML
folded block scalar, so the indicator that opens it is part of the raw text a regular expression
captures; the scan strips it (`scripts/lib/tidy_scan.py:84`) and the check set was then compared end
to end — the string the scan passes to `--checks=`, against a bare `clang-tidy --list-checks` that
reads only `.clang-tidy`: **the same 168 checks, with `modernize-use-trailing-return-type` off in
both**, which is the exclusion the file's longest comment exists for. Left as it was, the marker made
the string begin with `>-*` instead of `-*`, which clang-tidy's globber happens to resolve to the
same 168 — measured both spellings — but an accident of glob handling is not a property of the tree,
and a check set is the last thing that should be right by luck. The enabled counts are identical
before and after the fix, so no baseline entry moves.

**The baseline, read back from the committed file** — `tool 22.1.8`, `tus 480`, 45 checks, **3,157
findings**:

| family | checks | findings |
|---|---|---|
| `cppcoreguidelines-owning-memory` | 1 | 6 |
| `modernize-*` | 18 | 2,150 |
| `performance-*` | 8 | 142 |
| `bugprone-*` | 18 | 859 |

Its head: `modernize-use-nodiscard` 790, `modernize-avoid-c-style-cast` 362,
`modernize-use-designated-initializers` 305, `bugprone-suspicious-stringview-data-usage` 302,
`modernize-use-scoped-lock` 296, `bugprone-unchecked-optional-access` 225, `modernize-avoid-c-arrays`
81. No `clang-diagnostic-*` count appears, which is its own result: every TU in the scan **compiles
cleanly** under the check set. The scan costs **5m15s wall, 39m12s CPU** at eight workers, which is
why it runs once per full gate and never in a `--only` run.

**The six rule-16 sites**, which is the rule the row's first half exists for — the gate now holds
them at six and will fail if a seventh appears:

| site | what it is |
|---|---|
| `packages/memory/src/shared/services/extract/extraction-service.cc:20,23` | a `FILE *` initialised from a newly created owner, then used without one |
| `services/camera/src/feature/health/health-rpc-service.cc:44` | a new `grpc::ServerWriteReactor<...>` returned from a function whose type is not an owner |
| `services/voice/src/feature/health/health-rpc-service.cc:44` | the same reactor, the same line, in the other health service |
| `services/voice/src/feature/voice/voice-rpc-service.cc:165` | a new `VoiceSessionStream` into a non-owning pointer |
| `packages/lib/mdns/src/mdns/mdns-service.cc:70` | a legacy resource function called without an owner |

Two of the six are the gRPC reactor raw-news in the two health services, which is the family already
flagged while fixing the gateway's lifetime defects (a reactor or `ClientContext` freed before
`OnDone`): the ratchet does not fix them, it stops the set growing until the phase that owns those
files touches them.

## `scripts/check-deps.sh` and `scripts/lib/check-deps.py`

§2.4's tier table, encoded: `ALLOWED = {1:{1}, 2:{1,2}, 3:{1,2}, 4:{1,2,3}, 5:{1,2,3,4}}` — rule 1
read with the table's "may never" column, so tier 3 never reaches another client and tier 5 never
reaches another service — plus the table's two exceptions (`lib/http` is tier 2 although it is a
`lib/`; `lib/auth` is tier 4 alone), the tier-5 same-unit exemption (a service's own nested
`CMakeLists.txt` is not "another service"), and rule 3 (which is the same-unit rule stated for
services).

**Rule 7 says the mechanical check reads `target_link_libraries`, and row 4 measured that this is
almost none of the graph in this tree**: a first-party edge is written in the `DEPENDS`,
`SYSTEM_DEPENDS` or `MODULES` list of an `argus_*` helper, and the raw target names those lists use
resolve back to the package that declared them (`add_library`/`add_executable` are collected for
exactly that). The check reads both spellings, and reports which one an offence came from:

```
forbidden: T3 -> T5  packages/clients/x  (DEPENDS of argus_clients) -> argus::s  [declared in services/s]  packages/clients/x/CMakeLists.txt:2
```

Edges that touch a package §9.1 has not moved yet (`audit`, `identity`, `intent`, `memory`, `room`,
`socket`, `sync` — nested tools included) have no tier to be judged against until the phase that
moves them, so they are counted, grouped and printable with `--list-deferred`, and never fatal; the
phase that moves them is the one that makes each edge legal or moves it. Cycles are found by DFS and
are fatal, because rule 1 forbids them outright — and the graph is built from **every** edge rather
than from the classified ones, since a tier decides whether an edge is *allowed*, never whether it may
exist, and a cycle through a package §9.1 has not moved yet breaks the same rule. Measured on this
tree: **39 units and 217 edges instead of 32 and 141, and 0 cycles either way**, so the wider reach
costs nothing today and a cycle among the unmoved packages can no longer hide behind its missing tier
(the first draft of the check built the graph the narrow way, i.e. `forbidden: cycle` was print-only
for exactly the packages phase 3 is about to move).

**The first run on the real tree**: `54 declarations, 424 edges, 0 forbidden, 0 cycles, 110 edges
deferred to phase 3 (208 third-party mentions over 23 roots)` — row 4's hand-measured inventory
(54/424/111→110 after row 4's Fix 2 removed the dead `cert`→`identity` edge) reproduced by the
check that now guards it, and **0 forbidden where row 4 found 2**: both violated edges were
re-homed in row 4, and this is the check that would have caught them.

**Reading its own output found three places where a first-party edge could still hide** — the list of
unresolved items, which the check prints as third-party roots, is where a hole in a dependency checker
shows up, and three of the 23 roots were not third-party at all:

- `argus_client_grpc_base` is neither third-party nor a package: `cmake/argus-module.cmake:208`
  declares it as the object library the gRPC clients share, so it has no package to be tiered by and
  stays a mention. The other `$`-rooted mentions are third-party targets named through variables
  (`${ARGUS_NCNN_TARGET}`, `${ARGUS_SQLITE_VEC_TARGET}`, `${ARGUS_LLAMA_TARGETS}`,
  `${ARGUS_SHERPA_ONNX_TARGET}`, `${RNNOISE_LIBRARY}`) — correctly third-party, and mentions for the
  same structural reason: the reading is static. Both kinds are listed here so the next reader knows
  the limit is known rather than missed. (`mdns`, the other single-count root, is `mdns::mdns`, the
  vendored responder, and no reading of it is missing.)
- `$<LINK_LIBRARY:WHOLE_ARCHIVE,gateway-core>` **is** a first-party edge, wrapped in a generator
  expression; it is now unwrapped and resolved (`resolve`), so the item inside a `$<LINK_LIBRARY:…>`
  is an edge like any other, and the count says so: **207 third-party mentions over 22 roots**.
- `argus_service(NAME x)` creates the executable `x` **inside the helper**
  (`cmake/argus-module.cmake:519`), so the package's own file never says `add_executable` and a bare
  service name resolved to nothing — which is precisely the spelling a tier-3 → tier-5 edge is written
  in, the one thing this check exists to fail. `collect` now registers that target like any other.

Neither change moves a number the table is judged by — 54 declarations, 424 edges, 0 forbidden, 0
cycles, 110 deferred, before and after — because the tree has no cross-package edge of either shape
today: `gateway-core` is linked by the service that declares it, and no `CMakeLists.txt` names another
service's executable. They are worth their two lines for the same reason the ratchet is: a check is
only worth what it can see.

## What the review of the two scanners found, and what each fix measured

A code review of the two new checks — the same discipline every unit in this phase gets — returned
seven findings against `check-deps.py`, `tidy_scan.py` and the fixtures. Five were real, one was the
`## Verified` section below (empty until the gate landed), and one was the stray database in `/tmp`
already recorded. Each of the five was verified against the code before it was acted on, and each fix
is measured rather than argued:

- **An item that spelled the first-party namespace and resolved to nothing was counted as a
  third-party mention.** `foreign[("unresolved alias", item)] += 1` — a typo, or a name that had
  changed, went into the same tally as `Drogon::Drogon` and the run stayed green. That is the one
  class of spelling an edge check must not absorb: `argus::` is this tree's own namespace, so a name
  it does not declare is a mistake in the edge. `resolve` now reports those to an `unresolved` table,
  they are printed with the file and line that wrote them, and they fail the run. Measured on this
  tree first: **0 today**, so the change is free here; a fixture that writes `DEPENDS argus::lib::yy`
  now prints `unresolved: packages/clients/x names argus::lib::yy, which this tree does not
  declare  packages/clients/x/CMakeLists.txt:1` and exits 1, where the old code counted it as a root
  and exited 0.
- **The tokeniser split on whitespace only.** CMake separates list items with a semicolon as well, so
  `DEPENDS argus::lib::y;argus::s` was handed to the resolver as one name — which, through the gap
  above, became a foreign mention and hid a tier-3 → tier-5 edge. `tokens_of` now splits on both, and
  never splits a generator expression, which is one item however many semicolons it holds. Measured:
  no dependency token in this tree carries a `;` or a trailing comma today (0 of them), and the
  fixture shows the semicolon form producing the **identical verdict and identical message** as the
  same edge written across two lines (`forbidden: T3 -> T5 … argus::s`, exit 1) — the old tokeniser
  saw `argus::lib::y;argus::s` as a single name.
- **Only tracked `CMakeLists.txt` files were read.** `git ls-files` without `--others` cannot see a
  package that has been written but not staged, and phases 3a–3d are exactly the phases that add
  packages: a new package's forbidden edge would first be checked *after* somebody committed it.
  Measured before switching: `git ls-files --cached` and `git ls-files --cached --others
  --exclude-standard` both list **64** files under `packages/` and `services/` today, with **0** under
  a `build/` or `third_party/` path — the per-package ignore rules already carry `build/`
  (`packages/lib/sqlite/.gitignore:1`), so the switch costs nothing and cannot read a build tree; the
  two directory filters are kept on the git path anyway, since the ignore rules are a convention and
  the filter is not. The fix is pinned by a fixture in a throwaway repository: a package removed from
  the index is still read, and its `forbidden: T1 -> T5` is reported with its file and line, while a
  `CMakeLists.txt` inside an ignored build tree that carries the same edge is not read at all.
- **A run that examined nothing exited 0.** Zero declarations, zero edges, zero forbidden — a
  picture identical to a clean tree, which is the failure mode a gate cannot have.
  `check-deps.py` now exits 2 with `nothing was checked` for a tree with no `CMakeLists.txt` and for
  one with no `argus_*` declaration, the same floor `tidy_scan.py` has carried since its first
  version (`if not jobs: return 2`).
- **`--write-baseline` recorded a baseline over a tree it could not read.** It wrote `tus len(jobs)`
  and the counts it had, whatever `failures` held, and returned 0 — and a baseline is what every
  later run is compared against, so one recorded from a partially analysed tree lowers the floor
  permanently, `tus` ratchet included, since a TU that failed to load is not a TU the baseline knows
  about. It now refuses: it names each unread TU, prints `N of M TUs could not be analysed, so no
  baseline was written` and exits 1. Measured directly on a one-TU fixture whose compile command
  names a header that is not there: `unread: …/z.cc  Error while processing …`, `1 of 1 TUs could not
  be analysed`, exit 1, and the baseline file's md5 unchanged. The suite asserts all three (the
  message, the status, and that the file was not written).
- **The first cycle fixture proved less than it looked.** It asserted a non-zero exit, but its state
  also carried a forbidden tier-1 → tier-3 edge, so a run that found only *that* would have satisfied
  it — the fixture would have kept passing with the cycle detector broken. It is now isolated (two
  tier-1 packages pointing at each other, nothing else in the tree that the table objects to) and
  every negative fixture asserts its **message fragment** through one `expect_rejected` helper, so
  each case proves the rule it is named for. `build-all-test.sh` grew four fixtures with it: the
  semicolon list, the unresolved alias, and the two floors.

The check's summary line now carries the count that decides the new failure — `54 declarations, 424
edges, 0 forbidden, 0 cycles, 0 unresolved, 110 edges deferred to phase 3 (207 third-party mentions
over 22 roots)` — because a reader of the gate's output should be able to see that unresolved names
were looked for, not just that none were found.

## The first gate run was red, and the fault was in the scanner

The row's first full gate reached its last step and failed there — and the line it printed is the
whole diagnosis:

```
check-tidy: 503 TUs, 3157 findings over 45 checks, baseline 3157
check-tidy: worst file services/guard/src/feature/guard/guard-repository.cc (104 findings)
[error] project failed:
```

The ratchet's numbers were exactly right; the **work list** was not. Twenty-three of the 503
translation units are not in the tree at all — nineteen under `packages/argus-common/` and four
under the older `packages/argus-socket/` and `packages/argus-room/` spellings — so clang-tidy was
being handed files that do not exist and answering each with a stack dump, which the scan reported
as `unread:` and counted as a failure. All twenty-three come from **one** compile database,
`packages/socket/build/prod/compile_commands.json`, dated 2026-09-10: a `prod` build tree that
survived the rename of `argus-common` to `socket` and went on listing the sources of the package
Phase 1 step 2 deleted. Nothing else in the tree had that shape (one database, verified by walking
all of them).

The fix is in the tool, not in the cleanup, because any machine can carry a build tree older than
the sources it names: `translation_units()` skips an entry whose file is not in the tree and
**reports it** — `check-tidy: 23 entries skipped in packages/socket/build/prod/compile_commands.json
-- the database names files that are gone; delete that build tree` — since a stale database is build
residue to delete, not a finding about the source tree. Measured against the real trigger before
anything was deleted: the same tree yields **503 TUs without the check and 480 with it**, none of
the 480 missing. The orphaned tree itself (17 MB, untracked, gitignored) was then deleted, the only
one of its kind left after Phase 1 step 3's sweep, and the baseline was re-measured by
`./scripts/check-tidy.sh --write-baseline` against the fixed scanner: it wrote `tus 480` with
**every one of the 45 check counts unchanged** — the twenty-three phantom units had contributed no
findings, only noise, which is also why the red run's own count line was already correct. A fresh CI
checkout has no build trees, so CI would never have seen this — which is the argument for the check
living in the scanner rather than in a one-off sweep.

## Both are wired into the gate

`scripts/build-all.sh` runs `check-deps.sh` **before the Conan install**: it reads `CMakeLists.txt`
files only, so it costs nothing, and a forbidden edge stops the run before a long build starts. The
clang-tidy scan runs at the **end of a full run only** — `[ "$NO_TESTS" -eq 0 ] && [ -z "$ONLY" ]` —
because it needs every project's compile database and a per-project run has to stay quick.

`scripts/build-all-test.sh` grew the assertions that keep the wiring honest: both invocations are
grepped for in `build-all.sh`; a fixture DAG (one lib, one client, one service that declares itself
with both `argus_service` and `argus_module`, the way the real camera→`argus::guard` edge was
written) is legal at 0, forbidden when the client reaches the service by alias, **forbidden again when
it reaches it by the bare target name `argus_service` creates**, and forbidden when two packages form
a cycle — **including a cycle that runs through `packages/memory`, whose tier arrives with §9.1**,
which is the case the narrow graph could not see; a `--only camera` run is asserted **not** to reach
the tidy stage; and a clang-tidy fixture pins the ratchet's behaviour — the version guard refuses a
baseline measured with another major, `--write-baseline` records the tool it used, the scan is green
against what it just recorded, and a compile database carrying an entry for a file the fixture does
not have is skipped and reported instead of handed to clang-tidy.

**CI**: `.github/workflows/ci.yml` runs `build-all-test.sh` and then `build-all.sh dev`, so the two
new gates reach CI by construction — and the runner had no clang-tidy at all, which would have
failed the last step of every run. The workflow now installs **clang-tidy 22 from LLVM's own
repository** (Ubuntu's archive ships 18, and the baseline's counts belong to the tool that produced
them) and prints its version. Unlike the rest of this report, that install line was **not run
here**: this machine's clang-tidy came from outside the archive too, so the step is the standard
recipe for the same major rather than a measurement. If the runner's package is a different major,
the scan says so in one line, naming both versions and the command that re-measures deliberately.

## The documentation sweep

- The plan's §4.13 table: `.clang-tidy` and `scripts/check-deps.sh` move from "**to add** (Phase 2)"
  to "exists", and `scripts/check-tidy.sh` joins the table.
- `AGENTS.md`'s §2.4 prose said the row's check "reads `target_link_libraries` in every
  `CMakeLists.txt`" — row 4's correction, now stated there: both spellings are read, because the
  helper keyword lists are where a first-party edge actually lives.
- Rule 19 gained the paragraph that says the rule is measured, where the numbers live, that a TU
  the scan cannot run is not a pass, and that a baseline count comes down in the change that fixes
  what stands behind it.
- The Build Commands section names the two gates and which selections skip the clang-tidy scan.

## The guard suite aborted under load, and the fixture was racing drogon

The row's first gate reached its last step and failed in the scan (above). Its second run failed
earlier, in `services/guard`: `guard-migration-test (Subprocess aborted)`, `1 tests failed out of
49` — one run in 49 and the run's only failure. The suite standalone is clean (`--only guard` is
49/49), so the abort needs load, and a flake that can stop a phase gate is the kind of defect this
row exists to catch: it was measured, not re-run until green.

**The abort site.** An `LD_PRELOAD` shim around `pthread_join` — `pthread_equal(target,
pthread_self())`, then `backtrace_symbols_fd` — run under a load harness (six detached spin loops,
eight concurrent runs of the test) reproduced it: **1 failure in 480 runs**, and the log names a
thread joining itself:

```
[joinwatch] SELF-JOIN self=140139959621312(EventLoopThread) target=140139959621312(EventLoopThread)
...
terminate called after throwing an instance of 'std::system_error'
  what():  Resource deadlock avoided
[doctest] assertions: 0 | 0 passed | 0 failed |
```

`addr2line` on the same stack resolves it: the self-joining thread is `EventLoopThread::loopFuncs()`,
the join is `~EventLoopThread()` (EventLoopThread.cc:47) inside `~Sqlite3Connection()`, and that
destructor runs because the last `shared_ptr<Sqlite3Connection>` use — the capture in
`Sqlite3Connection::execSql(...)::{lambda()#1}` (Sqlite3Connection.cc:155) — is released by
`EventLoop::doRunInLoopFuncs()` (EventLoop.cc:349), i.e. the statement lambda is being destroyed **on
the connection's own loop thread**.

**The trigger.** That lambda captures `thisPtr = shared_from_this()`. If the client that owned the
connection has already been torn down by the time the loop thread destroys the lambda, the lambda
holds the last reference, and the connection — with the `EventLoopThread` that *is* that thread —
dies on the thread itself. `DbClientImpl::closeAll()` is what opens that door: it swaps
`connections_` out, clears `readyConnections_`/`busyConnections_`, and returns as soon as
`disconnect()`'s promise is set, while the loop thread is still inside the batch that carried the
statement.

The test reached it through its fixture: `seedLegacyDb` built the legacy database with a **throwaway
drogon client** (`DbClient::newSqlite3Client(...)`, statements by `execSqlSync`, destroyed at scope
exit) — destroying a client right after its last statement is precisely the window. A 25-line program
that does nothing else (`newSqlite3Client`, three `execSqlSync`, scope exit, 50 cycles per process)
reproduced the same abort with the same signature: **7 self-joins in 16,000 cycles** across two
8,000-cycle batches under load. And **0 in 8,000 cycles** when the client is destroyed 5 ms after the
last statement instead of immediately — the window is the loop thread's unfinished batch, not the
statement. Idle, the same binary never fails (400 runs of it, 0 aborts), which is why a standalone
suite never shows this.

**The fix.** `seedLegacyDb` now writes the legacy database through the **sqlite3 C API**
(`sqlite3_open` + `sqlite3_exec`), the way the other four migration suites in the tree already write
theirs (`identity-`, `camera-`, `notification-`, `productivity-migration-test`); guard was the only one
of the five that built its fixture through the client stack it was about to hand the database to. No
client, no loop thread, no window. Verified by re-running the reproduction against the built test:
**5,600 runs under the same harness, 0 failures** (2,000 at eight-way and 3,600 at twelve-way
concurrency on this 16-core machine, same six spin loops) where the unfixed binary gave 1 in 480. The
samples are independent, so if the rate had been unchanged the chance of seeing none is
`(479/480)^5600 < 1e-5`. The suite stays green: `--only guard` → 49/49.

**Where the defect lives, and what still carries it.** The abort is drogon's, not the tree's: a sqlite
client destroyed soon after a statement can release its connection's last reference before the
connection's loop thread destroys the lambda that holds it, and `~EventLoopThread()` then joins the
current thread. It is upstream's code and outside this row, and it is reported here as measured
rather than patched. Eighteen further construction sites in eleven test files keep the same shape,
in six units this row does not own — `lib/sqlite` (`identity-client-test.cc`, already flagged as
aborting in step 2b's report), `identity` (`device-credential-test.cc`), `camera`
(`camera-action-rpc-test.cc`, `camera-controller-test.cc`, `camera-talk-cutover-test.cc`), `gateway`
(`audit-sync-read-test.cc` ×4, `gateway-test.cc` ×2, `camera-notifier-test.cc`), `notification`
(`notification-controller-test.cc`, `push-intent-test.cc`) and `productivity`
(`productivity-controller-test.cc` ×3). Each carries the same window at the same order of magnitude
and can stop a phase gate the same way; the mechanism, the rate and the fix are the ones above. They
belong to the row that makes each unit's tests standalone.

**Step 2b's mechanism for this signature is corrected here.** That report attributed an
identical-looking abort in `lib/sqlite`'s `identity-client-test` to a `SharedMutex` shared-then-unique
self-lock. Measured on this toolchain (glibc 2.43, GCC 16.1.1), a `std::shared_mutex` taken shared and
then unique by the same thread **hangs**; it cannot raise `std::system_error("Resource deadlock
avoided")`, which is `std::thread::join()` refusing to join the calling thread. EDEADLK in this tree
means a self-join, and every instance found so far is the one above.

## The vlm and llm suites aborted because Drogon force-closed a connection mid-inference

The row's third gate run failed in `services/vlm`: `vlm-wire-test (Subprocess aborted)`, `1 tests
failed out of 7` — CTest's count of the project's seven binaries, the other six passing in 0.01–0.06 s
each, the project at 314.85 s — with doctest inside that binary reporting one test case, 41
assertions, 40 passed, 1 failed. **It is not a load-only flake**: the same binary re-run standalone,
no gate and no other process on the machine, fails identically (`41 | 40 passed | 1 failed`, EXIT=134).

**What failed.** The failing assertion is the test's own client, at `vlm-wire-test.cc:104`:
`REQUIRE( split != std::string::npos )` — the socket read reached EOF without ever seeing a response
head, i.e. **the server closed the connection without writing a response**. The MESSAGE lines that
made it out bracket the moment: the first HTTP describe answered in **47,058 ms** standalone
(45,555 ms under the gate), the cached repeat in 78 ms, and the call that failed is the next one — a
fresh, uncached describe with the default prompt — whose MESSAGE never printed. The person prompt
returns "No." in one token; the default prompt asks for a description, so it generates far more. The
connection died before that answer existed, and the client had nothing to read.

**Why, read out of the two libraries rather than inferred.** The handler is a coroutine —
`VlmController::describe` awaits `service_.describeMatAsync(...)` — so the IO thread is free while
inference runs and the connection has **no read and no write**. Drogon hands its idle budget to
trantor in the `HttpServer` constructor (`lib/src/HttpServer.cc:86`:
`server_.kickoffIdleConnections(HttpAppFrameworkImpl::instance().getIdleConnectionTimeout())`), and
the default is **60 seconds** (`HttpAppFrameworkImpl.h:694`: `size_t idleConnectionTimeout_{60}`).
trantor's timing wheel holds the last `shared_ptr<KickoffEntry>`, whose destructor calls
`conn->forceClose()`; the entry is re-inserted only by `TcpConnectionImpl::extendLife()`, and that is
called from exactly three places — `readCallback` (line 173) and the two write paths (lines 688, 760),
throttled to once per second. A handler that is busy has neither a read nor a write to reset it, so
the budget the request is judged against is **the inference's own duration**. trantor's only way to
pause the wheel is an explicit `sendAsyncStream(disableKickoff)`, which an ordinary handler does not
use. Drogon's own header says it plainly (`HttpAppFramework.h:1178`): "the lifetime of the connection
without read or write … in seconds. 60 by default."

**The experiment that proves it, in both directions.** Setting `setIdleConnectionTimeout(5)` in the
test — one line, nothing else — moves the failure to the **first** describe: 23 assertions in, the
same `REQUIRE` at `:104`, the same SIGABRT, the same EXIT=134. At the 60 s default the same binary
gets through two describes and dies on the third (41 assertions, the one above). The knob decides
which request is cut off, so the cause is the budget and not the image, the engine or the client, and
the failing request is simply the first one whose generate exceeds it.

**The fix** is the one this tree already made for the same defect class:
`services/llm/src/main.cc:140-141` sets 600 right after `loadConfigJson` with the comment "A
whole-emitting tool loop outruns Drogon's 60 s idle default (f8-b4)". `services/vlm/src/main.cc` now
does the same, for the same reason, and so does the wire test, because it drives real inference
through a real HTTP server. 600 s is twelve times the measured worst case (47 s). The value is a
`size_t` in seconds and `0` would mean never — 600 is deliberate rather than unlimited.

**The same defect was waiting one project later, in `llm`, and the gate found it next.** With `vlm`
fixed, the run failed in `llm`: `llm-wire-test (Subprocess aborted)`, `1 tests failed out of 32`, the
other thirty-one passing. Identical signature — `REQUIRE( split != std::string::npos )` at
`llm-wire-test.cc:96`, 40 assertions with 39 passing — in the request after the first chat, whose
MESSAGE printed and whose successor's never did. `services/llm/src/main.cc` has carried 600 since
f8-b4, but the **test's own app** never did, and this test drives a real 1.2B model through HTTP:
164 s of run, 22 s of it model load. The 5 s arm moves it the same way, 15 assertions earlier (25
assertions, death at the first chat, nothing printed), so the budget decides here too. Fixed with the
same line and the same comment, and the two MESSAGE lines now carry elapsed milliseconds — the first
chat and the history chat — so the margin this test lives on is visible in every run rather than
inferred.

**The abort is a property of the test, not a second defect, and llm's own crash proves it.** `test
case CRASHED: SIGABRT` together with `terminate called without an active exception` is not drogon:
every in-process suite here runs the app on a `std::thread runner(...)` local inside the test body,
and a *failing* `REQUIRE` throws out of that body, so unwinding destroys a joinable `std::thread` and
`std::terminate()` fires. The four-line doctest program written to test that claim reproduces the
message, the `CRASHED: SIGABRT` line and EXIT=134 exactly — and llm's failure has llama.cpp's own
terminate handler attach a debugger and print the frames (`std::terminate()` ←
`std::thread::~thread` at `std_thread.h:184` ← `DOCTEST_ANON_FUNC_14 () at llm-wire-test.cc:472`),
which is the same mechanism in a real binary rather than a probe. So one failed assertion in any of
these suites reports as a red *abort*, which is what made both of these read like a crash: the shape
is in every wire test and in the migration suites, and it belongs with the row that makes each unit's
tests standalone (row 6), next to the eighteen throwaway-client sites above.

**What the other wire tests are, measured rather than assumed** — my first reading of this table was
wrong twice and the correction is the point. `stt-wire-test` and `tts-wire-test` do **load real
engines** (`SttService::instance().init()` with `REQUIRE_MESSAGE(isLoaded(), ...)`), so "they only
sleep 10 ms" is false — that sleep is `waitForBoot`'s poll. What separates them from vlm and llm is
how a handler *emits*: stt and tts stream, and every write on the way resets trantor's wheel
(`extendLife()` is called from both write paths), while a describe or a chat writes nothing at all
until the answer is complete — llm's own f8-b4 comment says it ("a whole-emitting tool loop"). The
criterion for this defect is therefore whole-emitting handlers, the two instances here are vlm and
llm, and both are fixed; a non-streaming long handler in stt or tts would need the same line, and
their rows are the place to notice it.

## The push-intent case aborted on a locked database, and the fixture was not using the tree's bootstrap

The row's third gate run went red one project after llm, and the third instance is the one that looked
least like a test problem: `push-intent-test` in `notification` (project 11) aborted with
`test cases: 4 | 3 passed | 1 failed`, `assertions: 28 | 28 passed | 0 failed` and
`terminate called without an active exception`, preceded in the same second by
`ERROR Transaction roll back error - TransactionImpl.cc:175`. Standalone it is a flake — **6 aborts in
30 runs**, and the assertion count at the abort moves between instances (26, 28), which is the first
sign that the failure point is not fixed.

The frames say the same thing the vlm and llm aborts said, on the main thread:
`std::terminate()` ← `std::__terminate` ← `std::thread::~thread (this=0x7fffffffce50)` ←
`DOCTEST_ANON_FUNC_20 () at push-intent-test.cc:317`. Two questions answered in gdb at that frame:
`p &runner` is the **same** `0x7fffffffce50` and `p runner.joinable()` is `true`, so the object being
destroyed is the case's own runner and the `runner.join()` on the line above never ran; `&service` is a
different address, so no member thread is involved. doctest reports `CRASHED: SIGABRT` and never
`THREW exception`, so the abort happens *during* the unwinding of an exception that is already in
flight, inside the case frame, before doctest's own `catch(...)` (doctest.h:7041, after
`catch(const TestFailureException&)`) could report it — `TestFailureException` is an empty struct
(doctest.h:1325), not a `std::exception`, which is also why a diagnostic `catch (const std::exception&)`
can be wrapped around a case body without swallowing a failed assertion.

So the body throws and something below it must not be joinable. The exception is caught by wrapping the
body after `waitForBoot` — `catch (const std::exception& exc)` printing `typeid(exc).name()` and
`exc.what()` — and the first instrumented run printed it:

```
PI-MARK caught on main: type=N6drogon3orm8SqlErrorE what=database is locked
PI-MARK runner still joinable, tearing down
ERROR Transaction roll back error - TransactionImpl.cc:175
```

`drogon::orm::SqlError("database is locked")`: SQLITE_BUSY. The case keeps **two connections to one
SQLite file** — its own `newSqlite3Client` (`push-intent-test.cc:188`, the connection the case's
statements run on — sixteen of them before this fix adds a seventeenth — including the deliberate
`ALTER TABLE ... RENAME` faults that are the point of the case) and the app's client, registered by
`addDbClient` and used by the service through `DbService::client()`. Neither carried the tree's
pragmas. The per-boot pragma list is where `PRAGMA busy_timeout = 5000` and
`PRAGMA journal_mode = WAL` live (`packages/lib/sqlite/src/sqlite/db-service.cc:76`), and
`DbService::applyPragmas()` is
what every service in this tree runs at boot (`camera/src/main.cc:279`, `gateway:462` and `474`,
`guard:333`, `notification:139`, `productivity:115`); without it, a statement that meets the other
connection's write lock is answered **immediately** with SQLITE_BUSY instead of waiting. The two
throw points the earlier runs showed land after the case's assertion 16 (`CHECK(commandThrew)`) and
after 18 (the count that follows `CHECK(threw)`, `CHECK(threw)` itself being 17) — in both instances
on the `ALTER TABLE ... RENAME` that **puts the table back**, which is exactly when both connections
write in quick succession, and the count moving between 26 and 28 is why the abort read as a second
service defect.

That the throw lands in the first window is confirmed by the debris an abort leaves behind: `TempDb`'s
destructor never ran, so the database file survives in the tree root, and the one these runs left
(`push-intent-test-2343769-0.db`, read with `sqlite3 .tables` and moved out of the tree) holds
`notification`, `notification_delivery` and **`notification_command_backup` — with no
`notification_command`**: the file as it stood between the fault and the restore, which is exactly
where a 26-assertion abort stops. The database's own last-written page, not the assertion count, is
what says the throw is on the restore.

The fix is three lines of intent in the fixture, at the layer that owns the race:

- `DbService::applyPragmas(client)` on the case's own client, immediately after it is created — the
  tree's per-connection bootstrap on the connection the test drives.
- `drogon::app().registerBeginningAdvice([] { DbService::applyPragmas(); })` — the *service's* client
  is created inside `run()` (`dbClientManagerPtr_->createDbClients(ioLoops)`,
  `HttpAppFrameworkImpl.cc:631`, before `running_ = true` at `:643`), so its pragmas can only go on at
  boot; that is what `notification/src/main.cc:139` does, and the same call now covers the app here.
- an assertion that the intent holds, because the pragma is per connection and `applyPragmas` swallows
  its own failures with a `LOG_WARN`: `CHECK(client->execSqlSync("PRAGMA busy_timeout")
  .front()["timeout"].as<int64_t>() == 5000)`.

Measured: **0 failures in 50 runs**, where the unfixed binary gave 6 in 30 — at 20 % the chance of 50
clean runs is `0.8^50 < 2e-5`. An intermediate attempt measures something worth keeping: pointing the
fixture at the app's client with `DbService::client()` before the app runs aborts **50 times in 50** on
drogon's own assert, `DbClientManager.h:37: Assertion 'dbClientsMap_.find(name) != dbClientsMap_.end()'
failed` — `addDbClient` only registers a config, and the client exists from `createDbClients` onward.
That is also why the schema stays on the fixture's own connection: the clients are created before
`running_ = true`, but the beginning advices are *queued* after it (`HttpAppFrameworkImpl.cc:674`), so
a client fetched at boot is not yet certain to have had its schema applied.

The teardown is the second half of the same signature and is fixed as a shape. The case ran the app on a
`std::thread runner` local and stopped it at the end, so **any** throw inside the body destroyed a
joinable thread and called `std::terminate` — an abort with no assertion to read, which is how this
spent three gate runs reading like a second service defect. A local `AppRunner` now owns the thread and
stops it in its destructor. Drogon's `quit()` cannot be reached before the loop is looping — it is
gated on `getLoop()->isRunning()` (`HttpAppFrameworkImpl.cc:1036`, `EventLoop.h:272`) — so the
destructor waits for the loop and detaches a boot that never got there instead of joining it for ever;
trantor's own `EventLoop::quit()` is safe at any time (`quit_.store`). Proven in both directions: with
a `throw std::runtime_error("guard probe")` at the end of the body the case reports
`ERROR: test case THREW exception: guard probe` with **39/39 assertions passed and no signal at all**,
and without it the 50 runs above are clean. `notification-controller-test.cc`'s `seedNotificationDb`
keeps a throwaway client of the same family and is left to row 6 with the other seventeen sites.

## Verified

`./scripts/build-all.sh dev` — exit **0**, 18/18 projects, every one of them `100% tests passed, 0
tests failed`, **405 tests** in the ledger (2, 13, 2, 22, 29, 22, 4, 41, 50, 34, 39, 49, 18, 6, 7, 32,
23, 12 in project order). Its last step is the scan:

```
check-tidy: clang-tidy 22.1.8 (/usr/bin/clang-tidy)
check-tidy: 480 TUs, 3157 findings over 45 checks, baseline 3157
check-tidy: worst file services/guard/src/feature/guard/guard-repository.cc (104 findings)
```

No `risen:` line and no `unread:` line, which is what rule 19's ratchet passing means. The scan was
then re-run on its own after the `--write-baseline` refusal landed — the gate ran the scanner as it
stood a few minutes earlier, and the change is confined to a branch the gate never takes — with the
identical three lines and exit 0, so the numbers above belong to the committed file.

`./scripts/check-deps.sh` — exit **0**:

```
check-deps: 54 declarations, 424 edges, 0 forbidden, 0 cycles, 0 unresolved, 110 edges deferred to phase 3 (207 third-party mentions over 22 roots)
```

`./scripts/build-all-test.sh` — `build-all tests passed`, exit **0**: the tier fixtures (legal, three
forbidden shapes, two cycles, the unresolved alias, the two floors), the tidy fixtures (version
guard, `--write-baseline`, the stale database entry, the refusal to record a baseline over a TU it
could not read), the `--only`/`--no-tests`/`--install-only` wiring and the single dependency
resolution.

The three defects the gate found, each with its own measurement above: guard **49/49** inside the
full run plus **5,600 runs, 0 failures** against the rebuilt suite (the unfixed binary: 1 in 480);
vlm **7/7** and llm **32/32**, where both wire tests used to abort past Drogon's 60 s idle budget;
notification **39/39** plus **0 failures in 50 runs** of the push-intent case (the unfixed binary: 6
in 30), and the `AppRunner` shape proven by a deliberate throw that reports `THREW exception: guard
probe` with 39/39 assertions and no signal.
