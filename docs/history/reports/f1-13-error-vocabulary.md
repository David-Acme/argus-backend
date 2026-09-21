# F1-13 — the duplicated error vocabulary

Phase 1 step 13: *"Remove the error vocabulary duplicated in the HTTP config: the
`ERROR_CODE_*` string constants collapse into the one `ErrorCode` declaration in
`lib/errors`, and `AppConfig::SYNC_LIMIT` (a sync wire invariant) moves to
`contracts/sync`. One declaration per code, or the wire breaks the first time the
two disagree. `wire-sync-tables.md` cites both by their old paths
(`backend/src/config/app-config.hxx`, `backend/src/shared/enums.hxx`) and its
citation moves with them — the values it freezes do not"*.

**The step needed no work: both halves were absorbed by step 10.** No file
changes here beyond this report and the plan row. What follows is the evidence,
because a step closed as already-done has to prove it rather than assert it.

## Why it was already done

Step 10 is `31fd41c` (`build: split response into errors and http, and extract
mdns`), and the three measurements below are all against that commit and the
tree it produced.

**The subject file is gone.** `31fd41c` deletes
`packages/response/src/config/app-config.{hxx,cc}` (69 + 134 lines). At its
parent commit that header is exactly what the row describes:

```cpp
static inline const std::string SYNC_LIMIT{"200"};
static inline const std::string ERROR_CODE_BAD_REQUEST{"BAD_REQUEST"};
static inline const std::string ERROR_CODE_UNAUTHORIZED{"UNAUTHORIZED"};
... through ERROR_CODE_BAD_GATEWAY{"BAD_GATEWAY"}
```

Eleven `ERROR_CODE_*` string constants plus `SYNC_LIMIT`, declared in the HTTP
config layer, next to (and able to drift from) the codes the boundary actually
threw. (Both this step's first draft and step 10's report said twelve and
thirteen; the file at `31fd41c^` holds fifteen `static inline const
std::string` members — the three `*_CTX_KEY`s, `SYNC_LIMIT` and eleven codes —
and all eleven names survive in `ErrorCode`, so the count is eleven. Step 10's
report is corrected below.)

**The error vocabulary is single again.** `git grep ERROR_CODE_` over the live
tree — `-- . ':!docs/history' ':!*/build/*'` — returns **one file**:
`packages/contracts/proto/argus/common/v1/base.proto`. That is the frozen gRPC
wire enum (`argus.common.v1.ErrorCode`, 11 values plus `UNSPECIFIED = 0`),
which is a *wire* vocabulary with its own home in `contracts/`, not the C++
constants the row retires; its own comment says it mirrors
`packages/errors`. No C++ translation unit declares a second copy, and the
string literals that remain outside `packages/errors`
(`"CAMERA_UNREACHABLE"`, `"TTS_NOT_LOADED"`, `"BAD_GATEWAY"`, …) are **test
assertions** pinning the wire value (`services/tts/tests/unit/tts-rpc-test.cc:198`,
`services/camera/tests/unit/camera-talk-cutover-test.cc:208`,
`services/gateway/tests/gateway-test.cc:990`) — the contract being tested, not a
declaration that can drift. `ErrorCode` itself gained the four codes that had
existed only as literals where the refusal was raised (step 10's report), and
its `toString`/`fromString` pair is the only spelling of each code name.

**`SYNC_LIMIT` moved to the sync contract.** It is
`packages/contracts/sync-contract/src/shared/contracts/sync-limits.hxx` —
created by `31fd41c` (+12 lines) — as `SyncLimits::kMaxRows{"200"}`, with the
reason in its own comment: it is a wire invariant, "declared once, here, so the
SQL constants in the sync and audit repositories concatenate the same digits".
The row's target spelled the path `contracts/sync`; the unit that holds it is
`packages/contracts/sync-contract`, which Phase 2's rename turns into
`contracts/sync` — the placement is what the row asks for, and the rename is
Phase 2's.

## The citation clause

`wire-sync-tables.md` cites the constant at `docs/architecture/wire-sync-tables.md:29-31`
as `backend/packages/contracts/sync-contract/src/shared/contracts/sync-limits.hxx`
(`SyncLimits::kMaxRows`), and `TableName`'s home as
`backend/packages/contracts/sync-contract/src/shared/contracts/table-name.hxx`
(`:19-20`). Both are current paths. The two old paths the row names —
`backend/src/config/app-config.hxx` and `backend/src/shared/enums.hxx` — occur
**nowhere in the live tree** (`git grep` over everything but `docs/history` is
empty), and step 11 already closed the `backend/src/...` spelling class. The
values the file freezes are untouched: `TableName` 0–23 and `SYNC_LIMIT = 200`
still read the same, which is the half of the row's sentence that matters to the
wire.

## A count this step corrected in step 10's report

`docs/history/reports/f1-10-errors-http-mdns.md:73` maps "the 13 `ERROR_CODE_*`
strings" out of `AppConfig`. The file it maps them out of holds eleven: fifteen
`static inline const std::string` members under the class, of which three are
`*_CTX_KEY`, one is `SYNC_LIMIT` and eleven are codes (`BAD_REQUEST` through
`BAD_GATEWAY`), and `git grep ERROR_CODE_ 31fd41c^ -- packages/response` finds
no twelfth name in either file. The number is corrected in place rather than
left standing, because a report is where a later step looks for the count; the
change is a digit, not a claim, and this step's own report carried the same
error until its verification pass re-measured it.

## What was left undone on purpose

Nothing in this row, and that is itself the finding worth recording: the row
predates the step-10 split that solved it, so the plan carried a step whose
subject no longer existed by the time it was reached. It is closed as
already-done, with the measurements above as its evidence, rather than
re-attributed to step 10's row — the two rows describe the same change from
different directions (step 10 from the HTTP config it was deleting, step 13 from
the codes it was deduplicating), and rewriting history to merge them would lose
whichever half a reader arrives from.

## Verification

The step changes no source, so the gate's job is the same as step 12's: prove the
tree is the one that was already verified.

- `./scripts/build-all.sh dev` — `GATE_EXIT=0`, 18/18 projects, every suite
  `100% tests passed`, the reached-test ledger **302** (unchanged from steps 11
  and 12), 0 first-party warnings.
- `git grep ERROR_CODE_` over the live tree: one file, the proto enum.
- `git grep SYNC_LIMIT`: eight files, no second declaration — the constant's own
  header and seven documentation references (`docs/README.md`,
  `wire-sync-tables.md`, `packages/contracts/{AGENTS,CONTEXT}.md`, and the
  `camera`, `notification` and `productivity` service docs). The value is
  consumed as `SyncLimits::kMaxRows` in six files: its declaration, the proto's
  comment, `wire-sync-tables.md`, and the two repositories that append it
  (audit's `audit-log` and `user-audit-log`, sync's `notification-repository`).
  No test names either spelling — the suites that pin this wire pin codes and
  frames, not the page size.
- `git grep app-config.hxx` and `git grep 'src/shared/enums.hxx'`: empty.
