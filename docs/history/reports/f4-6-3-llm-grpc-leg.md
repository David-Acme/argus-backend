# Phase 4 step 6, part 6c — the argus-llm internal gRPC leg

Step 6 of the architecture plan gives the three AI services with a legacy
internal wire a typed gRPC face beside it, following the `argus-tts` precedent
field by field. 6a did `services/stt` (report
`docs/history/reports/f4-6-1-stt-grpc-leg.md`, commit `6c4e4482`) and 6b
`services/vlm` (`docs/history/reports/f4-6-2-vlm-grpc-leg.md`, commit
`483c77fb`); this is 6c, `services/llm`, the largest of the three: its wire has
three operations where stt and vlm have two, and one of them streams.

## What existed before

- `services/llm` served the internal HTTP wire alone (`POST /llm/v1/chat`,
  `POST /llm/v1/chat-stream`, `GET /llm/v1/config`) with the tool-loop decision
  taken inside the controller: `chat` and `chatStream` each asked
  `body.toolsEnabled ? registeredTools() : {}` and either called the engine
  directly or drove `LfmAdapter::chatWithTools(.Stream)`. That made the HTTP
  handler the only entry to the brain, which a second surface cannot reuse.
- `packages/clients/llm` carried the caller side under `src/llm/details/`:
  `details/llm-remote.{hxx,cc}` — `LlmRemoteConfig`, the HTTP transport
  `LlmHttpClient` — 502 lines of `.cc` no consumer could reach by a documented
  path, because `details/` is the level the layout rules retire.
- `services/llm/src/feature/llm/controllers/llm-errors.hxx` was the service's
  local refusal catalog, and `packages/contracts/proto/argus/ai/v1/llm.proto`
  the last orphan skeleton of the `ai` domain: no CMake target compiled it and
  no source imported it, exactly as `stt.proto`, `tts.proto` and `vlm.proto`
  were before their own steps replaced and deleted them.

## The contract: `packages/contracts/llm`

Five files on the `contracts/vlm` model, the fifth contract under
`packages/contracts/` that is not header-only:

- `llm.proto` (50 lines) — `package argus.llm.v1`, one `Chat` service:
  `Capabilities` and `Chat` **unary**, `ChatStream` **server-streaming**. The
  request carries what the HTTP body carries (`messages`, `max_tokens`,
  `temperature`, `reset_context`, `tools`, `grammar`, `grammar_required`,
  `user_id`) and the stream answers one `ChatToken` per token, terminated by a
  token whose `done` is set and which carries the three prefill counters — the
  job the HTTP sentinel line does. It is the second streaming RPC among the
  five boundary contracts (the voice session's bidirectional `Connect` and the
  camera's `Subscribe` stream too; among this group it follows
  `argus.tts.v1.Synthesize`).
- `src/llm/llm-errors.hxx` (48 lines) — the ten refusals. `LlmEngineNotLoaded`
  (503, the one domain-specific code; `ErrorCode::LlmNotLoaded` already lived
  in `packages/lib/errors`) plus `BodyNotJsonObject` 400, `InvalidRequest` 400,
  `Unauthorized` 401, `Cancelled` 499, `DeadlineExceeded` 504, `Busy` 429,
  `InternalError` 500, `InvalidResponse` 502 and `Unavailable` 503. The
  controller's local catalog is deleted; the contract's has six includers —
  the client's `.cc`, the gRPC server, the controller and the three suites
  (the catalog's own, the client's and the service's).
- `CMakeLists.txt` — `argus_contracts(NAME llm ...)` for the vocabulary, plus
  `argus_llm_rpc_contract()`, which calls `argus_response_rpc_contract()`,
  ensures `lib/grpc` and declares `argus::contracts::llm-wire` with the package
  folder as `PROTO_ROOT`, so the `.proto` sits at the package root rather than
  under the group's shared `proto/` root.
- `tests/unit/llm-contract-catalog-test.cc` (115 lines) — the ten as a pinned
  table, each entry's wire legality, and that no two say the same thing.
- `AGENTS.md`.

One judgement the catalog needed, recorded in the document: `BodyNotJsonObject`
is the HTTP leg's alone (the gRPC leg carries typed fields, so there is no JSON
body to refuse), while the streaming leg has **no** "no tokens" refusal — an
empty generation is a legitimate answer on both legs, so a stream that produced
no text still ends with its `done` token instead of an error.

