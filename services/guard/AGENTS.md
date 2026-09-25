# argus-guard — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for every change in
this service. The MUST-FOLLOW rules below restate the ones that apply.

## MUST-FOLLOW Rules

1. **Guard domain only** — danger policy, incidents and security actions. No
   AI engines compile here (face, LLM, VLM, STT, TTS, VAD stay in their
   owners); no media, no stream relay.
2. **Single-owner database (rule 27)** — this service alone opens `guard.db`.
   Identity and notifications are reached through `argus::clients::identity` and
   `argus::clients::notification`; never read another service's database.
3. **Parameter structs for 3+ params** — any function with 3+ parameters must
   take a struct (designated initializers, every member listed).
4. **Dependency injection** — classes hold dependencies as private members
   with `_` suffix.
5. **Smart pointers** — no raw owning pointers.
6. **File naming** — `.hxx` headers, `.cc` sources, hyphenated `*-test.cc`.
7. **100% English** — code, identifiers, docs, commits.
8. **No comments** — none in code, of any kind (root rule 20); the "why"
   goes to CONTEXT.md.
9. **Logging** — Drogon built-ins only.
10. **Health safety** — `/health` never fails or blocks on downstream services.
11. **Audible rule** — any announcement, alarm tone or siren arming is a guard
    action gated by flags and caps; never during tests. Camera hardware is
    reached only through fleet-gated RPCs in a later phase.
12. **No argus.db migrations** — this service owns `guard.db` only.
13. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's own
    code; third-party includes are SYSTEM.

## Layout

```
argus-guard/
  CMakeLists.txt        standalone buildable: module graph + test targets
  src/app/main.cc       config load, guard.db wiring, app run
  src/config/           argus::guard-config — db, listener, notifications,
                          identity, actions, assessment, service and belief
                          resolution
  src/shared/vocabulary/
                        guard-mode, belief-gate-scope and belief-config —
                        the types the config module and the feature both read
  src/feature/guard/    argus::guard — the whole vertical slice:
                          the domain (assessment, belief, policy, risk,
                          dialogue, action, repository, schema, service),
                          vocabulary/, controllers/, dtos/, services/
  config.toml.example   guard-domain keys only
  CONTEXT.md            purpose, ownership, wiring decisions
```

There is one feature and two modules: `argus::guard` compiles the domain and
the HTTP surface together, and `argus::guard-config` (`src/config/`) resolves
what `main.cc` boots with, so `main.cc` holds composition and reads no key a
config module owns — its one `ConfigService::getString("nats.url")` is the
gate on the optional bus, a key `NatsBus::connect()` resolves again itself.
`main.cc` registers the controller explicitly
(a Drogon `HttpController<GuardController, false>`), so no route depends on
static-init registration. The folder IS the module (root rule 25) — a
consumer links `argus::guard` and never lists `.cc` files.

## Build

```bash
./scripts/build-all.sh dev --only guard
```
