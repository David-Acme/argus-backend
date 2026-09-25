# Phase 4 step 6, part 6b — the argus-vlm internal gRPC leg

Step 6 of the architecture plan gives the three AI services with a legacy
internal wire a typed gRPC face beside it, following the `argus-tts` precedent
field by field. 6a did `services/stt` (report
`docs/history/reports/f4-6-1-stt-grpc-leg.md`, commit `6c4e4482`); this is 6b,
`services/vlm`, and 6c `services/llm` then owes the same shape.

## What existed before

- `services/vlm` served the internal HTTP wire alone (`POST /vlm/v1/describe`,
  `GET /vlm/v1/config`) and carried its own copy of the caller side —
  `feature/vlm/services/remote/remote-vision-adapter.{hxx,cc}` (an `IService`
  wrapper) and `feature/vlm/services/remote/vlm-remote.{hxx,cc}` (a second HTTP
  client) — 482 lines of code no service outside `services/vlm` could reach and
  no consumer inside it used either, since the guard calls the same wire through
  `argus::clients::vlm`. Rule 23's "duplicated copies are removed in the same
  change that introduces their replacement" is what this unit discharges.
- `packages/clients/vlm` held the caller side in `vlm-client.{hxx,cc}`: the
  global-namespace `VlmClient` façade, `VlmDescribeInput`, `VlmDescribeResult`
  and `VlmHttpClient`, with `argus::vlm` unused as a namespace. Its `AGENTS.md`
  recorded that as a §2.3 deviation.
- `packages/contracts/proto/argus/ai/v1/vlm.proto` — an `argus.ai.v1`
  skeleton no CMake target compiles and no source imports. It was one of four
  such orphans; the stt step deleted its own, and the tts step had left
  `ai/v1/tts.proto` behind even though `packages/contracts/tts` had replaced it.

## The contract: `packages/contracts/vlm`

Five files, on the `contracts/stt` and `contracts/tts` model:

- `vlm.proto` (27 lines) — `package argus.vlm.v1`, one `Vision` service with
  two **unary** RPCs: `Capabilities` (loaded, the input-pixel ceiling, the
  engine's default token cap) and `Describe` (JPEG bytes, prompt, camera id,
  token cap in; the caption out). Not a stream: a description is one image, not
  a tile sequence — the domain's own shape, the way stt's turn is one buffer.
- `src/vlm/vlm-errors.hxx` (52 lines) — the eleven refusals. Ten are shared
  `ErrorCode` spellings; `VisionEngineNotLoaded` is the one domain-specific code
  (503, `VLM_NOT_LOADED`), and `ErrorCode::VlmNotLoaded` already existed in
  `packages/lib/errors` since the service extraction, so nothing in `lib/errors`
  moved. The controller's local `vlm-errors.hxx` (16 lines) is deleted and the
  contract's has seven includers: the client, the gRPC server, the controller
  and the engine, plus the three suites.
- `CMakeLists.txt` (49 lines) — `argus_contracts(NAME vlm ...)` for the header
  vocabulary and `argus_vlm_rpc_contract()`, the function the package owns: it
  calls `argus_response_rpc_contract()`, ensures `lib/grpc` and declares
  `argus::contracts::vlm-wire` with `PROTO_ROOT` at the package folder, so the
  `.proto` sits at the root rather than under the group's shared `proto/` root.
- `tests/unit/vlm-contract-catalog-test.cc` (120 lines) — the eleven as a
  pinned table, each entry's wire legality and that no two say the same thing.
- `AGENTS.md`.

The catalog needed one judgement the stt one did not, and the document records
it: `BodyNotJsonObject` is the HTTP leg's alone (the gRPC leg carries
`bytes image_jpeg`, so there is no JSON body to refuse), while
`ImageNotDecodable` is the gRPC leg's spelling of bytes that are not an image —
the HTTP controller answers that case with a 422 field envelope instead, because
its surface carries field granularity and a status face does not.

## The client: the gRPC client and the façade

`packages/clients/vlm` now carries both transports, split the way §2.3 spells
them:

