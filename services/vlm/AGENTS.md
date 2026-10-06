# argus-vlm — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to vision-service code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **Vision capacity only** — this service runs the LFM2.5-VL engine
   (llama.cpp + libmtmd) and nothing else (no ncnn/sherpa/onnx/tts/stt/face
   symbols; verified with `nm -C`). The engine is THE capacity of this
   service (F4-4, Ruling BO).
2. **Internal wire only** — the service serves the legacy adapters over
   loopback plain HTTP (`/vlm/v1/*`) and the internal gRPC leg
   (`argus.vlm.v1`) when `rpc.address` is set; no JWT, no CORS, no public
   routing or announcement. Never expose either publicly.
3. **Frozen envelope** — every JSON response uses the
   `{status, info, errors}` envelope (`ApiResponse`); the describe body is
   JSON `{image_b64, prompt?, camera_id?}` (base64 JPEG).
4. **Parameter structs for 3+ params** — any function with 3+ parameters
   must take a struct (designated initializers, every member listed).
5. **Dependency injection** — services/controllers hold dependencies as
   private members with `_` suffix; the controller owns `VisionService` BY
   VALUE (the adapter shape — no singleton).
6. **Smart pointers** — no raw owning pointers; the engine handles ride
   `std::unique_ptr` with custom deleters (vision-service.cc).
7. **File naming** — `.hxx` headers, `.cc` sources, hyphenated
   `*-test.cc` tests. No `.h`/`.cpp`.
8. **100% English** — code, identifiers, docs, commits.
9. **No comments** — none in code, of any kind (root rule 20); the "why"
   goes to CONTEXT.md.
10. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`,
    `LOG_FATAL`); no spdlog.
11. **No std::future** — plain `std::thread` + join when parallelism is
    needed.
12. **No database at all** — this service owns no database, no schema
    runner, no migrations; the caption cache is in-process warm state only.
13. **Models are never copied** — the LFM2.5-VL artifacts are read from the
    shared `models/vision/lfm2vl-25/` tree through `[vision]` config keys.
14. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's
    own code; third-party includes are SYSTEM.

## Layout

```
argus-vlm/
  CMakeLists.txt        standalone buildable: module graph + test targets
  src/app/main.cc       config load, llama_backend_init/free, engine boot load,
                          rpc leg, app run
  src/app/rpc/          argus::vlm-rpc — the internal gRPC face (the
                          argus.vlm.v1 Vision service), dormant unless
                          [rpc] address and [rpc.callers] are set
  src/config/           argus::vlm-config — the listener and the optional
                          gRPC leg's address and callers
  src/feature/vlm/      argus::vlm — the whole vertical slice:
                          controllers/ (the frozen /vlm/v1/* wire),
                          dtos/ (the describe DTO, validation DSL),
                          services/ (the LFM2.5-VL engine facade and the
                                     caption cache)
  src/feature/settings/ argus::vlm-settings — the owner-editable catalog
  src/feature/components/ argus::vlm-components — the `vision` component:
                          install, cancel and remove through the settings
                          wire, the lib/http download, the engine (re)load
  config.toml.example   [vision] engine keys + [server] + [rpc] only; no other domains
  CONTEXT.md            purpose, ownership, wiring decisions
```

There are three features and four modules: `argus::vlm-components` installs
the `vision` component (CONTEXT.md, "The vision component"), `argus::vlm-settings` is the
owner catalog (lib/config only, served by `argus.settings.v1` on the gRPC
listener), `argus::vlm` compiles the engine facade,
the DTOs and the HTTP surface together, `argus::vlm-rpc` compiles the gRPC
server and links `argus::clients::vlm` and `argus::contracts::vlm-wire` PUBLIC
so the executable reaches both, and `app/main.cc` registers the controller
explicitly (a Drogon `HttpController<VlmController, false>`), so no route
depends on static-init registration. The folder IS the module (root rule
25) — a consumer links `argus::vlm` and never lists `.cc` files. `src/shared/`
does not exist: rule 23's 2+ rule earns it, so code moves there only when a
second feature of this service reads it.

The gRPC leg is composed in `main.cc` and nowhere else, only when `rpc.address`
and at least one non-empty `[rpc.callers]` pair are set — the RPC server answers
`argus.vlm.v1` with the same engine the HTTP controller drives, refuses an
unlisted caller with 401 and sanitizes anything that is not a
`ResponseException` into 500. Both keys are empty in `config.toml.example`, no
deploy config sets them, and no toml in the tree sets `vlm.grpc_target`, so a
default install answers the HTTP wire alone while the gRPC face stays reachable
for the cutover.

Three properties of the face are the composition's, not the engine's. The
caller must present the credential header exactly once — zero or two entries
are 401, compared in constant time over the pairs. The capabilities are a live
callback over the engine rather than a boot snapshot, so a shut-down engine
reports `loaded: false` truthfully, and the engine's one inference slot set
(`ThreadBudget::inferenceSlots()`) is what a concurrent call is refused against
— 429 `Busy`, acquired with `try_acquire` and released by an RAII guard. On the
request side, `Describe` refuses an empty or over-32-MiB JPEG, a prompt over 512
bytes, a camera id over 64 and a `max_tokens` outside 0..4096 with 400, refuses
an image that decodes to nothing with the same status, and refuses a
caller-declared deadline beyond `argus::vlm::kMaxTimeout` plus one second with
the same 400 — the extra second exists because gRPC encodes a deadline as a
relative `grpc-timeout` header and rounds it, so a client asking for the
client's own ceiling of two minutes was being refused at the boundary; a caller
that sends no deadline at all is served, because the ceiling is a bound on what
the caller asks for and not a requirement that it ask.

The check is `argus::client::FleetCallerGate` (`packages/lib/grpc`), the same
gate identity, sync control and auth use, built from the `[rpc.callers]` pairs
with no legacy secret: a `CHANGE_ME` placeholder is never a credential, and a
listener whose every pair is a placeholder refuses to start
(`std::invalid_argument`) instead of answering an open gate.

## Build commands

```bash
# From the monorepo root
./scripts/build-all.sh dev --only vlm
```

Driving CMake by hand inside the folder means installing the root graph once
(`./scripts/build-all.sh dev --install-only`) and passing its toolchain; the
exact flag set is in `docs/operations/build-and-test.md` under "Working
inside one project".

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, no comments in code, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
