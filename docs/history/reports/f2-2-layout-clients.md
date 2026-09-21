# Phase 2 step 2c — section 2.3 applied to the ten client packages

Base: `c68bca2` (2b, the contracts). Unit of work: the ten `packages/clients/`
packages. Gate: `./scripts/build-all.sh dev`.

## What the layout change is

Section 2.3 draws a client as:

```
packages/clients/<name>/
├── AGENTS.md
├── CMakeLists.txt                     argus_clients(NAME <name> PROTO_ROOT ... PROTO ...)
├── src/<name>/
│   ├── <name>-*-client.hxx            the SDK method surface callers use
│   ├── <name>-*-client.cc
│   └── details/                       channel, credentials, retry, envelope parsing
└── tests/unit/
    └── <name>-*-client-test.cc
```

with two sentences that decide what belongs where: consumers write
`#include <camera/camera-sync-client.hxx>` and link `argus::clients::camera`,
and **no consumer ever sees a protobuf type, a stub, a URL or a retry policy**.
The third shape in section 2.3 settles the meaning of `details/`: the lib tree
says it is "private by convention, never included outside", and the packages
that already practise it are `lib/auth`, `lib/sqlite`, `lib/http`, `lib/phrase`,
`lib/storage` and `lib/validation`.

Six of the ten clients were already there (`camera`, `camera-actions`,
`identity`, `notification`, `productivity`, `voice`), three of them sharing a
domain folder — `camera-actions` holds `src/camera/` because it speaks the
camera domain beside `clients/camera`. Four were not:

| package | before | after |
|---|---|---|
| `clients/llm` | `src/shared/services/llm/llm-service.hxx` | `src/llm/llm-service.hxx` |
| `clients/llm` | `src/shared/contracts/tool-contracts.hxx` | `src/llm/tool-contracts.hxx` |
| `clients/llm` | `src/shared/services/llm/remote/llm-remote.{hxx,cc}` | `src/llm/details/llm-remote.{hxx,cc}` |
| `clients/stt` | `src/shared/services/stt/remote/stt-remote.{hxx,cc}` | `src/stt/stt-remote.{hxx,cc}` |
| `clients/tts` | `tts-client.{hxx,cc}` (at the package root) | `src/tts/tts-client.{hxx,cc}` |
| `clients/tts` | `src/shared/services/tts/tts-wire.hxx` | `src/tts/tts-wire.hxx` |
| `clients/tts` | `src/shared/services/tts/remote/tts-remote.{hxx,cc}` | `src/tts/tts-remote.{hxx,cc}` |
| `clients/vlm` | `src/shared/services/vision/remote/vlm-client.{hxx,cc}` | `src/vlm/vlm-client.{hxx,cc}` |

Thirteen files, moved with `git mv`. Every basename is preserved: the four
packages are flattened, not renamed, so what changed for a consumer is the
include spelling and nothing else. `clients/tts` also stops exporting the
package root — its `INCLUDES .` is gone, which is what made `<tts-client.hxx>`
a legal include.

Only `clients/llm` ends up with a `details/` folder, because it is the only
one of the four whose transport is a separate translation unit: `LlmHttpClient`
plus its `LlmRemoteConfig` (a URL and a timeout) and its stream input. `stt`,
`tts` and `vlm` are each a single client class whose transport is its
implementation, so there is nothing to bury, exactly as the six already-shaped
clients have nothing to bury. The two candidates this leaves open are recorded
under Deviations.

## The sweep

Eight spellings, one pass, code only — with the docs handled as their own
measured fix:

| old spelling | new spelling | lines | in the package | elsewhere |
|---|---|---|---|---|
| `<shared/services/llm/llm-service.hxx>` | `<llm/llm-service.hxx>` | 13 | 1 | 12 |
| `<shared/services/llm/remote/llm-remote.hxx>` | `<llm/details/llm-remote.hxx>` | 5 | 0 | 5 |
| `<shared/contracts/tool-contracts.hxx>` | `<llm/tool-contracts.hxx>` | 9 | 0 | 9 |
| `<shared/services/stt/remote/stt-remote.hxx>` | `<stt/stt-remote.hxx>` | 3 | 0 | 3 |
| `<shared/services/tts/tts-wire.hxx>` | `<tts/tts-wire.hxx>` | 5 | 1 | 4 |
| `<shared/services/tts/remote/tts-remote.hxx>` | `<tts/tts-remote.hxx>` | 3 | 0 | 3 |
| `<tts-client.hxx>` | `<tts/tts-client.hxx>` | 3 | 1 | 2 |
| `<shared/services/vision/remote/vlm-client.hxx>` | `<vlm/vlm-client.hxx>` | 5 | 1 | 4 |
| **total** | | **46** | **4** | **42** |