## The client: the gRPC client and the façade

`packages/clients/llm` now carries both transports, split the way §2.3 spells
them, and the `details/` level is gone:

- `src/llm/llm-client.{hxx,cc}` (46 + 187 lines) is the gRPC client,
  `argus::llm::Client` — `ClientConfig` (`target`, `credential`, `timeout`),
  `kMaxTimeout` (two minutes), `Capabilities` and `argus::llm::Client` with
  `capabilities()`, `chat()` and `chatStream()`. It is the `-client` name §2.3
  reserves, and this package never had one before. It links
  `argus::contracts::llm-wire` **PRIVATE**, so the generated stub stays out of
  its public headers and the server reaches the wire by its own name.
- `src/llm/llm-remote.{hxx,cc}` (60 + 512 lines) is the HTTP transport and the
  façade, the spelling `stt`, `tts` and `vlm` use: `LlmRemoteConfig`,
  `LlmHttpClient` and the façade `LlmClient` (`chat`, `chatStream`, `remote()`),
  which picks the leg per call and keeps the gRPC client in a
  `std::atomic<std::shared_ptr<RpcCache>>` whose entry carries **both** the
  target and the credential it was built for (`compare_exchange_weak`) — a
  runtime change of either rebuilds before the next call, so a rotated
  credential cannot silently reuse the stale client.

The constructor is the client's gate, exactly as stt's and vlm's are: an empty
target or credential is 400 `InvalidRequest`, a timeout of zero or below or
over two minutes the same status and code carrying `"timeout must be within 1
and 120000 ms"`, two minutes exactly accepted. `chat` and `chatStream` refuse a
request that violates the wire bounds before the channel is used (no messages,
65 of them, an empty role or one over 32 bytes, empty content or over 32 KiB,
`maxTokens` outside `0..4096`, a temperature outside `-1..2`, a grammar over
8 KiB, a negative `userId`), and `capabilities()` refuses a reply outside
`0..4096` / `0..2` / `1..2^22` / `0..2^20` as 502 `InvalidResponse`.

Statuses map as the tree's other clients map them, with the one llm-specific
split the streaming shape forces: `CANCELLED` is 504 `DeadlineExceeded` once
the call's own deadline has passed and 499 `Cancelled` otherwise, and every
other status goes through `argus::response::fromRpcStatus`. **The two legs do
not share an error type** — the gRPC leg throws `ResponseException`, the HTTP
leg keeps its older `std::runtime_error` spelling — which is why every in-tree
caller catches `std::exception`; the client `AGENTS.md` states it as the
invariant it is rather than as an accident.

## The service: `src/app/rpc/` and the controller refactor

`services/llm/src/app/rpc/llm-rpc-server.{hxx,cc}` (32 + 325 lines) is
`argus::llm-rpc`, the module the executable links explicitly: `main.cc`
composes it only when `rpc.address` and at least one non-empty `[rpc.callers]`
pair are set (the pairs are filtered pairwise, so an empty secret cannot
authorize anything), and shuts it down after `app().run()` returns. The server
holds three callbacks — a live capabilities read, a `chat` and a `chatStream` —
and `main.cc` binds all three to the **controller**, not to the engine.

That is this unit's one structural change to the service, and it is what makes
the leg honest: `LlmController::chatSync` / `chatStreamSync` are new public
methods holding the tool-loop decision the HTTP handlers used to hold privately
(`requestTools`, `toolLoopInput`, `LlmChatOutcome`), and the HTTP route now
calls them too — `chat` through `BlockingTask<LlmChatOutcome>` (rule 13c), the
stream job through a `LlmPrefillStats` member it reads on the terminating
callback. So a gRPC caller gets the same answer an HTTP caller gets, tool loop
included: the two legs differ in transport, not in the decision above it. The
stream's closing counters are the engine's last-prefill numbers on both legs —
the HTTP sentinel line reads the same member the gRPC frame carries — and the
review pass below records how far that member's provenance reaches.
`ChatStreamJob` lost its `ToolChatInput`/`history` members with the move.

The face follows the stt and vlm servers field by field — the caller must
present the credential header exactly once (zero or two entries are 401,
compared in constant time over the pairs), an invalid request is 400, a stalled
call is 499 or 504 depending on whether its deadline had passed, the engine's
inference slots (`ThreadBudget::inferenceSlots()`, never a global mutex) are
acquired with `try_acquire` and released by an RAII guard with 429 `Busy` for
the loser, every `ResponseException` goes through `toRpcStatus` and anything
else is sanitized to 500 — with three properties the domain forced:

