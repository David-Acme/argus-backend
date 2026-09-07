# argus-vlm — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root, next to `src/`) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to vision-service code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **Vision capacity only** — this service runs the LFM2.5-VL engine
   (llama.cpp + libmtmd) and nothing else (no ncnn/sherpa/onnx/tts/stt/face
   symbols; verified with `nm -C`). The engine is THE capacity of this
   service (F4-4, Ruling BO).
2. **Internal wire only** — the service serves the legacy adapters over
   loopback plain HTTP (`/vlm/v1/*`); no JWT, no CORS, no gateway routing.
   Never expose it publicly.
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
8. **100% English** — code, comments, identifiers, docs, commits.
9. **Minimal comments** — small "what it does" comments only; project-level
   "why" goes to CONTEXT.md.
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
  CMakeLists.txt        add_subdirectory-compatible AND standalone buildable
  conanfile.txt         Drogon + OpenCV headless (same versions as root)
  CMakePresets.json     dev preset, binaryDir build/dev inside the folder
  src/main.cc           config load, llama_backend_init/free, engine boot gate
  src/controllers/      HTTP controllers (health + /vlm/v1/* wire)
  src/vlm/              describe DTO (validation DSL)
  src/server/           internal listener resolution
  config.toml.example   [vision] engine keys + [server] only; no other domains
  CONTEXT.md            purpose, ownership, wiring decisions
```

## Build commands

```bash
# From the monorepo root (recommended)
cmake --build --preset vlm --target argus-vlm

# Standalone
conan install . --output-folder=build/dev -s build_type=Debug --build=missing
cmake --preset dev
cmake --build --preset dev -j 8
```

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, comment discipline, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
