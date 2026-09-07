# argus-tunnel — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root, next to `src/`) is binding for
every change in this service. The MUST-FOLLOW rules below restate the ones
that apply to tunnel code; when in doubt, the root file wins.

## MUST-FOLLOW Rules

1. **No database anywhere** (Ruling CL) — the tunnel never opens, links or
   configures a database; conan pulls no sqlite-orm target of our own and
   no `*.db` file is ever created here.
2. **Byte transparency** — client and relay never terminate the TLS carried
   over the tunnel: bytes are relayed verbatim, frame boundaries are
   transport-internal and never leak into the carried stream.
3. **ONE home link** — the client holds exactly one persistent outbound
   connection to the relay; device connections are multiplexed over it.
   Streams do NOT survive a reconnect (see CONTEXT.md).
4. **Bounded buffers everywhere** — every buffer (frame parser, per-stream
   pending queues, link pending, peer send buffers) has a hard cap; a cap
   breach triggers read-pause back-pressure first, stream close second,
   link drop last. No unbounded queues.
5. **No thread-per-stream** — all tunnel I/O runs on one PollLoop thread
   per binary; the only other threads are Drogon's `/health` server.
6. **Frozen envelope** — `/health` answers through `ApiResponse::ok`
   (`{status, info, errors}`).
7. **Parameter structs for 3+ params** — any function with 3+ parameters
   must take a struct (designated initializers, every member listed).
8. **Dependency injection** — services hold dependencies as private members
   with `_` suffix; no static service methods.
9. **Smart pointers** — no raw owning pointers; sockets are RAII
   (`UniqueFd`), peers and listeners are `unique_ptr`/`shared_ptr`.
10. **File naming** — `.hxx` headers, `.cc` sources, hyphenated
    `*-test.cc` tests. No `.h`/`.cpp`.
11. **100% English** — code, comments, identifiers, docs, commits.
12. **Minimal comments** — small "what it does" comments only; project-level
    "why" goes to CONTEXT.md.
13. **Logging** — Drogon built-ins only (`LOG_INFO`, `LOG_WARN`,
    `LOG_FATAL`); no spdlog.
14. **No alarms/sirens** — this service never triggers `setAlarm` or any
    siren/notification path; it is a byte pipe.
15. **Secret stays out of templates** — `config.toml.example` ships
    `secret = ""`; both binaries refuse to start with an empty secret.
16. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's
    own code; third-party includes are SYSTEM.

## Layout

```
argus-tunnel/
  CMakeLists.txt        add_subdirectory-compatible AND standalone buildable
  conanfile.txt         Drogon + tomlplusplus (same versions as root)
  CMakePresets.json     dev preset, binaryDir build/dev inside the folder
  src/net/              epoll engine (PollLoop, TcpPeer, TcpListener, UniqueFd)
  src/protocol/         frame codec + HMAC auth (frames.hxx/.cc)
  src/core/             home-link stream multiplexer (TunnelMux)
  src/client/           home-side client with reconnect (TunnelClient)
  src/relay/            US-side relay (TunnelRelay)
  src/server/           per-binary config resolution
  src/controllers/      /health controller
  src/main-client.cc    argus-tunnel-client entrypoint
  src/main-relay.cc     argus-relay entrypoint
  tests/                framing units + loopback harness + integration tests
  config.toml.example   [tunnel] + [server] only; secret stays empty
  CONTEXT.md            protocol spec, back-pressure, reconnect semantics
```

## Build commands

```bash
# From the monorepo root (recommended)
cmake --build --preset tunnel --target argus-tunnel-relay argus-tunnel-client

# Standalone
conan install . --output-folder=build/dev -s build_type=Debug --build=missing
cmake --preset dev
cmake --build --preset dev -j 8
ctest --test-dir build/dev --output-on-failure
```

> Binding cross-service code standards: root `AGENTS.md` MUST-FOLLOW rules 19-24 (modern C++20, comment discipline, efficiency, DB tuning, feature layout + shared SDK, monolith structure).
