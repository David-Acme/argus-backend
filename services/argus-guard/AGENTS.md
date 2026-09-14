# argus-guard — AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for every change in
this service. The MUST-FOLLOW rules below restate the ones that apply.

## MUST-FOLLOW Rules

1. **Guard domain only** — danger policy, incidents and security actions. No
   AI engines compile here (face, LLM, VLM, STT, TTS, VAD stay in their
   owners); no media, no stream relay.
2. **Single-owner database (rule 27)** — this service alone opens `guard.db`.
   Identity and notifications are reached through `argus::sdk-identity` and
   `argus::sdk-notification`; never read another service's database.
3. **Parameter structs for 3+ params** — any function with 3+ parameters must
   take a struct (designated initializers, every member listed).
4. **Dependency injection** — classes hold dependencies as private members
   with `_` suffix.
5. **Smart pointers** — no raw owning pointers.
6. **File naming** — `.hxx` headers, `.cc` sources, hyphenated `*-test.cc`.
7. **100% English** — code, comments, identifiers, docs, commits.
8. **Minimal comments** — small "what it does" comments only; the "why" goes
   to CONTEXT.md.
9. **Logging** — Drogon built-ins only.
10. **Health safety** — `/health` never fails or blocks on downstream services.
11. **Audible rule** — any announcement, alarm tone or siren arming is a guard
    action gated by flags and caps; never during tests. Camera hardware is
    reached only through fleet-gated RPCs in a later phase.
12. **No argus.db migrations** — this service owns `guard.db` only.
13. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's own
    code; third-party includes are SYSTEM.

## Build

```bash
./scripts/build-all.sh dev --only argus-guard
```
