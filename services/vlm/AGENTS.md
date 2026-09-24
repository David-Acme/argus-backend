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
   loopback plain HTTP (`/vlm/v1/*`); no JWT, no CORS, no public routing or
   announcement. Never expose it publicly.
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
  src/app/main.cc       config load, llama_backend_init/free, engine boot gate
  src/feature/vlm/      argus::vlm — the whole vertical slice:
                          controllers/ (the frozen /vlm/v1/* wire),
                          dtos/ (the describe DTO, validation DSL),
                          services/ (the LFM2.5-VL engine facade, its
                                     remote adapter and the caption cache)
  config.toml.example   [vision] engine keys + [server] only; no other domains
  CONTEXT.md            purpose, ownership, wiring decisions
```

There is one feature and one module: `argus::vlm` compiles the engine facade,
the DTOs and the HTTP surface together, and `app/main.cc` registers the
controller explicitly (a Drogon `HttpController<VlmController, false>`), so no
route depends on static-init registration. The folder IS the module (root rule
25) — a consumer links `argus::vlm` and never lists `.cc` files. `src/shared/`
does not exist: rule 23's 2+ rule earns it, so code moves there only when a
second feature of this service reads it.

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
