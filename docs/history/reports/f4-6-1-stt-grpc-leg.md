# Phase 4 step 6, part 6a — the argus-stt internal gRPC leg

Step 6 finishes the internal gRPC legs for `stt`, `vlm` and `llm` following the
`argus.tts.v1` precedent (D16), leaving their app-facing HTTP routes untouched.
It is three services, so it is three commits — 6a `stt`, 6b `vlm`, 6c `llm` —
in the order step 5 laid them down, and `stt` goes first because it is the
smallest: one engine, one controller, two routes, and a client whose HTTP leg
already speaks the whole domain.

The precedent is exact and was followed field by field rather than
re-invented: a contract package that owns the schema and the refusal catalog, a
client that carries both transports with the gRPC one behind a runtime knob, a
service that composes the server in `src/app/rpc/` and only when its `[rpc]`
keys are set, and a leg that ships dormant.

## What existed before

- `packages/clients/stt` was HTTP only: `stt-remote.{hxx,cc}` with
  `SttRemoteConfig` (`stt.remote_url`, `stt.remote_timeout_ms`),
  `SttHttpClient` posting `audio/x-argus-pcm-s16` to
  `/stt/v1/transcribe?lang=`, and a raw-socket transport under an anonymous
  namespace. No `details/`, no `-client` file — a deviation its `AGENTS.md`
  recorded and this step closes for the name while leaving the folder as it is.
- `services/stt` had one module (`argus::stt`, the engine facade plus the HTTP
  controller) and one `app/main.cc`; its three refusals lived in
  `src/feature/stt/controllers/stt-errors.hxx`, a controller-local catalog the
  gRPC server would have had to duplicate.
- `SttService`'s language semantics lived inside `transcribeAsync`: the public
  surface took a `TranscribeInput` of reference members, resolved the empty
  language to `stt.language`, rebuilt the recognizer on a language change, and
  the raw body `decode` held the mutex. A second caller could only have
  duplicated that preamble.
- `packages/contracts/proto/argus/ai/v1/stt.proto` was dead — referenced by
  nothing in the tree (the `ai` folder's three protos are rule 23's leftover
  skeletons; `llm`, `tts` and `vlm` still carry theirs).
- Nothing in the tree produced or consumed a proto for speech-to-text, and the
  plan's §9.3 said so: "there is no proto for `llm`, `stt` or `vlm` — those
  clients are HTTP".

## The contract: `packages/contracts/stt`

The third package under `packages/contracts/` that is not header-only, after
`tts` and `response`, and built the way `tts` is:

- `stt.proto` (28 lines) — `package argus.stt.v1`, one `Transcription` service,
  **both RPCs unary**: `Capabilities(CapabilitiesRequest)` answering
  `{sample_rate, loaded, language, default_language, languages}` and
  `Transcribe(TranscribeRequest)` taking `{repeated float samples, sample_rate,
  language}` and answering `{text}`. No streaming: a turn is one buffer at
  16 kHz, not a chunk stream, which is the one structural difference from the
  synthesis precedent that the domain forces.
- `src/stt/stt-errors.hxx` (52 lines) — eleven refusals. The three that lived
  in the controller (`SpeechEngineNotLoaded` 503 `STT_NOT_LOADED`,
  `BodyNotPcmS16` 400, `PcmBodyMisaligned` 400) were carried byte for byte,
  code, status and message, and eight were added for the leg the HTTP surface
  never needed: `InvalidRequest` 400, `Unauthorized` 401, `Cancelled` 499,
  `DeadlineExceeded` 504, `Busy` 429, `InternalError` 500, `InvalidResponse`
  502 and `Unavailable` 503.
- `CMakeLists.txt` — the `argus_contracts(NAME stt ...)` vocabulary plus
  `argus_stt_rpc_contract()`, the CMake function this package owns, which calls
  `argus_response_rpc_contract()`, ensures `lib/grpc`, and declares
  `argus::contracts::stt-wire` from the proto beside it. A consumer calls the
  function; it never links the wire target blind.
- `tests/unit/stt-contract-catalog-test.cc` (87 lines) — the eleven entries as a
  pinned table (name, code, status, message), each entry's wire legality, and
  that no two say the same thing. `CHECK(kCatalog.size() == 11)` is the count
  that has to move with the catalog.

