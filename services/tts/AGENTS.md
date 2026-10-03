# argus-tts — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to tts-service code. The explicitly approved singular
`app/feature/shared` pilot layout below overrides the root's legacy layout
for this service only; other root rules remain binding.

## MUST-FOLLOW Rules

1. **TTS capacity only** — this service runs the onnxruntime TTS engines and
   nothing else (no face/llm/vlm/stt/vad code or symbols; verified with
   `nm -C`). The engine is THE capacity of this service (F4-2, Ruling BG).
2. **Internal wire only** — the service serves the legacy adapters over
   loopback plain HTTP (`/tts/v1/*`); no JWT, no CORS, no public routing or
   announcement. Never expose it publicly.
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
8. **100% English** — code, identifiers, docs, commits.
9. **No comments** — none in code, of any kind (root rule 20); the "why"
   goes to CONTEXT.md.
10. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`,
    `LOG_FATAL`); no spdlog.
11. **No std::future** — plain `std::thread` + join when parallelism is
    needed.
12. **No argus.db migrations** — this service owns no database at all.
13. **Models are never copied** — the onnxruntime artifacts are read from
    the shared `models/tts` tree through `tts.models_dir`.
14. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's
    own code; third-party includes are SYSTEM.
15. **Installs run the pinned script only** — on-demand installation spawns
    `scripts/provision.sh` with a catalog-checked component and no shell;
    never a path, URL or command taken from a request or a setting, and never
    a download without its SHA-256 pin.
16. **Non-commercial voices stay opt-in** — `jean` (CC BY-NC 4.0) is
    installed only with `ARGUS_TTS_POCKET_NONCOMMERCIAL_VOICES=1` or
    `tts.pocket_noncommercial_voices = true`; templates and production keep
    them off.

## Layout

```
argus-tts/
  CMakeLists.txt        add_subdirectory-compatible AND standalone buildable
  src/app/main.cc       config load, engine boot gate, HTTP + gRPC composition, app run
  src/app/rpc/          argus.tts.v1 Synthesis gRPC listener (TtsRpcServer)
  src/config/           argus::tts-config — the listener and the optional
                        gRPC leg's address and callers
  src/feature/synthesis/
    CMakeLists.txt      owns each production source once
    controllers/        transitional /tts/v1/* HTTP controller
    dtos/               internal HTTP request DTO
    services/           TtsService facade, per-language engine choice, cache, async synthesis
    infra/supertonic/   ONNX engine, model/style loading, Unicode processing
    infra/pocket/       Kyutai Pocket TTS runtime (bundle, tokenizer, voices, engine, rate converter)
    text/               argus::tts-speech-text — text normalizer + prosodic chunker (pure)
  src/feature/settings/ argus::tts-settings — the owner-editable catalog
  src/feature/provisioning/ argus::tts-provisioning — per-choice install state
                        (infra/: catalog reader, provision.sh probe + spawn;
                        services/: choice states, PocketInstaller worker)
  scripts/provision.sh  Supertonic download + the selected Pocket variants and
                        voices with SHA-256 pins (--plan, --catalog, --variant,
                        --voice; non-commercial voices opt-in)
  tools/export-pocket.py  official Pocket weights -> parity-checked ONNX bundle
  tools/tts-preview/    argus-tts-preview + make-previews.sh: the app's
                        seeded, reproducible voice preview clips
  config.toml.example   [tts] engine keys, the [rpc]/[rpc.callers] gRPC gate,
                        [server], [drogon.app]; no other domains
  CONTEXT.md            purpose, ownership, wiring decisions
```

This is the canonical internal-layout pilot; the repository's `services/`
root is not renamed. `src/shared/` does not exist yet — rule 23's 2+ rule
earns it, so a repository, schema or service moves there only when a second
feature of this service reads it. Health, listener resolution,
configuration and validation still come from the existing shared packages.

The top-level CMake auto-discovers feature folders and links
`argus::tts-rpc` + `argus::tts-synthesis-http` by name. The synthesis feature
declares each production source once: `argus::tts-synthesis` owns the service
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
`<tts/tts-wire.hxx>` from `packages/clients/tts` via its
exported include path. `TtsClient` (that package) now delegates to
`argus::tts::Client` over gRPC whenever `tts.grpc_target` is set, and falls
back to the original HTTP wire otherwise — both wire shapes are live.
Service-local consumers use `<feature/synthesis/services/tts-service.hxx>`;
there is no duplicate implementation or old-path forwarding header.
The gRPC transport (typed unary `Capabilities` + server-streaming
`Synthesize`, credential-gated, float32 at the engine's own rate) is served
by this pilot when `[rpc]` is configured. Two engines run behind it, chosen
per language: Kyutai Pocket TTS (default for es and en) and Supertonic 3
(every other language, and the fallback when Pocket models are missing). The
wire shape and the announced sample rate are unchanged (see CONTEXT.md).

## Build commands

```bash
# From the monorepo root
./scripts/build-all.sh dev --only tts
```

Driving CMake by hand inside the folder means installing the root graph once
(`./scripts/build-all.sh dev --install-only`) and passing its toolchain; the
exact flag set is in `docs/operations/build-and-test.md` under "Working
inside one project".

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, no comments in code, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