Thirty-four code files carry those 46 lines — 30 outside the four packages and
4 inside them. The 42 lines outside sit in those 30 files, grouped by owner:
9 in `services/llm` (including `lfm-adapter.hxx`, a service file that includes
the *client's* `llm-service.hxx`), 8 in `packages/memory`, 4 in
`services/guard`, 4 in `services/tts`, 2 in `services/voice`, 2 in
`services/camera`, 1 in `services/stt`. One further line is a quoted include —
`tts-client.cc`'s `#include "tts-client.hxx"`, which names a file beside it and
so moved with it without a spelling change; it is not one of the 46.

The docs carry the same spellings and were handled as their own measured pass:
at HEAD the eight spellings appear in nine `AGENTS.md` lines — 4 in
`clients/llm`, 3 in `clients/tts`, 1 in `clients/stt`, 1 in `services/tts` — and
none of the nine survives in the tree. History is the only place they remain,
and four files carry one on the tree this report ships with, each citing a
spelling as a record rather than using it: this report, which quotes them to
document the pass; `docs/history/plans/architecture-plan.md`, whose row for this
step names `<tts-client.hxx>` as the spelling the package root's `INCLUDES .`
used to legalise; `docs/history/reports/f1-11-stale-docs.md`; and
`docs/history/reports/f1-9-tool-framework-into-llm.md`. No include line, no
`CMakeLists.txt` and no `AGENTS.md` keeps one -- every include line the sweep
targeted was rewritten. The plan's one
`src/shared/services/...` mention is `services/llm`'s own `lfm-adapter.hxx`,
which is still exactly where the sentence says it is.

The sweep had to be surgical rather than a substitution, and that is the one
thing this step knows that the last one did not: the four packages were not the
only inhabitants of the spellings they used. `services/llm` keeps its own
`src/shared/services/llm/` — `lfm-adapter.hxx`, `intent-gate.hxx`,
`llm-service.cc` — and `services/vlm` keeps its own
`src/shared/services/vision/` (`vision-service.hxx`, `vision-hash.hxx`,
`remote/remote-vision-adapter.hxx`, `remote/vlm-remote.hxx`). The two trees
were merged under one spelling, so `<shared/services/llm/llm-service.hxx>`
resolved into the client while `<shared/services/llm/lfm-adapter.hxx>` beside
it resolved into the service. Every replacement was therefore keyed on a
basename that exists in exactly one package — checked with `find` before the
sweep ran, and the eleven `.proto` and service-owned spellings left untouched
for 2d.

## The clients' own suites

`camera` was the only client with a suite of its own
(`tests/unit/client-caller-identity-test.cc`, one case, standing up a real
`CallbackService` and pinning that a receiver gates on the presence of the
`x-argus-*` keys). The other nine had none, and the four flattened packages had
no `enable_testing()` at all — they carried `tests/support/fake-*-server.hxx`
fixtures that *other* packages' suites import through an explicit
`target_include_directories`. Ten suites were added -- one for each of the nine
clients that lacked one, and a second one in `camera`
(`camera-sync-client-test`) -- each registered in its own `CMakeLists.txt` in the shape
`camera`'s already used, including the `EXCLUDE_FROM_ALL FALSE` opt-back-in
that its comment explains.

### How a client suite is counted

Client packages are not gate projects; the gate's eighteen are libs and
services, and a client is pulled in by whichever of them speaks its domain. A
client's suite is therefore collected by the ctest of every project whose
configure reaches the package's directory, and counted once per such project —
the number of instances is a property of the CMake graph, not of the client.

The first attempt to predict that number was a static model: a literal client
path in an `add_subdirectory(` call owned by one of the eighteen projects. It
reproduced the three `client-caller-identity-test` instances 2b had already
measured, which is what made it look trustworthy, and then predicted **28**
instances for the ten suites this step adds — 25 for the nine that filled a gap
and 3 for `camera-sync-client-test`. The measured figure is **39**. The model was
exactly right on six of the ten (`stt` and `tts` 3 each, `camera-actions`,
`productivity` and `voice` 2 each, `camera-sync-client-test` 3) and under-counted
the other four by 11: `identity` 5 → 13, `llm` 4 → 5, `notification` 3 → 4, `vlm`
1 → 2. Both mechanisms it cannot express are in the tree, and each is worth a
line because neither is exotic:

- **A lib package pulls a client in, and a lib is not one of the eighteen.**
  `packages/lib/auth/CMakeLists.txt:42-45` adds `../../clients/identity` into
  `${CMAKE_BINARY_DIR}/clients/identity` whenever `argus::clients::identity` is
  not already a target — the filter chain validates tokens through the identity
  RPC contract — so every project that configures `lib/auth` registers the
  identity suites transitively. The model's rule is one level deep by
  construction and cannot say "a lib that thirteen projects configure".
  Measured: the suite's thirteen trees are the thirteen that carry
  `build/dev/clients/identity`, and the five that do not — `sqlite`, `intent`,
  `stt`, `vlm`, `tunnel` — are the five whose ctest list does not name it.
- **A client pulled in by a loop.** `services/camera/CMakeLists.txt:369-380`
  runs `foreach(CLIENT IN ITEMS llm vlm notification)` and adds
  `packages/clients/${CLIENT}` into `${CMAKE_BINARY_DIR}/${CLIENT}`. The names
  are a loop variable, so no literal `clients/llm` exists in the file for a grep
  to find, and the binary directory is bare rather than `clients/llm`, so
  `ls <tree>/build/dev/clients` does not show it either. That is the whole of the
  missing instance on `llm`, `notification` and `vlm` — the three the loop names —
  and it is why the camera tree runs suites for clients its own `CMakeLists.txt`
  never mentions by name.

The listing was not a usable cross-check either, for the reason the second
bullet gives: `lib/auth`'s add puts its client under `clients/identity` while the
camera loop puts its three directly under the binary root, so one form is visible
in that directory and the other is not. What replaced the model is the only
authoritative source there is — `ctest -N` in each tree, which prints the
numbered list ctest will run, summed — and the arithmetic below closes exactly
against the gate's own per-project totals.

The ten new suites add **39** instances, distributed as measured:

| suite | trees | instances |
|---|---|---|
| `identity-grpc-client-test` | `cert`, `socket`, `identity`, `sync`, `memory`, `gateway`, `camera`, `productivity`, `notification`, `guard`, `tts`, `llm`, `voice` | 13 |
| `llm-client-test` | `memory`, `camera`, `guard`, `llm`, `voice` | 5 |
| `notification-client-test` | `gateway`, `camera`, `guard`, `notification` | 4 |
| `camera-sync-client-test` | `gateway`, `camera`, `llm` | 3 |
| `stt-client-test` | `camera`, `stt`, `voice` | 3 |
| `tts-client-test` | `camera`, `tts`, `voice` | 3 |
| `camera-action-client-test` | `camera`, `guard` | 2 |
| `productivity-sync-client-test` | `gateway`, `productivity` | 2 |
| `vlm-client-test` | `camera`, `guard` | 2 |
| `voice-client-test` | `gateway`, `voice` | 2 |
| **the ten** | | **39** |

The ledger is **428** against 2b's **389**, and the difference is exactly those
39: the **386** instances that are not client suites are the same number before
and after, so nothing else in the tree grew a test, and the pre-existing
`client-caller-identity-test` keeps its three. The eleven suites are not eleven
tests in one place — each instance runs against whatever the project that pulled
the package in resolved for gRPC and protobuf, which is how one of them found a
real defect in the build machinery while its identical twin in another project's
tree passed.

Eleven targets, thirty-two cases, 1782 lines of suite code — the column is
measured on the shipped tree, after the formatting pass and after the review
round's repairs to five of the eleven (see Deviations), so it is the figure this
commit carries:

| package | target | cases | lines | `add_test` at |
|---|---|---|---|---|
| `camera` | `client-caller-identity-test` (pre-existing) | 1 | 91 | `:35` |
| `camera` | `camera-sync-client-test` | 3 | 153 | `:51` |
| `camera-actions` | `camera-action-client-test` | 3 | 164 | `:33` |
| `identity` | `identity-grpc-client-test` | 3 | 160 | `:33` |
| `llm` | `llm-client-test` | 4 | 181 | `:69` |
| `notification` | `notification-client-test` | 2 | 162 | `:33` |
| `productivity` | `productivity-sync-client-test` | 2 | 141 | `:33` |
| `stt` | `stt-client-test` | 4 | 127 | `:44` |
| `tts` | `tts-client-test` | 5 | 197 | `:61` |
| `vlm` | `vlm-client-test` | 3 | 213 | `:40` |
| `voice` | `voice-client-test` | 2 | 193 | `:40` |

The ten new ones are registered in the shape `camera`'s target already used, which is
why that shape is worth spelling out: `enable_testing()`, `find_package(doctest
REQUIRED)`, `add_executable(<target> tests/unit/<target>.cc)`,
`set_target_properties(<target> PROPERTIES EXCLUDE_FROM_ALL FALSE)` — the folder is
pulled in `EXCLUDE_FROM_ALL`, so a suite that does not opt back in is absent when ctest
reaches for it — `target_link_libraries(<target> PRIVATE argus::clients::<name>
doctest::doctest)`, `target_compile_options(<target> PRIVATE -Wall -Wextra)` and
`add_test`. Two carry one line more: `llm` and `stt` add
`target_include_directories(<target> PRIVATE tests/support)`, because the fake server
their suite drives lives there and is imported by path rather than by a target.
`tts` does not, and that is a correction the review round forced: its own suite never
includes `tests/support/fake-tts-server.hxx`, which is driven by three suites outside
the package (`voice-tts-remote-test.cc`, `camera-talk-cutover-test.cc`,
`camera-action-rpc-test.cc`) that add that path in their own `CMakeLists.txt`. The
inert line was removed rather than kept, so the include path now appears exactly where
it is used and nowhere else.

Two naming facts, both measured rather than assumed. `identity`'s suite is
`identity-grpc-client-test` because `packages/lib/sqlite` already registers a suite
called `identity-client-test`: ctest tolerates the duplicate — it was verified before
deciding — but two rows of the same name in one ledger are two rows a reader cannot
tell apart, so the file was renamed. It was renamed with `mv`, not `git mv`, because the
file is untracked at the time of the rename and `git mv` refuses that. And `camera` is the
one package with two targets — it arrived with a suite and gained a second, differently
aimed one — so its new `AGENTS.md` documents both, one target per suite, rather than the
single-target sentence it would otherwise have carried.

## The ten `AGENTS.md`

The ten packages do not arrive at this step alike. Measured against `HEAD`: **six gain the file**
— `camera`, `camera-actions`, `identity`, `notification`, `productivity`, `voice` — and **four
already had one and are rewritten** (`llm` +80/−32, `stt` +73/−14, `tts` +116/−18, `vlm` +99/−22),
which are exactly the four packages 2c flattened. Either way the shape is the one 2b fixed for the
contracts: `# argus_clients_<name>` as the H1 — the CMake target, not the folder — a one-line
subtitle, then `## What this is`, `## Layout`, `## Rules`, `## Tests`. That is checked mechanically
rather than by eye: ten of ten carry exactly that H1 and exactly those four H2s in that order. Sizes
run 84–127 lines, and all ten use typographic characters (7–18 lines carry a non-ASCII character):
the repository's ASCII-only rule is a rule about code, and its tracked markdown has never followed
it — the root `AGENTS.md` alone carries 121 non-ASCII lines.

What each file carries, because the ten are the only description of these surfaces a consumer reads
before including one:

- the **consumer count, re-derived from the CMake graph** rather than from memory — the link lines
  that take the module, the `add_subdirectory` sites that pull it into a standalone tree, and the
  number of files that include the header, each stated separately, which is what makes statements
  like `identity`'s "21 files include it" checkable in one grep.
- the **wire facts**: which `x-argus-*` keys ride which call, the deadline constant and its value,
  and the plaintext channel, all read out of the `.cc`.
- the **config keys with `file:line` anchors**, including the ones that are declared by nobody and
  the service that never sets the generic key (`identity.rpc_host`/`rpc_port` for the gateway, the
  missing `camera.actions_credential` in every `.toml`).
- the **refusals that stay local** — the ids and tokens each method rejects before it opens a
  socket — which is what makes the suites' dead-target controls meaningful.
- the **`Nothing else:` inventory** (`find <package> -type f` spelled out), so a reader can see that
  nine of the ten have no `details/` folder instead of hunting for one.
- the **include prefix as load-bearing** (`<identity/identity-client.hxx>`, not the folder path) and
  rule 25, the folder IS the module, with the explicit source list and no `file(GLOB)`.

One document was found false by measurement and corrected here:
`packages/clients/vlm/CONTEXT.md:6` asserted that nullopt covers "transport, status and
empty-caption failures", and a transport failure is not one of them — it raises
`drogon::HttpException` out of `sync_wait`. The suite pins both sides of that line, so the sentence
now names the two cases that answer nullopt and the exception that does not. The four rewritten
files' numeric claims are the other half of this section's work, and every one of them was
re-measured independently after the subagent that wrote it reported its figures (see Verification).

## Deviations, and the one file outside the ten packages

### A single-flavor tree must not carry the abseil bridge (gate-blocking, fixed here)

The gate's first run after the ten suites were registered stopped in its second project, `socket`:
`12 of 13` with the new suite the failing one, and not on an assertion — `identity-grpc-client-test`
**segfaulted**, reproducibly, five runs out of five, after printing `[doctest]` nothing at all. A
`gdb` backtrace showed unbounded mutual recursion between `grpc_call_run_cq_cb`
(`packages/lib/grpc/src/grpc/grpc-cq-bridge-entry.cc:7-13`) and `argus::bridgeCq`
(`grpc-cq-bridge-exit.cc:9-12`): the entry hands the abseil invocable to `argus::bridgeCq`, and the
exit calls `grpc_call_run_cq_cb` again.

Why that is a recursion and not a round trip is a property of the *flavor* count, and it is the whole
of the defect. The entry defines `grpc_call_run_cq_cb(const grpc_call*,
absl::lts_20260526::AnyInvocable<void()>&&)`, and the vendored `libgrpc++.so` itself carries that
symbol as **undefined** — it expects `libgrpc.so` to provide it. With one abseil flavor in the
binary, the entry's definition *is* that symbol, so it interposes the real implementation, and the
exit's call back into it lands on the entry. Measured both ways in the same build tree:

| binary | `grpc_call_run_cq_cb` | abseil flavors in the binary | result |
|---|---|---|---|
| `socket`/`identity-grpc-client-test` | `T 0000000000097783` (local definition) | one — 773 references, all `lts_20260526` | recurses, segfaults |
| `camera`/`camera-action-rpc-test` | `U` (binds to `libgrpc++.so`) | one | passes |
| `llm`/`argus-llm` | `T ... absl::lts_20260107::AnyInvocable ...` beside `U ... lts_20260526 ...` | two — 53,506 symbol lines naming `lts_20260107` against 41 naming `lts_20260526` | passes |

The third row is the reason this is a *condition* and not a broken pair: `argus-llm` carries a local
definition too, but in the *second* flavor, so it shares no symbol with libgrpc and nothing
interposes — the bridge does exactly what it was built for. Every cell in the table is
`nm -C <binary>` — the flavor counts are that output piped to `grep -c`, and they are a magnitude
and not a ratio: what the row has to show is that the two flavors coexist in one binary at all. The two configurations are decided by
one thing: `argus_contracts_substrate()` appends the **Conan abseil export's** include directory to
`ARGUS_PROTOBUF_INCLUDES`, and the bridge entry is compiled with it. A tree where that export exists
(`llm`, `voice`, `memory`, `tts`) compiles the entry against the second flavor and gets a distinct
symbol; a tree without it (`socket`, `cert`, `camera`, `gateway`, `guard`, `notification`,
`productivity`, `stt`) compiles it against the host's abseil and gets a colliding one. Measured
before the fix: **seven of the thirty-nine** `libargus_clients_*.a` archives in the dev trees carried
the two bridge members, and which seven was an accident of configure order, not a decision.

`packages/contracts/CONTEXT.md:45-50` had already written the hazard down — "with a single abseil
flavor the cq bridge entry symbol would interpose the identically-named implementation inside
`libgrpc`, so the standalone configure declares empty bridge stand-ins" — and the gate trees did the
opposite. The fix is two sites in `cmake/argus-module.cmake` and nothing else:

- `:347` — the substrate records `ARGUS_SECOND_ABSEIL_FLAVOR` exactly where it puts the Conan abseil
  includes in play, which is the only place that knows whether there are two flavors at all.
- `:172-175` — `argus_grpc_absl_bridge()` returns before creating anything when that property is
  absent. No caller needs editing: the `if(TARGET argus_client_grpc_bridge_entry)` guard at `:476`
  then skips the append in `argus_client_module` by itself, and `packages/lib/grpc/CMakeLists.txt:27`
  gets an immediate return.

Measured after: the socket tree's archive is `grpc-client-base.cc.o` plus the three proto/client
objects, the suite's binary binds `U` like the passing camera one, and the suite passes **5/5 runs,
21 assertions each, 0 warnings** on its own recompile. The `llm` tree was re-measured as the
regression control: its `identity` and `camera` archives still carry both bridge members and
`argus-llm` still shows `T ... lts_20260107 ...` at the same address, so the two-flavor
configuration is byte-for-byte what it was.

This is in this step because the step is what surfaced it — these are the first suites to run an RPC
inside a single-flavor tree while the bridge rode in the archive — and because the alternative was to
register nine suites and quietly leave the tenth out. It is flagged to the user as a scope question
rather than presented as settled: the change is in shared build machinery, and the honest reading is
that `argus_client_module` was appending a construct that its own package's documentation says must
not exist in that configuration.

### The three findings of the suites' first run (all fixed here)

The suites were written to pin what the ten clients put on the wire, and three of
them failed on their first real run for reasons that were not the suite being
badly written — one was a defect in the client, two were checks asserting
something the wire cannot carry. All three are recorded because a suite that has
only ever confirmed is a suite that has not been aimed yet.

**`voice`: `finish()` dropped every frame the caller had already written.** The
suite writes four frames — `start`, `pcm`, `skip`, `stop` — then calls
`finish()`, and the server received one: only the start. The cause is in
`VoiceStreamImpl::finish()` (`src/voice/voice-client.cc`), which latched
`writesDone_` and called `StartWritesDone()` in the same breath, while the drain
path refuses to start a write once that latch is set:

```cpp
if (writing_ || pending_.empty() || writesDone_)   // drainLocked, before
  return;
```

so the three frames still queued were never written and the half-close went out
behind them; the server's `Read` loop ended after the first frame. The latch was
in the wrong place — frames already queued are part of what the caller asked to
send, and only *new* ones are what `finish()` is meant to stop. `finish()` now
sets the latch and drains under it, and the close moved to a `maybeCloseLocked()`
that runs when the queue empties with nothing in flight; the interface grew the
sentence that says so (`voice-client.hxx:30-32`). Measured after: four frames in
order, carrying `start()`'s identity and the connect-time metadata, five runs out
of five, 22 assertions each.

The first version of that fix was itself wrong, and how it failed is worth a line
because it costs nothing to state and everything to hit again: it cleared
`writing_` *after* the branch that decides whether to chain the next write, so
`drainLocked()` early-returned on a flag that was still true, the queue stalled
with the stream open, `waitClosed(5000)` returned false, and the process then sat
in `Server::Shutdown()` — the server-side handler was still blocked in `Read` on
a stream the client had never closed. `timeout 30 ./clients/voice/voice-client-test
--test-case="one stream*"` measured `exit=124`, `real 0m30.004s`, with doctest
reporting the `SIGTERM` rather than a failure of its own; the flag now clears
before the drain decision.

**`camera`: the deadline bound was wrong, the deadline was not.** The suite
asserted that the deadline the client puts on the wire was `<= 5000` ms, the
value of `kPullTimeoutMs`. It failed, reproducibly, on the same 5009. The client
sets the deadline correctly — `argus::client::setDeadline(context,
kPullTimeoutMs)` — and the difference is the wire: a gRPC deadline travels as a
*relative* timeout and the server rebuilds the absolute one from its own clock,
so what the server reads lands a few ms above the constant. The bound exists to
tell a deadline apart from none at all, and no deadline at all reads as ~9.2e15
ms; 5009 is a deadline. The bound is 6000 now, with the measurement in the
comment beside it, and both checks carry `CHECK_MESSAGE` so the next failure
names the number. Five runs out of five, 20 assertions each.

**`camera-actions`: the check pinned an ack the wire never carries.** The case
"an ack's outcome is read off the wire, never inferred" set `accepted = true`
and `detail = "in_flight"` on one ack and required the detail to outrank the
flag: the client answered `SUCCEEDED` (1) where the check wanted `CONFLICT` (7),
and `inFlight()` read false. Measured against the producer, the client is right
and the check was wrong. `finishAck`
(`services/camera/src/feature/actions/camera-action-rpc-service.cc:71-80`)
*derives* `accepted` from an outcome that every verdict sets explicitly —
`accepted` is true only for `SUCCEEDED` and `DUPLICATE_SUCCEEDED` — so no ack
carrying `in_flight` or `command_id_conflict` carries `accepted` too, and the
client's own `AGENTS.md` already documented the precedence the code has (explicit
outcome, then duplicate, then accepted, then detail). The case now clears
`accepted` before the two detail-only readings, which is the shape the fallback
exists for, and its name says what it pins. Both trees that collect the suite
(`camera`, `guard`) pass five runs of five, 31 assertions each. It is the same
species as the false constraint 2b removed from the seven catalogs — a suite
asserting a rule the producer never made — and the second time in this step that
reading the producer settled which side was wrong.