The controller's catalog file is deleted; seven files now include the contract's
(`stt-client.cc`, both stt suites, the rpc server, the controller, the service
and the catalog test).

## The client: the gRPC client and the façade

`packages/clients/stt` gains `stt-client.{hxx,cc}` (51 + 96 lines) — the
`-client` name §2.3 reserves for the method surface, which the package did not
have before — and `stt-remote.{hxx,cc}` keeps the HTTP transport and gains the
façade.

- `argus::stt::Client` is `ClientConfig{target, credential, timeout}` (the same
  30 s default as tts), `Capabilities`, `TranscribeInput{samples, sampleRate,
  language, cancellation}` and two const methods, `capabilities()` and
  `transcribe(input)`. The constructor is the gate and throws before dialling —
  an empty target, an empty credential, a timeout of zero or below, or a
  timeout over two minutes is `ResponseException(400, SttErrors::InvalidRequest)`
  — and `transcribe` refuses empty samples or a rate outside 8000..192000
  locally, plus an already-requested stop with 499. `capabilities()` validates
  the reply (a rate inside 8000..192000, a non-empty `language`, a non-empty
  `default_language`, a non-empty language list) and refuses a malformed one
  with 502 `InvalidResponse`, which is the eleventh catalog entry and the only
  one with no other reader.
