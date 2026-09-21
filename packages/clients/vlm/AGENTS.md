# argus_clients_vlm

The argus-vlm internal wire seen from the caller's side: the request envelope,
the base64 image encoding and the caption parsing, with nothing of the engine
in it.

## What this is

A CLIENT module, not a service: one `argus_clients(NAME vlm ...)`, a STATIC
library whose include root is `src/`, so a consumer writes
`<vlm/vlm-client.hxx>` and links `argus::clients::vlm`. It depends on
`argus::lib::text` (for `base64::encode`) and Drogon, compiles no `.proto`, and
carries no configuration of its own: the endpoint arrives as a constructor
argument.

argus-guard is its only consumer, through four link lines in two CMakeLists —
`argus-guard` PRIVATE (`services/guard/CMakeLists.txt:144`), the `guard`
feature module that calls it (`src/feature/guard/CMakeLists.txt:23`), and two
live suites (`vlm-client-live-test`, `guard-assessment-live-test`, :225 and
:249). `services/guard/CMakeLists.txt:120` also adds the package by path
(`if(NOT TARGET argus::clients::vlm)`) so guard builds standalone. The
assessment calls it in two places, `guard-assessment.cc:299` and `:363`, to
describe a person crop before an optional LLM classification.

## Layout

- `src/vlm/vlm-client.hxx` — `VlmDescribeInput` (`jpeg`, `prompt`,
  `cameraId`), `VlmDescribeResult` (`caption`) and `VlmClient`
  (`explicit VlmClient(std::string baseUrl, double timeoutS = 8.0)` and a
  coroutine `describe(const VlmDescribeInput&) const` returning
  `drogon::Task<std::optional<VlmDescribeResult>>`); 6 files include it, 4 of
  them outside the package (guard's `main.cc`, `guard-assessment.cc` and the
  two live suites).
- `src/vlm/vlm-client.cc` — the package's only translation unit, and the one
  that owns the wire: the JSON body, the `POST /vlm/v1/describe` path, and the
  reading of the `{status, info}` envelope.
- `CONTEXT.md` — the package's "why" (rule 20). Its transport sentence was
  corrected in this step, and the suite pins both sides of it; see the transport
  note under Rules.
- `tests/unit/vlm-client-test.cc` — the package's first test file; the suite is
  described below.

## Rules

- Rule 25: the folder IS the module. One `argus_clients(NAME vlm ...)` with an
  explicit source list, never `file(GLOB)`.
- The include prefix is `<vlm/vlm-client.hxx>`.
- What a consumer sees: `VlmClient::describe`, a coroutine handing back a
  `std::optional<VlmDescribeResult>`, plus the two DTOs. What it must not see:
  no protobuf type (this package compiles none), no URL constant, no JSON key,
  no HTTP method or path, no retry policy, and no deadline other than the
  header's 8.0 s default.
- The wire it owns: a POST to `/vlm/v1/describe` whose body is `image_b64` —
  standard-alphabet base64 with padding, so `argus` is `YXJndXM=` — with
  `prompt` always present (even when empty) and `camera_id` only when the
  caller set one.
- The envelope it owns: a value comes back only when the status is 200, the
  body parses as JSON, `info` is an object and `caption` is not empty. Every
  other shape is `std::nullopt`, and so is a call with nothing to send: an
  empty `jpeg` or an empty `baseUrl` returns before the wire.
- A transport failure is NOT `std::nullopt`. In drogon 1.9.13,
  `HttpRespAwaiter::await_suspend` (`lib/inc/drogon/HttpClient.h:384`) turns
  any non-Ok `ReqResult` — connection refused as much as a timeout — into a
  `drogon::HttpException` through the coroutine, and `drogon::sync_wait`
  rethrows it (`lib/inc/drogon/utils/coroutine.h:500`). So a caller that only
  checks the optional will see the exception escape; the one consumer does
  exactly that, at `guard-assessment.cc:299` and `:363`. `CONTEXT.md` said
  "nullopt covers transport, status and empty-caption failures" and now names
  the two cases itself, the same two the suite pins.
- No config of its own, by design: argus-guard reads `guard.assess.vlm_url`
  (declared once, `argus-deploy/config.guard.toml:131`,
  `http://172.19.0.31:7031`, inside the `[guard.assess]` block at :128) and
  hands the client that URL with `guard.assess.timeout_ms / 1000.0` — default
  8000 ms, so 8.0 s (`services/guard/src/main.cc:158-164`). An empty URL means
  guard builds no client at all.
- Flagged deviations from §2.3, neither hidden: there is no `details/` folder
  — the channel and the envelope parsing live in the single `vlm-client.cc`;
  and the surface is declared in the global namespace (`VlmClient`,
  `VlmDescribeInput`, `VlmDescribeResult`), where sibling clients put theirs
  in `argus::tts`, `argus::clients::*`, and a consumer's file gets names that
  generic unqualified.

## Tests

- `tests/unit/vlm-client-test.cc` — three cases against a real drogon server
  on a loopback port, standing in for argus-vlm. "The describe request carries
  the JPEG as base64 on the wire": the round trip returns the canned caption
  and the captured request is a POST to `/vlm/v1/describe` whose `image_b64` is
  `YXJndXM=`, with `prompt` present and `camera_id` sent, then a bare ask that
  still sends an empty `prompt` and omits `camera_id`. "The describe envelope
  decides what the caller gets": a pinned table of four refusals — a blank
  caption, a non-object `info`, a body that is not JSON, a 503 envelope — each
  of which must leave the caller without a value. "Describe stops before the
  wire, and raises when the wire is dead": an empty `jpeg` and an empty
  `baseUrl` are `nullopt` with nothing sent, while the same client against a
  dead port raises `drogon::HttpException` instead of answering — the behaviour
  the transport note above records, pinned so it cannot drift unnoticed.
- Nothing else tests this package: it had no `tests/` directory before this
  suite, and the live suites in `services/guard` need argus-vlm running.