### The review round: what it found, what is fixed, what is flagged

The ten suites and the ten `AGENTS.md` files were reviewed after they were written, by readers who
were told to treat the documents as claims. Six things changed as a result. Every finding below was
re-measured here before it was acted on, and two of the review's claims did not survive that.

**Fixed — `vlm`'s suite failed in doctest's random order.** The fake is one object for the whole
process (`fakeVlmService()`), and the envelope case left the last thing it had scripted — a 503 —
behind for whichever case ran next. Reproduced before the fix: `--order-by=rand --rand-seed=7`
failed at `vlm-client-test.cc:127` (`REQUIRE( result.has_value() )`), 2 passed / 1 failed, while the
same binary passed 3/3 in default order. Each case now asks for the answer it needs. Measured after:
five seeds (1, 7, 13, 42, 99) in both trees that collect the suite, `camera` and `guard`, 3/3 with 23
assertions each — ten runs of ten.

**Fixed — the `voice` suite hung instead of failing, and locating the block corrected the
mechanism.** The case's last statement was `server->Shutdown()`, so a failing `REQUIRE` above it
unwinds past it with the stream still open, which is the regression the suite exists to catch. Three
probes, each measured on the real binary:

- with the stream left open, the process printed nothing and was still running at 30 s
  (`timeout 30` → `exit=124`);
