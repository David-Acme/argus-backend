# argus-llm — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to llm-service code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **Brain process** — this service runs LFM2.5 chat, intent routing, the
   tool-calling loop and the memory stack. It must not absorb
   camera, face, VLM, STT, TTS or voice-session responsibilities.
2. **Internal wire only** — the service serves the legacy voice session over
   loopback plain HTTP (`/llm/v1/*`) and the internal gRPC leg (`argus.llm.v1`)
   when `rpc.address` is set; no JWT, no CORS, no public routing or
   announcement. Never expose either publicly. The HTTP face trusts the loopback
   bind, and binds a declared identity to the `voice` caller's credential
   when one is configured; the gRPC face lists its callers in
   `[rpc.callers]`, answers an unlisted credential with 401 and lets only
   `voice` declare a user, a role or the tool loop.
3. **Frozen envelope** — every JSON response uses the
   `{status, info, errors}` envelope (`ApiResponse`); the chat body is JSON
   `{messages, max_tokens?, temperature?, reset_context?}`.
4. **Tool loop stays here** — `LfmAdapter`, `ToolRegistry` and the fast intent
   gate are part of the brain. Memory tools execute in process; unconfident
   classification falls through to the model rather than guessing.
5. **Parameter structs for 3+ params** — any function with 3+ parameters
   must take a struct (designated initializers, every member listed).
6. **Dependency injection** — services/controllers hold dependencies as
   private members with `_` suffix; the controller owns `LlmService` BY
   VALUE (the adapter shape — no singleton).
7. **Smart pointers** — no raw owning pointers; the engine handles ride
   `std::unique_ptr` with custom deleters (llm-service.cc).
8. **File naming** — `.hxx` headers, `.cc` sources, hyphenated
   `*-test.cc` tests. No `.h`/`.cpp`.
9. **100% English** — code, identifiers, docs, commits.
10. **No comments** — none in code, of any kind (root rule 20); the "why"
    goes to CONTEXT.md.
11. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`,
    `LOG_ERROR`, `LOG_FATAL`); no spdlog.
12. **No std::future** — the HTTP stream producer runs on the Heavy
    blocking lane behind `StreamSlots`, as the TTS controller does.
13. **Memory database only** — the memory feature owns `memory.db`.
    Identity/camera catalog snapshots arrive over the SDK clients
    (`argus::clients::identity`, `argus::clients::camera`); this service opens no
    other domain database.
14. **Models are never copied** — LLM, memory and intent artifacts are read
    from the shared `models/` tree.
15. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's
    own code; third-party includes are SYSTEM.

## Layout

```
argus-llm/
  CMakeLists.txt        standalone buildable: module graph + test targets
  src/app/main.cc       config load, llama_backend_init/free, engine boot gate
  src/app/rpc/          argus::llm-rpc — the internal gRPC face (the
                          argus.llm.v1 Chat service), dormant unless [rpc]
                          address and [rpc.callers] are set
  src/config/           argus::llm-config — the listener, the optional gRPC
                          leg, the identity target/secret, the camera target
                          and the memory gate
  src/feature/llm/      argus::llm — the brain:
                          controllers/ (the frozen /llm/v1/* wire),
                          dtos/ (the chat DTO, validation DSL),
                          services/ (the LFM2.5 engine facade, the fast
                                     intent gate, and the tool runtime
                                     under its own tools/)
  src/feature/memory/   argus::memory — the memory stack:
                          services/memory (MemoryService, SqliteGraph,
                            GraphRecall, MemoryFormation, EntityResolver,
                            ToolParser, MemoryChat), services/embedding,
                          services/extract, repositories/memory-graph,
                          vocabulary/, infra/catalog-replica
  src/feature/intent/   argus::intent — the fast tier of the router:
                          services/ (fastText classifier + intent router),
                          models/ (the artifact pin and its NOTICE)
  src/feature/encounter-closed/
                        argus::encounter-closed — the camera guard feed's
                          durable JetStream consumer, writing the memory
                          graph through the injected capture
  src/feature/settings/ argus::llm-settings — the owner-editable catalog
                          served by argus.settings.v1 on the gRPC leg
  src/shared/           argus::llm-shared — vocabulary 2+ features read
                          (vocabulary/tool-contracts.hxx)
  database/schema.sql   memory.db: graph tables, memory_vec partitions,
                          catalog replicas, encounter_closed_inbox
  config.toml.example   listener, LLM, intent, [rpc], memory/extract/nats
  CONTEXT.md            purpose, ownership, wiring decisions
```
There are five features and seven modules: `argus::llm` compiles the engine
facade, the DTOs, the tool runtime and the HTTP surface together,
`argus::memory` the memory stack the tool loop calls in process,
`argus::intent` the fastText router tier `argus::llm`'s gate drives,
`argus::encounter-closed` the consumer `app/main.cc` starts on the beginning
advice and stops before `memory.shutdown()`, `argus::llm-settings` the
owner settings catalog, `argus::llm-rpc` the gRPC
server under `src/app/rpc/`, and `argus::llm-shared` the vocabulary two
features read (`vocabulary/tool-contracts.hxx`). `app/main.cc` registers its
controllers explicitly (Drogon `HttpController<…, false>`), so no route
depends on static-init registration. The folder IS the module (root rule 25) —
a consumer links `argus::llm`, `argus::memory`, `argus::intent`,
`argus::encounter-closed`, `argus::llm-rpc` or `argus::llm-shared`
and never lists `.cc` files. `src/shared/` holds what 2+ features of this
service read and nothing else (root rule 23).

The gRPC leg is composed in `main.cc` and nowhere else, only when `rpc.address`
and at least one non-empty `[rpc.callers]` pair are set — the RPC server answers
`argus.llm.v1` through the same controller the HTTP route drives, tool loop
included, refuses an unlisted caller with 401 and sanitizes anything that is not
a `ResponseException` into 500. Both keys are empty in `config.toml.example`, no
deploy config sets them, and nothing in the tree sets `llm.grpc_target`, so a
default install answers the HTTP wire alone while the gRPC face stays reachable
for the cutover.

Three properties of the face are the composition's, not the engine's. The caller
must present the credential header exactly once — zero or two entries are 401,
compared in constant time over the pairs. `Chat` calls the engine
synchronously on the gRPC server's own thread — never on the Drogon event
loop — and the whole RPC sits behind a `std::counting_semaphore` sized
`ThreadBudget::inferenceSlots()`, so a caller arriving while every slot is
taken gets 429 `Busy` instead of queueing behind a generation. And the request
bounds are enforced twice on purpose — the client refuses a request its own wire
should never carry, and the server re-checks it, because the wire is a boundary
and not every caller is the client: no messages or 65 of them, an empty role or
one over 32 bytes, empty content or over 32 KiB, `max_tokens` outside `0..4096`,
a declared temperature outside `-1..2`, a grammar over 8 KiB and a negative
`user_id` are 400. A caller-declared deadline more than a second past
`kMaxTimeout` is the same 400 — the ceiling is a bound on what the caller asks
for, not a requirement that it ask.

The check is `argus::client::FleetCallerGate` (`packages/lib/grpc`), the same
gate identity, sync control and auth use, built from the `[rpc.callers]` pairs
with no legacy secret: a `CHANGE_ME` placeholder is never a credential, and a
listener whose every pair is a placeholder refuses to start
(`std::invalid_argument`) instead of answering an open gate.

`ChatStream` is where this leg differs most from the HTTP one: the engine's
token callback fires on a `std::jthread` producer into a 64-token bounded queue
that polls its condition variable rather than blocking forever, the consumer
thread writes each token as its ordinal in `sequence` plus `text`, and the
stream ends with a `done` token carrying the token count and the three prefill
counters. That last token is the client's exact end-of-stream marker, the same
role the HTTP sentinel line plays; a stream that ends without one is 502 at the
client. `temperature` and `tools` are proto3 `optional`, so a caller that
declares neither gets the engine's default and the tool loop exactly as an HTTP
caller that omits both keys does. The
two legs do not share an error type: the gRPC leg throws `ResponseException`, so
a caller sees the shared vocabulary, while the HTTP leg keeps its older
`std::runtime_error` spelling, which is why every in-tree caller catches
`std::exception`.

## Build commands

```bash
# From the monorepo root
./scripts/build-all.sh dev --only llm
```

Driving CMake by hand inside the folder means installing the root graph once
(`./scripts/build-all.sh dev --install-only`) and passing its toolchain; the
exact flag set is in `docs/operations/build-and-test.md` under "Working
inside one project".

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, no comments in code, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
