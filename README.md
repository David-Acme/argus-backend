# Argus Monorepo

This is the root of the **Argus** monorepo — a 100% local, modular AI platform
for the home (intelligent security guard + virtual assistant). Everything lives
in this single repository; all agent rules, code and comments are in English.

## Layout

- `argus-<name>/` — one folder per service (produces a process) or package
  (a library compiled into them). There is no monolith: `src/` was deleted
  once the last service moved out.
- `argus-contracts/` — protobuf contracts (`argus.<domain>.v1`), manifest
  schemas, `buf` config and sync fixtures
- `docs/` — architecture and migration documents
- `database/`, `docker/`, `scripts/` — schema, local runtime and tooling

## Adding services

Services are top-level folders:

- `argus-gateway/` (Phase 1), `argus-camera/` (Phase 2), and so on.

Each service keeps its own `CMakeLists.txt`, `conanfile`, `database/`,
`CONTEXT.md`, `AGENTS.md` and `tests/`, builds its own artifacts and is
deployed as its own container.

Internal versioning uses repository tags: `contracts-v*` for
`argus-contracts/`, `service-v*` for each service folder.
