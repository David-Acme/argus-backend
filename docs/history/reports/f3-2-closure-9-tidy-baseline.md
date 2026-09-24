# Closure item 9 (S6) — the tidy baseline is not the tree's

Phase 3a step 2, closure item 9. Commit:
`build: record the tidy baseline the tree actually has`.

## What the row said

> **S6 — the tidy baseline is not the tree's** — One `--write-baseline` run at the
> close of the step, once items 1–8 have moved the tree, so the ratchet and the
> tree agree.

The row is a measurement, not a repair: `scripts/lib/tidy-baseline.txt` is the
floor the tidy gate (rules 16 and 19, plan section 4.13) compares every later run
against, and it was recorded before the step's nine items moved the tree.

## The pre-state

`scripts/lib/tidy-baseline.txt` held `tool 22.1.8`, `tus 481`, and 45 check lines
summing **3,141** findings. Both figures were inherited verbatim from
`dfbaf990 build: retire the sync package into argus-sync`; item 0
(`d017ab48`) touched the file only to strip its six header comment lines when
rule 20 landed, leaving the counts where they were.

The scan at the step's close sees **508 TUs**. The gap is the defect the row
names, and it is asymmetric in the two directions the gate has:

- **`tus` is a floor.** The gate fails when the scan sees *fewer* TUs than the
  baseline records — "a check that cannot be run is not a check that passed". A
  floor 27 below the tree's own count would have let a whole project's compile
  database go missing (or a project be dropped from the orchestrator) without
  the gate saying anything. That is the half that can hide a regression.
- **The per-check counts are a ceiling.** A count that rises fails; a count that
  falls is the ratchet moving down, which is the intent. At 3,141 against a
  measured 3,049 the file was simply stale — 92 findings that items 1–7 (and the
  code they moved) had already removed were still being tolerated.

## What the step's close did

Three runs, in this order, on an unchanged tree:

1. `./scripts/build-all.sh dev` — the full orchestrator, exit 0: **17/17
   projects**, **450 tests**, `check-comments: 1251 files checked, 0 comments`,
   `check-deps: 64 declarations, 505 edges, 0 forbidden, 0 cycles, 0 unresolved,
   49 edges deferred to phase 3 (233 third-party mentions over 22 roots)`, and
   its own closing tidy stage, which compares the tree against the **old**
   baseline: `check-tidy: 508 TUs, 3049 findings over 45 checks, baseline 3141;
   8 checks below it` with **no `risen:` line and no `unread:` line**. Worst file
   `services/guard/src/feature/guard/guard-repository.cc` (104 findings).
2. `./scripts/check-tidy.sh --write-baseline` →
   `baseline written to scripts/lib/tidy-baseline.txt (508 TUs, 3049 findings)`,
   exit 0.
3. `./scripts/check-tidy.sh` (no flag) → `508 TUs, 3049 findings over 45 checks,
   baseline 3049`, exit 0.

## The write, measured

Per check, before → after. All eight deltas are falls; the other 37 checks are
unchanged and no check line vanished (both files carry the same 45, so nothing
fell to zero where a later rise would be invisible).

| Check | Before | After | Δ |
|---|---:|---:|---:|
| `modernize-use-designated-initializers` | 302 | 256 | −46 |
| `modernize-use-nodiscard` | 780 | 759 | −21 |
| `modernize-use-scoped-lock` | 295 | 283 | −12 |
| `modernize-use-auto` | 61 | 55 | −6 |
| `bugprone-throwing-static-initialization` | 39 | 36 | −3 |
| `bugprone-narrowing-conversions` | 68 | 66 | −2 |
| `bugprone-optional-value-conversion` | 31 | 30 | −1 |
| `bugprone-unchecked-optional-access` | 224 | 223 | −1 |
| **Total** | **3,141** | **3,049** | **−92** |

`tus` 481 → 508. `tool` unchanged at 22.1.8 — the counts belong to the tool that
produced them, and the gate refuses a foreign major rather than reinterpreting
one. The check set itself did not move under the counts: `.clang-tidy`'s only
change since the measurement commit `990691f3` is the removal of its own comment
block (rule 20, item 0), and its `Checks:` list is identical between the two
revisions.

The review attributed each fall to a named site in the code (see below); the two
largest read plainly: the −46 `designated-initializers` are four
contract-catalog test tables rewritten as
`constexpr std::array<CatalogEntry, N> kCatalog{{ {.name = …, .definition = …} }}`,
where 46 pre-existing entries plus 4 the new initializers introduced reconcile
exactly with the −46, and the −1 `optional-value-conversion` is
`subscription_ = *subscription;` → `subscription_ = subscription;` at
`services/sync/src/feature/fanout/services/notification-delivery-consumer.cc:142`,
whose member is `std::optional<uint64_t>`
(`notification-delivery-consumer.hxx:52`) — the optional → value → optional
shape the check exists for.

## What proves it

- **The write cannot have accepted a regression.** The full run's own tidy stage
  ran *before* the write, against the old baseline, on the same tree, and
  reported no `risen:` — the state being recorded is one the old ratchet had
  already accepted. The write only lowers the ceilings it records (each of the
  eight falls tightens the gate: a return to 302 designated-initializer findings
  now fails where it previously passed) and raises the TU floor to the tree's own
  count.