- `SttClient`, in `stt-remote.hxx` beside `SttHttpClient`, is what consumers
  hold: `transcribe(samples, lang)` and `remote()`. It reads `stt.grpc_target`
  and `stt.grpc_credential` through `ConfigService`, keeps the gRPC client in a
  `std::atomic<std::shared_ptr<RpcCache>>` where the cache entry carries the
  target it was built for, and adopts it only while that target still matches
  (a `compare_exchange_weak` loop, the tts precedent's shape plus the target),
  falls back to the HTTP leg when the knob is unset, and throws the same
  `"stt.remote_url is not configured"` message the consumers already knew when
  neither is set. The deadline is `stt.remote_timeout_ms` on both legs — one
  timeout knob, not two.
- Failures keep the two flavours the package already documented: the gRPC path
  throws `ResponseException` (a contract refusal, or what
  `fromRpcStatus` maps a bare transport failure to), the HTTP path keeps its
  frozen `std::runtime_error` messages.

Both live consumers were switched to the façade so the new leg is reachable
rather than dead code, and each kept its own behaviour: `services/voice`'s
`RemoteVoiceStt` keeps the 16 kHz refusal and the mutex around `lang_` and loses
`clientFor` and its three cache members; `services/camera`'s
`HttpSttTranscriber` keeps its `remote()`-false empty return and its int16 →
float conversion.

## The service: `src/app/rpc/` and the seam

`services/stt/src/app/rpc/stt-rpc-server.{hxx,cc}` (31 + 159 lines) is
`argus::stt-rpc`, and its `CMakeLists.txt` is four lines: the module, a PUBLIC
link to `argus::clients::stt`, `argus::contracts::stt-wire` and `argus::stt`,
and the `-Wall -Wextra` line every module carries.

- `SttRpcInput` is the composition root's struct: the address, the caller
  credentials, a `std::function<argus::stt::Capabilities()>`, a
  `std::function<bool(const std::string&)>` for the language gate and a
  `std::function<std::string(const TranscribeRequest&)>`, plus the slot count.
  The capabilities are a callback, not a boot snapshot as in the tts precedent,
  so `loaded` and `language` are read live and a shut-down engine reports
  `loaded: false` truthfully. The language gate is a predicate over the wire
  string rather than a read of the capabilities list, so validating a request
  copies nothing and the gate is the service's own
  `SttService::isSupportedLanguage`.
- The server refuses in order: 401 when the caller's credential metadata is
  absent, wrong or presented twice (`lib/grpc`'s `kCallerCredentialKey`, exactly
  one entry required, compared in constant time over the credential pairs), 400
  when the request is invalid (empty samples, a rate outside 8000..192000, a
  language the predicate does not accept), 400 for a deadline more than two
  minutes out — a caller that declares no deadline at all is served, since the
  ceiling bounds what a caller asks for rather than requiring it to ask — 499/504
  when the call is already stopped, and 429 `SttErrors::Busy` when the inference
  slots are exhausted — one `std::counting_semaphore` acquired with
  `try_acquire` and released by an RAII guard, never a global mutex.
- Every `ResponseException` becomes its wire status through `toRpcStatus`;
  anything else becomes 500 `InternalError` with the message dropped, so no
  engine detail and no secret reaches the caller.
- `main.cc` composes the leg in one block: `rpc.address` and the non-empty
  `[rpc.callers]` pairs gate it, the capabilities callback reads
  `SttService::instance()` live, and the transcribe callback is the same
  `SttService::transcribe` the HTTP controller calls. `shutdown()` runs before
  the engine's.

The seam that makes one engine answer two wires is in `SttService`: the old
reference-member `TranscribeInput` became an owned `TranscribeRequest{samples,
sampleRate, lang}`, the public `transcribe(const TranscribeRequest&)` now owns
the language semantics (an empty `lang` resolves to `stt.language`; a different
language switches the recognizer, logging a warning and continuing if the
switch fails), and the old body became the private `decode()`, which still holds
the mutex and now **throws** `SpeechEngineNotLoaded` for a null recognizer
instead of logging and returning an empty string — so both legs refuse a dead
engine with the same 503, and the wire test's existing `down.status == 503`
assertion now describes the service rather than the controller.

`config.toml.example` gains `[rpc] address = ""` and `[rpc.callers] voice = ""`
/ `camera = ""` between `[server]` and `[stt]`, the position the tts template
uses.

## The suites

`services/stt/tests/unit/stt-rpc-test.cc` is new and drives the leg from both
sides: the façade, `argus::stt::Client`, and a raw `wire::Transcription::Stub`
built on `argus::client::makeChannel`, which is the client minus its local gate
— the only way to ask what the server does with a request its own client would
never send. Thirteen cases:

- The legacy delegation: `SttClient` over `stt.grpc_target` reaches the gRPC
  server and answers the fake engine's transcript; clearing the knob turns
  `remote()` false again.
- Capabilities metadata: rate, loaded, current and default language, the
  language list.
- A capabilities reply outside the wire contract — a zero sample rate, and
  separately an empty language list — refused as 502 `BAD_GATEWAY` by the
  client.
- A wrong credential refused on both RPCs, typed as 401 `UNAUTHORIZED`.
- The typed-error roundtrip: 503 `STT_NOT_LOADED` with the catalog's message,
  and a 422 whose `std::vector<ResponseError>` survives the wire intact.
- A failure outside the response contract sanitized to 500 `INTERNAL_ERROR`
  with `"secret"` absent from the message.
- The two input gates together: empty samples, rates 0/7999/192001 refused by
  the client before it dials, an unknown language refused by the server, with
  `calls.load() == 0` proving the engine never ran.
- The raw wire refusing what the client's gate would have caught: empty
  samples, 7999, 192001, `"fr"`, a 300 s deadline and a missing credential
  (400 five times, 401 once), while 8000 and 192000 with an empty language do
  reach the engine — two edges the client band and the server band share — and
  a call with no deadline at all is served, the ceiling being a bound on what a
  caller asks for.
- A call that presents two credentials refused 401: the exactly-one rule needs
  raw metadata, since the client's own helper always sends one entry.
- The transport mapping split out as its own table: `fromRpcStatus` taking
  CANCELLED to 499 and DEADLINE_EXCEEDED to 504, and a garbage or malformed
  detail to 502.
- Deadline and cancellation against an engine held on a latch, each checking
  504/499 and then that the engine did finish — the inherent cost of the unary
  shape, recorded rather than hidden.
- A second call refused 429 `TOO_MANY_REQUESTS` while the single slot is held,
  deterministic through the same latch: the holder's answer arrives intact once
  the slot is released.
- The real engine behind the legacy client, transcript compared with the
  in-process call.

`tests/support/wav-fixture.hxx` is new: the WAV reader both stt suites now
share (the two had a copy each), and `services/stt/CMakeLists.txt` puts
`tests/support` on `stt-rpc-test`'s include path the way `stt-wire-test`'s
already was.

`packages/clients/stt/tests/unit/stt-client-test.cc` grew the band edges beside
its empty-samples refusal: 0, 7999 and 192001 are now pinned as 400
`"BAD_REQUEST"` from the caller's side, so the `8000..192000` the package's
`AGENTS.md` states is a measured claim rather than a restatement of the
constant. The same sentence in `AGENTS.md` was stale — it said `1..192000`,
the band before this step's server gate — and was corrected with it. Its
constructor-gate table also pins the two out-of-band timeouts by their own
message, `"timeout must be within 1 and 120000 ms"`.

The real-engine cases in both suites need `models/stt/zipformer-en/`, which
`services/stt/scripts/provision.sh` does not fetch — it provisions only the
nemo FastConformer transducer the service boots with. The `REQUIRE_MESSAGE`
both suites carry now says so, so a missing model reports the provisioning gap
instead of a bare load failure.

## Deletions

- `services/stt/src/feature/stt/controllers/stt-errors.hxx` — superseded by the
  contract catalog, all three entries carried.
- `packages/contracts/proto/argus/ai/v1/stt.proto` — dead; the live schema is
  `packages/contracts/stt/stt.proto`. The other three `ai` protos (`llm`,
  `tts`, `vlm`) are all still on disk and stay until 6b and 6c replace the two
  they own; the tts step built its own contract package without deleting
  `ai/v1/tts.proto`, so that one is 6c's to remove.

## Dormant by default, as the precedent is

No template and no deploy config sets `stt.grpc_target`, and
`config.toml.example` declares `[rpc]` empty, so a default install answers the
HTTP wire alone and composes no gRPC server. Nothing in the tree enables the
leg; the only code that sets either key is the two new suites. That is the
property the tts precedent carries and it is carried here deliberately, not by
omission: the cutover is a later decision, and both legs are live behind it.

## Verified

- `./scripts/build-all.sh dev --only stt` — **0 errors, 0 warnings** (the run's
  `warning:` count is zero), ctest **15/15** green in 28.67 s
  (`stt-wire-test` 18.60 s driving the real engine, `stt-rpc-test` 7.11 s
  including its real-engine case, `stt-client-test`, `stt-contract-catalog-test`
  and the packages' suites the project reaches).
- `check-comments`: 1368 files, 0 comments (1359 at step 5c — the new files are
  comment-free).
- `check-deps`: 115 declarations, 827 edges, 0 forbidden, 0 cycles,
  0 unresolved, 22 deferred (292 third-party mentions over 20 roots).
- Whole-tree `check-tidy`: **543 TUs, 2901 findings over 45 checks, baseline
  2901** — no check above the baseline and none below it, so the ratchet holds
  and `scripts/lib/tidy-baseline.txt` is untouched by this step. The scan ran
  on the frozen tree; an earlier scan taken while the new files were still being
  written saw six checks risen (2919 findings) and that measurement is
  superseded by this one. The five `-Wunused-result` warnings the new
  `[[nodiscard]]` pair forced were fixed before the scan, not suppressed.
- The two consumers were built and tested too (`--only voice`, `--only camera`).

## Flagged, not fixed

- `SttErrors::Unavailable` (503 `ServiceUnavailable`) has no in-tree reader,
  exactly as the tts catalog's `Unavailable` has none: it is the catalog's
  spelling of the status `argus::response` maps a bare transport failure to, and
  the catalog is a vocabulary, not a call graph.
- The client keeps no `details/` directory: the URL parsing, the socket
  plumbing, the PCM scaling and the envelope parsing stay in `stt-remote.cc`'s
  anonymous namespace, and the gRPC channel and credential helpers are
  `lib/grpc`'s `argus::client`. The `-client` name the layout asks for now
  exists; the folder shape is left as the step found it.
- The 499/504 branch that fires when a call *arrives* already stopped or past
  its deadline (`stt-rpc-server.cc:115`) has no test: reaching it means
  cancelling between the client's send and the server's dispatch, which the
  unary shape gives no seam to force. The refusals the suites do pin — the
  client-side cancellation and the client-side deadline — are the two the
  vocabulary has to be right about, and they are green.
- A dead-engine `capabilities()` is served happily: the callback's `loaded`
  field is read, never required to be true, so a caller learns the engine is
  down from the flag rather than from a refusal. That is deliberate — the flag
  exists to be read — and it is the same shape the tts precedent ships.
- Two pre-existing properties of the pieces this step reused, recorded rather
  than touched: `SttService::isLoaded()` hands out a plain `bool` with no
  synchronisation (the tts precedent's `isLoaded()` is the same), and
  `argus::client::constantTimeEquals` short-circuits on a length mismatch, so
  it compares unequal-length secrets faster than equal-length ones. Both are
  outside this step's surface and neither is reachable from the wire's
  credential check, which compares an exactly-one-entry header against a
  configured pair.
- The tts precedent carries the same two edges this step fixed here, and they
  are left where the closed step put them: `services/tts/src/app/rpc/tts-rpc-server.cc:111`
  treats a caller that declares no deadline as a request beyond the ceiling
  (the `std::chrono::system_clock::time_point::max()` comparison against
  `now + 120 s`), so a tts caller that sends none is refused 400. The stt leg
  skips that comparison when no deadline was sent; the tts leg is a one-line
  change in a step that is already closed, flagged for whoever reopens it.

## The second pass

The step was reviewed adversarially after it was first written — one reviewer
over the code, one over the documents — and every finding was re-measured here
before anything moved. Eight code findings, five of them acted on:

1. **The real-engine cases depend on a model no script provisions** —
   confirmed as a real gap and measured as pre-existing: `git diff` on
   `stt-wire-test.cc` shows the `models/stt/zipformer-en/test_wavs/*.wav`
   dependency predates this step (only the `REQUIRE_MESSAGE` text and the
   `transcribe` call shape changed), and `services/stt/scripts/provision.sh`
   fetches only the nemo tarball and deletes its `test_wavs`. The exposure is
   two suites, not one, and both now say so in their `REQUIRE_MESSAGE`. The fix
   would mean provisioning files out of a 107 MB tarball nobody has verified
   this step, so the flag is sharpened and the tree is left as it was found.
2. **The server's arrival-race 499/504 branch has no test** — confirmed and
   kept: forcing it needs a cancellation between send and dispatch. Recorded
   above rather than faked with a sleep.
3. **The exactly-one-credential rule was untested** — confirmed and fixed: the
   new case sends the header twice through raw metadata and pins 401.
4. **The façade's client cache ignored a change of target** — confirmed and
   fixed: the cache entry carries its target and the CAS loop rebuilds on a
   change, so `transcribe` and `remote()` read the same knob per call.
5. **A timeout knob above 120 s produced a misleading refusal** — confirmed and
   fixed: the constructor splits the empty-target/empty-credential branch from
   the band branch, and the band branch says
   `"timeout must be within 1 and 120000 ms"` on the same 400 `BAD_REQUEST`.
6. **A client that sends no deadline was refused 400** — confirmed and fixed:
   `Clock::time_point::max() > now + 120 s` is true, so the ceiling refused
   every undedlined call; the check now skips a call that declared none.
7. **A per-request `Capabilities` allocation in the hot path** — confirmed and
   fixed: the gate is the injected `acceptsLanguage` predicate, so `Transcribe`
   copies no capability list per call.
8. **`isLoaded()`'s plain `bool` and `constantTimeEquals`' length
   short-circuit** — confirmed as pre-existing and left: both predate this step
   and neither is reachable from a wire the step introduced.

The documents reviewer's twelve findings were all drift between the prose and
the code the second pass had just changed (line counts, the `SttRpcInput`
member list, the cache's type and operation, the language gate's wording, the
timeout message, the `ai` proto count, `ai/v1/tts.proto` still being on disk,
the number of files declaring `stt.remote_url`, and a root-`AGENTS.md` claim
that `intent` declares itself with a literal `add_library` when it uses
`argus_module`). All twelve are corrected in the documents this report came
with; the root `AGENTS.md` line was the one that was wrong about the tree
rather than about this step, and it is fixed in the same change.