- **The bounds are enforced twice, on purpose.** The server re-checks every
  bound the client checks, because the wire is a boundary and not every caller
  is the client. A raw stub that sends 65 messages, a blank role, a 33-byte
  role, empty content, 32 KiB + 1 of content, a negative `max_tokens` or a
  temperature of 2.5 is refused 400 with the engine untouched.
- **The deadline ceiling is `argus::llm::kMaxTimeout` plus one second**, the
  measurement 6b took: gRPC carries a deadline as a relative `grpc-timeout`
  header and rounds it, so a flat two-minute ceiling refuses the client's own
  maximum. A caller that declares no deadline at all is served.
- **`ChatStream` is the tts server's producer shape**, not a per-token write:
  a `std::jthread` runs the engine and pushes into a 64-token bounded queue,
  and both sides wait with a 10 ms bound on their condition variable so a
  cancelled or expired call is noticed while the queue is full and while it is
  empty, instead of parking on a wakeup that a dead peer will never send. The
  consumer writes each token as its ordinal in `sequence` plus `text`, and the
  final frame is the `done` token carrying the token count and the three
  counters.
- **`temperature` and `tools` are proto3 `optional`**, so a raw caller that
  declares neither gets the engine's default temperature and the tool loop —
  the same answer an HTTP caller that omits both keys gets. Without the
  presence bits an undeclared `temperature` would read as `0.0` (greedy
  sampling, where the HTTP leg's `-1` sentinel means the engine's own value)
  and an undeclared `tools` as false; the review pass below records the
  finding that forced them.

## The suites

- `services/llm/tests/unit/llm-rpc-test.cc` (858 lines, sixteen cases) drives
  both sides. Through `argus::llm::Client`: the capabilities round trip and a
  reply outside the contract refused 502 `InvalidResponse`, a request carried
  to the engine with its completion returned, a generation that produces no
  text as a legitimate answer, the stream's token order — ordinals `0..3` and
  a closing frame counting them — and its closing counters, wrong credentials
  refused on both RPCs, a typed refusal
  round-tripping from the server while a `std::runtime_error` from the engine
  is sanitized to 500 with the message absent, invalid requests refused before
  the engine runs (`calls == 0`), the `fromRpcStatus` table pinned by direct
  construction (CANCELLED 499, DEADLINE_EXCEEDED 504, UNKNOWN and a malformed
  detail 502) rather than by a live round trip (a real transport failure
  carries whichever status the transport chose), and the real engine answering
  through the gRPC leg (LFM2.5 loaded from
  `models/llm/`). Through a raw `wire::Chat::Stub` — the client minus its gate
  — the bounds, a request presenting the credential header twice, a 300 s
  deadline refused while 120 s is served, the absence semantics of
  `temperature`/`tools` (the engine echo spells back `temp=-1` and `tools=1`
  when neither is declared, `temp=0` and `tools=0` when both are), and three
  latch-driven deterministic cases: a deadline that expires while the engine is
  held, an external `TryCancel`, and a second call refused 429 while the only
  slot is held. Each latch is released by an RAII guard, so a failing assertion
  cannot leave the engine held and hang `shutdown()`.
- `services/llm/tests/unit/llm-wire-test.cc` grew a second case (543 lines):
  the HTTP DTO's handoff — every field the body carried reaches `ChatRequest`,
  including `tools:false`, and an omitted body takes the leg's defaults
  (`maxTokens` 0 → the engine's cap, `temperature` −1 → the engine's own,
  `tools` true, `userId` 0, empty grammar).
- `packages/clients/llm/tests/unit/llm-client-test.cc` grew the gRPC client's
  gate table and the façade's leg-choice case: a target nothing listens on
  answers 503 on both legs **while the fake HTTP server's request count does
  not move** (no fallback when the knob is set), a target set with an emptied
  credential is 400, and clearing both knobs returns to the HTTP leg.
- `packages/contracts/llm/tests/unit/llm-contract-catalog-test.cc` (115 lines,
  three cases) pins the catalog.

## Callers repointed

Four call sites, none of them behavioural except where the leg choice is the
point:

