# argus-llm — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root, next to `src/`) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to llm-service code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **LLM capacity only** — this service runs the LFM2.5-1.2B chat engine
   (llama.cpp, no mtmd) and nothing else (no ncnn/sherpa/onnx/opencv/mtmd
   symbols; verified with `nm -C` — llama is the ONLY AI linkage, Ruling BS).
2. **Internal wire only** — the service serves the legacy voice session over
   loopback plain HTTP (`/llm/v1/*`); no JWT, no CORS, no gateway routing.
   Never expose it publicly. No auth: the loopback bind is the trust
   boundary.
3. **Frozen envelope** — every JSON response uses the
   `{status, info, errors}` envelope (`ApiResponse`); the chat body is JSON
   `{messages, max_tokens?, temperature?, reset_context?}`.
4. **No tool loop** — chat and chat-stream only (Ruling BV). LfmAdapter,
   ToolRegistry and chatWithTools stay legacy-side; never wire them here.
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
13. **No database at all** — this service owns no database, no schema
    runner, no migrations; the KV-prefix cache is in-process warm state only.
14. **Models are never copied** — the GGUF artifact is read from the shared
    `models/llm/` tree through `[llm]` config keys.
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
  config.toml.example   [llm] engine keys + [server] only; no other domains
  CONTEXT.md            purpose, ownership, wiring decisions
```

## Build commands

```bash
# From the monorepo root (recommended)
cmake --build --preset llm --target argus-llm

# Standalone
conan install . --output-folder=build/dev -s build_type=Debug --build=missing
cmake --preset dev
cmake --build --preset dev -j 8
```

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, comment discipline, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
