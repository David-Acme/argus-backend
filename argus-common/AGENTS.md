# AGENTS.md — argus-common AI Agent Instructions

> This file is read by AI coding assistants before any code generation task.
> It defines this folder's purpose, conventions, and constraints.

## Project Identity

- **argus-common** — the shared C++20 foundation library (`argus_common` /
  `argus::common`) every backend service links: enums, role access,
  validation DSL, the API response envelope, the NATS event bus, the
  hardware-profile probe, the config-service TOML reader, the schema runner,
  the socket-emit DTO, and the pure-C++ sync contract interfaces
  (`Syncable`, `SyncFilter`, `SyncOperation`).
- Top-level folder of the Argus monorepo (rooted at `backend/`), not an
  independent repo — a package, not a service. It carries no
  `conanfile.txt`/`CMakePresets.json` of its own; it is never built or run
  standalone.
- English only: file contents, comments, commit messages.

## Layout

```
argus-common/
  CMakeLists.txt          argus_module(NAME common ...) + hardware-profile
  src/shared/...           the moved files, original relative path preserved
```

The `src/shared/...` path prefix inside this package is intentional, not
legacy: every `#include <shared/...>` across the 450 consumer files in the
rest of the tree keeps resolving unchanged. See CONTEXT.md for why.

## Binding rules

Root `AGENTS.md` MUST-FOLLOW rules 19-25 apply in full (modern C++20,
comment discipline, efficiency, DB tuning, feature layout, shared SDK,
monolith structure, build-by-module-name). In particular:

- Rule 20 (comment discipline) applies to every file here regardless of how
  long it predates this package — a file's age or its having been moved is
  never a reason to leave a stale or misplaced comment.
- Rule 25 (the folder IS the module): this package is declared once, through
  `argus_module(NAME common ...)` in `cmake/argus-module.cmake`. Never list
  `argus-common/src/...` files by raw path in a consumer's CMakeLists — link
  `argus_common` (or `argus::common`) instead.

## What does NOT live here

`src/shared/repositories`, `src/shared/schemas`, the other
`src/shared/services/*`, the rest of `src/shared/contracts/*` (domain seams:
`camera-*`, `identity-change-sink.hxx`, `push-intent-sink.hxx`,
`tool-contracts.hxx`, `user-*`), `src/shared/vocabulary`, and the rest of
`src/shared/wrapper/*` all stay in `src/shared` for now — later steps of the
microservices migration move them. Before adding a file here, check
`docs/migracion-microservicios.md` for the step that owns it.