- `services/guard` — `main.cc` builds the façade instead of the HTTP transport
  and does so when **either** `guard.assess.llm_url` or `llm.grpc_target` is
  set, so the knob alone can move guard's assessment onto the gRPC leg;
  `guard-assessment.{hxx,cc}` and the live suite take the type change. Guard is
  also where the DTO's dropped field was costing something real: it sets
  `toolsEnabled = false` on every assessment to skip the memory-tool preamble,
  and `ChatCompletionDto::request()` parsed `tools` and never carried it, so
  the leg guard actually uses always ran the loop anyway. The DTO now carries
  the flag (the review pass below), so the request guard writes is the request
  the engine answers.
- `services/voice` — the seam's cached client is the façade, and its
  "not configured" refusal moved from `LlmRemoteConfig::enabled()` to
  `client->remote()`, so a gRPC-only voice configuration is accepted while
  neither-knob-set still refuses with the same message.
- `packages/memory` — `wire-memory-chat.hxx` holds the façade.

## Deletions

- `packages/contracts/proto/argus/ai/v1/llm.proto` — the orphan skeleton; the
  `ai` domain is now gone (the report of 6a had assigned `ai/v1/tts.proto` to
  this step too, and 6b removed it, nothing owning a schema already replaced).
- `services/llm/src/feature/llm/controllers/llm-errors.hxx` — superseded by the
  contract catalog, every entry carried.
- `packages/clients/llm/src/llm/details/llm-remote.hxx` — the `details/` level
  the layout rules retire; the file itself survives one level up.

## Dormant by default, as the precedent is

Verified rather than assumed: `services/llm/config.toml.example` declares
`[rpc] address = ""` and three empty `[rpc.callers]` pairs; `argus-deploy`'s
`config.llm.toml` and its tracked template declare no `[rpc]` block and neither
`llm.grpc_target` nor `llm.grpc_credential` — their two `rpc`-spelling lines are
other clients' knobs, `rpc_secret` under `[identity]` and `grpc_target` under
`[camera]`; and no toml in the tree declares either llm knob, so the façade
takes the HTTP leg and the process composes no gRPC server. The leg is live,
reachable and off — the cutover is a later decision, and the only code that sets
either knob is the two suites.

## The review pass

Two adversarial read-only reviewers ran after the implementation — one over the
code, one over the documents — and every finding was re-measured against the
tree before it was acted on. Six code findings and ten document findings; each
one below says what the measurement showed.

**Fixed — code**

1. **`ChatCompletionDto::request()` dropped `tools`.** The DTO parsed the key
   (`chat-dto.cc:31-32`) and its `request()` never assigned it, so every HTTP
   caller ran the tool loop: the flag was a no-op on the one leg guard uses.
   Measured, then fixed in the same change that documents the invariant.
2. **`LlmService::lastPrefillStats()` read `lastStats_` without a lock** while
   `prefill()` wrote it under `mutex_` — a data race on a member the gRPC
   face's closing frame and the HTTP sentinel both read. The member is now
   `std::atomic<LlmPrefillStats>` (a trivially copyable aggregate; one `store`
   in `prefill()`, one `load` in the getter), matching the `busy_` atomic
   beside it. A locking getter was not an option: the direct path reads the
   member inside the engine's token callback, while the engine holds `mutex_`.
3. **A raw caller that declared no `temperature` got `0.0`** — greedy sampling
   — where the HTTP leg's omitted key means the engine's own value (`-1`), and
   an undeclared `tools` meant false where the HTTP leg means the loop. Both
   fields are now proto3 `optional` and the server resolves absence to the
   HTTP defaults; the client was already declaring both, so its behaviour is
   unchanged and the suite pins the resolution through the engine's own echo.
4. **The façade forwarded its configured timeout into the gRPC client's gate**,
   which refuses anything over two minutes with a 400 — so a
   `guard.assess.timeout_ms` a user raised past 120 s would have made every
   typed-leg call fail where the HTTP leg had accepted it. The façade now
   clamps to `argus::llm::kMaxTimeout` (a non-positive budget takes the
   ceiling).
5. **The latch-driven cases could hang the suite instead of failing**: the
   release ran after the assertions, so an aborting assertion left the engine
   held and `LlmRpcServer::shutdown()` — called by the destructor — waited on
   it forever. Each release is now an RAII guard, so unwinding releases first.
6. **The stream's `sequence` field was never asserted** — both raw-stream call
   sites compared text only, so the ordinals could have been anything. The
   helper now collects them and the case pins `0..3` plus the closing frame's
   token count.

