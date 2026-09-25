# Phase 5 step 1 — per-service build, tests and isolation

Plan row: `docs/history/plans/architecture-plan.md` "1 | Per service: 0
errors, 0 warnings, its unit tests green, `--test-case`/`--order-by` isolation
checked".

## What the row asks for, and what answering it turned up

The row is a sign-off: for each of the fifteen projects, prove the build is
clean, the registered suites pass, and a test case still passes when it is the
only case running and when the cases run in another order.

The first measurement said the tree was green. The second said the same — and
both were taken the way the row's own words invite: `--order-by=rand`. That
flag is *deterministic*: doctest seeds its shuffle from a fixed default, so
every "random" run of a binary is the same permutation, and the drill proved
one order per binary rather than an order axis. Measured, then repaired:
`--order-by=rand` twice gives identical case order, while `--rand-seed=N`
selects others.

Re-running the axis that the project's own stability protocol names
(`docs/operations/build-and-test.md`: "20 consecutive runs per binary, plus 50
per ordering (`--order-by=name`, `--order-by=name --reverse`, random seeds)
for any case that ever flaked") found **five order-dependent suites** that the
default-seed drill had missed, two of them failing the deterministic
`--order-by=name` orderings as well — no seed required. All five are
test-isolation defects in first-party test sources: a case inheriting a config
key, a config file, or rows another case left behind, and one case whose
SQLite URI arming silently no-ops once another case has booted SQLite in the
same process. Five test sources and one library source were edited to fix
them; the evidence below is taken on the fixed tree, and the pre-fix failures
are quoted with the mechanism that produced them.

Four measurements were taken, all under the build lock on the tree these fixes
leave behind:

1. **A fresh compile of every first-party translation unit.**
   `build-all.sh dev` on an unchanged tree compiles nothing, so a "0 errors, 0
   warnings" claim inherited from an earlier run would have been vacuous.
   Every first-party `.cc` was touched, which forces every first-party object
   to be rebuilt while leaving vendored objects cached.
2. **An isolation drill over every registered test binary**: baseline,
   `--order-by=rand`, then `--test-case=<name>` one case at a time.
3. **The order axis, exercised as an axis**, not as one shuffle: the two
   deterministic name orderings and four `--rand-seed` values over every
   registered binary, plus a permutation-distinctness check that pins what
   "random" means in this harness.
4. **The stability protocol of `docs/operations/build-and-test.md`** over
   every binary the edits reach: 20 consecutive runs each, plus 50 runs per
   ordering — `--order-by=name`, `--order-by=name --reverse` and 50 distinct
   random seeds — for the suites that had flaked.

## Measurement 1 — a fresh compile of every first-party translation unit

`build-all.sh dev` on an unchanged tree compiles nothing, so a green "0
errors, 0 warnings" inherited from an earlier run would be vacuous. The 553
tracked first-party `.cc` files were touched immediately before this run, so
every first-party object was rebuilt against cached vendored ones. The
orchestrator's own gates ran first, on this tree:

| Gate | Result on this tree |
|---|---|
| `check-comments` | 1401 files checked, 0 comments |
| `check-deps` | 135 declarations, 911 edges, 0 forbidden, 0 cycles, 0 unresolved, 0 edges deferred to phase 3 (298 third-party mentions over 20 roots) |
| `check-tidy` | check-tidy: clang-tidy 22.1.8 (/usr/bin/clang-tidy) / check-tidy: 553 TUs, 2873 findings over 45 checks, baseline 2901; 11 checks below it / check-tidy: worst file services/guard/src/feature/guard/guard-repository.cc (104 findings) |

15 projects, 1269 object builds, 0 `warning:` and 0 `error:` on a first-party
path, 458 registered tests, 0 failed, 284.74 ctest seconds:

| Project | Objects | Warnings | Errors | Tests | Failed | Seconds |
|---|---:|---:|---:|---:|---:|---:|
| cert | 4 | 0 | 0 | 2 | 0 | 5.03 |
| sqlite | 8 | 0 | 0 | 2 | 0 | 0.07 |
| auth | 97 | 0 | 0 | 31 | 0 | 5.59 |
| identity | 123 | 0 | 0 | 36 | 0 | 8.49 |
| sync | 125 | 0 | 0 | 49 | 0 | 3.22 |
| camera | 170 | 0 | 0 | 53 | 0 | 5.20 |
| productivity | 117 | 0 | 0 | 35 | 0 | 3.66 |
| notification | 106 | 0 | 0 | 41 | 0 | 8.74 |
| guard | 124 | 0 | 0 | 57 | 0 | 23.10 |
| tts | 76 | 0 | 0 | 29 | 0 | 12.39 |
| stt | 40 | 0 | 0 | 15 | 0 | 24.99 |
| vlm | 46 | 0 | 0 | 17 | 0 | 44.88 |
| llm | 120 | 0 | 0 | 44 | 0 | 121.95 |
| voice | 73 | 0 | 0 | 32 | 0 | 5.94 |
| tunnel | 40 | 0 | 0 | 15 | 0 | 11.49 |
| **15 projects** | **1269** | **0** | **0** | **458** | **0** | **284.74** |

The 1269 object builds cover 552 of the 553 tracked sources — a shared source
is compiled once per project that links it, which is why the object count
exceeds the source count. The one tracked source no project compiles is
`services/auth/tools/migrate-auth/migrate-auth.cc`: `argus-migrate-auth` is an
`EXCLUDE_FROM_ALL` executable and `build-all.sh` lists extra targets only for
identity, camera, productivity, notification and sync. Its library
(`auth-migration.cc`) is built with its project; the tool that links it is
not, and nothing else invokes it — the deploy stack runs the other five
migrators and copies them into their images, while the auth image carries
`argus-auth` alone. So the row's 0/0 covers 552 of 553 first-party sources
rather than all of them. Recorded, not changed: the five test sources this row
fixed are the row's work, and deciding whether a one-shot migrator belongs in
the orchestrator is a different unit's call.

## Measurement 2 — the isolation drill

Method, per registered binary: run it as ctest does — from the binary's own
directory, which is what ctest sets as the working directory; run it again
with `--order-by=rand` and compare the doctest case and assertion totals with
the baseline; then run cases one at a time with `--test-case=<name>` — every
case of a binary whose baseline is at or under 1.5 s, and a first/middle/last
sample of a heavier one. A case run counts as isolated only when doctest
reports **exactly one case**, which is the check that makes the drill honest
(see the comma trap below).

| Project | Registrations | Cases | Assertions | Isolated runs | Order diffs | Alone failures |
|---|---:|---:|---:|---:|---:|---:|
| auth | 31 | 168 | 4054 | 157 | 0 | 0 |
| camera | 53 | 273 | 5345 | 265 | 0 | 0 |
| cert | 2 | 9 | 117 | 9 | 0 | 0 |
| guard | 57 | 309 | 5997 | 301 | 0 | 0 |
| identity | 36 | 166 | 4994 | 158 | 0 | 0 |
| llm | 44 | 215 | 4655 | 202 | 0 | 0 |
| notification | 41 | 194 | 4280 | 171 | 0 | 0 |
| productivity | 35 | 163 | 4420 | 155 | 0 | 0 |
| sqlite | 2 | 10 | 61 | 10 | 0 | 0 |
| stt | 15 | 85 | 2899 | 67 | 0 | 0 |
| sync | 48 | 203 | 4888 | 195 | 0 | 0 |
| tts | 29 | 161 | 4086 | 145 | 0 | 0 |
| tunnel | 15 | 95 | 597 | 84 | 0 | 0 |
| vlm | 17 | 109 | 2972 | 89 | 0 | 0 |
| voice | 32 | 161 | 2106 | 155 | 0 | 0 |
| **15 projects** | **457** | **2321** | **51471** | **2163** | **0** | **0** |

457 binaries rather than 458 because one registered binary is not a doctest
suite at all (`golden-sync-test`, below) — 457 registrations over 161 distinct
suites, since a package suite is compiled into every project that links it.
The 2163 isolated runs cover 637 distinct case names; the remaining 158 case
slots all live inside the 30 registrations sampled for cost (they hold 232
cases between them), so no case was skipped outside a stated sample — every
sampled registration is listed in the appendix. That boundary is a timing
rule, so it lands where a pass measures it: between the pre-fix and fixed
passes five `mdns-service-test` registrations crossed it in opposite
directions (three from all-eleven to three, two back), which is the whole of
the difference between the two passes' run counts — 2171, then 2163 — and the
set of distinct case names the drill ran is exactly the same 637 both times,
so nothing lost coverage. The heaviest suites are `llm`'s `llm-wire-test`
(75.51 s, 2 cases), `vlm`'s `vlm-wire-test` (29.47 s, 1 case), `llm`'s
`llm-rpc-test` (25.10 s, 16 cases). Per-project ctest seconds sum to 284.74
over the fifteen projects.

### The comma trap, which would have made the drill lie

doctest reads `--test-case=<value>` as a comma-separated list of filters, and
45 of the tree's 685 case names contain a comma ("a bad camera id is refused
locally, an unreachable one is not") — 685 being the distinct names the 458
registrations list, counted over `--list-test-cases` (686 lines, one of them
`golden-sync-test`'s `SKIP:` line, which is not a case). Passed unescaped,
such a filter matches nothing — and a run that matches nothing is still
**SUCCESS**: 0 cases, 0 assertions, exit 0. The drill escapes `,` and `\` in
every filter and requires the run to report exactly one case, so a filter that
matches nothing is a failure rather than a pass. Measured on
`services/camera/build/dev/clients/camera-actions/camera-action-client-test`:

| Filter | doctest summary | Exit |
|---|---|---|
| `--test-case='a bad camera id is refused locally, an unreachable one is not'` | `0 cases \| 0 passed` | 0 |
| `--test-case='a bad camera id is refused locally\, an unreachable one is not'` | `1 case \| 1 passed \| 11 assertions` | 0 |

This is a property of the harness, not a defect in the tree — but it is the
reason a future "just run `--test-case`" check needs the same guard.

## Measurement 3 — the order axis, measured as an axis

doctest's `--order-by=rand` shuffles from a fixed default seed. Two runs of
the same binary produce byte-identical case order, and so do two runs whose
`--rand-seed` differs only in the seed's *value* when that value is not passed
at all — the flag is randomized in name only. Measured on a light suite:
`--rand-seed=2`, `3`, `5` and `7` give four distinct orders, while omitting
the flag twice gives one. The first drill's "0 order differences", measured
457 times, was therefore one permutation per binary.

The orderings the project's own protocol names were then run over every
registered binary. **Before the fixes**, the four-seed sweep
(`--order-by=rand --rand-seed=2,3,5,7`, 1828 runs over 457 registrations)
found 37 failures across the five suites above, and the deterministic pair
(`--order-by=name` and `--order-by=name --reverse`, 916 runs over 458
registrations) found 4 failures of its own, in two of the same five suites —
orders that need no seed at all. **On the tree these fixes leave behind:**

- `--order-by=rand --rand-seed=2,3,5,7` over all 457 registered binaries: 1828
  runs (457 registrations x four seeds), 0 failures: every run reported its
  baseline's case and assertion totals
- `--order-by=name` and `--order-by=name --reverse` over all 458 registered
  binaries: 916 runs (458 registrations x two orderings, 2 of them the
  non-doctest binary), 0 failures, identical totals
- permutation distinctness: 12 registrations whose baseline is at or under
  0.20 s were each run 5 orders — the default `--order-by=rand` and seeds 2,
  3, 5 and 7 — and the distinct case orders observed per registration ranged
  from 2 to 5 of 5, so the seeds do select different permutations. The default
  order itself was run twice per registration and came back identical in 12 of
  12; with no `--rand-seed` named the shuffle repeats one fixed permutation,
  so the "random" flag is random only in name.

## Measurement 4 — the stability protocol

For the binaries the fixes reach (the 25 registrations carrying the five
edited test sources, plus the two `sync` suites that call the arming function
the library edit touches — and `camera`'s same-named but unrelated
`change-outbox-test`, which the name-keyed matcher picks up), the protocol of
`docs/operations/build-and-test.md` was run in full: 20 consecutive runs per
registration, plus 50 runs per ordering for the cases that had flaked —
`--order-by=name`, `--order-by=name --reverse`, and 50 distinct random seeds.

| Suite | Registrations | Runs | Failures | Seconds |
|---|---:|---:|---:|---:|
| `mdns-service-test` | 13 | 2210 | 0 | 3565 |
| `identity-client-test` | 9 | 1530 | 0 | 109 |
| `audit-sync-read-test` | 1 | 170 | 0 | 42 |
| `change-outbox-test` | 2 | 340 | 0 | 12 |
| `sync-surface-test` | 1 | 170 | 0 | 9 |
| `guard-belief-test` | 1 | 170 | 0 | 2 |
| `service-config-test` | 1 | 170 | 0 | 0 |
| **total** | **28** | **4760** | **0** | **3738** |

4760 runs over 28 registrations, every one reporting its baseline's case and
assertion totals: 20 consecutive runs in the default order per registration,
then 50 in `--order-by=name`, 50 in `--order-by=name --reverse`, and 50
random-seed runs (seeds 1 to 50) — 170 per registration.

## Findings

**Five suites were order-dependent; each is fixed at its cause.** They are 25
of the 458 registrations — a package suite is compiled into every project that
links it, which is why `mdns-service-test` accounts for 13 of them and
`identity-client-test` for 9 — and between them they failed 37 of the 1828
pre-fix seed runs and 4 of the 916 pre-fix deterministic ones, listed in
Appendix B. In each case the defect is in the test, not in the product code it
drives:

- **`mdns-service-test`** — the case that clamps a name longer than a DNS
  label sets `mdns.name` to eighty `a`s, and the case that asserts an instance
  label (`Argus-camera._argus-route._tcp.local.`) inherited it: doctest's
  expansion of that `CHECK` reads `aaaa…-camera… == Argus-camera…`, two of
  forty assertions, under `--rand-seed=7`. It survives `--order-by=name` only
  by accident — the empty name case happens to run between them and leaves a
  value that yields the same default label — which is precisely what makes a
  single permutation worthless. Fixed by announcing the name the case asserts.
  Its baseline is measured per registration and moved between the two passes —
  three registrations from roughly 1.0–1.4 s to 2.0 s, two from 1.5 s to about
  1.0 s — which is the whole of the sampling-boundary difference described in
  Measurement 2.
- **`guard-belief-test`** — the "unresolved config matches the member defaults
  exactly" case fails its threshold comparison with doctest's expansion
  `9 == 5`, under `--order-by=name` **and** `--order-by=name --reverse`:
  deterministic, no seed required. The case that loads a config file puts
  `[guard.belief] threshold_medium = 9` into the global config, and
  `ConfigService::load()` replaces it without a way back — a runtime write can
  restore a value but nothing can unset one — so every later reader sees 9
  where the member default is 5. Fixed by restoring the three keys that case
  moves before it returns.
- **`service-config-test`** — the case that asserts a missing key yields the
  struct default fails with doctest's expansion `5s == 300s`, under
  `--rand-seed=2,3` (the `tunnel` suite, one registration). The case that
  overrides `tunnel.stream_idle_seconds` to 5 leaves that runtime override in
  the global config, and the default case reads it instead of the struct
  default. The same shape as `guard-belief-test`, in the same in-memory
  override map with the same lack of an unset, and fixed the same way: the
  overriding case restores the key to `TunnelMux::Limits{}.idleTimeout` before
  it returns.
- **`change-outbox-test`** — the minted-id case throws
  `UNIQUE constraint failed: change_outbox.event_id` when the legacy case ran
  first (`--rand-seed=2,3`, and under both `--order-by=name` orderings too, in
  the `auth` registration; the tree has a second suite registered under this
  name, `camera`'s own `change-outbox-test`, which is a different file and
  never failed). `entropy()` is a fixed byte array, so
  `actionMsgId(entropy())` is one constant; the legacy case leaves that row
  behind, and the minted case's own enqueue then collides with it. Fixed by
  leaving the shared outbox empty for whoever runs next.
- **`identity-client-test`** — the identity case throws
  `no such table: marker` whenever the frozen-client case ran first
  (`--rand-seed=2,3`, in all nine registrations). Its URI client
  (`filename=file:identity-client-test.db?mode=ro`) had opened that string as
  a literal filename, because `DbService::enableUriFilenames()` — called
  inside the case — is a `sqlite3_config(SQLITE_CONFIG_URI, 1)` that SQLite
  silently ignores once it has been initialized, and the frozen-client case
  boots Drogon, which initializes it. The arming worked only while that case
  happened to run first. Fixed by arming in the test's own `main()`, before
  doctest starts; the library call now also logs the refusal
  (`sqlite: URI filenames were not enabled rc=<n> …`), so the next suite that
  arms too late says so instead of failing obscurely.

**One registered binary is not a unit test and skips without secrets.**
`sync`'s `golden-sync-test` (`services/sync/tests/e2e/`, non-doctest, custom
`main`) is the frozen-`/sync` recorder/verifier: it takes a mode argument
(`record`/`verify`, not passed by ctest), reads `ARGUS_TEST_BASE_URL`,
`ARGUS_TEST_FIXTURES_DIR` and `ARGUS_TEST_REFRESH_TOKEN`, and without the
token prints `SKIP: no ARGUS_TEST_REFRESH_TOKEN provided; …` and exits 0. Its
ctest green is therefore vacuous, and the drill recorded it as the one binary
with no doctest summary. It is Phase 5 step 2's harness, already in the tree,
and this row's job was to record what it does when the environment is absent —
not to change it. `docs/operations/build-and-test.md` says the same thing in
one line: "A skipped test is not a pass."

**Five `*-test` binaries under `build/dev/` have no source and no target.**
`change-feed-test`, `device-credential-test`, `enums-test`,
`sdk-caller-identity-test` and `vision-remote-adapter-test` are on-disk
executables in ignored build trees — 657 executable `*-test` files under
`build/dev/` against 166 distinct names (63 more files sit under
`build/prod/`) — but appear in no `CMakeLists.txt` and have no `*-test.cc`
anywhere. The 161 registered names are all on disk, and exactly these five
on-disk names are registered nowhere. Four trace to named deletion commits —
`enums-test` to `62b841c9`, `device-credential-test` to `31b8a793`,
`sdk-caller-identity-test` to `55767fe5`, `vision-remote-adapter-test` to
`483c77fb`. The fifth, `change-feed-test`, has no tracked ancestor: no path
matching `change-feed-test` exists in any commit and no commit's content ever
contained that string, yet the binary embeds a source path
(`packages/contracts/sync/tests/unit/change-feed-test.cc`) that was never
committed. They are CMake's leftovers for deleted targets, nothing runs them,
and every first-party test source is registered: 161 distinct registered
names, no orphans. (The 22 `*-test.cc` under vendored
`third_party/sherpa-onnx/` are not registered anywhere, and are not
first-party.)

**The 458 registrations are 161 suites.** 54 of those names are registered by
more than one project, because a service's `ctest` re-runs the package suites
it links (`config-service-test`, `role-access-test`, `thread-budget-test`,
`auth-grpc-client-test`, `llm-client-test`, `vlm-client-test`, …). The
per-project counts in the tables above are therefore registrations, not
distinct suites; `guard` shows 57 and `tunnel` 15 for that reason.

**Two `vlm` binaries print raw non-UTF-8 bytes** (`vlm-wire-test`,
`vlm-rpc-test`); decoded with replacement, nothing else in the tree does. The
first drill run died on those bytes with the results written only at the end
(408 binaries recorded before the crash), which is why the drill now decodes
defensively and appends each binary's result as it finishes.

**Suites write into whatever directory they are run from.** 46 first-party
`*-test.cc` sources name 76 distinct relative scratch paths — `.db`, `.toml`
and certificate literals that resolve against the working directory (measured
by lexing every string literal in each tracked `*-test.cc` and keeping the
non-absolute ones that read as a filename token and end in a data or
certificate extension, plus `cert-san-test`'s extension-less
`cert-san-test-certs` directory); a bare suffix such as a helper's `".db"` has
no stem and is not a path, `golden-sync-test`'s usage text adds two more
(`.raw.json`, `stored as .bin` — prose, and the suite builds that filename by
concatenation), and one further match, `guard-saga-test`'s
`guard/incidents/1/71.json`, is an object key in a database row rather than a
file the suite opens. All four are excluded. Some are pid-suffixed and deleted
on destruction, others are fixed names that stay behind:
`identity-client-test.db`, `vlm-wire-test.toml`, `certs/server.pem` from three
suites, the `config-service-test` family's ten `.toml` files. One more writer
is production code rather than a test — the go2rtc manager's default
`go2rtc.yaml`. Under ctest that is the build directory, where the leftovers
are ignored; a drill run from the repository root litters the tree instead —
this session's first runs left `cert-san-test-certs/` and a 0-byte
`file:identity-client-test.db?mode=ro` there, and `scripts/check-comments.sh`
fails on files it cannot classify. This drill therefore runs every binary from
its own directory, as ctest does, because an unknown parent directory is a
variable the isolation claim should not have.

## What this row does not cover

Stated plainly so the row is not read as more than it is: the order axis is
sampled, not exhaustive — six orders per binary (four random seeds plus the
two name orderings) out of the `n!` a suite of `n` cases admits — so each
green there means "no failure observed in the orders measured", and the
strongest form of it is the deterministic pair, which needs no seed; the
case-sampling of Measurement 2 is listed in full in the appendix, and no case
was skipped outside a stated sample; the live suites that need a running
backend skip by design (`golden-sync-test` here, the `guard`
DLQ/assessment/encounter live suites, `vlm-client-live-test`), and their green
means "skipped cleanly", not "the live path was exercised" — Phase 5 steps 4
to 6 are where live paths get driven.

## Why no gate was landed

The drill is not committed as `scripts/check-isolation.sh`. It takes minutes
of wall clock on an idle machine, and the orchestrator's per-project runs are
required to stay quick (root `AGENTS.md`: "`--only`, `--no-tests` and
`--install-only` skip the clang-tidy scan deliberately: it needs every
project's compile database, and a per-project run has to stay quick"). A
fourth gate in `build-all.sh` is not this step's call either: `AGENTS.md`
states the orchestrator "runs three gates of its own", each tied to a rule it
enforces (`check-comments` to rule 20, `check-deps` to section 2.4's tiers,
`check-tidy` to rules 16 and 19), and `./scripts/build-all-test.sh` locks the
orchestrator's flags — adding one is a change to all three, for a different
unit. The row asks for the check to be *performed*, and the recipe is recorded
here so it can be performed again:

```bash
# whole tree, under the build lock (gates, build, ctest):
flock /tmp/argus-build.lock ./scripts/build-all.sh dev

# per registered binary, from its ctest command and its own directory:
<binary>                                        # baseline
<binary> --order-by=name                        # documented orderings
<binary> --order-by=name --reverse
<binary> --order-by=rand --rand-seed=<N>        # one seed per run
<binary> --test-case='<name with \, for each comma>'   # exactly 1 case
```

## Verification

Every number above was recomputed from the run's own artifacts rather than
carried forward. The per-project table parses the orchestrator's log: the
`Building CXX object` lines, the `warning:`/`error:` lines whose path is
first-party, and each ctest summary with its `Total Test time` line — and the
ctest total was derived a second time with an independent one-line `awk` over
the same log, both giving 284.74 s. The census maps each object line to its
source through the project's own `compile_commands.json` (1269 object lines, 0
of them naming no configured translation unit), rather than by matching
suffixes by eye.

The drill polices itself: a `--test-case` run counts as isolated only when
doctest reports exactly one case, which is what turns the harness's
match-nothing-is-success behaviour (the comma trap above) from a false pass
into a failure. Both sweeps compare total case and assertion counts against
the baseline row of the same registration, so a suite that silently stopped
running a case would show up as drift. The seed sweep resolves a
registration's binary through its project's ctest listing and looks only in
directories carrying a built `CTestTestfile.cmake`, so a package directory
sharing a project's name is not mistaken for it; the one registered binary
with no doctest baseline (`sync`'s `golden-sync-test`) is recorded as a note
rather than a failure, and the permutation sample was re-derived by the sweep
at exit 0 after that bookkeeping was fixed.

The stability protocol was scoped by counting, not by feel: the five edited
test sources are compiled into 25 registrations (`mdns-service-test` 13,
`identity-client-test` 9, `change-outbox-test` 1, `guard-belief-test` 1,
`service-config-test` 1) and the two `sync` suites that call
`DbService::enableUriFilenames()` are one registration each, so 27 of the 458
registrations can be affected by this change and all 27 were run through the
full protocol. The protocol's matcher selects by test name, so it also picked
up `camera`'s same-named `change-outbox-test` — a different file — which is
why the run covers 28 registrations. The tree the run describes is the
committed one: `git status` lists the six edited sources named above and this
report and nothing else, and the orchestrator's own three gates ran over it
before it built (see Measurement 1).


## Appendix A — the registrations sampled for cost

| Project | Suite | Cases | Run | Seconds |
|---|---|---:|---:|---:|
| llm | `llm-wire-test` | 2 | 2/2 | 75.51 |
| vlm | `vlm-wire-test` | 1 | 1/1 | 29.47 |
| llm | `llm-rpc-test` | 16 | 3/16 | 25.10 |
| guard | `guard-retry-lifecycle-test` | 1 | 1/1 | 18.49 |
| stt | `stt-wire-test` | 1 | 1/1 | 13.94 |
| llm | `memory-backpressure-test` | 3 | 3/3 | 10.19 |
| vlm | `vlm-rpc-test` | 15 | 3/15 | 7.33 |
| tunnel | `tunnel-negauth-test` | 5 | 3/5 | 6.34 |
| stt | `stt-rpc-test` | 13 | 3/13 | 6.21 |
| cert | `cert-san-test` | 1 | 1/1 | 5.05 |
| identity | `cert-san-test` | 1 | 1/1 | 5.03 |
| tts | `tts-wire-test` | 1 | 1/1 | 4.86 |
| tts | `tts-rpc-test` | 11 | 3/11 | 4.55 |
| llm | `memory-reminder-test` | 1 | 1/1 | 3.47 |
| voice | `voice-tts-remote-test` | 9 | 3/9 | 3.09 |
| notification | `notification-no-nats-test` | 2 | 2/2 | 3.06 |
| tunnel | `tunnel-integration-test` | 4 | 3/4 | 2.73 |
| auth | `refresh-rate-gate-test` | 6 | 3/6 | 2.30 |
| camera | `mdns-service-test` | 11 | 3/11 | 2.02 |
| guard | `mdns-service-test` | 11 | 3/11 | 2.02 |
| identity | `mdns-service-test` | 11 | 3/11 | 2.02 |
| stt | `mdns-service-test` | 11 | 3/11 | 2.02 |
| tts | `mdns-service-test` | 11 | 3/11 | 2.02 |
| notification | `camera-notifier-test` | 18 | 3/18 | 1.91 |
| auth | `mdns-service-test` | 11 | 3/11 | 1.52 |
| notification | `mdns-service-test` | 11 | 3/11 | 1.52 |
| productivity | `mdns-service-test` | 11 | 3/11 | 1.52 |
| sync | `mdns-service-test` | 11 | 3/11 | 1.52 |
| vlm | `mdns-service-test` | 11 | 3/11 | 1.52 |
| tunnel | `mdns-service-test` | 11 | 3/11 | 1.51 |

## Appendix B — the pre-fix order failures, verbatim

The failing rows of the pre-fix sweeps — `--order-by=rand --rand-seed=2,3,5,7`
and the two deterministic name orderings — over every registered binary, by
project, suite, seed or ordering, and verdict. All of them are green on the
fixed tree (Measurement 3).

| Suite | Order | Registrations | Projects | doctest verdict |
|---|---|---:|---|---|
| `change-outbox-test` | `name` | 1 | auth | {cases: 2, cases_failed: 1, assertions: 25, assertions_failed: 0} |
| `change-outbox-test` | `name-reverse` | 1 | auth | {cases: 2, cases_failed: 1, assertions: 25, assertions_failed: 0} |
| `change-outbox-test` | `seed 2` | 1 | auth | {cases: 2, cases_failed: 1, assertions: 25, assertions_failed: 0} |
| `change-outbox-test` | `seed 3` | 1 | auth | {cases: 2, cases_failed: 1, assertions: 25, assertions_failed: 0} |
| `guard-belief-test` | `name` | 1 | guard | {cases: 15, cases_failed: 1, assertions: 105, assertions_failed: 1} |
| `guard-belief-test` | `name-reverse` | 1 | guard | {cases: 15, cases_failed: 1, assertions: 105, assertions_failed: 1} |
| `guard-belief-test` | `seed 5` | 1 | guard | {cases: 15, cases_failed: 1, assertions: 105, assertions_failed: 1} |
| `guard-belief-test` | `seed 7` | 1 | guard | {cases: 15, cases_failed: 1, assertions: 105, assertions_failed: 1} |
| `identity-client-test` | `seed 2` | 9 | auth, camera, guard, identity, llm, notification, productivity, sqlite, sync | {cases: 2, cases_failed: 1, assertions: 10, assertions_failed: 0} |
| `identity-client-test` | `seed 3` | 9 | auth, camera, guard, identity, llm, notification, productivity, sqlite, sync | {cases: 2, cases_failed: 1, assertions: 10, assertions_failed: 0} |
| `mdns-service-test` | `seed 7` | 13 | auth, camera, guard, identity, llm, notification, productivity, stt, sync, tts, tunnel, vlm, voice | {cases: 11, cases_failed: 1, assertions: 40, assertions_failed: 2} |
| `service-config-test` | `seed 2` | 1 | tunnel | {cases: 2, cases_failed: 1, assertions: 2, assertions_failed: 1} |
| `service-config-test` | `seed 3` | 1 | tunnel | {cases: 2, cases_failed: 1, assertions: 2, assertions_failed: 1} |
