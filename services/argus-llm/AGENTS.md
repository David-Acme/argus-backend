# argus-llm — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to llm-service code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **Brain process** — this service runs LFM2.5 chat, intent routing, the
   tool-calling loop and the in-process memory package. It must not absorb
   camera, face, VLM, STT, TTS or voice-session responsibilities.
2. **Internal wire only** — the service serves the legacy voice session over
   loopback plain HTTP (`/llm/v1/*`); no JWT, no CORS, no gateway routing.
   Never expose it publicly. No auth: the loopback bind is the trust
   boundary.
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
9. **100% English** — code, comments, identifiers, docs, commits.
10. **Minimal comments** — small "what it does" comments only; project-level
    "why" goes to CONTEXT.md.
11. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`,
    `LOG_ERROR`, `LOG_FATAL`); no spdlog.
12. **No std::future** — plain `std::thread` for the stream producer, as the
    TTS controller does.
13. **Memory database only** — the hosted memory package owns `memory.db`.
    Identity and camera snapshot sources are read-only; no other domain
    database is written here.
14. **Models are never copied** — LLM, memory and intent artifacts are read
    from the shared `models/` tree.
15. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's
    own code; third-party includes are SYSTEM.

## Layout

```
argus-llm/
  CMakeLists.txt        add_subdirectory-compatible AND standalone buildable
  conanfile.txt         Drogon + tomlplusplus + nlohmann_json (same pins)
  CMakePresets.json     dev preset, binaryDir build/dev inside the folder
  src/main.cc           config load, llama_backend_init/free, engine boot gate
  src/controllers/      HTTP controllers (health + /llm/v1/* wire)
  src/llm/              chat DTO (validation DSL)
  src/server/           internal listener resolution
  config.toml.example   listener, LLM and intent defaults
  CONTEXT.md            purpose, ownership, wiring decisions
```

## Build commands

```bash
# From the monorepo root
./scripts/build-all.sh dev --only argus-llm

# From services/argus-llm
conan install . --output-folder=build/dev -s build_type=Debug --build=missing
cmake --preset dev
cmake --build --preset dev -j 8
ctest --test-dir build/dev --output-on-failure
```

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, comment discipline, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
