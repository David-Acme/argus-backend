# AGENTS.md — argus-vlm-client AI Agent Instructions

The root `AGENTS.md` (at the monorepo root) is binding for every change in
this package. The MUST-FOLLOW rules below restate the ones that apply.

## MUST-FOLLOW Rules

1. **VLM client only** — this package is the argus-vlm internal wire seen
   from the caller's side: request envelope, base64 image encoding and
   caption parsing. No AI engine compiles here and no service logic lives
   here.
2. **One wire handler (rule 23)** — cross-service callers go through this
   client; never hand-roll the VLM HTTP wire in a consumer.
3. **Parameter structs for 3+ params** — any function with 3+ parameters
   must take a struct (designated initializers, every member listed).
4. **Smart pointers** — no raw owning pointers.
5. **File naming** — `.hxx` headers, `.cc` sources.
6. **100% English** — code, comments, identifiers, docs, commits.
7. **Minimal comments** — small "what it does" comments only; the "why"
   goes to CONTEXT.md.
8. **Build gate** — 0 errors AND 0 warnings (`-Wall -Wextra`) in Argus's
   own code; third-party includes are SYSTEM.
