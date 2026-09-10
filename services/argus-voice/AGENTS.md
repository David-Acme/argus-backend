# argus-voice — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to voice-service code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **Pure gRPC service** — the only network surfaces are the
   `argus.voice.v1.VoiceService` + `grpc.health.v1.Health` gRPC listener and
   the minimal `/health` HTTP listener. No `/sync`, no Drogon filters, no
   WebSocket, no HTTP routes to other services; all inter-service traffic is
   gRPC.
2. **Zero database** — no db clients, no schema, no repositories. The
   spoken-name write is the typed `IdentityService.UpdateUser` RPC to the
   gateway.
3. **Identity arrives typed** — the `VoiceIdentity` in `VoiceStart` and the
   `x-argus-user` / `x-argus-role` metadata are the only identity inputs;
   presence is required at `Connect`, role validation happened at the
   gateway.
4. **Remote-only engines** — STT/TTS/LLM resolve through the remote HTTP
   adapters only; no in-process engine compiles here.
5. **Parameter structs for 3+ params** — any function with 3+ parameters
   must take a struct (designated initializers, every member listed).
6. **Dependency injection** — services hold dependencies as private members
   with `_` suffix; the engine seam is injected as `VoiceEngineSeam`.
7. **Smart pointers** — no raw owning pointers; raw pointers only for
   non-owning access.
8. **File naming** — `.hxx` headers, `.cc` sources, hyphenated `*-test.cc`
   tests. No `.h`/`.cpp`.
9. **100% English** — code, comments, identifiers, docs, commits.
10. **Minimal comments** — small "what it does" comments only; "why" goes to
    CONTEXT.md.
11. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`,
    `LOG_FATAL`); no spdlog.
12. **Health safety** — `/health` and the gRPC health service never block on
    the engines or downstream services; degraded dependencies degrade logs,
    not health.
13. **Never trigger setAlarm/siren paths** — voice reactions must never
    carry alarm/siren triggers; the audible alarm test is the user's
    personal task.
14. **No std::future** — plain `std::thread` + join when parallelism is
    needed.
15. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's
    own code; third-party includes are SYSTEM.

## Layout

```
argus-voice/
  CMakeLists.txt        argus_module(voice-core) + argus_service(argus-voice)
  src/main.cc           config load, gRPC server + minimal /health boot
  src/feature/voice/    voice session, remote-only engine seam, VoiceService RPC
  src/feature/health/   grpc.health.v1 service
  src/controllers/      /health HTTP controller
  src/server/           listener resolution (grpc_port / health_port)
  config.toml.example   [server], [identity], [stt], [tts], [llm], [vad]
  CONTEXT.md            purpose, ownership, wiring decisions
```

## Build commands

```bash
# From the monorepo root
./scripts/build-all.sh dev --only argus-voice

# From services/argus-voice
conan install . --output-folder=build/dev -s build_type=Debug --build=missing
cmake --preset dev
cmake --build --preset dev -j 8
ctest --test-dir build/dev --output-on-failure
```

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules
> 19-25 (modern C++20, comment discipline, efficiency, DB tuning, feature
> layout + shared SDK, monolith structure, build ergonomics).
