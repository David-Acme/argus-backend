# F6-3b comment-discipline sweep — batch 1 (src/shared/**) — report

Branch: `f6-3b-batch1` (isolated worktree at /tmp/f6-3b-batch1, branched from master f30254e). Main tree untouched; no builds run.

## Result

Rule 20 sweep over the batch-1 inventory scope `src/shared/**`, excluding `src/shared/wrapper/qr/**` (deleted by F6-4; skipped entirely to avoid merge conflicts). Comment-only changes; no behavior change; no constexpr introduced (ruling 3).

112 files changed: 215 insertions, 778 deletions (CONTEXT.md +23/-0; code files comment-only).

## What was done

- Multi-line `/** */` class/field doc blocks condensed to single-line class-scope comments; per-field docs folded into the struct one-liner (e.g. `schemas/camera/camera-schema.hxx`, `schemas/calendar-event/calendar-event-schema.hxx`, `services/llm/llm-service.hxx` ChatRequest, `contracts/sync-filter.hxx`).
- In-function and per-statement comments deleted wholesale (llm-remote, tts-remote, memory/*, stt-service, noise-suppression-service, socket-service, rule-parser, graph-recall, vec-db, text-norm, phrase-automaton, …), including dangling-pointer, chunking and pipeline-flow explanations.
- Section banners and dividers deleted: `// --- X ---` in tts-engine.cc/onnx-utils.hxx, three-line banners in onnx-utils.cc and unicode-processor.cc (now comment-free), `// ---- X ----` dividers in validation/rules.hxx and validation_dsl.hxx, test section banners.
- Forbidden tokens deleted: all `Ruling X` references (db-service.cc, llm-remote, memory-service, stt-service, memory/* adapters, nats-bus), phase tokens (`Fase X`, `F2-…`) and AGENTS.md-rule references.
- Magic-number trailing comments deleted without introducing constants (ruling 3): `noise-suppression-service.cc` `kFrameSize = 480 // 10 ms at 48 kHz`, `intent-service.cc` `kLabelPrefixLen = 9 // strlen("__label__")`, `enums.hxx` `System = 0 // resolve from config stt.language`.
- Proven-stale comment deleted: `src/shared/services/sqlite/db-service.hxx` (former lines 40-44, obsolete camera-client narrative). Rulings 8's other three sites are outside batch 1's scope.
- Ruling 1: `// namespace` end tags kept everywhere.
- Ruling 2: frozen-contract wire-table trailing comments kept as short lines: `contracts/sync-operation.hxx` enum comments (SocketIO wire table) and `contracts/tool-contracts.hxx` `arguments`/`type` field lines.
- Ruling 9: zero commented-out code existed; none found.
- Kept compliant one-liners (F bucket) and the two external-document citations (`COGNITIVE_MEMORY_PLAN.md §6` in tool-registry.hxx/tool-contracts.hxx, same style as the kept `argus-contracts/subjects.md` reference) — these are doc pointers, not Ruling tokens.

## CONTEXT.md re-homing (ruling c)

The Drogon "no chunked on Connection: close" constraint was already documented in root CONTEXT.md ("Drogon chunking constraint"), so no addition was made for it. Four whys were documented nowhere else and gained 1-2 sentences each, placed in matching sections:

1. TTS: synthesized chunks are padded with ~300-500 ms of silence at both ends; concatenating raw chunks measured 565 ms at the boundary, hence trim-to-margin + `joinSilenceMs_` joins.
2. go2rtc security: RTSP/Tapo are unauthenticated on the wire — private/loopback hosts only; credentials go through the 0600 config file, never argv (`/proc/<pid>/cmdline` is world-readable).
3. Camera: cloudPassword never crosses the sync/API DTO boundary (repository schema only).
4. Noise suppression: RNNoise expects 16-bit-range floats, not [-1, 1] (xiph/rnnoise#184); the AGC ahead of it targets ~0.12 RMS, ≤24 dB gain.

## Verification

- Scripted diff classification over `git diff -U0`: every changed line is a comment line, a blank-line delta, a comment-strip pair (code text identical after removing the trailing comment), or a CONTEXT.md addition. Two exceptions, both intended: `wrapper/audio/audio-resampler.{cc,hxx}` gained a missing trailing EOF newline (pre-existing at f30254e; gate requires tail -c1 = 0a; the final `}`/`};` lines are textually identical).
- Multi-line comment blocks remaining in src/shared (excluding qr): 0. Remaining non-`namespace` trailing comments: only the two frozen-contract keeps above.
- EOF gate: tail -c1 = 0a on every touched file.
- Language: all added/kept comments 100% English; non-ASCII remains only on deleted Spanish lines.

## Commit

`style(comments): rule 20 sweep over src/shared (f6-3b-1)` on branch `f6-3b-batch1` (hash in the dispatch reply).