- **The write refuses what it cannot measure.** `--write-baseline` exits 1 with
  the file untouched when any TU cannot be analysed, and prints each one; the run
  wrote, so every one of the 508 was analysed.
- **The file is the gate's own input.** The confirming run read the new file
  (`baseline 3049` in its output) and exited 0 with no `risen:` and no `unread:`.
- **No stale entry inflated the count.** The scan prints
  `entries skipped in <database> -- the database names files that are gone` for
  every stale compile database; it printed none, so the 508 TUs are files that
  exist, deduplicated by absolute path across the 20 compile databases under the
  tree.
- **The file keeps the shape the reader parses** (`read_baseline()`: a `tool`
  line, a `tus` line, `name value` lines) and carries no comments — rule 20
  applies to it as to every other non-doc file, and item 0 had already stripped
  the header the writer never re-emits.

## Files

- `scripts/lib/tidy-baseline.txt` — the recorded measurement: `tus 481 → 508`,
  eight check lines lowered, total 3,141 → 3,049.
- `docs/history/plans/architecture-plan.md` — row 9 marked Done; the 3a step-2
  status line and the step's closing paragraph updated.
- this report.

## Verification

- Full orchestrator run at the step's close: exit 0, 17/17 projects, 450 tests,
  check-comments 1251 files / 0 comments, check-deps 505 edges / 0 forbidden,
  check-tidy 508 TUs / 3,049 findings under the 3,141 baseline with 8 checks
  below and none risen.
- `--write-baseline` run: exit 0, wrote 508 / 3,049.
- Confirming gate run on the written file: exit 0, 508 TUs / 3,049 findings /
  baseline 3,049, no `risen:`, no `unread:`.
- Per-check diff of the two baseline files: 45 checks each, eight falls
  (92 findings), no rise, no vanished line; sums 3,141 and 3,049.

## What the review found

An adversarial review (a fresh agent, opus) returned **no defect** — every figure
reproduced and no fall turned out to be deletion-driven. Its evidence, and the
one wording of its own that did not survive my check:

- **The TU count recomputed independently.** It reimplemented
  `translation_units()`'s filters and dedupe from the same 20 compile
  databases: 7,596 raw entries → **508 distinct existing first-party `.cc`**,
  with **0 entries naming a missing file** (hence no `entries skipped in …` line
  in any run), and the set equals every tracked `.cc` under `packages|services`
  today. 5,261 entries were third-party, 698 under a build tree, 1,129 duplicate
  paths.
- **Same check set, all deltas one-directional.** Both files carry the same 45
  check names, 8 falls / 0 rises / 37 identical, and the falls sum to 92 — the
  exact difference 3,141 → 3,049.
- **The eight falls attributed by name**, each to changed code rather than
  deleted code: the `-46` to the four catalog tables above; the `-12`
  `scoped-lock` to `std::lock_guard` → `std::scoped_lock` (net −11 guards, +31
  scoped locks); the `-6` `use-auto` to six same-type cast declarations
  rewritten (four `jsErrCode` sites in
  `packages/lib/nats/src/nats/nats-bus.cc`, two `now` sites in `services/llm`
  and `services/sync`); the three `throwing-static-initialization` to
  pre-existing tables becoming `constexpr std::array`. Deletion was checked
  directly: exactly six files were removed in `990691f3..HEAD` — one
  `.gitignore`, one `CMakeLists.txt`, a 10-line `sync-forwarder.cc` and three
  21–28-line interface headers whose content survives verbatim in
  `services/sync/src/feature/transport/infra/*-sync-source.hxx` — and none
  carries the shape of a falling check.
- **The gate still bites, demonstrated rather than argued.** Three runs against
  copies of the baseline under `/tmp` (the repo file untouched): a check floor
  lowered by one → `risen: bugprone-suspicious-stringview-data-usage 301
  findings, baseline 300`, exit 1; a TU floor raised above the scan's count →
  `risen: the scan saw 508 TUs, baseline 509 -- build the whole tree before the
  gate`, exit 1; and a check floor one below its measured value → exit 1. So
  the recorded values are a floor in both directions, not a description.
- **Reproducibility.** A fresh read-only run of the gate gave `508 TUs, 3,049
  findings over 45 checks, baseline 3049`, no `risen:`, no `unread:`, exit 0 —
  and the *pre-write* baseline also passes this tree
  (`baseline 3141; 8 checks below it`, exit 0), which is the same fact the full
  run's closing stage established: the state recorded is one the old ratchet
  accepted.
- **One correction to the review**: it reported `.clang-tidy` as byte-identical
  to `990691f3`. It is not — item 0 stripped the file's comment block
  (`md5 22f753a7…` → `48382a14…`) — but the load-bearing half holds and I
  verified it: the `Checks:` list is identical between the two revisions, so the
  counts were measured under the same check set.
- **Two observations recorded, not defects**: the version guard compares only
  clang-tidy's major and is deliberately skipped under `--write-baseline` (the
  same 22.1.8 wrote the old file), and the finding key drops the column, so two
  identical message/line/check findings in one file would count once — no such
  collision occurs here, since the review's per-check attribution sums exactly to
  the falls.
