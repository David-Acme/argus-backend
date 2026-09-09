# Argus Monorepo

This is the root of the **Argus** monorepo — a 100% local, modular AI platform
for the home (intelligent security guard + virtual assistant). Everything lives
in this single repository; all agent rules, code and comments are in English.

## Layout

- `services/argus-<name>/` — one folder per service; each produces a process
- `packages/argus-<name>/` — shared libraries compiled into those services;
  they produce no process of their own
- `packages/argus-contracts/` — protobuf contracts (`argus.<domain>.v1`),
  manifest schemas, `buf` config and sync fixtures
- `argus-deploy/` — compose stack, Dockerfile and per-service config examples
- `docs/` — architecture and migration documents
- `database/`, `docker/`, `scripts/` — schema, local runtime and tooling

There is no monolith: `src/` was deleted once the last service moved out.

## Adding services

A service is a folder under `services/`; a shared library is a folder under
`packages/`. Both are discovered by the root build, so adding one means
creating its folder.

Each service keeps its own `CMakeLists.txt`, `conanfile`, `database/`,
`CONTEXT.md`, `AGENTS.md` and `tests/`, builds its own artifacts and is
deployed as its own container.

Internal versioning uses repository tags: `contracts-v*` for
`packages/argus-contracts/`, `service-v*` for each service folder.