- a bounded `Shutdown(deadline)` on that last line changes nothing, because the abort never reaches
  it — the same probe with the bounded form measured `exit=124` again, and a marker placed before
  that line never printed. The first version of this fix was therefore inert, and is recorded here
  rather than left as a comment claiming a mechanism it does not have;
- the block is not in the server. Markers at `~CollectingObserver` and after the client's own scope
  printed the observer's destructor and then nothing, which puts the wait inside `~VoiceClient`:
  the channel teardown waits on the call that is still in flight.

The fix is a `CallCleanup` guard declared between the client and the stream, so it is destroyed after
the stream and before the client: its destructor cancels the live call from the server side
(`ServerContext::TryCancel()`, the documented way to unblock a synchronous handler's `Read`) and then
shuts the server down with a deadline. Measured after, with the same regression still in place:
doctest **reports** `REQUIRE( observer->waitClosed(5000) ) is NOT correct` at `:173`, three runs of
three, instead of hanging with no output. The process then dies with a `SIGSEGV` during the same
teardown — the reactor-lifetime defect flagged below, which is pre-existing — so the abort path still
ends in a signal, but the failure is reported first and ctest sees a failing test rather than a
timeout. The shipped path measured after the guard: five runs of five in each of `gateway` and
`voice`, 22 assertions each.

**Fixed — the `vlm` suite dirtied whatever tree it ran from.** Starting drogon creates 256 upload
subdirectories (`uploads/tmp/00`–`FF`) under its upload path, which defaults to the process's current
directory. Running the suite from the repository root left them there: 258 directories, no files,
`git check-ignore` exit 1. The fake now sends `setUploadPath` to a temporary directory. Measured
after: six runs from the repository root across both trees and `uploads/` does not reappear; the
stray tree was removed.

**Fixed — `camera-sync`'s deadline checks could not see a shrunken constant.** They asserted
`> 0 && <= 6000` against a 5 s constant (`camera-sync-client.cc:7`), so a regression to 1 ms passed.
Both pairs now carry a 4000 ms floor beside the 6000 ms ceiling, with the reasoning — the floor
catches a constant that shrank, the ceiling tells a deadline from none at all (~9.2e15) — in the
comment. Measured after: five runs in `camera`, three each in `gateway` and `llm`, all green.

**Fixed — `tts`'s `CMakeLists.txt` wired an include path nothing used**, which is also what the
review caught in that package's `AGENTS.md` (see the suites section).

**Flagged, not fixed — five defects the review reproduced, all in code this step did not touch.**

- `VoiceStreamImpl` never calls `AddHold`/`RemoveHold` on its callback reactor, and `begin()` starts
  the call for a stream whose writes can begin from any thread (`sendPcm` →
  `drainLocked` → `StartWrite`). The vendored header states the rule
  (`grpcpp/support/client_callback.h:308-321`): a `StartWrite` initiated from outside a reaction
  needs a hold taken before `StartCall`. The review reproduced a SEGV inside
  `ClientCallbackReaderWriterImpl::Write` with the server ending the session while an app thread was
  in `sendPcm`. This is pre-existing, and the drain fix slightly narrows the window rather than
  widening it.
- The reactor and its `ClientContext` can outlive nothing: `self_` is built with a no-op deleter
  (`voice-client.cc:131`), so it keeps no object alive, and the context is owned by the stream. The
  header promises the stream owns the observer until `OnDone`, which invites a caller to drop the
  handle early; the review reproduced a heap-use-after-free in `InterceptorBatchMethodsImpl` freed by
  `~VoiceStreamImpl`. It is also the `SIGSEGV` the fixed voice abort path ends in.
- `sendPcm` and `writeFrame` drop frames silently once `finish()` has latched or the 1024-frame cap
  is reached — no return value, no callback, and the comment in the file is the only notice. The
  suite pins that the cap exists, not that anyone can observe it.
- `fake-llm-server.hxx` and `fake-stt-server.hxx` `recv()` without a timeout, so a client that
  connects and then stalls hangs those suites rather than failing them — the same species as the
  voice hang, one layer down.
- `vlm`'s dead-wire control uses `127.0.0.1:1` and assumes the connection is refused; where
  something answers on that port the case changes meaning. It is a control, not an assertion, so it
  is recorded rather than rewritten.

**Rejected — two claims came back from the reviewers and neither survived re-measurement.** Both
reviewers, independently, reported that the `AGENTS.md` lines carrying one of the eight spellings at
`HEAD` number six — 2 in `clients/llm`, 2 in `clients/tts`, 1 in `clients/stt`, 1 in `services/tts` —
and not nine. Nine is what the tree says, and both read six for the same reason: they searched for
the spelling in its include form, `<shared/services/llm/llm-service.hxx>` and its siblings, while
four of the nine lines carry the same path in backticks with the old `src/` prefix instead —
`clients/llm/AGENTS.md:15` and `:18` and `clients/tts/AGENTS.md:15` among them. What settles it is
`git grep -F -e '<spelling>' HEAD -- '*/AGENTS.md'` run over all eight spellings rather than over
the bracketed four, which returns exactly nine lines: 4 / 3 / 1 / 1.

The ledger figures both reviewers declared unverifiable — 428 / 389 / 39 / 386, the 39 client
archives with 6 carrying the bridge members, the 13 trees carrying `clients/identity` — were
measured here and hold. What failed was each reviewer's starting point, in a different way: the
first had a working directory that had moved, and the second ran `find build` at the repository
root, where no such directory exists, because the eighteen build trees are
`services/<name>/build` and `packages/<name>/build`. Both are recorded because a review is evidence
too, and its false claims cost the same kind of time as a false document.

### The reviewers' second round: thirteen findings, all read from a tree that had already moved

The second reviewer audited every numeric claim in the plan row, this report, the ten `AGENTS.md`,
the root `AGENTS.md` and `vlm/CONTEXT.md`, and wrote down the command behind each verdict. It
reported thirteen falsified claims, and every one of them is a figure this report had already
corrected — the corrections landed while the review was reading the files, so what it measured was a
tree several edits behind the one it was reviewing. Each is listed here beside the value the shipped
tree carries, re-measured after the review closed, because for the record the useful question is not
which side was right but where each figure now stands.

| Claim the review falsified | Read at | Shipped value (measured now) |
| --- | --- | --- |
| "the row covers thirty-six packages" | plan:817 | forty-six; `ls` gives 15 + 10 + 10 + 11 |
| the "42 outside" split read as a line split | plan:817 | labelled files, 30 of them; the line split is 10/9/8/6/6/2/1 |
| "nine `AGENTS.md` lines at `HEAD`" | plan:817, report:87 | nine, 4/3/1/1 — the claim above, rejected |
| "History is the only place they remain" | report:89 | four carriers, the plan among them |
| "the plan carries none of them" | report:92 | the plan carries one, `<tts-client.hxx>`, at :817 |
| "six of the ten clients wrap a generated gRPC stub" | root `AGENTS.md:750` | seven — six pass `PROTO`, and `tts` reaches the same stub through `argus::contracts::tts` |
| voice 357 lines behind a 23-line `CMakeLists.txt` | voice:10 | 370 (59/221/90) behind a 40-line one |
| "six calls" on `VoiceStream` | voice:47 | five |
| "two of its three includers live outside the package" | voice:88 | three of three |
| notification 16-line `CMakeLists.txt` | notification:12 | 33 |
| productivity 16-line `CMakeLists.txt` | productivity:12 | 33 |
| `CONTEXT.md`'s stale transport claim, and the sentence quoting it | vlm:37, vlm:66 | corrected; the file states the exception rule at :6-8 |
| "the include path is added by those CMakeLists, not by this one" | tts:51 | true — and the inert line this `CMakeLists.txt` carried was removed rather than left wired to nothing |

Each row is one measurement — `git show HEAD:` for the `HEAD` figures, `wc -l`, `git diff --numstat`,
the per-package stub greps — and each figure is argued where it stands in the sections above. Its
verified list is worth as much as its findings: the four `AGENTS.md` diffstats (`llm` +80/−32,
`stt` +73/−14, `tts` +116/−18, `vlm` +99/−22), the sizes 84..127, the eleven-suite
table and its `add_test` line numbers, `enable_testing()` absent at `HEAD` in the four flattened
packages and present in each now, the two collection mechanisms with their anchors
(`packages/lib/auth/CMakeLists.txt:42-45`, `services/camera/CMakeLists.txt:369-380`), the name
`identity-grpc-client-test` and the `packages/lib/sqlite` collision behind it, the four §2.3
deviations, the root `AGENTS.md`'s rule 23 and rule 26 blocks, and `vlm/CONTEXT.md` as the one
document this step found false.

### What §2.3 still does not hold for

The rule is one sentence — **no consumer ever sees a protobuf type, a stub, a URL or a retry policy**
— and four spellings of it are measured false in the ten, each recorded in the package's own
`AGENTS.md` rather than smoothed over:

- **A protoc type crosses three of the ten surfaces.** `identity`'s answer types are protoc messages,
  `camera`'s single read answers `std::optional<argus::camera::v1::PullTableResponse>`, and
  `camera-actions` publishes `using CameraCommandOutcome = argus::camera::v1::CommandOutcome;` at
  global scope. Hiding them would mean re-encoding enums and rows the callers already switch on.
- **Nine of the ten have no `details/` folder.** The channel, the deadline and the metadata live
  inline in the single `.cc`. `llm` is the exception because it is the only one whose transport is a
  separate translation unit, and even there the folder is not private in practice:
  `<llm/details/llm-remote.hxx>` is included by five files outside the package.
- **`camera-actions` compiles `src/camera/`, not `src/camera-actions/`** — the target and the include
  prefix follow the package name, the source directory follows the domain it speaks, exactly as the
  section's own note about the shared `camera` domain allows.
- **`tts` keeps `tts-wire.hxx` in the client.** It is a wire vocabulary, so by §2.3's own logic it
  belongs in `packages/contracts/tts/`; it stayed where the consumers' includes already pointed,
  which is a decision the contracts step left open and this one did not take unilaterally.

### Left open for a decision

Four questions were raised to the user in free text and none has an answer yet, so nothing was
changed for them: whether the per-contract `proto/` root §2.3 draws can exist at all while
`argus_client_module` takes one `--proto_path` and seven protos name a domain no contract folder
owns; whether `tts-wire.hxx` moves to `packages/contracts/tts/`; whether `llm`'s `details/` access
should be closed with a surface factory; and whether `packages/clients/voice`'s
`argus::contracts::auth` dependency and `<auth/user-role.hxx>` include — provably unused, since no
symbol of the package reaches it, `voice.proto` imports nothing and the one `UserRole` consumer
includes the contract itself — should be deleted. The fifth, the protoc types crossing the `camera`,
`camera-actions` and `identity` surfaces, is the first bullet above. The defects the review round
reproduced are not questions: each is recorded with its reproduction and none was fixed here, because
all five live in code this step did not touch.

## Verification

The gate was run to completion twice on the tree this report ships with: once when the ten suites
were registered, and again after every fix of the review round, which is the run every figure below
is taken from — `./scripts/build-all.sh dev`, exit 0, closing line `[setup] All selected projects
built and tested (profile: dev).`, log at `/tmp/argus/gate-2c-review.log`. Every figure was then
re-derived from that run's own log or from `git` on the same tree, not from the notes the work was
done with, and the second run reproduces the first figure for figure.

**The gate.** 18 of 18 projects report `100% tests passed, 0 tests failed`, and the ledger is **428**
twice over: the eighteen per-project totals sum to 428 and the log carries exactly 428 numbered
`Test #` lines. `warning:`, `Not Run` and `***Failed` each count **zero**.

**The ledger, decomposed from that log.** The eleven client suites hold **42** instances — the
**39** the ten new ones add, plus the **3** the pre-existing `client-caller-identity-test` keeps — so
the **386** instances that are not client suites are unchanged against 2b's 389 − 3, and nothing
else in the tree grew a test. Counted per suite from the numbered list, the instances are
`identity-grpc-client-test` 13, `llm-client-test` 5, `notification-client-test` 4,
`camera-sync-client-test` 3, `stt-client-test` 3, `tts-client-test` 3, `camera-action-client-test` 2,
`productivity-sync-client-test` 2, `vlm-client-test` 2, `voice-client-test` 2 — the table above,
exactly.

**The two collection mechanisms, re-measured rather than restated.** Thirteen trees carry
`build/dev/clients/identity` — `cert`, `socket`, `identity`, `sync`, `memory`, `gateway`, `camera`,
`productivity`, `notification`, `guard`, `tts`, `llm`, `voice` — and the five whose `ctest -N` does
not name the suite are `sqlite`, `intent`, `stt`, `vlm` and `tunnel`, the five without the directory.
`services/camera/CMakeLists.txt:369-380`'s `foreach(CLIENT IN ITEMS llm vlm notification)` is the
missing instance on those three, as the section says.

**Every repaired suite, five times.** `camera-sync-client-test` 3 cases, 3 passed, `Status:
SUCCESS!` on five consecutive runs in the camera tree; `camera-action-client-test` likewise 3/3 five
times in the camera tree and five more in the guard tree; `voice-client-test` 2/2 five times. And the
suite the bridge defect broke: `packages/socket/build/dev/clients/identity/identity-grpc-client-test`
— reproducibly segfaulting five runs of five before the fix — is now 3 cases, 3 passed, `SUCCESS!`
on five consecutive runs, with `grpc_call_run_cq_cb` measured **`U`** in that binary where it was a
local `T` definition. Post-fix, **6** of the dev trees' 39 `libargus_clients_*.a` archives carry the
two bridge members — `memory/clients/identity`, `llm/clients/{camera,identity}`,
`tts/clients/identity` and `voice/clients/{identity,voice}` — and all six sit in the four trees that
carry the Conan abseil export (`llm`, `voice`, `memory`, `tts`), the condition the fix makes
explicit, where before the seven that had them were an accident of configure order. Both figures
were re-measured over all 39 archives after the second gate run, along with the thirteen trees and
the `U` binding above. Every repair of the review round was run the same way: the voice guard 5/5 in
each of the gateway and voice trees with 22 assertions each, plus the abort path with the regression
put back, which reports the failing `REQUIRE` 3/3 where it used to print nothing and hang;
`vlm-client-test` 3/3 under five `--order-by=rand` seeds in both trees that collect it, and six runs
from the repository root with no `uploads/` left behind; `camera-sync-client-test` 5/5 in camera
with its new floor and 3/3 each in gateway and llm; and `tts-client-test` 3/3 in each of the three
trees that collect it after the inert include line came out.

**The sweep, re-derived from `HEAD`.** `git grep` on `HEAD` restricted to `*.cc` and `*.hxx` gives
**46** lines in **34** files, split per spelling exactly as the table prints (13, 5, 9, 3, 5, 3, 3,
5), of which 4 are inside the four packages and 42 outside; grouped by owner, 9 in `services/llm`, 8
in `packages/memory`, 4 in `services/guard`, 4 in `services/tts`, 2 in `services/voice`, 2 in
`services/camera`, 1 in `services/stt`. Under the bare spellings the file count is **35** — the 34
plus `tts-client.cc`'s quoted self-include, which the report says is not one of the 46.

**The docs pass.** Nine `AGENTS.md` lines carried one of the eight spellings at `HEAD` — 4 in
`clients/llm`, 3 in `clients/tts`, 1 in `clients/stt`, 1 in `services/tts` — and none of the nine
survives in the tree. Four files on the shipped tree still print one, and `git grep` over the eight
exact spellings names them: this report, the plan (whose row prints `<tts-client.hxx>` as the retired
spelling), `docs/history/reports/f1-11-stale-docs.md` and
`docs/history/reports/f1-9-tool-framework-into-llm.md`. No code file, no `CMakeLists.txt` and no
`AGENTS.md` keeps one. The plan does carry `src/shared/services/...` elsewhere — eleven mentions on
nine lines — but only one of them is the `services/llm` spelling this pass was about, `:806`'s
`src/shared/services/llm/lfm-adapter.hxx`, and that file is still exactly where the sentence says it
is. Three more name the gateway's own internal `services/gateway/src/shared/services/socket/`, which
is live and which this step did not touch, and one names the tool runtime's new home under
`services/llm/src/shared/services/tools/`. The rest are records of a layout: `:281`, `:809` and
`:865` state what becomes of an empty `src/shared/services/`, `:817` is this step's own row, and
`:964` cites the `mdns` source path the `packages/lib/mdns` extraction has since removed.

**The ten `AGENTS.md`.** The four rewritten files' diffstats are `llm` +80/−32, `stt` +73/−14, `tts`
+116/−18, `vlm` +99/−22, and the six that gain the file are the six the section names. Ten of ten
carry `# argus_clients_<name>` as their only H1 and `## What this is`, `## Layout`, `## Rules`,
`## Tests` as their four H2s, in that order. Sizes run 84–127 lines; non-ASCII characters appear on
7–18 lines each against the root `AGENTS.md`'s 121. The **seventeen** includer counts asserted across
the ten were re-measured one by one, scoped to code files, and all seventeen match.

**The suites table.** 32 cases and 1782 lines, summed from the eleven files, and each `add_test`
line number read out of the `CMakeLists.txt` that carries it (the `tts` anchor moved from `:64` to
`:61` when the inert include line was removed). `enable_testing()` is absent at `HEAD`
in all four flattened packages and present in each of the four now.

**Six figures this report carried and corrected in place.** The suite-line total was 1714, became
**1728** when the Deviations work repaired two suites, and is **1782** after the review round grew
three more — the table is the shipped figure, and the plan row and the commit message carry the same
number. The suite count was **nine** and is **ten**: `camera` gained a second suite, so the ten
packages carry ten new suites across eleven targets. The claim that the plan carries none of the old
spellings is false — it carries one, and three history documents carry one each (above). Three of the
ten new suites were said to add `tests/support` to the include path and two do. The sweep's per-owner
list was labelled as lines and is files: 42 lines in 30 files outside, 9 in `services/llm`, 8 in
`packages/memory`, 4 each in `services/guard` and `services/tts`, 2 each in `services/voice` and
`services/camera`, 1 in `services/stt`. And the plan's row said the step covers thirty-six packages
against its own enumeration of 15 + 10 + 10 + 11 = 46, which is what the four package groups hold. One figure in the Deviations table was also replaced: the
`argus-llm` flavor counts were taken by a method that could not be reconstructed, and now carry
counts measured with the method the other rows use.

**Three measurement failures were mine, not the documents'.** They are recorded because each looked
exactly like a finding: an unscoped includer count, which was inflated by `AGENTS.md` and by this
report; a scope glob that could not see `packages/memory/src` and `packages/lib/auth/src` in one
pass, which made `identity`'s 21 and `llm`'s 14 look false when both are exact; and a non-ASCII count
run under a UTF-8 locale, which read the root `AGENTS.md` at 768 against a true 121 and the ten at
75–115 against a true 7–18. Every claim in the ten `AGENTS.md` files survived the corrected
measurement; the only numbers that did not survive anything were the ones in this report's own
tables, and those are corrected above.

