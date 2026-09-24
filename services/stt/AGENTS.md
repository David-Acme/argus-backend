# argus-stt — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to stt-service code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **STT capacity only** — this service runs the sherpa-onnx STT engine and
   nothing else (no face/llm/vlm/tts/vad code or symbols; verified with
   `nm -C`). The engine is THE capacity of this service (F4-3, Ruling BK).
2. **Internal wire only** — the service serves the legacy adapters over
   loopback plain HTTP (`/stt/v1/*`); no JWT, no CORS, no public routing or
   announcement. Never expose it publicly.
3. **Frozen envelope** — every JSON response uses the
   `{status, info, errors}` envelope (`ApiResponse`); the transcribe body is
   binary `audio/x-argus-pcm-s16` (16 kHz mono).
4. **Parameter structs for 3+ params** — any function with 3+ parameters
   must take a struct (designated initializers, every member listed).
5. **Dependency injection** — services/filters hold dependencies as private
   members with `_` suffix; controllers hold instance members, never static
   methods.
6. **Smart pointers** — no raw owning pointers; raw pointers only for
   non-owning access.
7. **File naming** — `.hxx` headers, `.cc` sources, hyphenated
   `*-test.cc` tests. No `.h`/`.cpp`.
8. **100% English** — code, identifiers, docs, commits.
9. **No comments** — none in code, of any kind (root rule 20); the "why"
   goes to CONTEXT.md.
10. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`,
    `LOG_FATAL`); no spdlog.
11. **No std::future** — plain `std::thread` + join when parallelism is
    needed.
12. **No database at all** (Ruling BN) — this service owns no database, no
    schema runner, no migrations.
13. **Models are never copied** — the sherpa-onnx artifacts are read from
    the shared `models/stt` tree through `stt.models_dir`.
14. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's
    own code; third-party includes are SYSTEM.

## Layout

```
argus-stt/
  CMakeLists.txt        standalone buildable: module graph + test targets
  src/app/main.cc       config load, engine boot gate, app run
  src/feature/stt/      argus::stt — the whole vertical slice:
                          controllers/ (the frozen /stt/v1/* wire),
                          services/ (the sherpa-onnx engine facade)
  config.toml.example   [stt] engine keys + [server] only; no other domains
  CONTEXT.md            purpose, ownership, wiring decisions
```

There is one feature and one module: `argus::stt` compiles the engine facade
and the HTTP surface together, and `app/main.cc` registers the controller
explicitly (a Drogon `HttpController<SttController, false>`), so no route
depends on static-init registration. The folder IS the module (root rule 25) —
a consumer links `argus::stt` and never lists `.cc` files. `src/shared/` does
not exist: rule 23's 2+ rule earns it, so code moves there only when a second
feature of this service reads it.

## Build commands

```bash
# From the monorepo root
./scripts/build-all.sh dev --only stt
```

Driving CMake by hand inside the folder means installing the root graph once
(`./scripts/build-all.sh dev --install-only`) and passing its toolchain; the
exact flag set is in `docs/operations/build-and-test.md` under "Working
inside one project".

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, no comments in code, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