**Fixed — documents**

The document review measured four claims of mine that were false and six that
were imprecise, all corrected in place: the report's "counters the generation
actually reported" (they are the engine's last-prefill member, as the text now
says), the report's "`fromRpcStatus` table read off the wire" (it is pinned by
direct construction), the report's "the last sequence" (the closing frame
carries the count), `services/llm/CONTEXT.md`'s repeated `tools:false`
invariant (it holds now — and did not when the review ran),
`packages/clients/llm/AGENTS.md`'s "seven link lines in six CMakeLists" (seven
in seven, `services/llm` contributing three) and its "exactly two files declare
the HTTP pair" (four do: the two generated configs and their two tracked
templates) and its "the camera and tts configs" (the other pairs are in the
voice and camera configs, under `[stt]` and `[tts]`), its `ChatRequest` field
list without `userId`, its "the only CMakeLists that does" (outside this
package, `services/voice` is not alone in adding a `tests/support` path), the
contract doc's "the tree's second streaming AI RPC" (the voice session's
bidirectional `Connect` and the camera's `Subscribe` also stream — it is the
second among the five boundary contracts) and the same doc's and
`services/llm/CONTEXT.md`'s "argus-memory" as a live process (it is
`packages/memory`, compiled into argus-llm since f8-b3), plus
`packages/contracts/AGENTS.md`'s list of domains still served from the shared
proto root, which omitted `auth`. The report itself is the seventh correction:
the sentence above it now says where the counters come from.

**Recorded, not fixed**

- **The counters' provenance.** The value is the engine's *most recent* prefill,
  not necessarily the caller's own, whenever more than one generation can be in
  flight: the tool loop's non-streamed terminal emit happens after the engine
  releases `mutex_`. Both legs read the same member at the same point (the
  sentinel line and the gRPC closing frame), so this is a pre-existing
  property of `LlmService`, relocated but not created here; every suite runs
  `slots = 1`, while `main.cc` passes `ThreadBudget::inferenceSlots()`, so it
  is reachable in production and invisible in test. Carrying the stats in the
  generation's own outcome would change `LlmService`'s callback contract, which
  four consumers share — out of this unit's scope, recorded here and in
  `services/llm/CONTEXT.md`.
- **The `CANCELLED` split is only half reachable in test.** `499` arrives from
  an external `TryCancel` and is pinned; the `504` branch needs a server that
  answers `CANCELLED` after the caller's deadline has passed, which no real
  transport does — gRPC reports `DEADLINE_EXCEEDED` there. The unit pins the
  `504` through `fromRpcStatus` and the deadline-expiry case instead.
- **The bounds are enforced twice**, `BodyNotJsonObject` has no gRPC reader,
  the streaming leg has no "no tokens" refusal, `Unavailable` has no in-tree
  reader and the two legs keep their own error types. Six observations of that
  shape survived re-measurement as deliberate design; each is already stated
  in `services/llm/AGENTS.md` or the contract document.

## Verified

- `./scripts/build-all.sh dev --only llm` — exit 0, 0 warnings, 44/44 tests
  passed.
- The three consumers repointed onto the façade rebuilt the same way:
  `--only guard` 57/57, `--only voice` 32/32, `--only memory` 26/26, 0 warnings
  each.
- `./scripts/check-comments.sh` — 1379 files, 0 comments (6b recorded 1371 over
  a smaller tree, so the two counts are not comparable as a delta).
- `./scripts/check-deps.sh` — 121 declarations, 852 edges, 0 forbidden, 0
  cycles, 0 unresolved, 22 deferred (6b: 118 declarations, 840 edges).
- `./scripts/check-tidy.sh` over the frozen tree, re-run after the review pass's
  six fixes — exit 0: 548 translation units, 2877 findings over 45 checks
  against a baseline of 2901, no check above it and eight below; the worst file
  is guard's `guard-repository.cc` at 104, which this unit did not touch, and
  the review's fixes moved no count at all. The unit's own tree had risen four
  checks while it was being written —
  `modernize-use-designated-initializers` 236/223,
  `bugprone-implicit-widening-of-multiplication-result` 76/72,
  `modernize-return-braced-init-list` 52/49 and
  `modernize-use-integer-sign-comparison` 19/18 — all of them in the files this
  unit wrote, all closed before the final run.