- `src/vlm/vlm-client.{hxx,cc}` is the gRPC client, `argus::vlm::Client` —
  `ClientConfig` (`target`, `credential`, `timeout` defaulting to 30 s),
  `kMaxTimeout` (120 s), `Capabilities` and `DescribeInput` (`jpeg`, `prompt`,
  `cameraId`, `maxTokens`, `0` meaning the engine's default). It is the name
  §2.3 reserves, and before this unit the package's `-client` file held the HTTP
  façade instead.
- `src/vlm/vlm-remote.{hxx,cc}` (58 + 114 lines) is the HTTP transport and the
  façade: `VlmHttpClient` plus `VlmClient`, `VlmDescribeInput`,
  `VlmDescribeResult` — the global-namespace surface guard already spells,
  preserved name for name and signature for signature, which is why guard's
  edit is four include lines and nothing else.

The façade picks the transport per call from process-global config: the gRPC
path when `vlm.grpc_target` is set (`vlm.grpc_credential` as the caller's
credential, the constructor's `timeoutS` as the deadline), the HTTP path
otherwise, and `remote()` is true when either is set. The gRPC client is cached
in a `std::atomic<std::shared_ptr<RpcCache>>` whose entry carries the target it
was built for: `rpcClient()` re-reads the knob on every call and adopts the
cached client only while the target still matches (`compare_exchange_weak`), so
a racing thread adopts the winner's client and a runtime change of the target
rebuilds before the next call. The blocking unary call runs inside
`BlockingTask`, off the Drogon event loop, because the façade is a coroutine
caller (rule 13c).

The failure vocabularies stay as the stt precedent left them, and the asymmetry
is real rather than accidental: the gRPC leg raises `ResponseException` (the
contract's eleven, or what `argus::response::fromRpcStatus` makes of a bare
transport failure or a malformed detail), and the façade folds every one of them
into `std::nullopt` — the gRPC client's own construction included, so a target
set with an empty credential or a façade timeout past two minutes answers no
caption instead of throwing out of the coroutine, which is what guard reads as
"no caption" and carries on with — while the HTTP leg's transport failure raises
`drogon::HttpException`
out of `sync_wait` (drogon 1.9.13, `HttpClient.h:384` through
`coroutine.h:500`) and escapes at `guard-assessment.cc:298` and `:362`. The
suite pins both sides.

The constructor is the client's gate, exactly as stt's is: an empty target or an
empty credential is 400 `InvalidRequest`, a timeout of zero or below or over two
minutes is 400 `InvalidRequest` carrying `"timeout must be within 1 and 120000
ms"`, and two minutes exactly is accepted. `describe` refuses an empty JPEG and
a negative `maxTokens` locally, before the channel is used; `capabilities()`
refuses a reply outside `1..16384` / `0..4096` as 502 `InvalidResponse`, and so
does a `describe` that answers an empty caption.

## The service: `src/app/rpc/` and the seam

`services/vlm/src/app/rpc/vlm-rpc-server.{hxx,cc}` (30 + 172 lines) is
`argus::vlm-rpc`, the module the feature module links nothing of and the
executable links explicitly: `main.cc` composes it only when `rpc.address` and
at least one non-empty `[rpc.callers]` pair are set, and shuts it down after
`app().run()` returns. The server holds the same `VisionService` the controller
drives, reached through two callbacks — a live `capabilities` read and a
`describe` taking the decoded `cv::Mat` — so the caption a gRPC caller receives
is the caption the HTTP route would answer, caption cache included. The
real-engine case in the suite pins that by construction: it calls the RPC client
and `VisionService::describeMat` for the same JPEG and prompt and asserts the
two strings are equal.

The face follows the stt server field by field: the caller must present the
credential header exactly once (zero or two entries are 401, compared in
constant time over the pairs), an invalid request is 400, a stalled call is 499
or 504 depending on whether its deadline had passed, the engine's inference
slots (`ThreadBudget::inferenceSlots()`, never a global mutex) are acquired with
`try_acquire` and released by an RAII guard with 429 `Busy` for the loser, every
`ResponseException` goes through `toRpcStatus` and anything else is sanitized to
500, and a caller that sends no deadline at all is served.

Two things the domain forced. The bounds are the wire's own: a JPEG over 32 MiB,
a prompt over 512 bytes, a camera id over 64 and a `max_tokens` outside
0..4096 are 400 `InvalidRequest`, and bytes that decode to nothing are 400
`ImageNotDecodable` — the HTTP leg's field-level 422 has no analogue on a status
face, which is why the catalog carries both spellings. And the deadline ceiling
needed a measurement no precedent had taken: gRPC carries a deadline as a
relative `grpc-timeout` header and rounds it, so a server that refuses anything
beyond the client's own two-minute ceiling **refuses the client's own
maximum**. It was measured on the wire rather than inferred — a raw
`wire::Vision::Stub` call with a 120 s deadline was refused while the same call
with 5 s was served — and the ceiling is now `argus::vlm::kMaxTimeout` plus one
second, named as the header's rounding rather than as a magic 121.

The engine seam is the stt one: `VisionService::run` used to log "service not
loaded" and return an empty caption; it now throws
`VisionEngineNotLoaded` (503). The HTTP route is unchanged observably — the
controller checks `isLoaded()` first and throws the same 503, as
`vlm-wire-test.cc` already pinned — and the throw is what lets the gRPC server
answer the same 503 instead of a caption nobody asked for.

## The suites

- `services/vlm/tests/unit/vlm-rpc-test.cc` (568 lines, fifteen cases) drives
  both sides. Through `argus::vlm::Client`: the capabilities round trip, a
  capabilities reply outside the contract refused as 502 `InvalidResponse`, the
  engine's answer for a prompt-less and a camera-tagged request, wrong
  credentials on both RPCs, a typed refusal round-tripping from the server
  (`503 VLM_NOT_LOADED`) while a `std::runtime_error` from the engine is
  sanitized to 500 with the message absent, invalid requests refused before the
  engine runs (`calls == 0`), the `fromRpcStatus` table for transport failures
  read off the wire, and the guard façade taking the gRPC leg when the knob is
  set and falling back to the HTTP leg when it is cleared. Through a raw
  `wire::Vision::Stub` — which is the client minus its gate: the six request
  bounds, the deadline ceiling from both sides (120 s served, 125 s and 300 s
  refused), a missing credential, and a request presenting the credential header
  twice. Three cases are latch-driven and therefore deterministic — a deadline
  that expires while the engine is held, an external `TryCancel`, and a second
  call refused 429 while the single slot is held — and each releases the engine
  before it asserts, so a regression fails instead of hanging: the entry signal
  is a `std::counting_semaphore` with a ten-second bound rather than a latch, so
  a call refused before the handler runs cannot park the canceller thread on a
  `join()` either. A final case loads the real LFM2.5-VL engine, starts the
  server over it and compares the caption the wire answers with the one the
  in-process call answers for the same image.
- `services/vlm/tests/support/app-loop.hxx` (58 lines) gives the suites a
  running Drogon loop, which `BlockingTask` needs; `image-fixture.hxx` (30
  lines) is the JPEG both the client suite and the RPC suite encode (OpenCV,
  a rectangle on a canvas, with a `decodedJpeg` twin for the in-process
  comparison).
- `packages/clients/vlm/tests/unit/vlm-client-test.cc` grew 120 lines and a
  sixth case: the pinned constructor-gate table (empty target, empty
  credential, the two out-of-band timeouts, two minutes accepted) with the local
  empty-JPEG and negative-`maxTokens` refusals, a case that sets the runtime
  knob, watches the façade switch legs, and clears it again, and — the case the
  review pass added — a misconfigured leg (target set with an empty credential,
  and a façade timeout past two minutes) answering `nullopt` rather than
  throwing, written against a live HTTP server so a fallback to the HTTP leg
  would answer a caption and fail the check.

## Deletions

- `services/vlm/src/feature/vlm/controllers/vlm-errors.hxx` — superseded by the
  contract catalog, every entry carried.
- `services/vlm/src/feature/vlm/services/remote/` — `remote-vision-adapter` and
  `vlm-remote`, both `{hxx,cc}`: a service does not host a client of itself, and
  the caller side is `argus::clients::vlm`.
- `services/vlm/tests/unit/vision-remote-adapter-test.cc` (222 lines) and
  `services/vlm/tests/support/fake-vlm-server.hxx` (171 lines) — the suite and
  the fake that existed to drive the deleted adapter.
- `packages/contracts/proto/argus/ai/v1/vlm.proto` — the orphan skeleton.
- `packages/contracts/proto/argus/ai/v1/tts.proto` — dead since the tts step
  built `packages/contracts/tts` without deleting its skeleton. The 6a report
  assigned this file to 6c along with `llm.proto`; that was a misattribution —
  nothing owns a schema already replaced. It is removed here, and
  `ai/v1/llm.proto` is what remains for 6c, which replaces it the same way and
  empties the `ai` domain.

## Dormant by default, as the precedent is

Verified rather than assumed: `services/vlm/config.toml.example` declares
`[rpc] address = ""` and one empty `[rpc.callers]` pair; no deploy config sets
`[rpc]` for this service (measured over `argus-deploy/config.vlm.toml`, which
carries no `rpc` or `grpc` key at all); and no toml in the tree sets
`vlm.grpc_target` or `vlm.grpc_credential`, so the façade takes the HTTP leg and
the process composes no gRPC server. The leg is live, reachable and off — the
cutover is a later decision, and the only code that sets either knob is the two
suites.

## Verified

- `./scripts/build-all.sh dev --only vlm` — exit 0, 0 warnings, 17/17 tests
  passed.
- `./scripts/build-all.sh dev --only guard` — exit 0, 0 warnings, 56/56 tests
  passed, which is the consumer contract holding through the four include-line
  repoints.
- `./scripts/check-comments.sh` — 1371 files, 0 comments (6a recorded 1368 over
  a smaller tree, so the two counts are not comparable as a delta).
- `./scripts/check-deps.sh` — 118 declarations, 840 edges, 0 forbidden, 0
  cycles, 0 unresolved, 22 deferred.
- `./scripts/check-tidy.sh` over the frozen tree — exit 0: 544 translation
  units, 2876 findings over 45 checks against a baseline of 2901, no check
  above it (the first scan of this unit's tree failed, at 207 against 205 on
  `bugprone-unchecked-optional-access` and 23 against 22 on
  `performance-unnecessary-copy-initialization`; the three assertion sites and
  the one copy are in the review pass above).

## The review pass

Two adversarial readers went over the frozen change — one over the code, one
over the documents — and every finding was re-measured here before anything
moved. Ten findings: nine fixed (two code defects, six documentation errors,
one count in this report) and one recorded; three further observations the code
report raised are recorded beside it, and its remaining items were deviations
it verified as deliberate and correct.

Fixed:

- **The façade could throw where its own contract says it answers `nullopt`.**
  `VlmClient::describe` called `rpcClient()` outside the fold, and the gRPC
  client's constructor is a refusal gate of its own (400 `InvalidRequest` for an
  empty credential or a timeout outside one millisecond to two minutes), so a
  leg pointed at a target with no credential — or a guard timeout past two
  minutes — raised `ResponseException` out of the coroutine and into
  `guard-assessment.cc:298`/`:362`, the two call sites that only read the
  optional. Not reachable in any shipped config (both knobs are unset
  everywhere), but reachable on the cutover the leg exists for, and the client
  `AGENTS.md` stated the invariant that was false. The construction is now
  inside the fold, and the client suite gained the case that pins it.
- **Two latch waits could hang the suite instead of failing it.** In the
  cancellation case the held engine was released only after
  `REQUIRE_FALSE(status.ok())`, so a regression that made `TryCancel`
  ineffective would unwind into `~VlmRpcServer` → `Wait()` with the handler
  parked on a latch nobody would ever count down; and the canceller thread's
  `cancelEntered.wait()` was unbounded, so a call refused before the handler ran
  would park `join()` forever. Both are the same defect the report had claimed
  the opposite of. The release now happens before the first assertion, and the
  three latch-driven cases signal entry through a `std::counting_semaphore`
  acquired with a ten-second bound, so every one of those regressions fails
  instead of hanging.
- **`packages/contracts/AGENTS.md` was wrong about its own group, twice.** "The
  four domains that grew a gRPC boundary each moved their schema" reads as a
  claim about the tree, and nine domains have a gRPC service from the shared
  root (measured over `proto/argus/*/v1/*.proto`); and the bullet described all
  four `response/`, `stt/`, `tts/`, `vlm/` packages as carrying a
  `<domain>-errors.hxx` and owning an `argus_<domain>_rpc_contract()` that
  *calls* `argus_response_rpc_contract()`, where `response` has no such header
  and is where that function is *defined*.
- **Two stale measurements in the client `AGENTS.md`**: the escaping consumer
  call sites are `:298` and `:362` (the `:363` was inside the call), and
  `guard.assess.vlm_url` is declared in three places, not one — the deploy
  config at `:131` plus both templates.
- **A misdescribed test outcome** in the same file: after clearing the knob the
  façade under test answers `nullopt`; the canned caption comes from a second
  façade built over the fake server's URL.
- **"links `argus::contracts::<domain>-wire` through `argus::clients::<domain>`"**
  — wrong in all three of `packages/contracts/{stt,tts,vlm}/AGENTS.md`, since
  the client links the wire PRIVATE and the server module names the target
  itself (`services/{stt,tts,vlm}/src/app/rpc/CMakeLists.txt:3`). Fixed in all
  three rather than only in this unit's, because it is one sentence repeated.
- The report's own count arithmetic was corrected with them: it had derived
  "1371 files" as *one fewer than 6a's 1372*, and 6a measured 1368.
- **The tidy ratchet caught what the readers did not.** The frozen tree had
  risen two checks above the baseline — `bugprone-unchecked-optional-access`
  207 against 205, `performance-unnecessary-copy-initialization` 23 against 22
  (`/tmp/tidy-6b2.log`, exit 1) — from three `REQUIRE(x.has_value())` followed
  by `x->field` assertions in the two suites, a shape clang-tidy cannot read
  through doctest's macro, and one needless copy in the façade
  (`const VlmDescribeInput ask = input;`, which the detached `BlockingTask`
  lambda needed as a value but can take as a by-value capture of the parameter
  instead). All four sites are now `value_or` comparisons and a
  `[client, input]` capture, and both counts are back at the baseline.

Recorded, not fixed:

- **The catalogue's `CHECK(kCatalog.size() == 11)` pins nothing** — the array's
  extent is the same 11 — though the per-entry name/definition/code/status/
  message table beside it does, and so does the duplicate scan. It is the shape
  `stt` and `tts` both carry (`stt-contract-catalog-test.cc:88`,
  `tts-contract-catalog-test.cc:51`), so it stays for parity; the honest
  statement is that "the eleven" is pinned by the table, not by the count.
- **`camera_id` is bounded and then dropped.** The gRPC server validates it and
  does not pass it to the engine seam, while the HTTP route logs it;
  `VisionDescribeMatInput` has no such member, and the request-side bound is
  therefore a wire-shape check, not engine steering. Left for whoever gives the
  seam a camera context.
- **`VlmClient::remote()` has no production reader** — guard, its only consumer,
  never calls it. It is the name-for-name surface the stt and tts façades carry
  (where it *is* read), kept so the guard-side include edit stayed four lines.
- **The HTTP leg's `drogon::HttpException` escape** at the same two call sites
  is unchanged and predates this unit: in drogon 1.9.13 a transport failure on
  `sendRequestCoro` rethrows through `sync_wait`, which the façade cannot fold
  without a second try/catch around the HTTP path. Pinned by the suite on both
  sides, documented in the client `AGENTS.md`, and left as it is because
  changing it changes what guard sees, which is not this unit's call.

## Flagged, not fixed

- The deadline ceiling in `services/tts/src/app/rpc/tts-rpc-server.cc:111` and
  `services/stt/src/app/rpc/stt-rpc-server.cc:20` is a flat 120 s, which is the
  same defect this unit measured: a caller asking for the client's own maximum
  was refused at the header-rounding boundary. The stt server was briefly
  raised to the same `kMaxTimeout + 1 s` shape during this unit and then
  reverted, because both services are outside 6b's scope and their own
  documents state the two-minute ceiling as a fact. It is a one-line fix owing
  its own change, with this report as the measurement.
- `packages/contracts/AGENTS.md`'s validation line now names both roots: buf is
  absent from this machine and its module declares `path: proto`, so the four
  boundary contracts (whose protos sit at their package roots) were validated
  with a second `protoc` invocation. Whether `buf.yaml` should gain the four as
  modules is a step 8 decision, not this unit's.
