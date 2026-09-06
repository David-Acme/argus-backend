# argus-stt — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root, next to `src/`) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to stt-service code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **STT capacity only** — this service runs the sherpa-onnx STT engine and
   nothing else (no face/llm/vlm/tts/vad code or symbols; verified with
   `nm -C`). The engine is THE capacity of this service (F4-3, Ruling BK).
2. **Internal wire only** — the service serves the legacy adapters over
   loopback plain HTTP (`/stt/v1/*`); no JWT, no CORS, no gateway routing.
   Never expose it publicly.
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
8. **100% English** — code, comments, identifiers, docs, commits.
9. **Minimal comments** — small "what it does" comments only; project-level
   "why" goes to CONTEXT.md.
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
  CMakeLists.txt        add_subdirectory-compatible AND standalone buildable
  conanfile.txt         Drogon + onnxruntime (same versions as root)
  CMakePresets.json     dev preset, binaryDir build/dev inside the folder
  src/main.cc           config load, engine boot gate, app run
  src/controllers/      HTTP controllers (health + /stt/v1/* wire)
  src/server/           internal listener resolution
  config.toml.example   [stt] engine keys + [server] only; no other domains
  CONTEXT.md            purpose, ownership, wiring decisions
```

## Build commands

```bash
# From the monorepo root (recommended)
cmake --build --preset stt --target argus-stt

# Standalone
conan install . --output-folder=build/dev -s build_type=Debug --build=missing
cmake --preset dev
cmake --build --preset dev -j 8
```
