# Argus Monorepo

This is the root of the **Argus** monorepo — a 100% local, modular AI platform
for the home (intelligent security guard + virtual assistant). Everything lives
in this single repository; all agent rules, code and comments are in English.

## Layout

- `src/` — backend monolith (C++20 / Drogon), with `labs/` beside it
- `argus-contracts/` — protobuf contracts (`argus.<domain>.v1`), manifest
  schemas, `buf` config and sync fixtures
- `docs/` — architecture and migration documents
- `database/`, `docker/`, `scripts/` — schema, local runtime and tooling

## Adding services

Future services are top-level folders **siblings of `src/`**:

- `argus-gateway/` (Phase 1), `argus-camera/` (Phase 2), and so on.

Each service keeps its own `CMakeLists.txt`, `conanfile`, `database/`,
`CONTEXT.md`, `AGENTS.md`, `tests/` and `labs/`, builds its own artifacts and
is deployed as its own container.

Internal versioning uses repository tags: `contracts-v*` for
`argus-contracts/`, `service-v*` for each service folder.