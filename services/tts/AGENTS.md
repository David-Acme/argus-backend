# argus-tts — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to tts-service code. The explicitly approved singular
`app/feature/shared` pilot layout below overrides the root's legacy layout
for this service only; other root rules remain binding.

## MUST-FOLLOW Rules

1. **TTS capacity only** — this service runs the onnxruntime TTS engine and
   nothing else (no face/llm/vlm/stt/vad code or symbols; verified with
   `nm -C`). The engine is THE capacity of this service (F4-2, Ruling BG).
2. **Internal wire only** — the service serves the legacy adapters over
   loopback plain HTTP (`/tts/v1/*`); no JWT, no CORS, no gateway routing.
   Never expose it publicly.
3. **Frozen envelope** — every JSON error uses the
   `{status, info, errors}` envelope (`ApiResponse`); binary responses carry
   `audio/x-argus-pcm-f32` + `X-Argus-Sample-Rate`.
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
12. **No argus.db migrations** — this service owns no database at all.
13. **Models are never copied** — the onnxruntime artifacts are read from
    the shared `models/tts` tree through `tts.models_dir`.
14. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's
    own code; third-party includes are SYSTEM.

## Layout

```
argus-tts/
  CMakeLists.txt        add_subdirectory-compatible AND standalone buildable
  conanfile.txt         Drogon + onnxruntime (same versions as root)
  CMakePresets.json     dev preset, binaryDir build/dev inside the folder
  src/app/main.cc       config load, engine boot gate, HTTP + gRPC composition, app run
  src/app/rpc/          argus.tts.v1 Synthesis gRPC listener (TtsRpcServer)
  src/feature/synthesis/
    CMakeLists.txt      owns each production source once
    api/http/
      controller/       transitional /tts/v1/* HTTP controller
      dto/              internal HTTP request DTO
    domain/             TtsService facade, lifecycle, cache, async synthesis
    infra/supertonic/   ONNX engine, model/style loading, Unicode processing
  config.toml.example   [tts] engine keys, [rpc] gRPC gate, [server]; no other domains
  CONTEXT.md            purpose, ownership, wiring decisions
```

This is the canonical internal-layout pilot; the repository's `services/`
root is not renamed. `src/shared/` is reserved for genuinely cross-feature
service-local code and currently has no files. Health, listener resolution,
configuration and validation still come from the existing shared packages.

The top-level CMake auto-discovers feature folders and links
`argus::tts-rpc` + `argus::tts-synthesis-http` by name. The synthesis feature
declares each production source once: `argus::tts-synthesis` owns the domain
facade and Supertonic infrastructure; `argus::tts-synthesis-http` owns the
controller and DTO and links the synthesis target. `argus::tts-rpc`
(`src/app/rpc/`) owns the `argus.tts.v1.Synthesis` gRPC listener and links the
shared `argus::clients::tts` module (`packages/clients/tts`). The
executable and wire tests reuse these targets rather than compiling
duplicate production source lists.

HTTP is transitional, not the target transport architecture. The existing
`/tts/v1/*` routes, envelopes, PCM format, singleton lifecycle and synthesis
behavior remain unchanged (retained for consumers not yet converted; do not
claim the HTTP fallback removed). The service still consumes
`<shared/services/tts/tts-wire.hxx>` from `packages/clients/tts` via its
exported include path. `TtsClient` (that package) now delegates to
`argus::tts::Client` over gRPC whenever `tts.grpc_target` is set, and falls
back to the original HTTP wire otherwise — both wire shapes are live.
Service-local consumers use `<feature/synthesis/domain/tts-service.hxx>`;
there is no duplicate implementation or old-path forwarding header.
The gRPC transport (typed unary `Capabilities` + server-streaming
`Synthesize`, credential-gated, float32 at the engine's own rate) is served
by this pilot when `[rpc]` is configured; the Pocket engine is NOT
implemented, and Supertonic remains the running engine with unchanged model
settings.

## Build commands

```bash
# From the monorepo root
./scripts/build-all.sh dev --only tts

# From services/tts
conan install . --output-folder=build/dev -s build_type=Debug --build=missing
cmake --preset dev
cmake --build --preset dev -j 8
ctest --test-dir build/dev --output-on-failure
```

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, comment discipline, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
